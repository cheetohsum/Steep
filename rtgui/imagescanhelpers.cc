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
#include "imagescanhelpers.h"

#include <algorithm>
#ifdef _WIN32
#include "rtengine/leanwindows.h"
#endif

#include <algorithm>
#include <climits>
#include <cstring>
#include <memory>

#include <giomm.h>
#include <glibmm/keyfile.h>
#include <glibmm/miscutils.h>

#include "cacheimagedata.h"
#include "cachemanager.h"
#include "options.h"
#include "pathutils.h"

#include "../rtengine/rtengine.h"

namespace
{

// Minimal line scanner over a keyfile's [General] section: reads integer and
// boolean values without constructing a Glib::KeyFile. Returns false if the
// section is absent. Stops at the next section header.
struct GeneralValues {
    long rank = -1;
    long rating = -1;
    long colorLabel = -1;
    long pickLabel = LONG_MIN;
    int supported = -1; // -1 unknown, 0 false, 1 true
};

bool scanGeneralSection(const char* data, size_t length, GeneralValues& values)
{
    const char* pos = data;
    const char* const end = data + length;
    bool inGeneral = false;
    bool sawGeneral = false;

    const auto lineValue = [](const char* line, const char* lineEnd, const char* key, size_t keyLen) -> const char* {
        if (static_cast<size_t>(lineEnd - line) <= keyLen || strncmp(line, key, keyLen) != 0 || line[keyLen] != '=') {
            return nullptr;
        }

        return line + keyLen + 1;
    };

    while (pos < end) {
        const char* lineEnd = pos;

        while (lineEnd < end && *lineEnd != '\n' && *lineEnd != '\r') {
            ++lineEnd;
        }

        if (pos < lineEnd) {
            if (*pos == '[') {
                if (inGeneral) {
                    break; // [General] ended
                }

                inGeneral = strncmp(pos, "[General]", 9) == 0;
                sawGeneral = sawGeneral || inGeneral;
            } else if (inGeneral) {
                if (const char* v = lineValue(pos, lineEnd, "Rank", 4)) {
                    values.rank = strtol(v, nullptr, 10);
                } else if (const char* v2 = lineValue(pos, lineEnd, "Rating", 6)) {
                    values.rating = strtol(v2, nullptr, 10);
                } else if (const char* v3 = lineValue(pos, lineEnd, "ColorLabel", 10)) {
                    values.colorLabel = strtol(v3, nullptr, 10);
                } else if (const char* v4 = lineValue(pos, lineEnd, "PickLabel", 9)) {
                    values.pickLabel = strtol(v4, nullptr, 10);
                } else if (const char* v5 = lineValue(pos, lineEnd, "Supported", 9)) {
                    values.supported = strncmp(v5, "true", 4) == 0 ? 1 : 0;
                }
            }
        }

        while (lineEnd < end && (*lineEnd == '\n' || *lineEnd == '\r')) {
            ++lineEnd;
        }

        pos = lineEnd;
    }

    return sawGeneral;
}

} // namespace

namespace imagescan
{

std::vector<Glib::ustring> listImageFiles(const Glib::ustring& dirPath)
{
    std::vector<Glib::ustring> result;

    try {
        auto dir = Gio::File::create_for_path(dirPath);

        if (!dir->query_exists()) {
            return result;
        }

        const auto& exts = App::get().options().parsedExtensionsSet;
        auto enumerator = dir->enumerate_children("standard::name,standard::type");

        while (auto info = enumerator->next_file()) {
            if (info->get_file_type() != Gio::FILE_TYPE_REGULAR) {
                continue;
            }

            Glib::ustring name = info->get_name();
            auto dotPos = name.rfind('.');

            if (dotPos == Glib::ustring::npos) {
                continue;
            }

            std::string ext = Glib::ustring(name.substr(dotPos + 1)).lowercase();

            if (exts.count(ext)) {
                result.push_back(Glib::build_filename(dirPath, name));
            }
        }
    } catch (...) {}

    return result;
}

std::vector<Glib::ustring> listImageFilesRecursive(const Glib::ustring& dirPath, int maxDepth)
{
    std::vector<Glib::ustring> result = listImageFiles(dirPath);

    if (maxDepth <= 0) {
        return result;
    }

    try {
        auto dir = Gio::File::create_for_path(dirPath);

        if (!dir->query_exists()) {
            return result;
        }

        auto enumerator = dir->enumerate_children("standard::name,standard::type,standard::is-hidden");

        while (auto info = enumerator->next_file()) {
            if (info->get_file_type() != Gio::FILE_TYPE_DIRECTORY || info->is_hidden()) {
                continue;
            }

            const Glib::ustring name = info->get_name();

            if (!name.empty() && name[0] == '.') {
                continue;
            }

            auto sub = listImageFilesRecursive(Glib::build_filename(dirPath, name), maxDepth - 1);
            result.insert(result.end(), sub.begin(), sub.end());
        }
    } catch (...) {}

    return result;
}

bool listImageFilesStamped(const Glib::ustring& dirPath, std::vector<Glib::ustring>& files,
                           std::vector<Glib::ustring>& subdirs, std::uint64_t& signature)
{
    const auto& exts = App::get().options().parsedExtensionsSet;
    struct Item {
        std::string name;
        std::uint64_t size;
        std::uint64_t mtime;
        bool operator<(const Item& o) const { return name < o.name; }
    };
    std::vector<Item> items;

    const auto consider = [&](const Glib::ustring& name, std::uint64_t size, std::uint64_t mtime) {
        const auto dot = name.rfind('.');
        if (dot == Glib::ustring::npos) {
            return;
        }
        const std::string ext = Glib::ustring(name.substr(dot + 1)).lowercase().raw();
        const bool photo = exts.count(ext) > 0;
        if (photo) {
            files.push_back(Glib::build_filename(dirPath, name));
        }
        if (photo || ext == "pp3" || ext == "xmp") {
            items.push_back({name.casefold().raw(), size, mtime});
        }
    };

#ifdef _WIN32
    // FindFirstFileEx returns size and write time with the listing: no stat
    // per file, which is what made walking a spinning disk slow.
    gunichar2* pattern = g_utf8_to_utf16(Glib::build_filename(dirPath, "*").c_str(), -1, nullptr, nullptr, nullptr);
    if (!pattern) {
        return false;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(reinterpret_cast<wchar_t*>(pattern), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    g_free(pattern);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    do {
        gchar* utf8 = g_utf16_to_utf8(reinterpret_cast<const gunichar2*>(fd.cFileName), -1, nullptr, nullptr, nullptr);
        if (!utf8) {
            continue;
        }
        const Glib::ustring name(utf8);
        g_free(utf8);
        if (name == "." || name == "..") {
            continue;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) && name[0] != '.') {
                subdirs.push_back(Glib::build_filename(dirPath, name));
            }
            continue;
        }
        const std::uint64_t size = (static_cast<std::uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        const std::uint64_t mtime = (static_cast<std::uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32)
                                  | fd.ftLastWriteTime.dwLowDateTime;
        consider(name, size, mtime);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    try {
        auto dir = Gio::File::create_for_path(dirPath);
        auto enumerator = dir->enumerate_children("standard::name,standard::type,standard::is-hidden,standard::size,time::modified");
        while (auto info = enumerator->next_file()) {
            const Glib::ustring name = info->get_name();
            if (info->get_file_type() == Gio::FILE_TYPE_DIRECTORY) {
                if (!info->is_hidden() && !name.empty() && name[0] != '.') {
                    subdirs.push_back(Glib::build_filename(dirPath, name));
                }
                continue;
            }
            if (info->get_file_type() != Gio::FILE_TYPE_REGULAR) {
                continue;
            }
            consider(name, static_cast<std::uint64_t>(info->get_size()),
                     static_cast<std::uint64_t>(info->get_attribute_uint64("time::modified")));
        }
    } catch (...) {
        return false;
    }
#endif

    std::sort(items.begin(), items.end());
    std::uint64_t hash = 1469598103934665603ULL;   // FNV-1a 64
    const auto mix = [&hash](const void* data, size_t n) {
        const unsigned char* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < n; ++i) {
            hash ^= p[i];
            hash *= 1099511628211ULL;
        }
    };
    for (const auto& it : items) {
        mix(it.name.data(), it.name.size());
        mix("\0", 1);
        mix(&it.size, sizeof(it.size));
        mix(&it.mtime, sizeof(it.mtime));
    }
    signature = hash ? hash : 1;
    return true;
}

bool loadCacheDataForFile(const Glib::ustring& fpath, CacheImageData& cid)
{
    std::string md5 = cacheMgr->getMD5(fpath);

    if (md5.empty()) {
        return false;
    }

    Glib::ustring cacheName = cacheMgr->getCacheFileName("data", fpath, ".txt", md5);
    return cid.load(cacheName) == 0 && cid.supported;
}

bool loadExifForFile(const Glib::ustring& fpath, CacheImageData& cid)
{
    try {
        std::unique_ptr<rtengine::FramesMetaData> meta(
            rtengine::FramesMetaData::fromFile(fpath));

        if (!meta) {
            return false;
        }

        cid.exifValid = meta->hasExif();

        if (cid.exifValid) {
            cid.fnumber = meta->getFNumber();
            cid.shutter = meta->getShutterSpeed();
            cid.focalLen = meta->getFocalLen();
            cid.iso = meta->getISOSpeed();
            cid.lens = meta->getLens();
            cid.camMake = meta->getMake();
            cid.camModel = meta->getModel();
            cid.rating = meta->getRating();
            cid.colorLabel = meta->getColorLabel();
        }

        cid.updateCameraName();
        cid.filetype = getExtension(fpath).lowercase();
        cid.updateFiletypeUpper();
        cid.supported = true;
        return true;
    } catch (...) {
        return false;
    }
}

bool loadRankFromPP3(const Glib::ustring& fpath, CacheImageData& cid)
{
    // PP3 sidecar is imagefile.ext.pp3
    Glib::ustring pp3path = fpath + ".pp3";

    try {
        Glib::KeyFile kf;

        if (!kf.load_from_file(pp3path)) {
            return false;
        }

        if (kf.has_key("General", "Rank")) {
            int rank = kf.get_integer("General", "Rank");

            if (rank >= 0) {
                // Use the higher of EXIF rating and PP3 rank
                cid.rating = std::max(cid.rating, rank);
            }
        }

        if (kf.has_key("General", "ColorLabel")) {
            int cl = kf.get_integer("General", "ColorLabel");

            if (cl > 0) {
                cid.colorLabel = cl;
            }
        }

        return true;
    } catch (...) {
        return false;
    }
}

bool loadPickFromPP3(const Glib::ustring& fpath, CacheImageData& cid)
{
    Glib::ustring pp3path = fpath + ".pp3";

    try {
        Glib::KeyFile kf;

        if (!kf.load_from_file(pp3path)) {
            return false;
        }

        if (kf.has_key("General", "PickLabel")) {
            cid.pickLabel = kf.get_integer("General", "PickLabel");
            return true;
        }

        return false;
    } catch (...) {
        return false;
    }
}

bool loadCacheDataFast(const Glib::ustring& fpath, CacheImageData& cid)
{
    const std::string md5 = cacheMgr->getMD5(fpath);

    if (md5.empty()) {
        return false;
    }

    const Glib::ustring cacheName = cacheMgr->getCacheFileName("data", fpath, ".txt", md5);

    gchar* raw = nullptr;
    gsize length = 0;

    if (!g_file_get_contents(cacheName.c_str(), &raw, &length, nullptr)) {
        return false;
    }

    GeneralValues values;
    const bool ok = scanGeneralSection(raw, length, values);
    g_free(raw);

    if (!ok) {
        return false;
    }

    if (values.rating >= 0) {
        cid.rating = static_cast<int>(values.rating);
    }

    if (values.colorLabel >= 0) {
        cid.colorLabel = static_cast<int>(values.colorLabel);
    }

    if (values.pickLabel != LONG_MIN) {
        cid.pickLabel = static_cast<int>(values.pickLabel);
    }

    // Entries always persist Supported; treat a missing key (ancient cache)
    // as supported since the entry exists at all.
    cid.supported = values.supported != 0;

    return cid.supported;
}

bool loadPP3OverlaysFast(const Glib::ustring& fpath, CacheImageData& cid)
{
    gchar* raw = nullptr;
    gsize length = 0;

    if (!g_file_get_contents((fpath + ".pp3").c_str(), &raw, &length, nullptr)) {
        return false;
    }

    GeneralValues values;
    const bool ok = scanGeneralSection(raw, length, values);
    g_free(raw);

    if (!ok) {
        return false;
    }

    if (values.rank >= 0) {
        cid.rating = std::max(cid.rating, static_cast<int>(values.rank));
    }

    if (values.colorLabel > 0) {
        cid.colorLabel = static_cast<int>(values.colorLabel);
    }

    if (values.pickLabel != LONG_MIN) {
        cid.pickLabel = static_cast<int>(values.pickLabel);
    }

    return true;
}

bool loadPP3Overlays(const Glib::ustring& fpath, CacheImageData& cid)
{
    Glib::ustring pp3path = fpath + ".pp3";

    try {
        Glib::KeyFile kf;

        if (!kf.load_from_file(pp3path)) {
            return false;
        }

        if (kf.has_key("General", "Rank")) {
            const int rank = kf.get_integer("General", "Rank");

            if (rank >= 0) {
                cid.rating = std::max(cid.rating, rank);
            }
        }

        if (kf.has_key("General", "ColorLabel")) {
            const int cl = kf.get_integer("General", "ColorLabel");

            if (cl > 0) {
                cid.colorLabel = cl;
            }
        }

        if (kf.has_key("General", "PickLabel")) {
            cid.pickLabel = kf.get_integer("General", "PickLabel");
        }

        return true;
    } catch (...) {
        return false;
    }
}

} // namespace imagescan
