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
#include "partnerimagestore.h"

#include <algorithm>

#include <glibmm/fileutils.h>

#include "colortemp.h"
#include "imagefloat.h"
#include "procparams.h"
#include "rawimagesource.h"
#include "stdimagesource.h"

namespace
{

bool hasNonRawExtension(const Glib::ustring& path)
{
    const auto pos = path.rfind('.');

    if (pos == Glib::ustring::npos) {
        return false;
    }

    const Glib::ustring ext = path.substr(pos + 1).lowercase();
    return ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "tif" || ext == "tiff";
}

// Decode a file to scene-referred linear working-space RGB. The partner is
// rendered neutrally (camera WB, default input profile, no creative edits),
// matching the physics of light hitting the same frame of film.
std::shared_ptr<rtengine::PartnerImage> decodePartner(const Glib::ustring& path, const Glib::ustring& workingProfile, bool fullRes)
{
    using namespace rtengine;

    if (!Glib::file_test(path, Glib::FILE_TEST_EXISTS)) {
        return nullptr;
    }

    procparams::ProcParams params;
    params.raw.deadPixelFilter = false;
    params.raw.ca_autocorrect = false;

    if (!fullRes) {
        // The preview tier never needs full detail: fast demosaic.
        params.raw.bayersensor.method = procparams::RAWParams::BayerSensor::getMethodString(procparams::RAWParams::BayerSensor::Method::FAST);
        params.raw.xtranssensor.method = procparams::RAWParams::XTransSensor::getMethodString(procparams::RAWParams::XTransSensor::Method::FAST);
    }

    std::unique_ptr<ImageSource> src;

    if (hasNonRawExtension(path)) {
        auto stdSrc = std::unique_ptr<StdImageSource>(new StdImageSource());

        if (stdSrc->load(path) == 0) {
            src = std::move(stdSrc);
        }
    }

    if (!src) {
        auto rawSrc = std::unique_ptr<RawImageSource>(new RawImageSource());

        if (rawSrc->load(path) == 0) {
            src = std::move(rawSrc);
        }
    }

    if (!src && !hasNonRawExtension(path)) {
        // Unrecognized raw: last attempt via the generic loader.
        auto stdSrc = std::unique_ptr<StdImageSource>(new StdImageSource());

        if (stdSrc->load(path) == 0) {
            src = std::move(stdSrc);
        }
    }

    if (!src) {
        return nullptr;
    }

    float reddeha = 0.f, greendeha = 0.f, bluedeha = 0.f;
    src->preprocess(params.raw, params.lensProf, params.coarse, reddeha, greendeha, bluedeha, false);
    double contrastThresholdDummy = 0.0;
    src->demosaic(params.raw, false, contrastThresholdDummy);

    const ColorTemp wb = src->getWB();

    int fw = 0, fh = 0;
    src->getFullSize(fw, fh, TR_NONE);

    if (fw <= 0 || fh <= 0) {
        return nullptr;
    }

    int skip = 1;

    if (!fullRes) {
        const int longEdge = std::max(fw, fh);
        skip = std::max(1, (longEdge + 2559) / 2560);
    }

    const PreviewProps pp(0, 0, fw, fh, skip);
    int w = 0, h = 0;
    src->getSize(pp, w, h);

    if (w <= 0 || h <= 0) {
        return nullptr;
    }

    auto partner = std::make_shared<PartnerImage>();
    partner->image.reset(new Imagefloat(w, h));
    partner->fullWidth = fw;
    partner->fullHeight = fh;
    partner->skip = skip;

    src->getImage(wb, TR_NONE, partner->image.get(), pp, params.toneCurve, params.raw);

    if (!workingProfile.empty()) {
        params.icm.workingProfile = workingProfile;
    }

    src->convertColorSpace(partner->image.get(), params.icm, wb);

    return partner;
}

} // namespace

namespace rtengine
{

PartnerImage::PartnerImage() :
    fullWidth(0),
    fullHeight(0),
    skip(1)
{
}

PartnerImage::~PartnerImage() = default;

PartnerImageStore::PartnerImageStore() :
    previewCache(8),
    fullCache(3)
{
}

PartnerImageStore& PartnerImageStore::getInstance()
{
    static PartnerImageStore instance;
    return instance;
}

std::shared_ptr<PartnerImage> PartnerImageStore::getPartner(const Glib::ustring& path, const Glib::ustring& workingProfile, bool fullRes)
{
    if (path.empty()) {
        return nullptr;
    }

    const Glib::ustring key = path + "\n" + workingProfile;
    Cache<Glib::ustring, std::shared_ptr<PartnerImage>>& cache = fullRes ? fullCache : previewCache;

    std::shared_ptr<PartnerImage> result;

    if (cache.get(key, result)) {
        return result;
    }

    result = decodePartner(path, workingProfile, fullRes);

    if (result) {
        cache.insert(key, result);
    }

    return result;
}

void PartnerImageStore::clearFullResTier()
{
    fullCache.clear();
}

void PartnerImageStore::clearCache()
{
    previewCache.clear();
    fullCache.clear();
}

} // namespace rtengine
