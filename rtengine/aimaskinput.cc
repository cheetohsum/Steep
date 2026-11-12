/*
 *  This file is part of RawTherapee.
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
#include "aimaskinput.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

#include "aiidlereaper.h"
#include "aimaskcache.h"
#include "aisegmentation.h"
#include "partnerdepthstore.h"
#include "colortemp.h"
#include "edittrace.h"
#include "iccstore.h"
#include "iimage.h"
#include "imagedimensions.h"
#include "imagefloat.h"
#include "imagesource.h"
#include "improcfun.h"
#include "procparams.h"

namespace rtengine
{

namespace
{

class Fnv
{
public:
    void add(const void* data, std::size_t size)
    {
        const unsigned char* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            hash_ = (hash_ ^ bytes[i]) * 1099511628211ULL;
        }
    }
    template<typename T>
    void value(const T& v)
    {
        add(&v, sizeof(v));
    }
    void text(const Glib::ustring& s)
    {
        add(s.raw().data(), s.raw().size());
        value(s.raw().size());
    }
    template<typename T>
    void list(const std::vector<T>& v)
    {
        value(v.size());
        if (!v.empty()) {
            add(v.data(), v.size() * sizeof(T));
        }
    }
    std::uint64_t get() const
    {
        return hash_;
    }

private:
    std::uint64_t hash_ = 14695981039346656037ULL;
};

}

std::uint64_t aiMaskGeometryKey(const procparams::ProcParams& params)
{
    Fnv h;
    h.value(params.coarse.rotate);
    h.value(params.coarse.hflip);
    h.value(params.coarse.vflip);
    h.value(params.rotate.degree);
    h.value(params.distortion.amount);
    h.value(params.distortion.defish);
    h.value(params.distortion.focal_length);
    const auto& p = params.perspective;
    h.text(p.method);
    h.value(p.render);
    for (double v : {p.horizontal, p.vertical, p.camera_crop_factor, p.camera_focal_length, p.camera_pitch,
                     p.camera_roll, p.camera_shift_horiz, p.camera_shift_vert, p.camera_yaw, p.projection_pitch,
                     p.projection_rotate, p.projection_shift_horiz, p.projection_shift_vert, p.projection_yaw}) {
        h.value(v);
    }
    h.value(p.mesh_grid_size);
    h.list(p.mesh_offsets_x);
    h.list(p.mesh_offsets_y);
    const auto& l = params.lensProf;
    h.value(l.lcMode);
    h.text(l.lcpFile);
    h.value(l.useDist);
    h.text(l.lfCameraMake);
    h.text(l.lfCameraModel);
    h.text(l.lfLens);
    h.text(params.commonTrans.method);
    h.value(params.commonTrans.autofill);
    h.value(params.commonTrans.scale);
    h.value(params.commonTrans.scale_horizontally);
    h.value(params.commonTrans.scale_vertically);
    return h.get();
}

std::unique_ptr<Imagefloat> buildAIMaskInput(ImageSource* source,
                                             const procparams::ProcParams& params,
                                             const ColorTemp& wb,
                                             int maxDim,
                                             bool multiThread)
{
    if (!source || maxDim < 16) {
        return nullptr;
    }

    const int tr = getCoarseBitMask(params.coarse);
    int fw = 0;
    int fh = 0;
    source->getFullSize(fw, fh, tr);
    if (fw <= 0 || fh <= 0) {
        return nullptr;
    }

    // The source only subsamples by whole factors; take the nearest one, so
    // a 6240 px frame gets ~2080 rather than dropping to ~1560.
    const int skip = std::max(1, static_cast<int>(std::lround(static_cast<double>(std::max(fw, fh)) / maxDim)));
    const PreviewProps pp(0, 0, fw, fh, skip);
    int w = 0;
    int h = 0;
    source->getSize(pp, w, h);
    if (w <= 0 || h <= 0) {
        return nullptr;
    }

    std::unique_ptr<Imagefloat> image(new Imagefloat(w, h));
    source->getImage(wb, tr, image.get(), pp, params.toneCurve, params.raw);
    source->convertColorSpace(image.get(), params.icm, wb);

    // The same geometry the preview gets (lens, rotation, perspective), so a
    // class map pixel sits where the local adjustment will read it.
    ImProcFunctions ipf(&params, multiThread);
    ipf.setScale(skip);
    if (ipf.needsTransform(fw, fh, source->getRotateDegree(), source->getMetaData())) {
        std::unique_ptr<Imagefloat> transformed(new Imagefloat(w, h));
        ipf.transform(image.get(), transformed.get(), 0, 0, 0, 0, w, h, fw, fh,
                      source->getMetaData(), source->getRotateDegree(), false);
        image = std::move(transformed);
    }

    if (source->isRAW()) {
        // A raw file rendered neutrally is scene-linear with no tone curve:
        // its middle sits a stop or two under a camera JPEG's. Lift it so its
        // median luminance lands where a typical photo's does.
        const TMatrix toXYZ = ICCStore::getInstance()->workingSpaceMatrix(params.icm.workingProfile);
        const float ky[3] = {static_cast<float>(toXYZ[1][0]), static_cast<float>(toXYZ[1][1]), static_cast<float>(toXYZ[1][2])};
        constexpr int bins = 4096;
        std::vector<unsigned> histogram(bins, 0);
        const int step = std::max(1, std::min(w, h) / 256);
        unsigned count = 0;
        for (int y = 0; y < h; y += step) {
            for (int x = 0; x < w; x += step) {
                const float lum = (ky[0] * image->r(y, x) + ky[1] * image->g(y, x) + ky[2] * image->b(y, x)) / 65535.f;
                histogram[std::max(0, std::min(bins - 1, static_cast<int>(lum * bins)))]++;
                ++count;
            }
        }
        unsigned seen = 0;
        int medianBin = 0;
        for (; medianBin < bins - 1 && seen < count / 2; ++medianBin) {
            seen += histogram[medianBin];
        }
        const float median = (medianBin + 0.5f) / bins;
        const float gain = std::max(0.5f, std::min(16.f, 0.18f / std::max(median, 1e-4f)));
        if (std::fabs(gain - 1.f) > 0.05f) {
#ifdef _OPENMP
            #pragma omp parallel for if(multiThread)
#endif
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    image->r(y, x) *= gain;
                    image->g(y, x) *= gain;
                    image->b(y, x) *= gain;
                }
            }
        }
    }

    return image;
}

bool aiMasksNeedDepth(const procparams::ProcParams& params)
{
    if (!params.locallab.enabled) {
        return false;
    }
    for (const auto& spot : params.locallab.spots) {
        if (spot.useAIMask
                && (spot.aiMaskClass == static_cast<int>(AISegClass::FOREGROUND_DEPTH)
                    || spot.aiMaskClass == static_cast<int>(AISegClass::BACKGROUND_DEPTH))) {
            return true;
        }
    }
    return false;
}

namespace
{

// Splits the near and far depth classes out of the scene depth of the same
// frame the class maps came from, so the split lines up with them.
void computeDepthClasses(const std::string& imageId, std::uint64_t key, const Imagefloat& input,
                         int fullW, int fullH, const Glib::ustring& workingProfile)
{
    AIMaskCache& cache = AIMaskCache::getInstance();
    if (!cache.hasCachedMasks(imageId, key) || cache.hasDepthClasses(imageId, key)) {
        return;
    }
    const auto depth = PartnerDepthStore::getInstance().getSceneDepth(imageId, input, fullW, fullH, workingProfile);
    if (depth && depth->valid()) {
        cache.setDepthClasses(imageId, key, *depth);
    }
}

}

bool ensureAIMasks(ImageSource* source, const procparams::ProcParams& params,
                   const ColorTemp& wb, bool multiThread, bool needDepth)
{
    if (!source || !getAISegmentationEngine().isInitialized()) {
        return false;
    }

    const std::string imageId = source->getFileName().raw();
    const std::uint64_t key = aiMaskGeometryKey(params);
    AIMaskCache& cache = AIMaskCache::getInstance();
    const bool haveMasks = cache.hasCachedMasks(imageId, key);
    if (haveMasks && (!needDepth || cache.hasDepthClasses(imageId, key))) {
        return true;
    }

    const std::unique_ptr<Imagefloat> input = buildAIMaskInput(source, params, wb, AI_MASK_INPUT_DIMENSION, multiThread);
    if (!input) {
        return false;
    }

    int fullW = 0;
    int fullH = 0;
    source->getFullSize(fullW, fullH, getCoarseBitMask(params.coarse));
    if (!haveMasks) {
        cache.computeMasks(imageId, input->r.ptrs, input->g.ptrs, input->b.ptrs,
                           input->getWidth(), input->getHeight(), fullW, fullH,
                           params.icm.workingProfile.raw(), multiThread, key);
    }
    if (needDepth) {
        computeDepthClasses(imageId, key, *input, fullW, fullH, params.icm.workingProfile);
    }
    return cache.hasCachedMasks(imageId, key);
}

namespace
{

// The one thread the editor's AI mask analysis runs on (see requestAIMasks).
// It holds at most one job waiting: a newer photo replaces an older one that
// has not started, since nobody is looking at that photo any more.
class AIMaskWorker
{
public:
    struct Job
    {
        std::string imageId;
        std::uint64_t key = 0;
        std::string workingProfile;
        std::unique_ptr<Imagefloat> input;
        int fullW = 0;
        int fullH = 0;
        bool multiThread = true;
        bool needDepth = false;
    };

    static AIMaskWorker& instance()
    {
        // Never destroyed: the thread is detached and may be mid-inference
        // when static destructors run at exit.
        static AIMaskWorker* const worker = new AIMaskWorker();
        return *worker;
    }

    bool failed(const std::string& imageId, std::uint64_t key)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return !failedId_.empty() && failedId_ == imageId && failedKey_ == key;
    }

    // Registers the callback when this photo is queued or computing, in the
    // same lock the worker takes to collect callbacks, so it cannot miss one.
    bool attachIfBusy(const std::string& imageId, std::uint64_t key, bool needDepth,
                      const void* owner, std::function<void()>& onReady)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool queued = queued_ && queued_->imageId == imageId && queued_->key == key;
        // A running job that will not estimate depth cannot answer a depth
        // request: the caller queues one more (its maps step is a cache hit).
        const bool running = running_ && runningId_ == imageId && runningKey_ == key
                             && (!needDepth || runningNeedDepth_);
        if (!queued && !running) {
            return false;
        }
        if (queued && needDepth) {
            queued_->needDepth = true;
        }
        addWaiterLocked(imageId, key, owner, onReady);
        return true;
    }

    bool runningOther(const std::string& imageId, std::uint64_t key)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return running_ && (runningId_ != imageId || runningKey_ != key);
    }

    void submit(std::unique_ptr<Job> job, const void* owner, std::function<void()>& onReady)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queued_) {
            // Replaced before it started: its callbacks would only start a
            // pass for a photo that is no longer coming.
            const std::string id = queued_->imageId;
            const std::uint64_t key = queued_->key;
            waiters_.erase(std::remove_if(waiters_.begin(), waiters_.end(),
                                          [&](const Waiter& w) { return w.imageId == id && w.key == key; }),
                           waiters_.end());
        }
        addWaiterLocked(job->imageId, job->key, owner, onReady);
        queued_ = std::move(job);
        if (!started_) {
            started_ = true;
            std::thread([this]() { run(); }).detach();
        }
        ready_.notify_one();
    }

    void forget(const void* owner)
    {
        std::lock_guard<std::mutex> fire(fireMutex_);
        std::lock_guard<std::mutex> lock(mutex_);
        waiters_.erase(std::remove_if(waiters_.begin(), waiters_.end(),
                                      [owner](const Waiter& w) { return w.owner == owner; }),
                       waiters_.end());
    }

private:
    struct Waiter
    {
        const void* owner;
        std::string imageId;
        std::uint64_t key;
        std::function<void()> onReady;
    };

    AIMaskWorker() = default;

    void addWaiterLocked(const std::string& imageId, std::uint64_t key,
                         const void* owner, std::function<void()>& onReady)
    {
        if (!onReady) {
            return;
        }
        for (Waiter& w : waiters_) {
            if (w.owner == owner && w.imageId == imageId && w.key == key) {
                w.onReady = std::move(onReady);
                return;
            }
        }
        waiters_.push_back({owner, imageId, key, std::move(onReady)});
    }

    void run()
    {
#ifdef _OPENMP
        // This thread lives as long as the process and so does its OpenMP
        // team; the models bring their own thread pools, so four is plenty
        // for the resampling around them.
        omp_set_num_threads(4);
#endif
        while (true) {
            std::unique_ptr<Job> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [this]() { return static_cast<bool>(queued_); });
                job = std::move(queued_);
                running_ = true;
                runningId_ = job->imageId;
                runningKey_ = job->key;
                runningNeedDepth_ = job->needDepth;
            }

            const long long startUs = edittrace::enabled() ? edittrace::nowUs() : 0;
            AIMaskCache& cache = AIMaskCache::getInstance();
            // A failed analysis is usually memory -- another program holding
            // the GPU for a moment. Free the models and try again a little
            // later, unless the editor has moved on to another photo.
            constexpr int kRetries = 2;
            for (int attempt = 0;; ++attempt) {
                cache.computeMasks(job->imageId, job->input->r.ptrs, job->input->g.ptrs, job->input->b.ptrs,
                                   job->input->getWidth(), job->input->getHeight(), job->fullW, job->fullH,
                                   job->workingProfile, job->multiThread, job->key);
                if (cache.hasCachedMasks(job->imageId, job->key) || !cache.isPending(job->imageId, job->key)
                        || attempt == kRetries) {
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (queued_) {
                        break;
                    }
                }
                std::fprintf(stderr, "AI Masking: analysis failed; freeing models, retry %d of %d\n",
                             attempt + 1, kRetries);
                AIIdleReaper::releaseNow();
                std::this_thread::sleep_for(std::chrono::seconds(2 * (attempt + 1)));
            }
            if (job->needDepth && cache.hasCachedMasks(job->imageId, job->key)) {
                computeDepthClasses(job->imageId, job->key, *job->input, job->fullW, job->fullH,
                                    job->workingProfile);
            }
            job->input.reset();
            const bool ready = cache.hasCachedMasks(job->imageId, job->key);
            // Not ready and still the photo the cache waits for: the models
            // produced nothing. Superseded (another photo took the cache) is
            // not a failure; the next pass asks again.
            const bool failedNow = !ready && cache.isPending(job->imageId, job->key);
            if (failedNow) {
                cache.clearPending(job->imageId, job->key);
            }
            if (edittrace::enabled()) {
                edittrace::logf("[aiMask] worker %s img=%s %.0fms",
                                ready ? "ready" : failedNow ? "failed" : "superseded",
                                job->imageId.c_str(), (edittrace::nowUs() - startUs) / 1000.0);
            }

            std::lock_guard<std::mutex> fire(fireMutex_);
            std::vector<std::function<void()>> callbacks;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                running_ = false;
                if (failedNow) {
                    failedId_ = job->imageId;
                    failedKey_ = job->key;
                } else if (ready && failedId_ == job->imageId) {
                    failedId_.clear();
                }
                for (auto it = waiters_.begin(); it != waiters_.end();) {
                    if (it->imageId == job->imageId && it->key == job->key) {
                        callbacks.push_back(std::move(it->onReady));
                        it = waiters_.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            // Under fireMutex_ only: forget() waits here, so an owner that
            // has returned from it is never called back.
            for (const auto& callback : callbacks) {
                callback();
            }
        }
    }

    std::mutex mutex_;
    std::mutex fireMutex_;
    std::condition_variable ready_;
    std::unique_ptr<Job> queued_;
    bool started_ = false;
    bool running_ = false;
    std::string runningId_;
    std::uint64_t runningKey_ = 0;
    bool runningNeedDepth_ = false;
    std::vector<Waiter> waiters_;
    std::string failedId_;
    std::uint64_t failedKey_ = 0;
};

}

AIMaskRequest requestAIMasks(ImageSource* source, const procparams::ProcParams& params,
                             const ColorTemp& wb, bool multiThread,
                             const void* owner, std::function<void()> onReady, bool needDepth)
{
    if (!source || !getAISegmentationEngine().isInitialized()) {
        return AIMaskRequest::UNAVAILABLE;
    }

    const std::string imageId = source->getFileName().raw();
    const std::uint64_t key = aiMaskGeometryKey(params);
    AIMaskCache& cache = AIMaskCache::getInstance();
    const bool haveMasks = cache.hasCachedMasks(imageId, key);
    if (haveMasks && (!needDepth || cache.hasDepthClasses(imageId, key))) {
        return AIMaskRequest::READY;
    }

    AIMaskWorker& worker = AIMaskWorker::instance();
    if (worker.failed(imageId, key)) {
        return haveMasks ? AIMaskRequest::READY : AIMaskRequest::UNAVAILABLE;
    }
    if (worker.attachIfBusy(imageId, key, needDepth, owner, onReady)) {
        return AIMaskRequest::PENDING;
    }

    const long long startUs = edittrace::enabled() ? edittrace::nowUs() : 0;
    std::unique_ptr<Imagefloat> input = buildAIMaskInput(source, params, wb, AI_MASK_INPUT_DIMENSION, multiThread);
    if (!input) {
        return AIMaskRequest::UNAVAILABLE;
    }

    std::unique_ptr<AIMaskWorker::Job> job(new AIMaskWorker::Job());
    source->getFullSize(job->fullW, job->fullH, getCoarseBitMask(params.coarse));
    job->workingProfile = params.icm.workingProfile.raw();
    // With the maps already in, only the depth step is left: the job's maps
    // step is then a cache hit.
    if (!haveMasks
            && cache.restoreOrBeginPending(imageId, input->getWidth(), input->getHeight(),
                                           job->fullW, job->fullH, job->workingProfile, key,
                                           worker.runningOther(imageId, key))
            && (!needDepth || cache.hasDepthClasses(imageId, key))) {
        return AIMaskRequest::READY;
    }

    if (edittrace::enabled()) {
        edittrace::logf("[aiMask] queued img=%s input=%dx%d build=%.0fms",
                        imageId.c_str(), input->getWidth(), input->getHeight(),
                        (edittrace::nowUs() - startUs) / 1000.0);
    }
    job->imageId = imageId;
    job->key = key;
    job->input = std::move(input);
    job->multiThread = multiThread;
    job->needDepth = needDepth;
    worker.submit(std::move(job), owner, onReady);
    return AIMaskRequest::PENDING;
}

void cancelAIMaskRequests(const void* owner)
{
    AIMaskWorker::instance().forget(owner);
}

}
