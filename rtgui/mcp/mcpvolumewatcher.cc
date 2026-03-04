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

#include "mcp/mcpvolumewatcher.h"

#include <ctime>

namespace mcp {

namespace {

std::string nowISO8601() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    struct tm tm;
    localtime_r(&t, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}

} // anonymous namespace

VolumeWatcher::VolumeWatcher() = default;

VolumeWatcher::~VolumeWatcher() {
    stop();
}

void VolumeWatcher::start() {
    if (running_) return;

    monitor_ = Gio::VolumeMonitor::get();
    connAdded_ = monitor_->signal_mount_added().connect(
        sigc::mem_fun(*this, &VolumeWatcher::onMountAdded));
    connRemoved_ = monitor_->signal_mount_removed().connect(
        sigc::mem_fun(*this, &VolumeWatcher::onMountRemoved));
    running_ = true;
}

void VolumeWatcher::stop() {
    if (!running_) return;

    connAdded_.disconnect();
    connRemoved_.disconnect();
    monitor_.reset();
    running_ = false;
}

std::vector<VolumeInfo> VolumeWatcher::getVolumes() const {
    std::vector<VolumeInfo> result;
    if (!monitor_) return result;

    // Drives and their volumes
    auto drives = monitor_->get_connected_drives();
    for (const auto& drive : drives) {
        auto volumes = drive->get_volumes();
        for (const auto& vol : volumes) {
            VolumeInfo info;
            info.name = vol->get_name();

            auto mount = vol->get_mount();
            if (mount) {
                info.path = mount->get_root()->get_parse_name();
                info.mounted = true;
            } else {
                info.mounted = false;
            }

            if (drive->is_media_removable()) {
                info.type = "removable";
            } else {
                info.type = "fixed";
            }

            result.push_back(std::move(info));
        }

        // Drive with no volumes
        if (volumes.empty()) {
            VolumeInfo info;
            info.name = drive->get_name();
            info.mounted = false;
            if (drive->is_media_removable()) {
                info.type = "removable";
            } else {
                info.type = "fixed";
            }
            result.push_back(std::move(info));
        }
    }

    // Volumes not associated with a drive (e.g., network mounts)
    auto volumes = monitor_->get_volumes();
    for (const auto& vol : volumes) {
        if (vol->get_drive()) continue;

        VolumeInfo info;
        info.name = vol->get_name();
        info.type = "unknown";

        auto mount = vol->get_mount();
        if (mount) {
            info.path = mount->get_root()->get_parse_name();
            info.mounted = true;
        } else {
            info.mounted = false;
        }

        result.push_back(std::move(info));
    }

    // Mounts not associated with a volume
    auto mounts = monitor_->get_mounts();
    for (const auto& mount : mounts) {
        if (mount->get_volume()) continue;

        VolumeInfo info;
        info.name = mount->get_name();
        info.path = mount->get_root()->get_parse_name();
        info.type = "unknown";
        info.mounted = true;

        result.push_back(std::move(info));
    }

    return result;
}

std::vector<MountEvent> VolumeWatcher::drainEvents() {
    std::lock_guard<std::mutex> lock(eventsMutex_);
    std::vector<MountEvent> result(events_.begin(), events_.end());
    events_.clear();
    return result;
}

void VolumeWatcher::onMountAdded(const Glib::RefPtr<Gio::Mount>& mount) {
    MountEvent evt;
    evt.event = "mount_added";
    evt.name = mount->get_name();
    evt.path = mount->get_root()->get_parse_name();
    evt.timestamp = nowISO8601();

    std::lock_guard<std::mutex> lock(eventsMutex_);
    events_.push_back(std::move(evt));
    while (events_.size() > MAX_EVENTS) {
        events_.pop_front();
    }
}

void VolumeWatcher::onMountRemoved(const Glib::RefPtr<Gio::Mount>& mount) {
    MountEvent evt;
    evt.event = "mount_removed";
    evt.name = mount->get_name();
    try {
        auto root = mount->get_root();
        if (root) evt.path = root->get_parse_name();
    } catch (...) {}
    evt.timestamp = nowISO8601();

    std::lock_guard<std::mutex> lock(eventsMutex_);
    events_.push_back(std::move(evt));
    while (events_.size() > MAX_EVENTS) {
        events_.pop_front();
    }
}

} // namespace mcp
