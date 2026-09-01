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
#include "modeldownloader.h"

#include <glib/gstdio.h>
#include <giomm.h>

#include <algorithm>
#include <iomanip>
#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#endif

#include "multilangmgr.h"
#include "options.h"

#include "rtengine/aiinpainting.h"

#ifndef STEEP_INPAINT_MODEL_URL
#define STEEP_INPAINT_MODEL_URL ""
#endif

#ifndef STEEP_INPAINT_MODEL_SHA256
#define STEEP_INPAINT_MODEL_SHA256 ""
#endif

namespace
{

constexpr const char* MODEL_FILE = "lama_inpainting.onnx";

// Roughly what to expect, only so the progress bar can show a fraction before
// curl has reported anything. Being wrong here costs nothing: the bar falls
// back to counting bytes.
constexpr double EXPECTED_BYTES = 208044816.0;

Glib::ustring modelDirectory()
{
    return Glib::build_filename(Options::rtdir, "models");
}

Glib::ustring humanSize(double bytes)
{
    return Glib::ustring::format(std::fixed, std::setprecision(0), bytes / 1048576.0) + " MB";
}

/// Locate curl once. Plain "curl" resolves through PATH on every platform we
/// ship to; the absolute paths are a fallback for the cases where a stripped
/// environment has no useful PATH (some AppImage launchers, notably).
Glib::ustring findCurl()
{
    const char* candidates[] = {
#ifdef _WIN32
        "curl.exe",
        "C:\\Windows\\System32\\curl.exe",
#else
        "curl",
        "/usr/bin/curl",
        "/bin/curl",
#endif
    };

    for (const char* candidate : candidates) {
        const auto found = Glib::find_program_in_path(candidate);

        if (!found.empty()) {
            return found;
        }

        if (Glib::file_test(candidate, Glib::FILE_TEST_IS_EXECUTABLE)) {
            return candidate;
        }
    }

    return Glib::ustring();
}

} // namespace


bool ModelDownloader::isConfigured()
{
    return Glib::ustring(STEEP_INPAINT_MODEL_URL).length() > 0;
}

Glib::ustring ModelDownloader::modelPath()
{
    return Glib::build_filename(modelDirectory(), MODEL_FILE);
}

bool ModelDownloader::isPresent()
{
    return Glib::file_test(modelPath(), Glib::FILE_TEST_EXISTS);
}

bool ModelDownloader::runDialog(Gtk::Window& parent)
{
    if (!isConfigured()) {
        return false;
    }

    const Glib::ustring curl = findCurl();

    if (curl.empty()) {
        Gtk::MessageDialog err(parent, M("MODELDOWNLOAD_NO_CURL"), false,
                               Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
        err.set_secondary_text(Glib::ustring::compose(
            M("MODELDOWNLOAD_MANUAL"), Glib::ustring(STEEP_INPAINT_MODEL_URL), modelPath()));
        err.run();
        return false;
    }

    g_mkdir_with_parents(modelDirectory().c_str(), 0755);

    // Download beside the target, then rename. A half-finished file left under
    // the real name would satisfy the existence check in rtengine/init.cc and
    // be loaded as though it were whole on the next start.
    const Glib::ustring partial = modelPath() + ".part";
    g_remove(partial.c_str());

    Gtk::Dialog dialog(M("MODELDOWNLOAD_TITLE"), parent, true);
    dialog.set_default_size(420, -1);
    dialog.add_button(M("GENERAL_CANCEL"), Gtk::RESPONSE_CANCEL);

    Gtk::Box* box = dialog.get_content_area();
    box->set_spacing(8);
    box->set_border_width(12);

    Gtk::Label caption(M("MODELDOWNLOAD_EXPLAIN"));
    caption.set_line_wrap(true);
    caption.set_xalign(0.f);
    box->pack_start(caption, Gtk::PACK_SHRINK);

    Gtk::ProgressBar progress;
    progress.set_show_text(true);
    progress.set_text(M("MODELDOWNLOAD_STARTING"));
    box->pack_start(progress, Gtk::PACK_SHRINK);

    box->show_all();

    Glib::Pid pid = 0;
    bool spawned = false;
    int exitStatus = -1;
    bool finished = false;

    std::vector<std::string> argv = {
        curl.raw(),
        "--location",            // release assets redirect to a CDN
        "--fail",                // an HTTP error page must not be saved as a model
        "--silent",
        "--show-error",
        "--output", partial.raw(),
        std::string(STEEP_INPAINT_MODEL_URL),
    };

    try {
        Glib::spawn_async(Glib::get_current_dir(), argv,
                          Glib::SPAWN_DO_NOT_REAP_CHILD | Glib::SPAWN_SEARCH_PATH,
                          sigc::slot<void>(), &pid);
        spawned = true;
    } catch (const Glib::Error& e) {
        Gtk::MessageDialog err(parent, M("MODELDOWNLOAD_FAILED"), false,
                               Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
        err.set_secondary_text(e.what());
        err.run();
        return false;
    }

    auto childWatch = Glib::signal_child_watch().connect(
        [&](Glib::Pid p, int status) {
            Glib::spawn_close_pid(p);
            exitStatus = status;
            finished = true;
            dialog.response(Gtk::RESPONSE_OK);
        }, pid);

    // curl is quiet, so progress comes from watching the file grow. That is
    // enough for a bar and avoids parsing curl's output format.
    auto tick = Glib::signal_timeout().connect(
        [&]() -> bool {
            double have = 0.0;

            try {
                const auto info = Gio::File::create_for_path(partial)->query_info(
                    G_FILE_ATTRIBUTE_STANDARD_SIZE);
                have = static_cast<double>(info->get_size());
            } catch (...) {
                return true;   // not created yet
            }

            progress.set_fraction(std::min(1.0, have / EXPECTED_BYTES));
            progress.set_text(Glib::ustring::compose(
                M("MODELDOWNLOAD_PROGRESS"), humanSize(have), humanSize(EXPECTED_BYTES)));
            return true;
        }, 250);

    const int response = dialog.run();

    tick.disconnect();

    if (!finished && spawned) {
        // Cancelled: stop the transfer and clear the partial file, so a later
        // attempt starts clean rather than resuming into a truncated file.
        childWatch.disconnect();
#ifdef _WIN32
        TerminateProcess(pid, 1);
#else
        ::kill(pid, SIGTERM);
#endif
        Glib::spawn_close_pid(pid);
        g_remove(partial.c_str());
        return false;
    }

    childWatch.disconnect();

    // 0 means a clean exit with status 0 on POSIX wait status and on Windows
    // alike, which avoids g_spawn_check_exit_status/g_spawn_check_wait_status
    // and their rename across glib versions.
    const bool curlOk = finished && exitStatus == 0;

    if (response == Gtk::RESPONSE_CANCEL || !curlOk) {
        g_remove(partial.c_str());

        if (!curlOk) {
            Gtk::MessageDialog err(parent, M("MODELDOWNLOAD_FAILED"), false,
                                   Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
            err.set_secondary_text(Glib::ustring::compose(
                M("MODELDOWNLOAD_MANUAL"), Glib::ustring(STEEP_INPAINT_MODEL_URL), modelPath()));
            err.run();
        }

        return false;
    }

    // Check what actually arrived. curl reports transport failures, but not a
    // truncated proxy response or a tampered file, and loading a corrupt model
    // fails in a much less obvious place than here.
    const Glib::ustring expected = STEEP_INPAINT_MODEL_SHA256;

    if (!expected.empty()) {
        progress.set_text(M("MODELDOWNLOAD_VERIFYING"));

        gchar* contents = nullptr;
        gsize length = 0;
        Glib::ustring actual;

        if (g_file_get_contents(partial.c_str(), &contents, &length, nullptr)) {
            actual = Glib::Checksum::compute_checksum(
                Glib::Checksum::CHECKSUM_SHA256,
                std::string(contents, length));
            g_free(contents);
        }

        if (actual != expected) {
            g_remove(partial.c_str());
            Gtk::MessageDialog err(parent, M("MODELDOWNLOAD_BAD_CHECKSUM"), false,
                                   Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
            err.run();
            return false;
        }
    }

    if (g_rename(partial.c_str(), modelPath().c_str()) != 0) {
        g_remove(partial.c_str());
        return false;
    }

#ifdef RT_AI_MASKING
    // Load it now, so the tools light up without a restart.
    if (!rtengine::getAIInpaintingEngine().init(modelPath().raw())) {
        Gtk::MessageDialog err(parent, M("MODELDOWNLOAD_BAD_MODEL"), false,
                               Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
        err.run();
        g_remove(modelPath().c_str());
        return false;
    }

    return true;
#else
    return false;
#endif
}
