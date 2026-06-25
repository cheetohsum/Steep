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
#include <condition_variable>
#include <cstdint>
#include <iterator>
#include <list>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <gtkmm.h>

#include "thumbimageupdater.h"
#include "thumbbrowserentrybase.h"

#include "guiutils.h"
#include "rawloadactivity.h"
#include "threadutils.h"
#include "thumbnail.h"

#ifdef _WIN32
#include "rtengine/leanwindows.h"
#endif

#include "rtengine/procparams.h"

#ifdef _OPENMP
#include <omp.h>
#endif

#define DEBUG(format,args...)
//#define DEBUG(format,args...) printf("ThumbImageUpdate::%s: " format "\n", __FUNCTION__, ## args)

static void lowerBackgroundWorkerThreadPriority()
{
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
}

namespace
{

constexpr int kRawUpgradeForegroundQuietMs = 600;
constexpr int kRawUpgradeForegroundRetryMs = 75;

void waitForForegroundRawQuietBeforeUpgrade()
{
    while (true) {
        const int retryMs = rawLoadForegroundQuietRetryMs(
            kRawUpgradeForegroundQuietMs,
            kRawUpgradeForegroundRetryMs);
        if (retryMs <= 0) {
            return;
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(std::min(retryMs, kRawUpgradeForegroundRetryMs)));
    }
}

} // namespace

class ThumbImageUpdater::Impl :
    public rtengine::NonCopyable
{
public:

    struct Job {
        Job(ThumbBrowserEntryBase* tbe, bool* priority, bool upgrade,
            bool forceUpgrade, bool cachePixbuf, ThumbImageUpdateListener* listener):
            tbe_(tbe),
            /*pparams_(pparams),
            height_(height), */
            priority_(priority),
            upgrade_(upgrade),
            force_upgrade_(forceUpgrade),
            cache_pixbuf_(cachePixbuf),
            listener_(listener)
        {}

        Job():
            tbe_(nullptr),
            priority_(nullptr),
            upgrade_(false),
            force_upgrade_(false),
            cache_pixbuf_(false),
            listener_(nullptr)
        {}

        ThumbBrowserEntryBase* tbe_;
        /*rtengine::procparams::ProcParams pparams_;
        int height_;*/
        bool* priority_;
        bool upgrade_;
        bool force_upgrade_;
        bool cache_pixbuf_;
        ThumbImageUpdateListener* listener_;
    };

    typedef std::list<Job> JobList;

    struct JobKey {
        ThumbBrowserEntryBase* tbe;
        ThumbImageUpdateListener* listener;
        bool upgrade;
        bool force_upgrade;

        bool operator==(const JobKey& other) const
        {
            return tbe == other.tbe
                && listener == other.listener
                && upgrade == other.upgrade
                && force_upgrade == other.force_upgrade;
        }
    };

    struct JobKeyHash {
        std::size_t operator()(const JobKey& key) const
        {
            auto hash = static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(key.tbe));
            const auto listenerHash = static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(key.listener));
            hash ^= listenerHash + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            hash ^= static_cast<std::size_t>(key.upgrade) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            hash ^= static_cast<std::size_t>(key.force_upgrade) + 0x9e3779b9 + (hash << 6) + (hash >> 2);

            return hash;
        }
    };

    Impl():
        active_(0),
        inactive_waiting_(false),
        maxThreadCount_(1),
        queuedWorkers_(0),
        nonUpgradeJobs_(0),
        pauseDepth_(0),
        priorityScanNeeded_(true)
    {
#ifdef _OPENMP
        int threadCount = omp_get_num_procs();
#else
        int threadCount = std::max(2u, std::thread::hardware_concurrency());
#endif
        // Thumbnail upgrades are background work. Keep them bounded so large
        // folders don't contend heavily with foreground RAW decoding.
        threadCount = std::max(1, std::min(threadCount, 2));
        maxThreadCount_ = threadCount;

        threadPool_.reset(new Glib::ThreadPool(threadCount, true));
        priorityScanHint_ = jobs_.end();
        firstCachePixbuf_ = jobs_.end();
        firstNonUpgrade_ = jobs_.end();
    }

    std::unique_ptr<Glib::ThreadPool> threadPool_;

    // Need to be a std::mutex because used in a std::condition_variable object...
    // This is the only exceptions along with GThreadMutex (guiutils.cc), MyMutex is used everywhere else
    std::mutex mutex_;

    JobList jobs_;

    std::unordered_map<JobKey, JobList::iterator, JobKeyHash> jobIndex_;

    JobList::iterator priorityScanHint_;

    JobList::iterator firstCachePixbuf_;

    JobList::iterator firstNonUpgrade_;

    std::atomic<unsigned int> active_;

    bool inactive_waiting_;

    std::condition_variable inactive_;

    // Listeners that have been removed — skip callbacks for these.
    std::unordered_set<ThumbImageUpdateListener*> cancelled_;

    std::unordered_map<ThumbImageUpdateListener*, unsigned int> callbacksInFlight_;

    int maxThreadCount_;

    int queuedWorkers_;

    std::size_t nonUpgradeJobs_;

    std::atomic<unsigned> pauseDepth_;

    bool priorityScanNeeded_;

    struct CallbackLease {
        CallbackLease(Impl& impl, ThumbImageUpdateListener* listener):
            impl_(impl),
            listener_(listener),
            active_(false)
        {}

        ~CallbackLease()
        {
            if (active_) {
                impl_.finishCallback_(listener_);
            }
        }

        Impl& impl_;
        ThumbImageUpdateListener* listener_;
        bool active_;
    };

    static JobKey
    keyForJob_(const Job& job)
    {
        return JobKey{job.tbe_, job.listener_, job.upgrade_, job.force_upgrade_};
    }

    JobList::iterator
    findNextNonUpgradeLocked_(JobList::iterator start)
    {
        for (auto i = start; i != jobs_.end(); ++i) {
            if (!i->upgrade_) {
                return i;
            }
        }

        return jobs_.end();
    }

    JobList::iterator
    findNextCachePixbufLocked_(JobList::iterator start, bool nonUpgradeOnly = false)
    {
        for (auto i = start; i != jobs_.end(); ++i) {
            if (i->cache_pixbuf_ && (!nonUpgradeOnly || !i->upgrade_)) {
                return i;
            }
        }

        return jobs_.end();
    }

    JobList::iterator
    eraseJobLocked_(JobList::iterator job)
    {
        const auto next = std::next(job);
        const bool erasedFirstCachePixbuf = firstCachePixbuf_ == job;

        if (priorityScanHint_ == job) {
            priorityScanHint_ = next;
        }

        if (!job->upgrade_ && nonUpgradeJobs_ > 0) {
            --nonUpgradeJobs_;
            if (firstNonUpgrade_ == job) {
                firstNonUpgrade_ = findNextNonUpgradeLocked_(next);
            }
        }

        jobIndex_.erase(keyForJob_(*job));
        const auto erasedNext = jobs_.erase(job);

        if (jobs_.empty()) {
            priorityScanHint_ = jobs_.end();
            firstCachePixbuf_ = jobs_.end();
            firstNonUpgrade_ = jobs_.end();
        } else if (priorityScanHint_ == jobs_.end()) {
            priorityScanHint_ = jobs_.begin();
        }
        if (!jobs_.empty() && erasedFirstCachePixbuf) {
            firstCachePixbuf_ = findNextCachePixbufLocked_(jobs_.begin());
        }

        return erasedNext;
    }

    bool
    addJobLocked_(const Job& job)
    {
        const JobKey key = keyForJob_(job);
        const auto existing = jobIndex_.find(key);
        if (existing != jobIndex_.end()) {
            DEBUG("updating job %s", job.tbe_->shortname.c_str());
            // we have one, update queue entry, will be picked up by thread when processed
            existing->second->priority_ = job.priority_;
            const bool gainedCachePixbuf = job.cache_pixbuf_ && !existing->second->cache_pixbuf_;
            existing->second->cache_pixbuf_ = existing->second->cache_pixbuf_ || job.cache_pixbuf_;
            if (gainedCachePixbuf) {
                firstCachePixbuf_ = findNextCachePixbufLocked_(jobs_.begin());
            }
            if (job.priority_ && *job.priority_) {
                priorityScanNeeded_ = true;
            }
            return false;
        }

        // create a new job and append to queue
        DEBUG("queueing job %s", job.tbe_->shortname.c_str());
        jobs_.push_back(job);
        auto inserted = jobs_.end();
        --inserted;
        jobIndex_.emplace(key, inserted);
        if (job.cache_pixbuf_ && firstCachePixbuf_ == jobs_.end()) {
            firstCachePixbuf_ = inserted;
        }
        if (!job.upgrade_) {
            ++nonUpgradeJobs_;
            if (firstNonUpgrade_ == jobs_.end()) {
                firstNonUpgrade_ = inserted;
            }
        }

        if (priorityScanHint_ == jobs_.end()) {
            priorityScanHint_ = inserted;
        }

        if (job.priority_ && *job.priority_) {
            priorityScanNeeded_ = true;
        }

        return true;
    }

    JobList::iterator
    findPriorityJobLocked_(bool nonUpgradeOnly = false)
    {
        if (jobs_.empty() || !priorityScanNeeded_) {
            return jobs_.end();
        }

        auto start = priorityScanHint_;
        if (start == jobs_.end()) {
            start = jobs_.begin();
        }

        auto i = start;
        bool sawPriority = false;
        do {
            if ( *(i->priority_) ) {
                sawPriority = true;
                if (nonUpgradeOnly && i->upgrade_) {
                    ++i;
                    if (i == jobs_.end()) {
                        i = jobs_.begin();
                    }
                    continue;
                }
                DEBUG("processing(priority) %s", i->tbe_->thumbnail->getFileName().c_str());
                priorityScanHint_ = std::next(i);
                if (priorityScanHint_ == jobs_.end()) {
                    priorityScanHint_ = jobs_.begin();
                }
                return i;
            }

            ++i;
            if (i == jobs_.end()) {
                i = jobs_.begin();
            }
        } while (i != start);

        priorityScanHint_ = jobs_.begin();
        if (!nonUpgradeOnly || !sawPriority) {
            priorityScanNeeded_ = false;
        }
        return jobs_.end();
    }

    JobList::iterator
    pickBestJobLocked_()
    {
        JobList::iterator i = findPriorityJobLocked_(true);

        if (i == jobs_.end()) {
            i = findNextCachePixbufLocked_(jobs_.begin(), true);
            if (i != jobs_.end()) {
                DEBUG("processing(cache-pixbuf/not-upgrade) %s", i->tbe_->thumbnail->getFileName().c_str());
            }
        }

        // see if any non-upgrade jobs exist; these put first-pass thumbnails
        // on screen and should beat expensive RAW thumbnail upgrades.
        if (i == jobs_.end() && firstNonUpgrade_ != jobs_.end()) {
            i = firstNonUpgrade_;
            DEBUG("processing(not-upgrade) %s", i->tbe_->thumbnail->getFileName().c_str());
        }

        if (i == jobs_.end()) {
            i = findPriorityJobLocked_();
        }

        if (i == jobs_.end() && firstCachePixbuf_ != jobs_.end()) {
            i = firstCachePixbuf_;
            DEBUG("processing(cache-pixbuf) %s", i->tbe_->thumbnail->getFileName().c_str());
        }

        // if none, then use first
        if ( i == jobs_.end() ) {
            i = jobs_.begin();
            DEBUG("processing(first) %s", i->tbe_->thumbnail->getFileName().c_str());
        }

        return i;
    }

    bool
    takeNextJobLocked_(Job& j, Thumbnail*& thm, std::pair<hidpi::LogicalSize, int>& size_and_scale)
    {
        while (!jobs_.empty()) {
            JobList::iterator i = pickBestJobLocked_();

            if (i == jobs_.end()) {
                return false;
            }

            // copy found job
            j = *i;

            // remove so not run again
            eraseJobLocked_(i);
            DEBUG("%d job(s) remaining", int(jobs_.size()) );

            // If listener already cancelled, skip entirely
            if (cancelled_.find(j.listener_) != cancelled_.end()) {
                continue;
            }

            // Copy data from entry while it's guaranteed alive (under lock).
            // Hold a reference to the thumbnail so it survives entry deletion.
            thm = j.tbe_->thumbnail;
            thm->increaseRef();
            size_and_scale = j.tbe_->getDesiredPreviewSize();

            return true;
        }

        return false;
    }

    int
    scheduleWorkersLocked_()
    {
        if (jobs_.empty() || pauseDepth_.load(std::memory_order_relaxed) > 0) {
            return 0;
        }

        const int running = static_cast<int>(active_.load(std::memory_order_relaxed));
        const int capacity = maxThreadCount_ - running - queuedWorkers_;
        const int usefulWorkers = static_cast<int>(jobs_.size()) - queuedWorkers_;
        const int toSchedule = std::max(0, std::min(capacity, usefulWorkers));
        queuedWorkers_ += toSchedule;

        return toSchedule;
    }

    void
    pushWorkers_(int count)
    {
        for (int i = 0; i < count; ++i) {
            threadPool_->push(sigc::mem_fun(*this, &ThumbImageUpdater::Impl::processQueuedJobs_));
        }
    }

    void
    notifyInactiveLocked_()
    {
        if (active_.load(std::memory_order_relaxed) == 0 && queuedWorkers_ == 0) {
            // All jobs finished; safe to clear cancelled set.
            cancelled_.clear();
            if (inactive_waiting_) {
                inactive_waiting_ = false;
                inactive_.notify_all();
            }
        }
    }

    bool
    beginCallbackLocked_(ThumbImageUpdateListener* listener)
    {
        if (cancelled_.find(listener) != cancelled_.end()) {
            return false;
        }

        ++callbacksInFlight_[listener];
        return true;
    }

    void
    finishCallback_(ThumbImageUpdateListener* listener)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto callback = callbacksInFlight_.find(listener);
        if (callback != callbacksInFlight_.end()) {
            if (callback->second <= 1) {
                callbacksInFlight_.erase(callback);
                inactive_.notify_all();
            } else {
                --callback->second;
            }
        }
    }

    void
    processQueuedJobs_()
    {
        lowerBackgroundWorkerThreadPriority();

        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (queuedWorkers_ > 0) {
                --queuedWorkers_;
            }

            // nothing to do; could be jobs have been removed
            if ( jobs_.empty() || pauseDepth_.load(std::memory_order_relaxed) > 0 ) {
                DEBUG("processing: nothing to do (%d)", jobs_.empty());
                notifyInactiveLocked_();
                return;
            }

            ++active_;
        }

        while (true) {
            Job j;
            Thumbnail* thm = nullptr;
            std::pair<hidpi::LogicalSize, int> size_and_scale;

            {
                std::lock_guard<std::mutex> lock(mutex_);

                if (pauseDepth_.load(std::memory_order_relaxed) > 0) {
                    break;
                }

                if (!takeNextJobLocked_(j, thm, size_and_scale)) {
                    break;
                }
            }

            // Process using copied data; no more access to j.tbe_.
            double scale = 1.0;
            rtengine::IImage8* img = nullptr;
            hidpi::LogicalSize logical = size_and_scale.first;
            int device_scale = size_and_scale.second;
            int preview_height = logical.scaleToDevice(device_scale).height;
            rtengine::procparams::CropParams crop;

            if ( j.upgrade_ ) {
                if ( thm->isQuick() || j.force_upgrade_ ) {
                    if (thm->getType() == FT_Raw) {
                        waitForForegroundRawQuietBeforeUpgrade();
                    }
                    img = thm->upgradeThumbImage(preview_height, scale, j.force_upgrade_, &crop, j.cache_pixbuf_);
                }
            } else {
                img = thm->processThumbImage(preview_height, scale, &crop, j.cache_pixbuf_);
            }

            if (img) {
                CallbackLease callback(*this, j.listener_);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    callback.active_ = beginCallbackLocked_(j.listener_);
                }

                if (callback.active_) {
                    DEBUG("pushing image %s", thm->getFileName().c_str());
                    ThumbImageUpdateListener::ImageUpdate update(img, logical, device_scale, scale, crop);
                    j.listener_->updateImage(update);
                } else {
                    // Listener was removed; discard result, don't callback.
                    delete img;
                }
            }

            // Release our reference to the thumbnail.
            thm->decreaseRef();
        }

        --active_;

        int workersToPush = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!jobs_.empty()) {
                workersToPush = scheduleWorkersLocked_();
            }
            notifyInactiveLocked_();
        }

        pushWorkers_(workersToPush);
    }

};

ThumbImageUpdater*
ThumbImageUpdater::getInstance()
{
    static ThumbImageUpdater instance_;
    return &instance_;
}

ThumbImageUpdater::ThumbImageUpdater():
    impl_(new Impl())
{
}

ThumbImageUpdater::~ThumbImageUpdater() {
    delete impl_;
}

void ThumbImageUpdater::add(ThumbBrowserEntryBase* tbe, bool* priority, bool upgrade, bool forceUpgrade, ThumbImageUpdateListener* l)
{
    // nobody listening?
    if ( l == nullptr ) {
        return;
    }

    int workersToPush = 0;

    {
        std::lock_guard<std::mutex> lock(impl_->mutex_);

        if (!impl_->addJobLocked_(Impl::Job(tbe, priority, upgrade, forceUpgrade, tbe->shouldCacheRenderedThumbnailPixbuf(), l))) {
            return;
        }

        workersToPush = impl_->scheduleWorkersLocked_();
    }

    if (workersToPush > 0) {
        DEBUG("adding %d run request(s) for %s", workersToPush, tbe->shortname.c_str());
        impl_->pushWorkers_(workersToPush);
    }
}

void ThumbImageUpdater::addBatch(const std::vector<Request>& requests)
{
    if (requests.empty()) {
        return;
    }

    int workersToPush = 0;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex_);

        impl_->jobIndex_.reserve(impl_->jobIndex_.size() + requests.size());

        bool inserted = false;
        for (const auto& request : requests) {
            if (request.listener == nullptr || request.tbe == nullptr) {
                continue;
            }
            inserted = impl_->addJobLocked_(Impl::Job(
                request.tbe,
                request.priority,
                request.upgrade,
                request.forceUpgrade,
                request.cachePixbuf,
                request.listener)) || inserted;
        }

        if (inserted) {
            workersToPush = impl_->scheduleWorkersLocked_();
        }
    }

    if (workersToPush > 0) {
        impl_->pushWorkers_(workersToPush);
    }
}

void ThumbImageUpdater::prioritiesChanged()
{
    int workersToPush = 0;

    {
        std::lock_guard<std::mutex> lock(impl_->mutex_);
        impl_->priorityScanNeeded_ = true;
        workersToPush = impl_->scheduleWorkersLocked_();
    }

    if (workersToPush > 0) {
        impl_->pushWorkers_(workersToPush);
    }
}

void ThumbImageUpdater::pause()
{
    impl_->pauseDepth_.fetch_add(1, std::memory_order_acq_rel);
}

void ThumbImageUpdater::resume()
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

    int workersToPush = 0;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex_);
        workersToPush = impl_->scheduleWorkersLocked_();
    }

    if (workersToPush > 0) {
        impl_->pushWorkers_(workersToPush);
    }
}

void ThumbImageUpdater::removeJobs(ThumbImageUpdateListener* listener)
{
    DEBUG("removeJobs(%p)", listener);

    std::unique_lock<std::mutex> lock(impl_->mutex_);

    for( Impl::JobList::iterator i(impl_->jobs_.begin()); i != impl_->jobs_.end(); ) {
        if (i->listener_ == listener) {
            DEBUG("erasing specific job");
            i = impl_->eraseJobLocked_(i);
        } else {
            ++i;
        }
    }

    // Mark listener as cancelled so any in-flight jobs skip the callback
    // instead of delivering to a deleted object.
    impl_->cancelled_.insert(listener);
    impl_->inactive_.wait(lock, [this, listener]() {
        return impl_->callbacksInFlight_.find(listener) == impl_->callbacksInFlight_.end();
    });
}

void ThumbImageUpdater::removeAllJobs()
{
    DEBUG("stop");

    std::lock_guard<std::mutex> lock(impl_->mutex_);
    impl_->jobs_.clear();
    impl_->jobIndex_.clear();
    impl_->priorityScanHint_ = impl_->jobs_.end();
    impl_->firstCachePixbuf_ = impl_->jobs_.end();
    impl_->firstNonUpgrade_ = impl_->jobs_.end();
    impl_->nonUpgradeJobs_ = 0;
    impl_->priorityScanNeeded_ = false;
    // Don't wait for active jobs — they will finish naturally and their
    // results will be harmlessly delivered to entries pending deletion.
}

