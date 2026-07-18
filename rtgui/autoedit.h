/*
 *  This file is part of RawTherapee.
 *
 *  RawTherapee is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */
#pragma once

#include "rtengine/procparams.h"

class Thumbnail;

enum class SteepAutoEditMode {
    Neutral,
    Grade,
    GradeFilm
};

// Produces the same scene-analyzed profile used by the file-browser Auto Edit
// actions while preserving the source image's framing and alignment.
void buildSteepAutoEditParams(
    Thumbnail& thumbnail,
    SteepAutoEditMode mode,
    const rtengine::procparams::ProcParams& source,
    rtengine::procparams::ProcParams& result);
