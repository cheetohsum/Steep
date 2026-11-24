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
#include <memory>
#include <glibmm/ustring.h>
#include "smartrepair.h"

namespace rtengine
{

/**
 * High-quality object removal: RORem, an SDXL-inpainting model trained to
 * take things out of photographs, run locally through ONNX Runtime.
 *
 * The convolutional fill (LaMa) extends nearby texture and holds lines only
 * across a narrow hole; over a wide one it averages what it cannot decide
 * and leaves a blur. A diffusion model commits to one plausible scene -- the
 * wall meets the pavement, the kerb continues -- which is what a wide hole
 * needs. It works on a reduced view (at most 1024 px), so the smart repair
 * uses it for the structure of the fill and still takes fine texture from
 * the convolutional model at full resolution.
 *
 * Optional: three model files (see tools/export_rorem_model.py), about 5.5 GB,
 * and a GPU. Without them available() is false and nothing changes.
 */
class AIRemoverEngine final
{
public:
    static AIRemoverEngine& getInstance();

    AIRemoverEngine(const AIRemoverEngine&) = delete;
    AIRemoverEngine& operator=(const AIRemoverEngine&) = delete;

    /// Directory holding rorem_unet.onnx (+ .data), rorem_vae_encoder.onnx
    /// and rorem_vae_decoder.onnx. Empty turns the engine off (Preferences >
    /// AI models); a loaded model is released when the directory changes.
    void setModelDirectory(const Glib::ustring& directory);
    bool available() const;

    /// What the system must be able to commit before a fill is started, in
    /// bytes, given whether the denoiser is already in memory.
    static unsigned long long memoryNeeded(bool denoiserLoaded);

    /// Longest side the model works at.
    static constexpr int maxSide = 1024;

    /**
     * Rebuild the masked part of a region.
     * Planes are linear RGB in sRGB primaries, 0..65535 (same contract as
     * AIInpaintingEngine::inpaint); mask is 0..1, anything above 0.05 is
     * regenerated. The output is the whole region as the model sees it: the
     * caller keeps the original outside the mask.
     * Blocking and slow (seconds on a GPU); the same seed gives the same fill.
     * Returns false when the models are missing or inference failed -- the
     * caller then falls back to the convolutional fill.
     */
    bool fill(const float* r, const float* g, const float* b, const float* mask, int w, int h,
              float* outR, float* outG, float* outB, std::uint32_t seed, const SmartRepairJob& job = {});

private:
    AIRemoverEngine();
    ~AIRemoverEngine();
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace rtengine
