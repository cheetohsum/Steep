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

#include "mcp/mcpprotocol.h"

struct cJSON;

namespace rtengine { namespace procparams { class ProcParams; } }

namespace mcp {

class McpServer;

// Get all tool definitions for tools/list
std::vector<ToolDef> getToolDefinitions();

// Handle a tools/call request. Returns JSON string of the result.
// Must be called on GTK thread.
std::string handleToolCall(const std::string& toolName, cJSON* args, McpServer* server);

// Serialize ProcParams to cJSON. Caller must cJSON_Delete the result.
// If section is non-empty, only that section is included.
cJSON* paramsToJson(const rtengine::procparams::ProcParams& pp, const std::string& section = "");

} // namespace mcp
