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

#include "aiinpainting.h"
#include "onnxruntime_compat.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cmath>

#ifdef _WIN32
#include <windows.h>
#endif

namespace rtengine
{

// LaMa model expects 512x512 input tiles. For larger regions, we tile.
constexpr int LAMA_TILE_SIZE = 512;
constexpr int LAMA_OVERLAP = 32;

struct AIInpaintingEngine::Impl {
    const OrtApi* api;
    OrtEnv* env;
    OrtSession* session;
    OrtSessionOptions* sessionOptions;
    OrtMemoryInfo* memoryInfo;
    bool initialized;

    Impl() : api(nullptr), env(nullptr), session(nullptr),
             sessionOptions(nullptr), memoryInfo(nullptr), initialized(false)
    {
    }

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

AIInpaintingEngine::AIInpaintingEngine()
    : pImpl(new Impl())
{
}

AIInpaintingEngine::~AIInpaintingEngine() = default;

bool AIInpaintingEngine::init(const std::string& modelPath)
{
    pImpl->api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (!pImpl->api) {
        fprintf(stderr, "AI Inpainting: Failed to get ONNX Runtime API\n");
        return false;
    }

    OrtStatus* status = pImpl->api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "RawTherapee_Inpaint", &pImpl->env);
    if (status) {
        pImpl->api->ReleaseStatus(status);
        return false;
    }

    status = pImpl->api->CreateSessionOptions(&pImpl->sessionOptions);
    if (status) {
        pImpl->api->ReleaseStatus(status);
        return false;
    }

    pImpl->api->SetIntraOpNumThreads(pImpl->sessionOptions, 1);
    pImpl->api->SetSessionGraphOptimizationLevel(pImpl->sessionOptions, ORT_ENABLE_EXTENDED);

#ifdef _WIN32
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, modelPath.c_str(), -1, nullptr, 0);
    std::vector<wchar_t> wpath(wlen);
    MultiByteToWideChar(CP_UTF8, 0, modelPath.c_str(), -1, wpath.data(), wlen);
    status = pImpl->api->CreateSession(pImpl->env, wpath.data(), pImpl->sessionOptions, &pImpl->session);
#else
    status = pImpl->api->CreateSession(pImpl->env, modelPath.c_str(), pImpl->sessionOptions, &pImpl->session);
#endif
    if (status) {
        const char* msg = pImpl->api->GetErrorMessage(status);
        fprintf(stderr, "AI Inpainting: Failed to load model: %s\n", msg);
        pImpl->api->ReleaseStatus(status);
        return false;
    }

    status = pImpl->api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &pImpl->memoryInfo);
    if (status) {
        pImpl->api->ReleaseStatus(status);
        return false;
    }

    pImpl->initialized = true;
    fprintf(stderr, "AI Inpainting: LaMa engine initialized successfully\n");
    return true;
}

bool AIInpaintingEngine::isInitialized() const
{
    return pImpl->initialized;
}

bool AIInpaintingEngine::inpaint(const float* imageR, const float* imageG, const float* imageB,
                                  const float* mask, int width, int height,
                                  float* outR, float* outG, float* outB)
{
    if (!pImpl->initialized) {
        fprintf(stderr, "AI Inpainting: Engine not initialized\n");
        return false;
    }

    // For simplicity, resize to LAMA_TILE_SIZE x LAMA_TILE_SIZE for inference
    // then resize back. For production, we'd tile larger images.
    const int modelW = LAMA_TILE_SIZE;
    const int modelH = LAMA_TILE_SIZE;

    // Prepare image input tensor: (1, 3, modelH, modelW) in [0, 1]
    std::vector<float> imageTensor(3 * modelH * modelW, 0.f);
    // Prepare mask input tensor: (1, 1, modelH, modelW) binary
    std::vector<float> maskTensor(modelH * modelW, 0.f);

    // Bilinear resize from (width, height) to (modelW, modelH) and normalize to [0,1]
    for (int y = 0; y < modelH; ++y) {
        float srcY = float(y) * float(height - 1) / float(modelH - 1);
        int sy = std::min(int(srcY), height - 1);
        for (int x = 0; x < modelW; ++x) {
            float srcX = float(x) * float(width - 1) / float(modelW - 1);
            int sx = std::min(int(srcX), width - 1);
            int srcIdx = sy * width + sx;
            int dstIdx = y * modelW + x;

            // Convert from [0, 65535] to [0, 1]
            imageTensor[0 * modelH * modelW + dstIdx] = imageR[srcIdx] / 65535.f;
            imageTensor[1 * modelH * modelW + dstIdx] = imageG[srcIdx] / 65535.f;
            imageTensor[2 * modelH * modelW + dstIdx] = imageB[srcIdx] / 65535.f;

            maskTensor[dstIdx] = mask[srcIdx] > 0.5f ? 1.f : 0.f;
        }
    }

    // Create ONNX tensors
    const int64_t imageShape[] = {1, 3, modelH, modelW};
    const int64_t maskShape[] = {1, 1, modelH, modelW};

    OrtValue* imageOrt = nullptr;
    OrtValue* maskOrt = nullptr;
    OrtValue* outputOrt = nullptr;

    OrtStatus* status = pImpl->api->CreateTensorWithDataAsOrtValue(
        pImpl->memoryInfo, imageTensor.data(), imageTensor.size() * sizeof(float),
        imageShape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &imageOrt);

    if (status) {
        const char* msg = pImpl->api->GetErrorMessage(status);
        fprintf(stderr, "AI Inpainting: Failed to create image tensor: %s\n", msg);
        pImpl->api->ReleaseStatus(status);
        return false;
    }

    status = pImpl->api->CreateTensorWithDataAsOrtValue(
        pImpl->memoryInfo, maskTensor.data(), maskTensor.size() * sizeof(float),
        maskShape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &maskOrt);

    if (status) {
        const char* msg = pImpl->api->GetErrorMessage(status);
        fprintf(stderr, "AI Inpainting: Failed to create mask tensor: %s\n", msg);
        pImpl->api->ReleaseStatus(status);
        pImpl->api->ReleaseValue(imageOrt);
        return false;
    }

    // Run inference
    const char* inputNames[] = {"image", "mask"};
    const char* outputNames[] = {"output"};
    OrtValue* inputs[] = {imageOrt, maskOrt};

    status = pImpl->api->Run(pImpl->session, nullptr,
                             inputNames, inputs, 2,
                             outputNames, 1, &outputOrt);

    pImpl->api->ReleaseValue(imageOrt);
    pImpl->api->ReleaseValue(maskOrt);

    if (status) {
        const char* msg = pImpl->api->GetErrorMessage(status);
        fprintf(stderr, "AI Inpainting: Inference failed: %s\n", msg);
        pImpl->api->ReleaseStatus(status);
        return false;
    }

    // Extract output data
    float* outputData = nullptr;
    pImpl->api->GetTensorMutableData(outputOrt, reinterpret_cast<void**>(&outputData));

    // Resize output back from (modelW, modelH) to (width, height) and convert to [0, 65535]
    for (int y = 0; y < height; ++y) {
        float srcY = float(y) * float(modelH - 1) / float(height - 1);
        int sy = std::min(int(srcY), modelH - 1);
        for (int x = 0; x < width; ++x) {
            float srcX = float(x) * float(modelW - 1) / float(width - 1);
            int sx = std::min(int(srcX), modelW - 1);
            int srcIdx = sy * modelW + sx;
            int dstIdx = y * width + x;

            outR[dstIdx] = std::max(0.f, std::min(65535.f, outputData[0 * modelH * modelW + srcIdx] * 65535.f));
            outG[dstIdx] = std::max(0.f, std::min(65535.f, outputData[1 * modelH * modelW + srcIdx] * 65535.f));
            outB[dstIdx] = std::max(0.f, std::min(65535.f, outputData[2 * modelH * modelW + srcIdx] * 65535.f));

            // Only replace pixels where mask is active
            if (mask[dstIdx] <= 0.5f) {
                outR[dstIdx] = imageR[dstIdx];
                outG[dstIdx] = imageG[dstIdx];
                outB[dstIdx] = imageB[dstIdx];
            }
        }
    }

    pImpl->api->ReleaseValue(outputOrt);
    return true;
}

// Singleton
static AIInpaintingEngine* s_inpaintEngine = nullptr;

AIInpaintingEngine& getAIInpaintingEngine()
{
    if (!s_inpaintEngine) {
        s_inpaintEngine = new AIInpaintingEngine();
    }
    return *s_inpaintEngine;
}

} // namespace rtengine

#endif // RT_AI_MASKING
