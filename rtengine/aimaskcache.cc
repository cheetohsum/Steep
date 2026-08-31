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
#include "edittrace.h"
#include "guidedfilter.h"
#include "iccstore.h"
#include "rt_math.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace rtengine
{

namespace
{

constexpr std::size_t MAX_PREPARED_MASKS = 12;
constexpr std::size_t MAX_REFINED_MASKS = 4;
constexpr std::size_t MAX_DISTANCE_MASKS = 4;
constexpr int MAX_MASK_CACHE_DIMENSION = 1024;
constexpr float DISTANCE_INFINITY = 1.e20f;

int quantize(float value, float scale)
{
    return static_cast<int>(std::lround(value * scale));
}

/** Compose the SUBJECT pseudo-class from the model output: the union of the
 *  person/vehicle/animal/foreground-object probabilities, cut down to its
 *  dominant connected regions (every region at least 30% the size of the
 *  largest, so a second person in a group shot survives), with interior
 *  holes filled. NOT_SUBJECT is its complement. Both are appended to @p maps
 *  in AISegClass order.
 */
void appendSubjectMasks(std::vector<array2D<float>>& maps, int width, int height, bool multiThread)
{
    if (static_cast<int>(maps.size()) != static_cast<int>(AISegClass::NUM_CLASSES)
            || width <= 0 || height <= 0) {
        return;
    }

    maps.reserve(static_cast<std::size_t>(AISegClass::TOTAL_CLASSES));

    const array2D<float>& person = maps[static_cast<int>(AISegClass::PERSON)];
    const array2D<float>& vehicle = maps[static_cast<int>(AISegClass::VEHICLE)];
    const array2D<float>& animal = maps[static_cast<int>(AISegClass::ANIMAL)];
    const array2D<float>& object = maps[static_cast<int>(AISegClass::FOREGROUND_OBJECT)];

    maps.emplace_back(width, height);
    array2D<float>& subject = maps[static_cast<int>(AISegClass::SUBJECT)];

#ifdef _OPENMP
    #pragma omp parallel for if(multiThread)
#endif
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            subject[y][x] = std::max(std::max(person[y][x], vehicle[y][x]),
                                     std::max(animal[y][x], object[y][x]));
        }
    }

    // The support threshold matches the default mask threshold, so the soft
    // probability skirt around a kept region survives into the composed map.
    constexpr float support = 0.3f;

    // Connected components over the support region (4-neighborhood).
    const int n = width * height;
    std::vector<int> label(n, 0);
    std::vector<int> stack;
    std::vector<int> sizes(1, 0); // 1-based; sizes[0] unused
    int labelCount = 0;

    for (int seed = 0; seed < n; ++seed) {
        if (label[seed] != 0 || subject[seed / width][seed % width] <= support) {
            continue;
        }

        ++labelCount;
        int size = 0;
        stack.assign(1, seed);
        label[seed] = labelCount;

        while (!stack.empty()) {
            const int p = stack.back();
            stack.pop_back();
            ++size;
            const int y = p / width;
            const int x = p % width;

            const auto visit = [&](int q) {
                if (label[q] == 0 && subject[q / width][q % width] > support) {
                    label[q] = labelCount;
                    stack.push_back(q);
                }
            };
            if (y > 0) visit(p - width);
            if (y < height - 1) visit(p + width);
            if (x > 0) visit(p - 1);
            if (x < width - 1) visit(p + 1);
        }

        sizes.push_back(size);
    }

    if (labelCount > 0) {
        int largest = 0;
        for (int l = 1; l <= labelCount; ++l) {
            largest = std::max(largest, sizes[l]);
        }
        const int keepAbove = std::max(1, largest * 3 / 10);
        std::vector<char> keep(labelCount + 1, 0);
        for (int l = 1; l <= labelCount; ++l) {
            keep[l] = sizes[l] >= keepAbove ? 1 : 0;
        }

        const auto kept = [&](int p) {
            return label[p] != 0 && keep[label[p]];
        };

        // Flood the true outside (non-kept pixels reachable from the border);
        // what remains un-flooded and non-kept is an interior hole.
        std::vector<char> outside(n, 0);
        stack.clear();
        const auto seedOutside = [&](int p) {
            if (!outside[p] && !kept(p)) {
                outside[p] = 1;
                stack.push_back(p);
            }
        };
        for (int x = 0; x < width; ++x) {
            seedOutside(x);
            seedOutside(n - width + x);
        }
        for (int y = 0; y < height; ++y) {
            seedOutside(y * width);
            seedOutside(y * width + width - 1);
        }
        while (!stack.empty()) {
            const int p = stack.back();
            stack.pop_back();
            const int y = p / width;
            const int x = p % width;
            if (y > 0) seedOutside(p - width);
            if (y < height - 1) seedOutside(p + width);
            if (x > 0) seedOutside(p - 1);
            if (x < width - 1) seedOutside(p + 1);
        }

        for (int p = 0; p < n; ++p) {
            if (!kept(p)) {
                // Interior holes join the subject; stray blobs and noise go.
                subject[p / width][p % width] = outside[p] ? 0.f : 1.f;
            }
        }
    }

    maps.emplace_back(width, height);
    array2D<float>& notSubject = maps[static_cast<int>(AISegClass::NOT_SUBJECT)];

#ifdef _OPENMP
    #pragma omp parallel for if(multiThread)
#endif
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            notSubject[y][x] = LIM(1.f - subject[y][x], 0.f, 1.f);
        }
    }
}

void squaredDistanceTransform1D(const float* source, float* target, int length,
                                std::vector<int>& locations,
                                std::vector<float>& separators)
{
    int envelopeSize = -1;
    for (int q = 0; q < length; ++q) {
        if (source[q] >= DISTANCE_INFINITY * 0.5f) {
            continue;
        }

        float separator = -DISTANCE_INFINITY;
        while (envelopeSize >= 0) {
            const int p = locations[envelopeSize];
            separator = ((source[q] + static_cast<float>(q * q))
                         - (source[p] + static_cast<float>(p * p)))
                / static_cast<float>(2 * (q - p));
            if (separator > separators[envelopeSize]) {
                break;
            }
            --envelopeSize;
        }

        ++envelopeSize;
        locations[envelopeSize] = q;
        separators[envelopeSize] = envelopeSize == 0
            ? -DISTANCE_INFINITY
            : separator;
        separators[envelopeSize + 1] = DISTANCE_INFINITY;
    }

    if (envelopeSize < 0) {
        std::fill(target, target + length, DISTANCE_INFINITY);
        return;
    }

    int envelope = 0;
    for (int q = 0; q < length; ++q) {
        while (separators[envelope + 1] < q) {
            ++envelope;
        }
        const int delta = q - locations[envelope];
        target[q] = static_cast<float>(delta * delta)
            + source[locations[envelope]];
    }
}

std::shared_ptr<array2D<float>> distanceToRegion(
    const array2D<float>& probabilities, float threshold, bool featureInside,
    int width, int height, bool multiThread)
{
    auto horizontal = std::make_shared<array2D<float>>(width, height);
    auto distance = std::make_shared<array2D<float>>(width, height);

#ifdef _OPENMP
    #pragma omp parallel if(multiThread)
#endif
    {
        std::vector<float> source(width);
        std::vector<int> locations(width);
        std::vector<float> separators(width + 1);
#ifdef _OPENMP
        #pragma omp for
#endif
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const bool inside = probabilities[y][x] >= threshold;
                source[x] = inside == featureInside ? 0.f : DISTANCE_INFINITY;
            }
            squaredDistanceTransform1D(source.data(), (*horizontal)[y], width,
                                       locations, separators);
        }
    }

#ifdef _OPENMP
    #pragma omp parallel if(multiThread)
#endif
    {
        std::vector<float> source(height);
        std::vector<float> target(height);
        std::vector<int> locations(height);
        std::vector<float> separators(height + 1);
#ifdef _OPENMP
        #pragma omp for
#endif
        for (int x = 0; x < width; ++x) {
            for (int y = 0; y < height; ++y) {
                source[y] = (*horizontal)[y][x];
            }
            squaredDistanceTransform1D(source.data(), target.data(), height,
                                       locations, separators);
            for (int y = 0; y < height; ++y) {
                (*distance)[y][x] = target[y];
            }
        }
    }

    return distance;
}

std::shared_ptr<array2D<float>> buildSignedDistance(
    const array2D<float>& probabilities, float threshold,
    int width, int height, bool multiThread)
{
    auto distanceInside = distanceToRegion(
        probabilities, threshold, true, width, height, multiThread);
    auto distanceOutside = distanceToRegion(
        probabilities, threshold, false, width, height, multiThread);
    auto signedDistance = std::make_shared<array2D<float>>(width, height);

#ifdef _OPENMP
    #pragma omp parallel for if(multiThread)
#endif
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const bool inside = probabilities[y][x] >= threshold;
            (*signedDistance)[y][x] = inside
                ? std::sqrt((*distanceOutside)[y][x]) - 0.5f
                : 0.5f - std::sqrt((*distanceInside)[y][x]);
        }
    }

    return signedDistance;
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
        && maskSize == other.maskSize
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

bool AIMaskCache::DistanceKey::operator==(const DistanceKey& other) const
{
    return classIndex == other.classIndex
        && threshold == other.threshold
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

    const long long traceStartUs = edittrace::enabled() ? edittrace::nowUs() : 0;

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

    const long long traceSegmentUs = edittrace::enabled() ? edittrace::nowUs() : 0;
    std::vector<array2D<float>> maps = engine.segment(segmentR, segmentG, segmentB,
                                                      maskWidth, maskHeight, multiThread);
    if (edittrace::enabled()) {
        edittrace::logf("[aiMask] segment img=%s src=%dx%d mask=%dx%d infer=%.0fms",
                        imageId.c_str(), width, height, maskWidth, maskHeight,
                        (edittrace::nowUs() - traceSegmentUs) / 1000.0);
    }

    appendSubjectMasks(maps, maskWidth, maskHeight, multiThread);

    auto masks = std::make_shared<const std::vector<array2D<float>>>(std::move(maps));

    std::vector<float> coverage(masks->size(), 0.f);
    {
        const float total = static_cast<float>(maskWidth) * static_cast<float>(maskHeight);
        for (size_t c = 0; c < masks->size(); ++c) {
            const array2D<float>& map = masks->at(c);
            int hits = 0;
#ifdef _OPENMP
            #pragma omp parallel for reduction(+:hits) if(multiThread)
#endif
            for (int y = 0; y < maskHeight; ++y) {
                for (int x = 0; x < maskWidth; ++x) {
                    if (map[y][x] > 0.5f) {
                        ++hits;
                    }
                }
            }
            coverage[c] = total > 0.f ? hits / total : 0.f;
        }
    }
    if (edittrace::enabled() && coverage.size() >= 10) {
        edittrace::logf("[aiMask] coverage bg=%d%% person=%d%% sky=%d%% veg=%d%% bldg=%d%% veh=%d%% animal=%d%% fg=%d%% subj=%d%% notsubj=%d%%",
                        static_cast<int>(coverage[0] * 100), static_cast<int>(coverage[1] * 100),
                        static_cast<int>(coverage[2] * 100), static_cast<int>(coverage[3] * 100),
                        static_cast<int>(coverage[4] * 100), static_cast<int>(coverage[5] * 100),
                        static_cast<int>(coverage[6] * 100), static_cast<int>(coverage[7] * 100),
                        static_cast<int>(coverage[8] * 100), static_cast<int>(coverage[9] * 100));
    }

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
        if (edittrace::enabled()) {
            edittrace::logf("[aiMask] compute discarded (stale) img=%s", imageId.c_str());
        }
        return;
    }

    if (edittrace::enabled()) {
        edittrace::logf("[aiMask] compute done img=%s total=%.0fms",
                        imageId.c_str(),
                        (edittrace::nowUs() - traceStartUs) / 1000.0);
    }

    cachedMasks_ = std::move(masks);
    cachedGuide_ = std::move(guide);
    coverage_ = std::move(coverage);
    cachedImageId_ = imageId;
    cachedWorkingProfile_ = workingProfile;
    sourceWidth_ = width;
    sourceHeight_ = height;
    cachedWidth_ = maskWidth;
    cachedHeight_ = maskHeight;
    fullW_ = fullW;
    fullH_ = fullH;
    refinedMasks_.clear();
    distanceMasks_.clear();
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
    AISegClass cls, float threshold, float feather, float blur, float maskSize,
    bool invert, int refineRadius, float refineEps, bool multiThread)
{
    const PreparedKey key {
        static_cast<int>(cls),
        quantize(threshold, 10000.f),
        quantize(feather, 100.f),
        quantize(blur, 100.f),
        quantize(maskSize, 100.f),
        invert ? 1 : 0,
        refineRadius,
        quantize(refineEps, 1000000.f)
    };
    const RefinedKey refinedKey {
        key.classIndex, key.blur, key.refineRadius, key.refineEps
    };
    const DistanceKey distanceKey {
        key.classIndex, key.threshold, key.blur, key.refineRadius, key.refineEps
    };

    std::shared_ptr<const array2D<float>> baseMask;
    std::shared_ptr<const array2D<float>> guide;
    std::shared_ptr<const array2D<float>> signedDistance;
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
                return {it->mask, cachedWidth_, cachedHeight_, fullW_, fullH_,
                        it->maskX0, it->maskY0, it->maskX1, it->maskY1};
            }
        }

        for (auto it = distanceMasks_.rbegin(); it != distanceMasks_.rend(); ++it) {
            if (it->generation == generation_ && it->key == distanceKey) {
                signedDistance = it->signedDistance;
                break;
            }
        }

        if (!signedDistance) {
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
        }
        generation = generation_;
        width = cachedWidth_;
        height = cachedHeight_;
        fullWidth = fullW_;
        fullHeight = fullH_;
    }

    const long long traceStartUs = edittrace::enabled() ? edittrace::nowUs() : 0;
    const bool traceDistanceHit = static_cast<bool>(signedDistance);

    const float safeThreshold = LIM(threshold, 0.f, 1.f);
    if (!signedDistance) {
        auto refinedWorking = std::make_shared<array2D<float>>(*baseMask);

        if (!hasRefinedMask) {
            const float previewScale = fullWidth > 0
                ? static_cast<float>(width) / static_cast<float>(fullWidth)
                : 1.f;
            const int blurRadius = std::max(
                0, static_cast<int>(std::lround(blur * previewScale)));
            if (blurRadius > 0) {
                auto blurred = std::make_shared<array2D<float>>(width, height);
                boxblur(static_cast<float**>(*refinedWorking),
                        static_cast<float**>(*blurred),
                        blurRadius, width, height, multiThread);
                refinedWorking = std::move(blurred);
            }

            if (guide && refineRadius > 0) {
                const int maxRadius = std::max(
                    1, (std::min(width, height) - 1) / 2 - 1);
                const int radius = std::min(refineRadius, maxRadius);
                auto refined = std::make_shared<array2D<float>>(width, height);
                guidedFilter(*guide, *refinedWorking, *refined, radius,
                             LIM(refineEps, 0.0001f, 0.5f), multiThread);
                refinedWorking = std::move(refined);
            }

            const std::shared_ptr<const array2D<float>> refinedMask = refinedWorking;
            MyMutex::MyLock lock(mutex_);
            if (generation == generation_) {
                refinedMasks_.push_back({refinedKey, generation, refinedMask});
                while (refinedMasks_.size() > MAX_REFINED_MASKS) {
                    refinedMasks_.pop_front();
                }
            }
        }

        signedDistance = buildSignedDistance(
            *refinedWorking, safeThreshold, width, height, multiThread);
        MyMutex::MyLock lock(mutex_);
        if (generation == generation_) {
            distanceMasks_.push_back({distanceKey, generation, signedDistance});
            while (distanceMasks_.size() > MAX_DISTANCE_MASKS) {
                distanceMasks_.pop_front();
            }
        }
    }

    const float previewScale = fullWidth > 0 && fullHeight > 0
        ? std::min(static_cast<float>(width) / fullWidth,
                   static_cast<float>(height) / fullHeight)
        : 1.f;
    const float sizeShift = (maskSize - 18.f) * 0.5f * previewScale;
    const float featherRadius = std::max(0.f, feather) * 0.5f * previewScale;
    auto prepared = std::make_shared<array2D<float>>(width, height);

#ifdef _OPENMP
    #pragma omp parallel for if(multiThread)
#endif
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float edgeDistance = (*signedDistance)[y][x] + sizeShift;
            float value = edgeDistance >= 0.f ? 1.f : 0.f;
            if (featherRadius > 0.01f) {
                const float transition = LIM(
                    0.5f + edgeDistance / (2.f * featherRadius), 0.f, 1.f);
                value = transition * transition * (3.f - 2.f * transition);
            }
            if (invert) {
                value = 1.f - value;
            }
            (*prepared)[y][x] = value < 0.001f ? 0.f : (value > 0.999f ? 1.f : value);
        }
    }

    int maskX0 = width;
    int maskY0 = height;
    int maskX1 = 0;
    int maskY1 = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if ((*prepared)[y][x] > 0.f) {
                maskX0 = std::min(maskX0, x);
                maskY0 = std::min(maskY0, y);
                maskX1 = std::max(maskX1, x + 1);
                maskY1 = std::max(maskY1, y + 1);
            }
        }
    }
    if (maskX1 == 0 || maskY1 == 0) {
        maskX0 = maskY0 = maskX1 = maskY1 = 0;
    }

    const std::shared_ptr<const array2D<float>> readyMask = prepared;
    {
        MyMutex::MyLock lock(mutex_);
        if (generation == generation_) {
            preparedMasks_.push_back({key, generation, readyMask,
                                      maskX0, maskY0, maskX1, maskY1});
            while (preparedMasks_.size() > MAX_PREPARED_MASKS) {
                preparedMasks_.pop_front();
            }
        }
    }

    if (edittrace::enabled()) {
        edittrace::logf("[aiMask] prepare class=%d dist=%s refined=%s total=%.1fms",
                        key.classIndex,
                        traceDistanceHit ? "hit" : "miss",
                        hasRefinedMask ? "hit" : "miss",
                        (edittrace::nowUs() - traceStartUs) / 1000.0);
    }

    return {readyMask, width, height, fullWidth, fullHeight,
            maskX0, maskY0, maskX1, maskY1};
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

float AIMaskCache::getClassCoverage(const std::string& imageId, int classIndex) const
{
    MyMutex::MyLock lock(mutex_);
    if (cachedImageId_ != imageId || classIndex < 0
            || classIndex >= static_cast<int>(coverage_.size())) {
        return -1.f;
    }
    return coverage_[classIndex];
}

int AIMaskCache::getDominantClassAt(const std::string& imageId, int fullX, int fullY) const
{
    MyMutex::MyLock lock(mutex_);
    if (cachedImageId_ != imageId || !cachedMasks_ || cachedMasks_->empty()
            || fullW_ <= 0 || fullH_ <= 0 || cachedWidth_ <= 0 || cachedHeight_ <= 0) {
        return -1;
    }

    const int mx = LIM(fullX * cachedWidth_ / fullW_, 0, cachedWidth_ - 1);
    const int my = LIM(fullY * cachedHeight_ / fullH_, 0, cachedHeight_ - 1);
    if (fullX < 0 || fullY < 0 || fullX >= fullW_ || fullY >= fullH_) {
        return -1;
    }

    const int modelClasses = std::min<int>(static_cast<int>(AISegClass::NUM_CLASSES),
                                           cachedMasks_->size());
    int best = -1;
    float bestProb = 0.f;
    for (int c = 0; c < modelClasses; ++c) {
        const float p = cachedMasks_->at(c)[my][mx];
        if (p > bestProb) {
            bestProb = p;
            best = c;
        }
    }

    // A click on any subject-ish class means "select the subject" — the
    // composed class is the better mask (dominant regions, holes filled).
    const int subjectIndex = static_cast<int>(AISegClass::SUBJECT);
    if (best >= 0 && subjectIndex < static_cast<int>(cachedMasks_->size())
            && (best == static_cast<int>(AISegClass::PERSON)
                || best == static_cast<int>(AISegClass::VEHICLE)
                || best == static_cast<int>(AISegClass::ANIMAL)
                || best == static_cast<int>(AISegClass::FOREGROUND_OBJECT))
            && cachedMasks_->at(subjectIndex)[my][mx] > 0.5f) {
        return subjectIndex;
    }

    return best;
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
    coverage_.clear();
    refinedMasks_.clear();
    distanceMasks_.clear();
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
    coverage_.clear();
    refinedMasks_.clear();
    distanceMasks_.clear();
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
