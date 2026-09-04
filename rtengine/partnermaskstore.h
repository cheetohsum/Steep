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
#pragma once

#ifdef RT_AI_MASKING

#include <memory>
#include <vector>

#include <glibmm/ustring.h>

#include "array2D.h"
#include "cache.h"
#include "procparams.h"

namespace rtengine
{

// One segmented class of a double exposure partner, ready to weight the
// layer with. Held separately from AIMaskCache on purpose: that cache holds
// exactly one image — the photo being edited, which locallab depends on —
// and asking it for a partner would evict the base's masks on every
// composite.
struct PartnerMask {
    array2D<float> mask;    // 0..1, `width` x `height`
    int width = 0;
    int height = 0;
    int fullWidth = 0;      // the partner's own full frame, which `mask` covers
    int fullHeight = 0;
    // Bounding box of the mask's support, in partner full-frame pixels. Empty
    // when the class was not found; callers then use the whole frame.
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;

    bool valid() const
    {
        return width > 0 && height > 0;
    }

    bool hasBounds() const
    {
        return x1 > x0 && y1 > y0;
    }

    // Bilinear read at partner full-frame coordinates, edge-clamped.
    float sample(float u, float v) const;
};

// LRU of segmented partner masks, one entry per (file, profile, class,
// feather). Segmentation runs on the calling thread the first time a mask is
// asked for; the picker warms the cache on its own workers so an edit rarely
// pays for it.
class PartnerMaskStore final
{
public:
    static PartnerMaskStore& getInstance();

    PartnerMaskStore(const PartnerMaskStore&) = delete;
    PartnerMaskStore& operator=(const PartnerMaskStore&) = delete;

    // Returns nullptr for MaskClass::OFF, when AI masking is unavailable, or
    // when the partner cannot be decoded — the layer then renders unmasked,
    // which is the least surprising way to lose a mask.
    std::shared_ptr<const PartnerMask> getMask(const Glib::ustring& path,
                                               const Glib::ustring& workingProfile,
                                               procparams::DoubleExposureParams::MaskClass cls,
                                               double feather, bool invert, const MaskPaint& paint,
                                               bool multiThread);

    // Cache-only lookup. The picker's preview uses this so a redraw never
    // stalls on a segmentation; a warm-up request on its own worker fills
    // the cache and asks for another redraw.
    std::shared_ptr<const PartnerMask> peekMask(const Glib::ustring& path,
                                                const Glib::ustring& workingProfile,
                                                procparams::DoubleExposureParams::MaskClass cls,
                                                double feather, bool invert, const MaskPaint& paint);

    // What fraction of the partner each class covers, 0..1, indexed by
    // AISegClass. Empty until the file has been measured, which warmCoverage
    // does; a redraw must never wait on it.
    std::vector<float> getCoverage(const Glib::ustring& path, const Glib::ustring& workingProfile);

    // Whether a current reading exists — the gate on asking for one, kept
    // apart from getCoverage because that one will hand back an older reading
    // rather than let the numbers vanish.
    bool hasCoverage(const Glib::ustring& path, const Glib::ustring& workingProfile);

    // Segments the partner for its coverage alone, with no mask to show for
    // it. Blocking: call it on a worker, as the picker does.
    bool warmCoverage(const Glib::ustring& path, const Glib::ustring& workingProfile,
                      bool multiThread);

    void clearCache();

private:
    PartnerMaskStore();

    static Glib::ustring makeKey(const Glib::ustring& path, const Glib::ustring& workingProfile,
                                 procparams::DoubleExposureParams::MaskClass cls,
                                 double feather, bool invert, const MaskPaint& paint);

    static Glib::ustring coverageKey(const Glib::ustring& path, const Glib::ustring& workingProfile,
                                     bool subjectReady);
    static Glib::ustring coverageKey(const Glib::ustring& path, const Glib::ustring& workingProfile);

    Cache<Glib::ustring, std::shared_ptr<PartnerMask>> cache;
    Cache<Glib::ustring, std::shared_ptr<std::vector<float>>> coverageCache;
};

} // namespace rtengine

#endif // RT_AI_MASKING
