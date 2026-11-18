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

#include <cstdint>
#include <vector>

#include <glibmm/ustring.h>

class CacheImageData;

// Lightweight folder/metadata scanning shared by the album browser's smart
// albums and the double-exposure picker. None of these create Thumbnails.
namespace imagescan
{

// All image files (per parsed extensions) directly inside dirPath.
std::vector<Glib::ustring> listImageFiles(const Glib::ustring& dirPath);

// As listImageFiles, but also descends into subdirectories up to maxDepth
// levels (0 = just dirPath). Hidden directories are skipped.
std::vector<Glib::ustring> listImageFilesRecursive(const Glib::ustring& dirPath, int maxDepth);

// One listing of a folder: its photos, its visible subfolders, and a
// signature over every photo and .pp3/.xmp sidecar (name, size, write time).
// The signature changes whenever a file appears, goes, or is rewritten.
// Never 0 on success; false when the folder can't be read.
bool listImageFilesStamped(const Glib::ustring& dirPath, std::vector<Glib::ustring>& files,
                           std::vector<Glib::ustring>& subdirs, std::uint64_t& signature);

// Load CacheImageData from the data cache without creating a Thumbnail.
bool loadCacheDataForFile(const Glib::ustring& fpath, CacheImageData& cid);

// EXIF-only read (rating/label/camera info); no thumbnail generation.
bool loadExifForFile(const Glib::ustring& fpath, CacheImageData& cid);

// Overlay rank/color label from the PP3 sidecar onto cid.
bool loadRankFromPP3(const Glib::ustring& fpath, CacheImageData& cid);

// Load pick flag (pickLabel) from the PP3 sidecar if present.
bool loadPickFromPP3(const Glib::ustring& fpath, CacheImageData& cid);

// Overlay rank, color label, and pick flag from the PP3 sidecar in a single
// parse (cheaper than the individual helpers when all three are wanted).
bool loadPP3Overlays(const Glib::ustring& fpath, CacheImageData& cid);

// Scan-optimized readers: raw line scans of the [General] section instead of
// a full Glib::KeyFile parse — roughly an order of magnitude cheaper per
// file. They fill only rating/pickLabel/colorLabel (+ supported for the
// cache reader); everything else in cid is untouched.
bool loadCacheDataFast(const Glib::ustring& fpath, CacheImageData& cid);
bool loadPP3OverlaysFast(const Glib::ustring& fpath, CacheImageData& cid);

} // namespace imagescan
