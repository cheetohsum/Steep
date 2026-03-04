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

#include "mcp/mcpprotocol.h"

#include "rtengine/cJSON.h"

#include <cstdlib>
#include <cstring>

namespace mcp {

// JsonPtr implementation
JsonPtr::JsonPtr() : ptr_(nullptr) {}
JsonPtr::JsonPtr(cJSON* p) : ptr_(p) {}
JsonPtr::~JsonPtr() { if (ptr_) cJSON_Delete(ptr_); }
JsonPtr::JsonPtr(JsonPtr&& other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }
JsonPtr& JsonPtr::operator=(JsonPtr&& other) noexcept {
    if (this != &other) {
        if (ptr_) cJSON_Delete(ptr_);
        ptr_ = other.ptr_;
        other.ptr_ = nullptr;
    }
    return *this;
}
cJSON* JsonPtr::get() const { return ptr_; }
cJSON* JsonPtr::release() { cJSON* p = ptr_; ptr_ = nullptr; return p; }
JsonPtr::operator bool() const { return ptr_ != nullptr; }

std::string buildResult(cJSON* id, cJSON* result) {
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    if (id) {
        cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, true));
    } else {
        cJSON_AddNullToObject(resp, "id");
    }
    cJSON_AddItemToObject(resp, "result", result ? result : cJSON_CreateObject());

    char* str = cJSON_PrintUnformatted(resp);
    std::string out(str);
    free(str);
    cJSON_Delete(resp);
    return out;
}

std::string buildError(cJSON* id, ErrorCode code, const std::string& message) {
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    if (id) {
        cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, true));
    } else {
        cJSON_AddNullToObject(resp, "id");
    }

    cJSON* err = cJSON_CreateObject();
    cJSON_AddNumberToObject(err, "code", static_cast<int>(code));
    cJSON_AddStringToObject(err, "message", message.c_str());
    cJSON_AddItemToObject(resp, "error", err);

    char* str = cJSON_PrintUnformatted(resp);
    std::string out(str);
    free(str);
    cJSON_Delete(resp);
    return out;
}

cJSON* buildInitializeResult(const std::string& serverName, const std::string& serverVersion) {
    cJSON* result = cJSON_CreateObject();

    // Protocol version
    cJSON_AddStringToObject(result, "protocolVersion", "2025-03-26");

    // Capabilities
    cJSON* caps = cJSON_CreateObject();
    cJSON* tools = cJSON_CreateObject();
    cJSON_AddItemToObject(caps, "tools", tools);
    cJSON* resources = cJSON_CreateObject();
    cJSON_AddItemToObject(caps, "resources", resources);
    cJSON_AddItemToObject(result, "capabilities", caps);

    // Server info
    cJSON* info = cJSON_CreateObject();
    cJSON_AddStringToObject(info, "name", serverName.c_str());
    cJSON_AddStringToObject(info, "version", serverVersion.c_str());
    cJSON_AddItemToObject(result, "serverInfo", info);

    return result;
}

cJSON* buildToolsListResult(const std::vector<ToolDef>& tools) {
    cJSON* result = cJSON_CreateObject();
    cJSON* arr = cJSON_CreateArray();

    for (const auto& t : tools) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", t.name.c_str());
        cJSON_AddStringToObject(item, "description", t.description.c_str());
        if (t.inputSchema) {
            cJSON_AddItemToObject(item, "inputSchema", cJSON_Duplicate(t.inputSchema, true));
        } else {
            // Empty object schema
            cJSON* schema = cJSON_CreateObject();
            cJSON_AddStringToObject(schema, "type", "object");
            cJSON_AddItemToObject(item, "inputSchema", schema);
        }
        cJSON_AddItemToArray(arr, item);
    }

    cJSON_AddItemToObject(result, "tools", arr);
    return result;
}

cJSON* buildResourcesListResult(const std::vector<ResourceDef>& resources) {
    cJSON* result = cJSON_CreateObject();
    cJSON* arr = cJSON_CreateArray();

    for (const auto& r : resources) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "uri", r.uri.c_str());
        cJSON_AddStringToObject(item, "name", r.name.c_str());
        cJSON_AddStringToObject(item, "description", r.description.c_str());
        if (!r.mimeType.empty()) {
            cJSON_AddStringToObject(item, "mimeType", r.mimeType.c_str());
        }
        cJSON_AddItemToArray(arr, item);
    }

    cJSON_AddItemToObject(result, "resources", arr);
    return result;
}

cJSON* buildToolResult(const std::string& text, bool isError) {
    cJSON* result = cJSON_CreateObject();
    cJSON* content = cJSON_CreateArray();
    cJSON* item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text.c_str());
    cJSON_AddItemToArray(content, item);
    cJSON_AddItemToObject(result, "content", content);
    if (isError) {
        cJSON_AddTrueToObject(result, "isError");
    }
    return result;
}

cJSON* buildToolResultJson(cJSON* jsonContent, bool isError) {
    char* str = cJSON_Print(jsonContent);
    std::string text(str);
    free(str);
    return buildToolResult(text, isError);
}

cJSON* buildResourceReadResult(const std::string& uri, const std::string& text, const std::string& mimeType) {
    cJSON* result = cJSON_CreateObject();
    cJSON* contents = cJSON_CreateArray();
    cJSON* item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "uri", uri.c_str());
    cJSON_AddStringToObject(item, "mimeType", mimeType.c_str());
    cJSON_AddStringToObject(item, "text", text.c_str());
    cJSON_AddItemToArray(contents, item);
    cJSON_AddItemToObject(result, "contents", contents);
    return result;
}

cJSON* createObjectSchema(const std::vector<std::pair<std::string, std::string>>& properties,
                          const std::vector<std::string>& required) {
    cJSON* schema = cJSON_CreateObject();
    cJSON_AddStringToObject(schema, "type", "object");

    cJSON* props = cJSON_CreateObject();
    for (const auto& p : properties) {
        cJSON* prop = cJSON_CreateObject();
        cJSON_AddStringToObject(prop, "type", p.second.c_str());
        cJSON_AddItemToObject(props, p.first.c_str(), prop);
    }
    cJSON_AddItemToObject(schema, "properties", props);

    if (!required.empty()) {
        cJSON* req = cJSON_CreateArray();
        for (const auto& r : required) {
            cJSON_AddItemToArray(req, cJSON_CreateString(r.c_str()));
        }
        cJSON_AddItemToObject(schema, "required", req);
    }

    return schema;
}

bool parseRequest(const std::string& body, std::string& method, cJSON*& params, cJSON*& id) {
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        return false;
    }

    // Verify jsonrpc version
    cJSON* ver = cJSON_GetObjectItemCaseSensitive(root, "jsonrpc");
    if (!ver || !cJSON_IsString(ver) || strcmp(ver->valuestring, "2.0") != 0) {
        cJSON_Delete(root);
        return false;
    }

    // Extract method
    cJSON* m = cJSON_GetObjectItemCaseSensitive(root, "method");
    if (!m || !cJSON_IsString(m)) {
        cJSON_Delete(root);
        return false;
    }
    method = m->valuestring;

    // Extract id (can be string, number, or null)
    id = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (id) {
        id = cJSON_Duplicate(id, true);
    }

    // Extract params (optional)
    params = cJSON_GetObjectItemCaseSensitive(root, "params");
    if (params) {
        params = cJSON_Duplicate(params, true);
    }

    cJSON_Delete(root);
    return true;
}

} // namespace mcp
