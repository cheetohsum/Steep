/*
 *  This file is part of RawTherapee.
 *
 *  RawTherapee is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */
#pragma once

#ifdef RT_AI_MASKING

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "array2D.h"
#include "aisegmentation.h"
#include "rtgui/threadutils.h"

namespace rtengine
{

struct AIMaskSnapshot
{
    std::shared_ptr<const array2D<float>> mask;
    int width = 0;
    int height = 0;
    int fullWidth = 0;
    int fullHeight = 0;
    int maskX0 = 0;
    int maskY0 = 0;
    int maskX1 = 0;
    int maskY1 = 0;

    explicit operator bool() const { return mask && width > 0 && height > 0; }
    bool hasBounds() const { return maskX1 > maskX0 && maskY1 > maskY0; }
};

class AIMaskCache
{
public:
    static AIMaskCache& getInstance();

    void computeMasks(const std::string& imageId,
                      float* const* rRows, float* const* gRows, float* const* bRows,
                      int width, int height, int fullW, int fullH,
                      const std::string& workingProfile, bool multiThread);

    float getMaskValue(AISegClass cls, int y, int x) const;

    // A shared snapshot stays valid if another image replaces the active cache.
    AIMaskSnapshot getMaskSnapshot(AISegClass cls) const;

    // Return a ready-to-sample mask. Costly blur, guided refinement,
    // edge sizing, spatial feathering and inversion are cached per settings tuple.
    AIMaskSnapshot getPreparedMask(AISegClass cls,
                                   float threshold, float feather, float blur,
                                   float maskSize, bool invert,
                                   int refineRadius, float refineEps,
                                   bool multiThread);

    bool hasCachedMasks(const std::string& imageId) const;
    bool hasCachedMasks() const;

    void invalidate(const std::string& imageId);
    void invalidateAll();

    int getCachedWidth() const;
    int getCachedHeight() const;
    int getFullWidth() const;
    int getFullHeight() const;

private:
    AIMaskCache();

    struct PreparedKey
    {
        int classIndex;
        int threshold;
        int feather;
        int blur;
        int maskSize;
        int invert;
        int refineRadius;
        int refineEps;

        bool operator==(const PreparedKey& other) const;
    };

    struct PreparedEntry
    {
        PreparedKey key;
        std::uint64_t generation;
        std::shared_ptr<const array2D<float>> mask;
        int maskX0;
        int maskY0;
        int maskX1;
        int maskY1;
    };

    struct RefinedKey
    {
        int classIndex;
        int blur;
        int refineRadius;
        int refineEps;

        bool operator==(const RefinedKey& other) const;
    };

    struct RefinedEntry
    {
        RefinedKey key;
        std::uint64_t generation;
        std::shared_ptr<const array2D<float>> mask;
    };

    struct DistanceKey
    {
        int classIndex;
        int threshold;
        int blur;
        int refineRadius;
        int refineEps;

        bool operator==(const DistanceKey& other) const;
    };

    struct DistanceEntry
    {
        DistanceKey key;
        std::uint64_t generation;
        std::shared_ptr<const array2D<float>> signedDistance;
    };

    mutable MyMutex mutex_;
    std::string cachedImageId_;
    std::string cachedWorkingProfile_;
    std::shared_ptr<const std::vector<array2D<float>>> cachedMasks_;
    std::shared_ptr<const array2D<float>> cachedGuide_;
    std::deque<RefinedEntry> refinedMasks_;
    std::deque<DistanceEntry> distanceMasks_;
    std::deque<PreparedEntry> preparedMasks_;
    int sourceWidth_;
    int sourceHeight_;
    int cachedWidth_;
    int cachedHeight_;
    int fullW_;
    int fullH_;
    std::uint64_t generation_ = 0;
    std::uint64_t requestGeneration_ = 0;
};

} // namespace rtengine

#endif // RT_AI_MASKING
