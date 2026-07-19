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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "cachemanager.h"
#include "filebrowserentry.h"
#include "previewloader.h"
#include "guiutils.h"
#include "thumbnail.h"
#include "threadutils.h"

#ifdef _WIN32
#include "rtengine/leanwindows.h"
#endif

#ifdef _OPENMP
#include <omp.h>
#endif

#define DEBUG(format,args...)
//#define DEBUG(format,args...) printf("PreviewLoader::%s: " format "\n", __FUNCTION__, ## args)

namespace {

bool isRawPreviewPath(const Glib::ustring& path)
{
    const Glib::ustring basename = Glib::path_get_basename(path);
    const auto lastdot = basename.find_last_of('.');
    if (lastdot >= basename.length() - 1) {
        return false;
    }

    static const std::set<Glib::ustring> rawExtensions = {
        "3fr", "arw", "arq", "cr2", "cr3", "crf", "crw", "dcr", "dng",
        "fff", "iiq", "kdc", "mef", "mos", "mrw", "nef", "nrw", "orf",
        "ori", "pef", "raf", "raw", "rw2", "rwl", "rwz", "sr2", "srf",
        "srw", "x3f"
    };

    return rawExtensions.count(basename.substr(lastdot + 1).lowercase()) > 0;
}

}

static void setPreviewWorkerThreadPriority()
{
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
}

class PreviewLoader::Impl :
    public rtengine::NonCopyable
{
public:
    struct Job {
        Job(int dir_id, const Glib::ustring& dir_entry, PreviewLoaderListener* listener, int generation, std::string&& dir_entry_key = std::string()):
            dir_id_(dir_id),
            dir_entry_(dir_entry),
            dir_entry_key_(std::move(dir_entry_key)),
            dir_entry_sort_key_(dir_entry_key_.empty() ? dir_entry_.raw() : dir_entry_key_),
            is_raw_(isRawPreviewPath(dir_entry_)),
            listener_(listener),
            generation_(generation)
        {}

        Job(int dir_id, Glib::ustring&& dir_entry, PreviewLoaderListener* listener, int generation, std::string&& dir_entry_key = std::string()):
            dir_id_(dir_id),
            dir_entry_(std::move(dir_entry)),
            dir_entry_key_(std::move(dir_entry_key)),
            dir_entry_sort_key_(dir_entry_key_.empty() ? dir_entry_.raw() : dir_entry_key_),
            is_raw_(isRawPreviewPath(dir_entry_)),
            listener_(listener),
            generation_(generation)
        {}

        Job():
            dir_id_(0),
            is_raw_(false),
            listener_(nullptr),
            generation_(0)
        {}

        int dir_id_;
        Glib::ustring dir_entry_;
        std::string dir_entry_key_;
        std::string dir_entry_sort_key_;
        bool is_raw_;
        PreviewLoaderListener* listener_;
        int generation_;
    };

    struct JobCompare {
        bool operator()(const Job& lhs, const Job& rhs) const
        {
            if ( lhs.dir_id_ == rhs.dir_id_ ) {
                return lhs.dir_entry_sort_key_ < rhs.dir_entry_sort_key_;
            }

            return lhs.dir_id_ < rhs.dir_id_;
        }
    };

    typedef std::set<Job, JobCompare> JobSet;

    std::atomic<unsigned> pauseDepth_{0};
    std::atomic<bool> postScanDrainMode_{false};
    std::atomic<int> generation_{0};
    std::atomic<unsigned> priorityCounter_{0};
    int maxThreadCount_;
    int queuedWorkers_ = 0;
    unsigned fastJobCredit_ = 0;
    int ioPressure_ = 0;

    Impl(): nConcurrentThreads(0)
    {
#ifdef _OPENMP
        int threadCount = omp_get_num_procs();
#else
        int threadCount = std::max(2u, std::thread::hardware_concurrency());
#endif
        // Preview loading is paused around foreground image opens. Use enough
        // workers to overlap cold-cache preview extraction, while keeping them
        // below normal priority so GTK stays responsive for early paints.
        // Embedded-JPEG extraction is mostly CPU-bound decode+scale, so on
        // big machines a wider pool meaningfully cuts first-visit times; the
        // ioPressure feedback below still throttles slow (spinning) storage.
        threadCount = std::max(4, std::min(threadCount, 12));
        maxThreadCount_ = threadCount;

        threadPool_.reset(new Glib::ThreadPool(threadCount, true));
    }

    std::unique_ptr<Glib::ThreadPool> threadPool_;
    MyMutex mutex_;
    JobSet jobs_;
    Glib::ustring priorityHint_;
    std::string priorityHintKey_;
    JobSet::iterator priorityForward_;
    JobSet::iterator priorityBackward_;
    bool priorityIteratorsValid_ = false;
    gint nConcurrentThreads;

    void invalidatePriorityIterators_()
    {
        priorityIteratorsValid_ = false;
        priorityForward_ = jobs_.end();
        priorityBackward_ = jobs_.end();
    }

    std::pair<JobSet::iterator, std::size_t> findRawForward_(JobSet::iterator start, std::size_t maxSteps)
    {
        const int dirId = start == jobs_.end() ? 0 : start->dir_id_;
        std::size_t steps = 0;
        for (auto it = start; it != jobs_.end() && it->dir_id_ == dirId && steps <= maxSteps; ++it, ++steps) {
            if (it->is_raw_) {
                return {it, steps};
            }
        }

        return {jobs_.end(), maxSteps + 1};
    }

    std::pair<JobSet::iterator, std::size_t> findRawBackward_(JobSet::iterator start, std::size_t maxSteps)
    {
        if (start == jobs_.end()) {
            return {jobs_.end(), maxSteps + 1};
        }

        const int dirId = start->dir_id_;
        std::size_t steps = 0;
        auto it = start;
        while (steps <= maxSteps) {
            if (it->dir_id_ != dirId) {
                break;
            }

            if (it->is_raw_) {
                return {it, steps};
            }

            if (it == jobs_.begin()) {
                break;
            }

            --it;
            ++steps;
        }

        return {jobs_.end(), maxSteps + 1};
    }

    JobSet::iterator preferNearbyRaw_(JobSet::iterator preferred, JobSet::iterator alternate)
    {
        if (preferred == jobs_.end() || preferred->is_raw_ || jobs_.size() < 4) {
            return preferred;
        }

        if (alternate != jobs_.end() && alternate->is_raw_) {
            return alternate;
        }

        // RAW quick thumbnails are cheap and unblock RAW navigation previews,
        // so prefer one nearby without abandoning proximity ordering.
        constexpr std::size_t kRawPriorityProbe = 28;
        auto forward = findRawForward_(preferred, kRawPriorityProbe);
        auto backward = findRawBackward_(preferred, kRawPriorityProbe);

        if (forward.first == jobs_.end()) {
            return backward.first != jobs_.end() ? backward.first : preferred;
        }

        if (backward.first == jobs_.end()) {
            return forward.first;
        }

        return forward.second <= backward.second ? forward.first : backward.first;
    }

    void refreshPriorityIterators_()
    {
        if (priorityIteratorsValid_) {
            return;
        }

        priorityIteratorsValid_ = true;

        if (jobs_.empty() || priorityHint_.empty()) {
            priorityForward_ = jobs_.end();
            priorityBackward_ = jobs_.end();
            return;
        }

        const int dir_id = jobs_.begin()->dir_id_;
        priorityForward_ = jobs_.lower_bound(Job(dir_id, priorityHint_, nullptr, 0, std::string(priorityHintKey_)));
        priorityBackward_ =
            priorityForward_ != jobs_.begin()
                ? std::prev(priorityForward_)
                : jobs_.end();
    }

    // Pick the best job from the set, considering the priority hint.
    // When a hint is set, alternates between forward and backward neighbors
    // so filmstrip thumbnails expand symmetrically from the target.
    // Returns jobs_.end() if empty or paused.
    JobSet::iterator pickBestJob_()
    {
        if (jobs_.empty() || pauseDepth_.load(std::memory_order_relaxed) > 0) {
            return jobs_.end();
        }

        if (priorityHint_.empty()) {
            return preferNearbyRaw_(jobs_.begin(), jobs_.end());
        }

        refreshPriorityIterators_();

        if (priorityForward_ != jobs_.end() && priorityBackward_ != jobs_.end()) {
            // Both directions available — alternate to expand symmetrically
            unsigned count = priorityCounter_.fetch_add(1, std::memory_order_relaxed);
            if (count % 2 == 0) {
                auto result = priorityForward_;
                priorityForward_ = std::next(result);
                result = preferNearbyRaw_(result, priorityBackward_);
                invalidatePriorityIterators_();
                return result;
            }

            auto result = priorityBackward_;
            priorityBackward_ =
                result != jobs_.begin()
                    ? std::prev(result)
                    : jobs_.end();
            result = preferNearbyRaw_(result, priorityForward_);
            invalidatePriorityIterators_();
            return result;
        } else if (priorityForward_ != jobs_.end()) {
            auto result = priorityForward_;
            priorityForward_ = std::next(result);
            result = preferNearbyRaw_(result, jobs_.end());
            invalidatePriorityIterators_();
            return result;
        } else if (priorityBackward_ != jobs_.end()) {
            auto result = priorityBackward_;
            priorityBackward_ =
                result != jobs_.begin()
                    ? std::prev(result)
                    : jobs_.end();
            result = preferNearbyRaw_(result, jobs_.end());
            invalidatePriorityIterators_();
            return result;
        }
        return preferNearbyRaw_(jobs_.begin(), jobs_.end());
    }

    void recordJobDurationLocked_(std::chrono::milliseconds duration)
    {
        const auto ms = duration.count();

        if (ms >= 800) {
            ioPressure_ = std::min(12, ioPressure_ + 3);
            fastJobCredit_ = 0;
        } else if (ms >= 250) {
            ioPressure_ = std::min(12, ioPressure_ + 1);
            fastJobCredit_ = 0;
        } else if (ms <= 60) {
            fastJobCredit_ = std::min(64u, fastJobCredit_ + 1);
            if (ioPressure_ > 0 && fastJobCredit_ % 4 == 0) {
                --ioPressure_;
            }
        } else {
            fastJobCredit_ = fastJobCredit_ > 0 ? fastJobCredit_ - 1 : 0;
            if (ioPressure_ > 0 && ms < 150) {
                --ioPressure_;
            }
        }
    }

    int targetThreadCountLocked_() const
    {
        int queueTarget;

        if (postScanDrainMode_.load(std::memory_order_relaxed)) {
            if (jobs_.size() >= 64) {
                queueTarget = maxThreadCount_;
            } else if (jobs_.size() >= 8) {
                queueTarget = std::min(maxThreadCount_, 4);
            } else {
                queueTarget = std::min(maxThreadCount_, 2);
            }
        } else if (jobs_.size() >= 256) {
            queueTarget = maxThreadCount_;
        } else if (jobs_.size() >= 64) {
            queueTarget = std::min(maxThreadCount_, 8);
        } else if (jobs_.size() >= 8) {
            queueTarget = std::min(maxThreadCount_, 4);
        } else {
            queueTarget = std::min(maxThreadCount_, 2);
        }

        int ioTarget;
        if (ioPressure_ >= 6) {
            ioTarget = 2;
        } else if (ioPressure_ >= 3) {
            ioTarget = 3;
        } else if (ioPressure_ >= 1) {
            ioTarget = 4;
        } else if (fastJobCredit_ < 6) {
            ioTarget = 4;
        } else if (fastJobCredit_ < 16) {
            ioTarget = 6;
        } else {
            ioTarget = maxThreadCount_;
        }

        return std::max(1, std::min(queueTarget, ioTarget));
    }

    int scheduleWorkersLocked_()
    {
        if (pauseDepth_.load(std::memory_order_relaxed) > 0 || jobs_.empty()) {
            return 0;
        }

        const int running = g_atomic_int_get(&nConcurrentThreads);
        const int capacity = targetThreadCountLocked_() - running - queuedWorkers_;
        const int usefulWorkers = static_cast<int>(jobs_.size()) - queuedWorkers_;
        const int toSchedule = std::max(0, std::min(capacity, usefulWorkers));
        queuedWorkers_ += toSchedule;

        return toSchedule;
    }

    void pushWorkers_(int count)
    {
        for (int i = 0; i < count; ++i) {
            threadPool_->push(sigc::mem_fun(*this, &PreviewLoader::Impl::processNextJob));
        }
    }

    void wakePendingWorkers()
    {
        int workersToPush = 0;
        {
            MyMutex::MyLock lock(mutex_);
            workersToPush = scheduleWorkersLocked_();
        }

        pushWorkers_(workersToPush);
    }

    bool hasPendingWork()
    {
        MyMutex::MyLock lock(mutex_);
        return !jobs_.empty()
            || queuedWorkers_ > 0
            || g_atomic_int_get(&nConcurrentThreads) > 0;
    }

    void processNextJob()
    {
        setPreviewWorkerThreadPriority();
#ifdef _OPENMP
        // The pool already runs up to eight jobs concurrently. Prevent each
        // worker from retaining its own full OpenMP team on Windows.
        omp_set_num_threads(1);
#endif

        // Peek at the queue before committing to the concurrent thread count.
        // This ensures no-op tasks (from stale thread pool pushes) don't
        // interfere with the nConcurrentThreads counter.
        {
            MyMutex::MyLock lock(mutex_);
            if (queuedWorkers_ > 0) {
                --queuedWorkers_;
            }

            if (jobs_.empty() || pauseDepth_.load(std::memory_order_relaxed) > 0) {
                return;
            }

            g_atomic_int_inc (&nConcurrentThreads);
        }

        int lastDirId = -1;
        PreviewLoaderListener* lastListener = nullptr;
        PreviewLoaderListener* batchListener = nullptr;
        PreviewLoaderListener::PreviewReadyBatch readyBatch;
        int batchGeneration = -1;
        constexpr std::size_t INITIAL_PREVIEW_READY_BATCH_SIZE = 1;
        // Keep UI feedback frequent for RAW-heavy folders. Waiting for 32
        // ready entries can hide progress for seconds when cache misses are
        // expensive; FileCatalog coalesces these callbacks before touching GTK.
        constexpr std::size_t STEADY_PREVIEW_READY_BATCH_SIZE = 2;
        bool firstReadyBatchFlushed = false;
        readyBatch.reserve(STEADY_PREVIEW_READY_BATCH_SIZE);
        auto flushReadyBatch = [&]() {
            if (batchListener && !readyBatch.empty()) {
                if (batchGeneration == generation_.load(std::memory_order_acquire)) {
                    batchListener->previewReadyBatch(std::move(readyBatch));
                    firstReadyBatchFlushed = true;
                } else {
                    for (auto& entry : readyBatch) {
                        delete entry.second;
                    }
                }
                readyBatch.clear();
                readyBatch.reserve(STEADY_PREVIEW_READY_BATCH_SIZE);
            }
            batchListener = nullptr;
            batchGeneration = -1;
        };

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

            setPreviewWorkerThreadPriority();

            // Skip jobs from a previous directory — removeAllJobs() increments
            // the generation counter, so any job queued before the switch has a
            // stale generation and we can skip the expensive cacheMgr I/O.
            if (j.generation_ != generation_.load(std::memory_order_acquire)) {
                continue;
            }

            const auto jobStart = std::chrono::steady_clock::now();
            try {
                Thumbnail* tmb = cacheMgr->getEntry(j.dir_entry_);

                if ( tmb ) {
                    if (j.generation_ != generation_.load(std::memory_order_acquire)) {
                        tmb->decreaseRef();
                        continue;
                    }

                    DEBUG("Preview Ready\n");
                    auto* entry = new FileBrowserEntry(tmb, j.dir_entry_);
                    entry->setBrowserPathKey(std::move(j.dir_entry_key_));
                    if (batchListener && (batchListener != j.listener_ || batchGeneration != j.generation_)) {
                        flushReadyBatch();
                    }
                    batchListener = j.listener_;
                    batchGeneration = j.generation_;
                    readyBatch.emplace_back(j.dir_id_, entry);
                    const std::size_t readyBatchTarget = firstReadyBatchFlushed
                        ? STEADY_PREVIEW_READY_BATCH_SIZE
                        : INITIAL_PREVIEW_READY_BATCH_SIZE;
                    if (readyBatch.size() >= readyBatchTarget) {
                        flushReadyBatch();
                    }
                }

            } catch (Glib::Error &e) {} catch(...) {}

            int workersToPush = 0;
            bool retireWorker = false;
            {
                MyMutex::MyLock lock(mutex_);
                recordJobDurationLocked_(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - jobStart));
                retireWorker = g_atomic_int_get(&nConcurrentThreads) > targetThreadCountLocked_();
                if (!retireWorker) {
                    workersToPush = scheduleWorkersLocked_();
                }
            }
            pushWorkers_(workersToPush);

            lastDirId = j.dir_id_;
            lastListener = j.listener_;
            if (retireWorker) {
                break;
            }
        }

        flushReadyBatch();

        const bool last = g_atomic_int_dec_and_test (&nConcurrentThreads);
        bool finished = false;
        int workersToPush = 0;
        {
            MyMutex::MyLock lock(mutex_);
            if (!jobs_.empty()) {
                workersToPush = scheduleWorkersLocked_();
            } else if (last && lastListener && queuedWorkers_ == 0) {
                finished = true;
            }
        }

        pushWorkers_(workersToPush);

        // signal at end
        if (finished) {
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
    add(dir_id, dir_entry, std::string(), l);
}

void PreviewLoader::add(int dir_id, const Glib::ustring& dir_entry, std::string&& dir_entry_key, PreviewLoaderListener* l)
{
    // somebody listening?
    if ( l != nullptr ) {
        int workersToPush = 0;
        {
            MyMutex::MyLock lock(impl_->mutex_);

            // create a new job and append to queue
            DEBUG("saving job %s", dir_entry.c_str());
            if (impl_->jobs_.insert(Impl::Job(dir_id, dir_entry, l, impl_->generation_.load(std::memory_order_relaxed), std::move(dir_entry_key))).second) {
                impl_->invalidatePriorityIterators_();
                workersToPush = impl_->scheduleWorkersLocked_();
            }
        }

        if (workersToPush > 0) {
            DEBUG("adding %d run request(s) for %s", workersToPush, dir_entry.c_str());
            impl_->pushWorkers_(workersToPush);
        }
    }
}

void PreviewLoader::addBatch(int dir_id, std::vector<Glib::ustring>&& dir_entries, PreviewLoaderListener* l)
{
    addBatch(dir_id, std::move(dir_entries), std::vector<std::string>(), l);
}

void PreviewLoader::addBatch(int dir_id, std::vector<Glib::ustring>&& dir_entries, std::vector<std::string>&& dir_entry_keys, PreviewLoaderListener* l)
{
    if (l == nullptr || dir_entries.empty()) {
        return;
    }

    constexpr std::size_t SMALL_BATCH_FAST_PATH = 32;
    const bool haveEntryKeys = dir_entry_keys.size() == dir_entries.size();

    int workersToPush = 0;
    if (dir_entries.size() <= SMALL_BATCH_FAST_PATH) {
        {
            MyMutex::MyLock lock(impl_->mutex_);

            const int generation = impl_->generation_.load(std::memory_order_relaxed);
            bool inserted = false;
            for (std::size_t i = 0; i < dir_entries.size(); ++i) {
                inserted = impl_->jobs_.emplace(
                    dir_id,
                    std::move(dir_entries[i]),
                    l,
                    generation,
                    haveEntryKeys ? std::move(dir_entry_keys[i]) : std::string()).second || inserted;
            }

            if (inserted) {
                impl_->invalidatePriorityIterators_();
                workersToPush = impl_->scheduleWorkersLocked_();
            }
        }

        if (workersToPush > 0) {
            impl_->pushWorkers_(workersToPush);
        }

        return;
    }

    std::vector<std::size_t> sortedKeyOrder;

    if (haveEntryKeys && !std::is_sorted(dir_entry_keys.begin(), dir_entry_keys.end())) {
        sortedKeyOrder.reserve(dir_entry_keys.size());

        for (std::size_t i = 0; i < dir_entry_keys.size(); ++i) {
            sortedKeyOrder.push_back(i);
        }

        std::sort(
            sortedKeyOrder.begin(),
            sortedKeyOrder.end(),
            [&dir_entry_keys](std::size_t lhs, std::size_t rhs) {
                return dir_entry_keys[lhs] < dir_entry_keys[rhs];
            });
    } else if (!haveEntryKeys && !std::is_sorted(dir_entries.begin(), dir_entries.end())) {
        std::sort(dir_entries.begin(), dir_entries.end());
    }
    {
        MyMutex::MyLock lock(impl_->mutex_);

        const int generation = impl_->generation_.load(std::memory_order_relaxed);
        const auto oldSize = impl_->jobs_.size();
        if (haveEntryKeys) {
            auto hint = impl_->jobs_.end();
            if (sortedKeyOrder.empty()) {
                for (std::size_t i = 0; i < dir_entries.size(); ++i) {
                    hint = impl_->jobs_.emplace_hint(
                        hint,
                        dir_id,
                        std::move(dir_entries[i]),
                        l,
                        generation,
                        std::move(dir_entry_keys[i]));
                }
            } else {
                for (const std::size_t i : sortedKeyOrder) {
                    hint = impl_->jobs_.emplace_hint(
                        hint,
                        dir_id,
                        std::move(dir_entries[i]),
                        l,
                        generation,
                        std::move(dir_entry_keys[i]));
                }
            }
        } else {
            auto hint = impl_->jobs_.end();
            for (auto& dir_entry : dir_entries) {
                hint = impl_->jobs_.emplace_hint(hint, dir_id, std::move(dir_entry), l, generation);
            }
        }

        if (impl_->jobs_.size() != oldSize) {
            impl_->invalidatePriorityIterators_();
            workersToPush = impl_->scheduleWorkersLocked_();
        }
    }

    if (workersToPush > 0) {
        impl_->pushWorkers_(workersToPush);
    }
}

void PreviewLoader::removeAllJobs()
{
    DEBUG("stop %d", impl_->nConcurrentThreads);
    MyMutex::MyLock lock(impl_->mutex_);
    impl_->jobs_.clear();
    impl_->priorityHint_.clear();
    impl_->priorityHintKey_.clear();
    impl_->invalidatePriorityIterators_();
    impl_->queuedWorkers_ = 0;
    impl_->postScanDrainMode_.store(false, std::memory_order_release);
    // Increment generation so in-flight threads that already dequeued a job
    // from the old directory will skip the expensive cacheMgr I/O.
    impl_->generation_.fetch_add(1, std::memory_order_release);
}

void PreviewLoader::setPriorityHint(const Glib::ustring& targetFile)
{
    std::string targetFileKey;

    if (!targetFile.empty()) {
        targetFileKey = targetFile.raw();
    }

    setPriorityHint(targetFile, std::move(targetFileKey));
}

void PreviewLoader::setPriorityHint(const Glib::ustring& targetFile, std::string&& targetFileKey)
{
    MyMutex::MyLock lock(impl_->mutex_);
    if (targetFileKey.empty() && !targetFile.empty()) {
        targetFileKey = targetFile.raw();
    }

    if (impl_->priorityHint_ == targetFile && impl_->priorityHintKey_ == targetFileKey) {
        return;
    }

    impl_->priorityHint_ = targetFile;
    impl_->priorityHintKey_ = std::move(targetFileKey);
    impl_->priorityCounter_.store(0, std::memory_order_relaxed);
    impl_->invalidatePriorityIterators_();
}

void PreviewLoader::pause()
{
    impl_->pauseDepth_.fetch_add(1, std::memory_order_acq_rel);
}

void PreviewLoader::resume()
{
    unsigned depth = impl_->pauseDepth_.load(std::memory_order_acquire);
    while (depth > 0) {
        if (impl_->pauseDepth_.compare_exchange_weak(
                depth,
                depth - 1,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            if (depth > 1) {
                return;
            }
            break;
        }
    }

    // Re-schedule pending jobs without flooding the thread pool with one
    // task per file.
    if (impl_->hasPendingWork()) {
        impl_->wakePendingWorkers();
    }
}

void PreviewLoader::setPostScanDrainMode(bool enabled)
{
    const bool old = impl_->postScanDrainMode_.exchange(enabled, std::memory_order_acq_rel);
    if (enabled && !old) {
        impl_->wakePendingWorkers();
    }
}

bool PreviewLoader::hasPendingWork() const
{
    return impl_->hasPendingWork();
}

void PreviewLoader::wakePendingWorkers()
{
    impl_->wakePendingWorkers();
}
