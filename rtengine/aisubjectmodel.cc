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
#ifdef RT_AI_MASKING

#include "aisubjectmodel.h"
#include "aiidlereaper.h"

#include "onnxruntime_compat.h"

#ifdef RT_AI_MASKING_DIRECTML
#include <dml_provider_factory.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace rtengine
{

namespace
{

// The exported network takes a fixed 320x320 and emits seven side maps; only
// the first, the fused one, is wanted, and the sigmoid is already inside it.
constexpr int SUBJECT_INPUT = 320;

constexpr float MEAN_R = 0.485f;
constexpr float MEAN_G = 0.456f;
constexpr float MEAN_B = 0.406f;
constexpr float STD_R = 0.229f;
constexpr float STD_G = 0.224f;
constexpr float STD_B = 0.225f;

float linearToSRGB(float v)
{
    v = std::max(0.f, std::min(1.f, v));
    return v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f;
}

} // namespace

struct AISubjectEngine::Impl {
    const OrtApi* api = nullptr;
    OrtEnv* env = nullptr;
    OrtSession* session = nullptr;
    OrtSessionOptions* sessionOptions = nullptr;
    OrtMemoryInfo* memoryInfo = nullptr;
    std::atomic<bool> initialized{false};
    bool usingDirectML = false;
    std::string inputName;
    std::string outputName;
    // Square input side read from the model: 320 for U^2-Net, 1024 for
    // BiRefNet. They also differ in preparation, see saliency().
    int side = SUBJECT_INPUT;
    std::string modelPath;
    // Inference and the idle release take turns on the session.
    std::mutex runMutex;
    std::atomic<long long> lastUse{0};

    // Caller holds runMutex.
    bool loadSessionLocked()
    {
        if (session) {
            return true;
        }
        if (!api || !env || !sessionOptions || modelPath.empty()) {
            return false;
        }
#ifdef _WIN32
        const int wlen = MultiByteToWideChar(CP_UTF8, 0, modelPath.c_str(), -1, nullptr, 0);
        std::vector<wchar_t> wpath(wlen);
        MultiByteToWideChar(CP_UTF8, 0, modelPath.c_str(), -1, wpath.data(), wlen);
        OrtStatus* status = api->CreateSession(env, wpath.data(), sessionOptions, &session);
#else
        OrtStatus* status = api->CreateSession(env, modelPath.c_str(), sessionOptions, &session);
#endif
        if (status) {
            std::fprintf(stderr, "AI Subject: failed to reload model: %s\n", api->GetErrorMessage(status));
            api->ReleaseStatus(status);
            session = nullptr;
            return false;
        }
        std::fprintf(stderr, "AI Subject: reloaded %s\n", modelPath.c_str());
        return true;
    }

    void releaseIfIdle(bool force)
    {
        std::unique_lock<std::mutex> lock(runMutex, std::try_to_lock);
        if (!lock.owns_lock() || !session || (!force && AIIdleReaper::nowMs() - lastUse.load() < AIIdleReaper::kIdleMs)) {
            return;
        }
        api->ReleaseSession(session);
        session = nullptr;
        if (force) {
            std::fprintf(stderr, "AI Subject: model unloaded to free memory\n");
        } else {
            std::fprintf(stderr, "AI Subject: model unloaded after %lld s idle\n", AIIdleReaper::kIdleMs / 1000);
        }
    }
    int reaperId = -1;
    // A load that failed for want of memory, and when (see retryLoad()).
    std::string retryPath;
    long long failedAtMs = 0;
    std::mutex initMutex;
    // A deferred load still in flight, and its end.
    bool loading = false;
    std::mutex loadMutex;
    std::condition_variable loaded;

    ~Impl()
    {
        if (api) {
            if (session) api->ReleaseSession(session);
            if (sessionOptions) api->ReleaseSessionOptions(sessionOptions);
            if (memoryInfo) api->ReleaseMemoryInfo(memoryInfo);
            if (env) api->ReleaseEnv(env);
        }
    }
};

AISubjectEngine::AISubjectEngine() : pImpl(new Impl())
{
}

AISubjectEngine::~AISubjectEngine()
{
    delete pImpl;
}

bool AISubjectEngine::init(const std::string& modelPath)
{
    std::lock_guard<std::mutex> lock(pImpl->initMutex);

    if (pImpl->initialized) {
        return true;
    }

    pImpl->api = OrtGetApiBase()->GetApi(ORT_API_VERSION);

    if (!pImpl->api) {
        return false;
    }

    OrtStatus* status = nullptr;

    // Set up once; a load retried after running out of memory only
    // builds the session again.
    if (!pImpl->sessionOptions) {
        status = pImpl->api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "RawTherapee_Subject", &pImpl->env);

        if (status) {
            pImpl->api->ReleaseStatus(status);
            return false;
        }

        status = pImpl->api->CreateSessionOptions(&pImpl->sessionOptions);

        if (status) {
            pImpl->api->ReleaseStatus(status);
            return false;
        }

        pImpl->api->SetSessionGraphOptimizationLevel(pImpl->sessionOptions, ORT_ENABLE_ALL);

#ifdef RT_AI_MASKING_DIRECTML
        pImpl->api->DisableMemPattern(pImpl->sessionOptions);
        pImpl->api->DisableCpuMemArena(pImpl->sessionOptions);
        pImpl->api->SetSessionExecutionMode(pImpl->sessionOptions, ORT_SEQUENTIAL);
        status = OrtSessionOptionsAppendExecutionProvider_DML(pImpl->sessionOptions, 0);

        if (status) {
            pImpl->api->ReleaseStatus(status);
        } else {
            pImpl->usingDirectML = true;
        }
#endif

        if (!pImpl->usingDirectML) {
            const unsigned int hardwareThreads = std::thread::hardware_concurrency();
            pImpl->api->SetIntraOpNumThreads(pImpl->sessionOptions, static_cast<int>(
                std::max(1u, std::min(4u, hardwareThreads > 1 ? hardwareThreads / 2 : 1u))));
        }
    }

#ifdef _WIN32
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, modelPath.c_str(), -1, nullptr, 0);
    std::vector<wchar_t> wpath(wlen);
    MultiByteToWideChar(CP_UTF8, 0, modelPath.c_str(), -1, wpath.data(), wlen);
    status = pImpl->api->CreateSession(pImpl->env, wpath.data(), pImpl->sessionOptions, &pImpl->session);
#else
    status = pImpl->api->CreateSession(pImpl->env, modelPath.c_str(), pImpl->sessionOptions, &pImpl->session);
#endif

    if (status) {
        const char* message = pImpl->api->GetErrorMessage(status);
        std::fprintf(stderr, "AI Subject: failed to load model: %s\n", message);
        // Out of memory is usually someone else's, for a while: retryLoad()
        // tries again later instead of leaving the model off for the session.
        if (AIIdleReaper::outOfMemory(message)) {
            pImpl->retryPath = modelPath;
            pImpl->failedAtMs = AIIdleReaper::nowMs();
        }
        pImpl->session = nullptr;
        pImpl->api->ReleaseStatus(status);
        return false;
    }

    status = pImpl->api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &pImpl->memoryInfo);

    if (status) {
        pImpl->api->ReleaseStatus(status);
        return false;
    }

    // The released graph names its tensors by number, not "input"/"output", so
    // they are read from the model rather than assumed.
    OrtAllocator* allocator = nullptr;
    pImpl->api->GetAllocatorWithDefaultOptions(&allocator);

    char* name = nullptr;

    if (!pImpl->api->SessionGetInputName(pImpl->session, 0, allocator, &name)) {
        pImpl->inputName = name;
        allocator->Free(allocator, name);
    }

    if (!pImpl->api->SessionGetOutputName(pImpl->session, 0, allocator, &name)) {
        pImpl->outputName = name;
        allocator->Free(allocator, name);
    }

    if (pImpl->inputName.empty() || pImpl->outputName.empty()) {
        std::fprintf(stderr, "AI Subject: model does not name its tensors\n");
        return false;
    }

    // NCHW with a fixed square H x W.
    const auto ok = [this](OrtStatus* st) {
        if (st) {
            pImpl->api->ReleaseStatus(st);
            return false;
        }
        return true;
    };
    OrtTypeInfo* typeInfo = nullptr;
    if (ok(pImpl->api->SessionGetInputTypeInfo(pImpl->session, 0, &typeInfo)) && typeInfo) {
        const OrtTensorTypeAndShapeInfo* tensorInfo = nullptr;
        size_t rank = 0;
        if (ok(pImpl->api->CastTypeInfoToTensorInfo(typeInfo, &tensorInfo)) && tensorInfo
                && ok(pImpl->api->GetDimensionsCount(tensorInfo, &rank)) && rank == 4) {
            int64_t dims[4] = {0, 0, 0, 0};
            if (ok(pImpl->api->GetDimensions(tensorInfo, dims, 4)) && dims[2] > 0 && dims[2] == dims[3]) {
                pImpl->side = static_cast<int>(dims[2]);
            }
        }
        pImpl->api->ReleaseTypeInfo(typeInfo);
    }

    pImpl->modelPath = modelPath;
    pImpl->retryPath.clear();
    pImpl->lastUse = AIIdleReaper::nowMs();
    pImpl->initialized = true;
    {
        Impl* const impl = pImpl;
        pImpl->reaperId = AIIdleReaper::watch([impl](bool force) { impl->releaseIfIdle(force); });
    }
    std::fprintf(stderr, "AI Subject: initialised from %s (%s)\n", modelPath.c_str(),
                 pImpl->usingDirectML ? "DirectML" : "CPU");
    return true;
}

void AISubjectEngine::initDeferred(const std::string& modelPath)
{
    // Building a session for a 170 MB model takes seconds; startup should not
    // wait for a model that is only wanted once a subject mask is asked for.
    // Anything segmented while this is still loading holds the
    // composed-from-classes subject. The caches record whether this model was
    // available when they computed, so those entries stop matching the moment
    // it is -- see cachedSubjectModel_ in AIMaskCache and the key in
    // PartnerMaskStore. Clearing them from here instead would race whatever
    // pass is running.
    {
        std::lock_guard<std::mutex> lock(pImpl->loadMutex);

        if (pImpl->initialized || pImpl->loading) {
            return;
        }

        pImpl->loading = true;
    }

    std::thread([this, modelPath]() {
        this->init(modelPath);

        {
            std::lock_guard<std::mutex> lock(pImpl->loadMutex);
            pImpl->loading = false;
        }

        pImpl->loaded.notify_all();
    }).detach();
}

bool AISubjectEngine::retryLoad()
{
    std::string path;
    {
        std::lock_guard<std::mutex> lock(pImpl->initMutex);
        if (pImpl->initialized) {
            return true;
        }
        constexpr long long kRetryMs = 30000;
        if (pImpl->retryPath.empty() || AIIdleReaper::nowMs() - pImpl->failedAtMs < kRetryMs) {
            return false;
        }
        path = pImpl->retryPath;
        pImpl->failedAtMs = AIIdleReaper::nowMs();
    }
    std::fprintf(stderr, "AI Subject: retrying the load that ran out of memory\n");
    AIIdleReaper::releaseNow();
    return init(path);
}

void AISubjectEngine::waitForDeferredInit() const
{
    std::unique_lock<std::mutex> lock(pImpl->loadMutex);
    pImpl->loaded.wait(lock, [this]() { return !pImpl->loading; });
}

std::string AISubjectEngine::modelPath() const
{
    return pImpl->initialized ? pImpl->modelPath : std::string();
}

bool AISubjectEngine::isInitialized() const
{
    return pImpl->initialized;
}

array2D<float> AISubjectEngine::saliency(float* const* rRows, float* const* gRows, float* const* bRows,
                                         int width, int height, bool multiThread) const
{
    array2D<float> result;

    if (!pImpl->initialized || width <= 0 || height <= 0) {
        return result;
    }

    // One session shared by the coordinator, the thumbnail pool and the
    // picker's workers; the DirectML provider is not safe for concurrent Run.
    std::lock_guard<std::mutex> inferenceLock(pImpl->runMutex);
    // Freed when idle (AIIdleReaper); back for this photo.
    if (!pImpl->loadSessionLocked()) {
        return result;
    }
    pImpl->lastUse = AIIdleReaper::nowMs();

    const int side = pImpl->side;
    // U^2-Net's reference preparation stretches by the brightest value;
    // BiRefNet (and anything else) takes plain sRGB in 0..1.
    const bool stretchToPeak = side == SUBJECT_INPUT;
    std::vector<float> rgb(3 * side * side);

    const float scaleX = static_cast<float>(width) / side;
    const float scaleY = static_cast<float>(height) / side;

    // U^2-Net's own preparation divides by the picture's brightest value
    // rather than by white, so a dark frame is stretched before the ImageNet
    // normalisation rather than arriving as a near-black block.
    float peak = stretchToPeak ? 1e-6f : 1.f;

    for (int y = 0; y < side; ++y) {
        const float srcY = std::max(0.f, (y + 0.5f) * scaleY - 0.5f);
        const int sy0 = std::min(static_cast<int>(srcY), height - 1);
        const int sy1 = std::min(sy0 + 1, height - 1);
        const float fy = srcY - sy0;

        for (int x = 0; x < side; ++x) {
            const float srcX = std::max(0.f, (x + 0.5f) * scaleX - 0.5f);
            const int sx0 = std::min(static_cast<int>(srcX), width - 1);
            const int sx1 = std::min(sx0 + 1, width - 1);
            const float fx = srcX - sx0;

            const auto sample = [=](float* const* rows) {
                const float top = rows[sy0][sx0] + fx * (rows[sy0][sx1] - rows[sy0][sx0]);
                const float bottom = rows[sy1][sx0] + fx * (rows[sy1][sx1] - rows[sy1][sx0]);
                return top + fy * (bottom - top);
            };

            const float r = linearToSRGB(sample(rRows) / 65535.f);
            const float g = linearToSRGB(sample(gRows) / 65535.f);
            const float b = linearToSRGB(sample(bRows) / 65535.f);

            const size_t plane = static_cast<size_t>(side) * side;
            const size_t at = static_cast<size_t>(y) * side + x;
            rgb[at] = r;
            rgb[plane + at] = g;
            rgb[2 * plane + at] = b;
            if (stretchToPeak) {
                peak = std::max(peak, std::max(r, std::max(g, b)));
            }
        }
    }

    const size_t plane = static_cast<size_t>(side) * side;

    for (size_t i = 0; i < plane; ++i) {
        rgb[i] = (rgb[i] / peak - MEAN_R) / STD_R;
        rgb[plane + i] = (rgb[plane + i] / peak - MEAN_G) / STD_G;
        rgb[2 * plane + i] = (rgb[2 * plane + i] / peak - MEAN_B) / STD_B;
    }

    const int64_t shape[4] = {1, 3, side, side};
    OrtValue* input = nullptr;
    OrtStatus* status = pImpl->api->CreateTensorWithDataAsOrtValue(
        pImpl->memoryInfo, rgb.data(), rgb.size() * sizeof(float),
        shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input);

    if (status) {
        pImpl->api->ReleaseStatus(status);
        return result;
    }

    const char* inputNames[] = {pImpl->inputName.c_str()};
    const char* outputNames[] = {pImpl->outputName.c_str()};
    OrtValue* output = nullptr;

    status = pImpl->api->Run(pImpl->session, nullptr, inputNames, &input, 1, outputNames, 1, &output);

    if (status && AIIdleReaper::outOfMemory(pImpl->api->GetErrorMessage(status))) {
        // Out of memory -- often another program's, on a GPU it shares.
        // Free every other model and this session's buffers, then once more.
        std::fprintf(stderr, "AI Subject: out of memory; freeing models and retrying\n");
        pImpl->api->ReleaseStatus(status);
        status = nullptr;
        pImpl->api->ReleaseSession(pImpl->session);
        pImpl->session = nullptr;
        AIIdleReaper::releaseNow(pImpl->reaperId);
        std::this_thread::sleep_for(std::chrono::milliseconds(750));

        if (!pImpl->loadSessionLocked()) {
            pImpl->api->ReleaseValue(input);
            return result;
        }

        status = pImpl->api->Run(pImpl->session, nullptr, inputNames, &input, 1, outputNames, 1, &output);
    }

    pImpl->api->ReleaseValue(input);

    if (status) {
        std::fprintf(stderr, "AI Subject: inference failed: %s\n", pImpl->api->GetErrorMessage(status));
        pImpl->api->ReleaseStatus(status);
        return result;
    }

    float* data = nullptr;
    status = pImpl->api->GetTensorMutableData(output, reinterpret_cast<void**>(&data));

    if (status || !data) {
        if (status) {
            pImpl->api->ReleaseStatus(status);
        }

        pImpl->api->ReleaseValue(output);
        return result;
    }

    // Back to the caller's resolution. The map arrives as a probability
    // already, so there is nothing to normalise -- and normalising it, as
    // some reference code does, would stretch an empty frame's noise into a
    // confident-looking subject.
    result(width, height);

    const float outScaleX = static_cast<float>(side) / width;
    const float outScaleY = static_cast<float>(side) / height;

#ifdef _OPENMP
    #pragma omp parallel for if(multiThread)
#endif
    for (int y = 0; y < height; ++y) {
        const float srcY = std::max(0.f, (y + 0.5f) * outScaleY - 0.5f);
        const int sy0 = std::min(static_cast<int>(srcY), side - 1);
        const int sy1 = std::min(sy0 + 1, side - 1);
        const float fy = srcY - sy0;

        for (int x = 0; x < width; ++x) {
            const float srcX = std::max(0.f, (x + 0.5f) * outScaleX - 0.5f);
            const int sx0 = std::min(static_cast<int>(srcX), side - 1);
            const int sx1 = std::min(sx0 + 1, side - 1);
            const float fx = srcX - sx0;

            const float top = data[sy0 * side + sx0] + fx * (data[sy0 * side + sx1] - data[sy0 * side + sx0]);
            const float bottom = data[sy1 * side + sx0] + fx * (data[sy1 * side + sx1] - data[sy1 * side + sx0]);
            result[y][x] = std::min(std::max(top + fy * (bottom - top), 0.f), 1.f);
        }
    }

    pImpl->api->ReleaseValue(output);
    pImpl->lastUse = AIIdleReaper::nowMs();
    return result;
}

AISubjectEngine& getAISubjectEngine()
{
    // Process lifetime, like the inpainting engine: a short export can finish
    // while the loader still owns `this`. Static destruction races that load,
    // and joining under Windows DLL teardown can deadlock model initialization.
    static auto* instance = new AISubjectEngine();
    return *instance;
}

} // namespace rtengine

#endif // RT_AI_MASKING
