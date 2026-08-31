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
#include <atomic>
#include <cmath>
#include <thread>

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
    // Whether the export accepts non-512 input dims: -1 unknown, 1 yes, 0 no.
    // Probed on first use; native-resolution inference beats the 512 squash
    // for anything large (LaMa is fully convolutional).
    int dynamicDims;
    // Output range of THIS export: 0 unknown, 1 = [0,1], 255 = [0,255].
    // A property of the model, so it is decided once and reused. Deciding
    // per call from the data peak misfires on dark content (a dark tile can
    // peak below the threshold and get scaled 255x wrong) and can disagree
    // between the tiles of one fill.
    std::atomic<int> outputRange;

    Impl() : api(nullptr), env(nullptr), session(nullptr),
             sessionOptions(nullptr), memoryInfo(nullptr), initialized(false),
             dynamicDims(-1), outputRange(0)
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

    {
        const unsigned int hardwareThreads = std::thread::hardware_concurrency();
        const int inferenceThreads = static_cast<int>(
            std::max(1u, std::min(4u, hardwareThreads > 1 ? hardwareThreads / 2 : 1u)));
        pImpl->api->SetIntraOpNumThreads(pImpl->sessionOptions, inferenceThreads);
    }
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

    // The pipeline hands us scene-linear data, but LaMa is trained on
    // display-referred (gamma-encoded) images. Feed it sRGB-encoded values
    // and decode its output again, or fills come back with wrong tones and
    // color casts.
    const auto srgbEncode = [](float v) -> float {
        v = std::max(0.f, std::min(1.f, v));
        return v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f;
    };
    const auto srgbDecode = [](float v) -> float {
        v = std::max(0.f, std::min(1.f, v));
        return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
    };

    // Area-average when shrinking (no aliasing confetti), bilinear when
    // growing. The old path truncated coordinates — nearest-neighbor both
    // ways — which aliased fine texture into colored speckle.
    const auto resamplePlane = [](const float* src, int sw, int sh,
                                  float* dst, int dw, int dh) {
        if (dw == sw && dh == sh) {
            std::memcpy(dst, src, static_cast<size_t>(sw) * sh * sizeof(float));
            return;
        }
        if (static_cast<long long>(dw) * dh < static_cast<long long>(sw) * sh) {
            for (int y = 0; y < dh; ++y) {
                const int y0 = static_cast<int>(static_cast<float>(y) * sh / dh);
                const int y1 = std::min(sh, std::max(y0 + 1,
                    static_cast<int>(std::ceil(static_cast<float>(y + 1) * sh / dh))));
                for (int x = 0; x < dw; ++x) {
                    const int x0 = static_cast<int>(static_cast<float>(x) * sw / dw);
                    const int x1 = std::min(sw, std::max(x0 + 1,
                        static_cast<int>(std::ceil(static_cast<float>(x + 1) * sw / dw))));
                    double sum = 0.0;
                    for (int yy = y0; yy < y1; ++yy) {
                        for (int xx = x0; xx < x1; ++xx) {
                            sum += src[yy * sw + xx];
                        }
                    }
                    dst[y * dw + x] = static_cast<float>(sum / ((y1 - y0) * (x1 - x0)));
                }
            }
        } else {
            for (int y = 0; y < dh; ++y) {
                const float sy = dh > 1 ? static_cast<float>(y) * (sh - 1) / (dh - 1) : 0.f;
                const int y0 = std::min(static_cast<int>(sy), sh - 1);
                const int y1 = std::min(y0 + 1, sh - 1);
                const float wy = sy - y0;
                for (int x = 0; x < dw; ++x) {
                    const float sx = dw > 1 ? static_cast<float>(x) * (sw - 1) / (dw - 1) : 0.f;
                    const int x0 = std::min(static_cast<int>(sx), sw - 1);
                    const int x1 = std::min(x0 + 1, sw - 1);
                    const float wx = sx - x0;
                    const float top = src[y0 * sw + x0] + wx * (src[y0 * sw + x1] - src[y0 * sw + x0]);
                    const float bot = src[y1 * sw + x0] + wx * (src[y1 * sw + x1] - src[y1 * sw + x0]);
                    dst[y * dw + x] = top + wy * (bot - top);
                }
            }
        }
    };

    // Encode once at source resolution; resampling then happens in the
    // model's (display-referred) space where interpolation behaves.
    const int srcCount = width * height;
    std::vector<float> encR(srcCount), encG(srcCount), encB(srcCount);
    for (int i = 0; i < srcCount; ++i) {
        encR[i] = srgbEncode(imageR[i] / 65535.f);
        encG[i] = srgbEncode(imageG[i] / 65535.f);
        encB[i] = srgbEncode(imageB[i] / 65535.f);
    }

    // LaMa is fully convolutional, so most exports accept arbitrary /8 input
    // dims. Native-resolution inference keeps large fills sharp where the
    // fixed 512 squash goes soft; probe it once, fall back if the export
    // insists on 512.
    // Capped: LaMa's FFT layers scale badly past ~0.7MP on CPU — a 1.5MP
    // native pass took seconds and made 1:1 panning feel broken.
    const bool nativeWorthIt = width >= 256 && height >= 256
        && static_cast<long long>(width) * height <= 700000;
    const int firstAttempt = (nativeWorthIt && pImpl->dynamicDims != 0) ? 0 : 1;

    for (int attempt = firstAttempt; attempt < 2; ++attempt) {
        const int modelW = attempt == 0 ? ((width + 7) / 8) * 8 : LAMA_TILE_SIZE;
        const int modelH = attempt == 0 ? ((height + 7) / 8) * 8 : LAMA_TILE_SIZE;

        // Prepare image input tensor: (1, 3, modelH, modelW) in [0, 1]
        std::vector<float> imageTensor(3 * modelH * modelW, 0.f);
        // Prepare mask input tensor: (1, 1, modelH, modelW) binary
        std::vector<float> maskTensor(modelH * modelW, 0.f);

        resamplePlane(encR.data(), width, height, &imageTensor[0 * modelH * modelW], modelW, modelH);
        resamplePlane(encG.data(), width, height, &imageTensor[1 * modelH * modelW], modelW, modelH);
        resamplePlane(encB.data(), width, height, &imageTensor[2 * modelH * modelW], modelW, modelH);

        // The caller's mask is soft (feathered stroke). The model wants a
        // binary region: everything the stroke touches gets filled, and the
        // caller blends the result back through the soft values.
        resamplePlane(mask, width, height, maskTensor.data(), modelW, modelH);
        for (int i = 0; i < modelH * modelW; ++i) {
            maskTensor[i] = maskTensor[i] > 0.05f ? 1.f : 0.f;
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
            if (attempt == 0) {
                fprintf(stderr, "AI Inpainting: native-size inference unavailable (%s), using 512 tile\n", msg);
                pImpl->api->ReleaseStatus(status);
                pImpl->dynamicDims = 0;
                continue;
            }
            fprintf(stderr, "AI Inpainting: Inference failed: %s\n", msg);
            pImpl->api->ReleaseStatus(status);
            return false;
        }

        if (attempt == 0) {
            pImpl->dynamicDims = 1;
        }

        // Extract output data
        float* outputData = nullptr;
        pImpl->api->GetTensorMutableData(outputOrt, reinterpret_cast<void**>(&outputData));

        // LaMa exports disagree on the output range: some emit [0,1], the common
        // Carve export emits [0,255]. Sample the output and normalize either way.
        int range = pImpl->outputRange.load();
        if (range == 0) {
            float outPeak = 0.f;
            const int outCount = 3 * modelH * modelW;
            for (int i = 0; i < outCount; i += 7) {
                outPeak = std::max(outPeak, outputData[i]);
            }
            // Only a confident sample decides it: a peak in the ambiguous
            // middle leaves the question open for a later, brighter call
            // rather than locking in a guess from dark content.
            if (outPeak > 8.f) {
                range = 255;
                pImpl->outputRange.store(range);
            } else if (outPeak > 0.f && outPeak <= 1.5f) {
                range = 1;
                pImpl->outputRange.store(range);
            } else {
                range = outPeak > 2.f ? 255 : 1;
            }
        }
        const float outNorm = range == 255 ? (1.f / 255.f) : 1.f;

        // Normalize in place, resample the encoded planes back up (bilinear),
        // then decode to linear [0, 65535]. The whole reconstruction is
        // returned — blending fill against original is the caller's job,
        // using its soft mask.
        const int planeCount = modelH * modelW;
        for (int i = 0; i < 3 * planeCount; ++i) {
            outputData[i] *= outNorm;
        }

        resamplePlane(&outputData[0 * planeCount], modelW, modelH, outR, width, height);
        resamplePlane(&outputData[1 * planeCount], modelW, modelH, outG, width, height);
        resamplePlane(&outputData[2 * planeCount], modelW, modelH, outB, width, height);

        for (int i = 0; i < srcCount; ++i) {
            outR[i] = srgbDecode(outR[i]) * 65535.f;
            outG[i] = srgbDecode(outG[i]) * 65535.f;
            outB[i] = srgbDecode(outB[i]) * 65535.f;
        }

        pImpl->api->ReleaseValue(outputOrt);
        return true;
    }

    return false;
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
