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
#include "partnermaskstore.h"

#ifdef RT_AI_MASKING

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "aisegmentation.h"
#include "aisubject.h"
#include "boxblur.h"
#include "iccstore.h"
#include "imagefloat.h"
#include "partnerimagestore.h"
#include "rt_math.h"
#include "settings.h"

namespace rtengine
{

namespace
{

// The mask never needs to be finer than this: it weights a partner that is
// itself decoded at the preview tier, and a repeated motif is small on the
// page. Same cap as AIMaskCache, so the two agree on how much detail a
// segmentation is worth.
constexpr int MAX_PARTNER_MASK_DIMENSION = 1024;

int classIndex(procparams::DoubleExposureParams::MaskClass cls)
{
    using MaskClass = procparams::DoubleExposureParams::MaskClass;

    switch (cls) {
        case MaskClass::SUBJECT:
            return static_cast<int>(AISegClass::SUBJECT);

        case MaskClass::PERSON:
            return static_cast<int>(AISegClass::PERSON);

        case MaskClass::SKY:
            return static_cast<int>(AISegClass::SKY);

        case MaskClass::VEGETATION:
            return static_cast<int>(AISegClass::VEGETATION);

        case MaskClass::BUILDING:
            return static_cast<int>(AISegClass::BUILDING);

        case MaskClass::VEHICLE:
            return static_cast<int>(AISegClass::VEHICLE);

        case MaskClass::ANIMAL:
            return static_cast<int>(AISegClass::ANIMAL);

        case MaskClass::OFF:
        default:
            return -1;
    }
}

// Segment the partner and reduce it to the one class the layer asked for.
std::shared_ptr<PartnerMask> computeMask(const Glib::ustring& path, const Glib::ustring& workingProfile,
                                         procparams::DoubleExposureParams::MaskClass cls,
                                         double feather, bool invert, const MaskPaint& paint,
                                         bool multiThread)
{
    const int wanted = classIndex(cls);

    if (wanted < 0) {
        return nullptr;
    }

    AISegmentationEngine& engine = getAISegmentationEngine();

    if (!engine.isInitialized()) {
        return nullptr;
    }

    // The composite has already decoded and cached this partner, so the input
    // costs nothing beyond the segmentation itself.
    auto partner = PartnerImageStore::getInstance().getPartner(path, workingProfile, false);

    if (!partner || !partner->image || partner->fullWidth <= 0 || partner->fullHeight <= 0) {
        return nullptr;
    }

    const Imagefloat& img = *partner->image;
    const int srcW = img.getWidth();
    const int srcH = img.getHeight();

    if (srcW <= 0 || srcH <= 0) {
        return nullptr;
    }

    const float cacheScale = std::min(1.f, static_cast<float>(MAX_PARTNER_MASK_DIMENSION)
                                      / static_cast<float>(std::max(srcW, srcH)));
    const int maskW = std::max(1, static_cast<int>(std::lround(srcW * cacheScale)));
    const int maskH = std::max(1, static_cast<int>(std::lround(srcH * cacheScale)));

    array2D<float> scaledR(maskW, maskH);
    array2D<float> scaledG(maskW, maskH);
    array2D<float> scaledB(maskW, maskH);

    // Same preparation as AIMaskCache: bilinear downscale in the working
    // space, then the matrix into linear sRGB the model was trained on.
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

    const float scaleX = static_cast<float>(srcW) / maskW;
    const float scaleY = static_cast<float>(srcH) / maskH;

#ifdef _OPENMP
    #pragma omp parallel for if(multiThread)
#endif
    for (int y = 0; y < maskH; ++y) {
        const float sourceY = std::max(0.f, (y + 0.5f) * scaleY - 0.5f);
        const int y0 = std::min(static_cast<int>(sourceY), srcH - 1);
        const int y1 = std::min(y0 + 1, srcH - 1);
        const float fy = sourceY - y0;

        for (int x = 0; x < maskW; ++x) {
            const float sourceX = std::max(0.f, (x + 0.5f) * scaleX - 0.5f);
            const int x0 = std::min(static_cast<int>(sourceX), srcW - 1);
            const int x1 = std::min(x0 + 1, srcW - 1);
            const float fx = sourceX - x0;

            const auto sample = [&](float a, float b, float c, float d) {
                const float top = a + fx * (b - a);
                const float bottom = c + fx * (d - c);
                return top + fy * (bottom - top);
            };

            const float workR = sample(img.r(y0, x0), img.r(y0, x1), img.r(y1, x0), img.r(y1, x1));
            const float workG = sample(img.g(y0, x0), img.g(y0, x1), img.g(y1, x0), img.g(y1, x1));
            const float workB = sample(img.b(y0, x0), img.b(y0, x1), img.b(y1, x0), img.b(y1, x1));

            scaledR[y][x] = workToSRGB[0][0] * workR + workToSRGB[0][1] * workG + workToSRGB[0][2] * workB;
            scaledG[y][x] = workToSRGB[1][0] * workR + workToSRGB[1][1] * workG + workToSRGB[1][2] * workB;
            scaledB[y][x] = workToSRGB[2][0] * workR + workToSRGB[2][1] * workG + workToSRGB[2][2] * workB;
        }
    }

    std::vector<array2D<float>> maps = engine.segment(static_cast<float**>(scaledR),
                                                      static_cast<float**>(scaledG),
                                                      static_cast<float**>(scaledB),
                                                      maskW, maskH, multiThread);

    if (static_cast<int>(maps.size()) != static_cast<int>(AISegClass::NUM_CLASSES)) {
        return nullptr;
    }

    appendSubjectMasks(maps, maskW, maskH, multiThread);

    if (wanted >= static_cast<int>(maps.size())) {
        return nullptr;
    }

    auto result = std::make_shared<PartnerMask>();
    result->width = maskW;
    result->height = maskH;
    result->fullWidth = partner->fullWidth;
    result->fullHeight = partner->fullHeight;
    result->mask(maskW, maskH);

    array2D<float>& chosen = maps[wanted];

    // Feather is a blur radius as a fraction of the mask's short edge: a
    // segmentation boundary is only ever approximate, and a hard edge on an
    // approximate boundary is what reads as a cut-out.
    const float radius = static_cast<float>(std::max(0.0, feather)) / 100.f * 0.06f
                         * static_cast<float>(std::min(maskW, maskH));

    if (radius >= 0.5f) {
        boxblur(static_cast<float**>(chosen), static_cast<float**>(result->mask),
                static_cast<int>(std::lround(radius)), maskW, maskH, multiThread);
    } else {
#ifdef _OPENMP
        #pragma omp parallel for if(multiThread)
#endif
        for (int y = 0; y < maskH; ++y) {
            for (int x = 0; x < maskW; ++x) {
                result->mask[y][x] = chosen[y][x];
            }
        }
    }

    // Inversion belongs to the automatic selection, so it happens before the
    // strokes: the user paints on what they can see, and what they see is the
    // inverted mask.
    if (invert) {
#ifdef _OPENMP
        #pragma omp parallel for if(multiThread)
#endif
        for (int y = 0; y < maskH; ++y) {
            for (int x = 0; x < maskW; ++x) {
                result->mask[y][x] = LIM01(1.f - result->mask[y][x]);
            }
        }
    }

    if (!paint.empty()) {
        paint.apply(result->mask, maskW, maskH, multiThread);
    }

    // Bounds come from the UNBLURRED class, so a heavy feather does not creep
    // the crop outward; the box is then padded by the feather so a softened
    // edge is not clipped by its own bounding box. Once anything has been
    // inverted or painted, though, the class no longer describes what is
    // selected, and the finished mask has to be measured instead.
    const bool measureFinished = invert || !paint.empty();
    int bx0 = maskW;
    int by0 = maskH;
    int bx1 = -1;
    int by1 = -1;

    for (int y = 0; y < maskH; ++y) {
        for (int x = 0; x < maskW; ++x) {
            const float value = measureFinished ? result->mask[y][x] : chosen[y][x];

            if (value > 0.5f) {
                bx0 = std::min(bx0, x);
                by0 = std::min(by0, y);
                bx1 = std::max(bx1, x);
                by1 = std::max(by1, y);
            }
        }
    }

    if (bx1 >= bx0 && by1 >= by0) {
        // Room to breathe around the subject. The feather needs its own width
        // or a softened edge is clipped by the box that produced it, and a
        // little beyond that keeps a repeated motif from touching its
        // neighbours -- an animal cut out flush with its own outline reads as
        // a sticker, not as a second exposure.
        const int margin = static_cast<int>(std::lround(
                               0.08f * std::min(bx1 - bx0 + 1, by1 - by0 + 1)));
        const int pad = static_cast<int>(std::ceil(radius)) + 1 + std::max(2, margin);
        bx0 = std::max(0, bx0 - pad);
        by0 = std::max(0, by0 - pad);
        bx1 = std::min(maskW - 1, bx1 + pad);
        by1 = std::min(maskH - 1, by1 + pad);

        // Back to the partner's own full frame, which is what the map works in.
        const float toFullX = static_cast<float>(result->fullWidth) / maskW;
        const float toFullY = static_cast<float>(result->fullHeight) / maskH;
        result->x0 = static_cast<int>(std::floor(bx0 * toFullX));
        result->y0 = static_cast<int>(std::floor(by0 * toFullY));
        result->x1 = static_cast<int>(std::ceil((bx1 + 1) * toFullX));
        result->y1 = static_cast<int>(std::ceil((by1 + 1) * toFullY));
    }

    if (settings->verbose) {
        std::fprintf(stderr, "[partnerMask] %s class=%d mask=%dx%d feather=%.0f invert=%d bbox=%d,%d..%d,%d\n",
                     path.c_str(), wanted, maskW, maskH, feather, invert ? 1 : 0,
                     result->x0, result->y0, result->x1, result->y1);
    }

    return result;
}

} // namespace

float PartnerMask::sample(float u, float v) const
{
    if (!valid()) {
        return 1.f;
    }

    const float tu = u * width / std::max(1, fullWidth) - 0.5f;
    const float tv = v * height / std::max(1, fullHeight) - 0.5f;

    int x0 = static_cast<int>(std::floor(tu));
    int y0 = static_cast<int>(std::floor(tv));
    float dx = tu - x0;
    float dy = tv - y0;

    if (x0 < 0) {
        x0 = 0;
        dx = 0.f;
    } else if (x0 > width - 1) {
        x0 = width - 1;
        dx = 0.f;
    }

    if (y0 < 0) {
        y0 = 0;
        dy = 0.f;
    } else if (y0 > height - 1) {
        y0 = height - 1;
        dy = 0.f;
    }

    const int x1 = x0 + 1 < width ? x0 + 1 : width - 1;
    const int y1 = y0 + 1 < height ? y0 + 1 : height - 1;

    const float top = mask[y0][x0] + dx * (mask[y0][x1] - mask[y0][x0]);
    const float bottom = mask[y1][x0] + dx * (mask[y1][x1] - mask[y1][x0]);
    return LIM01(top + dy * (bottom - top));
}

PartnerMaskStore::PartnerMaskStore() :
    cache(8)
{
}

PartnerMaskStore& PartnerMaskStore::getInstance()
{
    static PartnerMaskStore instance;
    return instance;
}

std::shared_ptr<const PartnerMask> PartnerMaskStore::getMask(const Glib::ustring& path,
                                                             const Glib::ustring& workingProfile,
                                                             procparams::DoubleExposureParams::MaskClass cls,
                                                             double feather, bool invert,
                                                             const MaskPaint& paint, bool multiThread)
{
    if (path.empty() || cls == procparams::DoubleExposureParams::MaskClass::OFF) {
        return nullptr;
    }

    const Glib::ustring key = makeKey(path, workingProfile, cls, feather, invert, paint);

    std::shared_ptr<PartnerMask> result;

    if (cache.get(key, result)) {
        return result;
    }

    result = computeMask(path, workingProfile, cls, feather, invert, paint, multiThread);

    if (result) {
        cache.insert(key, result);
    }

    return result;
}

std::shared_ptr<const PartnerMask> PartnerMaskStore::peekMask(const Glib::ustring& path,
                                                              const Glib::ustring& workingProfile,
                                                              procparams::DoubleExposureParams::MaskClass cls,
                                                              double feather, bool invert,
                                                              const MaskPaint& paint)
{
    if (path.empty() || cls == procparams::DoubleExposureParams::MaskClass::OFF) {
        return nullptr;
    }

    std::shared_ptr<PartnerMask> result;

    if (cache.get(makeKey(path, workingProfile, cls, feather, invert, paint), result)) {
        return result;
    }

    return nullptr;
}

// Feather is quantised into the key: a slider drag would otherwise re-segment
// on every value it passes through.
Glib::ustring PartnerMaskStore::makeKey(const Glib::ustring& path, const Glib::ustring& workingProfile,
                                        procparams::DoubleExposureParams::MaskClass cls,
                                        double feather, bool invert, const MaskPaint& paint)
{
    return Glib::ustring::compose("%1|%2|%3|%4|%5|%6", path, workingProfile,
                                  static_cast<int>(cls), static_cast<int>(std::lround(feather)),
                                  invert ? 1 : 0, paint.hash());
}

void PartnerMaskStore::clearCache()
{
    cache.clear();
}

} // namespace rtengine

#endif // RT_AI_MASKING
