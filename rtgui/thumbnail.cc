/*
 *  This file is part of RawTherapee.
 *
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

#include "cachemanager.h"
#include "selectsindex.h"
#include "multilangmgr.h"
#include "thumbnail.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <future>
#include <sstream>
#include <iomanip>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include "rtengine/colortemp.h"
#include "rtengine/imagedata.h"
#include "rtengine/procparams.h"
#include "rtengine/rtthumbnail.h"
#include <glib/gstdio.h>
#include <glibmm/keyfile.h>
#include <glibmm/timezone.h>

#include "rtengine/dynamicprofile.h"
#include "rtengine/metadata.h"
#include "rtengine/profilestore.h"
#include "rtengine/settings.h"
#include "rtengine/utils.h"
#include "guiutils.h"
#include "batchqueue.h"
#include "extprog.h"
#include "md5helper.h"
#include "pathutils.h"
#include "paramsedited.h"
#include "ppversion.h"
#include "procparamchangers.h"
#include "version.h"

#ifdef _WIN32
#include "rtengine/leanwindows.h"
#include <shellapi.h>
#endif // _WIN32

namespace {

using ThumbnailBenchClock = std::chrono::steady_clock;

bool thumbnailBenchEnabled()
{
    static const bool enabled = g_getenv("RT_THUMBNAIL_BENCH") != nullptr;
    return enabled;
}

long long thumbnailBenchMs(ThumbnailBenchClock::time_point start, ThumbnailBenchClock::time_point end)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
}

bool isRawOriginalExtension(const std::string& extension)
{
    static constexpr const char* rawExtensions[] = {
        "3fr", "arw", "arq", "cr2", "cr3", "crf", "crw", "dcr", "dng",
        "fff", "iiq", "kdc", "mef", "mos", "mrw", "nef", "nrw", "orf",
        "ori", "pef", "raf", "raw", "rw2", "rwl", "rwz", "sr2", "srf",
        "srw", "x3f"
    };

    for (const char* rawExtension : rawExtensions) {
        if (extension == rawExtension) {
            return true;
        }
    }

    return false;
}

Glib::ustring findPairedJpegPreview(const Glib::ustring& rawPath)
{
    static constexpr const char* jpegExtensions[] = {
        ".jpg", ".JPG", ".jpeg", ".JPEG"
    };

    const Glib::ustring base = removeExtension(rawPath);

    for (const char* jpegExtension : jpegExtensions) {
        const Glib::ustring candidate = base + jpegExtension;

        if (candidate != rawPath && Glib::file_test(candidate, Glib::FILE_TEST_IS_REGULAR)) {
            return candidate;
        }
    }

    return {};
}

void lowerThumbnailMetadataThreadPriority()
{
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
}

int populateCacheInfoFromMetadata(
    const Glib::ustring& fname,
    CacheImageData& cfs,
    rtengine::FramesMetaData& idata)
{
    int deg = 0;
    cfs.timeValid = false;
    cfs.exifValid = false;
    cfs.exifAbsentKnown = false;

    if (idata.getDateTimeAsTS() > 0) {
        cfs.year         = 1900 + idata.getDateTime().tm_year;
        cfs.month        = idata.getDateTime().tm_mon + 1;
        cfs.day          = idata.getDateTime().tm_mday;
        cfs.hour         = idata.getDateTime().tm_hour;
        cfs.min          = idata.getDateTime().tm_min;
        cfs.sec          = idata.getDateTime().tm_sec;
        cfs.timeValid    = true;
    }

    if (idata.hasExif()) {
        cfs.shutter      = idata.getShutterSpeed ();
        cfs.fnumber      = idata.getFNumber ();
        cfs.focalLen     = idata.getFocalLen ();
        cfs.focalLen35mm = idata.getFocalLen35mm ();
        cfs.focusDist    = idata.getFocusDist ();
        cfs.iso          = idata.getISOSpeed ();
        cfs.expcomp      = idata.expcompToString (idata.getExpComp(), false); // do not mask Zero expcomp
        cfs.isHDR        = idata.getHDR ();
        cfs.isPixelShift = idata.getPixelShift ();
        cfs.frameCount   = idata.getFrameCount ();
        cfs.sampleFormat = idata.getSampleFormat ();
        cfs.lens         = idata.getLens();
        cfs.camMake      = idata.getMake();
        cfs.camModel     = idata.getModel();
        cfs.rating       = idata.getRating();
        cfs.colorLabel   = idata.getColorLabel();
        cfs.exifValid    = true;
        cfs.exifAbsentKnown = false;

        if (idata.getOrientation() == "Rotate 90 CW") {
            deg = 90;
        } else if (idata.getOrientation() == "Rotate 180") {
            deg = 180;
        } else if (idata.getOrientation() == "Rotate 270 CW") {
            deg = 270;
        }
    } else {
        cfs.lens     = "Unknown";
        cfs.camMake  = "Unknown";
        cfs.camModel = "Unknown";
        cfs.exifAbsentKnown = true;
    }
    cfs.updateCameraName();

    std::string::size_type idx;
    idx = fname.rfind('.');

    if(idx != std::string::npos) {
        cfs.filetype = fname.substr(idx + 1);
    } else {
        cfs.filetype = "";
    }
    cfs.updateFiletypeUpper();

    idata.getDimensions(cfs.width, cfs.height);

    return deg;
}

bool CPBDump(
    const Glib::ustring& commFName,
    const Glib::ustring& imageFName,
    const Glib::ustring& profileFName,
    const Glib::ustring& defaultPParams,
    const CacheImageData* cfs,
    bool flagMode
)
{
    const std::unique_ptr<Glib::KeyFile> kf(new Glib::KeyFile);

    if (!kf) {
        return false;
    }

    // open the file in write mode
    const std::unique_ptr<FILE, int (*)(FILE *)> f(g_fopen(commFName.c_str(), "wt"), &std::fclose);

    if (!f) {
        printf ("CPBDump(\"%s\") >>> Error: unable to open file with write access!\n", commFName.c_str());
        return false;
    }

    const auto& options = App::get().options();
    try {
        kf->set_string ("RT General", "CachePath", options.cacheBaseDir);
        kf->set_string ("RT General", "AppVersion", RTVERSION);
        kf->set_integer ("RT General", "ProcParamsVersion", PPVERSION);
        kf->set_string ("RT General", "ImageFileName", imageFName);
        kf->set_string ("RT General", "OutputProfileFileName", profileFName);
        kf->set_string ("RT General", "DefaultProcParams", defaultPParams);
        kf->set_boolean ("RT General", "FlaggingMode", flagMode);

        kf->set_integer ("Common Data", "FrameCount", cfs->frameCount);
        kf->set_integer ("Common Data", "SampleFormat", cfs->sampleFormat);
        kf->set_boolean ("Common Data", "IsHDR", cfs->isHDR);
        kf->set_boolean ("Common Data", "IsPixelShift", cfs->isPixelShift);
        kf->set_double ("Common Data", "FNumber", cfs->fnumber);
        kf->set_double ("Common Data", "Shutter", cfs->shutter);
        kf->set_double ("Common Data", "FocalLength", cfs->focalLen);
        kf->set_integer ("Common Data", "ISO", cfs->iso);
        kf->set_string ("Common Data", "Lens", cfs->lens);
        kf->set_string ("Common Data", "Make", cfs->camMake);
        kf->set_string ("Common Data", "Model", cfs->camModel);

    } catch (const Glib::KeyFileError&) {
    }

    try {
        fprintf (f.get(), "%s", kf->to_data().c_str());
    } catch (const Glib::KeyFileError&) {
    }

    return true;
}

/**
 * Gets the rank and color from the image metadata, if they exist.
 *
 * @param cfs The cached image data.
 * @param fname The image's file name.
 * @param rank Where the rank will be stored. If there is no rank in the
 * metadata, the value will not be changed.
 * @param color Where the color will be stored. If there is no color in the
 * metadata, the value will not be changed.
 * @param hasRankPtr Pointer to boolean that stores if the rank is in the
 * metadata. May be null.
 * @param hasColorPtr Pointer to boolean that stores if the color is in the
 * metadata. May be null.
 */
void getRankAndColorFromMetadata(
    const CacheImageData &cfs,
    const Glib::ustring &fname,
    int &rank,
    int &color,
    bool *hasRankPtr,
    bool *hasColorPtr)
{
    const auto setHasMetadataFlags = [=](bool has_rank, bool has_color) {
        if (hasRankPtr) {
            *hasRankPtr = has_rank;
        }
        if (hasColorPtr) {
            *hasColorPtr = has_color;
        }
    };

    if (cfs.exifValid) {
        rank = rtengine::LIM(cfs.getRating(), 0, 5);
        color = rtengine::LIM(cfs.getColorLabel(), 0, 5);
        setHasMetadataFlags(true, true);
        return;
    }
    if (cfs.exifAbsentKnown) {
        setHasMetadataFlags(false, false);
        return;
    }
    const std::unique_ptr<const rtengine::FramesMetaData> md(rtengine::FramesMetaData::fromFile(fname));
    if (md && md->hasExif()) {
        rank = rtengine::LIM(md->getRating(), 0, 5);
        bool has_color = false;
        const auto color_label = md->getColorLabel();
        if (color_label >= 1 && color_label <= 5) {
            color = color_label;
            has_color = true;
        }
        setHasMetadataFlags(true, has_color);
        return;
    }
    setHasMetadataFlags(false, false);
}

/**
 * Gets the rank from the image metadata, if it exists.
 *
 * @param cfs The cached image data.
 * @param fname The image's file name.
 * @param rank Where the rank will be stored. If there is no rank in the
 * metadata, the value will not be changed.
 * @returns If the rank is in the metadata.
 */
bool getRankFromMetadata(
    const CacheImageData &cfs, const Glib::ustring &fname, int &rank)
{
    bool has_rank = false;
    int color;
    getRankAndColorFromMetadata(cfs, fname, rank, color, &has_rank, nullptr);
    return has_rank;
}

/**
 * Gets the rank from the XMP.
 *
 * @param xmp The XMP data.
 * @param rank Where the rank will be stored. If there is no rank in the XMP,
 * the value will not be changed.
 * @returns If the rank is in the XMP.
 */
bool getRankFromXmp(const Exiv2::XmpData &xmp, int &rank)
{
    auto pos = xmp.findKey(Exiv2::XmpKey("Xmp.xmp.Rating"));
    if (pos != xmp.end()) {
        int r = rtengine::to_long(pos);
        rank = rtengine::LIM(r, 0, 5);
        return true;
    }
    return false;
}

/**
 * Gets the rank from the XMP or image metadata.
 *
 * The priority is to load from the XMP. The XMP will only be used if the
 * option's thumbnail rank/color mode is set to XMP. If no rank is retrieved
 * from the XMP, an attempt to get the rank from the metadata will be made.
 *
 * @param options Options.
 * @param xmp The XMP data.
 * @param cfs The cached image data.
 * @param fname The image's file name.
 * @param rank Where the rank will be stored. If there is no rank retrieved from
 * the XMP and there is no rank in the metadata, the value will not be changed.
 * @returns If a rank was retrieved.
 */
bool getRankFromXmpOrMetadata(
    const Options &options,
    const Exiv2::XmpData &xmp,
    const CacheImageData &cfs,
    const Glib::ustring &fname,
    int &rank)
{
    bool got_rank_from_xmp = false;
    if (options.thumbnailRankColorMode == Options::ThumbnailPropertyMode::XMP) {
        try {
            got_rank_from_xmp = getRankFromXmp(xmp, rank);
        } catch (std::exception &exc) {
            std::cerr << "ERROR loading rank from "
                      << rtengine::Exiv2Metadata::xmpSidecarPath(fname)
                      << ": " << exc.what() << std::endl;
        }
    }
    return got_rank_from_xmp || getRankFromMetadata(cfs, fname, rank);
}

/**
 * Gets the color label from the XMP.
 *
 * @param xmp The XMP data.
 * @param color Where the color will be stored. If there is no color in the XMP,
 * the value will not be changed.
 * @returns If the color is in the XMP.
 */
bool getColorFromXmp(const Exiv2::XmpData &xmp, int &color)
{
    auto pos = xmp.findKey(Exiv2::XmpKey("Xmp.xmp.Label"));
    if (pos != xmp.end()) {
        color = rtengine::FramesData::xmp_label2color(pos->toString());
        return true;
    }
    return false;
}

/**
 * Gets the color label from the XMP.
 *
 * The XMP will only be used if the option's thumbnail rank/color mode is set to
 * XMP.
 *
 * @param options Options.
 * @param xmp The XMP data.
 * @param fname The image's file name.
 * @param color Where the color will be stored. If there is no color in the XMP,
 * the value will not be changed.
 * @returns If the color is in the XMP.
 */
bool getColorFromXmpOrNone(
    const Options &options,
    const Exiv2::XmpData &xmp,
    const Glib::ustring &fname,
    int &color)
{
    if (options.thumbnailRankColorMode == Options::ThumbnailPropertyMode::XMP) {
        try {
            return getColorFromXmp(xmp, color);
        } catch (std::exception &exc) {
            std::cerr << "ERROR loading color label from "
                      << rtengine::Exiv2Metadata::xmpSidecarPath(fname)
                      << ": " << exc.what() << std::endl;
        }
    }
    return false;
}

} // namespace

using namespace rtengine::procparams;

Thumbnail::Thumbnail(CacheManager* cm, const Glib::ustring& fname, CacheImageData* cf, const Glib::ustring& cacheBaseName) :
    fname(fname),
    cfs(*cf),
    cacheBaseName_(cacheBaseName),
    cachemgr(cm),
    ref(1),
    enqueueNumber(0),
    tpp(nullptr),
    pparams(new ProcParams),
    pparamsValid(false),
    imageLoading(false),
    lastImg(nullptr),
    lastW(0),
    lastH(0),
    lastScale(0),
    initial_(false)
{

    loadProcParams (false);

    initCachedThumbnailSize ();

    if (cfs.rankOld >= 0) {
        // rank and inTrash were found in cache (old style), move them over to pparams or xmp sidecar

        // try to load the last saved parameters from the cache or from the paramfile file
        createProcParamsForUpdate(false, false); // this can execute customprofilebuilder to generate param file

        // TODO? should we call notifylisterners_procParamsChanged here?

        setRank(cfs.rankOld);
        setTrashed(cfs.inTrashOld);
    }

    loadProperties();

    delete tpp;
    tpp = nullptr;
}

void Thumbnail::initCachedThumbnailSize()
{
    const auto& options = App::get().options();
    tw = options.maxThumbnailWidth;
    th = options.maxThumbnailHeight;
    imgRatio = -1.f;

    if (cfs.width <= 0 || cfs.height <= 0) {
        return;
    }

    const auto& pparams = getProcParamsU();
    if (pparams.coarse.rotate == 90 || pparams.coarse.rotate == 270) {
        imgRatio = static_cast<float>(cfs.height) / static_cast<float>(cfs.width);
    } else {
        imgRatio = static_cast<float>(cfs.width) / static_cast<float>(cfs.height);
    }

    tw = std::max(static_cast<int>(imgRatio * static_cast<float>(th)), 1);
}

Thumbnail::Thumbnail(CacheManager* cm, const Glib::ustring& fname, const std::string& md5, const std::string &xmpSidecarMd5, const Glib::ustring& cacheBaseName) :
    fname(fname),
    cacheBaseName_(cacheBaseName),
    cachemgr(cm),
    ref(1),
    enqueueNumber(0),
    tpp(nullptr),
    pparams(new ProcParams),
    pparamsValid(false),
    imageLoading(false),
    lastImg(nullptr),
    lastW(0),
    lastH(0),
    lastScale(0.0),
    initial_(true)
{


    cfs.md5 = md5;
    cfs.xmpSidecarMd5 = xmpSidecarMd5;
    loadProcParams (false);
    _generateThumbnailImage ();
    cfs.recentlySaved = false;

    initial_ = false;

    loadProperties();

    delete tpp;
    tpp = nullptr;
}

Glib::ustring Thumbnail::xmpSidecarPath(const Glib::ustring &imagePath)
{
    return rtengine::Exiv2Metadata::xmpSidecarPath(imagePath);
}

void Thumbnail::_generateThumbnailImage()
{
    const bool bench = thumbnailBenchEnabled();
    const auto benchStart = bench ? ThumbnailBenchClock::now() : ThumbnailBenchClock::time_point{};
    long long imageProbeMs = 0;
    long long quickRawMs = 0;
    long long fullRawMs = 0;
    long long infoMs = 0;
    long long saveMs = 0;
    bool quick = false;
    std::future<std::unique_ptr<rtengine::FramesMetaData>> rawMetadataFuture;
    bool rawMetadataFutureStarted = false;

    auto startRawMetadataFuture = [&]() {
        if (rawMetadataFutureStarted) {
            return;
        }

        rawMetadataFutureStarted = true;
        const Glib::ustring metadataFname = fname;
        rawMetadataFuture = std::async(
            std::launch::async,
            [metadataFname]() {
                lowerThumbnailMetadataThreadPriority();
                return std::unique_ptr<rtengine::FramesMetaData>(rtengine::FramesMetaData::fromFile(metadataFname));
            });
    };

    auto applyRawInfoFromFuture = [&]() {
        if (!rawMetadataFutureStarted) {
            return infoFromImage(fname);
        }

        auto idata = rawMetadataFuture.get();
        rawMetadataFutureStarted = false;
        return idata ? populateCacheInfoFromMetadata(fname, cfs, *idata) : 0;
    };

    //  delete everything loaded into memory
    delete tpp;
    tpp = nullptr;
    delete[] lastImg;
    lastImg = nullptr;
    const auto& options = App::get().options();
    tw = options.maxThumbnailWidth;
    th = options.maxThumbnailHeight;
    imgRatio = -1.;

    // generate thumbnail image
    const std::string ext = getExtension(fname).lowercase().raw();

    if (ext.empty()) {
        return;
    }

    cfs.supported = false;
    cfs.exifValid = false;
    cfs.timeValid = false;

    // This loads formats supported by imagio (jpg, png, jxl, and tiff).
    // Known RAW originals can skip this guaranteed-fail probe and go straight
    // to the RAW embedded-preview loader.
    if (!isRawOriginalExtension(ext)) {
        const auto stageStart = bench ? ThumbnailBenchClock::now() : ThumbnailBenchClock::time_point{};
        tpp = rtengine::Thumbnail::loadFromImage(fname, tw, th, -1, pparams->wb.equal, pparams->wb.observer);
        if (bench) {
            imageProbeMs = thumbnailBenchMs(stageStart, ThumbnailBenchClock::now());
        }
    }

    if (tpp) {
        cfs.format = FT_Custom;
        const auto stageStart = bench ? ThumbnailBenchClock::now() : ThumbnailBenchClock::time_point{};
        infoFromImage(fname);
        if (bench) {
            infoMs += thumbnailBenchMs(stageStart, ThumbnailBenchClock::now());
        }
    }

    if (!tpp) {
        // RAW works like this:
        //  1. if we are here it's because we aren't in the cache so load the JPG
        //     image out of the RAW. Mark as "quick".
        //  2. if we don't find that then just grab the real image.
        rtengine::eSensorType sensorType = rtengine::ST_NONE;

        if (isRawOriginalExtension(ext)) {
            startRawMetadataFuture();
        }

        if (initial_ && options.internalThumbIfUntouched) {
            quick = true;
            const auto stageStart = bench ? ThumbnailBenchClock::now() : ThumbnailBenchClock::time_point{};
            tpp = rtengine::Thumbnail::loadQuickFromRaw(fname, sensorType, tw, th, 1, TRUE);
            if (bench) {
                quickRawMs = thumbnailBenchMs(stageStart, ThumbnailBenchClock::now());
            }
        }

        if (!tpp && quick) {
            const Glib::ustring pairedJpegPreview = findPairedJpegPreview(fname);

            if (!pairedJpegPreview.empty()) {
                const auto stageStart = bench ? ThumbnailBenchClock::now() : ThumbnailBenchClock::time_point{};
                tpp = rtengine::Thumbnail::loadFromImage(pairedJpegPreview, tw, th, -1, pparams->wb.equal, pparams->wb.observer);
                if (bench) {
                    quickRawMs += thumbnailBenchMs(stageStart, ThumbnailBenchClock::now());
                }
            }
        }

        if (!tpp) {
            quick = false;
            const auto stageStart = bench ? ThumbnailBenchClock::now() : ThumbnailBenchClock::time_point{};
            tpp = rtengine::Thumbnail::loadFromRaw(fname, sensorType, tw, th, 1, pparams->wb.equal, pparams->wb.observer, TRUE, &(pparams->raw));
            if (bench) {
                fullRawMs = thumbnailBenchMs(stageStart, ThumbnailBenchClock::now());
            }
        }

        cfs.sensortype = sensorType;

        if (tpp) {
            cfs.format = FT_Raw;
            cfs.thumbImgType = quick ? CacheImageData::QUICK_THUMBNAIL : CacheImageData::FULL_THUMBNAIL;
            const auto stageStart = bench ? ThumbnailBenchClock::now() : ThumbnailBenchClock::time_point{};
            applyRawInfoFromFuture();
            if (bench) {
                infoMs += thumbnailBenchMs(stageStart, ThumbnailBenchClock::now());
            }

            // Embedded RAW previews can report the embedded JPEG dimensions
            // through Exiv2; the editor placeholder needs the full RAW size.
            if (tpp->full_width > 0 && tpp->full_height > 0) {
                cfs.width = tpp->full_width;
                cfs.height = tpp->full_height;
            }
        }
    }

    if (!tpp && rawMetadataFutureStarted) {
        rawMetadataFuture.get();
        rawMetadataFutureStarted = false;
    }

    if (tpp) {
        tpp->getAutoWBMultipliers(cfs.redAWBMul, cfs.greenAWBMul, cfs.blueAWBMul);
        const Glib::ustring cacheDataName = getCacheFileName("data", ".txt");
        const auto stageStart = bench ? ThumbnailBenchClock::now() : ThumbnailBenchClock::time_point{};
        cachemgr->noteCacheFileWritten(cacheDataName);
        _saveThumbnail(false);
        cfs.supported = true;

        cfs.save(cacheDataName, tpp);
        cachemgr->noteCacheFileWritten(cacheDataName);
        if (bench) {
            saveMs = thumbnailBenchMs(stageStart, ThumbnailBenchClock::now());
        }

        invalidateExifDateTimeStrings();
    }

    if (bench) {
        std::fprintf(
            stdout,
            "RT_THUMBNAIL_BENCH total_ms=%lld image_ms=%lld quickraw_ms=%lld fullraw_ms=%lld info_ms=%lld save_ms=%lld raw=%d quick=%d supported=%d ext=%s file=\"%s\"\n",
            thumbnailBenchMs(benchStart, ThumbnailBenchClock::now()),
            imageProbeMs,
            quickRawMs,
            fullRawMs,
            infoMs,
            saveMs,
            static_cast<int>(cfs.format == FT_Raw),
            static_cast<int>(quick),
            static_cast<int>(tpp != nullptr),
            ext.c_str(),
            fname.c_str());
        std::fflush(stdout);
    }
}

bool Thumbnail::isSupported () const
{
    return cfs.supported;
}

const ProcParams& Thumbnail::getProcParams ()
{
    MyMutex::MyLock lock(mutex);
    return getProcParamsU();
}

ProcParams Thumbnail::getProcParamsCopy ()
{
    MyMutex::MyLock lock(mutex);
    // The return value is copy-constructed inside this scope, i.e. while the
    // lock is still held — safe against concurrent setProcParams.
    return getProcParamsU();
}

// Unprotected version of getProcParams, when
const ProcParams& Thumbnail::getProcParamsU ()
{
    if (pparamsValid) {
        return *pparams;
    } else {
        *pparams = *(ProfileStore::getInstance()->getDefaultProcParams (getType() == FT_Raw));

        if (pparams->wb.method == "Camera") {
            double ct;
            getCamWB (ct, pparams->wb.green, pparams->wb.observer);
            pparams->wb.temperature = ct;
        } else if (pparams->wb.method == "autold") {
            double ct;
            getAutoWB (ct, pparams->wb.green, pparams->wb.equal, pparams->wb.observer, pparams->wb.tempBias);
            pparams->wb.temperature = ct;
        }
    }

    return *pparams; // there is no valid pp to return, but we have to return something
}

/** @brief  Create default params on demand and returns a new updatable object
 *
 *  The loaded profile may be partial, but it return a complete ProcParams (i.e. without ParamsEdited)
 *
 *  @param returnParams Ask to return a pointer to a ProcParams object if true
 *  @param force True if the profile has to be re-generated even if it already exists
 *  @param flaggingMode True if the ProcParams will be created because the file browser is being flagging an image
 *                      (rank, to trash, color labels). This parameter is passed to the CPB.
 *
 *  @return Return a pointer to a ProcPamas structure to be updated if returnParams is true and if everything went fine, NULL otherwise.
 */
rtengine::procparams::ProcParams* Thumbnail::createProcParamsForUpdate(bool returnParams, bool force, bool flaggingMode)
{
    // try to load the last saved parameters from the cache or from the paramfile file
    ProcParams* ldprof = nullptr;

    const auto& options = App::get().options();
    Glib::ustring defProf = getType() == FT_Raw ? options.defProfRaw : options.defProfImg;

    const CacheImageData* cfs = getCacheImageData();
    Glib::ustring defaultPparamsPath = options.findProfilePath(defProf);
    const bool create = (!hasProcParams() || force);
    const bool run_cpb = !options.CPBPath.empty() && !defaultPparamsPath.empty() && cfs && cfs->exifValid && create;

    const Glib::ustring outFName =
        (options.paramsLoadLocation == PLL_Input && options.saveParamsFile) ?
        fname + App::PARAM_FILE_EXTENSION :
        getCacheFileName("profiles", App::PARAM_FILE_EXTENSION);

    if (!run_cpb) {
        if (defProf == DEFPROFILE_DYNAMIC && create && cfs && cfs->exifValid) {
            const auto pp_deleter =
                [](PartialProfile* pp)
                {
                    pp->deleteInstance();
                    delete pp;
                };
            const std::unique_ptr<const rtengine::FramesMetaData> imageMetaData(rtengine::FramesMetaData::fromFile(fname));
            const std::unique_ptr<PartialProfile, decltype(pp_deleter)> pp(
                imageMetaData
                    ? ProfileStore::getInstance()->loadDynamicProfile(imageMetaData.get(), fname)
                    : nullptr,
                pp_deleter
            );
            cachemgr->noteCacheFileWritten(outFName);
            if (pp && !pp->pparams->save(outFName)) {
                cachemgr->noteCacheFileWritten(outFName);
                loadProcParams();
            }
        } else if (create && defProf != DEFPROFILE_DYNAMIC) {
            const PartialProfile* const p = ProfileStore::getInstance()->getProfile(defProf);
            cachemgr->noteCacheFileWritten(outFName);
            if (p && !p->pparams->save(outFName)) {
                cachemgr->noteCacheFileWritten(outFName);
                loadProcParams();
            }
        }
    } else {
        // First generate the communication file, with general values and EXIF metadata
        static int index = 0; // Will act as unique identifier during the session
        Glib::ustring tmpFileName( Glib::build_filename(options.cacheBaseDir, Glib::ustring::compose("CPB_temp_%1.txt", index++)) );

        CPBDump(tmpFileName, fname, outFName,
                defaultPparamsPath == DEFPROFILE_INTERNAL ? DEFPROFILE_INTERNAL : Glib::build_filename(defaultPparamsPath, Glib::path_get_basename(defProf) + App::PARAM_FILE_EXTENSION), cfs, flaggingMode);

        // For the filename etc. do NOT use streams, since they are not UTF8 safe
        Glib::ustring cmdLine = options.CPBPath + Glib::ustring(" \"") + tmpFileName + Glib::ustring("\"");

        if (rtengine::settings->verbose) {
            printf("Custom profile builder's command line: %s\n", Glib::ustring(cmdLine).c_str());
        }

        bool success = ExtProgStore::spawnCommandSync (cmdLine);

        // Now they SHOULD be there (and potentially "partial"), so try to load them and store it as a full procparam
        if (success) {
            cachemgr->noteCacheFileWritten(outFName);
            loadProcParams();
        }

        g_remove (tmpFileName.c_str ());
    }

    if (returnParams && hasProcParams()) {
        ldprof = new ProcParams ();
        *ldprof = getProcParams ();
    }

    return ldprof;
}

void Thumbnail::notifylisterners_procParamsChanged(int whoChangedIt)
{
    for (size_t i = 0; i < listeners.size(); i++) {
        listeners[i]->procParamsChanged (this, whoChangedIt, false);
    }
}

/*
 * Load the procparams from the cache or from the sidecar file (priority set in
 * the Preferences).
 *
 * The result is a complete ProcParams with default values merged with the values
 * from the loaded ProcParams (sidecar or cache file).
*/
void Thumbnail::loadProcParams(bool resetToDefaults)
{
    MyMutex::MyLock lock(mutex);

    pparamsValid = false;
    if (resetToDefaults) {
        pparams->setDefaults();
    }

    auto loadProfile = [this](const Glib::ustring& profileName, bool requireModernVersion) -> bool {
        bool fileExistsKnown = false;
        const bool presenceKnown = cachemgr->getKnownFilePresence(profileName, fileExistsKnown);
        if (presenceKnown && !fileExistsKnown) {
            return false;
        }

        const int ppres = pparams->load(profileName, nullptr, presenceKnown && fileExistsKnown);
        return !ppres && (!requireModernVersion || pparams->ppVersion >= 220);
    };

    auto loadCacheProfile = [this, &loadProfile]() -> bool {
        return loadProfile(getCacheFileName("profiles", App::PARAM_FILE_EXTENSION), false);
    };

    if (App::get().options().paramsLoadLocation == PLL_Input) {
        // try to load it from params file next to the image file
        pparamsValid = loadProfile(fname + App::PARAM_FILE_EXTENSION, true);

        // if no success, try to load the cached version of the procparams
        if (!pparamsValid) {
            pparamsValid = loadCacheProfile();
        }
    } else {
        // try to load it from cache
        pparamsValid = loadCacheProfile();

        // if no success, try to load it from params file next to the image file
        if (!pparamsValid) {
            pparamsValid = loadProfile(fname + App::PARAM_FILE_EXTENSION, true);
        }
    }
}

void Thumbnail::clearProcParams (int whoClearedIt)
{

    /*  Clarification on current "clear profile" functionality:
        a. if rank/colorlabel/inTrash are NOT set,
        the "clear profile" will delete the pp3 file (as before).

        b. if any of the rank/colorlabel/inTrash ARE set,
        the "clear profile" will lead to execution of ProcParams::setDefaults
        (the CPB is NOT called) to set the params values and will preserve
        rank/colorlabel/inTrash in the param file. */

    {
        MyMutex::MyLock lock(mutex);

        cfs.recentlySaved = false;
        pparamsValid = false;

        //TODO: run though customprofilebuilder?
        // probably not as this is the only option to set param values to default

        // reset the params to defaults
        pparams->setDefaults();

        // preserve rank, colorlabel and inTrash across clear
        updateProcParamsProperties(true);

        // params could get validated by updateProcParamsProperties
        if (pparamsValid) {
            updateCache();
        } else {
            // remove param file from cache
            Glib::ustring fname_ = getCacheFileName ("profiles", App::PARAM_FILE_EXTENSION);
            g_remove (fname_.c_str ());
            cachemgr->invalidateMD5(fname_);

            // remove param file located next to the file
            fname_ = fname + App::PARAM_FILE_EXTENSION;
            g_remove (fname_.c_str ());
            cachemgr->invalidateMD5(fname_);

            fname_ = removeExtension(fname) + App::PARAM_FILE_EXTENSION;
            g_remove (fname_.c_str ());
            cachemgr->invalidateMD5(fname_);

            if (cfs.format == FT_Raw && App::get().options().internalThumbIfUntouched && cfs.thumbImgType != CacheImageData::QUICK_THUMBNAIL) {
                // regenerate thumbnail, ie load the quick thumb again. For the rare formats not supporting quick thumbs this will
                // be a bit slow as a new full thumbnail will be generated unnecessarily, but currently there is no way to pre-check
                // if the format supports quick thumbs.
                initial_ = true;
                _generateThumbnailImage();
                initial_ = false;
            }
        }

    } // end of mutex lock

    for (size_t i = 0; i < listeners.size(); i++) {
        listeners[i]->procParamsChanged (this, whoClearedIt, false);
    }
}

bool Thumbnail::hasProcParams () const
{

    return pparamsValid;
}

void Thumbnail::setProcParams (const ProcParams& pp, ParamsEdited* pe, int whoChangedIt, bool updateCacheNow, bool resetToDefault)
{
    const bool blackLevelChanged =
        pparams->raw.bayersensor.black0 != pp.raw.bayersensor.black0
        || pparams->raw.bayersensor.black1 != pp.raw.bayersensor.black1
        || pparams->raw.bayersensor.black2 != pp.raw.bayersensor.black2
        || pparams->raw.bayersensor.black3 != pp.raw.bayersensor.black3
        || pparams->raw.xtranssensor.blackred != pp.raw.xtranssensor.blackred
        || pparams->raw.xtranssensor.blackgreen != pp.raw.xtranssensor.blackgreen
        || pparams->raw.xtranssensor.blackblue != pp.raw.xtranssensor.blackblue;
    const bool needsReprocessing =
           resetToDefault
        || blackLevelChanged
        || pparams->raw.expos != pp.raw.expos
        || pparams->toneCurve != pp.toneCurve
        || pparams->locallab != pp.locallab
        || pparams->labCurve != pp.labCurve
        || pparams->localContrast != pp.localContrast
        || pparams->rgbCurves != pp.rgbCurves
        || pparams->colorToning != pp.colorToning
        || pparams->vibrance != pp.vibrance
        || pparams->wb != pp.wb
        || pparams->colorappearance != pp.colorappearance
        || pparams->epd != pp.epd
        || pparams->fattal != pp.fattal
        || pparams->sh != pp.sh
        || pparams->toneEqualizer != pp.toneEqualizer
        || pparams->crop != pp.crop
        || pparams->coarse != pp.coarse
        || pparams->commonTrans != pp.commonTrans
        || pparams->rotate != pp.rotate
        || pparams->distortion != pp.distortion
        || pparams->lensProf != pp.lensProf
        || pparams->perspective != pp.perspective
        || pparams->gradient != pp.gradient
        || pparams->pcvignette != pp.pcvignette
        || pparams->cacorrection != pp.cacorrection
        || pparams->vignetting != pp.vignetting
        || pparams->chmixer != pp.chmixer
        || pparams->blackwhite != pp.blackwhite
        || pparams->icm != pp.icm
        || pparams->hsvequalizer != pp.hsvequalizer
        || pparams->filmSimulation != pp.filmSimulation
        || pparams->softlight != pp.softlight
        || pparams->dehaze != pp.dehaze
        || pparams->filmNegative != pp.filmNegative
        || whoChangedIt == FILEBROWSER
        || whoChangedIt == BATCHEDITOR;
    const bool upgradeHint = blackLevelChanged;

    {
        MyMutex::MyLock lock(mutex);

        if (*pparams != pp) {
            cfs.recentlySaved = false;
        } else if (pparamsValid && !updateCacheNow) {
            // nothing to do
            return;
        }

        if (pe) {
            pe->combine(*pparams, pp, true);
        } else {
            *pparams = pp;
        }

        pparamsValid = true;

        // do not update rank, colorlabel and inTrash
        updateProcParamsProperties(true);

        if (updateCacheNow) {
            updateCache();
        }
    } // end of mutex lock

    if (needsReprocessing) {
        for (size_t i = 0; i < listeners.size(); i++) {
            listeners[i]->procParamsChanged (this, whoChangedIt, upgradeHint);
        }
    }
}

bool Thumbnail::procParamsMatch (const rtengine::procparams::ProcParams& pp)
{
    MyMutex::MyLock lock(mutex);

    return pparamsValid && *pparams == pp;
}

bool Thumbnail::isRecentlySaved () const
{

    return cfs.recentlySaved;
}

void Thumbnail::imageDeveloped ()
{

    cfs.recentlySaved = true;
    const Glib::ustring cacheDataName = getCacheFileName ("data", ".txt");
    cachemgr->noteCacheFileWritten(cacheDataName);
    cfs.save (cacheDataName);
    cachemgr->noteCacheFileWritten(cacheDataName);

    if (App::get().options().saveParamsCache) {
        const Glib::ustring profileName = getCacheFileName ("profiles", App::PARAM_FILE_EXTENSION);
        cachemgr->noteCacheFileWritten(profileName);
        pparams->save (profileName);
        cachemgr->noteCacheFileWritten(profileName);
    }
}

void Thumbnail::imageEnqueued ()
{

    enqueueNumber++;
}

void Thumbnail::imageRemovedFromQueue ()
{

    enqueueNumber--;
}

bool Thumbnail::isEnqueued () const
{

    return enqueueNumber > 0;
}

bool Thumbnail::isPixelShift () const
{
    return cfs.isPixelShift;
}
bool Thumbnail::isHDR () const
{
    return cfs.isHDR;
}

void Thumbnail::increaseRef ()
{
    MyMutex::MyLock lock(mutex);
    ++ref;
}

void Thumbnail::decreaseRef ()
{
    {
        MyMutex::MyLock lock(mutex);

        if ( ref == 0 ) {
            return;
        }

        if ( --ref != 0 ) {
            return;
        }
    }
    cachemgr->closeThumbnail (this);
}

int Thumbnail::decreaseRefCacheMgr ()
{
    MyMutex::MyLock lock(mutex);

    if ( ref == 0 ) {
        return 0;
    }

    return --ref;
}

void Thumbnail::getThumbnailSize(int &w, int &h, const rtengine::procparams::ProcParams *pparams)
{
    MyMutex::MyLock lock(mutex);

    int tw_ = tw;
    int th_ = th;

    float imgRatio_ = imgRatio;

    if (pparams) {
        int ppCoarse = pparams->coarse.rotate;

        if (ppCoarse >= 180) {
            ppCoarse -= 180;
        }

        int thisCoarse = this->pparams->coarse.rotate;

        if (thisCoarse >= 180) {
            thisCoarse -= 180;
        }

        if (thisCoarse != ppCoarse) {
            // different orientation -> swapping width & height
            std::swap(th_, tw_);
            if (imgRatio_ >= 0.0001f) {
                imgRatio_ = 1.f / imgRatio_;
            }
        }
    }

    if (imgRatio_ > 0.) {
        w = imgRatio_ * static_cast<float>(h);
    } else {
        w = tw_ * h / th_;
    }

    const auto& options = App::get().options();
    if (w > options.maxThumbnailWidth) {
        const float s = static_cast<float>(options.maxThumbnailWidth) / w;
        w = options.maxThumbnailWidth;
        h = std::max<int>(h * s, 1);
    }
}

void Thumbnail::getFinalSize (const rtengine::procparams::ProcParams& pparams, int& w, int& h)
{
    MyMutex::MyLock lock(mutex);

    // WARNING: When downscaled, the ratio have loosed a lot of precision, so we can't get back the exact initial dimensions
    double fw = lastW * lastScale;
    double fh = lastH * lastScale;

    if (pparams.coarse.rotate == 90 || pparams.coarse.rotate == 270) {
        fh = lastW * lastScale;
        fw = lastH * lastScale;
    }

    if (!pparams.resize.enabled) {
        w = fw;
        h = fh;
    } else {
        w = (int)(fw + 0.5);
        h = (int)(fh + 0.5);
    }
}

void Thumbnail::getOriginalSize (int& w, int& h) const
{
    w = tw;
    h = th;
}

rtengine::IImage8* Thumbnail::processThumbImageLocked (const rtengine::procparams::ProcParams& pparams, int h, double& scale, bool cachePixbuf)
{
    if (!tpp) {
        _loadThumbnail();

        if (!tpp) {
            return nullptr;
        }
    }

    rtengine::IImage8* image = nullptr;

    if ( cfs.thumbImgType == CacheImageData::QUICK_THUMBNAIL ) {
        // RAW internal thumbnail, no profile yet: just do some rotation etc.
        image = tpp->quickProcessImage (pparams, h, rtengine::TI_Nearest);
    } else {
        // Full thumbnail: apply profile
        image = tpp->processImage (pparams, static_cast<rtengine::eSensorType>(cfs.sensortype), h, rtengine::TI_Bilinear, &cfs, scale );
    }

    tpp->getDimensions(lastW, lastH, lastScale);

    // Cache a Pixbuf copy for instant editor preview on image switch.
    if (cachePixbuf && image) {
        int pixW = image->getWidth(), pixH = image->getHeight();
        if (pixW > 0 && pixH > 0 && image->getData()) {
            auto pb = Gdk::Pixbuf::create_from_data(
                image->getData(), Gdk::COLORSPACE_RGB, false, 8, pixW, pixH, pixW * 3);
            cachedPixbuf_ = pb->copy();
            cachedPixbufScale_ = tw > 0 ? static_cast<double>(tw) / pixW : scale;
        }
    }

    delete tpp;
    tpp = nullptr;
    return image;
}

Glib::RefPtr<Gdk::Pixbuf> Thumbnail::tryLoadCachedPreviewPixbuf(int h, double& scale)
{
    MyMutex::MyLock lock(mutex);

    if (cachedPixbuf_) {
        scale = cachedPixbufScale_;
        return cachedPixbuf_;
    }

    if (!tpp) {
        _loadThumbnail(false);

        if (!tpp) {
            scale = 1.0;
            return {};
        }
    }

    rtengine::IImage8* image = nullptr;

    if (cfs.thumbImgType == CacheImageData::QUICK_THUMBNAIL) {
        image = tpp->quickProcessImage(getProcParamsU(), h, rtengine::TI_Nearest);
    } else {
        image = tpp->processImage(getProcParamsU(), static_cast<rtengine::eSensorType>(cfs.sensortype), h, rtengine::TI_Bilinear, &cfs, scale);
    }

    tpp->getDimensions(lastW, lastH, lastScale);
    delete tpp;
    tpp = nullptr;

    if (!image) {
        scale = 1.0;
        return {};
    }

    const int pixW = image->getWidth();
    const int pixH = image->getHeight();
    if (pixW > 0 && pixH > 0 && image->getData()) {
        auto pb = Gdk::Pixbuf::create_from_data(
            image->getData(), Gdk::COLORSPACE_RGB, false, 8, pixW, pixH, pixW * 3);
        cachedPixbuf_ = pb->copy();
        cachedPixbufScale_ = tw > 0 ? static_cast<double>(tw) / pixW : scale;
        scale = cachedPixbufScale_;
    }

    delete image;
    return cachedPixbuf_;
}

rtengine::IImage8* Thumbnail::processThumbImage (const rtengine::procparams::ProcParams& pparams, int h, double& scale, bool cachePixbuf)
{
    MyMutex::MyLock lock(mutex);
    return processThumbImageLocked(pparams, h, scale, cachePixbuf);
}

rtengine::IImage8* Thumbnail::processThumbImage (int h, double& scale, rtengine::procparams::CropParams* crop, bool cachePixbuf)
{
    MyMutex::MyLock lock(mutex);
    const auto& pparams = getProcParamsU();
    if (crop) {
        *crop = pparams.crop;
    }
    return processThumbImageLocked(pparams, h, scale, cachePixbuf);
}

rtengine::IImage8* Thumbnail::processFullThumbImage(
    const rtengine::procparams::ProcParams& pparams,
    int h,
    double& scale,
    bool cachePixbuf)
{
    MyMutex::MyLock lock(mutex);

    // Auto analysis must not depend on whether the browser happened to finish
    // upgrading an embedded RAW preview before the command was invoked.
    if (cfs.thumbImgType == CacheImageData::QUICK_THUMBNAIL) {
        return upgradeThumbImageLocked(pparams, h, scale, true, cachePixbuf);
    }

    return processThumbImageLocked(pparams, h, scale, cachePixbuf);
}

rtengine::IImage8* Thumbnail::upgradeThumbImageLocked (const rtengine::procparams::ProcParams& pparams, int h, double& scale, bool forceUpgrade, bool cachePixbuf)
{
    if ( cfs.thumbImgType != CacheImageData::QUICK_THUMBNAIL && !forceUpgrade ) {
        return nullptr;
    }

    _generateThumbnailImage();

    if (!tpp) {
        return nullptr;
    }

    // rtengine::IImage8* image = tpp->processImage (pparams, h, rtengine::TI_Bilinear, cfs.getCamera(), cfs.focalLen, cfs.focalLen35mm, cfs.focusDist, cfs.shutter, cfs.fnumber, cfs.iso, cfs.expcomp,  scale );
    rtengine::IImage8* image = tpp->processImage (pparams, static_cast<rtengine::eSensorType>(cfs.sensortype), h, rtengine::TI_Bilinear, &cfs, scale );
    tpp->getDimensions(lastW, lastH, lastScale);

    if (cachePixbuf && image) {
        int pixW = image->getWidth(), pixH = image->getHeight();
        if (pixW > 0 && pixH > 0 && image->getData()) {
            auto pb = Gdk::Pixbuf::create_from_data(
                image->getData(), Gdk::COLORSPACE_RGB, false, 8, pixW, pixH, pixW * 3);
            cachedPixbuf_ = pb->copy();
            cachedPixbufScale_ = tw > 0 ? static_cast<double>(tw) / pixW : scale;
        }
    }

    delete tpp;
    tpp = nullptr;
    return image;
}

rtengine::IImage8* Thumbnail::upgradeThumbImage (const rtengine::procparams::ProcParams& pparams, int h, double& scale, bool forceUpgrade, bool cachePixbuf)
{
    MyMutex::MyLock lock(mutex);
    return upgradeThumbImageLocked(pparams, h, scale, forceUpgrade, cachePixbuf);
}

rtengine::IImage8* Thumbnail::upgradeThumbImage (int h, double& scale, bool forceUpgrade, rtengine::procparams::CropParams* crop, bool cachePixbuf)
{
    MyMutex::MyLock lock(mutex);
    const auto& pparams = getProcParamsU();
    if (crop) {
        *crop = pparams.crop;
    }
    return upgradeThumbImageLocked(pparams, h, scale, forceUpgrade, cachePixbuf);
}

void Thumbnail::generateExifDateTimeStrings () const
{
    if (exifDateTimeStringsValid_) {
        return;
    }

    exifDateTimeStringsValid_ = true;

    const auto& options = App::get().options();
    if (cfs.timeValid) {
        std::string dateFormat = options.dateFormat;
        std::ostringstream ostr;
        bool spec = false;

        for (size_t i = 0; i < dateFormat.size(); i++)
            if (spec && dateFormat[i] == 'y') {
                ostr << cfs.year;
                spec = false;
            } else if (spec && dateFormat[i] == 'm') {
                ostr << (int)cfs.month;
                spec = false;
            } else if (spec && dateFormat[i] == 'd') {
                ostr << (int)cfs.day;
                spec = false;
            } else if (dateFormat[i] == '%') {
                spec = true;
            } else {
                ostr << (char)dateFormat[i];
                spec = false;
            }

        ostr << " " << (int)cfs.hour;
        ostr << ":" << std::setw(2) << std::setfill('0') << (int)cfs.min;
        ostr << ":" << std::setw(2) << std::setfill('0') << (int)cfs.sec;

        dateTimeString = ostr.str ();
        dateTime = Glib::DateTime::create_local(cfs.year, cfs.month, cfs.day,
                                                cfs.hour, cfs.min, cfs.sec);
    }

    if (!dateTime.gobj() || !cfs.timeValid) {
        dateTimeString = "";
        dateTime = Glib::DateTime::create_now_utc(0);
    }

    if (!cfs.exifValid) {
        exifString = "";
        return;
    }

    exifString = Glib::ustring::compose ("f/%1 %2s %3%4 %5mm", Glib::ustring(rtengine::FramesData::apertureToString(cfs.fnumber)), Glib::ustring(rtengine::FramesData::shutterToString(cfs.shutter)), M("QINFO_ISO"), cfs.iso, Glib::ustring::format(std::setw(3), std::fixed, std::setprecision(2), cfs.focalLen));

    if (options.fbShowExpComp && cfs.expcomp != "0.00" && !cfs.expcomp.empty()) { // don't show exposure compensation if it is 0.00EV;old cache files do not have ExpComp, so value will not be displayed.
        exifString = Glib::ustring::compose ("%1 %2EV", exifString, cfs.expcomp);    // append exposure compensation to exifString
    }
}

void Thumbnail::invalidateExifDateTimeStrings () const
{
    exifDateTimeStringsValid_ = false;
}

const Glib::ustring& Thumbnail::getExifString () const
{
    generateExifDateTimeStrings();
    return exifString;
}

const Glib::ustring& Thumbnail::getDateTimeString () const
{
    generateExifDateTimeStrings();
    return dateTimeString;
}

const Glib::DateTime& Thumbnail::getDateTime () const
{
    generateExifDateTimeStrings();
    return dateTime;
}

void Thumbnail::getAutoWB (double& temp, double& green, double equal, rtengine::StandardObserver observer, double tempBias)
{
    if (cfs.redAWBMul != -1.0) {
        rtengine::ColorTemp ct(cfs.redAWBMul, cfs.greenAWBMul, cfs.blueAWBMul, equal, observer);
        temp = ct.getTemp();
        green = ct.getGreen();
    } else {
        temp = green = -1.0;
    }
}


ThFileType Thumbnail::getType () const
{

    return (ThFileType) cfs.format;
}

int Thumbnail::infoFromImage (const Glib::ustring& fname)
{
    return infoFromImage(fname, cfs);
}

int Thumbnail::infoFromImage(const Glib::ustring &fname, CacheImageData &cfs)
{
    std::unique_ptr<rtengine::FramesMetaData> idata(rtengine::FramesMetaData::fromFile (fname));

    if (!idata) {
        return 0;
    }

    return populateCacheInfoFromMetadata(fname, cfs, *idata);
}

/*
 * Read all thumbnail's data from the cache; build and save them if doesn't exist - NON PROTECTED
 * This includes:
 *  - image's bitmap (*.rtti)
 *  - auto exposure's histogram (full thumbnail only)
 *  - embedded profile (full thumbnail only)
 *  - LiveThumbData section of the data file
 */
void Thumbnail::_loadThumbnail(bool firstTrial)
{

    tw = -1;
    th = App::get().options().maxThumbnailHeight;
    delete tpp;
    tpp = new rtengine::Thumbnail ();
    tpp->isRaw = (cfs.format == (int) FT_Raw);

    // load supplementary data
    bool succ = tpp->readData(getCacheFileName ("data", ".txt"));

    if (succ) {
        tpp->getAutoWBMultipliers(cfs.redAWBMul, cfs.greenAWBMul, cfs.blueAWBMul);
    }

    // thumbnail image
    if (succ) {
        const Glib::ustring imageCacheName = getCacheFileName("images", ".rtti");
        bool imageCachePresent = false;
        const bool imageCachePresenceKnown = cachemgr->getKnownFilePresence(imageCacheName, imageCachePresent);
        succ = (!imageCachePresenceKnown || imageCachePresent)
            && tpp->readImageFile(imageCacheName);
    }

    if (!succ && firstTrial) {
        _generateThumbnailImage ();

        if (cfs.supported) {
            _loadThumbnail (false);
        }

        if (!tpp) {
            return;
        }
    } else if (!succ) {
        delete tpp;
        tpp = nullptr;
        return;
    }

    if ( cfs.thumbImgType == CacheImageData::FULL_THUMBNAIL ) {
        // load embedded profile
        const Glib::ustring embProfileName = getCacheFileName("embprofiles", ".icc");
        bool embProfilePresent = false;
        const bool embProfilePresenceKnown = cachemgr->getKnownFilePresence(embProfileName, embProfilePresent);
        if (!embProfilePresenceKnown || embProfilePresent) {
            tpp->readEmbProfile(embProfileName);
        }

        tpp->init ();
    }

    if (!initial_) {
        tw = tpp->getImageWidth (getProcParamsU(), th, imgRatio);    // this might return 0 if image was just building
    }
}

/*
 * Save thumbnail's data to the cache - NON PROTECTED
 * This includes:
 *  - image's bitmap (*.rtti)
 *  - auto exposure's histogram (full thumbnail only)
 *  - embedded profile (full thumbnail only)
 *  - LiveThumbData section of the data file
 */
void Thumbnail::_saveThumbnail (bool saveLiveThumbData)
{

    if (!tpp) {
        return;
    }

    const Glib::ustring imageCacheName = getCacheFileName ("images", ".rtti");
    cachemgr->noteCacheFileWritten(imageCacheName);
    g_remove (imageCacheName.c_str ());

    // save thumbnail image
    tpp->writeImageFile (imageCacheName);
    cachemgr->noteCacheFileWritten(imageCacheName);

    // save embedded profile
    const Glib::ustring embProfileName = getCacheFileName ("embprofiles", ".icc");
    cachemgr->noteCacheFileWritten(embProfileName);
    tpp->writeEmbProfile (embProfileName);
    cachemgr->noteCacheFileWritten(embProfileName);

    if (saveLiveThumbData) {
        // save supplementary data
        const Glib::ustring cacheDataName = getCacheFileName ("data", ".txt");
        cachemgr->noteCacheFileWritten(cacheDataName);
        tpp->writeData (cacheDataName);
        cachemgr->noteCacheFileWritten(cacheDataName);
    }
}

/*
 * Save thumbnail's data to the cache - MUTEX PROTECTED
 * This includes:
 *  - image's bitmap (*.rtti)
 *  - auto exposure's histogram (full thumbnail only)
 *  - embedded profile (full thumbnail only)
 *  - LiveThumbData section of the data file
 */
void Thumbnail::saveThumbnail ()
{
    MyMutex::MyLock lock(mutex);
    _saveThumbnail();
}

/*
 * Update the cached files
 *  - updatePParams==true (default)        : write the procparams file (sidecar or cache, depending on the options)
 *  - updateCacheImageData==true (default) : write the CacheImageData values in the cache folder,
 *                                           i.e. some General, DateTime, ExifInfo, File info and ExtraRawInfo,
 */
void Thumbnail::updateCache (bool updatePParams, bool updateCacheImageData)
{
    updateProcParamsProperties();

    if (updatePParams && pparamsValid) {
        const auto& options = App::get().options();
        const Glib::ustring fileProfileName = options.saveParamsFile ? fname + App::PARAM_FILE_EXTENSION : "";
        const Glib::ustring cacheProfileName = options.saveParamsCache ? getCacheFileName ("profiles", App::PARAM_FILE_EXTENSION) : "";
        if (!fileProfileName.empty()) {
            cachemgr->noteCacheFileWritten(fileProfileName);
        }
        if (!cacheProfileName.empty()) {
            cachemgr->noteCacheFileWritten(cacheProfileName);
        }
        pparams->save (
            fileProfileName,
            cacheProfileName,
            true
        );
        if (!fileProfileName.empty()) {
            cachemgr->noteCacheFileWritten(fileProfileName);
        }
        if (!cacheProfileName.empty()) {
            cachemgr->noteCacheFileWritten(cacheProfileName);
        }
    }

    if (updateCacheImageData) {
        const Glib::ustring cacheDataName = getCacheFileName ("data", ".txt");
        cachemgr->noteCacheFileWritten(cacheDataName);
        cfs.save (cacheDataName);
        cachemgr->noteCacheFileWritten(cacheDataName);

        // Write-through to the global selects index so cross-folder discovery
        // stays complete without rescanning.
        SelectsIndex::getInstance().note(fname, getRank(), getPick(), getColorLabel());
    }

    if (updatePParams && pparamsValid) {
        saveMetadata();
    }

    saveXMPSidecarProperties();
}

Thumbnail::~Thumbnail ()
{
    mutex.lock();

    delete [] lastImg;
    delete tpp;
    mutex.unlock();
}

Glib::ustring Thumbnail::getCacheFileName (const Glib::ustring& subdir, const Glib::ustring& fext) const
{
    return cachemgr->getCacheFileNameForBase (subdir, cacheBaseName_, fext);
}

void Thumbnail::setFileName (const Glib::ustring &fn)
{

    fname = fn;
    cfs.md5 = ::getMD5 (fname);
    cacheBaseName_ = Glib::path_get_basename(fname) + "." + cfs.md5;
}

int Thumbnail::getRank() const
{
    return properties.rank;
}

void Thumbnail::setRank(int rank)
{
    properties.rank = rank;
}

int Thumbnail::getColorLabel() const
{
    return properties.color;
}

void Thumbnail::setColorLabel(int colorlabel)
{
    properties.color = colorlabel;
}

bool Thumbnail::getTrashed() const
{
    return properties.trashed;
}

void Thumbnail::setTrashed(bool trashed)
{
    properties.trashed = trashed;
}

int Thumbnail::getPick() const
{
    return properties.pick;
}

void Thumbnail::setPick(int pick)
{
    properties.pick = pick;
    // Mirror into the cache data so the flag round-trips through the cache
    // even for images that have no procparams sidecar yet.
    cfs.pickLabel = pick;
}

void Thumbnail::addThumbnailListener (ThumbnailListener* tnl)
{

    increaseRef();
    listeners.push_back (tnl);
}

void Thumbnail::removeThumbnailListener (ThumbnailListener* tnl)
{

    std::vector<ThumbnailListener*>::iterator f = std::find (listeners.begin(), listeners.end(), tnl);

    if (f != listeners.end()) {
        listeners.erase (f);
        decreaseRef();
    }
}

bool Thumbnail::removeThumbnailListenerNoRelease (ThumbnailListener* tnl)
{
    std::vector<ThumbnailListener*>::iterator f = std::find (listeners.begin(), listeners.end(), tnl);

    if (f == listeners.end()) {
        return false;
    }

    listeners.erase(f);
    return true;
}

// Calculates the standard filename for the automatically named batch result
// and opens it in OS default viewer
// destination: 1=Batch conf. file; 2=batch out dir; 3=RAW dir
// Return: Success?
bool Thumbnail::openDefaultViewer(int destination)
{

#ifdef _WIN32
    Glib::ustring openFName;

    const auto& options = App::get().options();
    if (destination == 1) {
        openFName = Glib::ustring::compose ("%1.%2", BatchQueue::calcAutoFileNameBase(fname), options.saveFormatBatch.format);

        if (Glib::file_test (openFName, Glib::FILE_TEST_EXISTS)) {
            wchar_t *wfilename = (wchar_t*)g_utf8_to_utf16 (openFName.c_str(), -1, NULL, NULL, NULL);
            ShellExecuteW(NULL, L"open", wfilename, NULL, NULL, SW_SHOWMAXIMIZED );
            g_free(wfilename);
        } else {
            printf("%s not found\n", openFName.data());
            return false;
        }
    } else {
        openFName = destination == 3 ? fname
                    : Glib::ustring::compose ("%1.%2", BatchQueue::calcAutoFileNameBase(fname), options.saveFormatBatch.format);

        printf("Opening %s\n", openFName.c_str());

        if (Glib::file_test (openFName, Glib::FILE_TEST_EXISTS)) {
            // Output file exists, so open explorer and select output file
            wchar_t* org = (wchar_t*)g_utf8_to_utf16 (Glib::ustring::compose("/select,\"%1\"", openFName).c_str(), -1, NULL, NULL, NULL);
            wchar_t* par = new wchar_t[wcslen(org) + 1];
            wcscpy(par, org);

            // In this case the / disturbs
            wchar_t* p = par + 1; // skip the first backslash

            while (*p != 0) {
                if (*p == L'/') {
                    *p = L'\\';
                }

                p++;
            }

            ShellExecuteW(NULL, L"open", L"explorer.exe", par, NULL, SW_SHOWNORMAL );

            delete[] par;
            g_free(org);
        } else if (Glib::file_test (Glib::path_get_dirname(openFName), Glib::FILE_TEST_EXISTS)) {
            // Out file does not exist, but directory
            wchar_t *wfilename = (wchar_t*)g_utf8_to_utf16 (Glib::path_get_dirname(openFName).c_str(), -1, NULL, NULL, NULL);
            ShellExecuteW(NULL, L"explore", wfilename, NULL, NULL, SW_SHOWNORMAL );
            g_free(wfilename);
        } else {
            printf("File and dir not found\n");
            return false;
        }
    }

    return true;

#else
    // TODO: Add more OSes here
    printf("Automatic opening not supported on this OS\n");
    return false;
#endif

}

bool Thumbnail::imageLoad(bool loading)
{
    MyMutex::MyLock lock(mutex);
    bool previous = imageLoading;

    if( loading && !previous ) {
        imageLoading = true;
        return true;
    } else if( !loading ) {
        imageLoading = false;
    }

    return false;
}

void Thumbnail::getCamWB(double& temp, double& green, rtengine::StandardObserver observer) const
{
    if (tpp) {
        tpp->getCamWB  (temp, green, observer);
    } else {
        temp = green = -1.0;
    }
}

void Thumbnail::loadProperties()
{
    properties = Properties();

    const auto& options = App::get().options();
    bool needMetadataRank = true;
    bool needMetadataColor = true;

    // update rank and color from procparams or xmp sidecar
    // load trash from procparams
    if (pparamsValid) {
        if (options.thumbnailRankColorMode == Options::ThumbnailPropertyMode::PROCPARAMS) {
            if (pparams->rank >= 0) {
                properties.rank.value = pparams->rank;
                needMetadataRank = false;
            }
        }

        properties.trashed.value = pparams->inTrash;
        properties.color.value = pparams->colorlabel;
        properties.pick.value = pparams->pickLabel;
        needMetadataColor = false;
    } else {
        // No procparams (yet) — the pick flag still round-trips through the
        // cache image data written by setPick()/updateCache().
        properties.pick.value = cfs.pickLabel;
    }

    const bool sidecarPresenceKnown =
        rtengine::settings->metadata_xmp_sync != rtengine::Settings::MetadataXmpSync::NONE;
    const bool shouldLoadXmpSidecar =
        options.thumbnailRankColorMode == Options::ThumbnailPropertyMode::XMP
        && (!sidecarPresenceKnown || !cfs.xmpSidecarMd5.empty());

    if (shouldLoadXmpSidecar) {
        try {
            auto xmp = rtengine::Exiv2Metadata::getXmpSidecar(fname);
            if (getRankFromXmp(xmp, properties.rank.value)) {
                needMetadataRank = false;
            }
            if (getColorFromXmp(xmp, properties.color.value)) {
                needMetadataColor = false;
            }
        } catch (std::exception &exc) {
            std::cerr << "ERROR loading thumbnail properties data from "
                      << rtengine::Exiv2Metadata::xmpSidecarPath(fname)
                      << ": " << exc.what() << std::endl;
        }
    }

    if (needMetadataRank || needMetadataColor) {
        int metadataRank = properties.rank.value;
        int metadataColor = properties.color.value;
        getRankAndColorFromMetadata(
            cfs, fname, metadataRank, metadataColor, nullptr, nullptr);
        if (needMetadataRank) {
            properties.rank.value = metadataRank;
        }
        if (needMetadataColor) {
            properties.color.value = metadataColor;
        }
    }
}

void Thumbnail::updateProcParamsProperties(bool forceUpdate)
{
    if (!(properties.edited() || forceUpdate)) {
        return;
    }

    if ((properties.trashed.edited || forceUpdate) && properties.trashed != pparams->inTrash) {
        pparams->inTrash = properties.trashed;
        pparamsValid = true;
    }

    if ((properties.pick.edited || forceUpdate) && properties.pick != pparams->pickLabel) {
        pparams->pickLabel = properties.pick;
        pparamsValid = true;
    }

    const rtengine::MemoizingSupplier<Exiv2::XmpData> getXmpSidecar([this]() {
        return rtengine::Exiv2Metadata::getXmpSidecar(fname);
    });

    const auto& options = App::get().options();

    // save procparams rank and color also when options.thumbnailRankColorMode == Options::ThumbnailPropertyMode::XMP
    // so they'll be kept in sync
    // Rank can be -1 to prioritize the rank in the metadata. If the metadata
    // rank doesn't exist, it is interpreted as 0.
    if ((properties.rank.edited || forceUpdate) &&
        rtengine::LIM(properties.rank.value, 0, 5) != rtengine::LIM(pparams->rank, 0, 5)) {
        pparams->rank = properties.rank;
        if (!forceUpdate) {
            pparamsValid |= properties.rank.edited;
        }
        else if (!pparamsValid && forceUpdate) {
            // When force-updating, the processing parameters' rank needs not be
            // used if the embedded rank is the same.
            int initial_rank = 0;
            bool has_initial_rank = getRankFromXmpOrMetadata(
                options, getXmpSidecar(), cfs, fname, initial_rank);
            pparamsValid |= !(has_initial_rank && properties.rank == initial_rank);
        }
    }

    if ((properties.color.edited || forceUpdate) && properties.color != pparams->colorlabel) {
        pparams->colorlabel = properties.color;
        if (!forceUpdate) {
            pparamsValid |= properties.color.edited;
        }
        else if (!pparamsValid && forceUpdate) {
            // When force-updating, the processing parameters' color label needs
            // not be used if the embedded color label is the same.
            int initial_color = 0;
            bool has_initial_color = getColorFromXmpOrNone(
                options, getXmpSidecar(), fname, initial_color);
            pparamsValid |= !(has_initial_color && properties.color == initial_color);
        }
    }
}

void Thumbnail::saveXMPSidecarProperties()
{
    if (!properties.edited()) {
        return;
    }

    if (App::get().options().thumbnailRankColorMode != Options::ThumbnailPropertyMode::XMP) {
        return;
    }

    auto fn = rtengine::Exiv2Metadata::xmpSidecarPath(fname);
    try {
        auto xmp = rtengine::Exiv2Metadata::getXmpSidecar(fname);
        if (properties.rank.edited) {
            xmp["Xmp.xmp.Rating"] = std::to_string(properties.rank);
        }
        if (properties.color.edited) {
            xmp["Xmp.xmp.Label"] = rtengine::FramesData::xmp_color2label(properties.color);
        }

        rtengine::Exiv2Metadata meta;
        meta.xmpData() = std::move(xmp);
        meta.saveToXmp(fn);
    } catch (std::exception &exc) {
        std::cerr << "ERROR saving thumbnail properties data to " << fn
                  << ": " << exc.what() << std::endl;
    }
}

void Thumbnail::saveMetadata()
{
    const auto& options = App::get().options();
    if (options.rtSettings.metadata_xmp_sync != rtengine::Settings::MetadataXmpSync::READ_WRITE) {
        return;
    }

    if (pparams->metadata.exif.empty() && pparams->metadata.iptc.empty()) {
        return;
    }

    auto fn = rtengine::Exiv2Metadata::xmpSidecarPath(fname);
    try {
        auto xmp = rtengine::Exiv2Metadata::getXmpSidecar(fname);
        rtengine::Exiv2Metadata meta;
        meta.xmpData() = std::move(xmp);
        meta.setExif(pparams->metadata.exif);
        meta.setIptc(pparams->metadata.iptc);
        meta.saveToXmp(fn);
        if (options.rtSettings.verbose) {
            std::cout << "saved edited metadata for " << fname << " to "
                      << fn << std::endl;
        }
    } catch (std::exception &exc) {
        std::cerr << "ERROR saving metadata for " << fname << " to " << fn
                  << ": " << exc.what() << std::endl;
    }
}
void Thumbnail::getSpotWB(int x, int y, int rect, double& temp, double& green)
{
    if (tpp) {
        tpp->getSpotWB (getProcParams(), x, y, rect, temp, green);
    } else {
        temp = green = -1.0;
    }
}

bool Thumbnail::applyAutoExp (rtengine::procparams::ProcParams& pparams)
{
    MyMutex::MyLock lock(mutex);
    const bool loadedForAutoExposure = !tpp;
    if (loadedForAutoExposure) {
        _loadThumbnail();
    }

    bool metered = false;
    if (tpp) {
        metered = tpp->applyAutoExp (pparams);
    }

    if (loadedForAutoExposure) {
        delete tpp;
        tpp = nullptr;
    }

    return metered;
}

const CacheImageData* Thumbnail::getCacheImageData() const
{
    return &cfs;
}

std::string Thumbnail::getMD5() const
{
    return cfs.md5;
}

bool Thumbnail::isQuick() const
{
    return cfs.thumbImgType == CacheImageData::QUICK_THUMBNAIL;
}

bool Thumbnail::isPParamsValid() const
{
    return pparamsValid;
}
