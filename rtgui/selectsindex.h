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

#include <atomic>
#include <map>
#include <utility>
#include <vector>

#include <glibmm/ustring.h>

#include "threadutils.h"

// Persistent index of every image that carries a flag, star rating, or color
// label — the durable backbone of global discovery. Folder scans are capped
// by the recents list and can only see folders steep re-visits; the index
// remembers every select forever. Kept current write-through from
// Thumbnail::updateCache, and reconciled (pruned/absorbed) by the global
// scans themselves.
class SelectsIndex final
{
public:
    struct Entry {
        int rating = 0;
        int pick = 0;
        int colorLabel = 0;

        bool operator==(const Entry& other) const
        {
            return rating == other.rating && pick == other.pick && colorLabel == other.colorLabel;
        }
    };

    static SelectsIndex& getInstance();

    SelectsIndex(const SelectsIndex&) = delete;
    SelectsIndex& operator=(const SelectsIndex&) = delete;

    // Record the current select state of a file. All-zero state removes the
    // file from the index. No-ops (and no disk churn) when unchanged.
    // Thread-safe; may be called from worker threads.
    void note(const Glib::ustring& path, int rating, int pick, int colorLabel);

    // Remove a file from the index (e.g. it no longer exists on disk).
    void forget(const Glib::ustring& path);

    // Copy of all current entries for scanning.
    std::vector<std::pair<Glib::ustring, Entry>> snapshot();

    // Write the index to disk now if dirty (atomic replace).
    void flushNow();

private:
    SelectsIndex();

    Glib::ustring indexPath() const;
    void load();
    void scheduleFlush();

    std::map<std::string, Entry> entries_;
    MyMutex mutex_;
    bool dirty_ = false;
    std::atomic<bool> flushScheduled_{false};
};
