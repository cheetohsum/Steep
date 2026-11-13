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

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "array2D.h"
#include "aisegmentation.h"
#include "maskpaint.h"
#include "rtgui/threadutils.h"

namespace rtengine
{

class Imagefloat;
struct PartnerDepth;

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
    // A stand-in while this photo's maps are still being computed (see
    // AIMaskCache::restoreOrBeginPending): all zero, so a spot waiting for its
    // mask edits nothing. Not a real mask -- never offer it for refinement.
    bool pending = false;

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
                      const std::string& workingProfile, bool multiThread,
                      std::uint64_t inputKey = 0);

    float getMaskValue(AISegClass cls, int y, int x) const;

    // A shared snapshot stays valid if another image replaces the active cache.
    AIMaskSnapshot getMaskSnapshot(AISegClass cls) const;

    // Return a ready-to-sample mask. Costly blur, guided refinement,
    // edge sizing, spatial feathering and inversion are cached per settings tuple.
    AIMaskSnapshot getPreparedMask(AISegClass cls,
                                   float threshold, float feather, float blur,
                                   float maskSize, bool invert,
                                   int refineRadius, float refineEps,
                                   const MaskPaint& paint,
                                   bool multiThread,
                                   const std::array<double, 4>& depthRange = automaticDepthRange());

    // Depth classes select a band of the photo's depth scale: 0 at the
    // nearest thing in the frame, 1 at the farthest, in equal steps of
    // distance *ratio* (halfway is not "half as far" but the geometric middle,
    // the way a lens's focus scale is marked). A range is {a, b, c, d}: full
    // weight from the near limit b to the far limit c, fading to nothing
    // over the feather on either side (a = b - feather, d = c + feather, so
    // a may be below 0 and d above 1).
    //
    // The range a spot keeps when it has none of its own is all -1, meaning
    // "near" or "far" of this photo's own depth split.
    static const std::array<double, 4>& automaticDepthRange()
    {
        static const std::array<double, 4> automatic{{-1.0, -1.0, -1.0, -1.0}};
        return automatic;
    }
    static bool isAutomaticDepthRange(const std::array<double, 4>& range)
    {
        return range[3] < 0.0;
    }
    // Near limit, far limit and feather (each 0..1; feather at most
    // kMaxDepthFeather) to a range and back.
    static constexpr double kMaxDepthFeather = 0.5;
    static constexpr double kDefaultDepthFeather = 0.10;
    static std::array<double, 4> makeDepthRange(double nearLimit, double farLimit, double feather);
    static void splitDepthRange(const std::array<double, 4>& range, double& nearLimit, double& farLimit, double& feather);
    // "a;b;c;d" (LocallabSpot::aiMaskDepthRange) to a range; automatic when
    // empty or malformed.
    static std::array<double, 4> parseDepthRange(const std::string& text);
    static std::string formatDepthRange(const std::array<double, 4>& range);
    // The handles an automatic near / far spot uses on this photo, for the
    // depth range control to start from. False until depth is known.
    bool getDepthPreset(const std::string& imageId, int classIndex, std::array<double, 4>& range) const;

    bool hasCachedMasks(const std::string& imageId) const;
    // Only masks computed from the same kind of input (see aiMaskGeometryKey).
    bool hasCachedMasks(const std::string& imageId, std::uint64_t inputKey) const;
    bool hasCachedMasks() const;

    // FOREGROUND_DEPTH / BACKGROUND_DEPTH: empty until a mask needs them,
    // then split from the scene depth map (requestAIMasks / ensureAIMasks
    // with needDepth). Their coverage reads as unknown until then.
    bool hasDepthClasses(const std::string& imageId, std::uint64_t inputKey) const;
    void setDepthClasses(const std::string& imageId, std::uint64_t inputKey, const PartnerDepth& depth);

    /** The editor's non-blocking path (requestAIMasks), in one lock: true
     *  when this photo's maps are already active or come back from the
     *  archive. Otherwise the photo becomes the cache's image with no maps
     *  yet, and until computeMasks stores them getPreparedMask answers with
     *  an all-zero placeholder (AIMaskSnapshot::pending): a spot waiting for
     *  its mask edits nothing, rather than everything, and never draws the
     *  previous photo's maps. @p discardInFlight drops a computation already
     *  running for another photo, whose result would otherwise land on top. */
    bool restoreOrBeginPending(const std::string& imageId,
                               int width, int height, int fullW, int fullH,
                               const std::string& workingProfile,
                               std::uint64_t inputKey, bool discardInFlight);
    // Back to "no maps" once a computation for this photo has failed.
    void clearPending(const std::string& imageId, std::uint64_t inputKey);
    bool isPending(const std::string& imageId, std::uint64_t inputKey) const;

    /** Fraction of pixels (0..1) whose probability for the class passes
     *  @p threshold — the same threshold the mask itself is built at, so the
     *  number answers "how much of this picture would this class select?"
     *  rather than a fixed 0.5 that a mask at another tolerance disagrees
     *  with. Returns -1 when the cache holds a different image (or nothing) —
     *  callers must treat that as "unknown", not "absent". */
    float getClassCoverage(const std::string& imageId, int classIndex,
                           float threshold = 0.3f) const;

    /** The class to mask when the user clicks full-image point (x, y):
     *  the model class with the highest mean probability in a small
     *  neighbourhood of the point (~1% of the long mask edge), upgraded to
     *  SUBJECT when the point sits inside the composed subject. Returns -1
     *  when the cache holds a different image or the point is outside. */
    int getDominantClassAt(const std::string& imageId, int fullX, int fullY) const;

    void invalidate(const std::string& imageId);
    void invalidateAll();
    /** Memory trimmer. Level 1 drops the maps archived for photos left
     *  behind; level 2 also the masks derived for the current one, which
     *  are rebuilt from its maps on demand. The current photo's model output
     *  stays: without it its masks would go blank. Skipped while busy. */
    void trimMemory(int level);

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
        unsigned int paint;   // hash of the hand-painted corrections
        int depthRange[4];    // depth classes' handles, quantized (-1000 = automatic)

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
    std::uint64_t cachedInputKey_ = 0;
    bool cachedDepthReady_ = false;
    float cachedDepthSplit_ = 0.5f;
    std::string cachedDiskPath_;
    // Whether the subject model was loaded when these maps were computed. It
    // arrives on a worker some seconds after startup, and maps composed
    // without it must not be reused once it is there.
    bool cachedSubjectModel_ = false;
    // The cache's image is waiting for its maps; see restoreOrBeginPending.
    bool pending_ = false;
    std::shared_ptr<const std::vector<array2D<float>>> cachedMasks_;
    std::shared_ptr<const array2D<float>> cachedGuide_;
    std::shared_ptr<const Imagefloat> cachedSamSource_;
    std::vector<float> coverage_;
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

    // Model output of the last few images left behind, so stepping A -> B -> A
    // restores A's maps instead of running segmentation again (~30 MB each;
    // the derived prepared/refined/distance caches are rebuilt on demand).
    struct ArchivedImage
    {
        std::string imageId;
        std::string workingProfile;
        std::uint64_t inputKey = 0;
        bool depthReady = false;
        float depthSplit = 0.5f;
        std::string diskPath;
        bool subjectModel = false;
        std::shared_ptr<const std::vector<array2D<float>>> masks;
        std::shared_ptr<const array2D<float>> guide;
        std::shared_ptr<const Imagefloat> samSource;
        std::vector<float> coverage;
        int sourceWidth = 0;
        int sourceHeight = 0;
        int cachedWidth = 0;
        int cachedHeight = 0;
        int fullW = 0;
        int fullH = 0;
    };
    static constexpr std::size_t kArchivedImages = 2;
    std::deque<ArchivedImage> archive_;
    void archiveCurrentLocked_();
    bool restoreArchivedLocked_(const std::string& imageId,
                                int width, int height, int fullW, int fullH,
                                const std::string& workingProfile,
                                std::uint64_t inputKey, bool subjectModel);
    bool requestStale_(std::uint64_t request) const;
};

} // namespace rtengine

#endif // RT_AI_MASKING
