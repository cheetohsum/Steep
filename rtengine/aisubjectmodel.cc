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

#include "onnxruntime_compat.h"

#ifdef RT_AI_MASKING_DIRECTML
#include <dml_provider_factory.h>
#endif

#include <algorithm>
#include <cmath>
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
    bool initialized = false;
    bool usingDirectML = false;
    std::string inputName;
    std::string outputName;
    std::mutex initMutex;

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

    OrtStatus* status = pImpl->api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "RawTherapee_Subject", &pImpl->env);

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

#ifdef _WIN32
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, modelPath.c_str(), -1, nullptr, 0);
    std::vector<wchar_t> wpath(wlen);
    MultiByteToWideChar(CP_UTF8, 0, modelPath.c_str(), -1, wpath.data(), wlen);
    status = pImpl->api->CreateSession(pImpl->env, wpath.data(), pImpl->sessionOptions, &pImpl->session);
#else
    status = pImpl->api->CreateSession(pImpl->env, modelPath.c_str(), pImpl->sessionOptions, &pImpl->session);
#endif

    if (status) {
        std::fprintf(stderr, "AI Subject: failed to load model: %s\n", pImpl->api->GetErrorMessage(status));
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

    pImpl->initialized = true;
    std::fprintf(stderr, "AI Subject: initialised from %s (%s)\n", modelPath.c_str(),
                 pImpl->usingDirectML ? "DirectML" : "CPU");
    return true;
}

void AISubjectEngine::initDeferred(const std::string& modelPath)
{
    // Building a session for a 170 MB model takes seconds; startup should not
    // wait for a model that is only wanted once a subject mask is asked for.
    std::thread([this, modelPath]() {
        this->init(modelPath);
    }).detach();
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
    static std::mutex inferenceMutex;
    std::lock_guard<std::mutex> inferenceLock(inferenceMutex);

    const int side = SUBJECT_INPUT;
    std::vector<float> rgb(3 * side * side);

    const float scaleX = static_cast<float>(width) / side;
    const float scaleY = static_cast<float>(height) / side;

    // U^2-Net's own preparation divides by the picture's brightest value
    // rather than by white, so a dark frame is stretched before the ImageNet
    // normalisation rather than arriving as a near-black block.
    float peak = 1e-6f;

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
            peak = std::max(peak, std::max(r, std::max(g, b)));
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
    return result;
}

AISubjectEngine& getAISubjectEngine()
{
    static AISubjectEngine instance;
    return instance;
}

} // namespace rtengine

#endif // RT_AI_MASKING
