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

#include "mcp/mcptools.h"
#include "mcp/mcpserver.h"
#include "mcp/mcpprotocol.h"
#include "mcp/mcpvolumewatcher.h"

#include "editorpanel.h"
#include "toolpanelcoord.h"

#include "rtengine/procparams.h"
#include "rtengine/procevents.h"
#include "rtengine/rtengine.h"
#include "rtengine/rtapp.h"
#include "rtengine/cJSON.h"

// localtime_r is POSIX; Windows provides localtime_s with swapped args.
namespace {
inline struct tm* portable_localtime(const time_t* t, struct tm* result) {
#ifdef _WIN32
    localtime_s(result, t);
#else
    localtime_r(t, result);
#endif
    return result;
}
} // namespace

#include <glib/gstdio.h>
#include <giomm.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>

using namespace rtengine::procparams;

namespace mcp {

namespace {

// Helper to serialize a string as JSON
std::string jsonStr(cJSON* obj) {
    char* s = cJSON_PrintUnformatted(obj);
    std::string result(s);
    free(s);
    return result;
}

// Helper to get a string from cJSON or return default
std::string getStr(cJSON* obj, const char* key, const std::string& def = "") {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item && cJSON_IsString(item)) return item->valuestring;
    return def;
}

// Helper to get a number from cJSON or return default
double getNum(cJSON* obj, const char* key, double def = 0.0) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item && cJSON_IsNumber(item)) return item->valuedouble;
    return def;
}

// Helper to get a bool from cJSON or return default
bool getBool(cJSON* obj, const char* key, bool def = false) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item) {
        if (cJSON_IsTrue(item)) return true;
        if (cJSON_IsFalse(item)) return false;
    }
    return def;
}

// Check if a key exists in cJSON object
bool hasKey(cJSON* obj, const char* key) {
    return cJSON_GetObjectItemCaseSensitive(obj, key) != nullptr;
}

// Apply JSON params to a ProcParams object. Returns true if any param was changed.
bool applyJsonToParams(ProcParams& pp, cJSON* json) {
    if (!json || !cJSON_IsObject(json)) return false;

    bool changed = false;

    // Helper macro to reduce repetitive code
    #define APPLY_NUM(section, field, jsonKey) \
        if (hasKey(sec, jsonKey)) { pp.section.field = getNum(sec, jsonKey); changed = true; }
    #define APPLY_INT(section, field, jsonKey) \
        if (hasKey(sec, jsonKey)) { pp.section.field = static_cast<int>(getNum(sec, jsonKey)); changed = true; }
    #define APPLY_BOOL(section, field, jsonKey) \
        if (hasKey(sec, jsonKey)) { pp.section.field = getBool(sec, jsonKey); changed = true; }
    #define APPLY_STR(section, field, jsonKey) \
        if (hasKey(sec, jsonKey)) { pp.section.field = getStr(sec, jsonKey); changed = true; }

    cJSON* sec;

    sec = cJSON_GetObjectItemCaseSensitive(json, "toneCurve");
    if (sec) {
        APPLY_BOOL(toneCurve, autoexp, "autoexp")
        APPLY_NUM(toneCurve, expcomp, "expcomp")
        APPLY_INT(toneCurve, brightness, "brightness")
        APPLY_INT(toneCurve, contrast, "contrast")
        APPLY_INT(toneCurve, saturation, "saturation")
        APPLY_INT(toneCurve, black, "black")
        APPLY_INT(toneCurve, hlcompr, "hlcompr")
        APPLY_INT(toneCurve, hlcomprthresh, "hlcomprthresh")
        APPLY_INT(toneCurve, shcompr, "shcompr")
        APPLY_BOOL(toneCurve, hrenabled, "hrenabled")
        APPLY_STR(toneCurve, method, "method")
        APPLY_BOOL(toneCurve, histmatching, "histmatching")
        APPLY_BOOL(toneCurve, clampOOG, "clampOOG")
    }

    sec = cJSON_GetObjectItemCaseSensitive(json, "labCurve");
    if (sec) {
        APPLY_BOOL(labCurve, enabled, "enabled")
        APPLY_INT(labCurve, brightness, "brightness")
        APPLY_INT(labCurve, contrast, "contrast")
        APPLY_INT(labCurve, chromaticity, "chromaticity")
    }

    sec = cJSON_GetObjectItemCaseSensitive(json, "wb");
    if (sec) {
        APPLY_BOOL(wb, enabled, "enabled")
        APPLY_STR(wb, method, "method")
        APPLY_INT(wb, temperature, "temperature")
        APPLY_NUM(wb, green, "green")
        APPLY_NUM(wb, equal, "equal")
        APPLY_NUM(wb, tempBias, "tempBias")
    }

    sec = cJSON_GetObjectItemCaseSensitive(json, "sharpening");
    if (sec) {
        APPLY_BOOL(sharpening, enabled, "enabled")
        APPLY_NUM(sharpening, contrast, "contrast")
        APPLY_NUM(sharpening, radius, "radius")
        APPLY_INT(sharpening, amount, "amount")
        APPLY_STR(sharpening, method, "method")
        APPLY_INT(sharpening, deconvamount, "deconvamount")
        APPLY_NUM(sharpening, deconvradius, "deconvradius")
    }

    sec = cJSON_GetObjectItemCaseSensitive(json, "vibrance");
    if (sec) {
        APPLY_BOOL(vibrance, enabled, "enabled")
        APPLY_INT(vibrance, pastels, "pastels")
        APPLY_INT(vibrance, saturated, "saturated")
        APPLY_BOOL(vibrance, protectskins, "protectskins")
        APPLY_BOOL(vibrance, avoidcolorshift, "avoidcolorshift")
        APPLY_BOOL(vibrance, pastsattog, "pastsattog")
    }

    sec = cJSON_GetObjectItemCaseSensitive(json, "sh");
    if (sec) {
        APPLY_BOOL(sh, enabled, "enabled")
        APPLY_INT(sh, highlights, "highlights")
        APPLY_INT(sh, htonalwidth, "htonalwidth")
        APPLY_INT(sh, shadows, "shadows")
        APPLY_INT(sh, stonalwidth, "stonalwidth")
        APPLY_INT(sh, radius, "radius")
    }

    sec = cJSON_GetObjectItemCaseSensitive(json, "localContrast");
    if (sec) {
        APPLY_BOOL(localContrast, enabled, "enabled")
        APPLY_INT(localContrast, radius, "radius")
        APPLY_NUM(localContrast, amount, "amount")
        APPLY_NUM(localContrast, darkness, "darkness")
        APPLY_NUM(localContrast, lightness, "lightness")
    }

    sec = cJSON_GetObjectItemCaseSensitive(json, "defringe");
    if (sec) {
        APPLY_BOOL(defringe, enabled, "enabled")
        APPLY_NUM(defringe, radius, "radius")
        APPLY_INT(defringe, threshold, "threshold")
    }

    sec = cJSON_GetObjectItemCaseSensitive(json, "dirpyrDenoise");
    if (sec) {
        APPLY_BOOL(dirpyrDenoise, enabled, "enabled")
        APPLY_BOOL(dirpyrDenoise, enhance, "enhance")
        APPLY_NUM(dirpyrDenoise, luma, "luma")
        APPLY_NUM(dirpyrDenoise, Ldetail, "Ldetail")
        APPLY_NUM(dirpyrDenoise, chroma, "chroma")
        APPLY_NUM(dirpyrDenoise, redchro, "redchro")
        APPLY_NUM(dirpyrDenoise, bluechro, "bluechro")
        APPLY_NUM(dirpyrDenoise, gamma, "gamma")
    }

    sec = cJSON_GetObjectItemCaseSensitive(json, "crop");
    if (sec) {
        APPLY_BOOL(crop, enabled, "enabled")
        APPLY_INT(crop, x, "x")
        APPLY_INT(crop, y, "y")
        APPLY_INT(crop, w, "w")
        APPLY_INT(crop, h, "h")
        APPLY_BOOL(crop, fixratio, "fixratio")
        APPLY_STR(crop, ratio, "ratio")
    }

    sec = cJSON_GetObjectItemCaseSensitive(json, "rotate");
    if (sec) {
        APPLY_NUM(rotate, degree, "degree")
    }

    sec = cJSON_GetObjectItemCaseSensitive(json, "distortion");
    if (sec) {
        APPLY_NUM(distortion, amount, "amount")
    }

    sec = cJSON_GetObjectItemCaseSensitive(json, "resize");
    if (sec) {
        APPLY_BOOL(resize, enabled, "enabled")
        APPLY_NUM(resize, scale, "scale")
        APPLY_STR(resize, method, "method")
        APPLY_INT(resize, width, "width")
        APPLY_INT(resize, height, "height")
        APPLY_BOOL(resize, allowUpscaling, "allowUpscaling")
    }

    sec = cJSON_GetObjectItemCaseSensitive(json, "gradient");
    if (sec) {
        APPLY_BOOL(gradient, enabled, "enabled")
        APPLY_NUM(gradient, degree, "degree")
        APPLY_INT(gradient, feather, "feather")
        APPLY_NUM(gradient, strength, "strength")
        APPLY_INT(gradient, centerX, "centerX")
        APPLY_INT(gradient, centerY, "centerY")
    }

    sec = cJSON_GetObjectItemCaseSensitive(json, "dehaze");
    if (sec) {
        APPLY_BOOL(dehaze, enabled, "enabled")
        APPLY_INT(dehaze, strength, "strength")
        APPLY_INT(dehaze, saturation, "saturation")
        APPLY_INT(dehaze, depth, "depth")
    }

    sec = cJSON_GetObjectItemCaseSensitive(json, "softlight");
    if (sec) {
        APPLY_BOOL(softlight, enabled, "enabled")
        APPLY_INT(softlight, strength, "strength")
    }

    sec = cJSON_GetObjectItemCaseSensitive(json, "fattal");
    if (sec) {
        APPLY_BOOL(fattal, enabled, "enabled")
        APPLY_INT(fattal, threshold, "threshold")
        APPLY_INT(fattal, amount, "amount")
    }

    sec = cJSON_GetObjectItemCaseSensitive(json, "filmSimulation");
    if (sec) {
        APPLY_BOOL(filmSimulation, enabled, "enabled")
        APPLY_STR(filmSimulation, clutFilename, "clutFilename")
        APPLY_INT(filmSimulation, strength, "strength")
    }

    sec = cJSON_GetObjectItemCaseSensitive(json, "blackwhite");
    if (sec) {
        APPLY_BOOL(blackwhite, enabled, "enabled")
    }

    sec = cJSON_GetObjectItemCaseSensitive(json, "retinex");
    if (sec) {
        APPLY_BOOL(retinex, enabled, "enabled")
        APPLY_INT(retinex, str, "str")
        APPLY_INT(retinex, neigh, "neigh")
    }

    sec = cJSON_GetObjectItemCaseSensitive(json, "hsvequalizer");
    if (sec) {
        APPLY_BOOL(hsvequalizer, enabled, "enabled")
    }

    sec = cJSON_GetObjectItemCaseSensitive(json, "rgbCurves");
    if (sec) {
        APPLY_BOOL(rgbCurves, enabled, "enabled")
        APPLY_BOOL(rgbCurves, lumamode, "lumamode")
    }

    sec = cJSON_GetObjectItemCaseSensitive(json, "colorGrading");
    if (sec) {
        APPLY_BOOL(colorGrading, enabled, "enabled")
    }

    #undef APPLY_NUM
    #undef APPLY_INT
    #undef APPLY_BOOL
    #undef APPLY_STR

    return changed;
}

// Simple approach: get/set enabled state by tool name
bool getToolEnabled(const ProcParams& p, const std::string& name) {
    if (name == "labCurve") return p.labCurve.enabled;
    if (name == "sharpening") return p.sharpening.enabled;
    if (name == "vibrance") return p.vibrance.enabled;
    if (name == "wb") return p.wb.enabled;
    if (name == "defringe") return p.defringe.enabled;
    if (name == "dirpyrDenoise") return p.dirpyrDenoise.enabled;
    if (name == "impulseDenoise") return p.impulseDenoise.enabled;
    if (name == "sh") return p.sh.enabled;
    if (name == "localContrast") return p.localContrast.enabled;
    if (name == "crop") return p.crop.enabled;
    if (name == "resize") return p.resize.enabled;
    if (name == "gradient") return p.gradient.enabled;
    if (name == "pcvignette") return p.pcvignette.enabled;
    if (name == "retinex") return p.retinex.enabled;
    if (name == "rgbCurves") return p.rgbCurves.enabled;
    if (name == "colorGrading") return p.colorGrading.enabled;
    if (name == "dehaze") return p.dehaze.enabled;
    if (name == "softlight") return p.softlight.enabled;
    if (name == "fattal") return p.fattal.enabled;
    if (name == "filmSimulation") return p.filmSimulation.enabled;
    if (name == "blackwhite") return p.blackwhite.enabled;
    if (name == "hsvequalizer") return p.hsvequalizer.enabled;
    return false;
}

bool setToolEnabled(ProcParams& p, const std::string& name, bool v) {
    if (name == "labCurve") { p.labCurve.enabled = v; return true; }
    if (name == "sharpening") { p.sharpening.enabled = v; return true; }
    if (name == "vibrance") { p.vibrance.enabled = v; return true; }
    if (name == "wb") { p.wb.enabled = v; return true; }
    if (name == "defringe") { p.defringe.enabled = v; return true; }
    if (name == "dirpyrDenoise") { p.dirpyrDenoise.enabled = v; return true; }
    if (name == "impulseDenoise") { p.impulseDenoise.enabled = v; return true; }
    if (name == "sh") { p.sh.enabled = v; return true; }
    if (name == "localContrast") { p.localContrast.enabled = v; return true; }
    if (name == "crop") { p.crop.enabled = v; return true; }
    if (name == "resize") { p.resize.enabled = v; return true; }
    if (name == "gradient") { p.gradient.enabled = v; return true; }
    if (name == "pcvignette") { p.pcvignette.enabled = v; return true; }
    if (name == "retinex") { p.retinex.enabled = v; return true; }
    if (name == "rgbCurves") { p.rgbCurves.enabled = v; return true; }
    if (name == "colorGrading") { p.colorGrading.enabled = v; return true; }
    if (name == "dehaze") { p.dehaze.enabled = v; return true; }
    if (name == "softlight") { p.softlight.enabled = v; return true; }
    if (name == "fattal") { p.fattal.enabled = v; return true; }
    if (name == "filmSimulation") { p.filmSimulation.enabled = v; return true; }
    if (name == "blackwhite") { p.blackwhite.enabled = v; return true; }
    if (name == "hsvequalizer") { p.hsvequalizer.enabled = v; return true; }
    return false;
}

const std::vector<std::string>& getToolNames() {
    static const std::vector<std::string> names = {
        "labCurve", "sharpening", "vibrance", "wb", "defringe",
        "dirpyrDenoise", "impulseDenoise", "sh", "localContrast",
        "crop", "resize", "gradient", "pcvignette",
        "retinex", "rgbCurves", "colorGrading", "dehaze",
        "softlight", "fattal", "filmSimulation", "blackwhite", "hsvequalizer"
    };
    return names;
}

// Recursively collect image files from a directory
void collectImageFiles(const std::string& dirPath, bool recursive,
                       const std::set<std::string>& exts,
                       std::vector<std::string>& outFiles) {
    try {
        Glib::Dir dir(dirPath);
        for (auto it = dir.begin(); it != dir.end(); ++it) {
            const std::string& entry = *it;
            if (entry.empty() || entry[0] == '.') continue;

            std::string fullPath = Glib::build_filename(dirPath, entry);

            if (Glib::file_test(fullPath, Glib::FILE_TEST_IS_DIR)) {
                if (recursive) {
                    collectImageFiles(fullPath, recursive, exts, outFiles);
                }
            } else if (Glib::file_test(fullPath, Glib::FILE_TEST_IS_REGULAR)) {
                auto dotPos = entry.find_last_of('.');
                if (dotPos != std::string::npos && dotPos < entry.length() - 1) {
                    Glib::ustring ext = Glib::ustring(entry.substr(dotPos + 1)).lowercase();
                    if (exts.count(ext.raw())) {
                        outFiles.push_back(fullPath);
                    }
                }
            }
        }
    } catch (...) {
        // Directory not readable, skip
    }
}

// Get a date-based subfolder from EXIF or file mtime
std::string getExifDateFolder(const std::string& filePath, const std::string& tmpl) {
    struct tm date = {};
    bool hasDate = false;

    // Try EXIF first
    try {
        std::unique_ptr<rtengine::FramesMetaData> meta(
            rtengine::FramesMetaData::fromFile(filePath));
        if (meta && meta->hasExif()) {
            time_t ts = meta->getDateTimeAsTS();
            if (ts > 0) {
                portable_localtime(&ts, &date);
                hasDate = true;
            }
        }
    } catch (...) {}

    // Fall back to file mtime
    if (!hasDate) {
        GStatBuf st;
        if (g_stat(filePath.c_str(), &st) == 0) {
            portable_localtime(&st.st_mtime, &date);
            hasDate = true;
        }
    }

    if (!hasDate) {
        return "unknown_date";
    }

    char year[8], month[8], day[8];
    strftime(year, sizeof(year), "%Y", &date);
    strftime(month, sizeof(month), "%m", &date);
    strftime(day, sizeof(day), "%d", &date);

    std::string result = tmpl;
    size_t pos;
    while ((pos = result.find("YYYY")) != std::string::npos) {
        result.replace(pos, 4, year);
    }
    while ((pos = result.find("MM")) != std::string::npos) {
        result.replace(pos, 2, month);
    }
    while ((pos = result.find("DD")) != std::string::npos) {
        result.replace(pos, 2, day);
    }

    return result;
}

} // anonymous namespace

cJSON* paramsToJson(const ProcParams& pp, const std::string& section) {
    cJSON* root = cJSON_CreateObject();

    auto addSection = [&](const char* name, std::function<void(cJSON*)> fn) {
        if (section.empty() || section == name) {
            cJSON* sec = cJSON_CreateObject();
            fn(sec);
            cJSON_AddItemToObject(root, name, sec);
        }
    };

    addSection("toneCurve", [&](cJSON* s) {
        cJSON_AddBoolToObject(s, "autoexp", pp.toneCurve.autoexp);
        cJSON_AddNumberToObject(s, "expcomp", pp.toneCurve.expcomp);
        cJSON_AddNumberToObject(s, "brightness", pp.toneCurve.brightness);
        cJSON_AddNumberToObject(s, "contrast", pp.toneCurve.contrast);
        cJSON_AddNumberToObject(s, "saturation", pp.toneCurve.saturation);
        cJSON_AddNumberToObject(s, "black", pp.toneCurve.black);
        cJSON_AddNumberToObject(s, "hlcompr", pp.toneCurve.hlcompr);
        cJSON_AddNumberToObject(s, "hlcomprthresh", pp.toneCurve.hlcomprthresh);
        cJSON_AddNumberToObject(s, "shcompr", pp.toneCurve.shcompr);
        cJSON_AddBoolToObject(s, "hrenabled", pp.toneCurve.hrenabled);
        cJSON_AddStringToObject(s, "method", pp.toneCurve.method.c_str());
        cJSON_AddBoolToObject(s, "histmatching", pp.toneCurve.histmatching);
        cJSON_AddBoolToObject(s, "clampOOG", pp.toneCurve.clampOOG);
    });

    addSection("labCurve", [&](cJSON* s) {
        cJSON_AddBoolToObject(s, "enabled", pp.labCurve.enabled);
        cJSON_AddNumberToObject(s, "brightness", pp.labCurve.brightness);
        cJSON_AddNumberToObject(s, "contrast", pp.labCurve.contrast);
        cJSON_AddNumberToObject(s, "chromaticity", pp.labCurve.chromaticity);
    });

    addSection("wb", [&](cJSON* s) {
        cJSON_AddBoolToObject(s, "enabled", pp.wb.enabled);
        cJSON_AddStringToObject(s, "method", pp.wb.method.c_str());
        cJSON_AddNumberToObject(s, "temperature", pp.wb.temperature);
        cJSON_AddNumberToObject(s, "green", pp.wb.green);
        cJSON_AddNumberToObject(s, "equal", pp.wb.equal);
        cJSON_AddNumberToObject(s, "tempBias", pp.wb.tempBias);
    });

    addSection("sharpening", [&](cJSON* s) {
        cJSON_AddBoolToObject(s, "enabled", pp.sharpening.enabled);
        cJSON_AddNumberToObject(s, "contrast", pp.sharpening.contrast);
        cJSON_AddNumberToObject(s, "radius", pp.sharpening.radius);
        cJSON_AddNumberToObject(s, "amount", pp.sharpening.amount);
        cJSON_AddStringToObject(s, "method", pp.sharpening.method.c_str());
        cJSON_AddNumberToObject(s, "deconvamount", pp.sharpening.deconvamount);
        cJSON_AddNumberToObject(s, "deconvradius", pp.sharpening.deconvradius);
    });

    addSection("vibrance", [&](cJSON* s) {
        cJSON_AddBoolToObject(s, "enabled", pp.vibrance.enabled);
        cJSON_AddNumberToObject(s, "pastels", pp.vibrance.pastels);
        cJSON_AddNumberToObject(s, "saturated", pp.vibrance.saturated);
        cJSON_AddBoolToObject(s, "protectskins", pp.vibrance.protectskins);
        cJSON_AddBoolToObject(s, "avoidcolorshift", pp.vibrance.avoidcolorshift);
        cJSON_AddBoolToObject(s, "pastsattog", pp.vibrance.pastsattog);
    });

    addSection("sh", [&](cJSON* s) {
        cJSON_AddBoolToObject(s, "enabled", pp.sh.enabled);
        cJSON_AddNumberToObject(s, "highlights", pp.sh.highlights);
        cJSON_AddNumberToObject(s, "htonalwidth", pp.sh.htonalwidth);
        cJSON_AddNumberToObject(s, "shadows", pp.sh.shadows);
        cJSON_AddNumberToObject(s, "stonalwidth", pp.sh.stonalwidth);
        cJSON_AddNumberToObject(s, "radius", pp.sh.radius);
    });

    addSection("localContrast", [&](cJSON* s) {
        cJSON_AddBoolToObject(s, "enabled", pp.localContrast.enabled);
        cJSON_AddNumberToObject(s, "radius", pp.localContrast.radius);
        cJSON_AddNumberToObject(s, "amount", pp.localContrast.amount);
        cJSON_AddNumberToObject(s, "darkness", pp.localContrast.darkness);
        cJSON_AddNumberToObject(s, "lightness", pp.localContrast.lightness);
    });

    addSection("defringe", [&](cJSON* s) {
        cJSON_AddBoolToObject(s, "enabled", pp.defringe.enabled);
        cJSON_AddNumberToObject(s, "radius", pp.defringe.radius);
        cJSON_AddNumberToObject(s, "threshold", pp.defringe.threshold);
    });

    addSection("dirpyrDenoise", [&](cJSON* s) {
        cJSON_AddBoolToObject(s, "enabled", pp.dirpyrDenoise.enabled);
        cJSON_AddBoolToObject(s, "enhance", pp.dirpyrDenoise.enhance);
        cJSON_AddNumberToObject(s, "luma", pp.dirpyrDenoise.luma);
        cJSON_AddNumberToObject(s, "Ldetail", pp.dirpyrDenoise.Ldetail);
        cJSON_AddNumberToObject(s, "chroma", pp.dirpyrDenoise.chroma);
        cJSON_AddNumberToObject(s, "redchro", pp.dirpyrDenoise.redchro);
        cJSON_AddNumberToObject(s, "bluechro", pp.dirpyrDenoise.bluechro);
        cJSON_AddNumberToObject(s, "gamma", pp.dirpyrDenoise.gamma);
    });

    addSection("crop", [&](cJSON* s) {
        cJSON_AddBoolToObject(s, "enabled", pp.crop.enabled);
        cJSON_AddNumberToObject(s, "x", pp.crop.x);
        cJSON_AddNumberToObject(s, "y", pp.crop.y);
        cJSON_AddNumberToObject(s, "w", pp.crop.w);
        cJSON_AddNumberToObject(s, "h", pp.crop.h);
        cJSON_AddBoolToObject(s, "fixratio", pp.crop.fixratio);
        cJSON_AddStringToObject(s, "ratio", pp.crop.ratio.c_str());
    });

    addSection("rotate", [&](cJSON* s) {
        cJSON_AddNumberToObject(s, "degree", pp.rotate.degree);
    });

    addSection("distortion", [&](cJSON* s) {
        cJSON_AddNumberToObject(s, "amount", pp.distortion.amount);
    });

    addSection("resize", [&](cJSON* s) {
        cJSON_AddBoolToObject(s, "enabled", pp.resize.enabled);
        cJSON_AddNumberToObject(s, "scale", pp.resize.scale);
        cJSON_AddStringToObject(s, "method", pp.resize.method.c_str());
        cJSON_AddNumberToObject(s, "width", pp.resize.width);
        cJSON_AddNumberToObject(s, "height", pp.resize.height);
        cJSON_AddBoolToObject(s, "allowUpscaling", pp.resize.allowUpscaling);
    });

    addSection("colorGrading", [&](cJSON* s) {
        cJSON_AddBoolToObject(s, "enabled", pp.colorGrading.enabled);
    });

    addSection("rgbCurves", [&](cJSON* s) {
        cJSON_AddBoolToObject(s, "enabled", pp.rgbCurves.enabled);
        cJSON_AddBoolToObject(s, "lumamode", pp.rgbCurves.lumamode);
    });

    addSection("gradient", [&](cJSON* s) {
        cJSON_AddBoolToObject(s, "enabled", pp.gradient.enabled);
        cJSON_AddNumberToObject(s, "degree", pp.gradient.degree);
        cJSON_AddNumberToObject(s, "feather", pp.gradient.feather);
        cJSON_AddNumberToObject(s, "strength", pp.gradient.strength);
        cJSON_AddNumberToObject(s, "centerX", pp.gradient.centerX);
        cJSON_AddNumberToObject(s, "centerY", pp.gradient.centerY);
    });

    addSection("dehaze", [&](cJSON* s) {
        cJSON_AddBoolToObject(s, "enabled", pp.dehaze.enabled);
        cJSON_AddNumberToObject(s, "strength", pp.dehaze.strength);
        cJSON_AddNumberToObject(s, "saturation", pp.dehaze.saturation);
        cJSON_AddNumberToObject(s, "depth", pp.dehaze.depth);
    });

    addSection("softlight", [&](cJSON* s) {
        cJSON_AddBoolToObject(s, "enabled", pp.softlight.enabled);
        cJSON_AddNumberToObject(s, "strength", pp.softlight.strength);
    });

    addSection("fattal", [&](cJSON* s) {
        cJSON_AddBoolToObject(s, "enabled", pp.fattal.enabled);
        cJSON_AddNumberToObject(s, "threshold", pp.fattal.threshold);
        cJSON_AddNumberToObject(s, "amount", pp.fattal.amount);
    });

    addSection("filmSimulation", [&](cJSON* s) {
        cJSON_AddBoolToObject(s, "enabled", pp.filmSimulation.enabled);
        cJSON_AddStringToObject(s, "clutFilename", pp.filmSimulation.clutFilename.c_str());
        cJSON_AddNumberToObject(s, "strength", pp.filmSimulation.strength);
    });

    addSection("blackwhite", [&](cJSON* s) {
        cJSON_AddBoolToObject(s, "enabled", pp.blackwhite.enabled);
    });

    addSection("retinex", [&](cJSON* s) {
        cJSON_AddBoolToObject(s, "enabled", pp.retinex.enabled);
        cJSON_AddNumberToObject(s, "str", pp.retinex.str);
        cJSON_AddNumberToObject(s, "neigh", pp.retinex.neigh);
    });

    addSection("hsvequalizer", [&](cJSON* s) {
        cJSON_AddBoolToObject(s, "enabled", pp.hsvequalizer.enabled);
    });

    addSection("icm", [&](cJSON* s) {
        cJSON_AddStringToObject(s, "inputProfile", pp.icm.inputProfile.c_str());
        cJSON_AddStringToObject(s, "workingProfile", pp.icm.workingProfile.c_str());
        cJSON_AddStringToObject(s, "outputProfile", pp.icm.outputProfile.c_str());
    });

    return root;
}

std::vector<ToolDef> getToolDefinitions() {
    std::vector<ToolDef> tools;

    // get_image_info
    tools.push_back({
        "get_image_info",
        "Get information about the currently open image including filename, dimensions, and EXIF metadata.",
        createObjectSchema({}, {})
    });

    // get_params
    tools.push_back({
        "get_params",
        "Get current processing parameters. Optionally filter by section name (e.g., 'toneCurve', 'wb', 'sharpening').",
        createObjectSchema({{"section", "string"}}, {})
    });

    // set_params
    tools.push_back({
        "set_params",
        "Set processing parameters and trigger reprocessing. Pass a JSON object with section names as keys. "
        "Sections: toneCurve, labCurve, wb, sharpening, vibrance, sh, localContrast, defringe, dirpyrDenoise, "
        "crop, rotate, distortion, resize, gradient, dehaze, softlight, fattal, filmSimulation, blackwhite, "
        "retinex, hsvequalizer, rgbCurves, colorGrading. "
        "Example: {\"params\": {\"toneCurve\": {\"expcomp\": 1.5, \"contrast\": 20}, \"wb\": {\"temperature\": 5500}}}",
        createObjectSchema({{"params", "object"}}, {"params"})
    });

    // set_tool_enabled
    tools.push_back({
        "set_tool_enabled",
        "Enable or disable a specific processing tool. "
        "Tool names: labCurve, sharpening, vibrance, wb, defringe, dirpyrDenoise, impulseDenoise, sh, "
        "localContrast, crop, resize, gradient, pcvignette, retinex, rgbCurves, colorGrading, "
        "dehaze, softlight, fattal, filmSimulation, blackwhite, hsvequalizer.",
        createObjectSchema({{"tool_name", "string"}, {"enabled", "boolean"}}, {"tool_name", "enabled"})
    });

    // list_tools
    tools.push_back({
        "list_tools",
        "List all processing tools with their current enabled/disabled state.",
        createObjectSchema({}, {})
    });

    // adjust_exposure - convenience tool
    tools.push_back({
        "adjust_exposure",
        "Convenience tool to adjust exposure parameters. All parameters are optional. "
        "expcomp: exposure compensation (-5 to 12 EV), brightness: (-100 to 100), "
        "contrast: (-100 to 100), saturation: (-100 to 100), black: (0 to 32768).",
        createObjectSchema({
            {"expcomp", "number"}, {"brightness", "number"}, {"contrast", "number"},
            {"saturation", "number"}, {"black", "number"}
        }, {})
    });

    // adjust_white_balance - convenience tool
    tools.push_back({
        "adjust_white_balance",
        "Convenience tool to adjust white balance. "
        "temperature: color temperature in Kelvin (2000-25000), green: green/magenta tint (0.02-10.0), "
        "method: 'Camera', 'Auto', or 'Custom'.",
        createObjectSchema({
            {"temperature", "number"}, {"green", "number"}, {"method", "string"}
        }, {})
    });

    // load_profile
    tools.push_back({
        "load_profile",
        "Load a PP3 processing profile from a file and apply it to the current image.",
        createObjectSchema({{"filename", "string"}}, {"filename"})
    });

    // save_profile
    tools.push_back({
        "save_profile",
        "Save the current processing parameters as a PP3 profile file.",
        createObjectSchema({{"filename", "string"}}, {"filename"})
    });

    // list_volumes
    tools.push_back({
        "list_volumes",
        "List all connected drives and volumes with name, mount path, type (removable/fixed/unknown), and mount status.",
        createObjectSchema({}, {})
    });

    // get_mount_events
    tools.push_back({
        "get_mount_events",
        "Get mount/unmount events since the last poll. Returns events accumulated since the MCP server started or since the last call to this tool.",
        createObjectSchema({}, {})
    });

    // scan_photos
    tools.push_back({
        "scan_photos",
        "Scan a directory for image files and return file paths with EXIF metadata (dateTime, camera, ISO, fnumber, focalLength, shutterSpeed). "
        "Useful for surveying photos on an SD card or in a folder before import.",
        createObjectSchema({
            {"path", "string"}, {"recursive", "boolean"}, {"limit", "number"}
        }, {"path"})
    });

    // import_photos
    tools.push_back({
        "import_photos",
        "Copy or move image files from a source directory to a destination, organizing into date-based subfolders using EXIF metadata. "
        "folder_template uses YYYY, MM, DD placeholders (default 'YYYY/YYYY-MM-DD'). "
        "duplicate_action: 'skip' (default) or 'rename' (appends _1, _2, etc).",
        createObjectSchema({
            {"source", "string"}, {"destination", "string"}, {"action", "string"},
            {"recursive", "boolean"}, {"folder_template", "string"}, {"duplicate_action", "string"}
        }, {"source", "destination"})
    });

    return tools;
}

std::string handleToolCall(const std::string& toolName, cJSON* args, McpServer* server) {
    EditorPanel* ep = server->getEditorPanel();

    // Tools that don't require an open image
    if (toolName == "list_tools") {
        if (!ep || !ep->getIpc()) {
            cJSON* result = buildToolResult("No image is currently open. Tool enabled states unavailable.");
            std::string s = jsonStr(result);
            cJSON_Delete(result);
            return s;
        }

        ProcParams pp;
        ep->getIpc()->getParams(&pp);

        cJSON* arr = cJSON_CreateArray();
        for (const auto& name : getToolNames()) {
            cJSON* item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "name", name.c_str());
            cJSON_AddBoolToObject(item, "enabled", getToolEnabled(pp, name));
            cJSON_AddItemToArray(arr, item);
        }

        cJSON* result = buildToolResultJson(arr);
        cJSON_Delete(arr);
        std::string s = jsonStr(result);
        cJSON_Delete(result);
        return s;
    }

    // --- Volume / import tools (no open image required) ---

    if (toolName == "list_volumes") {
        auto vols = server->getVolumeWatcher().getVolumes();

        cJSON* arr = cJSON_CreateArray();
        for (const auto& v : vols) {
            cJSON* item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "name", v.name.c_str());
            cJSON_AddStringToObject(item, "path", v.path.c_str());
            cJSON_AddStringToObject(item, "type", v.type.c_str());
            cJSON_AddBoolToObject(item, "mounted", v.mounted);
            cJSON_AddItemToArray(arr, item);
        }

        cJSON* wrap = cJSON_CreateObject();
        cJSON_AddItemToObject(wrap, "volumes", arr);
        cJSON* result = buildToolResultJson(wrap);
        cJSON_Delete(wrap);
        std::string s = jsonStr(result);
        cJSON_Delete(result);
        return s;
    }

    if (toolName == "get_mount_events") {
        auto events = server->getVolumeWatcher().drainEvents();

        cJSON* arr = cJSON_CreateArray();
        for (const auto& e : events) {
            cJSON* item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "event", e.event.c_str());
            cJSON_AddStringToObject(item, "name", e.name.c_str());
            cJSON_AddStringToObject(item, "path", e.path.c_str());
            cJSON_AddStringToObject(item, "timestamp", e.timestamp.c_str());
            cJSON_AddItemToArray(arr, item);
        }

        cJSON* wrap = cJSON_CreateObject();
        cJSON_AddItemToObject(wrap, "events", arr);
        cJSON* result = buildToolResultJson(wrap);
        cJSON_Delete(wrap);
        std::string s = jsonStr(result);
        cJSON_Delete(result);
        return s;
    }

    if (toolName == "scan_photos") {
        if (!args) {
            cJSON* result = buildToolResult("Missing 'path' argument.", true);
            std::string s = jsonStr(result);
            cJSON_Delete(result);
            return s;
        }

        std::string path = getStr(args, "path");
        if (path.empty()) {
            cJSON* result = buildToolResult("Empty 'path' argument.", true);
            std::string s = jsonStr(result);
            cJSON_Delete(result);
            return s;
        }

        if (!Glib::file_test(path, Glib::FILE_TEST_IS_DIR)) {
            cJSON* result = buildToolResult("Directory not found: " + path, true);
            std::string s = jsonStr(result);
            cJSON_Delete(result);
            return s;
        }

        bool recursive = getBool(args, "recursive", true);
        int limit = static_cast<int>(getNum(args, "limit", 100));
        if (limit < 1) limit = 1;
        if (limit > 1000) limit = 1000;

        const auto& exts = App::get().options().parsedExtensionsSet;
        std::vector<std::string> allFiles;
        collectImageFiles(path, recursive, exts, allFiles);

        int totalFound = static_cast<int>(allFiles.size());
        int returned = std::min(limit, totalFound);

        cJSON* filesArr = cJSON_CreateArray();
        for (int i = 0; i < returned; ++i) {
            cJSON* fileObj = cJSON_CreateObject();
            cJSON_AddStringToObject(fileObj, "path", allFiles[i].c_str());

            try {
                std::unique_ptr<rtengine::FramesMetaData> meta(
                    rtengine::FramesMetaData::fromFile(allFiles[i]));
                if (meta && meta->hasExif()) {
                    time_t ts = meta->getDateTimeAsTS();
                    if (ts > 0) {
                        struct tm tm;
                        portable_localtime(&ts, &tm);
                        char buf[32];
                        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
                        cJSON_AddStringToObject(fileObj, "dateTime", buf);
                    }
                    std::string cam = meta->getCamera();
                    if (!cam.empty() && cam != "Unknown Unknown") {
                        cJSON_AddStringToObject(fileObj, "camera", cam.c_str());
                    }
                    int iso = meta->getISOSpeed();
                    if (iso > 0) cJSON_AddNumberToObject(fileObj, "iso", iso);
                    double fn = meta->getFNumber();
                    if (fn > 0) cJSON_AddNumberToObject(fileObj, "fnumber", fn);
                    double fl = meta->getFocalLen();
                    if (fl > 0) cJSON_AddNumberToObject(fileObj, "focalLength", fl);
                    double ss = meta->getShutterSpeed();
                    if (ss > 0) cJSON_AddNumberToObject(fileObj, "shutterSpeed", ss);
                }
            } catch (...) {
                // EXIF read failed, include path only
            }

            cJSON_AddItemToArray(filesArr, fileObj);
        }

        cJSON* wrap = cJSON_CreateObject();
        cJSON_AddNumberToObject(wrap, "totalFound", totalFound);
        cJSON_AddNumberToObject(wrap, "returned", returned);
        cJSON_AddItemToObject(wrap, "files", filesArr);
        cJSON* result = buildToolResultJson(wrap);
        cJSON_Delete(wrap);
        std::string s = jsonStr(result);
        cJSON_Delete(result);
        return s;
    }

    if (toolName == "import_photos") {
        if (!args) {
            cJSON* result = buildToolResult("Missing arguments.", true);
            std::string s = jsonStr(result);
            cJSON_Delete(result);
            return s;
        }

        std::string source = getStr(args, "source");
        std::string destination = getStr(args, "destination");
        if (source.empty() || destination.empty()) {
            cJSON* result = buildToolResult("Both 'source' and 'destination' are required.", true);
            std::string s = jsonStr(result);
            cJSON_Delete(result);
            return s;
        }

        if (!Glib::file_test(source, Glib::FILE_TEST_IS_DIR)) {
            cJSON* result = buildToolResult("Source directory not found: " + source, true);
            std::string s = jsonStr(result);
            cJSON_Delete(result);
            return s;
        }

        std::string action = getStr(args, "action", "copy");
        bool recursive = getBool(args, "recursive", true);
        std::string folderTemplate = getStr(args, "folder_template", "YYYY/YYYY-MM-DD");
        std::string dupAction = getStr(args, "duplicate_action", "skip");

        const auto& exts = App::get().options().parsedExtensionsSet;
        std::vector<std::string> allFiles;
        collectImageFiles(source, recursive, exts, allFiles);

        int totalFiles = static_cast<int>(allFiles.size());
        int imported = 0, skipped = 0, failed = 0;
        cJSON* filesArr = cJSON_CreateArray();

        for (const auto& srcPath : allFiles) {
            cJSON* fileObj = cJSON_CreateObject();
            std::string basename = Glib::path_get_basename(srcPath);
            cJSON_AddStringToObject(fileObj, "source", srcPath.c_str());

            // Determine date subfolder
            std::string dateFolder = getExifDateFolder(srcPath, folderTemplate);
            std::string destDir = Glib::build_filename(destination, dateFolder);

            // Create destination directory
            if (g_mkdir_with_parents(destDir.c_str(), 0755) != 0) {
                cJSON_AddStringToObject(fileObj, "status", "failed");
                cJSON_AddStringToObject(fileObj, "error", "Could not create directory");
                cJSON_AddItemToArray(filesArr, fileObj);
                ++failed;
                continue;
            }

            std::string destPath = Glib::build_filename(destDir, basename);

            // Handle duplicates
            if (Glib::file_test(destPath, Glib::FILE_TEST_EXISTS)) {
                if (dupAction == "skip") {
                    cJSON_AddStringToObject(fileObj, "destination", destPath.c_str());
                    cJSON_AddStringToObject(fileObj, "status", "skipped");
                    cJSON_AddItemToArray(filesArr, fileObj);
                    ++skipped;
                    continue;
                }
                // rename: find unique name
                auto dotPos = basename.find_last_of('.');
                std::string stem = (dotPos != std::string::npos) ? basename.substr(0, dotPos) : basename;
                std::string ext = (dotPos != std::string::npos) ? basename.substr(dotPos) : "";
                bool found = false;
                for (int i = 1; i < 10000; ++i) {
                    std::string newName = stem + "_" + std::to_string(i) + ext;
                    std::string newPath = Glib::build_filename(destDir, newName);
                    if (!Glib::file_test(newPath, Glib::FILE_TEST_EXISTS)) {
                        destPath = newPath;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    cJSON_AddStringToObject(fileObj, "status", "failed");
                    cJSON_AddStringToObject(fileObj, "error", "Could not find unique filename");
                    cJSON_AddItemToArray(filesArr, fileObj);
                    ++failed;
                    continue;
                }
            }

            // Copy or move
            try {
                auto srcFile = Gio::File::create_for_path(srcPath);
                auto dstFile = Gio::File::create_for_path(destPath);

                if (action == "move") {
                    srcFile->move(dstFile);
                } else {
                    srcFile->copy(dstFile);
                }

                cJSON_AddStringToObject(fileObj, "destination", destPath.c_str());
                cJSON_AddStringToObject(fileObj, "status", action == "move" ? "moved" : "copied");
                ++imported;
            } catch (const Gio::Error& e) {
                cJSON_AddStringToObject(fileObj, "status", "failed");
                cJSON_AddStringToObject(fileObj, "error", e.what().c_str());
                ++failed;
            } catch (const Glib::Error& e) {
                cJSON_AddStringToObject(fileObj, "status", "failed");
                cJSON_AddStringToObject(fileObj, "error", e.what().c_str());
                ++failed;
            }

            cJSON_AddItemToArray(filesArr, fileObj);
        }

        cJSON* wrap = cJSON_CreateObject();
        cJSON_AddNumberToObject(wrap, "totalFiles", totalFiles);
        cJSON_AddNumberToObject(wrap, "imported", imported);
        cJSON_AddNumberToObject(wrap, "skipped", skipped);
        cJSON_AddNumberToObject(wrap, "failed", failed);
        cJSON_AddItemToObject(wrap, "files", filesArr);
        cJSON* result = buildToolResultJson(wrap);
        cJSON_Delete(wrap);
        std::string s = jsonStr(result);
        cJSON_Delete(result);
        return s;
    }

    // All other tools require an open image
    if (!ep || !ep->getIpc()) {
        cJSON* result = buildToolResult("No image is currently open in the editor.", true);
        std::string s = jsonStr(result);
        cJSON_Delete(result);
        return s;
    }

    if (toolName == "get_image_info") {
        auto* ipc = ep->getIpc();
        auto* isrc = ipc->getInitialImage();

        cJSON* info = cJSON_CreateObject();
        cJSON_AddStringToObject(info, "filename", isrc->getFileName().c_str());
        cJSON_AddNumberToObject(info, "width", ipc->getFullWidth());
        cJSON_AddNumberToObject(info, "height", ipc->getFullHeight());

        const auto* meta = isrc->getMetaData();
        if (meta && meta->hasExif()) {
            cJSON* exif = cJSON_CreateObject();
            cJSON_AddNumberToObject(exif, "iso", meta->getISOSpeed());
            cJSON_AddNumberToObject(exif, "fnumber", meta->getFNumber());
            cJSON_AddNumberToObject(exif, "focalLength", meta->getFocalLen());
            cJSON_AddNumberToObject(exif, "focalLength35mm", meta->getFocalLen35mm());
            cJSON_AddNumberToObject(exif, "shutterSpeed", meta->getShutterSpeed());
            cJSON_AddNumberToObject(exif, "exposureComp", meta->getExpComp());
            cJSON_AddStringToObject(exif, "camera", meta->getCamera().c_str());
            cJSON_AddStringToObject(exif, "lens", meta->getLens().c_str());
            cJSON_AddItemToObject(info, "exif", exif);
        }

        cJSON* result = buildToolResultJson(info);
        cJSON_Delete(info);
        std::string s = jsonStr(result);
        cJSON_Delete(result);
        return s;
    }

    if (toolName == "get_params") {
        ProcParams pp;
        ep->getIpc()->getParams(&pp);

        std::string section = args ? getStr(args, "section") : "";
        cJSON* params = paramsToJson(pp, section);
        cJSON* result = buildToolResultJson(params);
        cJSON_Delete(params);
        std::string s = jsonStr(result);
        cJSON_Delete(result);
        return s;
    }

    if (toolName == "set_params") {
        if (!args) {
            cJSON* result = buildToolResult("Missing 'params' argument.", true);
            std::string s = jsonStr(result);
            cJSON_Delete(result);
            return s;
        }

        cJSON* paramsJson = cJSON_GetObjectItemCaseSensitive(args, "params");
        if (!paramsJson) {
            cJSON* result = buildToolResult("Missing 'params' key in arguments.", true);
            std::string s = jsonStr(result);
            cJSON_Delete(result);
            return s;
        }

        // Get current params, apply changes, then push via profileChange
        ProcParams pp;
        ep->getIpc()->getParams(&pp);

        if (applyJsonToParams(pp, paramsJson)) {
            PartialProfile profile(true);
            *profile.pparams = pp;
            profile.pedited->set(true); // Mark all as edited
            ep->getTpc()->profileChange(&profile, rtengine::EvProfileChanged, "MCP: set_params");
            profile.deleteInstance();

            cJSON* result = buildToolResult("Parameters applied successfully.");
            std::string s = jsonStr(result);
            cJSON_Delete(result);
            return s;
        } else {
            cJSON* result = buildToolResult("No recognized parameters found in input.", true);
            std::string s = jsonStr(result);
            cJSON_Delete(result);
            return s;
        }
    }

    if (toolName == "set_tool_enabled") {
        if (!args) {
            cJSON* result = buildToolResult("Missing arguments.", true);
            std::string s = jsonStr(result);
            cJSON_Delete(result);
            return s;
        }

        std::string tool_name = getStr(args, "tool_name");
        bool enabled = getBool(args, "enabled");

        ProcParams pp;
        ep->getIpc()->getParams(&pp);

        if (setToolEnabled(pp, tool_name, enabled)) {
            PartialProfile profile(true);
            *profile.pparams = pp;
            profile.pedited->set(true);
            ep->getTpc()->profileChange(&profile, rtengine::EvProfileChanged, "MCP: set_tool_enabled");
            profile.deleteInstance();

            cJSON* result = buildToolResult(
                std::string("Tool '") + tool_name + "' " + (enabled ? "enabled" : "disabled") + ".");
            std::string s = jsonStr(result);
            cJSON_Delete(result);
            return s;
        }

        cJSON* result = buildToolResult("Unknown tool: " + tool_name, true);
        std::string s = jsonStr(result);
        cJSON_Delete(result);
        return s;
    }

    if (toolName == "adjust_exposure") {
        ProcParams pp;
        ep->getIpc()->getParams(&pp);

        if (args) {
            if (hasKey(args, "expcomp")) pp.toneCurve.expcomp = getNum(args, "expcomp");
            if (hasKey(args, "brightness")) pp.toneCurve.brightness = static_cast<int>(getNum(args, "brightness"));
            if (hasKey(args, "contrast")) pp.toneCurve.contrast = static_cast<int>(getNum(args, "contrast"));
            if (hasKey(args, "saturation")) pp.toneCurve.saturation = static_cast<int>(getNum(args, "saturation"));
            if (hasKey(args, "black")) pp.toneCurve.black = static_cast<int>(getNum(args, "black"));
        }

        PartialProfile profile(true);
        *profile.pparams = pp;
        profile.pedited->set(true);
        ep->getTpc()->profileChange(&profile, rtengine::EvProfileChanged, "MCP: adjust_exposure");
        profile.deleteInstance();

        cJSON* result = buildToolResult("Exposure adjusted.");
        std::string s = jsonStr(result);
        cJSON_Delete(result);
        return s;
    }

    if (toolName == "adjust_white_balance") {
        ProcParams pp;
        ep->getIpc()->getParams(&pp);

        if (args) {
            if (hasKey(args, "temperature")) pp.wb.temperature = static_cast<int>(getNum(args, "temperature"));
            if (hasKey(args, "green")) pp.wb.green = getNum(args, "green");
            if (hasKey(args, "method")) pp.wb.method = getStr(args, "method");
        }

        PartialProfile profile(true);
        *profile.pparams = pp;
        profile.pedited->set(true);
        ep->getTpc()->profileChange(&profile, rtengine::EvProfileChanged, "MCP: adjust_white_balance");
        profile.deleteInstance();

        cJSON* result = buildToolResult("White balance adjusted.");
        std::string s = jsonStr(result);
        cJSON_Delete(result);
        return s;
    }

    if (toolName == "load_profile") {
        if (!args) {
            cJSON* result = buildToolResult("Missing 'filename' argument.", true);
            std::string s = jsonStr(result);
            cJSON_Delete(result);
            return s;
        }

        std::string filename = getStr(args, "filename");
        if (filename.empty()) {
            cJSON* result = buildToolResult("Empty filename.", true);
            std::string s = jsonStr(result);
            cJSON_Delete(result);
            return s;
        }

        PartialProfile profile(true);
        int err = profile.load(filename);
        if (err) {
            profile.deleteInstance();
            cJSON* result = buildToolResult("Failed to load profile: " + filename, true);
            std::string s = jsonStr(result);
            cJSON_Delete(result);
            return s;
        }

        ep->getTpc()->profileChange(&profile, rtengine::EvProfileChanged, "MCP: load_profile");
        profile.deleteInstance();

        cJSON* result = buildToolResult("Profile loaded: " + filename);
        std::string s = jsonStr(result);
        cJSON_Delete(result);
        return s;
    }

    if (toolName == "save_profile") {
        if (!args) {
            cJSON* result = buildToolResult("Missing 'filename' argument.", true);
            std::string s = jsonStr(result);
            cJSON_Delete(result);
            return s;
        }

        std::string filename = getStr(args, "filename");
        if (filename.empty()) {
            cJSON* result = buildToolResult("Empty filename.", true);
            std::string s = jsonStr(result);
            cJSON_Delete(result);
            return s;
        }

        ProcParams pp;
        ep->getIpc()->getParams(&pp);
        int err = pp.save(filename);
        if (err) {
            cJSON* result = buildToolResult("Failed to save profile: " + filename, true);
            std::string s = jsonStr(result);
            cJSON_Delete(result);
            return s;
        }

        cJSON* result = buildToolResult("Profile saved: " + filename);
        std::string s = jsonStr(result);
        cJSON_Delete(result);
        return s;
    }

    // Unknown tool
    cJSON* result = buildToolResult("Unknown tool: " + toolName, true);
    std::string s = jsonStr(result);
    cJSON_Delete(result);
    return s;
}

} // namespace mcp
