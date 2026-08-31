/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2024
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

#include <cairo.h>

#include "options.h"

namespace rtengine { class IImagefloat; }

void applyWatermark(rtengine::IImagefloat* img, const WatermarkOptions& opts);

/**
 * @brief Decode the watermark logo, cached by path and modification time.
 * @return a new surface the caller owns (cairo_surface_destroy), or nullptr
 *         when there is no usable image. Safe to call from any thread.
 */
cairo_surface_t* createWatermarkLogoSurface(const Glib::ustring& path);

/// True when the watermark would draw something — text, a logo, or both.
bool watermarkHasContent(const WatermarkOptions& opts);

/// Logo size in pixels for a photo whose short edge is @p shortEdge, keeping
/// the source aspect ratio. Returns false when there is no logo to draw.
bool watermarkLogoSize(const WatermarkOptions& opts, cairo_surface_t* logo,
                       int shortEdge, double& outW, double& outH);
