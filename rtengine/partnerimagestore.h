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

class Imagefloat;

// A partner image decoded to scene-referred linear RGB in a given working
// profile, for compositing by the double exposure tool. `skip` records the
// subsampling of `image` relative to the partner's full frame.
struct PartnerImage {
    std::unique_ptr<Imagefloat> image;
    int fullWidth;
    int fullHeight;
    int skip;

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
    // calling (processing) thread; results are cached.
    std::shared_ptr<PartnerImage> getPartner(const Glib::ustring& path, const Glib::ustring& workingProfile, bool fullRes);

    // Frees the (large) full-resolution tier, e.g. after a batch export.
    void clearFullResTier();
    void clearCache();

private:
    PartnerImageStore();

    Cache<Glib::ustring, std::shared_ptr<PartnerImage>> previewCache;
    Cache<Glib::ustring, std::shared_ptr<PartnerImage>> fullCache;
};

} // namespace rtengine
