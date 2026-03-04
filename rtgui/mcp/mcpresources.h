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

namespace mcp {

class McpServer;

// Get all resource definitions for resources/list
std::vector<ResourceDef> getResourceDefinitions();

// Handle a resources/read request. Returns JSON string of the result.
// Must be called on GTK thread.
std::string handleResourceRead(const std::string& uri, McpServer* server);

} // namespace mcp
