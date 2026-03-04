/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2004-2010 Gabor Horvath <hgabor@rawtherapee.com>
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
#include "aidenoise.h"

#include <cstdio>
#include <cstdlib>
#include <thread>

#include <glibmm/fileutils.h>
#include <glibmm/miscutils.h>
#include <glibmm/spawn.h>

#include "imageformat.h"
#include "rtapp.h"
#include "settings.h"

namespace rtengine
{

AIDenoiseManager::AIDenoiseManager()
    : available_(false)
    , detecting_(false)
    , running_(false)
    , cancelled_(false)
#ifdef _WIN32
    , childProcess_(nullptr)
#else
    , childPid_(0)
#endif
    , cachedIso_(0)
{
}

AIDenoiseManager::~AIDenoiseManager()
{
}

AIDenoiseManager& AIDenoiseManager::getInstance()
{
    static AIDenoiseManager instance;
    return instance;
}

bool AIDenoiseManager::findPython()
{
    // Try well-known venv paths first, then PATH
    std::vector<std::string> pythonPaths;

#ifdef _WIN32
    // Check common Windows venv locations
    const char* home = getenv("USERPROFILE");
    if (home) {
        pythonPaths.push_back(std::string(home) + "\\rawrefinery_env\\Scripts\\python.exe");
    }
    pythonPaths.push_back("python.exe");
    pythonPaths.push_back("python3.exe");
#else
    // Check common Linux/macOS venv locations
    const char* home = getenv("HOME");
    if (home) {
        pythonPaths.push_back(std::string(home) + "/rawrefinery_env/bin/python3");
    }
    pythonPaths.push_back("/tmp/rawrefinery_env/bin/python3");
    // Try versioned interpreters first (RawRefinery needs 3.11+)
    pythonPaths.push_back("python3.13");
    pythonPaths.push_back("python3.12");
    pythonPaths.push_back("python3.11");
    pythonPaths.push_back("python3");
    pythonPaths.push_back("python");
#endif

    for (const auto& path : pythonPaths) {
        if (triedPythonPaths_.count(path)) continue;
        Glib::ustring cmd = Glib::ustring(path) + " --version";
        try {
            std::string stdout_str;
            int exit_status = 0;
            Glib::spawn_command_line_sync(cmd, &stdout_str, nullptr, &exit_status);
            if (exit_status == 0) {
                pythonPath_ = path;
                return true;
            }
        } catch (...) {
            continue;
        }
    }

    return false;
}

bool AIDenoiseManager::findScript()
{
    std::vector<std::string> scriptPaths;

    // Check relative to the RT data directory first (most reliable)
    const Glib::ustring& dataDir = App::get().argv0();
    if (!dataDir.empty()) {
        scriptPaths.push_back(Glib::build_filename(dataDir, "scripts", "rawrefinery_cli.py"));
    }

    // Common install paths
    scriptPaths.push_back("/usr/local/share/rawtherapee/scripts/rawrefinery_cli.py");
    scriptPaths.push_back("/usr/share/rawtherapee/scripts/rawrefinery_cli.py");

#ifdef _WIN32
    const char* home = getenv("USERPROFILE");
    if (home) {
        scriptPaths.push_back(std::string(home) + "\\rawrefinery_cli.py");
    }
#endif

    for (const auto& path : scriptPaths) {
        if (Glib::file_test(path, Glib::FILE_TEST_EXISTS)) {
            scriptPath_ = path;
            return true;
        }
    }

    return false;
}

bool AIDenoiseManager::testRawRefinery()
{
    if (pythonPath_.empty()) {
        return false;
    }

    Glib::ustring cmd = pythonPath_ + " -c \"import RawRefinery; print('ok')\"";
    try {
        std::string stdout_str;
        int exit_status = 0;
        Glib::spawn_command_line_sync(cmd, &stdout_str, nullptr, &exit_status);
        return exit_status == 0 && stdout_str.find("ok") != std::string::npos;
    } catch (...) {
        return false;
    }
}

void AIDenoiseManager::detect()
{
    available_ = false;
    detecting_ = true;

    // Run detection in a background thread to avoid blocking startup.
    // spawn_command_line_sync has no timeout and can hang if Python is
    // slow or loads heavy deps (CUDA, TensorFlow, etc.)
    std::thread([this]() {
        // Try each Python candidate until one has RawRefinery importable.
        // findPython picks the first working interpreter, but RawRefinery
        // requires 3.11+ so we may need to keep trying.
        bool found = false;
        while (findPython()) {
            if (testRawRefinery()) {
                found = true;
                break;
            }
            fprintf(stderr, "AI Denoise: RawRefinery not importable with %s, trying next...\n",
                    pythonPath_.c_str());
            // Remove this path so findPython skips it next iteration
            triedPythonPaths_.insert(pythonPath_);
            pythonPath_.clear();
        }

        if (!found) {
            fprintf(stderr, "AI Denoise: No Python with RawRefinery found. "
                    "Install with: python3.11 -m pip install rawrefinery\n");
            detecting_ = false;
            if (detectDoneCb_) detectDoneCb_(false);
            return;
        }

        if (!findScript()) {
            fprintf(stderr, "AI Denoise: CLI script not found, will use inline python\n");
        }

        available_ = true;
        detecting_ = false;
        fprintf(stderr, "AI Denoise: Ready (python=%s, script=%s)\n",
                pythonPath_.c_str(),
                scriptPath_.empty() ? "<inline>" : scriptPath_.c_str());
        if (detectDoneCb_) detectDoneCb_(true);
    }).detach();
}

void AIDenoiseManager::detect(const Glib::ustring& pythonPath,
                               const Glib::ustring& scriptPath)
{
    available_ = false;

    // Use user-specified paths if provided
    if (!scriptPath.empty()) {
        if (Glib::file_test(scriptPath, Glib::FILE_TEST_EXISTS)) {
            scriptPath_ = scriptPath;
            if (!pythonPath.empty()) {
                pythonPath_ = pythonPath;
            }
            available_ = true;
            return;
        }
    }

    if (!pythonPath.empty()) {
        pythonPath_ = pythonPath;
        if (testRawRefinery()) {
            available_ = true;
            return;
        }
    }

    // Fall back to auto-detection
    detect();
}

void AIDenoiseManager::startDenoising(
    const Glib::ustring& rawPath,
    const procparams::AIDenoiseParams& params,
    const Glib::ustring& outputPath,
    std::function<void(double)> progressCb,
    std::function<void(bool, const Glib::ustring&)> doneCb,
    const Glib::ustring& inputTiffPath,
    int iso)
{
    if (!available_ || running_) {
        if (doneCb) {
            doneCb(false, "AI Denoise not available or already running");
        }
        return;
    }

    running_ = true;
    cancelled_ = false;

    std::thread([this, rawPath, params, outputPath, progressCb, doneCb, inputTiffPath, iso]() {
        // Build command line: python <script> --input <raw> --output <tif> --iso-strength <N> [--gpu]
        std::vector<std::string> argv;

        argv.push_back(pythonPath_);

        if (!scriptPath_.empty()) {
            argv.push_back(scriptPath_);
        } else {
            // Inline: run a minimal Python command that uses RawRefinery
            // This is a fallback when the CLI script isn't found
            argv.push_back("-c");
            argv.push_back("import sys; sys.argv = ['rawrefinery_cli']; exec(open('/dev/null').read())");
            // Without a CLI script, we can't proceed properly
            running_ = false;
            if (doneCb) {
                doneCb(false, "CLI script not found");
            }
            return;
        }

        if (!inputTiffPath.empty()) {
            // Use RT's pre-demosaiced TIFF (correct color space)
            argv.push_back("--input-tiff");
            argv.push_back(inputTiffPath);
        } else {
            // Fallback: pass raw file directly (rawpy path)
            argv.push_back("--input");
            argv.push_back(rawPath);
        }

        argv.push_back("--output");
        argv.push_back(outputPath);
        argv.push_back("--iso-strength");
        argv.push_back(std::to_string(params.isoConditioning));

        if (iso > 0) {
            argv.push_back("--iso");
            argv.push_back(std::to_string(iso));
        }

        if (params.useGpu) {
            argv.push_back("--gpu");
        }

        try {
            std::string stdout_str, stderr_str;
            int exit_status = 0;

            // Build command string for spawn
            Glib::ustring cmd;
            for (size_t i = 0; i < argv.size(); ++i) {
                if (i > 0) cmd += " ";
                cmd += Glib::ustring("\"") + argv[i] + "\"";
            }

            fprintf(stderr, "AI Denoise: Running: %s\n", cmd.c_str());
            Glib::spawn_command_line_sync(cmd, &stdout_str, &stderr_str, &exit_status);
            fprintf(stderr, "AI Denoise: Process stderr:\n%s\n", stderr_str.c_str());

            running_ = false;

            if (cancelled_) {
                if (doneCb) {
                    doneCb(false, "Cancelled");
                }
                return;
            }

            if (exit_status == 0) {
                // Load the output TIFF into the cache
                std::unique_ptr<Imagefloat> result(new Imagefloat());
                // Our CLI outputs 32-bit float TIFF; must set sampleFormat
                // before loadTIFF since loadTIFF doesn't set it itself
                result->setSampleFormat(IIOSF_FLOAT32);
                int loadErr = result->loadTIFF(outputPath);
                if (loadErr == 0) {
                    fprintf(stderr, "AI Denoise: Loaded result %dx%d, about to cache\n",
                            result->getWidth(), result->getHeight());
                    fflush(stderr);
                    fprintf(stderr, "AI Denoise: doneCb is %s BEFORE cache\n",
                            doneCb ? "set" : "NULL");
                    fflush(stderr);
                    setCachedResult(std::move(result), rawPath, params.isoConditioning);
                    fprintf(stderr, "AI Denoise: cache set OK\n");
                    fflush(stderr);
                    if (doneCb) {
                        try {
                            doneCb(true, "");
                            fprintf(stderr, "AI Denoise: doneCb returned OK\n");
                            fflush(stderr);
                        } catch (const std::exception& ex) {
                            fprintf(stderr, "AI Denoise: doneCb threw: %s\n", ex.what());
                            fflush(stderr);
                        } catch (...) {
                            fprintf(stderr, "AI Denoise: doneCb threw unknown exception\n");
                            fflush(stderr);
                        }
                    }
                } else {
                    fprintf(stderr, "AI Denoise: Failed to load output TIFF (err=%d)\n", loadErr);
                    if (doneCb) {
                        doneCb(false, "Failed to load denoised result");
                    }
                }
            } else {
                if (doneCb) {
                    doneCb(false, Glib::ustring("Process exited with code ") +
                           std::to_string(exit_status) + ": " + stderr_str);
                }
            }
        } catch (const Glib::Error& e) {
            running_ = false;
            if (doneCb) {
                doneCb(false, e.what());
            }
        }
    }).detach();
}

void AIDenoiseManager::cancel()
{
    cancelled_ = true;
    // Platform-specific process termination would go here
}

bool AIDenoiseManager::isCacheValid(const Glib::ustring& rawPath, double iso) const
{
    MyMutex::MyLock lock(cacheMutex_);
    bool valid = cachedResult_ != nullptr
        && cachedRawPath_ == rawPath
        && cachedIso_ == iso;
    fprintf(stderr, "AI Denoise: isCacheValid: result=%p path_match=%d iso_match=%d (cached='%s' vs '%s', iso=%.1f vs %.1f) => %d\n",
            cachedResult_.get(),
            (int)(cachedRawPath_ == rawPath),
            (int)(cachedIso_ == iso),
            cachedRawPath_.c_str(), rawPath.c_str(),
            cachedIso_, iso,
            (int)valid);
    return valid;
}

Imagefloat* AIDenoiseManager::getCachedResult() const
{
    MyMutex::MyLock lock(cacheMutex_);
    return cachedResult_.get();
}

void AIDenoiseManager::setCachedResult(std::unique_ptr<Imagefloat> result,
                                        const Glib::ustring& rawPath, double iso)
{
    MyMutex::MyLock lock(cacheMutex_);
    cachedResult_ = std::move(result);
    cachedRawPath_ = rawPath;
    cachedIso_ = iso;
}

void AIDenoiseManager::clearCache()
{
    MyMutex::MyLock lock(cacheMutex_);
    cachedResult_.reset();
    cachedRawPath_.clear();
    cachedIso_ = 0;
}

} // namespace rtengine
