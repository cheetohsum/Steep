/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2004-2010 Gabor Horvath <hgabor@rawtherapee.com>
 *
 *  RawTherapee is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  RawTherapee is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with RawTherapee.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <atomic>
#include <memory>
#include <set>
#include <thread>
#include "cachemanager.h"
#include "filebrowserentry.h"
#include "previewloader.h"
#include "guiutils.h"
#include "threadutils.h"

#ifdef _OPENMP
#include <omp.h>
#endif

#define DEBUG(format,args...)
//#define DEBUG(format,args...) printf("PreviewLoader::%s: " format "\n", __FUNCTION__, ## args)

class PreviewLoader::Impl :
    public rtengine::NonCopyable
{
public:
    struct Job {
        Job(int dir_id, const Glib::ustring& dir_entry, PreviewLoaderListener* listener, int generation):
            dir_id_(dir_id),
            dir_entry_(dir_entry),
            listener_(listener),
            generation_(generation)
        {}

        Job():
            dir_id_(0),
            listener_(nullptr),
            generation_(0)
        {}

        int dir_id_;
        Glib::ustring dir_entry_;
        PreviewLoaderListener* listener_;
        int generation_;
    };

    struct JobCompare {
        bool operator()(const Job& lhs, const Job& rhs) const
        {
            if ( lhs.dir_id_ == rhs.dir_id_ ) {
                return lhs.dir_entry_ < rhs.dir_entry_;
            }

            return lhs.dir_id_ < rhs.dir_id_;
        }
    };

    typedef std::set<Job, JobCompare> JobSet;

    std::atomic<bool> paused_{false};
    std::atomic<int> generation_{0};
    std::atomic<unsigned> priorityCounter_{0};

    Impl(): nConcurrentThreads(0)
    {
#ifdef _OPENMP
        int threadCount = omp_get_num_procs();
#else
        int threadCount = std::max(2u, std::thread::hardware_concurrency());
#endif
        // Preview loading is I/O-bound (reading cached thumbnails from disk
        // and, for uncached images, extracting embedded thumbnails from RAW).
        // Use enough threads to keep the I/O subsystem busy — modern SSDs
        // handle many concurrent small-file reads easily, and even for
        // uncached RAW folders the embedded-JPEG extraction benefits from
        // overlapping I/O with CPU work across more threads.
        threadCount = std::max(4, std::min(threadCount * 2, 16));

        threadPool_.reset(new Glib::ThreadPool(threadCount, 0));
    }

    std::unique_ptr<Glib::ThreadPool> threadPool_;
    MyMutex mutex_;
    JobSet jobs_;
    Glib::ustring priorityHint_;
    gint nConcurrentThreads;

    // Pick the best job from the set, considering the priority hint.
    // When a hint is set, alternates between forward and backward neighbors
    // so filmstrip thumbnails expand symmetrically from the target.
    // Returns jobs_.end() if empty or paused.
    JobSet::iterator pickBestJob_()
    {
        if (jobs_.empty() || paused_.load(std::memory_order_relaxed)) {
            return jobs_.end();
        }

        if (priorityHint_.empty()) {
            return jobs_.begin();
        }

        int dir_id = jobs_.begin()->dir_id_;
        auto hint_it = jobs_.lower_bound(Job(dir_id, priorityHint_, nullptr, 0));

        // hint_it points to first job >= hint.  prev(hint_it) is last job < hint.
        auto fwd = hint_it;
        auto bwd = (hint_it != jobs_.begin()) ? std::prev(hint_it) : jobs_.end();

        if (fwd != jobs_.end() && bwd != jobs_.end()) {
            // Both directions available — alternate to expand symmetrically
            unsigned count = priorityCounter_.fetch_add(1, std::memory_order_relaxed);
            return (count % 2 == 0) ? fwd : bwd;
        } else if (fwd != jobs_.end()) {
            return fwd;
        } else if (bwd != jobs_.end()) {
            return bwd;
        }
        return jobs_.begin();
    }

    void processNextJob()
    {
        // Peek at the queue before committing to the concurrent thread count.
        // This ensures no-op tasks (from stale thread pool pushes) don't
        // interfere with the nConcurrentThreads counter.
        {
            MyMutex::MyLock lock(mutex_);
            if (jobs_.empty() || paused_.load(std::memory_order_relaxed)) {
                return;
            }
        }

        g_atomic_int_inc (&nConcurrentThreads);

        int lastDirId = -1;
        PreviewLoaderListener* lastListener = nullptr;

        // Loop to drain the queue — each thread processes all available jobs
        // before returning to the thread pool, avoiding thousands of individual
        // thread pool task dispatches for large folders.
        while (true) {
            Job j;
            {
                MyMutex::MyLock lock(mutex_);

                auto best = pickBestJob_();
                if (best == jobs_.end()) {
                    break;
                }

                j = *best;
                jobs_.erase(best);
                DEBUG("processing %s", j.dir_entry_.c_str());
                DEBUG("%d job(s) remaining", jobs_.size());
            }

            // Skip jobs from a previous directory — removeAllJobs() increments
            // the generation counter, so any job queued before the switch has a
            // stale generation and we can skip the expensive cacheMgr I/O.
            if (j.generation_ != generation_.load(std::memory_order_acquire)) {
                continue;
            }

            try {
                Thumbnail* tmb = cacheMgr->getEntry(j.dir_entry_);

                if ( tmb ) {
                    DEBUG("Preview Ready\n");
                    j.listener_->previewReady(j.dir_id_, new FileBrowserEntry(tmb, j.dir_entry_));
                }

            } catch (Glib::Error &e) {} catch(...) {}

            lastDirId = j.dir_id_;
            lastListener = j.listener_;
        }

        bool last = g_atomic_int_dec_and_test (&nConcurrentThreads);

        // signal at end
        if (last && lastListener && jobs_.empty()) {
            lastListener->previewsFinished(lastDirId);
        }
    }
};

PreviewLoader::PreviewLoader():
    impl_(new Impl())
{
}

PreviewLoader::~PreviewLoader() {
    delete impl_;
}

PreviewLoader* PreviewLoader::getInstance()
{
    static PreviewLoader instance_;
    return &instance_;
}

void PreviewLoader::add(int dir_id, const Glib::ustring& dir_entry, PreviewLoaderListener* l)
{
    // somebody listening?
    if ( l != nullptr ) {
        {
            MyMutex::MyLock lock(impl_->mutex_);

            // create a new job and append to queue
            DEBUG("saving job %s", dir_entry.c_str());
            impl_->jobs_.insert(Impl::Job(dir_id, dir_entry, l, impl_->generation_.load(std::memory_order_relaxed)));
        }

        // queue a run request
        DEBUG("adding run request %s", dir_entry.c_str());
        impl_->threadPool_->push(sigc::mem_fun(*impl_, &PreviewLoader::Impl::processNextJob));
    }
}

void PreviewLoader::removeAllJobs()
{
    DEBUG("stop %d", impl_->nConcurrentThreads);
    MyMutex::MyLock lock(impl_->mutex_);
    impl_->jobs_.clear();
    impl_->priorityHint_.clear();
    // Increment generation so in-flight threads that already dequeued a job
    // from the old directory will skip the expensive cacheMgr I/O.
    impl_->generation_.fetch_add(1, std::memory_order_release);
}

void PreviewLoader::setPriorityHint(const Glib::ustring& targetFile)
{
    MyMutex::MyLock lock(impl_->mutex_);
    impl_->priorityHint_ = targetFile;
    impl_->priorityCounter_.store(0, std::memory_order_relaxed);
}

void PreviewLoader::pause()
{
    impl_->paused_.store(true, std::memory_order_relaxed);
}

void PreviewLoader::resume()
{
    impl_->paused_.store(false, std::memory_order_relaxed);

    // Re-schedule all pending jobs so the thread pool picks them up
    MyMutex::MyLock lock(impl_->mutex_);
    for (size_t i = 0; i < impl_->jobs_.size(); ++i) {
        impl_->threadPool_->push(sigc::mem_fun(*impl_, &PreviewLoader::Impl::processNextJob));
    }
}
