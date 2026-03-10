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

#include "aisegmentation.h"
#include "guidedfilter.h"
#include "opthelper.h"
#include "rt_math.h"

#include "onnxruntime_compat.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#endif

namespace rtengine
{

namespace
{

// ADE20K class mapping to AISegClass (150 classes)
// See: https://huggingface.co/nvidia/segformer-b0-finetuned-ade-512-512
AISegClass adeToAIClass(int adeClass)
{
    switch (adeClass) {
        // Person
        case 12: // person
            return AISegClass::PERSON;

        // Sky
        case 2: // sky
            return AISegClass::SKY;

        // Vegetation: trees, grass, plants, flowers, fields
        case 4:  // tree
        case 9:  // grass
        case 17: // plant
        case 29: // field
        case 66: // flower
        case 68: // hill
        case 72: // palm
        case 94: // land
            return AISegClass::VEGETATION;

        // Building: structures, walls, houses, towers
        case 0:  // wall
        case 1:  // building
        case 5:  // ceiling
        case 25: // house
        case 32: // fence
        case 42: // column
        case 48: // skyscraper
        case 61: // bridge
        case 84: // tower
            return AISegClass::BUILDING;

        // Vehicle: cars, buses, trucks, boats, bikes
        case 20:  // car
        case 76:  // boat
        case 80:  // bus
        case 83:  // truck
        case 90:  // airplane
        case 102: // van
        case 103: // ship
        case 116: // minibike
        case 127: // bicycle
            return AISegClass::VEHICLE;

        // Animal
        case 126: // animal
            return AISegClass::ANIMAL;

        // Foreground objects: furniture, appliances, common indoor objects
        case 7:  // bed
        case 10: // cabinet
        case 15: // table
        case 19: // chair
        case 23: // sofa
        case 24: // shelf
        case 30: // armchair
        case 33: // desk
        case 35: // wardrobe
        case 36: // lamp
        case 37: // bathtub
        case 44: // chest of drawers
        case 47: // sink
        case 50: // refrigerator
        case 62: // bookcase
        case 64: // coffee table
        case 65: // toilet
        case 69: // bench
        case 71: // stove
        case 74: // computer
        case 85: // chandelier
        case 89: // television receiver
        case 98: // bottle
            return AISegClass::FOREGROUND_OBJECT;

        // Background: floors, roads, sidewalks, earth, water, etc.
        default:
            return AISegClass::BACKGROUND;
    }
}

// Max model input dimension. The model was trained on 512x512.
// Higher values give sharper masks at the cost of inference time.
constexpr int MODEL_INPUT_SIZE = 1024;

// ImageNet normalization constants
constexpr float MEAN_R = 0.485f;
constexpr float MEAN_G = 0.456f;
constexpr float MEAN_B = 0.406f;
constexpr float STD_R = 0.229f;
constexpr float STD_G = 0.224f;
constexpr float STD_B = 0.225f;

constexpr int NUM_ADE_CLASSES = 150;

} // namespace

struct AISegmentationEngine::Impl {
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

AISegmentationEngine::AISegmentationEngine() : pImpl(new Impl())
{
}

AISegmentationEngine::~AISegmentationEngine()
{
    delete pImpl;
}

bool AISegmentationEngine::init(const std::string& modelPath)
{
    pImpl->api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (!pImpl->api) {
        return false;
    }

    OrtStatus* status = pImpl->api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "RawTherapee_AI", &pImpl->env);
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
    // ONNX Runtime on Windows requires wide-char path
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, modelPath.c_str(), -1, nullptr, 0);
    std::vector<wchar_t> wpath(wlen);
    MultiByteToWideChar(CP_UTF8, 0, modelPath.c_str(), -1, wpath.data(), wlen);
    status = pImpl->api->CreateSession(pImpl->env, wpath.data(), pImpl->sessionOptions, &pImpl->session);
#else
    status = pImpl->api->CreateSession(pImpl->env, modelPath.c_str(), pImpl->sessionOptions, &pImpl->session);
#endif
    if (status) {
        const char* msg = pImpl->api->GetErrorMessage(status);
        fprintf(stderr, "AI Masking: Failed to load model: %s\n", msg);
        pImpl->api->ReleaseStatus(status);
        return false;
    }

    status = pImpl->api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &pImpl->memoryInfo);
    if (status) {
        pImpl->api->ReleaseStatus(status);
        return false;
    }

    pImpl->initialized = true;
    return true;
}

bool AISegmentationEngine::isInitialized() const
{
    return pImpl->initialized;
}

int AISegmentationEngine::getClassCount()
{
    return static_cast<int>(AISegClass::NUM_CLASSES);
}

const char* AISegmentationEngine::getClassName(AISegClass cls)
{
    switch (cls) {
        case AISegClass::BACKGROUND: return "Background";
        case AISegClass::PERSON: return "Person";
        case AISegClass::SKY: return "Sky";
        case AISegClass::VEGETATION: return "Vegetation";
        case AISegClass::BUILDING: return "Building";
        case AISegClass::VEHICLE: return "Vehicle";
        case AISegClass::ANIMAL: return "Animal";
        case AISegClass::FOREGROUND_OBJECT: return "Foreground Object";
        default: return "Unknown";
    }
}

std::vector<array2D<float>> AISegmentationEngine::segment(
    float* const* rRows, float* const* gRows, float* const* bRows,
    int width, int height, bool multiThread) const
{
    const int numClasses = static_cast<int>(AISegClass::NUM_CLASSES);
    std::vector<array2D<float>> result(numClasses);

    if (!pImpl->initialized) {
        // Return empty probability maps
        for (int c = 0; c < numClasses; ++c) {
            result[c](width, height);
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    result[c][y][x] = 0.f;
                }
            }
        }
        return result;
    }

    // 1. Preprocess: planar linear RGB [0,65535] -> sRGB [0,1] -> ImageNet norm -> [1,3,H,W]
    // Only downscale if image exceeds MODEL_INPUT_SIZE; never upscale
    const float scale = std::min(1.0f, static_cast<float>(MODEL_INPUT_SIZE) / std::max(width, height));
    const int modelW = std::max(1, static_cast<int>(width * scale));
    const int modelH = std::max(1, static_cast<int>(height * scale));

    std::vector<float> inputTensor(1 * 3 * modelH * modelW);

    const float scaleX = static_cast<float>(width) / modelW;
    const float scaleY = static_cast<float>(height) / modelH;

    // sRGB gamma: linear -> sRGB (model was trained on sRGB images)
    auto linearToSRGB = [](float v) -> float {
        v = std::max(0.f, std::min(1.f, v));
        return (v <= 0.0031308f) ? (12.92f * v) : (1.055f * std::pow(v, 1.f / 2.4f) - 0.055f);
    };

    fprintf(stderr, "AI Masking: Input %dx%d -> model %dx%d\n", width, height, modelW, modelH);

#ifdef _OPENMP
    #pragma omp parallel for if(multiThread)
#endif
    for (int y = 0; y < modelH; ++y) {
        const float srcY = y * scaleY;
        const int sy = std::min(static_cast<int>(srcY), height - 1);

        for (int x = 0; x < modelW; ++x) {
            const float srcX = x * scaleX;
            const int sx = std::min(static_cast<int>(srcX), width - 1);

            // Convert linear RGB [0,65535] to sRGB [0,1]
            const float r = linearToSRGB(rRows[sy][sx] / 65535.f);
            const float g = linearToSRGB(gRows[sy][sx] / 65535.f);
            const float b = linearToSRGB(bRows[sy][sx] / 65535.f);

            // CHW layout with ImageNet normalization
            inputTensor[0 * modelH * modelW + y * modelW + x] = (r - MEAN_R) / STD_R;
            inputTensor[1 * modelH * modelW + y * modelW + x] = (g - MEAN_G) / STD_G;
            inputTensor[2 * modelH * modelW + y * modelW + x] = (b - MEAN_B) / STD_B;
        }
    }

    // 2. Run inference
    const int64_t inputShape[] = {1, 3, modelH, modelW};
    OrtValue* inputOrt = nullptr;
    OrtStatus* status = pImpl->api->CreateTensorWithDataAsOrtValue(
        pImpl->memoryInfo, inputTensor.data(), inputTensor.size() * sizeof(float),
        inputShape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inputOrt);

    if (status) {
        pImpl->api->ReleaseStatus(status);
        for (int c = 0; c < numClasses; ++c) {
            result[c](width, height);
        }
        return result;
    }

    const char* inputNames[] = {"input"};
    const char* outputNames[] = {"output"};
    OrtValue* outputOrt = nullptr;

    status = pImpl->api->Run(pImpl->session, nullptr,
                             inputNames, &inputOrt, 1,
                             outputNames, 1, &outputOrt);

    pImpl->api->ReleaseValue(inputOrt);

    if (status) {
        const char* msg = pImpl->api->GetErrorMessage(status);
        fprintf(stderr, "AI Masking: Inference failed: %s\n", msg);
        pImpl->api->ReleaseStatus(status);
        for (int c = 0; c < numClasses; ++c) {
            result[c](width, height);
        }
        return result;
    }

    // 3. Postprocess: [1,21,H,W] logits -> softmax -> map to AISegClass -> upscale
    float* outputData = nullptr;
    pImpl->api->GetTensorMutableData(outputOrt, reinterpret_cast<void**>(&outputData));

    OrtTensorTypeAndShapeInfo* typeInfo = nullptr;
    pImpl->api->GetTensorTypeAndShape(outputOrt, &typeInfo);

    size_t dimCount = 0;
    pImpl->api->GetDimensionsCount(typeInfo, &dimCount);

    std::vector<int64_t> dims(dimCount);
    pImpl->api->GetDimensions(typeInfo, dims.data(), dimCount);
    pImpl->api->ReleaseTensorTypeAndShapeInfo(typeInfo);

    const int outClasses = (dimCount >= 2) ? static_cast<int>(dims[1]) : NUM_ADE_CLASSES;
    const int outH = (dimCount >= 3) ? static_cast<int>(dims[2]) : modelH;
    const int outW = (dimCount >= 4) ? static_cast<int>(dims[3]) : modelW;

    fprintf(stderr, "AI Masking: Output tensor [%d, %d, %d]\n", outClasses, outH, outW);

    // Softmax and class mapping at model resolution
    std::vector<array2D<float>> modelMaps(numClasses);
    for (int c = 0; c < numClasses; ++c) {
        modelMaps[c](outW, outH);
        for (int y = 0; y < outH; ++y) {
            for (int x = 0; x < outW; ++x) {
                modelMaps[c][y][x] = 0.f;
            }
        }
    }

    // Process each pixel: softmax over ADE20K classes, then accumulate into AISegClass
#ifdef _OPENMP
    #pragma omp parallel for if(multiThread)
#endif
    for (int y = 0; y < outH; ++y) {
        for (int x = 0; x < outW; ++x) {
            // Find max for numerical stability
            float maxVal = -1e30f;
            for (int c = 0; c < outClasses; ++c) {
                float val = outputData[c * outH * outW + y * outW + x];
                if (val > maxVal) maxVal = val;
            }

            // Softmax
            float sumExp = 0.f;
            std::vector<float> probs(outClasses);
            for (int c = 0; c < outClasses; ++c) {
                probs[c] = std::exp(outputData[c * outH * outW + y * outW + x] - maxVal);
                sumExp += probs[c];
            }
            for (int c = 0; c < outClasses; ++c) {
                probs[c] /= sumExp;
            }

            // Accumulate into AISegClass (ADE20K has real sky/vegetation classes)
            for (int c = 0; c < outClasses; ++c) {
                AISegClass aiClass = adeToAIClass(c);
                modelMaps[static_cast<int>(aiClass)][y][x] += probs[c];
            }
        }
    }

    pImpl->api->ReleaseValue(outputOrt);

    // 4. Upscale to output resolution via bilinear interpolation + guided filter refinement
    for (int c = 0; c < numClasses; ++c) {
        result[c](width, height);

        const float xScale = static_cast<float>(outW) / width;
        const float yScale = static_cast<float>(outH) / height;

#ifdef _OPENMP
        #pragma omp parallel for if(multiThread)
#endif
        for (int y = 0; y < height; ++y) {
            const float srcYf = y * yScale;
            const int sy0 = std::min(static_cast<int>(srcYf), outH - 1);
            const int sy1 = std::min(sy0 + 1, outH - 1);
            const float fy = srcYf - sy0;

            for (int x = 0; x < width; ++x) {
                const float srcXf = x * xScale;
                const int sx0 = std::min(static_cast<int>(srcXf), outW - 1);
                const int sx1 = std::min(sx0 + 1, outW - 1);
                const float fx = srcXf - sx0;

                // Bilinear interpolation
                const float v00 = modelMaps[c][sy0][sx0];
                const float v01 = modelMaps[c][sy0][sx1];
                const float v10 = modelMaps[c][sy1][sx0];
                const float v11 = modelMaps[c][sy1][sx1];

                result[c][y][x] = (1.f - fy) * ((1.f - fx) * v00 + fx * v01)
                                + fy * ((1.f - fx) * v10 + fx * v11);
            }
        }

        // Apply guided filter for edge refinement using luminance as guide
        array2D<float> guide(width, height);

#ifdef _OPENMP
        #pragma omp parallel for if(multiThread)
#endif
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                guide[y][x] = (0.2126f * rRows[y][x] + 0.7152f * gRows[y][x] + 0.0722f * bRows[y][x]) / 65535.f;
            }
        }

        // Guided filter: refine mask edges using image luminance as guide.
        // Use moderate epsilon (0.1) to preserve fine detail like vegetation edges
        // while still cleaning up noise. Too small epsilon (0.01) aggressively snaps
        // to luminance edges and cuts off thin features.
        guidedFilter(guide, result[c], result[c], 6, 0.1f, multiThread);
    }

    // Debug: dump argmax class map as PPM to /tmp/aimask_debug.ppm
    {
        FILE* fp = fopen("/tmp/aimask_debug.ppm", "wb");
        if (fp) {
            fprintf(fp, "P6\n%d %d\n255\n", width, height);
            // Color map: BG=gray, Person=red, Sky=blue, Vegetation=green,
            // Building=brown, Vehicle=yellow, Animal=magenta, FG=cyan
            const unsigned char colors[][3] = {
                {128, 128, 128}, // BACKGROUND
                {255, 0, 0},     // PERSON
                {0, 100, 255},   // SKY
                {0, 200, 0},     // VEGETATION
                {160, 82, 45},   // BUILDING
                {255, 255, 0},   // VEHICLE
                {255, 0, 255},   // ANIMAL
                {0, 255, 255}    // FOREGROUND_OBJECT
            };
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    int bestClass = 0;
                    float bestProb = result[0][y][x];
                    for (int c = 1; c < numClasses; ++c) {
                        if (result[c][y][x] > bestProb) {
                            bestProb = result[c][y][x];
                            bestClass = c;
                        }
                    }
                    fwrite(colors[bestClass], 1, 3, fp);
                }
            }
            fclose(fp);
            fprintf(stderr, "AI Masking: Debug mask saved to /tmp/aimask_debug.ppm (%dx%d)\n", width, height);
        }

        // Also dump individual class probability maps
        const char* classNames[] = {"bg", "person", "sky", "veg", "building", "vehicle", "animal", "fg"};
        for (int c = 0; c < numClasses; ++c) {
            char fname[256];
            snprintf(fname, sizeof(fname), "/tmp/aimask_%s.pgm", classNames[c]);
            FILE* f = fopen(fname, "wb");
            if (f) {
                fprintf(f, "P5\n%d %d\n255\n", width, height);
                for (int y = 0; y < height; ++y) {
                    for (int x = 0; x < width; ++x) {
                        unsigned char val = static_cast<unsigned char>(std::min(255.f, result[c][y][x] * 255.f));
                        fwrite(&val, 1, 1, f);
                    }
                }
                fclose(f);
            }
        }
        fprintf(stderr, "AI Masking: Individual class masks saved to /tmp/aimask_*.pgm\n");
    }

    return result;
}

// Singleton
static AISegmentationEngine* s_engine = nullptr;

AISegmentationEngine& getAISegmentationEngine()
{
    if (!s_engine) {
        s_engine = new AISegmentationEngine();
    }
    return *s_engine;
}

} // namespace rtengine

#endif // RT_AI_MASKING
