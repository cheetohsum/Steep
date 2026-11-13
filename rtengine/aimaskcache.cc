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
#include "aisubject.h"
#include "maskpaint.h"

#include "boxblur.h"
#include "edittrace.h"
#include "guidedfilter.h"
#include "iccstore.h"
#include "imagefloat.h"
#include "samselector.h"
#include "partnerdepthstore.h"
#include "aisubjectmodel.h"
#include "settings.h"
#include "rt_math.h"

#include <algorithm>
#include <cstdio>
#include <zlib.h>

#include <glib/gstdio.h>
#include <glibmm/checksum.h>
#include <glibmm/fileutils.h>
#include <glibmm/miscutils.h>
#include <array>
#include <cstdlib>
#include <functional>
#include <cmath>
#include <vector>

namespace rtengine
{

namespace
{

constexpr std::size_t MAX_PREPARED_MASKS = 12;
constexpr std::size_t MAX_REFINED_MASKS = 4;
constexpr std::size_t MAX_DISTANCE_MASKS = 4;
// Class maps are kept at up to this size (the input is 2048 on its long side,
// see AI_MASK_INPUT_DIMENSION): the edges the refinement step recovers would
// be thrown away again by a coarser cache.
constexpr int MAX_MASK_CACHE_DIMENSION = 2048;
constexpr float DISTANCE_INFINITY = 1.e20f;

int quantize(float value, float scale)
{
    return static_cast<int>(std::lround(value * scale));
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
        && refineEps == other.refineEps
        && paint == other.paint
        && std::equal(depthRange, depthRange + 4, other.depthRange);
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

namespace
{

/*
 * Crisp edges for object classes. The scene parser decides WHAT is where but
 * draws it at an eighth (MobileNet) or a quarter (HRNet) of its input, so a
 * person's outline is a blur of blocks. SAM 2 knows nothing about classes but
 * draws an object's exact outline from a few clicks inside it. Each sizeable
 * region of an object class becomes a SAM prompt (points deep inside it); the
 * answer replaces the region's map only where it agrees with it, so SAM can
 * sharpen an edge but never decide that the person is the whole sofa.
 * Amorphous classes -- sky, foliage, ground -- stay with the parser.
 */
// The class-map-sized picture SAM is shown (SamSelector reads ProPhoto; the
// planes arrive as linear sRGB). Kept with the maps for click-to-select.
std::shared_ptr<const Imagefloat> makeSamSource(float* const* r, float* const* g, float* const* b, int w, int h)
{
    if (!SamSelector::getInstance().available() || w < 32 || h < 32) {
        return nullptr;
    }
    const TMatrix toXYZ = ICCStore::getInstance()->workingSpaceMatrix("sRGB");
    const TMatrix toPro = ICCStore::getInstance()->workingSpaceInverseMatrix("ProPhoto");
    float m[3][3] = {};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            for (int k = 0; k < 3; ++k) {
                m[i][j] += static_cast<float>(toPro[i][k] * toXYZ[k][j]);
            }
        }
    }
    auto source = std::make_shared<Imagefloat>(w, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float sr = std::max(0.f, r[y][x]);
            const float sg = std::max(0.f, g[y][x]);
            const float sb = std::max(0.f, b[y][x]);
            source->r(y, x) = m[0][0] * sr + m[0][1] * sg + m[0][2] * sb;
            source->g(y, x) = m[1][0] * sr + m[1][1] * sg + m[1][2] * sb;
            source->b(y, x) = m[2][0] * sr + m[2][1] * sg + m[2][2] * sb;
        }
    }
    return source;
}

bool refineObjectsWithSam(std::vector<array2D<float>>& maps, const Imagefloat& source, const std::string& id)
{
    SamSelector& sam = SamSelector::getInstance();
    const int w = source.getWidth();
    const int h = source.getHeight();
    if (!sam.available() || std::getenv("STEEP_AIMASK_NO_SAM") || w < 32 || h < 32) {
        return false;
    }

    struct Region {
        int cls;
        int labelSet;
        int label;
        int area;
        int x0, y0, x1, y1;
    };

    const AISegClass objectClasses[] = {
        AISegClass::PERSON, AISegClass::VEHICLE, AISegClass::ANIMAL, AISegClass::FOREGROUND_OBJECT
    };
    constexpr size_t maxPrompts = 8;
    const int minArea = std::max(64, static_cast<int>(0.001 * w * h));

    // One label image per class: regions of different classes may overlap.
    std::vector<std::vector<int>> labels;
    std::vector<Region> regions;
    for (AISegClass cls : objectClasses) {
        const int c = static_cast<int>(cls);
        if (c >= static_cast<int>(maps.size())) {
            continue;
        }
        const array2D<float>& map = maps[c];
        labels.emplace_back(static_cast<size_t>(w) * h, -1);
        const int labelSet = static_cast<int>(labels.size()) - 1;
        std::vector<int>& lab = labels.back();
        std::vector<int> stack;
        int next = 0;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                if (map[y][x] <= 0.5f || lab[y * w + x] >= 0) {
                    continue;
                }
                Region region{c, labelSet, next, 0, x, y, x, y};
                stack.assign(1, y * w + x);
                lab[y * w + x] = next;
                while (!stack.empty()) {
                    const int at = stack.back();
                    stack.pop_back();
                    const int px = at % w;
                    const int py = at / w;
                    ++region.area;
                    region.x0 = std::min(region.x0, px);
                    region.x1 = std::max(region.x1, px);
                    region.y0 = std::min(region.y0, py);
                    region.y1 = std::max(region.y1, py);
                    const int nx[4] = {px - 1, px + 1, px, px};
                    const int ny[4] = {py, py, py - 1, py + 1};
                    for (int k = 0; k < 4; ++k) {
                        if (nx[k] >= 0 && nx[k] < w && ny[k] >= 0 && ny[k] < h) {
                            const int n = ny[k] * w + nx[k];
                            if (lab[n] < 0 && map[ny[k]][nx[k]] > 0.5f) {
                                lab[n] = next;
                                stack.push_back(n);
                            }
                        }
                    }
                }
                ++next;
                if (region.area >= minArea) {
                    regions.push_back(region);
                }
            }
        }
    }
    if (regions.empty()) {
        return false;
    }
    std::sort(regions.begin(), regions.end(), [](const Region& a, const Region& b) { return a.area > b.area; });
    if (regions.size() > maxPrompts) {
        regions.resize(maxPrompts);
    }

    bool refined = false;
    int accepted = 0;
    for (const Region& region : regions) {
        const std::vector<int>& lab = labels[region.labelSet];
        const int label = region.label;
        array2D<float>& map = maps[region.cls];

        // Points deep inside the region: the interior maximum of a chamfer
        // distance, then up to two more spread across its core.
        const int bw = region.x1 - region.x0 + 3;
        const int bh = region.y1 - region.y0 + 3;
        std::vector<float> dt(static_cast<size_t>(bw) * bh, 0.f);
        for (int y = 1; y < bh - 1; ++y) {
            for (int x = 1; x < bw - 1; ++x) {
                const int ix = region.x0 + x - 1;
                const int iy = region.y0 + y - 1;
                dt[y * bw + x] = lab[iy * w + ix] == label ? 1e9f : 0.f;
            }
        }
        for (int y = 1; y < bh - 1; ++y) {
            for (int x = 1; x < bw - 1; ++x) {
                float& d = dt[y * bw + x];
                d = std::min({d, dt[y * bw + x - 1] + 1.f, dt[(y - 1) * bw + x] + 1.f,
                              dt[(y - 1) * bw + x - 1] + 1.414f, dt[(y - 1) * bw + x + 1] + 1.414f});
            }
        }
        float deepest = 0.f;
        int px = 0, py = 0;
        for (int y = bh - 2; y >= 1; --y) {
            for (int x = bw - 2; x >= 1; --x) {
                float& d = dt[y * bw + x];
                d = std::min({d, dt[y * bw + x + 1] + 1.f, dt[(y + 1) * bw + x] + 1.f,
                              dt[(y + 1) * bw + x + 1] + 1.414f, dt[(y + 1) * bw + x - 1] + 1.414f});
                if (d > deepest) {
                    deepest = d;
                    px = x;
                    py = y;
                }
            }
        }
        if (deepest < 2.f) {
            continue;
        }
        std::vector<std::pair<int, int>> points{{px, py}};
        if (deepest >= 4.f) {
            for (int extra = 0; extra < 2; ++extra) {
                float best = 0.f;
                int bx = -1, by = -1;
                for (int y = 1; y < bh - 1; ++y) {
                    for (int x = 1; x < bw - 1; ++x) {
                        if (dt[y * bw + x] < 0.4f * deepest) {
                            continue;
                        }
                        float nearest = 1e30f;
                        for (const auto& p : points) {
                            nearest = std::min(nearest, static_cast<float>((x - p.first) * (x - p.first) + (y - p.second) * (y - p.second)));
                        }
                        if (nearest > best) {
                            best = nearest;
                            bx = x;
                            by = y;
                        }
                    }
                }
                if (bx >= 0 && best > deepest * deepest) {
                    points.emplace_back(bx, by);
                }
            }
        }
        std::vector<std::array<float, 3>> prompts;
        for (const auto& p : points) {
            prompts.push_back({(region.x0 + p.first - 1 + 0.5f) / w, (region.y0 + p.second - 1 + 0.5f) / h, 1.f});
        }

        // The region itself as SAM's reference, so the candidate kept is the
        // one that redraws this region rather than a part of it or a group.
        array2D<float> reference(w, h, ARRAY2D_CLEAR_DATA);
        for (int y = region.y0; y <= region.y1; ++y) {
            for (int x = region.x0; x <= region.x1; ++x) {
                reference[y][x] = lab[y * w + x] == label ? 1.f : 0.f;
            }
        }
        const array2D<float> selected = sam.select(source, id, prompts, -1, &reference);
        if (selected.getWidth() != w || selected.getHeight() != h) {
            continue;
        }

        // Compare and splice inside the region's box plus a margin: SAM may
        // move the edge, not wander off to another object.
        const int margin = 4 + static_cast<int>(0.15f * std::max(bw, bh));
        const int ex0 = std::max(0, region.x0 - margin), ex1 = std::min(w - 1, region.x1 + margin);
        const int ey0 = std::max(0, region.y0 - margin), ey1 = std::min(h - 1, region.y1 + margin);
        long inter = 0, uni = 0, outside = 0;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const bool s = selected[y][x] > 0.5f;
                const bool inBox = x >= ex0 && x <= ex1 && y >= ey0 && y <= ey1;
                if (!inBox) {
                    outside += s;
                    continue;
                }
                const bool a = lab[y * w + x] == label;
                inter += a && s;
                uni += a || s;
            }
        }
        const float iou = uni > 0 ? static_cast<float>(inter) / uni : 0.f;
        if (iou < 0.55f || outside > region.area / 10) {
            if (edittrace::enabled()) {
                edittrace::logf("[aiMask] sam reject cls=%d area=%d iou=%.2f outside=%ld", region.cls, region.area, iou, outside);
            }
            continue;
        }
        for (int y = ey0; y <= ey1; ++y) {
            for (int x = ex0; x <= ex1; ++x) {
                const int other = lab[y * w + x];
                if (other >= 0 && other != label) {
                    continue;   // another region of this class keeps its own map
                }
                map[y][x] = selected[y][x];
            }
        }
        refined = true;
        ++accepted;
    }
    if (edittrace::enabled()) {
        edittrace::logf("[aiMask] sam refined %d of %zu regions", accepted, regions.size());
    }
    return refined;
}

}

namespace
{

/*
 * Persisted class maps. Segmenting a photo costs seconds of model time (more
 * with SAM refinement), so the result is kept under the cache directory and
 * a reopened photo, in this session or a later one, skips the models. The key
 * covers everything the maps depend on: the file (path, size, mtime), the
 * input geometry and size, the working profile and the exact models in use.
 * Stored as 8-bit probabilities, zlib-compressed: a few hundred KB a photo.
 */
constexpr char DISK_MAGIC[4] = {'S', 'A', 'I', 'M'};
// 2: twelve classes, depth flag. 3: depth is the log-distance scale and
// rides behind the class planes at 16 bits (8 bits put visible steps into a
// feathered depth mask); a version 2 file still serves its class maps, and
// its depth is estimated again.
constexpr std::uint32_t DISK_VERSION = 3;

std::string diskCachePath(const std::string& imageId, std::uint64_t inputKey, const std::string& workingProfile,
                          int width, int height, int fullW, int fullH)
{
    if (!settings || settings->aiMaskCacheDir.empty() || std::getenv("STEEP_AIMASK_NO_DISK_CACHE")) {
        return std::string();
    }
    GStatBuf st;
    if (g_stat(imageId.c_str(), &st) != 0) {
        return std::string();
    }
    std::string key = imageId;
    key += "|" + std::to_string(static_cast<long long>(st.st_size)) + "|" + std::to_string(static_cast<long long>(st.st_mtime));
    key += "|" + std::to_string(inputKey) + "|" + workingProfile;
    key += "|" + std::to_string(width) + "x" + std::to_string(height) + "|" + std::to_string(fullW) + "x" + std::to_string(fullH);
    key += "|seg=" + getAISegmentationEngine().modelPath();
    key += "|subj=" + getAISubjectEngine().modelPath();
    key += std::string("|sam=") + (SamSelector::getInstance().available() && !std::getenv("STEEP_AIMASK_NO_SAM") ? "1" : "0");
    key += "|v" + std::to_string(DISK_VERSION);
    const std::string digest = Glib::Checksum::compute_checksum(Glib::Checksum::CHECKSUM_MD5, key);
    return Glib::build_filename(settings->aiMaskCacheDir.raw(), Glib::path_get_basename(imageId) + "." + digest + ".aim");
}

// Depth-class helpers, defined with getPreparedMask below.
float depthSplitOf(const array2D<float>& depth);
std::array<double, 4> depthPreset(int classIndex, float split);
float depthRangeWeight(float d, const std::array<double, 4>& v);
bool isDepthClass(int classIndex);

bool loadMapsFromDisk(const std::string& path, int width, int height,
                      std::vector<array2D<float>>& maps, bool& usedSubjectModel, bool& depthReady)
{
    if (path.empty()) {
        return false;
    }
    std::FILE* f = g_fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    char magic[4] = {};
    std::uint32_t header[5] = {};
    bool ok = std::fread(magic, 1, 4, f) == 4 && std::equal(magic, magic + 4, DISK_MAGIC)
              && std::fread(header, sizeof(header[0]), 5, f) == 5
              && (header[0] == DISK_VERSION || header[0] == 2) && static_cast<int>(header[1]) == width
              && static_cast<int>(header[2]) == height
              && header[3] == static_cast<std::uint32_t>(AISegClass::TOTAL_CLASSES);
    std::vector<unsigned char> packed;
    if (ok) {
        packed.resize(header[4]);
        ok = std::fread(packed.data(), 1, packed.size(), f) == packed.size();
    }
    std::fclose(f);
    if (!ok) {
        return false;
    }
    const std::size_t plane = static_cast<std::size_t>(width) * height;
    const std::size_t classBytes = plane * header[3] + 1;
    std::vector<unsigned char> bytes(classBytes + plane * 2);
    uLongf length = bytes.size();
    if (uncompress(bytes.data(), &length, packed.data(), packed.size()) != Z_OK
            || (length != classBytes && length != classBytes + plane * 2)) {
        return false;
    }
    const unsigned char flags = bytes[classBytes - 1];
    usedSubjectModel = (flags & 1) != 0;
    depthReady = header[0] == DISK_VERSION && (flags & 2) != 0 && length == classBytes + plane * 2;
    maps.clear();
    for (std::uint32_t c = 0; c < header[3]; ++c) {
        maps.emplace_back(width, height);
        const unsigned char* src = bytes.data() + c * plane;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                maps.back()[y][x] = src[static_cast<std::size_t>(y) * width + x] / 255.f;
            }
        }
    }
    for (int c : {static_cast<int>(AISegClass::FOREGROUND_DEPTH), static_cast<int>(AISegClass::BACKGROUND_DEPTH)}) {
        if (c >= static_cast<int>(maps.size())) {
            continue;
        }
        const unsigned char* src = bytes.data() + classBytes;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 2;
                maps[c][y][x] = depthReady ? (src[i] | (src[i + 1] << 8)) / 65535.f : 0.f;
            }
        }
    }
    return true;
}

void saveMapsToDisk(const std::string& path, const std::vector<array2D<float>>& maps, int width, int height,
                    bool usedSubjectModel, bool depthReady)
{
    if (path.empty() || maps.size() != static_cast<std::size_t>(AISegClass::TOTAL_CLASSES)) {
        return;
    }
    const std::size_t plane = static_cast<std::size_t>(width) * height;
    const std::size_t classBytes = plane * maps.size() + 1;
    std::vector<unsigned char> bytes(classBytes + (depthReady ? plane * 2 : 0));
    for (std::size_t c = 0; c < maps.size(); ++c) {
        unsigned char* dst = bytes.data() + c * plane;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                dst[static_cast<std::size_t>(y) * width + x] =
                    static_cast<unsigned char>(LIM01(maps[c][y][x]) * 255.f + 0.5f);
            }
        }
    }
    bytes[classBytes - 1] = (usedSubjectModel ? 1 : 0) | (depthReady ? 2 : 0);
    if (depthReady) {
        const array2D<float>& depth = maps[static_cast<int>(AISegClass::FOREGROUND_DEPTH)];
        unsigned char* dst = bytes.data() + classBytes;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const unsigned int v = static_cast<unsigned int>(LIM01(depth[y][x]) * 65535.f + 0.5f);
                const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 2;
                dst[i] = v & 0xff;
                dst[i + 1] = v >> 8;
            }
        }
    }
    uLongf length = compressBound(bytes.size());
    std::vector<unsigned char> packed(length);
    if (compress2(packed.data(), &length, bytes.data(), bytes.size(), 6) != Z_OK) {
        return;
    }
    g_mkdir_with_parents(Glib::path_get_dirname(path).c_str(), 0755);
    const std::string temp = path + ".tmp";
    std::FILE* f = g_fopen(temp.c_str(), "wb");
    if (!f) {
        return;
    }
    const std::uint32_t header[5] = {DISK_VERSION, static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
                                     static_cast<std::uint32_t>(maps.size()), static_cast<std::uint32_t>(length)};
    const bool ok = std::fwrite(DISK_MAGIC, 1, 4, f) == 4
                    && std::fwrite(header, sizeof(header[0]), 5, f) == 5
                    && std::fwrite(packed.data(), 1, length, f) == length;
    std::fclose(f);
    if (!ok || g_rename(temp.c_str(), path.c_str()) != 0) {
        g_remove(temp.c_str());
    }
}

}

bool AIMaskCache::restoreArchivedLocked_(const std::string& imageId,
                                         int width, int height, int fullW, int fullH,
                                         const std::string& workingProfile,
                                         std::uint64_t inputKey, bool subjectModel)
{
    for (auto it = archive_.begin(); it != archive_.end(); ++it) {
        if (it->imageId == imageId
                && it->inputKey == inputKey
                && it->workingProfile == workingProfile
                && it->sourceWidth == width && it->sourceHeight == height
                && it->fullW == fullW && it->fullH == fullH && it->masks
                && it->subjectModel == subjectModel) {
            ArchivedImage restored = std::move(*it);
            archive_.erase(it);
            archiveCurrentLocked_();

            cachedImageId_ = std::move(restored.imageId);
            cachedWorkingProfile_ = std::move(restored.workingProfile);
            cachedInputKey_ = restored.inputKey;
            cachedDepthReady_ = restored.depthReady;
            cachedDepthSplit_ = restored.depthSplit;
            cachedDiskPath_ = std::move(restored.diskPath);
            cachedSubjectModel_ = restored.subjectModel;
            cachedMasks_ = std::move(restored.masks);
            cachedGuide_ = std::move(restored.guide);
            cachedSamSource_ = std::move(restored.samSource);
            coverage_ = std::move(restored.coverage);
            sourceWidth_ = restored.sourceWidth;
            sourceHeight_ = restored.sourceHeight;
            cachedWidth_ = restored.cachedWidth;
            cachedHeight_ = restored.cachedHeight;
            fullW_ = restored.fullW;
            fullH_ = restored.fullH;
            pending_ = false;
            refinedMasks_.clear();
            distanceMasks_.clear();
            preparedMasks_.clear();
            ++generation_;
            // Anything still computing for another image is now stale.
            ++requestGeneration_;

            if (edittrace::enabled()) {
                edittrace::logf("[aiMask] restored from archive img=%s", imageId.c_str());
            }
            return true;
        }
    }
    return false;
}

bool AIMaskCache::restoreOrBeginPending(const std::string& imageId,
                                        int width, int height, int fullW, int fullH,
                                        const std::string& workingProfile,
                                        std::uint64_t inputKey, bool discardInFlight)
{
    MyMutex::MyLock lock(mutex_);
    if (cachedImageId_ == imageId && cachedInputKey_ == inputKey
            && cachedMasks_ && !cachedMasks_->empty()) {
        return true;
    }
    if (restoreArchivedLocked_(imageId, width, height, fullW, fullH, workingProfile,
                               inputKey, getAISubjectEngine().isInitialized())) {
        return true;
    }
    if (pending_ && cachedImageId_ == imageId && cachedInputKey_ == inputKey) {
        return false;
    }

    archiveCurrentLocked_();
    cachedImageId_ = imageId;
    cachedWorkingProfile_ = workingProfile;
    cachedInputKey_ = inputKey;
    cachedDepthReady_ = false;
    cachedDiskPath_.clear();
    cachedMasks_.reset();
    cachedGuide_.reset();
    cachedSamSource_.reset();
    coverage_.clear();
    sourceWidth_ = width;
    sourceHeight_ = height;
    cachedWidth_ = 0;
    cachedHeight_ = 0;
    fullW_ = fullW;
    fullH_ = fullH;
    pending_ = true;
    refinedMasks_.clear();
    distanceMasks_.clear();
    preparedMasks_.clear();
    ++generation_;
    if (discardInFlight) {
        ++requestGeneration_;
    }
    return false;
}

void AIMaskCache::clearPending(const std::string& imageId, std::uint64_t inputKey)
{
    MyMutex::MyLock lock(mutex_);
    if (pending_ && cachedImageId_ == imageId && cachedInputKey_ == inputKey) {
        pending_ = false;
        ++generation_;
    }
}

bool AIMaskCache::isPending(const std::string& imageId, std::uint64_t inputKey) const
{
    MyMutex::MyLock lock(mutex_);
    return pending_ && cachedImageId_ == imageId && cachedInputKey_ == inputKey;
}

bool AIMaskCache::requestStale_(std::uint64_t request) const
{
    MyMutex::MyLock lock(mutex_);
    return request != requestGeneration_;
}

void AIMaskCache::computeMasks(const std::string& imageId,
                               float* const* rRows, float* const* gRows, float* const* bRows,
                               int width, int height, int fullW, int fullH,
                               const std::string& workingProfile, bool multiThread,
                               std::uint64_t inputKey)
{
    AISegmentationEngine& engine = getAISegmentationEngine();
    if (!engine.isInitialized() || width <= 0 || height <= 0) {
        return;
    }

    std::uint64_t request = 0;
    {
        MyMutex::MyLock lock(mutex_);
        const bool subjectModel = getAISubjectEngine().isInitialized();

        if (cachedImageId_ == imageId
                && cachedInputKey_ == inputKey
                && cachedWorkingProfile_ == workingProfile
                && sourceWidth_ == width && sourceHeight_ == height
                && fullW_ == fullW && fullH_ == fullH && cachedMasks_
                && cachedSubjectModel_ == subjectModel) {
            return;
        }

        // Revisiting a recent image: swap its maps back in.
        if (restoreArchivedLocked_(imageId, width, height, fullW, fullH,
                                   workingProfile, inputKey, subjectModel)) {
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

    const std::string diskPath = diskCachePath(imageId, inputKey, workingProfile, width, height, fullW, fullH);
    std::vector<array2D<float>> maps;
    bool usedSubjectModel = false;
    bool depthReady = false;
    const bool fromDisk = loadMapsFromDisk(diskPath, maskWidth, maskHeight, maps, usedSubjectModel, depthReady);
    if (fromDisk && edittrace::enabled()) {
        edittrace::logf("[aiMask] loaded from disk img=%s", imageId.c_str());
    }

    const long long traceSegmentUs = edittrace::enabled() ? edittrace::nowUs() : 0;
    if (!fromDisk) {
        bool segmented = true;
        maps = engine.segment(segmentR, segmentG, segmentB, maskWidth, maskHeight, multiThread, &segmented);
        if (!segmented) {
            // Empty maps would read as "nothing here" -- and on disk, for
            // good. Leave the photo pending: the mask worker tries again.
            std::fprintf(stderr, "AI Masking: segmentation failed for '%s'\n", imageId.c_str());
            return;
        }
    if (edittrace::enabled()) {
        edittrace::logf("[aiMask] segment img=%s src=%dx%d mask=%dx%d infer=%.0fms",
                        imageId.c_str(), width, height, maskWidth, maskHeight,
                        (edittrace::nowUs() - traceSegmentUs) / 1000.0);
    }

    }
    // Kept even when the maps come from disk: click-to-select prompts need it.
    std::shared_ptr<const Imagefloat> samSource = makeSamSource(segmentR, segmentG, segmentB, maskWidth, maskHeight);
    if (!fromDisk) {
        // SAM refinement and the subject model are seconds more work; when
        // the editor has already moved to another photo, stop here.
        if (requestStale_(request)) {
            if (edittrace::enabled()) {
                edittrace::logf("[aiMask] compute abandoned (stale) img=%s", imageId.c_str());
            }
            return;
        }
        const long long traceSamUs = edittrace::enabled() ? edittrace::nowUs() : 0;
        if (samSource && refineObjectsWithSam(maps, *samSource, imageId + "|aimask")
                && edittrace::enabled()) {
            edittrace::logf("[aiMask] sam refine %.0fms", (edittrace::nowUs() - traceSamUs) / 1000.0);
        }
        if (requestStale_(request)) {
            if (edittrace::enabled()) {
                edittrace::logf("[aiMask] compute abandoned (stale) img=%s", imageId.c_str());
            }
            return;
        }
        appendSubjectMasks(maps, maskWidth, maskHeight, multiThread);
        usedSubjectModel = applySubjectModel(maps, segmentR, segmentG, segmentB,
                                             maskWidth, maskHeight, multiThread);
        // Depth classes start empty; setDepthClasses fills them on demand.
        while (maps.size() < static_cast<std::size_t>(AISegClass::TOTAL_CLASSES)) {
            maps.emplace_back(maskWidth, maskHeight, ARRAY2D_CLEAR_DATA);
        }
        // A subject model that failed (out of memory) leaves the composed
        // subject in memory for now; never on disk, so the next open retries.
        if (usedSubjectModel || !getAISubjectEngine().isInitialized()) {
            saveMapsToDisk(diskPath, maps, maskWidth, maskHeight, usedSubjectModel, false);
        }
    }

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
                    // The default mask threshold, so this trace and the
                    // coverage the user is shown mean the same thing.
                    if (map[y][x] > 0.3f) {
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

    if (cachedImageId_ != imageId) {
        archiveCurrentLocked_();
    }

    cachedMasks_ = std::move(masks);
    cachedGuide_ = std::move(guide);
    cachedSamSource_ = std::move(samSource);
    cachedDepthReady_ = depthReady;
    cachedDepthSplit_ = depthReady && cachedMasks_->size() > static_cast<std::size_t>(AISegClass::FOREGROUND_DEPTH)
        ? depthSplitOf(cachedMasks_->at(static_cast<int>(AISegClass::FOREGROUND_DEPTH))) : 0.5f;
    cachedDiskPath_ = diskPath;
    coverage_ = std::move(coverage);
    cachedImageId_ = imageId;
    cachedSubjectModel_ = usedSubjectModel;
    cachedWorkingProfile_ = workingProfile;
    cachedInputKey_ = inputKey;
    pending_ = false;
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

namespace
{

// Otsu's split of a depth map (0 near .. 1 far): where this photograph's
// depths fall apart into a near group and a far one. What sits at the far
// end of the scale -- sky, anything past kFarReach -- is far by definition
// and stays out of the count: with it in, an open sky outweighs the whole
// scene and "near" comes out as everything that is not sky.
float depthSplitOf(const array2D<float>& depth)
{
    constexpr int bins = 256;
    constexpr int farEnd = 248;   // 0.97 and beyond
    std::vector<double> histogram(bins, 0.0);
    double count = 0.0;
    for (int y = 0; y < depth.getHeight(); ++y) {
        for (int x = 0; x < depth.getWidth(); ++x) {
            histogram[std::min(bins - 1, static_cast<int>(LIM01(depth[y][x]) * bins))] += 1.0;
            count += 1.0;
        }
    }
    if (count < 1.0) {
        return 0.5f;
    }
    double body = 0.0;
    for (int i = 0; i < farEnd; ++i) {
        body += histogram[i];
    }
    if (body >= 0.05 * count) {
        std::fill(histogram.begin() + farEnd, histogram.end(), 0.0);
        count = body;
    }
    double total = 0.0;
    for (int i = 0; i < bins; ++i) {
        total += i * histogram[i];
    }
    double below = 0.0, belowSum = 0.0, bestVariance = -1.0;
    int split = bins / 2;
    for (int i = 0; i < bins - 1; ++i) {
        below += histogram[i];
        belowSum += i * histogram[i];
        const double above = count - below;
        if (below <= 0.0 || above <= 0.0) {
            continue;
        }
        const double meanBelow = belowSum / below;
        const double meanAbove = (total - belowSum) / above;
        const double variance = below * above * (meanBelow - meanAbove) * (meanBelow - meanAbove);
        if (variance > bestVariance) {
            bestVariance = variance;
            split = i;
        }
    }
    return (split + 1.f) / bins;
}

// The automatic range of a near or far spot: everything this side of the
// split, with the default feather centred on it so that the two are each
// other's complement.
std::array<double, 4> depthPreset(int classIndex, float split)
{
    const double half = AIMaskCache::kDefaultDepthFeather / 2.0;
    if (classIndex == static_cast<int>(AISegClass::BACKGROUND_DEPTH)) {
        return AIMaskCache::makeDepthRange(std::min(1.0, split + half), 1.0, AIMaskCache::kDefaultDepthFeather);
    }
    return AIMaskCache::makeDepthRange(0.0, std::max(0.0, split - half), AIMaskCache::kDefaultDepthFeather);
}

float depthRangeWeight(float d, const std::array<double, 4>& v)
{
    const auto ease = [](double t) {
        t = std::max(0.0, std::min(1.0, t));
        return t * t * (3.0 - 2.0 * t);
    };
    const double n = v[1] <= v[0] ? (d >= v[1] ? 1.0 : 0.0) : ease((d - v[0]) / (v[1] - v[0]));
    const double f = v[3] <= v[2] ? (d <= v[2] ? 1.0 : 0.0) : ease((v[3] - d) / (v[3] - v[2]));
    return static_cast<float>(n * f);
}

bool isDepthClass(int classIndex)
{
    return classIndex == static_cast<int>(AISegClass::FOREGROUND_DEPTH)
        || classIndex == static_cast<int>(AISegClass::BACKGROUND_DEPTH);
}

}

constexpr double AIMaskCache::kMaxDepthFeather;
constexpr double AIMaskCache::kDefaultDepthFeather;

std::array<double, 4> AIMaskCache::makeDepthRange(double nearLimit, double farLimit, double feather)
{
    const double n = std::max(0.0, std::min(1.0, nearLimit));
    const double f = std::max(n, std::min(1.0, farLimit));
    const double w = std::max(0.0, std::min(kMaxDepthFeather, feather));
    return {{n - w, n, f, f + w}};
}

void AIMaskCache::splitDepthRange(const std::array<double, 4>& range, double& nearLimit, double& farLimit, double& feather)
{
    nearLimit = range[1];
    farLimit = range[2];
    feather = std::max(range[1] - range[0], range[3] - range[2]);
}

std::array<double, 4> AIMaskCache::parseDepthRange(const std::string& text)
{
    std::array<double, 4> range = automaticDepthRange();
    if (text.empty()) {
        return range;
    }
    std::array<double, 4> parsed{};
    size_t start = 0;
    for (int k = 0; k < 4; ++k) {
        const size_t end = text.find(';', start);
        const std::string part = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        char* stop = nullptr;
        const double value = std::strtod(part.c_str(), &stop);
        if (part.empty() || stop == part.c_str() || !std::isfinite(value)) {
            return range;
        }
        parsed[k] = value;
        if (end == std::string::npos) {
            if (k != 3) {
                return range;
            }
            break;
        }
        start = end + 1;
    }
    // limits ordered and inside 0..1, feathers no wider than allowed,
    // whatever the file says
    const double n = std::max(0.0, std::min(1.0, parsed[1]));
    const double f = std::max(n, std::min(1.0, parsed[2]));
    return {{n - std::max(0.0, std::min(kMaxDepthFeather, parsed[1] - parsed[0])), n,
             f, f + std::max(0.0, std::min(kMaxDepthFeather, parsed[3] - parsed[2]))}};
}

std::string AIMaskCache::formatDepthRange(const std::array<double, 4>& range)
{
    if (isAutomaticDepthRange(range)) {
        return std::string();
    }
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%.4f;%.4f;%.4f;%.4f", range[0], range[1], range[2], range[3]);
    return buffer;
}

bool AIMaskCache::getDepthPreset(const std::string& imageId, int classIndex, std::array<double, 4>& range) const
{
    MyMutex::MyLock lock(mutex_);
    if (cachedImageId_ != imageId || !cachedMasks_ || !cachedDepthReady_) {
        return false;
    }
    range = depthPreset(classIndex, cachedDepthSplit_);
    return true;
}

AIMaskSnapshot AIMaskCache::getPreparedMask(
    AISegClass cls, float threshold, float feather, float blur, float maskSize,
    bool invert, int refineRadius, float refineEps, const MaskPaint& paint,
    bool multiThread, const std::array<double, 4>& depthRange)
{
    const bool depthClass = isDepthClass(static_cast<int>(cls));
    const PreparedKey key {
        static_cast<int>(cls),
        quantize(threshold, 10000.f),
        quantize(feather, 100.f),
        quantize(blur, 100.f),
        quantize(maskSize, 100.f),
        invert ? 1 : 0,
        refineRadius,
        quantize(refineEps, 1000000.f),
        paint.hash(),
        {depthClass ? quantize(static_cast<float>(depthRange[0]), 1000.f) : 0,
         depthClass ? quantize(static_cast<float>(depthRange[1]), 1000.f) : 0,
         depthClass ? quantize(static_cast<float>(depthRange[2]), 1000.f) : 0,
         depthClass ? quantize(static_cast<float>(depthRange[3]), 1000.f) : 0}
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
    // Click-to-select: POINT strokes in the paint are SAM prompts, and the
    // object they pick replaces the class map as this spot's base mask. The
    // refined/distance caches are keyed by class only, so they are bypassed.
    const bool prompted = paint.hasPrompts();
    // A depth class is the depth map, not a mask: its base mask depends on
    // the spot's own range, which those caches do not key on either.
    const bool bypassCaches = prompted || depthClass;
    float depthSplit = 0.5f;
    std::shared_ptr<const Imagefloat> samSource;
    std::string samId;
    std::uint64_t generation = 0;
    int width = 0;
    int height = 0;
    int fullWidth = 0;
    int fullHeight = 0;

    {
        MyMutex::MyLock lock(mutex_);
        if (!cachedMasks_ && pending_) {
            static const std::shared_ptr<const array2D<float>> nothing = []() {
                auto zero = std::make_shared<array2D<float>>(1, 1);
                (*zero)[0][0] = 0.f;
                return zero;
            }();
            AIMaskSnapshot placeholder;
            placeholder.mask = nothing;
            placeholder.width = 1;
            placeholder.height = 1;
            placeholder.fullWidth = fullW_;
            placeholder.fullHeight = fullH_;
            placeholder.pending = true;
            return placeholder;
        }
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

        for (auto it = distanceMasks_.rbegin(); !bypassCaches && it != distanceMasks_.rend(); ++it) {
            if (it->generation == generation_ && it->key == distanceKey) {
                signedDistance = it->signedDistance;
                break;
            }
        }

        if (!signedDistance) {
            for (auto it = refinedMasks_.rbegin(); !bypassCaches && it != refinedMasks_.rend(); ++it) {
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
        samSource = cachedSamSource_;
        samId = cachedImageId_ + "|aimask";
        depthSplit = cachedDepthSplit_;
        if (depthClass && !cachedDepthReady_) {
            return {};   // not estimated yet: the spot edits nothing
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
    if (depthClass && baseMask) {
        // The spot's range over the depth map (or the automatic near / far).
        const std::array<double, 4> range = isAutomaticDepthRange(depthRange)
            ? depthPreset(static_cast<int>(cls), depthSplit) : depthRange;
        auto weighted = std::make_shared<array2D<float>>(width, height);
        const array2D<float>& depth = *baseMask;
#ifdef _OPENMP
        #pragma omp parallel for if(multiThread)
#endif
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                (*weighted)[y][x] = depthRangeWeight(depth[y][x], range);
            }
        }
        baseMask = std::move(weighted);
    }
    if (prompted && samSource) {
        array2D<float> selected = SamSelector::getInstance().select(*samSource, samId, paint.prompts());
        if (selected.getWidth() == width && selected.getHeight() == height) {
            auto picked = std::make_shared<array2D<float>>(width, height);
            picked->swap(selected);
            baseMask = std::move(picked);
            // SamSelector already followed the picture's edges.
            hasRefinedMask = true;
        }
    }
    // A depth band is already the finished mask: its soft edge is the
    // feather along the depth scale, which follows the ground as it recedes.
    // Cutting it at a threshold and feathering by distance in the frame
    // instead would turn that gradient into an outline with a halo.
    if (!depthClass && !signedDistance) {
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
            if (generation == generation_ && !bypassCaches) {
                refinedMasks_.push_back({refinedKey, generation, refinedMask});
                while (refinedMasks_.size() > MAX_REFINED_MASKS) {
                    refinedMasks_.pop_front();
                }
            }
        }

        signedDistance = buildSignedDistance(
            *refinedWorking, safeThreshold, width, height, multiThread);
        MyMutex::MyLock lock(mutex_);
        if (generation == generation_ && !bypassCaches) {
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
    // Feather as a fraction of the mask's own short side, not a count of
    // pixels scaled down with the preview. The cache tops out at 1024px, so
    // previewScale runs about 0.16 on a full frame and feather=100 came to
    // roughly 8 pixels of ramp -- under 1% of the mask, which still reads as
    // a cut edge however far the slider goes. At 100 the band is now 12% of
    // the short side, and it means the same thing at any zoom.
    const float featherRadius = (std::max(0.f, feather) / 100.f) * 0.12f
                                * static_cast<float>(std::min(width, height));
    auto prepared = std::make_shared<array2D<float>>(width, height);

#ifdef _OPENMP
    #pragma omp parallel for if(multiThread)
#endif
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float value;
            if (depthClass) {
                value = (*baseMask)[y][x];
            } else {
                const float edgeDistance = (*signedDistance)[y][x] + sizeShift;
                value = edgeDistance >= 0.f ? 1.f : 0.f;
                if (featherRadius > 0.01f) {
                    const float transition = LIM(
                        0.5f + edgeDistance / (2.f * featherRadius), 0.f, 1.f);
                    value = transition * transition * (3.f - 2.f * transition);
                }
            }
            if (invert) {
                value = 1.f - value;
            }
            (*prepared)[y][x] = value < 0.001f ? 0.f : (value > 0.999f ? 1.f : value);
        }
    }

    if (!paint.empty()) {
        paint.apply(*prepared, width, height, multiThread);
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

float AIMaskCache::getClassCoverage(const std::string& imageId, int classIndex,
                                    float threshold) const
{
    MyMutex::MyLock lock(mutex_);

    if (cachedImageId_ != imageId || !cachedMasks_) {
        return -1.f;
    }

    if (classIndex < 0 || classIndex >= static_cast<int>(cachedMasks_->size())) {
        return 0.f;
    }

    // Not measured yet: depth is only estimated once a mask asks for it.
    if (!cachedDepthReady_
            && (classIndex == static_cast<int>(AISegClass::FOREGROUND_DEPTH)
                || classIndex == static_cast<int>(AISegClass::BACKGROUND_DEPTH))) {
        return -1.f;
    }

    // Measured here rather than when the maps were built, because the
    // threshold is the user's tolerance and it moves. A class sitting at 0.35
    // covers a great deal at the default tolerance and nothing at all at 0.5,
    // which is how a mask plainly doing something came to be labelled 0%.
    const array2D<float>& map = cachedMasks_->at(classIndex);
    const int w = cachedWidth_;
    const int h = cachedHeight_;

    if (w <= 0 || h <= 0) {
        return 0.f;
    }

    const float cut = LIM(threshold, 0.f, 1.f);
    int hits = 0;

    if (isDepthClass(classIndex)) {
        // The map is depth, not a probability: count what the automatic
        // near / far range would select.
        const std::array<double, 4> range = depthPreset(classIndex, cachedDepthSplit_);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                hits += depthRangeWeight(map[y][x], range) > 0.5f;
            }
        }
        return static_cast<float>(hits) / (static_cast<float>(w) * h);
    }

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (map[y][x] > cut) {
                ++hits;
            }
        }
    }

    return static_cast<float>(hits) / (static_cast<float>(w) * h);
}

int AIMaskCache::getDominantClassAt(const std::string& imageId, int fullX, int fullY) const
{
    MyMutex::MyLock lock(mutex_);
    if (cachedImageId_ != imageId || !cachedMasks_ || cachedMasks_->empty()
            || fullW_ <= 0 || fullH_ <= 0 || cachedWidth_ <= 0 || cachedHeight_ <= 0) {
        return -1;
    }

    if (fullX < 0 || fullY < 0 || fullX >= fullW_ || fullY >= fullH_) {
        return -1;
    }

    // Map the full-image click into mask space and vote over a small
    // neighbourhood. The mask is a <=1024px rendering of the preview, so one
    // mask pixel covers many image pixels and sits on a class boundary far
    // more often than the click does; a single sample there flips between
    // the two classes at random. Averaging the softmax over ~1% of the long
    // edge picks the class that actually dominates under the cursor.
    const int mx = LIM(fullX * cachedWidth_ / fullW_, 0, cachedWidth_ - 1);
    const int my = LIM(fullY * cachedHeight_ / fullH_, 0, cachedHeight_ - 1);
    const int radius = std::max(2, std::max(cachedWidth_, cachedHeight_) / 200);
    const int x0 = std::max(0, mx - radius);
    const int x1 = std::min(cachedWidth_ - 1, mx + radius);
    const int y0 = std::max(0, my - radius);
    const int y1 = std::min(cachedHeight_ - 1, my + radius);

    const int modelClasses = std::min<int>(static_cast<int>(AISegClass::NUM_CLASSES),
                                           cachedMasks_->size());
    const int subjectIndex = static_cast<int>(AISegClass::SUBJECT);
    const bool haveSubject = subjectIndex < static_cast<int>(cachedMasks_->size());

    std::vector<double> classSum(modelClasses, 0.0);
    double subjectSum = 0.0;
    int samples = 0;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            for (int c = 0; c < modelClasses; ++c) {
                classSum[c] += cachedMasks_->at(c)[y][x];
            }
            if (haveSubject) {
                subjectSum += cachedMasks_->at(subjectIndex)[y][x];
            }
            ++samples;
        }
    }

    int best = -1;
    double bestSum = 0.0;
    for (int c = 0; c < modelClasses; ++c) {
        if (classSum[c] > bestSum) {
            bestSum = classSum[c];
            best = c;
        }
    }

    // A click on any subject-ish class means "select the subject" — the
    // composed class is the better mask (dominant regions, holes filled).
    if (best >= 0 && haveSubject && samples > 0
            && (best == static_cast<int>(AISegClass::PERSON)
                || best == static_cast<int>(AISegClass::VEHICLE)
                || best == static_cast<int>(AISegClass::ANIMAL)
                || best == static_cast<int>(AISegClass::FOREGROUND_OBJECT))
            && subjectSum / samples > 0.5) {
        return subjectIndex;
    }

    return best;
}

bool AIMaskCache::hasCachedMasks(const std::string& imageId, std::uint64_t inputKey) const
{
    MyMutex::MyLock lock(mutex_);
    // An archived image counts as missing: computeMasks swaps it back in.
    return cachedImageId_ == imageId && cachedInputKey_ == inputKey && cachedMasks_ && !cachedMasks_->empty();
}

bool AIMaskCache::hasDepthClasses(const std::string& imageId, std::uint64_t inputKey) const
{
    MyMutex::MyLock lock(mutex_);
    return cachedImageId_ == imageId && cachedInputKey_ == inputKey && cachedMasks_ && cachedDepthReady_;
}

void AIMaskCache::setDepthClasses(const std::string& imageId, std::uint64_t inputKey, const PartnerDepth& depth)
{
    std::vector<array2D<float>> masks;
    int width = 0;
    int height = 0;
    int fullWidth = 0;
    int fullHeight = 0;
    std::uint64_t generation = 0;
    {
        MyMutex::MyLock lock(mutex_);
        if (cachedImageId_ != imageId || cachedInputKey_ != inputKey || !cachedMasks_
                || cachedMasks_->size() != static_cast<std::size_t>(AISegClass::TOTAL_CLASSES)) {
            return;
        }
        masks = *cachedMasks_;
        width = cachedWidth_;
        height = cachedHeight_;
        fullWidth = fullW_;
        fullHeight = fullH_;
        generation = generation_;
    }
    if (!depth.valid() || width <= 0 || height <= 0) {
        return;
    }

    // Both depth classes keep the depth scale itself (0 near .. 1 far); each
    // spot reads its own range out of it in getPreparedMask.
    //
    // The estimator's map is linear in INVERSE depth between its own extreme
    // samples. Read as a depth scale that is badly lopsided: half of it is
    // spent between the nearest point and twice that distance, everything
    // from a few metres to the horizon shares the last tenth, and one stray
    // sample at either end squeezes the rest. A split made there calls the
    // strip of ground at the camera's feet "near" and a person three metres
    // away "far". So the scale is rebuilt here:
    //  - back to relative distance, using the estimator's own zero (its
    //    output is 0 at infinity) through depth.farRatio;
    //  - anchored at robust ends, the 0.5% nearest and farthest samples
    //    aside, and no farther than kFarReach times the nearest distance --
    //    beyond that everything is "the far end";
    //  - in equal steps of distance ratio (logarithmic), so the middle of the
    //    scale is the scene's geometric middle distance.
    constexpr float kFarReach = 40.f;
    array2D<float>& nearMap = masks[static_cast<int>(AISegClass::FOREGROUND_DEPTH)];
    array2D<float>& farMap = masks[static_cast<int>(AISegClass::BACKGROUND_DEPTH)];
    const float farRatio = LIM01(depth.farRatio);
    std::vector<float> ranked;
    ranked.reserve(static_cast<std::size_t>(width) * height / 16 + 16);
    for (int y = 0; y < height; ++y) {
        const float v = (y + 0.5f) / height * fullHeight;
        for (int x = 0; x < width; ++x) {
            const float u = (x + 0.5f) / width * fullWidth;
            const float d = depth.sample(u, v);
            // inverse depth, 1 at the estimator's nearest sample
            nearMap[y][x] = d >= 0.f ? 1.f - std::min(1.f, d) * (1.f - farRatio) : -1.f;
            if (d >= 0.f && (x & 3) == 0 && (y & 3) == 0) {
                ranked.push_back(nearMap[y][x]);
            }
        }
    }
    if (ranked.size() < 16) {
        return;
    }
    const auto rankAt = [&ranked](double fraction) {
        const std::size_t k = std::min(ranked.size() - 1, static_cast<std::size_t>(fraction * ranked.size()));
        std::nth_element(ranked.begin(), ranked.begin() + k, ranked.end());
        return ranked[k];
    };
    const float nearest = std::max(1e-6f, rankAt(0.995));
    const float farthest = std::max(std::max(rankAt(0.005), nearest / kFarReach), 1e-7f);
    // A flat subject (a wall square to the lens) has almost no range; keep
    // the scale from blowing its noise up to the full 0..1.
    const float span = std::log(std::max(1.05f, nearest / farthest));
    float fillSum = 0.f;
    int fillCount = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float& d = nearMap[y][x];
            if (d >= 0.f) {
                d = LIM01(std::log(nearest / LIM(d, farthest, nearest)) / span);
                fillSum += d;
                ++fillCount;
            }
        }
    }
    // Samples the estimator could not see take the scene's mean depth.
    const float fill = fillCount ? fillSum / fillCount : 0.5f;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (nearMap[y][x] < 0.f) {
                nearMap[y][x] = fill;
            }
            farMap[y][x] = nearMap[y][x];
        }
    }
    const float threshold = depthSplitOf(nearMap);
    const std::array<double, 4> nearPreset = depthPreset(static_cast<int>(AISegClass::FOREGROUND_DEPTH), threshold);
    const std::array<double, 4> farPreset = depthPreset(static_cast<int>(AISegClass::BACKGROUND_DEPTH), threshold);

    float nearCoverage = 0.f;
    float farCoverage = 0.f;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            nearCoverage += depthRangeWeight(nearMap[y][x], nearPreset) > 0.5f;
            farCoverage += depthRangeWeight(nearMap[y][x], farPreset) > 0.5f;
        }
    }
    nearCoverage /= static_cast<float>(width) * height;
    farCoverage /= static_cast<float>(width) * height;

    auto ready = std::make_shared<const std::vector<array2D<float>>>(std::move(masks));
    std::string diskPath;
    bool subjectModel = false;
    {
        MyMutex::MyLock lock(mutex_);
        if (generation != generation_ || cachedImageId_ != imageId || cachedInputKey_ != inputKey) {
            return;
        }
        cachedMasks_ = ready;
        cachedDepthReady_ = true;
        cachedDepthSplit_ = threshold;
        if (coverage_.size() > static_cast<std::size_t>(AISegClass::BACKGROUND_DEPTH)) {
            coverage_[static_cast<int>(AISegClass::FOREGROUND_DEPTH)] = nearCoverage;
            coverage_[static_cast<int>(AISegClass::BACKGROUND_DEPTH)] = farCoverage;
        }
        refinedMasks_.clear();
        distanceMasks_.clear();
        preparedMasks_.clear();
        ++generation_;
        diskPath = cachedDiskPath_;
        subjectModel = cachedSubjectModel_;
    }
    if (edittrace::enabled()) {
        edittrace::logf("[aiMask] depth classes img=%s split=%.2f near=%d%% far=%d%% reach=%.1fx", imageId.c_str(),
                        threshold, static_cast<int>(nearCoverage * 100), static_cast<int>(farCoverage * 100),
                        nearest / farthest);
    }
    if (const char* const dumpDir = std::getenv("STEEP_DEPTH_DUMP")) {
        // Headless check: the depth scale (white = near) and the automatic
        // near and far masks, as PGMs named after the photo.
        const std::string stem = std::string(dumpDir) + "/" + Glib::path_get_basename(imageId);
        const auto dump = [&](const char* suffix, const std::function<float(float)>& value) {
            if (std::FILE* const f = g_fopen((stem + suffix).c_str(), "wb")) {
                std::fprintf(f, "P5\n%d %d\n255\n", width, height);
                std::vector<unsigned char> row(width);
                const array2D<float>& map = ready->at(static_cast<int>(AISegClass::FOREGROUND_DEPTH));
                for (int y = 0; y < height; ++y) {
                    for (int x = 0; x < width; ++x) {
                        row[x] = static_cast<unsigned char>(LIM01(value(map[y][x])) * 255.f + 0.5f);
                    }
                    std::fwrite(row.data(), 1, row.size(), f);
                }
                std::fclose(f);
            }
        };
        dump(".depth.pgm", [](float d) { return 1.f - d; });
        dump(".near.pgm", [&](float d) { return depthRangeWeight(d, nearPreset); });
        dump(".far.pgm", [&](float d) { return depthRangeWeight(d, farPreset); });
        std::fprintf(stderr, "[depthDump] %s split=%.3f near=%.1f%% far=%.1f%% reach=%.1fx farRatio=%.3f\n",
                     stem.c_str(), threshold, nearCoverage * 100.f, farCoverage * 100.f, nearest / farthest, farRatio);
    }
    saveMapsToDisk(diskPath, *ready, width, height, subjectModel, true);
}



bool AIMaskCache::hasCachedMasks() const
{
    MyMutex::MyLock lock(mutex_);
    return cachedMasks_ && !cachedMasks_->empty();
}

void AIMaskCache::archiveCurrentLocked_()
{
    if (cachedImageId_.empty() || !cachedMasks_) {
        return;
    }

    for (auto it = archive_.begin(); it != archive_.end(); ++it) {
        if (it->imageId == cachedImageId_) {
            archive_.erase(it);
            break;
        }
    }

    ArchivedImage entry;
    entry.imageId = cachedImageId_;
    entry.workingProfile = cachedWorkingProfile_;
    entry.inputKey = cachedInputKey_;
    entry.depthReady = cachedDepthReady_;
    entry.depthSplit = cachedDepthSplit_;
    entry.diskPath = cachedDiskPath_;
    entry.subjectModel = cachedSubjectModel_;
    entry.masks = cachedMasks_;
    entry.guide = cachedGuide_;
    entry.samSource = cachedSamSource_;
    entry.coverage = coverage_;
    entry.sourceWidth = sourceWidth_;
    entry.sourceHeight = sourceHeight_;
    entry.cachedWidth = cachedWidth_;
    entry.cachedHeight = cachedHeight_;
    entry.fullW = fullW_;
    entry.fullH = fullH_;
    archive_.push_front(std::move(entry));

    while (archive_.size() > kArchivedImages) {
        archive_.pop_back();
    }
}

void AIMaskCache::invalidate(const std::string& imageId)
{
    MyMutex::MyLock lock(mutex_);

    for (auto it = archive_.begin(); it != archive_.end(); ++it) {
        if (it->imageId == imageId) {
            archive_.erase(it);
            break;
        }
    }

    if (cachedImageId_ != imageId) {
        return;
    }
    ++requestGeneration_;
    ++generation_;
    cachedImageId_.clear();
    cachedWorkingProfile_.clear();
    pending_ = false;
    cachedMasks_.reset();
    cachedGuide_.reset();
    cachedSamSource_.reset();
    coverage_.clear();
    refinedMasks_.clear();
    distanceMasks_.clear();
    preparedMasks_.clear();
    sourceWidth_ = sourceHeight_ = 0;
    cachedWidth_ = cachedHeight_ = fullW_ = fullH_ = 0;
}

void AIMaskCache::trimMemory(int level)
{
    if (!mutex_.trylock()) {
        return;
    }
    archive_.clear();
    if (level >= 2) {
        refinedMasks_.clear();
        distanceMasks_.clear();
        preparedMasks_.clear();
    }
    mutex_.unlock();
}

void AIMaskCache::invalidateAll()
{
    MyMutex::MyLock lock(mutex_);
    archive_.clear();
    ++requestGeneration_;
    ++generation_;
    cachedImageId_.clear();
    cachedWorkingProfile_.clear();
    pending_ = false;
    cachedMasks_.reset();
    cachedGuide_.reset();
    cachedSamSource_.reset();
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
