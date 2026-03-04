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

#include "mcp/mcpresources.h"
#include "mcp/mcpserver.h"
#include "mcp/mcpprotocol.h"
#include "mcp/mcptools.h"

#include "editorpanel.h"
#include "toolpanelcoord.h"

#include "rtengine/procparams.h"
#include "rtengine/rtengine.h"
#include "rtengine/cJSON.h"

#include <cstdlib>

using namespace rtengine::procparams;

namespace mcp {

std::vector<ResourceDef> getResourceDefinitions() {
    return {
        {"rawtherapee://current-image", "Current Image", "Metadata about the currently open image", "application/json"},
        {"rawtherapee://params", "Processing Parameters", "Full current processing parameters as JSON", "application/json"},
        {"rawtherapee://tools", "Tool List", "All processing tools with enabled state", "application/json"},
        {"rawtherapee://server-status", "Server Status", "MCP server status information", "application/json"},
    };
}

std::string handleResourceRead(const std::string& uri, McpServer* server) {
    EditorPanel* ep = server->getEditorPanel();

    if (uri == "rawtherapee://server-status") {
        cJSON* status = cJSON_CreateObject();
        cJSON_AddNumberToObject(status, "port", server->getPort());
        cJSON_AddBoolToObject(status, "imageOpen", ep && ep->getIpc());
        char* text = cJSON_Print(status);
        cJSON* result = buildResourceReadResult(uri, text);
        free(text);
        cJSON_Delete(status);
        char* s = cJSON_PrintUnformatted(result);
        std::string out(s);
        free(s);
        cJSON_Delete(result);
        return out;
    }

    if (!ep || !ep->getIpc()) {
        cJSON* result = buildResourceReadResult(uri, "{\"error\": \"No image open\"}");
        char* s = cJSON_PrintUnformatted(result);
        std::string out(s);
        free(s);
        cJSON_Delete(result);
        return out;
    }

    if (uri == "rawtherapee://current-image") {
        auto* ipc = ep->getIpc();
        auto* isrc = ipc->getInitialImage();

        cJSON* info = cJSON_CreateObject();
        cJSON_AddStringToObject(info, "filename", isrc->getFileName().c_str());
        cJSON_AddNumberToObject(info, "width", ipc->getFullWidth());
        cJSON_AddNumberToObject(info, "height", ipc->getFullHeight());

        const auto* meta = isrc->getMetaData();
        if (meta && meta->hasExif()) {
            cJSON* exif = cJSON_CreateObject();
            cJSON_AddNumberToObject(exif, "iso", meta->getISOSpeed());
            cJSON_AddNumberToObject(exif, "fnumber", meta->getFNumber());
            cJSON_AddNumberToObject(exif, "focalLength", meta->getFocalLen());
            cJSON_AddNumberToObject(exif, "shutterSpeed", meta->getShutterSpeed());
            cJSON_AddStringToObject(exif, "camera", meta->getCamera().c_str());
            cJSON_AddStringToObject(exif, "lens", meta->getLens().c_str());
            cJSON_AddItemToObject(info, "exif", exif);
        }

        char* text = cJSON_Print(info);
        cJSON* result = buildResourceReadResult(uri, text);
        free(text);
        cJSON_Delete(info);
        char* s = cJSON_PrintUnformatted(result);
        std::string out(s);
        free(s);
        cJSON_Delete(result);
        return out;
    }

    if (uri == "rawtherapee://params") {
        ProcParams pp;
        ep->getIpc()->getParams(&pp);

        cJSON* params = paramsToJson(pp);
        char* text = cJSON_Print(params);
        cJSON* result = buildResourceReadResult(uri, text);
        free(text);
        cJSON_Delete(params);
        char* s = cJSON_PrintUnformatted(result);
        std::string out(s);
        free(s);
        cJSON_Delete(result);
        return out;
    }

    if (uri == "rawtherapee://tools") {
        ProcParams pp;
        ep->getIpc()->getParams(&pp);

        cJSON* arr = cJSON_CreateArray();

        // Add key tools with enabled state
        auto addTool = [&](const char* name, bool enabled) {
            cJSON* item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "name", name);
            cJSON_AddBoolToObject(item, "enabled", enabled);
            cJSON_AddItemToArray(arr, item);
        };

        addTool("labCurve", pp.labCurve.enabled);
        addTool("sharpening", pp.sharpening.enabled);
        addTool("vibrance", pp.vibrance.enabled);
        addTool("wb", pp.wb.enabled);
        addTool("defringe", pp.defringe.enabled);
        addTool("dirpyrDenoise", pp.dirpyrDenoise.enabled);
        addTool("impulseDenoise", pp.impulseDenoise.enabled);
        addTool("sh", pp.sh.enabled);
        addTool("localContrast", pp.localContrast.enabled);
        addTool("crop", pp.crop.enabled);
        addTool("resize", pp.resize.enabled);
        addTool("gradient", pp.gradient.enabled);
        addTool("pcvignette", pp.pcvignette.enabled);
        addTool("retinex", pp.retinex.enabled);
        addTool("rgbCurves", pp.rgbCurves.enabled);
        addTool("colorGrading", pp.colorGrading.enabled);
        addTool("dehaze", pp.dehaze.enabled);
        addTool("softlight", pp.softlight.enabled);
        addTool("fattal", pp.fattal.enabled);
        addTool("filmSimulation", pp.filmSimulation.enabled);
        addTool("blackwhite", pp.blackwhite.enabled);
        addTool("hsvequalizer", pp.hsvequalizer.enabled);

        char* text = cJSON_Print(arr);
        cJSON* result = buildResourceReadResult(uri, text);
        free(text);
        cJSON_Delete(arr);
        char* s = cJSON_PrintUnformatted(result);
        std::string out(s);
        free(s);
        cJSON_Delete(result);
        return out;
    }

    // Unknown resource
    cJSON* result = buildResourceReadResult(uri, "{\"error\": \"Unknown resource URI\"}");
    char* s = cJSON_PrintUnformatted(result);
    std::string out(s);
    free(s);
    cJSON_Delete(result);
    return out;
}

} // namespace mcp
