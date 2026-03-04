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

#include "mcp/mcpvolumewatcher.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class EditorPanel;
struct cJSON;

namespace httplib { class Server; }

namespace mcp {

// Per-request completion token for thread-safe GTK dispatch
struct GtkDispatchSlot {
    std::mutex mutex;
    std::condition_variable cv;
    std::function<std::string()> task;
    std::string result;
    bool done{false};
};

class McpServer {
public:
    McpServer();
    ~McpServer();

    // Start the HTTP server on the given port (0 = auto-select)
    bool start(int port = 0);

    // Stop the HTTP server
    void stop();

    // Is the server running?
    bool isRunning() const;

    // Get the port the server is listening on
    int getPort() const;

    // Set/get the active editor panel (called from GTK thread)
    void setEditorPanel(EditorPanel* ep);
    EditorPanel* getEditorPanel() const;

    // Volume watcher for SD card / removable media detection
    VolumeWatcher& getVolumeWatcher() { return volumeWatcher_; }

    // Dispatch a function to the GTK main thread and wait for result.
    // Thread-safe: each call gets its own completion slot.
    // Must be called from the HTTP thread (not GTK thread).
    std::string dispatchToGtk(std::function<std::string()> fn);

private:
    void serverThread(int port);
    void handleMcpPost(const std::string& body, std::string& response);
    std::string handleMethod(const std::string& method, ::cJSON* params, ::cJSON* id);

    std::unique_ptr<httplib::Server> server_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<int> port_{0};
    std::atomic<bool> initialized_{false};

    // Editor panel access (only access from GTK thread or via dispatchToGtk)
    EditorPanel* editorPanel_{nullptr};
    mutable std::mutex epMutex_;

    VolumeWatcher volumeWatcher_;
};

} // namespace mcp
