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
#include "selectsindex.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#include <glib.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>

#include "options.h"

namespace
{

constexpr int FLUSH_DELAY_MS = 2500;

} // namespace

SelectsIndex& SelectsIndex::getInstance()
{
    static SelectsIndex instance;
    return instance;
}

SelectsIndex::SelectsIndex()
{
    load();
    loadStamps();
}

namespace
{
constexpr const char* kStampsVersion = "steep-dirstamps 1";

std::string foldDir(const Glib::ustring& dir)
{
    std::string s = dir.casefold().raw();
    while (s.size() > 3 && (s.back() == '\\' || s.back() == '/')) {
        s.pop_back();
    }
    return s;
}
}

Glib::ustring SelectsIndex::stampsPath() const
{
    return Glib::build_filename(App::get().options().cacheBaseDir, "selects.dirs");
}

void SelectsIndex::loadStamps()
{
    gchar* raw = nullptr;
    gsize length = 0;
    if (!g_file_get_contents(stampsPath().c_str(), &raw, &length, nullptr)) {
        return;
    }
    const std::string text(raw, length);
    g_free(raw);

    // "sig<TAB>path" per line under a version line; another version: start over.
    size_t pos = text.find('\n');
    if (pos == std::string::npos || text.compare(0, pos, kStampsVersion) != 0) {
        return;
    }
    MyMutex::MyLock lock(mutex_);
    while (++pos < text.size()) {
        const size_t end = text.find('\n', pos);
        const std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        const size_t tab = line.find('\t');
        if (tab != std::string::npos) {
            dirStamps_[line.substr(tab + 1)] = std::strtoull(line.c_str(), nullptr, 16);
        }
        if (end == std::string::npos) {
            break;
        }
        pos = end;
    }
}

std::uint64_t SelectsIndex::dirStamp(const Glib::ustring& dir)
{
    MyMutex::MyLock lock(mutex_);
    const auto it = dirStamps_.find(foldDir(dir));
    return it == dirStamps_.end() ? 0 : it->second;
}

void SelectsIndex::setDirStamp(const Glib::ustring& dir, std::uint64_t signature)
{
    bool changed = false;
    {
        MyMutex::MyLock lock(mutex_);
        auto& slot = dirStamps_[foldDir(dir)];
        if (slot != signature) {
            slot = signature;
            stampsDirty_ = true;
            changed = true;
        }
    }
    if (changed) {
        scheduleFlush();
    }
}

Glib::ustring SelectsIndex::indexPath() const
{
    return Glib::build_filename(App::get().options().cacheBaseDir, "selects.idx");
}

void SelectsIndex::load()
{
    gchar* raw = nullptr;
    gsize length = 0;

    if (!g_file_get_contents(indexPath().c_str(), &raw, &length, nullptr)) {
        return;
    }

    MyMutex::MyLock lock(mutex_);

    // One entry per line: rating '\t' pick '\t' colorLabel '\t' path
    const char* pos = raw;
    const char* const end = raw + length;

    while (pos < end) {
        const char* lineEnd = pos;

        while (lineEnd < end && *lineEnd != '\n') {
            ++lineEnd;
        }

        char* next = nullptr;
        Entry entry;
        entry.rating = static_cast<int>(strtol(pos, &next, 10));

        if (next && next < lineEnd && *next == '\t') {
            entry.pick = static_cast<int>(strtol(next + 1, &next, 10));

            if (next && next < lineEnd && *next == '\t') {
                entry.colorLabel = static_cast<int>(strtol(next + 1, &next, 10));

                if (next && next < lineEnd && *next == '\t') {
                    const char* pathStart = next + 1;
                    std::string path(pathStart, static_cast<size_t>(lineEnd - pathStart));

                    if (!path.empty()) {
                        entries_[path] = entry;
                    }
                }
            }
        }

        pos = lineEnd < end ? lineEnd + 1 : end;
    }

    g_free(raw);
}

void SelectsIndex::note(const Glib::ustring& path, int rating, int pick, int colorLabel)
{
    if (path.empty()) {
        return;
    }

    Entry entry;
    entry.rating = std::max(0, rating);
    entry.pick = pick;
    entry.colorLabel = std::max(0, colorLabel);

    const bool isSelect = entry.rating > 0 || entry.pick != 0 || entry.colorLabel > 0;
    bool changed = false;

    {
        MyMutex::MyLock lock(mutex_);
        const std::string key = path.raw();
        const auto it = entries_.find(key);

        if (isSelect) {
            if (it == entries_.end() || !(it->second == entry)) {
                entries_[key] = entry;
                changed = true;
            }
        } else if (it != entries_.end()) {
            entries_.erase(it);
            changed = true;
        }

        if (changed) {
            dirty_ = true;
        }
    }

    if (changed) {
        scheduleFlush();
    }
}

void SelectsIndex::forget(const Glib::ustring& path)
{
    bool changed = false;

    {
        MyMutex::MyLock lock(mutex_);
        changed = entries_.erase(path.raw()) > 0;

        if (changed) {
            dirty_ = true;
        }
    }

    if (changed) {
        scheduleFlush();
    }
}

std::vector<std::pair<Glib::ustring, SelectsIndex::Entry>> SelectsIndex::snapshot()
{
    MyMutex::MyLock lock(mutex_);
    std::vector<std::pair<Glib::ustring, Entry>> result;
    result.reserve(entries_.size());

    for (const auto& item : entries_) {
        result.emplace_back(Glib::ustring(item.first), item.second);
    }

    return result;
}

void SelectsIndex::flushNow()
{
    std::string data;
    std::string stamps;
    bool writeEntries = false;

    {
        MyMutex::MyLock lock(mutex_);

        if (stampsDirty_) {
            stamps = std::string(kStampsVersion) + "\n";
            char hex[24];
            for (const auto& item : dirStamps_) {
                std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(item.second));
                stamps += hex;
                stamps += '\t';
                stamps += item.first;
                stamps += '\n';
            }
            stampsDirty_ = false;
        }

        writeEntries = dirty_;
        if (!dirty_) {
            goto writeStamps;
        }

        data.reserve(entries_.size() * 96);

        for (const auto& item : entries_) {
            data += std::to_string(item.second.rating);
            data += '\t';
            data += std::to_string(item.second.pick);
            data += '\t';
            data += std::to_string(item.second.colorLabel);
            data += '\t';
            data += item.first;
            data += '\n';
        }

        dirty_ = false;
    }

writeStamps:
    // g_file_set_contents writes to a temp file and renames — atomic replace.
    if (writeEntries) {
        g_file_set_contents(indexPath().c_str(), data.c_str(), static_cast<gssize>(data.size()), nullptr);
    }
    if (!stamps.empty()) {
        g_file_set_contents(stampsPath().c_str(), stamps.c_str(), static_cast<gssize>(stamps.size()), nullptr);
    }
}

void SelectsIndex::scheduleFlush()
{
    bool expected = false;

    if (!flushScheduled_.compare_exchange_strong(expected, true)) {
        return;
    }

    // Hop to the GUI loop (thread-safe), then debounce the disk write.
    Glib::signal_idle().connect_once([this]() {
        Glib::signal_timeout().connect_once([this]() {
            flushScheduled_ = false;
            flushNow();
        }, FLUSH_DELAY_MS);
    });
}
