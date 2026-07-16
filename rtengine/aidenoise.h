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
// ONNX Runtime (with DirectML acceleration on Windows). There is no Python
// interpreter, external RawRefinery installation, or subprocess involved.
//
// The inference pipeline mirrors rawrefinery_cli.py:
//   1. Receive Steep's demosaiced image directly in memory
//   2. Tile to 256x256 with stride 64 (75% overlap)
//   3. Run model(rgb_tile, iso_cond) in bounded batches
//   4. Stream the tiles into a cosine-weighted output buffer
//   5. Remove position-dependent tile bias and preserve highlights
//
// The session is reused across invocations and lazily reset on GPU/CPU mode
// change. The model is shipped in Steep's data bundle, with the historical
// RawRefinery user-data location retained as a compatibility fallback.
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
        std::unique_ptr<Imagefloat> inputImage,
        std::function<void(double)> progressCb,
        std::function<void(bool, const Glib::ustring&)> doneCb,
        int iso = 0
    );
    void cancel();

    bool isCacheValid(const Glib::ustring& rawPath, double iso) const;
    std::shared_ptr<const Imagefloat> getCachedResult() const;
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
    std::shared_ptr<Imagefloat> cachedResult_;
    Glib::ustring cachedRawPath_;
    double cachedIso_;
};

} // namespace rtengine
