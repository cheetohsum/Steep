/*
 *  This file is part of RawTherapee.
 *
 *  Headless edit-latency bench for steep-cli.
 *
 *  The GUI bench (STEEP_EDIT_BENCH in editorpanel.cc) needs a window. This
 *  one drives an ImProcCoordinator and one detail crop directly, the way the
 *  editor does, with no GTK at all:
 *
 *    STEEP_CLI_EDIT_BENCH=<raw file>     run the bench instead of the CLI
 *    STEEP_CLI_EDIT_BENCH_TARGET=<name>  locallab (default) | chmixer |
 *                                        vibrance | exposure | spotsel | pan |
 *                                        wb | grain | gradient
 *    STEEP_CLI_EDIT_BENCH_STEPS=<n>      edits to time (default 30)
 *    STEEP_CLI_EDIT_BENCH_SKIP=<n>       crop subsampling, 4 ~ fit zoom
 *    STEEP_CLI_EDIT_BENCH_PP3=<file>     profile applied on top of defaults
 *    STEEP_CLI_EDIT_BENCH_OUT=<dir>      write the settled crop as PPM
 *
 *  A/B in one binary: STEEP_LEGACY_TONE, STEEP_LEGACY_SETTLE (engine switches).
 *
 *  Every edit is submitted after the previous one has painted, so the number
 *  reported is one pass, not a queue. After the drag the bench asks for the
 *  settled refinement exactly as EditorPanel does (M_HIGHQUAL|M_MONITOR) and
 *  times that too; the settled crop is the frame to compare between builds.
 *
 *  target=aimask: the spot is an AI-masked one (class from
 *  STEEP_CLI_EDIT_BENCH_AICLASS, default 8 = subject), and the edits start
 *  while its first analysis is still running (run with
 *  STEEP_AIMASK_NO_DISK_CACHE=1 for a cold one). Each pass prints whether the
 *  maps were in yet; then the bench waits for the maps and the repaint the
 *  worker asks for, and hashes the maps against the ones export's
 *  synchronous ensureAIMasks() computes from a fresh decode.
 *  STEEP_AIMASK_SYNC=1 is the old behaviour (analysis inside the pass).
 *
 *  target=pan instead moves a 1:1 (100% zoom) window by 240px per step, the
 *  way a pan release does, and while that crop renders times a
 *  startProcessing() call from this thread: the GUI-thread stall a pan
 *  used to cost. It ends by deleting a second crop mid-render (closing a
 *  detail window), which must wait for the render rather than crash.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

#include <glibmm/miscutils.h>

#include "options.h"
#include "rtengine/fullqualitytwin.h"
#include "rtengine/image8.h"
#include "rtengine/imagesource.h"
#include "rtengine/previewquality.h"
#include "rtengine/procevents.h"
#include "rtengine/refreshmap.h"
#include "rtengine/procparams.h"
#include "rtengine/profilestore.h"
#include "rtengine/refreshmap.h"
#include "rtengine/rtengine.h"
#ifdef RT_AI_MASKING
#include "rtengine/aimaskcache.h"
#include "rtengine/aimaskinput.h"
#include "rtengine/aisegmentation.h"
#include "rtengine/aisubjectmodel.h"
#endif

namespace
{

using BenchClock = std::chrono::steady_clock;

int envInt(const char* name, int fallback)
{
    const char* const value = std::getenv(name);
    return value && *value ? std::atoi(value) : fallback;
}

#ifdef RT_AI_MASKING
// FNV-1a over every class map's bits and size: equal hashes, equal maps.
unsigned long long hashAIMaps()
{
    unsigned long long h = 1469598103934665603ULL;
    const auto mix = [&h](const void* data, size_t bytes) {
        const unsigned char* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < bytes; ++i) {
            h = (h ^ p[i]) * 1099511628211ULL;
        }
    };
    for (int c = 0; c < static_cast<int>(rtengine::AISegClass::TOTAL_CLASSES); ++c) {
        const rtengine::AIMaskSnapshot snap =
            rtengine::AIMaskCache::getInstance().getMaskSnapshot(static_cast<rtengine::AISegClass>(c));
        if (!snap) {
            continue;
        }
        mix(&snap.width, sizeof(snap.width));
        mix(&snap.height, sizeof(snap.height));
        for (int y = 0; y < snap.height; ++y) {
            mix((*snap.mask)[y], sizeof(float) * snap.width);
        }
    }
    return h;
}
#endif

class BenchCropListener : public rtengine::DetailedCropListener
{
public:
    int skip = 4;
    int fullW = 0;
    int fullH = 0;
    // A window inside the image; the whole frame while winW is 0.
    std::atomic<int> winX{0};
    std::atomic<int> winY{0};
    int winW = 0;
    int winH = 0;

    void setDetailedCrop(rtengine::IImage8* img, rtengine::IImage8*, const rtengine::procparams::ColorManagementParams&,
                         const rtengine::procparams::CropParams&, int, int, int, int, int) override
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (keepNext) {
            if (auto* image = dynamic_cast<rtengine::Image8*>(img)) {
                kept.assign(image->data, image->data + 3 * size_t(image->getWidth()) * image->getHeight());
                keptW = image->getWidth();
                keptH = image->getHeight();
            }
        }
        ++published;
        cond.notify_all();
    }

    void getWindow(int& cx, int& cy, int& cw, int& ch, int& s) override
    {
        cx = winW ? winX.load() : 0;
        cy = winW ? winY.load() : 0;
        cw = winW ? winW : fullW;
        ch = winW ? winH : fullH;
        s = skip;
    }

    bool wantsTrueImage() const override
    {
        return false;
    }

    unsigned long count()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return published;
    }

    bool waitBeyond(unsigned long seen, int timeoutMs)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return cond.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return published > seen; });
    }

    std::mutex mutex;
    std::condition_variable cond;
    unsigned long published = 0;
    bool keepNext = false;
    std::vector<unsigned char> kept;
    int keptW = 0;
    int keptH = 0;
};

// Waits for the pass a submission started, then for the engine to go quiet,
// so a trailing pass never bleeds into the next measurement.
double timePass(rtengine::StagedImageProcessor* ipc, BenchCropListener& crop, unsigned long seen, BenchClock::time_point start)
{
    if (!crop.waitBeyond(seen, 60000)) {
        std::fprintf(stderr, "[cliEditBench] timed out waiting for a crop\n");
        return -1.0;
    }
    const double ms = std::chrono::duration<double, std::milli>(BenchClock::now() - start).count();
    // Drain: one submission may publish more than once.
    while (crop.waitBeyond(crop.count(), 250)) {
    }
    (void)ipc;
    return ms;
}

void applyEdit(rtengine::procparams::ProcParams& p, const std::string& target, int step, int& event)
{
    const int phase = step % 8;
    const double wave = phase < 4 ? phase : 8 - phase;   // 0..4..0

    if (target == "chmixer") {
        p.chmixer.enabled = true;
        p.chmixer.red[0] = 1000 + int(20 * wave);
        event = rtengine::EvChMixer;
    } else if (target == "vibrance") {
        p.vibrance.enabled = true;
        p.vibrance.pastels = int(10 + 5 * wave);
        event = rtengine::EvVibrancePastels;
    } else if (target == "exposure") {
        p.toneCurve.expcomp = 0.05 * wave;
        event = rtengine::EvExpComp;
    } else if (target == "spotsel") {
        event = rtengine::EvLocallabSpotSelected;
    } else if (target == "wbhold") {
        // Reference for target=wb: the drag's final white balance on every
        // step, so the settled frame is that WB without a drag behind it.
        p.wb.method = "Custom";
        p.wb.temperature = 5000;
        event = rtengine::EvWBTemp;
    } else if (target == "wb") {
        // A temperature drag, as the white balance slider sends it.
        p.wb.method = "Custom";
        p.wb.temperature = 5000 + 150 * wave;
        event = rtengine::EvWBTemp;
    } else if (target == "grain") {
        // The Grain panel's events map to EFFECTS; the CLI has no event
        // mapper, so the loop sends those flags directly (event < 0).
        p.grain.enabled = true;
        p.grain.strength = 20 + 10 * wave;
        event = -EFFECTS;
    } else if (target == "gradient") {
        p.gradient.enabled = true;
        p.gradient.strength = 0.5 + 0.25 * wave;
        event = rtengine::EvGradientStrength;
    } else {
        p.locallab.spots[0].expcomp = 0.1 * wave + 0.2;
        event = rtengine::Evlocallabexpcomp;
    }
}

}

int runHeadlessEditBench(const char* file)
{
    const std::string target = std::getenv("STEEP_CLI_EDIT_BENCH_TARGET") ? std::getenv("STEEP_CLI_EDIT_BENCH_TARGET") : "locallab";
    const int steps = std::max(1, envInt("STEEP_CLI_EDIT_BENCH_STEPS", 30));

    const Glib::ustring path(file);
    const Glib::ustring ext = path.substr(path.rfind('.') + 1).lowercase();
    const bool isRaw = !(ext == "jpg" || ext == "jpeg" || ext == "tif" || ext == "tiff" || ext == "png");

    int error = 0;
    rtengine::InitialImage* ii = rtengine::InitialImage::load(path, isRaw, &error, nullptr);
    if (!ii) {
        std::fprintf(stderr, "[cliEditBench] cannot load %s\n", file);
        return 2;
    }

    rtengine::procparams::ProcParams params;
    {
        const Options& options = App::get().options();
        const Glib::ustring& name = isRaw ? options.defProfRaw : options.defProfImg;
        const rtengine::procparams::PartialProfile* def = ProfileStore::getInstance()->getProfile(name);
        if (def) {
            def->applyTo(&params);
        }
    }
    if (const char* pp3 = std::getenv("STEEP_CLI_EDIT_BENCH_PP3")) {
        params.load(pp3);
    }

    if (target == "locallab" || target == "spotsel" || target == "aimask") {
        params.locallab.enabled = true;
        if (params.locallab.spots.empty()) {
            params.locallab.spots.emplace_back();
            params.locallab.selspot = 0;
        }
        auto& spot = params.locallab.spots[0];
        spot.isvisible = true;
        spot.expexpose = true;
        spot.visiexpose = true;
        spot.expcomp = 0.2;
        if (target == "aimask") {
            spot.useAIMask = true;
            spot.aiMaskClass = envInt("STEEP_CLI_EDIT_BENCH_AICLASS", 8);
        }
    }

    const bool aiBench = target == "aimask";
#ifdef RT_AI_MASKING
    if (aiBench) {
        // As a batch render does: which subject model answers must not
        // depend on how fast its session happened to build.
        rtengine::getAISubjectEngine().waitForDeferredInit();
        std::printf("[cliEditBench] aiModels seg=%d subject=%d sync=%d\n",
                    static_cast<int>(rtengine::getAISegmentationEngine().isInitialized()),
                    static_cast<int>(rtengine::getAISubjectEngine().isInitialized()),
                    static_cast<int>(std::getenv("STEEP_AIMASK_SYNC") != nullptr));
    }
#endif

    // STEEP_CLI_EDIT_BENCH_PRECOMPUTE=1: prepare the source at full quality
    // first, as the filmstrip preloader does, so the open adopts it.
    if (envInt("STEEP_CLI_EDIT_BENCH_PRECOMPUTE", 0) && isRaw) {
        const auto t0 = BenchClock::now();
        const bool done = ii->getImageSource()->precomputeFullQuality(params);
        std::printf("[cliEditBench] precompute=%.1fms done=%d\n",
                    std::chrono::duration<double, std::milli>(BenchClock::now() - t0).count(), static_cast<int>(done));
    }

    rtengine::StagedImageProcessor* ipc = rtengine::StagedImageProcessor::create(ii);
    ipc->setPreviewScale(10);

    // STEEP_CLI_EDIT_BENCH_TWIN=1: what the editor does on open -- decode a
    // second copy and demosaic it at full quality in the background while
    // the fast first pass runs; the settled pass adopts it.
    std::thread twinThread;
    if (envInt("STEEP_CLI_EDIT_BENCH_TWIN", 0) && isRaw) {
        auto twin = std::make_shared<rtengine::FullQualityTwin>();
        ipc->setFullQualityTwin(twin);
        twinThread = std::thread([twin, path, params]() {
            const auto t0 = BenchClock::now();
            twin->setStage(rtengine::FullQualityTwin::Stage::Decoding);
            int err = 0;
#ifdef _OPENMP
            // The team widths FullQualityTwinExecutor uses (editorpanel.cc).
            // STEEP_CLI_EDIT_BENCH_TWIN_LEGACY=1: one decode thread and the
            // 4-thread preview team, as before.
            const bool legacy = envInt("STEEP_CLI_EDIT_BENCH_TWIN_LEGACY", 0) != 0;
            const int rawThreads = legacy ? rtengine::previewThreadCount(omp_get_num_procs())
                                          : rtengine::rawStageThreadCount(omp_get_num_procs());
            omp_set_num_threads(legacy ? 1 : std::max(1, rawThreads / 2));
#endif
            rtengine::InitialImage* twinImage = rtengine::InitialImage::load(path, true, &err, nullptr);
            const double decodedMs = std::chrono::duration<double, std::milli>(BenchClock::now() - t0).count();
            twin->waitForPrecomputeRelease(2500);
            twin->setStage(rtengine::FullQualityTwin::Stage::Precomputing);
#ifdef _OPENMP
            omp_set_num_threads(rawThreads);
#endif
            std::printf("[cliEditBench] twin decoded after=%.1fms\n", decodedMs);
            if (twinImage && (err || !twinImage->getImageSource()->precomputeFullQuality(params))) {
                twinImage->decreaseRef();
                twinImage = nullptr;
            }
            std::printf("[cliEditBench] twin ready=%d after=%.1fms\n", static_cast<int>(twinImage != nullptr),
                        std::chrono::duration<double, std::milli>(BenchClock::now() - t0).count());
            twin->deliver(twinImage);
        });
        twinThread.detach();
    }

    BenchCropListener cropListener;
    cropListener.skip = std::max(1, envInt("STEEP_CLI_EDIT_BENCH_SKIP", 4));
    ii->getImageSource()->getFullSize(cropListener.fullW, cropListener.fullH, 0);

    rtengine::DetailedCrop* crop = ipc->createCrop(nullptr, true);
    crop->setListener(&cropListener);

    std::printf("[cliEditBench] file=%s target=%s steps=%d skip=%d full=%dx%d\n",
                file, target.c_str(), steps, cropListener.skip, cropListener.fullW, cropListener.fullH);

    // Open: same request the editor makes, then the settled refinement.
    auto start = BenchClock::now();
    unsigned long seen = cropListener.count();
    *ipc->beginUpdateParams() = params;
    ipc->endUpdateParams(FIRST);
    const double openMs = timePass(ipc, cropListener, seen, start);
    seen = cropListener.count();
    {
        std::lock_guard<std::mutex> lock(cropListener.mutex);
        cropListener.keepNext = true;
    }
    start = BenchClock::now();
    ipc->startProcessing(M_HIGHQUAL | M_MONITOR);
    const double openSettleMs = timePass(ipc, cropListener, seen, start);
    std::printf("[cliEditBench] open=%.1fms openSettle=%.1fms\n", openMs, openSettleMs);
    if (const char* out = std::getenv("STEEP_CLI_EDIT_BENCH_OPEN_OUT")) {
        // The settled open frame, to compare the precomputed path bit for bit.
        std::lock_guard<std::mutex> lock(cropListener.mutex);
        if (std::FILE* f = std::fopen(out, "wb")) {
            std::fprintf(f, "P6\n%d %d\n255\n", cropListener.keptW, cropListener.keptH);
            std::fwrite(cropListener.kept.data(), 1, cropListener.kept.size(), f);
            std::fclose(f);
        }
        cropListener.keepNext = false;
    }

    if (target == "pan") {
        cropListener.skip = 1;
        cropListener.winW = std::min(1920, cropListener.fullW);
        cropListener.winH = std::min(1080, cropListener.fullH);
        const int cx = (cropListener.fullW - cropListener.winW) / 2;
        cropListener.winX = cx;
        cropListener.winY = (cropListener.fullH - cropListener.winH) / 2;

        // Zoom to 100% first, as CropHandler::setZoom does.
        seen = cropListener.count();
        start = BenchClock::now();
        if (crop->tryUpdate()) {
            crop->startUpdate();
        }
        // The first 100% render is the long one (seconds with Fattal):
        // queue preview work from this thread in the middle of it.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const auto zoomStallStart = BenchClock::now();
        ipc->startProcessing(0);
        const double zoomStallMs =
            std::chrono::duration<double, std::milli>(BenchClock::now() - zoomStallStart).count();
        const double zoomMs = timePass(ipc, cropListener, seen, start);
        std::printf("[cliEditBench] target=pan zoom100=%.1fms guiStallDuringZoom=%.2fms\n", zoomMs, zoomStallMs);
        std::fflush(stdout);

        std::vector<double> renders;
        std::vector<double> stalls;
        for (int step = 1; step <= steps; ++step) {
            cropListener.winX = cx + ((step & 1) ? 240 : 0);
            seen = cropListener.count();
            start = BenchClock::now();
            if (crop->tryUpdate()) {
                crop->startUpdate();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            const auto stallStart = BenchClock::now();
            ipc->startProcessing(0);
            stalls.push_back(std::chrono::duration<double, std::milli>(BenchClock::now() - stallStart).count());
            const double ms = timePass(ipc, cropListener, seen, start);
            if (ms >= 0) {
                renders.push_back(ms);
            }
        }

        // Fit and back: the round trip a whole-frame cache (dehaze/Fattal)
        // must survive.
        auto renderAt = [&](int skip, int w, int h, int x, int y) {
            cropListener.skip = skip;
            cropListener.winW = w;
            cropListener.winH = h;
            cropListener.winX = x;
            cropListener.winY = y;
            const unsigned long before = cropListener.count();
            const auto t = BenchClock::now();
            if (crop->tryUpdate()) {
                crop->startUpdate();
            }
            return timePass(ipc, cropListener, before, t);
        };
        const double fitMs = renderAt(4, cropListener.fullW, cropListener.fullH, 0, 0);
        const double backMs = renderAt(1, std::min(1920, cropListener.fullW), std::min(1080, cropListener.fullH),
                                       cx, (cropListener.fullH - std::min(1080, cropListener.fullH)) / 2);
        std::printf("[cliEditBench] target=pan fit=%.1fms back100=%.1fms\n", fitMs, backMs);

        // Close a detail window while its crop renders.
        rtengine::DetailedCrop* extra = ipc->createCrop(nullptr, true);
        extra->setListener(&cropListener);
        if (extra->tryUpdate()) {
            extra->startUpdate();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        const auto closeStart = BenchClock::now();
        delete extra;
        const double closeMs = std::chrono::duration<double, std::milli>(BenchClock::now() - closeStart).count();
        while (cropListener.waitBeyond(cropListener.count(), 250)) {
        }

        std::sort(renders.begin(), renders.end());
        std::sort(stalls.begin(), stalls.end());
        auto at = [](const std::vector<double>& v, double q) {
            return v.empty() ? -1.0 : v[std::min(v.size() - 1, size_t(q * v.size()))];
        };
        std::printf("[cliEditBench] target=pan window=%dx%d zoom100=%.1fms n=%zu render p50=%.1fms p90=%.1fms "
                    "guiStall p50=%.2fms max=%.2fms closeMidRender=%.1fms\n",
                    cropListener.winW, cropListener.winH, zoomMs, renders.size(), at(renders, 0.5), at(renders, 0.9),
                    at(stalls, 0.5), stalls.empty() ? -1.0 : stalls.back(), closeMs);
        std::fflush(stdout);
        crop->setListener(nullptr);
        crop->destroy();
        rtengine::StagedImageProcessor::destroy(ipc);
        ii->decreaseRef();
        return 0;
    }

    const std::string imageId = path.raw();
    const auto masksReady = [&]() {
#ifdef RT_AI_MASKING
        return rtengine::AIMaskCache::getInstance().hasCachedMasks(imageId);
#else
        return false;
#endif
    };
    const auto benchStart = start;

#ifdef RT_AI_MASKING
    // STEEP_CLI_EDIT_BENCH_AISWITCH=<second file>: move on while the first
    // photo's analysis is still running, as a filmstrip step does -- close
    // this processor (its pending repaint must be dropped, not called on a
    // dead coordinator) and open the second photo, whose maps must replace
    // the abandoned job's and repaint it.
    if (const char* second = std::getenv("STEEP_CLI_EDIT_BENCH_AISWITCH")) {
        if (!aiBench || masksReady()) {
            std::printf("[cliEditBench] aiswitch needs target=aimask and a cold first analysis\n");
            std::_Exit(2);
        }
        crop->setListener(nullptr);
        crop->destroy();
        rtengine::StagedImageProcessor::destroy(ipc);
        std::fprintf(stderr, "[cliEditBench] aiswitch first processor destroyed\n");
        // ii is left alone: releasing it here segfaults with the AI mask
        // worker idle too (STEEP_AIMASK_SYNC=1) -- this bench's teardown
        // problem, the reason the other paths leave through _Exit.

        const Glib::ustring path2(second);
        rtengine::InitialImage* ii2 = rtengine::InitialImage::load(path2, isRaw, &error, nullptr);
        if (!ii2) {
            std::printf("[cliEditBench] cannot load %s\n", second);
            std::_Exit(2);
        }
        rtengine::StagedImageProcessor* ipc2 = rtengine::StagedImageProcessor::create(ii2);
        ipc2->setPreviewScale(10);
        BenchCropListener listener2;
        listener2.skip = cropListener.skip;
        ii2->getImageSource()->getFullSize(listener2.fullW, listener2.fullH, 0);
        rtengine::DetailedCrop* crop2 = ipc2->createCrop(nullptr, true);
        crop2->setListener(&listener2);

        const auto switchStart = BenchClock::now();
        unsigned long seen2 = listener2.count();
        *ipc2->beginUpdateParams() = params;
        ipc2->endUpdateParams(FIRST);
        const double open2 = timePass(ipc2, listener2, seen2, switchStart);
        std::fprintf(stderr, "[cliEditBench] aiswitch second open %.1fms\n", open2);
        const unsigned long quiet = listener2.count();
        const std::string id2 = path2.raw();
        while (!rtengine::AIMaskCache::getInstance().hasCachedMasks(id2)
                && BenchClock::now() - switchStart < std::chrono::seconds(60)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        const double ready2 = std::chrono::duration<double, std::milli>(BenchClock::now() - switchStart).count();
        const bool repainted = listener2.waitBeyond(quiet, 3000);
        std::printf("[cliEditBench] aiswitch open2=%.1fms masks2=%d readyBy=%.0fms repaint=%d firstStillActive=%d\n",
                    open2, static_cast<int>(rtengine::AIMaskCache::getInstance().hasCachedMasks(id2)), ready2,
                    static_cast<int>(repainted), static_cast<int>(masksReady()));
        std::fflush(stdout);
        std::_Exit(0);
    }
#endif

    std::vector<double> passes;
    std::vector<double> pendingPasses;
    for (int step = 1; step <= steps; ++step) {
        seen = cropListener.count();
        start = BenchClock::now();
        const bool readyBefore = masksReady();
        rtengine::procparams::ProcParams* p = ipc->beginUpdateParams();
        int event = rtengine::EvExpComp;
        applyEdit(*p, target, step, event);
        if (event < 0) {
            ipc->endUpdateParams(-event);   // raw refresh flags
        } else {
            ipc->endUpdateParams(rtengine::ProcEvent(event));
        }
        const double ms = timePass(ipc, cropListener, seen, start);
        if (ms >= 0) {
            passes.push_back(ms);
            if (aiBench && !readyBefore) {
                pendingPasses.push_back(ms);
            }
        }
        if (aiBench) {
            std::printf("[cliEditBench] step=%d ms=%.1f masksBefore=%d masksAfter=%d\n",
                        step, ms, static_cast<int>(readyBefore), static_cast<int>(masksReady()));
        }
    }

    if (aiBench) {
        // The drain in timePass leaves the engine quiet: the only publish
        // from here on is the repaint the AI mask worker asks for.
        const unsigned long quiet = cropListener.count();
        const bool wasReady = masksReady();
        const auto waitStart = BenchClock::now();
        while (!masksReady() && BenchClock::now() - waitStart < std::chrono::seconds(60)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        const double readyMs = std::chrono::duration<double, std::milli>(BenchClock::now() - benchStart).count();
        const bool repainted = wasReady || cropListener.waitBeyond(quiet, 3000);
        while (cropListener.waitBeyond(cropListener.count(), 250)) {
        }
        double pendingMax = 0.0;
        for (double ms : pendingPasses) {
            pendingMax = std::max(pendingMax, ms);
        }
        std::printf("[cliEditBench] aimask passesWhilePending=%zu maxWhilePending=%.1fms "
                    "masks=%d readyBy=%.0fms(from open) repaintAfterReady=%d\n",
                    pendingPasses.size(), pendingMax, static_cast<int>(masksReady()), readyMs,
                    static_cast<int>(repainted));
    }

    // Settle on a fixed final value so two builds compare the same edit.
    {
        rtengine::procparams::ProcParams* p = ipc->beginUpdateParams();
        int event = rtengine::EvExpComp;
        applyEdit(*p, target, 2, event);
        seen = cropListener.count();
        ipc->endUpdateParams(rtengine::ProcEvent(event));
        timePass(ipc, cropListener, seen, BenchClock::now());
    }

    seen = cropListener.count();
    {
        std::lock_guard<std::mutex> lock(cropListener.mutex);
        cropListener.keepNext = true;
    }
    start = BenchClock::now();
    ipc->startProcessing(M_HIGHQUAL | M_MONITOR);
    const double settleMs = timePass(ipc, cropListener, seen, start);

    std::sort(passes.begin(), passes.end());
    auto pct = [&](double q) {
        return passes.empty() ? -1.0 : passes[std::min(passes.size() - 1, size_t(q * passes.size()))];
    };
    std::printf("[cliEditBench] target=%s n=%zu p10=%.1fms p50=%.1fms p90=%.1fms settle=%.1fms\n",
                target.c_str(), passes.size(), pct(0.1), pct(0.5), pct(0.9), settleMs);

#ifdef RT_AI_MASKING
    if (aiBench && masksReady()) {
        // The editor's maps against export's: simpleprocess decodes the file
        // again and calls ensureAIMasks() on its own source.
        const unsigned long long editorHash = hashAIMaps();
        int err = 0;
        rtengine::InitialImage* exportImage = rtengine::InitialImage::load(path, isRaw, &err, nullptr);
        if (exportImage && !err) {
            rtengine::ImageSource* src = exportImage->getImageSource();
            if (isRaw) {
                src->precomputeFullQuality(params);
            }
            rtengine::AIMaskCache::getInstance().invalidateAll();
            const bool ok = rtengine::ensureAIMasks(src, params, src->getWB(), true);
            const unsigned long long exportHash = hashAIMaps();
            std::printf("[cliEditBench] aimask editorMaps=%016llx exportMaps=%016llx exportOk=%d identical=%d\n",
                        editorHash, exportHash, static_cast<int>(ok), static_cast<int>(editorHash == exportHash));
            // Tells a different input from a nondeterministic model: the
            // same export path twice, and the synchronous path on the
            // editor's own (now idle) source.
            rtengine::AIMaskCache::getInstance().invalidateAll();
            rtengine::ensureAIMasks(src, params, src->getWB(), true);
            const unsigned long long exportAgain = hashAIMaps();
            rtengine::AIMaskCache::getInstance().invalidateAll();
            rtengine::ImageSource* editorSource = ii->getImageSource();
            rtengine::ensureAIMasks(editorSource, params, editorSource->getWB(), true);
            const unsigned long long editorSync = hashAIMaps();
            std::printf("[cliEditBench] aimask exportAgain=%016llx (repeatable=%d) editorSourceSync=%016llx (matchesEditor=%d matchesExport=%d)\n",
                        exportAgain, static_cast<int>(exportAgain == exportHash), editorSync,
                        static_cast<int>(editorSync == editorHash), static_cast<int>(editorSync == exportHash));
        }
    }
#endif

    if (const char* out = std::getenv("STEEP_CLI_EDIT_BENCH_OUT")) {
        std::lock_guard<std::mutex> lock(cropListener.mutex);
        const char* const tag = std::getenv("STEEP_CLI_EDIT_BENCH_TAG");
        const std::string name = Glib::build_filename(out, "settled-" + target + (tag ? std::string("-") + tag : std::string()) + ".ppm");
        if (std::FILE* f = std::fopen(name.c_str(), "wb")) {
            std::fprintf(f, "P6\n%d %d\n255\n", cropListener.keptW, cropListener.keptH);
            std::fwrite(cropListener.kept.data(), 1, cropListener.kept.size(), f);
            std::fclose(f);
            std::printf("[cliEditBench] wrote %s\n", name.c_str());
        }
    }

    // A measuring tool: leave without tearing the engine down, which crashed
    // at exit here (the processor's worker tasks outlive this scope).
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(0);
}

/*
 *  AI mask dump for tools/aimask_eval.py: STEEP_AIMASK_EVAL=<list file, one
 *  image path per line> STEEP_AIMASK_EVAL_OUT=<dir>. Every photo goes through
 *  the same input path the editor uses (ensureAIMasks), then each class is
 *  written twice as 8-bit PNG: the raw class map to <out>/raw/<stem>/<c>.png
 *  and the mask a new AI spot with default settings would apply to
 *  <out>/prepared/<stem>/<c>.png.
 *  STEEP_AIMASK_EVAL_DIM=<px> overrides the input's long side (the editor's
 *  old preview-sized input was ~600 px for a 26 MP photo).
 */
#ifdef RT_AI_MASKING
#include <fstream>
#include "rtengine/aimaskcache.h"
#include "rtengine/aimaskinput.h"
#include "rtengine/aisegmentation.h"
#include "rtengine/aisubjectmodel.h"
#include "rtengine/colortemp.h"
#include "rtengine/iimage.h"
#include "rtengine/imagefloat.h"
#include "rtengine/maskpaint.h"
#endif

int runAIMaskEval(const char* listFile)
{
#ifndef RT_AI_MASKING
    (void)listFile;
    std::fprintf(stderr, "[aiMaskEval] built without AI masking\n");
    return 2;
#else
    const char* outEnv = std::getenv("STEEP_AIMASK_EVAL_OUT");
    if (!outEnv || !rtengine::getAISegmentationEngine().isInitialized()) {
        std::fprintf(stderr, "[aiMaskEval] need STEEP_AIMASK_EVAL_OUT and an initialised segmentation model\n");
        return 2;
    }
    // Same as a batch render: which subject model answers must not depend on
    // how fast its session happened to build.
    rtengine::getAISubjectEngine().waitForDeferredInit();
    const std::string outDir(outEnv);
    const int dim = std::max(64, envInt("STEEP_AIMASK_EVAL_DIM", rtengine::AI_MASK_INPUT_DIMENSION));

    std::ifstream list(listFile);
    std::string line;
    int done = 0;
    while (std::getline(list, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        const Glib::ustring path(line);
        const Glib::ustring ext = path.substr(path.rfind('.') + 1).lowercase();
        const bool isRaw = !(ext == "jpg" || ext == "jpeg" || ext == "tif" || ext == "tiff" || ext == "png");
        int error = 0;
        rtengine::InitialImage* ii = rtengine::InitialImage::load(path, isRaw, &error, nullptr);
        if (!ii || error) {
            std::fprintf(stderr, "[aiMaskEval] cannot load %s\n", line.c_str());
            continue;
        }
        rtengine::ImageSource* src = ii->getImageSource();

        rtengine::procparams::ProcParams params;
        {
            const Options& options = App::get().options();
            const rtengine::procparams::PartialProfile* def =
                ProfileStore::getInstance()->getProfile(isRaw ? options.defProfRaw : options.defProfImg);
            if (def) {
                def->applyTo(&params);
            }
        }
        if (isRaw) {
            src->precomputeFullQuality(params);
        }

        const auto t0 = BenchClock::now();
        const std::unique_ptr<rtengine::Imagefloat> input =
            rtengine::buildAIMaskInput(src, params, src->getWB(), dim, true);
        if (!input) {
            ii->decreaseRef();
            continue;
        }
        int fullW = 0;
        int fullH = 0;
        src->getFullSize(fullW, fullH, rtengine::getCoarseBitMask(params.coarse));
        const std::string imageId = path.raw();
        rtengine::AIMaskCache& cache = rtengine::AIMaskCache::getInstance();
        cache.computeMasks(imageId, input->r.ptrs, input->g.ptrs, input->b.ptrs,
                           input->getWidth(), input->getHeight(), fullW, fullH,
                           params.icm.workingProfile.raw(), true, rtengine::aiMaskGeometryKey(params));
        const double ms = std::chrono::duration<double, std::milli>(BenchClock::now() - t0).count();

        const std::string base = Glib::path_get_basename(line);
        const std::string stem = base.substr(0, base.rfind('.'));
        const rtengine::MaskPaint noPaint;
        for (const char* kind : {"raw", "prepared"}) {
            const std::string dir = Glib::build_filename(outDir, kind, stem);
            g_mkdir_with_parents(dir.c_str(), 0755);
            for (int c = 1; c < static_cast<int>(rtengine::AISegClass::TOTAL_CLASSES); ++c) {
                const auto cls = static_cast<rtengine::AISegClass>(c);
                const rtengine::AIMaskSnapshot snap = std::strcmp(kind, "raw") == 0
                    ? cache.getMaskSnapshot(cls)
                    : cache.getPreparedMask(cls, 0.3f, 35.f, 0.f, 18.f, false, 8, 0.01f, noPaint, true);
                if (!snap) {
                    continue;
                }
                rtengine::Image8 png(snap.width, snap.height);
                for (int y = 0; y < snap.height; ++y) {
                    for (int x = 0; x < snap.width; ++x) {
                        const unsigned char v = static_cast<unsigned char>(
                            std::max(0.f, std::min(1.f, (*snap.mask)[y][x])) * 255.f + 0.5f);
                        png.r(y, x) = png.g(y, x) = png.b(y, x) = v;
                    }
                }
                png.saveAsPNG(Glib::build_filename(dir, std::to_string(c) + ".png"), 8);
            }
        }
        cache.invalidate(imageId);
        ii->decreaseRef();
        ++done;
        std::printf("[aiMaskEval] %d %s input=%dx%d masks=%.0fms\n", done, stem.c_str(),
                    input->getWidth(), input->getHeight(), ms);
        std::fflush(stdout);
    }
    std::fflush(stdout);
    std::_Exit(0);
#endif
}

/*
 *  Memory loop: STEEP_CLI_MEM_LOOP=<list file, one image path per line>.
 *  Opens and closes every photo the way the editor does (decode, processor,
 *  first pass, settled pass, destroy) with its sidecar profile when there is
 *  one, and prints the process's private bytes after each, so growth per
 *  photo shows without a window.
 *    STEEP_CLI_MEM_LOOP_MODE=open (default) | thumb | both
 *      thumb = what a preview strip does: a full thumbnail from the RAW,
 *      rendered STEEP_CLI_MEM_LOOP_FRAMES (6) times.
 *    STEEP_CLI_MEM_LOOP_ROUNDS=<n>  go through the list n times (default 1)
 */
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif
#include <fstream>
#include "rtengine/rtthumbnail.h"

namespace
{

double privateGiB()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
        return counters.PrivateUsage / (1024.0 * 1024.0 * 1024.0);
    }
#endif
    return 0.0;
}

}

int runHeadlessMemLoop(const char* listFile)
{
    std::vector<std::string> files;
    {
        std::ifstream in(listFile);
        std::string line;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
                line.pop_back();
            }
            if (!line.empty()) {
                files.push_back(line);
            }
        }
    }
    const std::string mode = std::getenv("STEEP_CLI_MEM_LOOP_MODE") ? std::getenv("STEEP_CLI_MEM_LOOP_MODE") : "open";
    const int rounds = std::max(1, envInt("STEEP_CLI_MEM_LOOP_ROUNDS", 1));
    const int frames = std::max(1, envInt("STEEP_CLI_MEM_LOOP_FRAMES", 6));
    std::fprintf(stderr, "[memLoop] files=%d mode=%s rounds=%d start=%.2fGiB\n",
                static_cast<int>(files.size()), mode.c_str(), rounds, privateGiB());

    int n = 0;
    for (int round = 0; round < rounds; ++round) {
        for (const std::string& file : files) {
            const Glib::ustring path(file);
            const Glib::ustring ext = path.substr(path.rfind('.') + 1).lowercase();
            const bool isRaw = !(ext == "jpg" || ext == "jpeg" || ext == "tif" || ext == "tiff" || ext == "png");

            rtengine::procparams::ProcParams params;
            {
                const Options& options = App::get().options();
                const Glib::ustring& name = isRaw ? options.defProfRaw : options.defProfImg;
                if (const rtengine::procparams::PartialProfile* def = ProfileStore::getInstance()->getProfile(name)) {
                    def->applyTo(&params);
                }
            }
            const Glib::ustring sidecar = path + ".pp3";
            if (Glib::file_test(sidecar, Glib::FILE_TEST_EXISTS)) {
                params.load(sidecar);
            }

            const auto t0 = BenchClock::now();
            std::fprintf(stderr, "[memLoop] begin %s\n", file.c_str());

            if (mode == "open" || mode == "both") {
                int error = 0;
                rtengine::InitialImage* ii = rtengine::InitialImage::load(path, isRaw, &error, nullptr);
                if (!ii) {
                    std::fprintf(stderr, "[memLoop] cannot load %s\n", file.c_str());
                    continue;
                }
                rtengine::StagedImageProcessor* ipc = rtengine::StagedImageProcessor::create(ii);
                ipc->setPreviewScale(10);
                BenchCropListener cropListener;
                cropListener.skip = 4;
                ii->getImageSource()->getFullSize(cropListener.fullW, cropListener.fullH, 0);
                rtengine::DetailedCrop* crop = ipc->createCrop(nullptr, true);
                crop->setListener(&cropListener);

                unsigned long seen = cropListener.count();
                *ipc->beginUpdateParams() = params;
                ipc->endUpdateParams(FIRST);
                timePass(ipc, cropListener, seen, BenchClock::now());
                seen = cropListener.count();
                ipc->startProcessing(M_HIGHQUAL | M_MONITOR);
                timePass(ipc, cropListener, seen, BenchClock::now());

                // The editor's close order (EditorPanel::close): stop, drop the
                // crop's listener, destroy the processor. It owns its crops
                // and the image's load reference.
                ipc->stopProcessing();
                crop->setListener(nullptr);
                rtengine::StagedImageProcessor::destroy(ipc);
            }

            if (mode == "thumb" || mode == "both") {
                rtengine::eSensorType sensorType = rtengine::ST_NONE;
                int tw = -1, th = 720;
                const auto ms = [](BenchClock::time_point a) {
                    return std::chrono::duration<double, std::milli>(BenchClock::now() - a).count();
                };
                if (isRaw) {
                    // What a cold folder load pays per RAW: the embedded preview.
                    rtengine::eSensorType quickSensor = rtengine::ST_NONE;
                    int qw = -1, qh = 720;
                    const auto tq = BenchClock::now();
                    delete rtengine::Thumbnail::loadQuickFromRaw(path, quickSensor, qw, qh, 1, true);
                    std::fprintf(stderr, "[memLoop] thumb quick=%.1fms\n", ms(tq));
                }
                const auto tl = BenchClock::now();
                rtengine::Thumbnail* tpp = isRaw
                    ? rtengine::Thumbnail::loadFromRaw(path, sensorType, tw, th, 1, params.wb.equal, params.wb.observer, true, &params.raw)
                    : rtengine::Thumbnail::loadFromImage(path, tw, th, 1, params.wb.equal, params.wb.observer);
                if (tpp) {
                    const double loadMs = ms(tl);
                    const auto tm = BenchClock::now();
                    std::unique_ptr<rtengine::FramesMetaData> meta(rtengine::FramesMetaData::fromFile(path));
                    const double metaMs = ms(tm);
                    const int rheight = std::max(16, envInt("STEEP_CLI_MEM_LOOP_THUMB_H", 60));
                    std::vector<double> renders;
                    for (int f = 0; f < frames; ++f) {
                        rtengine::procparams::ProcParams pp = params;
                        pp.toneCurve.expcomp += 0.2 * f;
                        double scale = 1.0;
                        const auto tr = BenchClock::now();
                        delete tpp->processImage(pp, sensorType, rheight, rtengine::TI_Bilinear, meta.get(), scale);
                        renders.push_back(ms(tr));
                    }
                    std::sort(renders.begin(), renders.end());
                    std::fprintf(stderr, "[memLoop] thumb full=%.1fms meta=%.1fms render min/p50=%.1f/%.1fms h=%d\n",
                                 loadMs, metaMs, renders.front(), renders[renders.size() / 2], rheight);
                    delete tpp;
                }
            }

            ++n;
            std::fprintf(stderr, "[memLoop] #%d %.1fs private=%.2fGiB %s\n", n,
                        std::chrono::duration<double>(BenchClock::now() - t0).count(), privateGiB(), file.c_str());
            std::fflush(stdout);
        }
    }

    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(0);
}
