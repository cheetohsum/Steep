/*
 *  This file is part of RawTherapee.
 *
 *  RawTherapee is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#ifdef RT_AI_MASKING

#include "aimaskcache.h"

#include "boxblur.h"
#include "guidedfilter.h"
#include "iccstore.h"
#include "rt_math.h"

#include <algorithm>
#include <cmath>

namespace rtengine
{

namespace
{

constexpr std::size_t MAX_PREPARED_MASKS = 12;
constexpr std::size_t MAX_REFINED_MASKS = 4;
constexpr int MAX_MASK_CACHE_DIMENSION = 1024;

int quantize(float value, float scale)
{
    return static_cast<int>(std::lround(value * scale));
}

} // namespace

AIMaskCache::AIMaskCache()
    : sourceWidth_(0), sourceHeight_(0), cachedWidth_(0), cachedHeight_(0),
      fullW_(0), fullH_(0)
{
}

AIMaskCache& AIMaskCache::getInstance()
{
    static AIMaskCache instance;
    return instance;
}

bool AIMaskCache::PreparedKey::operator==(const PreparedKey& other) const
{
    return classIndex == other.classIndex
        && threshold == other.threshold
        && feather == other.feather
        && blur == other.blur
        && invert == other.invert
        && refineRadius == other.refineRadius
        && refineEps == other.refineEps;
}

bool AIMaskCache::RefinedKey::operator==(const RefinedKey& other) const
{
    return classIndex == other.classIndex
        && blur == other.blur
        && refineRadius == other.refineRadius
        && refineEps == other.refineEps;
}

void AIMaskCache::computeMasks(const std::string& imageId,
                               float* const* rRows, float* const* gRows, float* const* bRows,
                               int width, int height, int fullW, int fullH,
                               const std::string& workingProfile, bool multiThread)
{
    AISegmentationEngine& engine = getAISegmentationEngine();
    if (!engine.isInitialized() || width <= 0 || height <= 0) {
        return;
    }

    std::uint64_t request = 0;
    {
        MyMutex::MyLock lock(mutex_);
        if (cachedImageId_ == imageId
                && cachedWorkingProfile_ == workingProfile
                && sourceWidth_ == width && sourceHeight_ == height
                && fullW_ == fullW && fullH_ == fullH && cachedMasks_) {
            return;
        }
        request = ++requestGeneration_;
    }

    const float cacheScale = std::min(
        1.f,
        static_cast<float>(MAX_MASK_CACHE_DIMENSION) /
            static_cast<float>(std::max(width, height)));
    const int maskWidth = std::max(1, static_cast<int>(std::lround(width * cacheScale)));
    const int maskHeight = std::max(1, static_cast<int>(std::lround(height * cacheScale)));

    array2D<float> scaledR(maskWidth, maskHeight);
    array2D<float> scaledG(maskWidth, maskHeight);
    array2D<float> scaledB(maskWidth, maskHeight);

    const TMatrix workToXYZ = ICCStore::getInstance()->workingSpaceMatrix(workingProfile);
    const TMatrix xyzToSRGB = ICCStore::getInstance()->workingSpaceInverseMatrix("sRGB");
    float workToSRGB[3][3] = {};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            for (int component = 0; component < 3; ++component) {
                workToSRGB[row][column] += static_cast<float>(
                    xyzToSRGB[row][component] * workToXYZ[component][column]);
            }
        }
    }

    const float scaleX = static_cast<float>(width) / maskWidth;
    const float scaleY = static_cast<float>(height) / maskHeight;
#ifdef _OPENMP
    #pragma omp parallel for if(multiThread)
#endif
    for (int y = 0; y < maskHeight; ++y) {
        const float sourceY = std::max(0.f, (y + 0.5f) * scaleY - 0.5f);
        const int y0 = std::min(static_cast<int>(sourceY), height - 1);
        const int y1 = std::min(y0 + 1, height - 1);
        const float fy = sourceY - y0;

        for (int x = 0; x < maskWidth; ++x) {
            const float sourceX = std::max(0.f, (x + 0.5f) * scaleX - 0.5f);
            const int x0 = std::min(static_cast<int>(sourceX), width - 1);
            const int x1 = std::min(x0 + 1, width - 1);
            const float fx = sourceX - x0;

            const auto sample = [=](float* const* rows) {
                const float top = rows[y0][x0] + fx * (rows[y0][x1] - rows[y0][x0]);
                const float bottom = rows[y1][x0] + fx * (rows[y1][x1] - rows[y1][x0]);
                return top + fy * (bottom - top);
            };

            const float workR = sample(rRows);
            const float workG = sample(gRows);
            const float workB = sample(bRows);
            scaledR[y][x] = workToSRGB[0][0] * workR
                + workToSRGB[0][1] * workG + workToSRGB[0][2] * workB;
            scaledG[y][x] = workToSRGB[1][0] * workR
                + workToSRGB[1][1] * workG + workToSRGB[1][2] * workB;
            scaledB[y][x] = workToSRGB[2][0] * workR
                + workToSRGB[2][1] * workG + workToSRGB[2][2] * workB;
        }
    }

    float* const* segmentR = static_cast<float**>(scaledR);
    float* const* segmentG = static_cast<float**>(scaledG);
    float* const* segmentB = static_cast<float**>(scaledB);

    auto masks = std::make_shared<const std::vector<array2D<float>>>(
        engine.segment(segmentR, segmentG, segmentB,
                       maskWidth, maskHeight, multiThread));

    auto guide = std::make_shared<array2D<float>>(maskWidth, maskHeight);
#ifdef _OPENMP
    #pragma omp parallel for if(multiThread)
#endif
    for (int y = 0; y < maskHeight; ++y) {
        for (int x = 0; x < maskWidth; ++x) {
            (*guide)[y][x] = LIM(
                (0.2126f * segmentR[y][x] + 0.7152f * segmentG[y][x]
                    + 0.0722f * segmentB[y][x]) / 65535.f,
                0.f, 1.f);
        }
    }

    MyMutex::MyLock lock(mutex_);
    if (request != requestGeneration_) {
        return;
    }

    cachedMasks_ = std::move(masks);
    cachedGuide_ = std::move(guide);
    cachedImageId_ = imageId;
    cachedWorkingProfile_ = workingProfile;
    sourceWidth_ = width;
    sourceHeight_ = height;
    cachedWidth_ = maskWidth;
    cachedHeight_ = maskHeight;
    fullW_ = fullW;
    fullH_ = fullH;
    refinedMasks_.clear();
    preparedMasks_.clear();
    ++generation_;
}

AIMaskSnapshot AIMaskCache::getMaskSnapshot(AISegClass cls) const
{
    MyMutex::MyLock lock(mutex_);
    const int classIndex = static_cast<int>(cls);
    if (!cachedMasks_ || classIndex < 0
            || classIndex >= static_cast<int>(cachedMasks_->size())) {
        return {};
    }

    AIMaskSnapshot snapshot;
    snapshot.mask = std::shared_ptr<const array2D<float>>(
        cachedMasks_, &cachedMasks_->at(classIndex));
    snapshot.width = cachedWidth_;
    snapshot.height = cachedHeight_;
    snapshot.fullWidth = fullW_;
    snapshot.fullHeight = fullH_;
    return snapshot;
}

AIMaskSnapshot AIMaskCache::getPreparedMask(
    AISegClass cls, float threshold, float feather, float blur, bool invert,
    int refineRadius, float refineEps, bool multiThread)
{
    const PreparedKey key {
        static_cast<int>(cls),
        quantize(threshold, 10000.f),
        quantize(feather, 100.f),
        quantize(blur, 100.f),
        invert ? 1 : 0,
        refineRadius,
        quantize(refineEps, 1000000.f)
    };
    const RefinedKey refinedKey {
        key.classIndex, key.blur, key.refineRadius, key.refineEps
    };

    std::shared_ptr<const array2D<float>> baseMask;
    std::shared_ptr<const array2D<float>> guide;
    bool hasRefinedMask = false;
    std::uint64_t generation = 0;
    int width = 0;
    int height = 0;
    int fullWidth = 0;
    int fullHeight = 0;

    {
        MyMutex::MyLock lock(mutex_);
        if (!cachedMasks_ || key.classIndex < 0
                || key.classIndex >= static_cast<int>(cachedMasks_->size())) {
            return {};
        }

        for (auto it = preparedMasks_.rbegin(); it != preparedMasks_.rend(); ++it) {
            if (it->generation == generation_ && it->key == key) {
                return {it->mask, cachedWidth_, cachedHeight_, fullW_, fullH_};
            }
        }

        for (auto it = refinedMasks_.rbegin(); it != refinedMasks_.rend(); ++it) {
            if (it->generation == generation_ && it->key == refinedKey) {
                baseMask = it->mask;
                hasRefinedMask = true;
                break;
            }
        }

        if (!baseMask) {
            baseMask = std::shared_ptr<const array2D<float>>(
                cachedMasks_, &cachedMasks_->at(key.classIndex));
        }
        guide = cachedGuide_;
        generation = generation_;
        width = cachedWidth_;
        height = cachedHeight_;
        fullWidth = fullW_;
        fullHeight = fullH_;
    }

    auto prepared = std::make_shared<array2D<float>>(*baseMask);

    if (!hasRefinedMask) {
        const float previewScale = fullWidth > 0
            ? static_cast<float>(width) / static_cast<float>(fullWidth)
            : 1.f;
        const int blurRadius = std::max(0, static_cast<int>(std::lround(blur * previewScale)));
        if (blurRadius > 0) {
            auto blurred = std::make_shared<array2D<float>>(width, height);
            boxblur(static_cast<float**>(*prepared), static_cast<float**>(*blurred),
                    blurRadius, width, height, multiThread);
            prepared = std::move(blurred);
        }

        if (guide && refineRadius > 0) {
            const int maxRadius = std::max(1, (std::min(width, height) - 1) / 2 - 1);
            const int radius = std::min(refineRadius, maxRadius);
            auto refined = std::make_shared<array2D<float>>(width, height);
            guidedFilter(*guide, *prepared, *refined, radius,
                         LIM(refineEps, 0.0001f, 0.5f), multiThread);
            prepared = std::move(refined);
        }

        const std::shared_ptr<const array2D<float>> refinedMask = prepared;
        {
            MyMutex::MyLock lock(mutex_);
            if (generation == generation_) {
                refinedMasks_.push_back({refinedKey, generation, refinedMask});
                while (refinedMasks_.size() > MAX_REFINED_MASKS) {
                    refinedMasks_.pop_front();
                }
            }
        }
        prepared = std::make_shared<array2D<float>>(*refinedMask);
    }

    const float safeThreshold = LIM(threshold, 0.f, 1.f);
    const float halfWidth = std::max(0.001f, feather * 0.003f);
#ifdef _OPENMP
    #pragma omp parallel for if(multiThread)
#endif
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float value = LIM(
                ((*prepared)[y][x] - safeThreshold + halfWidth) / (2.f * halfWidth),
                0.f, 1.f);
            if (invert) {
                value = 1.f - value;
            }
            (*prepared)[y][x] = value < 0.001f ? 0.f : (value > 0.999f ? 1.f : value);
        }
    }

    const std::shared_ptr<const array2D<float>> readyMask = prepared;
    {
        MyMutex::MyLock lock(mutex_);
        if (generation == generation_) {
            preparedMasks_.push_back({key, generation, readyMask});
            while (preparedMasks_.size() > MAX_PREPARED_MASKS) {
                preparedMasks_.pop_front();
            }
        }
    }

    return {readyMask, width, height, fullWidth, fullHeight};
}

float AIMaskCache::getMaskValue(AISegClass cls, int y, int x) const
{
    const AIMaskSnapshot snapshot = getMaskSnapshot(cls);
    if (!snapshot || y < 0 || y >= snapshot.height || x < 0 || x >= snapshot.width) {
        return 0.f;
    }
    return (*snapshot.mask)[y][x];
}

bool AIMaskCache::hasCachedMasks(const std::string& imageId) const
{
    MyMutex::MyLock lock(mutex_);
    return cachedImageId_ == imageId && cachedMasks_ && !cachedMasks_->empty();
}

bool AIMaskCache::hasCachedMasks() const
{
    MyMutex::MyLock lock(mutex_);
    return cachedMasks_ && !cachedMasks_->empty();
}

void AIMaskCache::invalidate(const std::string& imageId)
{
    MyMutex::MyLock lock(mutex_);
    if (cachedImageId_ != imageId) {
        return;
    }
    ++requestGeneration_;
    ++generation_;
    cachedImageId_.clear();
    cachedWorkingProfile_.clear();
    cachedMasks_.reset();
    cachedGuide_.reset();
    refinedMasks_.clear();
    preparedMasks_.clear();
    sourceWidth_ = sourceHeight_ = 0;
    cachedWidth_ = cachedHeight_ = fullW_ = fullH_ = 0;
}

void AIMaskCache::invalidateAll()
{
    MyMutex::MyLock lock(mutex_);
    ++requestGeneration_;
    ++generation_;
    cachedImageId_.clear();
    cachedWorkingProfile_.clear();
    cachedMasks_.reset();
    cachedGuide_.reset();
    refinedMasks_.clear();
    preparedMasks_.clear();
    sourceWidth_ = sourceHeight_ = 0;
    cachedWidth_ = cachedHeight_ = fullW_ = fullH_ = 0;
}

int AIMaskCache::getCachedWidth() const
{
    MyMutex::MyLock lock(mutex_);
    return cachedWidth_;
}

int AIMaskCache::getCachedHeight() const
{
    MyMutex::MyLock lock(mutex_);
    return cachedHeight_;
}

int AIMaskCache::getFullWidth() const
{
    MyMutex::MyLock lock(mutex_);
    return fullW_;
}

int AIMaskCache::getFullHeight() const
{
    MyMutex::MyLock lock(mutex_);
    return fullH_;
}

} // namespace rtengine

#endif // RT_AI_MASKING
