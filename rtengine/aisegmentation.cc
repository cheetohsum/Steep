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
#include "opthelper.h"
#include "rt_math.h"

#include "onnxruntime_compat.h"
#ifdef RT_AI_MASKING_DIRECTML
#include <dml_provider_factory.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

namespace rtengine
{

namespace
{

// ADE20K class mapping to AISegClass (150 classes)
// Channel order is the standard ADE20K one (0=wall, 2=sky, 4=tree,
// 12=person). Verified against the model actually shipped -- see
// rtdata/models/README.md, which records how it was exported and checked.
AISegClass adeToAIClass(int adeClass)
{
    switch (adeClass) {
        // People and clothing
        case 12: // person
        case 92: // apparel
            return AISegClass::PERSON;

        // Sky
        case 2: // sky
            return AISegClass::SKY;

        // Natural vegetation and terrain
        case 4:  // tree
        case 9:  // grass
        case 16: // mountain
        case 17: // plant
        case 29: // field
        case 66: // flower
        case 68: // hill
        case 72: // palm
        case 94: // land
            return AISegClass::VEGETATION;

        // Architecture and permanent structures
        case 0:  // wall
        case 1:  // building
        case 5:  // ceiling
        case 8:  // windowpane
        case 14: // door
        case 25: // house
        case 32: // fence
        case 38: // railing
        case 40: // base
        case 42: // column
        case 48: // skyscraper
        case 53: // stairs
        case 58: // screen door
        case 59: // stairway
        case 61: // bridge
        case 79: // hovel
        case 84: // tower
        case 86: // awning
        case 88: // booth
        case 95: // bannister
        case 106: // canopy
        case 121: // step
        case 140: // pier
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
        case 122: // tank
        case 127: // bicycle
            return AISegClass::VEHICLE;

        // Animal
        case 126: // animal
            return AISegClass::ANIMAL;

        // Foreground objects: furniture, appliances and portable subjects
        case 7:  // bed
        case 10: // cabinet
        case 15: // table
        case 18: // curtain
        case 19: // chair
        case 22: // painting
        case 23: // sofa
        case 24: // shelf
        case 27: // mirror
        case 28: // rug
        case 30: // armchair
        case 31: // seat
        case 33: // desk
        case 35: // wardrobe
        case 36: // lamp
        case 37: // bathtub
        case 39: // cushion
        case 41: // box
        case 43: // signboard
        case 44: // chest of drawers
        case 45: // counter
        case 47: // sink
        case 49: // fireplace
        case 50: // refrigerator
        case 55: // case
        case 56: // pool table
        case 57: // pillow
        case 62: // bookcase
        case 63: // blind
        case 64: // coffee table
        case 65: // toilet
        case 67: // book
        case 69: // bench
        case 70: // countertop
        case 71: // stove
        case 73: // kitchen island
        case 74: // computer
        case 75: // swivel chair
        case 77: // bar
        case 78: // arcade machine
        case 81: // towel
        case 82: // light
        case 85: // chandelier
        case 87: // streetlight
        case 89: // television receiver
        case 93: // pole
        case 97: // ottoman
        case 98: // bottle
        case 99: // buffet
        case 100: // poster
        case 101: // stage
        case 104: // fountain
        case 105: // conveyor belt
        case 107: // washer
        case 108: // plaything
        case 110: // stool
        case 111: // barrel
        case 112: // basket
        case 114: // tent
        case 115: // bag
        case 117: // cradle
        case 118: // oven
        case 119: // ball
        case 120: // food
        case 123: // trade name
        case 124: // microwave
        case 125: // pot
        case 129: // dishwasher
        case 130: // screen
        case 131: // blanket
        case 132: // sculpture
        case 133: // hood
        case 134: // sconce
        case 135: // vase
        case 136: // traffic light
        case 137: // tray
        case 138: // ashcan
        case 139: // fan
        case 141: // CRT screen
        case 142: // plate
        case 143: // monitor
        case 144: // bulletin board
        case 145: // shower
        case 146: // radiator
        case 147: // glass
        case 148: // clock
        case 149: // flag
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
    bool usingDirectML;

    Impl() : api(nullptr), env(nullptr), session(nullptr),
             sessionOptions(nullptr), memoryInfo(nullptr), initialized(false),
             usingDirectML(false)
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

    pImpl->api->SetSessionGraphOptimizationLevel(pImpl->sessionOptions, ORT_ENABLE_ALL);

#ifdef RT_AI_MASKING_DIRECTML
    pImpl->api->DisableMemPattern(pImpl->sessionOptions);
    pImpl->api->DisableCpuMemArena(pImpl->sessionOptions);
    pImpl->api->SetSessionExecutionMode(pImpl->sessionOptions, ORT_SEQUENTIAL);
    status = OrtSessionOptionsAppendExecutionProvider_DML(pImpl->sessionOptions, 0);
    if (status) {
        fprintf(stderr, "AI Masking: DirectML unavailable (%s), using CPU\n",
                pImpl->api->GetErrorMessage(status));
        pImpl->api->ReleaseStatus(status);
    } else {
        pImpl->usingDirectML = true;
        fprintf(stderr, "AI Masking: using DirectML execution provider\n");
    }
#endif

    if (!pImpl->usingDirectML) {
        const unsigned int hardwareThreads = std::thread::hardware_concurrency();
        const int inferenceThreads = static_cast<int>(
            std::max(1u, std::min(4u, hardwareThreads > 1 ? hardwareThreads / 2 : 1u)));
        pImpl->api->SetIntraOpNumThreads(pImpl->sessionOptions, inferenceThreads);
    }

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
        case AISegClass::SUBJECT: return "Subject";
        case AISegClass::NOT_SUBJECT: return "Everything but Subject";
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
        const float srcY = std::max(0.f, (y + 0.5f) * scaleY - 0.5f);
        const int sy0 = std::min(static_cast<int>(srcY), height - 1);
        const int sy1 = std::min(sy0 + 1, height - 1);
        const float fy = srcY - sy0;

        for (int x = 0; x < modelW; ++x) {
            const float srcX = std::max(0.f, (x + 0.5f) * scaleX - 0.5f);
            const int sx0 = std::min(static_cast<int>(srcX), width - 1);
            const int sx1 = std::min(sx0 + 1, width - 1);
            const float fx = srcX - sx0;

            const auto sample = [=](float* const* rows) {
                const float top = rows[sy0][sx0] + fx * (rows[sy0][sx1] - rows[sy0][sx0]);
                const float bottom = rows[sy1][sx0] + fx * (rows[sy1][sx1] - rows[sy1][sx0]);
                return top + fy * (bottom - top);
            };

            // Convert linear RGB [0,65535] to sRGB [0,1]
            const float r = linearToSRGB(sample(rRows) / 65535.f);
            const float g = linearToSRGB(sample(gRows) / 65535.f);
            const float b = linearToSRGB(sample(bRows) / 65535.f);

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

    // 3. Postprocess: ADE logits -> softmax -> map to AISegClass -> upscale
    float* outputData = nullptr;
    pImpl->api->GetTensorMutableData(outputOrt, reinterpret_cast<void**>(&outputData));

    if (!outputData) {
        fprintf(stderr, "AI Masking: GetTensorMutableData returned null\n");
        pImpl->api->ReleaseValue(outputOrt);
        for (int c = 0; c < numClasses; ++c) {
            result[c](width, height);
        }
        return result;
    }

    OrtTensorTypeAndShapeInfo* typeInfo = nullptr;
    pImpl->api->GetTensorTypeAndShape(outputOrt, &typeInfo);

    if (!typeInfo) {
        fprintf(stderr, "AI Masking: GetTensorTypeAndShape returned null\n");
        pImpl->api->ReleaseValue(outputOrt);
        for (int c = 0; c < numClasses; ++c) {
            result[c](width, height);
        }
        return result;
    }

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

    std::vector<int> classMap(outClasses);
    for (int c = 0; c < outClasses; ++c) {
        classMap[c] = static_cast<int>(adeToAIClass(c));
    }

    // Process each pixel: one softmax pass directly into the broader Steep classes.
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

            float sumExp = 0.f;
            float groupedExp[static_cast<int>(AISegClass::NUM_CLASSES)] = {};
            for (int c = 0; c < outClasses; ++c) {
                const float probability =
                    std::exp(outputData[c * outH * outW + y * outW + x] - maxVal);
                groupedExp[classMap[c]] += probability;
                sumExp += probability;
            }
            const float inverseSum = sumExp > 0.f ? 1.f / sumExp : 0.f;
            for (int c = 0; c < numClasses; ++c) {
                modelMaps[c][y][x] = groupedExp[c] * inverseSum;
            }
        }
    }

    pImpl->api->ReleaseValue(outputOrt);

    // 4. Upscale probabilities to the preview resolution. Edge-aware refinement is
    // cached later with the user's per-mask settings instead of being repeated here.
    for (int c = 0; c < numClasses; ++c) {
        result[c](width, height);

        const float xScale = static_cast<float>(outW) / width;
        const float yScale = static_cast<float>(outH) / height;

#ifdef _OPENMP
        #pragma omp parallel for if(multiThread)
#endif
        for (int y = 0; y < height; ++y) {
            const float srcYf = std::max(0.f, (y + 0.5f) * yScale - 0.5f);
            const int sy0 = std::min(static_cast<int>(srcYf), outH - 1);
            const int sy1 = std::min(sy0 + 1, outH - 1);
            const float fy = srcYf - sy0;

            for (int x = 0; x < width; ++x) {
                const float srcXf = std::max(0.f, (x + 0.5f) * xScale - 0.5f);
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
