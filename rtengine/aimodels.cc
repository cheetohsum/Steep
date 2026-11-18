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
#include "aimodels.h"

#include <algorithm>
#include <mutex>

#include <glib/gstdio.h>
#include <glibmm/fileutils.h>
#include <glibmm/miscutils.h>

#include "aiidlereaper.h"
#include "airemover.h"
#include "samselector.h"
#include "settings.h"

namespace rtengine
{

namespace aimodels
{

const char* const kOff = "off";

namespace
{

std::mutex rootsMutex;
Glib::ustring roots[2];

long long megabytesOf(const Glib::ustring& path)
{
    GStatBuf info;
    return g_stat(path.c_str(), &info) == 0 ? static_cast<long long>(info.st_size / 1000000) : 0;
}

bool isFile(const Glib::ustring& path)
{
    return Glib::file_test(path, Glib::FILE_TEST_IS_REGULAR);
}

std::vector<Glib::ustring> rootList()
{
    std::lock_guard<std::mutex> lock(rootsMutex);
    std::vector<Glib::ustring> list;
    for (const auto& root : roots) {
        if (!root.empty()) list.push_back(root);
    }
    return list;
}

// A single-file model, wherever it is installed.
void addFile(std::vector<Choice>& out, const char* id, const char* name, const char* file)
{
    for (const auto& root : rootList()) {
        const Glib::ustring path = Glib::build_filename(root, file);
        if (isFile(path)) {
            out.push_back({id, name, path, megabytesOf(path)});
            return;
        }
    }
}

// Every SAM 2.1 size exports to the same two file names, so the size is told
// from the encoder itself (39, 46, 81 and 224 million weights).
Glib::ustring samName(long long encoderMegabytes)
{
    return encoderMegabytes < 150 ? "SAM 2.1 Tiny"
         : encoderMegabytes < 230 ? "SAM 2.1 Small"
         : encoderMegabytes < 500 ? "SAM 2.1 Base+"
         : "SAM 2.1 Large";
}

void addSamDirectory(std::vector<Choice>& out, const std::string& id, const Glib::ustring& directory)
{
    const Glib::ustring encoder = Glib::build_filename(directory, "sam2_encoder.onnx");
    const Glib::ustring decoder = Glib::build_filename(directory, "sam2_decoder.onnx");
    if (!isFile(encoder) || !isFile(decoder)) return;
    for (const auto& have : out) {
        if (have.id == id) return;   // the application's copy wins over the user's
    }
    const long long encoderMb = megabytesOf(encoder);
    out.push_back({id, samName(encoderMb), directory, encoderMb + megabytesOf(decoder)});
}

}

void setRoots(const Glib::ustring& applicationModels, const Glib::ustring& userModels)
{
    std::lock_guard<std::mutex> lock(rootsMutex);
    roots[0] = applicationModels;
    roots[1] = userModels;
}

std::vector<Choice> installed(Role role)
{
    std::vector<Choice> out;

    switch (role) {
        case Role::Scene:
            // HRNetV2: 43.2 mIoU on ADE20K against MobileNetV2's 34.8, and a
            // quarter- rather than eighth-resolution output.
            addFile(out, "hrnetv2", "HRNetV2", "ade20k_hrnetv2_c1.onnx");
            addFile(out, "mobilenetv2", "MobileNetV2", "ade20k_mobilenetv2_c1.onnx");
            break;

        case Role::Subject:
            // BiRefNet is trained at 1024 px against U2-Net's 320: it is what
            // keeps hair and fur in a subject mask.
            addFile(out, "birefnet", "BiRefNet", "birefnet_subject.onnx");
            addFile(out, "u2net", "U²-Net", "u2net_subject.onnx");
            break;

        case Role::Select:
            // The pair directly under models/ is the default; every
            // sub-directory holding a pair is an alternative.
            for (const auto& root : rootList()) {
                addSamDirectory(out, "standard", root);
            }
            for (const auto& root : rootList()) {
                try {
                    std::vector<std::string> names;
                    for (const auto& name : Glib::Dir(root)) names.push_back(name);
                    std::sort(names.begin(), names.end());
                    for (const auto& name : names) {
                        const Glib::ustring directory = Glib::build_filename(root, name);
                        if (Glib::file_test(directory, Glib::FILE_TEST_IS_DIR)) {
                            addSamDirectory(out, name, directory);
                        }
                    }
                } catch (const Glib::Error&) {
                    // no such root
                }
            }
            break;

        case Role::WideFill:
            for (const auto& root : rootList()) {
                bool all = true;
                long long megabytes = 0;
                for (const char* file : {"rorem_unet.onnx", "rorem_unet.onnx.data", "rorem_vae_encoder.onnx", "rorem_vae_decoder.onnx"}) {
                    const Glib::ustring path = Glib::build_filename(root, file);
                    all = all && isFile(path);
                    megabytes += megabytesOf(path);
                }
                if (all) {
                    out.push_back({"rorem", "RORem", root, megabytes});
                    break;
                }
            }
            break;
    }

    return out;
}

Choice resolve(Role role, const Glib::ustring& wanted)
{
    if (role == Role::WideFill && wanted == kOff) return Choice();
    const auto have = installed(role);
    for (const auto& choice : have) {
        if (wanted == choice.id) return choice;
    }
    return have.empty() ? Choice() : have.front();
}

void applyLive(const Settings& settings)
{
    AIIdleReaper::setKeepLoaded(settings.aiKeepLoaded);
    SamSelector::getInstance().setModelDirectory(resolve(Role::Select, settings.aiModelSelect).location);
#ifdef RT_AI_MASKING
    AIRemoverEngine::getInstance().setModelDirectory(resolve(Role::WideFill, settings.aiModelWideFill).location);
#endif
}

}

}
