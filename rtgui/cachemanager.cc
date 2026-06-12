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

#include <algorithm>
#include <iostream>
#include <limits>
#include <memory>
#include <unordered_set>
#include <utility>

#include <sys/stat.h>
#include <dirent.h>
#include <giomm.h>
#include <glib/gstdio.h>

#ifdef _WIN32
#include <fileapi.h>
#include <handleapi.h>
#endif

#include "cachemanager.h"

#include "guiutils.h"
#include "options.h"
#include "thumbnail.h"
#include "procparamchangers.h"

namespace
{

constexpr int cacheDirMode = 0777;
constexpr const char* cacheDirs[] = { "profiles", "images", "embprofiles", "data" };

std::string foldedNameKey(const Glib::ustring& name)
{
    return name.casefold().raw();
}

std::string foldedPathKey(const Glib::ustring& path)
{
    std::string key = path.casefold().raw();
    std::replace(key.begin(), key.end(), '\\', '/');

    return key;
}

#ifdef _WIN32
std::string md5FromWin32FileAttributes(
    const Glib::ustring& fname,
    DWORD fileSizeLow,
    DWORD creationTimeHigh,
    DWORD creationTimeLow)
{
    const auto identifier = Glib::ustring::compose(
        "%1-%2-%3-%4",
        fileSizeLow,
        creationTimeHigh,
        creationTimeLow,
        fname);

    return Glib::Checksum::compute_checksum(Glib::Checksum::CHECKSUM_MD5, identifier);
}
#endif

}

CacheManager* CacheManager::getInstance ()
{
    static CacheManager instance;
    return &instance;
}

void CacheManager::init ()
{
    MyMutex::MyLock lock (mutex);

    openEntries.clear ();
    baseDir = App::get().options().cacheBaseDir;
    profilesDir_ = Glib::build_filename(baseDir, "profiles");
    imagesDir_ = Glib::build_filename(baseDir, "images");
    embProfilesDir_ = Glib::build_filename(baseDir, "embprofiles");
    dataDir_ = Glib::build_filename(baseDir, "data");

    auto error = g_mkdir_with_parents (baseDir.c_str(), cacheDirMode);

    error |= g_mkdir_with_parents (profilesDir_.c_str(), cacheDirMode);
    error |= g_mkdir_with_parents (imagesDir_.c_str(), cacheDirMode);
    error |= g_mkdir_with_parents (embProfilesDir_.c_str(), cacheDirMode);
    error |= g_mkdir_with_parents (dataDir_.c_str(), cacheDirMode);

    if (error != 0 && rtengine::settings->verbose) {
        std::cerr << "Failed to create all cache directories: " << g_strerror(errno) << std::endl;
    }
}

Thumbnail* CacheManager::getEntry (const Glib::ustring& fname)
{
    std::unique_ptr<Thumbnail> thumbnail;
    const std::string fnameKey = fname.raw();

    // take manager lock and search for entry,
    // if found return it,
    // else release lock and create it
    {
        MyMutex::MyLock lock (mutex);

        // if it is open, return it
        const auto iterator = openEntries.find (fnameKey);

        if (iterator != openEntries.end ()) {

            auto cachedThumbnail = iterator->second;

            cachedThumbnail->increaseRef ();
            return cachedThumbnail;
        }
    }

    // build path name
    const auto md5 = getMD5 (fname);

    if (md5.empty ()) {
        return nullptr;
    }

    const auto cacheBaseName = Glib::path_get_basename(fname) + "." + md5;
    const auto cacheName = getCacheFileNameForBase("data", cacheBaseName, ".txt");
    const auto xmpSidecarMd5 =
        rtengine::settings->metadata_xmp_sync != rtengine::Settings::MetadataXmpSync::NONE
        ? getMD5(Thumbnail::xmpSidecarPath(fname))
        : "";

    const KnownFileState cacheState = getKnownFileState(cacheName);

    // let's see if we have it in the cache
    if (cacheState != KnownFileState::MISSING) {
        CacheImageData imageData;

        const auto error = imageData.load (cacheName, cacheState == KnownFileState::PRESENT);

        if (error == 0 && imageData.supported) {

            if (xmpSidecarMd5 != imageData.xmpSidecarMd5) {
                updateImageInfo(fname, imageData, xmpSidecarMd5);
                invalidateMD5(cacheName);
                imageData.save(cacheName);
                invalidateMD5(cacheName);
            }

            thumbnail.reset (new Thumbnail (this, fname, &imageData, cacheBaseName));

            if (!thumbnail->isSupported ()) {
                thumbnail.reset ();
            }
        }
    }

    // if not, create a new one
    if (!thumbnail) {

        thumbnail.reset (new Thumbnail (this, fname, md5, xmpSidecarMd5, cacheBaseName));

        if (!thumbnail->isSupported ()) {
            thumbnail.reset ();
        }
    }

    // retake the lock and see if it was added while we we're unlocked, if it
    // was use it over our version. if not added we create the cache entry
    if (thumbnail) {
        MyMutex::MyLock lock (mutex);

        const auto iterator = openEntries.find (fnameKey);

        if (iterator != openEntries.end ()) {

            auto cachedThumbnail = iterator->second;

            cachedThumbnail->increaseRef ();
            return cachedThumbnail;
        }

        // it wasn't, create a new entry
        openEntries.emplace (fnameKey, thumbnail.get ());
    }

    return thumbnail.release ();
}

void CacheManager::reserveEntries (std::size_t additionalEntries)
{
    if (additionalEntries == 0) {
        return;
    }

    MyMutex::MyLock lock (mutex);
    openEntries.reserve(openEntries.size() + additionalEntries);
}


void CacheManager::deleteEntry (const Glib::ustring& fname)
{
    const std::string fnameKey = fname.raw();

    MyMutex::MyLock lock (mutex);

    // check if it is opened
    auto iterator = openEntries.find (fnameKey);

    if (iterator == openEntries.end ()) {
        deleteFiles (fname, getMD5 (fname), true, true);
        return;
    }

    auto thumbnail = iterator->second;

    if (thumbnail->decreaseRefCacheMgr () == 0) {
        // If the ref count is not zero, it is open in the editor and the thumbnail can not be deleted.
        openEntries.erase (iterator);
        deleteFiles (fname, thumbnail->getMD5 (), true, true);
        delete thumbnail;
    }
}

void CacheManager::clearFromCache (const Glib::ustring& fname, bool purge) const
{
    MyMutex::MyLock lock (mutex);

    deleteFiles (fname, getMD5 (fname), true, purge);
}

void CacheManager::renameEntry (const std::string& oldfilename, const std::string& oldmd5, const std::string& newfilename)
{
    MyMutex::MyLock lock (mutex);

    const auto newmd5 = getMD5 (newfilename);

    const auto oldProfileName = getCacheFileName ("profiles", oldfilename, App::PARAM_FILE_EXTENSION, oldmd5);
    const auto newProfileName = getCacheFileName ("profiles", newfilename, App::PARAM_FILE_EXTENSION, newmd5);
    const auto oldDataName = getCacheFileName ("data", oldfilename, ".txt", oldmd5);
    const auto newDataName = getCacheFileName ("data", newfilename, ".txt", newmd5);
    const auto oldImageName = getCacheFileName ("images", oldfilename, ".rtti", oldmd5);
    const auto newImageName = getCacheFileName ("images", newfilename, ".rtti", newmd5);
    const auto oldEmbProfileName = getCacheFileName ("embprofiles", oldfilename, ".icc", oldmd5);
    const auto newEmbProfileName = getCacheFileName ("embprofiles", newfilename, ".icc", newmd5);

    auto error = g_rename (oldProfileName.c_str (), newProfileName.c_str ());
    error |= g_rename (oldImageName.c_str (), newImageName.c_str ());
    error |= g_rename (oldEmbProfileName.c_str (), newEmbProfileName.c_str ());
    error |= g_rename (oldDataName.c_str (), newDataName.c_str ());
    invalidateMD5(oldProfileName);
    invalidateMD5(newProfileName);
    invalidateMD5(oldDataName);
    invalidateMD5(newDataName);
    invalidateMD5(oldImageName);
    invalidateMD5(newImageName);
    invalidateMD5(oldEmbProfileName);
    invalidateMD5(newEmbProfileName);

    if (error != 0 && rtengine::settings->verbose) {
        std::cerr << "Failed to rename all files for cache entry '" << oldfilename << "': " << g_strerror(errno) << std::endl;
    }

    // check if it is opened
    // if it is open, update md5
    const auto iterator = openEntries.find (oldfilename);

    if (iterator == openEntries.end ()) {
        return;
    }

    auto thumbnail = iterator->second;
    openEntries.erase (iterator);
    openEntries.emplace (newfilename, thumbnail);

    thumbnail->setFileName (newfilename);
    thumbnail->updateCache ();
    thumbnail->saveThumbnail ();
}

void CacheManager::closeThumbnail (Thumbnail* thumbnail)
{
    const std::string fnameKey = thumbnail->getFileName().raw();

    {
        MyMutex::MyLock lock (mutex);
        openEntries.erase (fnameKey);
    }
    // Delete outside the lock — destructor may do work and we don't want
    // to block getEntry() calls from the preview loader thread pool.
    delete thumbnail;
}

void CacheManager::closeCache () const
{
    MyMutex::MyLock lock (mutex);

    applyCacheSizeLimitation ();
}

void CacheManager::clearAll () const
{
    MyMutex::MyLock lock (mutex);

    for (const auto& cacheDir : cacheDirs) {
        deleteDir (cacheDir);
    }
    clearMD5Cache();
}

void CacheManager::clearImages () const
{
    MyMutex::MyLock lock (mutex);

    deleteDir ("data");
    deleteDir ("images");
    deleteDir ("embprofiles");
    clearMD5Cache();
}

void CacheManager::clearProfiles () const
{
    MyMutex::MyLock lock (mutex);

    deleteDir ("profiles");
    clearMD5Cache();

}


void CacheManager::deleteDir (const Glib::ustring& dirName) const
{
    try {

        Glib::Dir dir (Glib::build_filename (baseDir, dirName));

        auto error = 0;

        for (auto entry = dir.begin (); entry != dir.end (); ++entry) {
            error |= g_remove (Glib::build_filename (baseDir, dirName, *entry).c_str ());
        }

        if (error != 0 && rtengine::settings->verbose) {
            std::cerr << "Failed to delete all entries in cache directory '" << dirName << "': " << g_strerror(errno) << std::endl;
        }

    } catch (Glib::Error&) {}
}

void CacheManager::deleteFiles (const Glib::ustring& fname, const std::string& md5, bool purgeData, bool purgeProfile) const
{
    if (md5.empty ()) {
        return;
    }

    const auto imageName = getCacheFileName ("images", fname, ".rtti", md5);
    const auto embProfileName = getCacheFileName ("embprofiles", fname, ".icc", md5);
    auto error = g_remove (imageName.c_str ());
    error |= g_remove (embProfileName.c_str ());
    invalidateMD5(imageName);
    invalidateMD5(embProfileName);

    if (purgeData) {
        const auto dataName = getCacheFileName ("data", fname, ".txt", md5);
        error |= g_remove (dataName.c_str ());
        invalidateMD5(dataName);
    }

    if (purgeProfile) {
        const auto profileName = getCacheFileName ("profiles", fname, App::PARAM_FILE_EXTENSION, md5);
        error |= g_remove (profileName.c_str ());
        invalidateMD5(profileName);
    }

    if (error != 0 && rtengine::settings->verbose) {
        std::cerr << "Failed to delete all files for cache entry '" << fname << "': " << g_strerror(errno) << std::endl;
    }
}

std::string CacheManager::getMD5 (const Glib::ustring& fname) const
{
    const std::string fileKey = fname.raw();

    // Check precomputed cache first (populated by precomputeMD5).
    {
        std::lock_guard<std::mutex> lock(md5CacheMutex_);
        const auto it = md5Cache_.find(fileKey);
        if (it != md5Cache_.end()) {
            return it->second;
        }
    }

    std::string result;

#ifdef _WIN32

    std::unique_ptr<wchar_t, GFreeFunc> wfname(reinterpret_cast<wchar_t*>(g_utf8_to_utf16 (fname.c_str (), -1, NULL, NULL, NULL)), g_free);

    WIN32_FILE_ATTRIBUTE_DATA fileAttr;
    if (wfname && GetFileAttributesExW(wfname.get(), GetFileExInfoStandard, &fileAttr)) {
        // We use name, size and creation time to identify a file.
        result = md5FromWin32FileAttributes(
            fname,
            fileAttr.nFileSizeLow,
            fileAttr.ftCreationTime.dwHighDateTime,
            fileAttr.ftCreationTime.dwLowDateTime);
    }

#else

    struct stat st;
    if (::stat(fname.c_str(), &st) == 0) {
        const auto identifier = Glib::ustring::compose("%1%2", fname, static_cast<gint64>(st.st_size));
        result = Glib::Checksum::compute_checksum(Glib::Checksum::CHECKSUM_MD5, identifier);
    }

#endif

    // Cache the result for future lookups.
    if (!result.empty()) {
        std::lock_guard<std::mutex> lock(md5CacheMutex_);
        md5Cache_.emplace(fileKey, result);
    }

    return result;
}

void CacheManager::precomputeMD5 (const std::vector<Glib::ustring>& files, std::size_t directoryScanThreshold)
{
    if (files.empty()) {
        return;
    }

#ifdef _WIN32
    struct MD5Probe {
        const Glib::ustring* filename;
        std::string fileKey;
    };

    std::vector<MD5Probe> uniqueFiles;
    uniqueFiles.reserve(files.size());
    std::unordered_set<std::string> queuedKeys;
    queuedKeys.reserve(files.size());
    for (const auto& f : files) {
        std::string fileKey = f.raw();
        if (queuedKeys.emplace(fileKey).second) {
            uniqueFiles.push_back({&f, std::move(fileKey)});
        }
    }

    std::vector<const MD5Probe*> missingFiles;
    missingFiles.reserve(uniqueFiles.size());

    {
        std::lock_guard<std::mutex> lock(md5CacheMutex_);
        md5Cache_.reserve(md5Cache_.size() + uniqueFiles.size());
        for (const auto& f : uniqueFiles) {
            if (md5Cache_.find(f.fileKey) == md5Cache_.end()) {
                missingFiles.push_back(&f);
            }
        }
    }

    if (missingFiles.empty()) {
        return;
    }

    // Group files by parent directory so we can do one FindFirstFileW
    // pass per directory instead of N individual GetFileAttributesExW calls.
    std::unordered_map<std::string, std::vector<const MD5Probe*>> byDir;
    byDir.reserve(missingFiles.size());
    for (const auto* f : missingFiles) {
        byDir[Glib::path_get_dirname(*f->filename)].push_back(f);
    }

    std::unordered_map<std::string, std::string> results;
    results.reserve(missingFiles.size());
    std::vector<std::string> fullyScannedDirs;
    std::vector<std::string> fullyScannedFiles;
    for (const auto& kv : byDir) {
        const std::string& dirPath = kv.first;
        const auto& dirFiles = kv.second;

        if (dirFiles.size() < directoryScanThreshold) {
            for (const auto* fp : dirFiles) {
                std::unique_ptr<wchar_t, GFreeFunc> wfile(
                    reinterpret_cast<wchar_t*>(g_utf8_to_utf16(fp->filename->c_str(), -1, NULL, NULL, NULL)), g_free);

                WIN32_FILE_ATTRIBUTE_DATA fileAttr;
                if (wfile && GetFileAttributesExW(wfile.get(), GetFileExInfoStandard, &fileAttr)) {
                    results.emplace(
                        fp->fileKey,
                        md5FromWin32FileAttributes(
                            *fp->filename,
                            fileAttr.nFileSizeLow,
                            fileAttr.ftCreationTime.dwHighDateTime,
                            fileAttr.ftCreationTime.dwLowDateTime));
                }
            }
            continue;
        }

        // Single directory enumeration pass. Once a batch is large enough to
        // justify scanning the directory, cache every regular file we see so
        // later streaming batches from the same folder are instant hits.
        const std::string pattern = dirPath + "\\*";
        std::unique_ptr<wchar_t, GFreeFunc> wpattern(
            reinterpret_cast<wchar_t*>(g_utf8_to_utf16(pattern.c_str(), -1, NULL, NULL, NULL)), g_free);
        if (!wpattern) {
            continue;
        }

        WIN32_FIND_DATAW fd;
        HANDLE hFind = FindFirstFileW(wpattern.get(), &fd);
        if (hFind == INVALID_HANDLE_VALUE) {
            continue;
        }

        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                continue;
            }

            std::unique_ptr<char, GFreeFunc> utf8Name(
                reinterpret_cast<char*>(g_utf16_to_utf8(reinterpret_cast<const gunichar2*>(fd.cFileName), -1, NULL, NULL, NULL)), g_free);
            if (!utf8Name) {
                continue;
            }

            const Glib::ustring fullPath = Glib::build_filename(dirPath, utf8Name.get());
            const auto identifier = Glib::ustring::compose(
                "%1-%2-%3-%4",
                fd.nFileSizeLow,
                fd.ftCreationTime.dwHighDateTime,
                fd.ftCreationTime.dwLowDateTime,
                fullPath);
            results.emplace(
                fullPath.raw(),
                Glib::Checksum::compute_checksum(Glib::Checksum::CHECKSUM_MD5, identifier));
            fullyScannedFiles.push_back(foldedPathKey(fullPath));
        } while (FindNextFileW(hFind, &fd));

        FindClose(hFind);
        fullyScannedDirs.push_back(foldedPathKey(dirPath));
    }

    // Bulk-insert into cache under a single lock acquisition.
    {
        std::lock_guard<std::mutex> lock(md5CacheMutex_);
        md5Cache_.reserve(md5Cache_.size() + results.size());
        for (const auto& result : results) {
            md5Cache_[result.first] = result.second;
        }
        fullyScannedMD5Dirs_.reserve(fullyScannedMD5Dirs_.size() + fullyScannedDirs.size());
        fullyScannedMD5Dirs_.insert(fullyScannedDirs.begin(), fullyScannedDirs.end());
        fullyScannedMD5Files_.reserve(fullyScannedMD5Files_.size() + fullyScannedFiles.size());
        fullyScannedMD5Files_.insert(fullyScannedFiles.begin(), fullyScannedFiles.end());
    }

#else
    // On Linux, stat() is already fast.  Just populate the cache
    // so preview-loader threads get instant hits.
    for (const auto& f : files) {
        getMD5(f);
    }
#endif
}

void CacheManager::precomputeFileStates (
    const std::vector<Glib::ustring>& files,
    std::size_t directoryScanThreshold,
    bool hashPresentFiles)
{
    if (files.empty()) {
        return;
    }

    struct FileStateProbe {
        Glib::ustring filename;
        std::string fileKey;
    };

    std::vector<FileStateProbe> uniqueFiles;
    uniqueFiles.reserve(files.size());
    std::unordered_set<std::string> queuedKeys;
    queuedKeys.reserve(files.size());
    for (const auto& f : files) {
        std::string fileKey = f.raw();
        if (queuedKeys.emplace(fileKey).second) {
            uniqueFiles.push_back({f, std::move(fileKey)});
        }
    }

    std::vector<const FileStateProbe*> filesToCheck;
    filesToCheck.reserve(uniqueFiles.size());

    {
        std::lock_guard<std::mutex> lock(md5CacheMutex_);
        md5Cache_.reserve(md5Cache_.size() + uniqueFiles.size());
        for (const auto& f : uniqueFiles) {
            if (md5Cache_.find(f.fileKey) != md5Cache_.end()) {
                continue;
            }
            if (!hashPresentFiles && knownPresentFiles_.find(f.fileKey) != knownPresentFiles_.end()) {
                continue;
            }

            const std::string dirKey = foldedPathKey(Glib::path_get_dirname(f.filename));
            if (fullyScannedMD5Dirs_.find(dirKey) != fullyScannedMD5Dirs_.end()) {
                const std::string pathKey = foldedPathKey(f.filename);
                if (fullyScannedMD5Files_.find(pathKey) == fullyScannedMD5Files_.end()) {
                    md5Cache_.emplace(f.fileKey, std::string());
                    knownPresentFiles_.erase(f.fileKey);
                    continue;
                }
                if (!hashPresentFiles) {
                    knownPresentFiles_.emplace(f.fileKey);
                    continue;
                }
            }

            filesToCheck.push_back(&f);
        }
    }

    if (filesToCheck.empty()) {
        return;
    }

    std::unordered_map<std::string, std::string> results;
    results.reserve(filesToCheck.size());
    std::unordered_set<std::string> knownPresentResults;
    if (!hashPresentFiles) {
        knownPresentResults.reserve(filesToCheck.size());
    }

#ifdef _WIN32
    std::unordered_map<std::string, std::vector<const FileStateProbe*>> byDir;
    byDir.reserve(filesToCheck.size());
    std::vector<std::string> fullyScannedDirs;
    std::vector<std::string> fullyScannedFiles;

    for (const auto* f : filesToCheck) {
        byDir[Glib::path_get_dirname(f->filename)].push_back(f);
    }

    struct WinFileIdentity {
        DWORD fileSizeLow;
        DWORD creationTimeHigh;
        DWORD creationTimeLow;
    };

    for (const auto& kv : byDir) {
        const std::string& dirPath = kv.first;
        const auto& dirFiles = kv.second;

        if (dirFiles.size() < directoryScanThreshold) {
            for (const auto* f : dirFiles) {
                std::unique_ptr<wchar_t, GFreeFunc> wfile(
                    reinterpret_cast<wchar_t*>(g_utf8_to_utf16(f->filename.c_str(), -1, NULL, NULL, NULL)), g_free);

                WIN32_FILE_ATTRIBUTE_DATA fileAttr;
                if (wfile && GetFileAttributesExW(wfile.get(), GetFileExInfoStandard, &fileAttr)) {
                    if (hashPresentFiles) {
                        results.emplace(
                            f->fileKey,
                            md5FromWin32FileAttributes(
                                f->filename,
                                fileAttr.nFileSizeLow,
                                fileAttr.ftCreationTime.dwHighDateTime,
                                fileAttr.ftCreationTime.dwLowDateTime));
                    } else {
                        knownPresentResults.emplace(f->fileKey);
                    }
                } else {
                    results.emplace(f->fileKey, std::string());
                }
            }
            continue;
        }

        std::unordered_map<std::string, WinFileIdentity> dirEntries;
        dirEntries.reserve(dirFiles.size() * 2);
        std::unordered_set<std::string> requestedNames;
        requestedNames.reserve(dirFiles.size() * 2);
        std::vector<std::pair<const FileStateProbe*, std::string>> requestedFiles;
        requestedFiles.reserve(dirFiles.size());
        for (const auto* f : dirFiles) {
            auto requestedName = foldedNameKey(Glib::path_get_basename(f->filename));
            requestedNames.insert(requestedName);
            requestedFiles.emplace_back(f, std::move(requestedName));
        }

        const std::string pattern = dirPath + "\\*";
        std::unique_ptr<wchar_t, GFreeFunc> wpattern(
            reinterpret_cast<wchar_t*>(g_utf8_to_utf16(pattern.c_str(), -1, NULL, NULL, NULL)), g_free);
        if (!wpattern) {
            for (const auto* f : dirFiles) {
                results.emplace(f->fileKey, std::string());
            }
            continue;
        }

        WIN32_FIND_DATAW fd;
        HANDLE hFind = FindFirstFileW(wpattern.get(), &fd);
        if (hFind == INVALID_HANDLE_VALUE) {
            for (const auto* f : dirFiles) {
                results.emplace(f->fileKey, std::string());
            }
            continue;
        }

        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                continue;
            }

            std::unique_ptr<char, GFreeFunc> utf8Name(
                reinterpret_cast<char*>(g_utf16_to_utf8(reinterpret_cast<const gunichar2*>(fd.cFileName), -1, NULL, NULL, NULL)), g_free);
            if (!utf8Name) {
                continue;
            }

            const Glib::ustring fullPath = Glib::build_filename(dirPath, utf8Name.get());
            const std::string foldedName = foldedNameKey(utf8Name.get());
            if (requestedNames.find(foldedName) != requestedNames.end()) {
                dirEntries.emplace(
                    std::move(foldedName),
                    WinFileIdentity{
                        fd.nFileSizeLow,
                        fd.ftCreationTime.dwHighDateTime,
                        fd.ftCreationTime.dwLowDateTime
                    });
            }
            fullyScannedFiles.push_back(foldedPathKey(fullPath));
        } while (FindNextFileW(hFind, &fd));

        FindClose(hFind);
        fullyScannedDirs.push_back(foldedPathKey(dirPath));

        for (const auto& requestedFile : requestedFiles) {
            const auto* f = requestedFile.first;
            const auto entry = dirEntries.find(requestedFile.second);
            if (entry != dirEntries.end()) {
                if (hashPresentFiles) {
                    results.emplace(
                        f->fileKey,
                        md5FromWin32FileAttributes(
                            f->filename,
                            entry->second.fileSizeLow,
                            entry->second.creationTimeHigh,
                            entry->second.creationTimeLow));
                } else {
                    knownPresentResults.emplace(f->fileKey);
                }
            } else {
                results.emplace(f->fileKey, std::string());
            }
        }
    }

#else
    for (const auto* f : filesToCheck) {
        struct stat st;
        if (::stat(f->filename.c_str(), &st) == 0) {
            if (hashPresentFiles) {
                const auto identifier = Glib::ustring::compose("%1%2", f->filename, static_cast<gint64>(st.st_size));
                results.emplace(
                    f->fileKey,
                    Glib::Checksum::compute_checksum(Glib::Checksum::CHECKSUM_MD5, identifier));
            } else {
                knownPresentResults.emplace(f->fileKey);
            }
        } else {
            results.emplace(f->fileKey, std::string());
        }
    }
#endif

    {
        std::lock_guard<std::mutex> lock(md5CacheMutex_);
        md5Cache_.reserve(md5Cache_.size() + results.size());
        for (const auto& result : results) {
            md5Cache_[result.first] = result.second;
            knownPresentFiles_.erase(result.first);
        }
        knownPresentFiles_.reserve(knownPresentFiles_.size() + knownPresentResults.size());
        knownPresentFiles_.insert(knownPresentResults.begin(), knownPresentResults.end());
#ifdef _WIN32
        fullyScannedMD5Dirs_.reserve(fullyScannedMD5Dirs_.size() + fullyScannedDirs.size());
        fullyScannedMD5Dirs_.insert(fullyScannedDirs.begin(), fullyScannedDirs.end());
        fullyScannedMD5Files_.reserve(fullyScannedMD5Files_.size() + fullyScannedFiles.size());
        fullyScannedMD5Files_.insert(fullyScannedFiles.begin(), fullyScannedFiles.end());
#endif
    }
}

void CacheManager::precomputeEntryMD5 (
    const std::vector<Glib::ustring>& files,
    std::size_t directoryScanThreshold,
    bool precomputeCacheEntries)
{
    if (files.empty()) {
        return;
    }

    struct EntryMD5Probe {
        const Glib::ustring* filename;
        std::string fileKey;
    };

    std::vector<EntryMD5Probe> probes;
    probes.reserve(files.size());
    for (const auto& f : files) {
        probes.push_back({&f, f.raw()});
    }

    precomputeMD5(files, directoryScanThreshold);

    const bool checkXmpSidecars =
        rtengine::settings->metadata_xmp_sync != rtengine::Settings::MetadataXmpSync::NONE;

    std::vector<Glib::ustring> sourceAuxiliaryFiles;
    sourceAuxiliaryFiles.reserve(files.size() * (checkXmpSidecars ? 2 : 1));

    for (const auto& probe : probes) {
        const auto& f = *probe.filename;
        if (checkXmpSidecars) {
            sourceAuxiliaryFiles.push_back(Thumbnail::xmpSidecarPath(f));
        }

        sourceAuxiliaryFiles.push_back(f + App::PARAM_FILE_EXTENSION);
    }

    // Cached bitmap and embedded-profile probes are intentionally left lazy:
    // folder population no longer reads them until visible thumbnails need pixels.
    precomputeFileStates(sourceAuxiliaryFiles, directoryScanThreshold, true);

    if (!precomputeCacheEntries) {
        return;
    }

    std::vector<std::pair<const EntryMD5Probe*, std::string>> imageMD5s;
    imageMD5s.reserve(probes.size());
    {
        std::lock_guard<std::mutex> lock(md5CacheMutex_);
        for (const auto& probe : probes) {
            const auto md5 = md5Cache_.find(probe.fileKey);
            if (md5 != md5Cache_.end() && !md5->second.empty()) {
                imageMD5s.emplace_back(&probe, md5->second);
            }
        }
    }

    std::vector<Glib::ustring> cachePresenceFiles;
    cachePresenceFiles.reserve(imageMD5s.size() * 2);
    for (const auto& imageMD5 : imageMD5s) {
        const auto cacheBaseName = Glib::path_get_basename(*imageMD5.first->filename) + "." + imageMD5.second;
        cachePresenceFiles.push_back(Glib::build_filename(dataDir_, cacheBaseName + ".txt"));
        cachePresenceFiles.push_back(Glib::build_filename(profilesDir_, cacheBaseName + App::PARAM_FILE_EXTENSION));
    }

    // The cache data/profiles directories are global and can contain many
    // thousands of entries from unrelated folders. For large image batches,
    // scanning those whole directories is often slower than direct probes and
    // makes folder load time grow with total cache size instead of current
    // folder size.
    precomputeFileStates(cachePresenceFiles, std::numeric_limits<std::size_t>::max(), false);
}

CacheManager::KnownFileState CacheManager::getKnownFileState (const Glib::ustring& fname) const
{
    std::lock_guard<std::mutex> lock(md5CacheMutex_);

    const std::string fileKey = fname.raw();
    const auto cached = md5Cache_.find(fileKey);
    if (cached != md5Cache_.end()) {
        return cached->second.empty() ? KnownFileState::MISSING : KnownFileState::PRESENT;
    }

    if (knownPresentFiles_.find(fileKey) != knownPresentFiles_.end()) {
        return KnownFileState::PRESENT;
    }

    const std::string dirKey = foldedPathKey(Glib::path_get_dirname(fname));
    if (fullyScannedMD5Dirs_.find(dirKey) != fullyScannedMD5Dirs_.end()) {
        const std::string pathKey = foldedPathKey(fname);
        return fullyScannedMD5Files_.find(pathKey) != fullyScannedMD5Files_.end()
            ? KnownFileState::PRESENT
            : KnownFileState::MISSING;
    }

    return KnownFileState::UNKNOWN;
}

bool CacheManager::isFileKnownMissing (const Glib::ustring& fname) const
{
    return getKnownFileState(fname) == KnownFileState::MISSING;
}

bool CacheManager::getKnownFilePresence (const Glib::ustring& fname, bool& present) const
{
    const KnownFileState state = getKnownFileState(fname);
    if (state == KnownFileState::UNKNOWN) {
        return false;
    }

    present = state == KnownFileState::PRESENT;
    return true;
}

bool CacheManager::isFileKnownPresent (const Glib::ustring& fname) const
{
    return getKnownFileState(fname) == KnownFileState::PRESENT;
}

void CacheManager::invalidateMD5 (const Glib::ustring& fname) const
{
    std::lock_guard<std::mutex> lock(md5CacheMutex_);

    const std::string fileKey = fname.raw();
    const std::string dirKey = foldedPathKey(Glib::path_get_dirname(fname));
    const std::string pathKey = foldedPathKey(fname);

    md5Cache_.erase(fileKey);
    knownPresentFiles_.erase(fileKey);
    fullyScannedMD5Dirs_.erase(dirKey);
    fullyScannedMD5Files_.erase(pathKey);
}

void CacheManager::clearMD5Cache () const
{
    std::lock_guard<std::mutex> lock(md5CacheMutex_);
    md5Cache_.clear();
    knownPresentFiles_.clear();
    fullyScannedMD5Dirs_.clear();
    fullyScannedMD5Files_.clear();
}

Glib::ustring CacheManager::getCacheFileName (const Glib::ustring& subDir,
        const Glib::ustring& fname,
        const Glib::ustring& fext,
        const Glib::ustring& md5) const
{
    return getCacheFileNameForBase(subDir, Glib::path_get_basename(fname) + "." + md5, fext);
}

Glib::ustring CacheManager::getCacheFileNameForBase (const Glib::ustring& subDir,
        const Glib::ustring& baseName,
        const Glib::ustring& fext) const
{
    if (subDir == "data") {
        return Glib::build_filename (dataDir_, baseName + fext);
    }

    if (subDir == "profiles") {
        return Glib::build_filename (profilesDir_, baseName + fext);
    }

    if (subDir == "images") {
        return Glib::build_filename (imagesDir_, baseName + fext);
    }

    if (subDir == "embprofiles") {
        return Glib::build_filename (embProfilesDir_, baseName + fext);
    }

    const auto dirName = Glib::build_filename (baseDir, subDir);
    return Glib::build_filename (dirName, baseName + fext);
}

void CacheManager::applyCacheSizeLimitation () const
{
    // first count files without fetching file name and timestamp.
    auto cachedir = opendir(Glib::build_filename(baseDir, "data").c_str());
    if (!cachedir) {
        return;
    }

    std::size_t numFiles = 0;
    while (readdir(cachedir)) {
        ++numFiles;
    }

    closedir(cachedir);
    if (numFiles > 2) {
        numFiles -= 2; // because . and .. are counted
    }

    const auto& options = App::get().options();
    if (numFiles <= options.maxCacheEntries) {
        return;
    }

    using FNameMTime = std::pair<Glib::ustring, Glib::TimeVal>;

    std::vector<FNameMTime> files;
    files.reserve(numFiles);

    constexpr std::size_t md5_size = 32;
    // get filenames and timestamps
    try {
        const auto dir = Gio::File::create_for_path(Glib::build_filename(baseDir, "data"));
        const auto enumerator = dir->enumerate_children("standard::name,time::modified");

        while (const auto file = enumerator->next_file()) {
            const auto name = file->get_name();
            if (name.size() >= md5_size + 5) {
                files.emplace_back(name, file->modification_time());
            }
        }

    } catch (Glib::Exception&) {}

    if (files.size() <= options.maxCacheEntries) {
        // limit not reached
        return;
    }

    const std::size_t toDelete = files.size() - options.maxCacheEntries + options.maxCacheEntries * 5 / 100; // reserve 5% free cache space

    std::nth_element(
        files.begin(),
        files.begin() + toDelete,
        files.end(),
        [](const FNameMTime& lhs, const FNameMTime& rhs) -> bool
        {
            return lhs.second < rhs.second;
        }
    );

    for (std::vector<FNameMTime>::const_iterator entry = files.begin(), end = files.begin() + toDelete; entry != end; ++entry) {
        const auto& name = entry->first;
        const auto name_size = name.size() - md5_size;
        const auto fname = name.substr(0, name_size - 5);
        const auto md5 = name.substr(name_size - 4, md5_size);

        deleteFiles(fname, md5, true, false);
    }
}

void CacheManager::updateImageInfo(const Glib::ustring &fname, CacheImageData &imageData, const Glib::ustring &xmpSidecarMd5) const
{
    Thumbnail::infoFromImage(fname, imageData);
    imageData.xmpSidecarMd5 = xmpSidecarMd5;
}

