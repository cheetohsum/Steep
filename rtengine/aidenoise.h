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
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <set>
#include <string>

#include <glibmm/ustring.h>

#include "imagefloat.h"
#include "procparams.h"
#include "rtgui/threadutils.h"

namespace rtengine
{

class AIDenoiseManager
{
public:
    static AIDenoiseManager& getInstance();

    bool isAvailable() const { return available_; }
    bool isDetecting() const { return detecting_; }
    void detect();
    void detect(const Glib::ustring& pythonPath, const Glib::ustring& scriptPath);
    void setDetectDoneCallback(std::function<void(bool)> cb) { detectDoneCb_ = cb; }

    void startDenoising(
        const Glib::ustring& rawPath,
        const procparams::AIDenoiseParams& params,
        const Glib::ustring& outputPath,
        std::function<void(double)> progressCb,
        std::function<void(bool, const Glib::ustring&)> doneCb,
        const Glib::ustring& inputTiffPath = "",
        int iso = 0
    );
    void cancel();

    bool isCacheValid(const Glib::ustring& rawPath, double iso) const;
    Imagefloat* getCachedResult() const;
    void setCachedResult(std::unique_ptr<Imagefloat> result,
                         const Glib::ustring& rawPath, double iso);
    void clearCache();

private:
    AIDenoiseManager();
    ~AIDenoiseManager();

    AIDenoiseManager(const AIDenoiseManager&) = delete;
    AIDenoiseManager& operator=(const AIDenoiseManager&) = delete;

    bool findPython();
    bool findScript();
    bool testRawRefinery();

    std::atomic<bool> available_;
    std::atomic<bool> detecting_;
    Glib::ustring pythonPath_;
    Glib::ustring scriptPath_;
    std::set<Glib::ustring> triedPythonPaths_;
    std::atomic<bool> running_;
    std::atomic<bool> cancelled_;
    std::function<void(bool)> detectDoneCb_;

#ifdef _WIN32
    void* childProcess_;  // HANDLE on Windows
#else
    int childPid_;
#endif

    // Result cache
    mutable MyMutex cacheMutex_;
    std::unique_ptr<Imagefloat> cachedResult_;
    Glib::ustring cachedRawPath_;
    double cachedIso_;
};

} // namespace rtengine
