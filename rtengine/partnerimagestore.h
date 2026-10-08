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

#include <memory>

#include <glibmm/ustring.h>

#include "cache.h"

namespace rtengine
{

class ColorTemp;
class Imagefloat;

// A partner image decoded to scene-referred linear RGB in a given working
// profile, for compositing by the double exposure tool. `skip` records the
// subsampling of `image` relative to the partner's full frame.
struct PartnerImage {
    std::unique_ptr<Imagefloat> image;
    int fullWidth;
    int fullHeight;
    double skip;    // fractional for a partner rendered as edited, whose preview tier is a resize

    PartnerImage();
    ~PartnerImage();
};

// CLUTStore-style LRU cache of decoded partner images. Two tiers: a preview
// tier decoded with a skip (long edge ~2560px) serving interactive edits, and
// a full-resolution tier for skip==1 detail windows and export.
class PartnerImageStore final
{
public:
    static PartnerImageStore& getInstance();

    PartnerImageStore(const PartnerImageStore&) = delete;
    PartnerImageStore& operator=(const PartnerImageStore&) = delete;

    // Returns the decoded partner in the given working profile, or nullptr if
    // the file is missing or cannot be decoded. Decoding happens on the
    // calling (processing) thread; results are cached. `wbOverride` replaces
    // the file's own camera white balance (raw files only; rendered files
    // already carry theirs) and is part of the cache key.
    // `path` may carry the "|edited" tag Layer::source() appends: the partner
    // is then developed with its own saved edits (crop, geometry, tone, film)
    // through the engine's own pipeline instead of decoded neutrally. The
    // preview tier is a 2560 pixel resize of that development.
    std::shared_ptr<PartnerImage> getPartner(const Glib::ustring& path, const Glib::ustring& workingProfile, bool fullRes,
                                             const ColorTemp* wbOverride = nullptr);

    // The file behind a possibly tagged source, and whether it was tagged.
    static Glib::ustring filePath(const Glib::ustring& source, bool* asEdited = nullptr);

    // Frees the (large) full-resolution tier, e.g. after a batch export.
    void clearFullResTier();
    void clearCache();

private:
    PartnerImageStore();

    Cache<Glib::ustring, std::shared_ptr<PartnerImage>> previewCache;
    Cache<Glib::ustring, std::shared_ptr<PartnerImage>> fullCache;
};

} // namespace rtengine
