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
#include <string>

#include <glibmm/ustring.h>

#include "imagefloat.h"
#include "procparams.h"
#include "rtgui/threadutils.h"

namespace rtengine
{

// AIDenoiseManager runs RawRefinery's TreeNet denoise model natively via
// ONNX Runtime (with DirectML acceleration on Windows). It replaces the
// previous Python subprocess pipeline — no Python interpreter, no
// `pip install rawrefinery`, no popup window.
//
// The inference pipeline mirrors rawrefinery_cli.py:
//   1. Load 32-bit float TIFF (RT-exported demosaiced image)
//   2. Tile to 256x256 with stride 64 (75% overlap)
//   3. Run model(rgb_tile, iso_cond) per tile via ONNX Runtime
//   4. Cosine-weighted stitch with bias correction (eliminates tile grid)
//   5. Highlight preservation (blend back original > 1.0)
//   6. Save 32-bit float TIFF for RT to consume
//
// The session is reused across invocations and lazily reset on GPU/CPU mode
// change. The model file (Tree Net Denoise / ShadowWeightedL1.onnx) is
// auto-located at LOCALAPPDATA\RawRefinery\RawRefinery\ on Windows or the
// platform-equivalent user data dir. If missing, it is downloaded from the
// upstream RawRefinery release.
class AIDenoiseManager
{
public:
    static AIDenoiseManager& getInstance();

    bool isAvailable() const { return available_; }
    bool isDetecting() const { return detecting_; }
    void detect();
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

    // Find the model file on disk; empty string if not found.
    Glib::ustring findModelPath() const;

    // PIMPL: ONNX Runtime types are kept out of the header so callers don't
    // need to include onnxruntime headers.
    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::atomic<bool> available_;
    std::atomic<bool> detecting_;
    std::atomic<bool> running_;
    std::atomic<bool> cancelled_;
    std::function<void(bool)> detectDoneCb_;

    // Result cache
    mutable MyMutex cacheMutex_;
    std::unique_ptr<Imagefloat> cachedResult_;
    Glib::ustring cachedRawPath_;
    double cachedIso_;
};

} // namespace rtengine
