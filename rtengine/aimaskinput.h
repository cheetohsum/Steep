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

#include <cstdint>
#include <functional>
#include <memory>

namespace rtengine
{

class ColorTemp;
class ImageSource;
class Imagefloat;

namespace procparams
{
class ProcParams;
}

// Long side of the picture the AI mask models are shown, and of the cached
// class maps. The editor's own preview is only ~600 px across at fit zoom.
constexpr int AI_MASK_INPUT_DIMENSION = 2048;

// Fingerprint of everything that moves pixels around (coarse rotation and
// flips, fine rotation, distortion, perspective, lens profile, auto-fill):
// class maps computed under one geometry do not line up under another.
std::uint64_t aiMaskGeometryKey(const procparams::ProcParams& params);

/**
 * The picture segmentation should look at: the source at up to maxDim on its
 * long side, white balanced and in the working profile, with the same coarse
 * and geometric transform the preview gets (so the maps line up with local
 * adjustments), but none of the tonal or creative edits -- a film look or a
 * hard curve must not change what counts as sky. Exposure is normalised for
 * raw files, whose neutral rendering is far darker than anything the models
 * were trained on. Working-space linear RGB in [0, 65535].
 *
 * The source must already be demosaiced; call with the pipeline lock held.
 */
std::unique_ptr<Imagefloat> buildAIMaskInput(ImageSource* source,
                                             const procparams::ProcParams& params,
                                             const ColorTemp& wb,
                                             int maxDim,
                                             bool multiThread);

/**
 * Makes sure AIMaskCache holds this photo's class maps computed from
 * buildAIMaskInput(): a no-op when they are already cached for the same
 * geometry, otherwise builds the input and segments it on the calling
 * thread. Every consumer (editor, export) goes through here, so
 * they all see the same maps. False when no maps are available.
 */
bool ensureAIMasks(ImageSource* source, const procparams::ProcParams& params,
                   const ColorTemp& wb, bool multiThread, bool needDepth = false);

// Whether any local adjustment masks by depth (FOREGROUND/BACKGROUND_DEPTH),
// the only thing that pays for a depth estimate in the AI mask paths.
bool aiMasksNeedDepth(const procparams::ProcParams& params);

enum class AIMaskRequest {
    READY,        // the maps are in AIMaskCache now
    PENDING,      // computing on the AI mask worker; spots draw nothing meanwhile
    UNAVAILABLE   // no model, no input, or this photo already failed once
};

/**
 * The editor's non-blocking twin of ensureAIMasks(), for the preview
 * pipeline: segmentation, SAM refinement and the subject model take seconds
 * the first time per photo, and must not hold the pipeline lock that long.
 *
 * Builds the input on the calling thread (cheap, needs the source: call with
 * the pipeline lock held) and hands the rest to one persistent worker thread
 * -- not a fresh thread per call, since MinGW's libgomp leaks an OpenMP team
 * for every thread that ever opened one. Until the maps land, AIMaskCache
 * hands out an all-zero placeholder for this photo (see
 * AIMaskCache::restoreOrBeginPending): a pass that needs them draws AI-masked
 * spots as editing nothing, and never computes them a second time.
 *
 * @p onReady, when set, runs on the worker thread once the computation
 * finishes (or is superseded), so the owner can start a pass that repaints
 * with the maps; it replaces any earlier callback of the same owner for this
 * photo. Call cancelAIMaskRequests(owner) before the owner goes away.
 */
AIMaskRequest requestAIMasks(ImageSource* source, const procparams::ProcParams& params,
                             const ColorTemp& wb, bool multiThread,
                             const void* owner, std::function<void()> onReady, bool needDepth = false);

/** Drops every callback @p owner registered. When this returns, none of
 *  them is running and none will run. */
void cancelAIMaskRequests(const void* owner);

}
