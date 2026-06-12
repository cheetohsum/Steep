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

#include <string>
#include <utility>
#include <vector>

#include "rtengine/noncopyable.h"

namespace Glib
{

class ustring;

}

class FileBrowserEntry;

class PreviewLoaderListener
{
public:
    using PreviewReadyBatch = std::vector<std::pair<int, FileBrowserEntry*>>;

    virtual ~PreviewLoaderListener() = default;

    /**
     * @brief a preview is ready
     *
     * @param dir_id directory ID this is for
     * @param fd entry
     */
    virtual void previewReady(int dir_id, FileBrowserEntry* fd) = 0;
    virtual void previewReadyBatch(PreviewReadyBatch&& entries)
    {
        for (auto& entry : entries) {
            previewReady(entry.first, entry.second);
        }
    }

    /**
     * @brief all previews have finished loading
     */
    virtual void previewsFinished(int dir_id_) = 0;
};

class PreviewLoader :
    public rtengine::NonCopyable
{
public:
    /**
     * @brief Singleton entry point.
     *
     * @note expects to be called inside gtk thread lock
     *
     * @return Pointer to thumbnail image updater.
     */
    static PreviewLoader* getInstance(void);

    /**
     * @brief Add an thumbnail image update request.
     *
     * Code will add the request to the queue and, if needed, start a pool
     * thread to process it.
     *
     * @param dir_id directory we're looking at
     * @param dir_entry entry in it
     * @param l listener
     */
    void add(int dir_id, const Glib::ustring& dir_entry, PreviewLoaderListener* l);
    void add(int dir_id, const Glib::ustring& dir_entry, std::string&& dir_entry_key, PreviewLoaderListener* l);
    void addBatch(int dir_id, std::vector<Glib::ustring>&& dir_entries, PreviewLoaderListener* l);
    void addBatch(int dir_id, std::vector<Glib::ustring>&& dir_entries, std::vector<std::string>&& dir_entry_keys, PreviewLoaderListener* l);

    /**
     * @brief Stop processing and remove all jobs.
     *
     * Will not return till all jobs have completed.
     *
     * @note expects to be called inside gtk thread lock
     */
    void removeAllJobs(void);

    /**
     * @brief Set a priority hint so jobs near the target file are loaded first.
     */
    void setPriorityHint(const Glib::ustring& targetFile);
    void setPriorityHint(const Glib::ustring& targetFile, std::string&& targetFileKey);

    /** Pause processing — queued jobs stay but no new work starts. */
    void pause();
    /** Resume processing — re-schedules all pending jobs. */
    void resume();
    void setPostScanDrainMode(bool enabled);
    bool hasPendingWork() const;
    void wakePendingWorkers();

private:

    PreviewLoader();
    ~PreviewLoader();

    class Impl;
    Impl* impl_;
};

/**
 * @brief Singleton boiler plate.
 *
 * To use: \c previewLoader->start() ,
 */
#define previewLoader PreviewLoader::getInstance()
