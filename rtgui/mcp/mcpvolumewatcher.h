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

#include <giomm.h>
#include <glibmm.h>

#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace mcp {

struct VolumeInfo {
    std::string name;
    std::string path;   // empty if not mounted
    std::string type;   // "removable", "fixed", or "unknown"
    bool mounted;
};

struct MountEvent {
    std::string event;      // "mount_added" or "mount_removed"
    std::string name;
    std::string path;
    std::string timestamp;  // ISO 8601
};

class VolumeWatcher {
public:
    VolumeWatcher();
    ~VolumeWatcher();

    void start();
    void stop();

    std::vector<VolumeInfo> getVolumes() const;
    std::vector<MountEvent> drainEvents();

private:
    void onMountAdded(const Glib::RefPtr<Gio::Mount>& mount);
    void onMountRemoved(const Glib::RefPtr<Gio::Mount>& mount);

    Glib::RefPtr<Gio::VolumeMonitor> monitor_;
    sigc::connection connAdded_;
    sigc::connection connRemoved_;

    mutable std::mutex eventsMutex_;
    std::deque<MountEvent> events_;
    static constexpr size_t MAX_EVENTS = 100;
    bool running_{false};
};

} // namespace mcp
