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

#include "mcp/mcpserver.h"
#include "mcp/mcpprotocol.h"
#include "mcp/mcptools.h"
#include "mcp/mcpresources.h"

#include "rtengine/cJSON.h"

#include <glibmm/main.h>

// Must be included after all other headers to avoid conflicts
#include "mcp/httplib.h"

#include <iostream>
#include <random>

namespace mcp {

McpServer::McpServer() = default;

McpServer::~McpServer() {
    stop();
}

bool McpServer::start(int port) {
    if (running_) {
        return false;
    }

    // Auto-select port with retry if 0
    if (port == 0) {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(39000, 39999);
        port = dis(gen);
    }

    volumeWatcher_.start();

    thread_ = std::thread(&McpServer::serverThread, this, port);

    // Wait briefly for server to start
    for (int i = 0; i < 50; ++i) {
        if (running_) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return running_.load();
}

void McpServer::stop() {
    if (!running_) return;

    volumeWatcher_.stop();
    running_ = false;
    if (server_) {
        server_->stop();
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    server_.reset();
    port_ = 0;
    initialized_ = false;
}

bool McpServer::isRunning() const {
    return running_.load();
}

int McpServer::getPort() const {
    return port_.load();
}

void McpServer::setEditorPanel(EditorPanel* ep) {
    std::lock_guard<std::mutex> lock(epMutex_);
    editorPanel_ = ep;
}

EditorPanel* McpServer::getEditorPanel() const {
    std::lock_guard<std::mutex> lock(epMutex_);
    return editorPanel_;
}

std::string McpServer::dispatchToGtk(std::function<std::string()> fn) {
    // Each request gets its own completion slot - safe for concurrent calls
    auto slot = std::make_shared<GtkDispatchSlot>();
    slot->task = std::move(fn);

    // Post to GTK main loop - capture shared_ptr by value to keep alive
    Glib::signal_idle().connect_once([slot]() {
        std::string result;
        {
            std::lock_guard<std::mutex> lock(slot->mutex);
            if (slot->task) {
                result = slot->task();
                slot->task = nullptr;
            }
            slot->result = std::move(result);
            slot->done = true;
        }
        slot->cv.notify_one();
    });

    // Wait for GTK thread with timeout to prevent deadlock during shutdown
    std::unique_lock<std::mutex> lock(slot->mutex);
    if (!slot->cv.wait_for(lock, std::chrono::seconds(10), [&slot]() { return slot->done; })) {
        slot->task = nullptr;
        std::cerr << "[MCP] dispatchToGtk timed out after 10s" << std::endl;
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Request timed out waiting for GTK main thread.\"}],\"isError\":true}";
    }
    return slot->result;
}

void McpServer::serverThread(int port) {
    server_.reset(new httplib::Server());

    // Try to bind, with retries on different ports if auto-selected
    bool bound = false;
    int attempts = 0;
    const int maxAttempts = 10;

    while (!bound && attempts < maxAttempts) {
        if (server_->bind_to_port("127.0.0.1", port)) {
            bound = true;
        } else {
            std::cerr << "[MCP] Port " << port << " unavailable, retrying..." << std::endl;
            attempts++;
            // Pick a new random port
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> dis(39000, 39999);
            port = dis(gen);
        }
    }

    if (!bound) {
        std::cerr << "[MCP] Failed to bind to any port after " << maxAttempts << " attempts" << std::endl;
        server_.reset();
        return;
    }

    port_ = port;
    std::cerr << "[MCP] Server bound to port " << port << std::endl;

    // CORS headers for local development
    auto setHeaders = [](httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "POST, GET, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
    };

    // OPTIONS handler for CORS preflight
    server_->Options("/mcp", [&setHeaders](const httplib::Request&, httplib::Response& res) {
        setHeaders(res);
        res.status = 204;
    });

    // POST /mcp - main MCP endpoint
    server_->Post("/mcp", [this, &setHeaders](const httplib::Request& req, httplib::Response& res) {
        setHeaders(res);
        res.set_header("Content-Type", "application/json");

        std::string response;
        handleMcpPost(req.body, response);
        res.set_content(response, "application/json");
    });

    // GET /mcp - SSE endpoint (not implemented yet, return method not allowed)
    server_->Get("/mcp", [&setHeaders](const httplib::Request&, httplib::Response& res) {
        setHeaders(res);
        res.status = 405;
        res.set_content("{\"error\":\"SSE not implemented, use POST\"}", "application/json");
    });

    // Health check
    server_->Get("/health", [this](const httplib::Request&, httplib::Response& res) {
        res.set_content("{\"status\":\"ok\",\"port\":" + std::to_string(port_.load()) + "}", "application/json");
    });

    running_ = true;
    std::cerr << "[MCP] Server listening on http://127.0.0.1:" << port << "/mcp" << std::endl;
    server_->listen_after_bind();
    std::cerr << "[MCP] Server stopped" << std::endl;
    running_ = false;
}

void McpServer::handleMcpPost(const std::string& body, std::string& response) {
    std::string method;
    cJSON* params = nullptr;
    cJSON* id = nullptr;

    if (!parseRequest(body, method, params, id)) {
        response = buildError(nullptr, ErrorCode::PARSE_ERROR, "Invalid JSON-RPC request");
        return;
    }

    response = handleMethod(method, params, id);

    if (params) cJSON_Delete(params);
    if (id) cJSON_Delete(id);
}

std::string McpServer::handleMethod(const std::string& method, cJSON* params, cJSON* id) {
    // MCP lifecycle methods (can be handled on HTTP thread)
    if (method == "initialize") {
        initialized_ = true;
        cJSON* result = buildInitializeResult("steep", "6.0");
        return buildResult(id, result);
    }

    if (method == "notifications/initialized") {
        // Client acknowledgment, no response needed for notifications
        // But since this comes via POST, we still send a response
        return buildResult(id, cJSON_CreateObject());
    }

    if (method == "ping") {
        return buildResult(id, cJSON_CreateObject());
    }

    // Tools
    if (method == "tools/list") {
        auto tools = getToolDefinitions();
        cJSON* result = buildToolsListResult(tools);
        // Clean up input schemas
        for (auto& t : tools) {
            if (t.inputSchema) cJSON_Delete(t.inputSchema);
        }
        return buildResult(id, result);
    }

    if (method == "tools/call") {
        if (!params) {
            return buildError(id, ErrorCode::INVALID_PARAMS, "Missing params");
        }
        cJSON* nameJ = cJSON_GetObjectItemCaseSensitive(params, "name");
        if (!nameJ || !cJSON_IsString(nameJ)) {
            return buildError(id, ErrorCode::INVALID_PARAMS, "Missing tool name");
        }
        std::string toolName = nameJ->valuestring;
        cJSON* args = cJSON_GetObjectItemCaseSensitive(params, "arguments");

        // Dispatch tool call to GTK thread
        std::string resultStr = dispatchToGtk([this, &toolName, args]() -> std::string {
            return handleToolCall(toolName, args, this);
        });

        // Parse the result string back to JSON
        cJSON* resultJson = cJSON_Parse(resultStr.c_str());
        if (!resultJson) {
            return buildError(id, ErrorCode::INTERNAL_ERROR, "Failed to build tool result");
        }
        return buildResult(id, resultJson);
    }

    // Resources
    if (method == "resources/list") {
        auto resources = getResourceDefinitions();
        cJSON* result = buildResourcesListResult(resources);
        return buildResult(id, result);
    }

    if (method == "resources/read") {
        if (!params) {
            return buildError(id, ErrorCode::INVALID_PARAMS, "Missing params");
        }
        cJSON* uriJ = cJSON_GetObjectItemCaseSensitive(params, "uri");
        if (!uriJ || !cJSON_IsString(uriJ)) {
            return buildError(id, ErrorCode::INVALID_PARAMS, "Missing resource URI");
        }
        std::string uri = uriJ->valuestring;

        std::string resultStr = dispatchToGtk([this, &uri]() -> std::string {
            return handleResourceRead(uri, this);
        });

        cJSON* resultJson = cJSON_Parse(resultStr.c_str());
        if (!resultJson) {
            return buildError(id, ErrorCode::INTERNAL_ERROR, "Failed to read resource");
        }
        return buildResult(id, resultJson);
    }

    return buildError(id, ErrorCode::METHOD_NOT_FOUND, "Unknown method: " + method);
}

} // namespace mcp
