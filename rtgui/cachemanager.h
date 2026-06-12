/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2004-2010 Gabor Horvath <hgabor@rawtherapee.com>
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

#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glibmm/ustring.h>

#include "threadutils.h"

#include "rtengine/noncopyable.h"

class CacheImageData;
class Thumbnail;

class CacheManager :
    public rtengine::NonCopyable
{
private:
    using Entries = std::unordered_map<std::string, Thumbnail*>;
    enum class KnownFileState {
        UNKNOWN,
        MISSING,
        PRESENT
    };

    Entries openEntries;
    Glib::ustring    baseDir;
    Glib::ustring    profilesDir_;
    Glib::ustring    imagesDir_;
    Glib::ustring    embProfilesDir_;
    Glib::ustring    dataDir_;
    mutable MyMutex  mutex;

    // MD5 cache: avoids redundant per-file GetFileAttributesExW / stat
    // calls when the same file is accessed multiple times (e.g. preview
    // load then thumbnail upgrade). Empty values mean known missing.
    // Populated in bulk by precomputeMD5().
    // Mutable because caching is a transparent optimization.
    mutable std::unordered_map<std::string, std::string> md5Cache_;
    mutable std::unordered_set<std::string> knownPresentFiles_;
    mutable std::unordered_set<std::string> fullyScannedMD5Dirs_;
    mutable std::unordered_set<std::string> fullyScannedMD5Files_;
    mutable std::mutex md5CacheMutex_;

    void deleteDir   (const Glib::ustring& dirName) const;
    void deleteFiles (const Glib::ustring& fname, const std::string& md5, bool purgeData, bool purgeProfile) const;
    KnownFileState getKnownFileState (const Glib::ustring& fname) const;
    void precomputeFileStates (const std::vector<Glib::ustring>& files, std::size_t directoryScanThreshold, bool hashPresentFiles);

    void applyCacheSizeLimitation () const;
    void updateImageInfo(const Glib::ustring &fname, CacheImageData &imageData, const Glib::ustring &xmpSidecarMd5) const;

public:
    static CacheManager* getInstance ();

    void        init        ();

    Thumbnail*  getEntry    (const Glib::ustring& fname);
    void        reserveEntries (std::size_t additionalEntries);
    void        deleteEntry (const Glib::ustring& fname);
    void        renameEntry (const std::string& oldfilename, const std::string& oldmd5, const std::string& newfilename);

    void closeThumbnail (Thumbnail* thumbnail);
    void closeCache () const;

    void clearAll () const;
    void clearImages () const;
    void clearProfiles () const;
    void clearFromCache (const Glib::ustring& fname, bool purge) const;

    // Compute MD5 cache key for a file (based on path + size + creation time).
    // Results are cached internally; use precomputeMD5() to batch-populate.
    std::string getMD5 (const Glib::ustring& fname) const;

    // Batch-precompute MD5 hashes for a list of image files.  On Windows this
    // uses cached values and chooses between per-file attributes for small
    // batches and a single FindFirstFileW pass per directory for large batches.
    void precomputeMD5 (const std::vector<Glib::ustring>& files, std::size_t directoryScanThreshold = 512);
    void precomputeEntryMD5 (const std::vector<Glib::ustring>& files, std::size_t directoryScanThreshold = 64, bool precomputeCacheEntries = true);
    bool getKnownFilePresence (const Glib::ustring& fname, bool& present) const;
    bool isFileKnownMissing (const Glib::ustring& fname) const;
    bool isFileKnownPresent (const Glib::ustring& fname) const;
    void invalidateMD5 (const Glib::ustring& fname) const;
    void clearMD5Cache () const;

    Glib::ustring    getCacheFileName (const Glib::ustring& subDir,
                                       const Glib::ustring& fname,
                                       const Glib::ustring& fext,
                                       const Glib::ustring& md5) const;
    Glib::ustring    getCacheFileNameForBase (const Glib::ustring& subDir,
                                              const Glib::ustring& baseName,
                                              const Glib::ustring& fext) const;
};

#define cacheMgr CacheManager::getInstance()
