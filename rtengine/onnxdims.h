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

#include <string>

namespace rtengine
{

/**
 * Lets a fully convolutional ONNX model that was exported at one fixed image
 * size run at any size.
 *
 * Such an export (the common LaMa file declares 512x512) computes every
 * internal shape from its input; only the declared height and width of its
 * image inputs and outputs stop the runtime from accepting other sizes. This
 * rewrites those declarations as free dimensions and drops the recorded
 * intermediate shapes, which would now be stale. The weights are untouched:
 * it is the same model, to the bit, at the size it was exported for.
 *
 * `model` is the serialized file. Returns true with the rewritten file in
 * `out` when at least one fixed dimension was freed; false (and `out`
 * untouched) when the model already takes any size or is not understood.
 *
 * Same operation as tools/make_lama_dynamic.py, done at load time so that a
 * model file obtained before free-size inference existed needs no
 * replacing.
 */
bool onnxFreeImageDimensions(const std::string& model, std::string& out);

}
