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

#ifdef RT_AI_MASKING

#include <string>
#include <vector>
#include <memory>

namespace rtengine
{

/**
 * @brief AI Inpainting engine using LaMa ONNX model.
 *
 * LaMa (Large Mask Inpainting) takes an image and binary mask as input
 * and produces an inpainted image where masked regions are filled
 * with plausible content.
 *
 * Input: image (1,3,H,W) [0,1] float + mask (1,1,H,W) binary float
 * Output: inpainted image (1,3,H,W) [0,1] float
 */
class AIInpaintingEngine
{
public:
    AIInpaintingEngine();
    ~AIInpaintingEngine();

    /**
     * @brief Initialize the engine with a LaMa ONNX model file.
     * @param modelPath Absolute path to the .onnx model file
     * @return true if initialization succeeded
     */
    bool init(const std::string& modelPath);

    /**
     * @brief Accept a model now, build the session on a background thread.
     *
     * Creating the ONNX session for this model measures around 4.5 seconds,
     * and doing it inside rtengine::init() stalled every startup by that much
     * whether or not the user ever reached for Remove Object. The wait now
     * overlaps with getting the first photo on screen.
     *
     * isAvailable() is true as soon as this is called, so the tools can be
     * offered; inpaint() blocks until the session is actually ready.
     */
    void initDeferred(const std::string& modelPath);

    /**
     * @brief Check if the engine is ready for inference right now.
     *
     * Callers deciding whether to OFFER the tools want isAvailable() instead:
     * during a deferred load the model is on its way but this is still false.
     */
    bool isInitialized() const;

    /**
     * @brief Whether a model exists for this engine, loaded or still loading.
     */
    bool isAvailable() const;

    /**
     * @brief Run inpainting on an image region.
     *
     * @param imageR Red channel data (row-major, linear [0,65535])
     * @param imageG Green channel data
     * @param imageB Blue channel data
     * @param mask Soft mask, same dimensions: any value > 0.05 is filled by
     *        the model. The output is the model's full reconstruction — the
     *        caller blends it against the original through this mask.
     * @param width Image width
     * @param height Image height
     * @param outR Output red channel (pre-allocated, same size)
     * @param outG Output green channel
     * @param outB Output blue channel
     * @return true if inpainting succeeded
     */
    bool inpaint(const float* imageR, const float* imageG, const float* imageB,
                 const float* mask, int width, int height,
                 float* outR, float* outG, float* outB);

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};

/**
 * @brief Get the global singleton inpainting engine instance.
 */
AIInpaintingEngine& getAIInpaintingEngine();

} // namespace rtengine

#endif // RT_AI_MASKING
