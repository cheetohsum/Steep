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

#include <string>
#include <vector>
#include <functional>

// Forward declaration
struct cJSON;

namespace mcp {

// JSON-RPC 2.0 error codes
enum class ErrorCode : int {
    PARSE_ERROR = -32700,
    INVALID_REQUEST = -32600,
    METHOD_NOT_FOUND = -32601,
    INVALID_PARAMS = -32602,
    INTERNAL_ERROR = -32603,
};

// MCP tool definition
struct ToolDef {
    std::string name;
    std::string description;
    cJSON* inputSchema; // JSON Schema object (owned externally or static)
};

// MCP resource definition
struct ResourceDef {
    std::string uri;
    std::string name;
    std::string description;
    std::string mimeType;
};

// Helper to manage cJSON lifetime
class JsonPtr {
public:
    JsonPtr();
    explicit JsonPtr(cJSON* p);
    ~JsonPtr();
    JsonPtr(JsonPtr&& other) noexcept;
    JsonPtr& operator=(JsonPtr&& other) noexcept;
    JsonPtr(const JsonPtr&) = delete;
    JsonPtr& operator=(const JsonPtr&) = delete;

    cJSON* get() const;
    cJSON* release();
    explicit operator bool() const;

private:
    cJSON* ptr_;
};

// Build a JSON-RPC 2.0 success response
std::string buildResult(cJSON* id, cJSON* result);

// Build a JSON-RPC 2.0 error response
std::string buildError(cJSON* id, ErrorCode code, const std::string& message);

// Build MCP initialize result
cJSON* buildInitializeResult(const std::string& serverName, const std::string& serverVersion);

// Build MCP tools/list result
cJSON* buildToolsListResult(const std::vector<ToolDef>& tools);

// Build MCP resources/list result
cJSON* buildResourcesListResult(const std::vector<ResourceDef>& resources);

// Build MCP tools/call result with text content
cJSON* buildToolResult(const std::string& text, bool isError = false);

// Build MCP tools/call result with JSON content embedded as text
cJSON* buildToolResultJson(cJSON* jsonContent, bool isError = false);

// Build MCP resources/read result
cJSON* buildResourceReadResult(const std::string& uri, const std::string& text, const std::string& mimeType = "application/json");

// Create a JSON Schema object for tool input
cJSON* createObjectSchema(const std::vector<std::pair<std::string, std::string>>& properties,
                          const std::vector<std::string>& required = {});

// Parse JSON-RPC request and extract method, params, id
bool parseRequest(const std::string& body, std::string& method, cJSON*& params, cJSON*& id);

} // namespace mcp
