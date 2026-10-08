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
#include <map>
#include <mutex>

#include <glibmm/fileutils.h>

#include <glib/gstdio.h>
#include <sys/stat.h>

#include "colortemp.h"
#include "iimage.h"
#include "imagefloat.h"
#include "procparams.h"
#include "processingjob.h"
#include "rawimagesource.h"
#include "rtengine.h"
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
std::shared_ptr<rtengine::PartnerImage> decodePartner(const Glib::ustring& path, const Glib::ustring& workingProfile, bool fullRes,
                                                      const rtengine::ColorTemp* wbOverride)
{
    using namespace rtengine;

    if (!Glib::file_test(path, Glib::FILE_TEST_EXISTS)) {
        return nullptr;
    }

    procparams::ProcParams params;
    params.raw.deadPixelFilter = false;
    params.raw.ca_autocorrect = false;
    // Sensor-clipped highlights must be reconstructed, or the white-balance
    // multipliers leave them clipped at different per-channel levels — a
    // magenta cast the composite then smears over the base ("purple blown
    // skies"). Blend recovery runs inline in getImage and neutralizes them.
    params.toneCurve.hrenabled = true;
    params.toneCurve.method = "Blend";

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

    // Rendered files (JPEG/TIFF) already carry their white balance in the
    // pixels; only raw decodes take the override.
    const bool isRaw = dynamic_cast<RawImageSource*>(src.get()) != nullptr;
    const ColorTemp wb = (wbOverride && isRaw) ? *wbOverride : src->getWB();

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

// Develop the partner as its own sidecar says, through the engine's export
// pipeline, into scene-linear working-space RGB. The preview tier uses the
// fast pipeline (demosaic and geometry at full size, an early resize to a
// 2560 pixel long edge, every later stage at that size). fullWidth/fullHeight
// report the size the full development has, so both tiers share one frame
// and `skip` carries the preview's reduction.
std::shared_ptr<rtengine::PartnerImage> developPartner(const Glib::ustring& path, const Glib::ustring& workingProfile, bool fullRes)
{
    using namespace rtengine;

    if (!Glib::file_test(path, Glib::FILE_TEST_EXISTS)) {
        return nullptr;
    }

    procparams::ProcParams params;
    const Glib::ustring sidecar = path + ".pp3";

    if (!Glib::file_test(sidecar, Glib::FILE_TEST_EXISTS) || params.load(sidecar) != 0) {
        return nullptr;
    }

    // The partner's own exposures are not nested: a partner that itself
    // composites this photograph would never finish.
    params.doubleExposure.enabled = false;
    params.resize.enabled = false;
    params.framing.enabled = false;

    int error = 0;
    const bool raw = !hasNonRawExtension(path);
    InitialImage* initial = InitialImage::load(path, raw, &error);

    if (error || !initial) {
        return nullptr;
    }

    struct Release {
        InitialImage* image;
        ~Release()
        {
            image->decreaseRef();
        }
    } release{initial};

    int fw = 0, fh = 0;
    ImageSource* imgsrc = initial->getImageSource();

    if (imgsrc->getSensorType() == ST_BAYER) {
        imgsrc->setBorder(params.raw.bayersensor.method == procparams::RAWParams::BayerSensor::getMethodString(procparams::RAWParams::BayerSensor::Method::PIXELSHIFT)
                          ? std::max(params.raw.bayersensor.border, 2) : params.raw.bayersensor.border);
    } else if (imgsrc->getSensorType() == ST_FUJI_XTRANS) {
        imgsrc->setBorder(params.raw.xtranssensor.border);
    }

    imgsrc->getFullSize(fw, fh, getCoarseBitMask(params.coarse));

    if (fw <= 0 || fh <= 0) {
        return nullptr;
    }

    // The crop clamping the engine applies, so the frame recorded here is
    // the one the full development reports.
    int outW = fw, outH = fh;

    if (params.crop.enabled) {
        if (params.crop.x > fw || params.crop.y > fh) {
            outW = fw;
            outH = fh;
        } else {
            const int cx = std::max(0, params.crop.x), cy = std::max(0, params.crop.y);
            outW = std::max(1, std::min(params.crop.w, fw - cx));
            outH = std::max(1, std::min(params.crop.h, fh - cy));
        }
    }

    if (!fullRes) {
        params.resize.enabled = true;
        params.resize.appliesTo = "Cropped area";
        params.resize.method = "Lanczos";
        params.resize.dataspec = 3;
        params.resize.width = params.resize.height = 2560;
        params.resize.allowUpscaling = false;
        params.raw.bayersensor.method = procparams::RAWParams::BayerSensor::getMethodString(procparams::RAWParams::BayerSensor::Method::FAST);
        params.raw.xtranssensor.method = procparams::RAWParams::XTransSensor::getMethodString(procparams::RAWParams::XTransSensor::Method::FAST);
    }

    auto job = new ProcessingJobImpl(initial, params, !fullRes);
    job->linearWorkingProfile = workingProfile.empty() ? Glib::ustring("ProPhoto") : workingProfile;
    IImagefloat* developed = processImage(job, error, nullptr, false);

    if (error || !developed) {
        return nullptr;
    }

    Imagefloat* image = dynamic_cast<Imagefloat*>(developed);

    if (!image) {
        delete developed;
        return nullptr;
    }

    auto partner = std::make_shared<PartnerImage>();
    partner->image.reset(image);
    partner->fullWidth = outW;
    partner->fullHeight = outH;
    partner->skip = std::max(1.0, static_cast<double>(outW) / std::max(1, image->getWidth()));
    return partner;
}

} // namespace

namespace rtengine
{

Glib::ustring PartnerImageStore::filePath(const Glib::ustring& source, bool* asEdited)
{
    static const Glib::ustring tag = "|edited";
    const bool tagged = source.size() > tag.size() && source.substr(source.size() - tag.size()) == tag;

    if (asEdited) {
        *asEdited = tagged;
    }

    return tagged ? source.substr(0, source.size() - tag.size()) : source;
}

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

std::shared_ptr<PartnerImage> PartnerImageStore::getPartner(const Glib::ustring& source, const Glib::ustring& workingProfile, bool fullRes,
                                                            const ColorTemp* wbOverride)
{
    bool asEdited = false;
    const Glib::ustring path = filePath(source, &asEdited);

    if (path.empty()) {
        return nullptr;
    }

    Glib::ustring key = source + "\n" + workingProfile;

    if (asEdited) {
        // The sidecar's own revision keys the cache: an edit made to the
        // partner must reach the composite on the next render.
        const Glib::ustring sidecar = path + ".pp3";
        GStatBuf st;
        key += Glib::ustring::compose("\nedited=%1", g_stat(sidecar.c_str(), &st) == 0 ? static_cast<long long>(st.st_mtime) : 0LL);
    }

    if (wbOverride) {
        key += Glib::ustring::compose("\nwb=%1,%2,%3,%4", wbOverride->getTemp(), wbOverride->getGreen(),
                                      wbOverride->getEqual(), static_cast<int>(wbOverride->getObserver()));
    }

    Cache<Glib::ustring, std::shared_ptr<PartnerImage>>& cache = fullRes ? fullCache : previewCache;

    std::shared_ptr<PartnerImage> result;

    if (cache.get(key, result)) {
        return result;
    }

    // The dialog and editor can ask for the same RAW concurrently. Share the
    // decode, without blocking requests for unrelated photographs.
    static std::mutex gatesMutex;
    static std::map<Glib::ustring, std::weak_ptr<std::mutex>> gates;
    std::shared_ptr<std::mutex> gate;
    {
        std::lock_guard<std::mutex> guard(gatesMutex);
        for (auto i = gates.begin(); i != gates.end();)
            if (i->second.expired()) i = gates.erase(i); else ++i;
        auto& weak = gates[key + (fullRes ? "\nfull" : "\npreview")];
        gate = weak.lock();
        if (!gate) weak = gate = std::make_shared<std::mutex>();
    }
    std::lock_guard<std::mutex> decodeGuard(*gate);
    if (cache.get(key, result)) return result;

    result = asEdited ? developPartner(path, workingProfile, fullRes) : nullptr;

    if (!result) {
        // No sidecar, or the development failed: the neutral decode stands
        // in, which at least keeps the layer visible.
        result = decodePartner(path, workingProfile, fullRes, wbOverride);
    }

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
