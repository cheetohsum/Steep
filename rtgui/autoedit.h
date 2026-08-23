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
    Neutral,    // technical auto edit only
    Grade,      // auto edit + color grade
    GradeFilm,  // auto edit + film stock ("Film Lab")
    GradedFilm  // auto edit + color grade + film stock, harmonized
};

enum class AutoGradeScene {
    Neutral,
    Portrait,
    GoldenHour,
    Landscape,
    Night,
    Urban
};

// What Auto Edit read off the picture before it decided anything.
struct AutoGradeFeatures {
    bool valid = false;
    AutoGradeScene scene = AutoGradeScene::Neutral;
    double medianLuma = 0.5;
    double p10 = 0.10;          // luma percentiles for curve anchor placement
    double p90 = 0.60;
    double p98 = 0.88;
    double dynamicRange = 0.5;
    double shadowFraction = 0.0;
    double highlightFraction = 0.0;
    double saturation = 0.0;
    double warmFraction = 0.0;
    double brightWarmFraction = 0.0;
    double coolFraction = 0.0;
    double skinFraction = 0.0;
    double skinSaturation = 0.0;
    double centerSkinFraction = 0.0;
    double skyFraction = 0.0;
    double foliageFraction = 0.0;
    double edgeDensity = 0.0;
    double strongEdgeFraction = 0.0; // share of neighbor gradients > 0.09 (real edges)
    double clippedFraction = 0.0;    // share of pixels with luma > 0.985 (true clips)
    unsigned iso = 100;
};

// Reads the picture with a neutral profile and describes it. Also used by
// auto-cull, which judges a frame on the same statistics.
AutoGradeFeatures analyzeSteepAutoGrade(Thumbnail& thumbnail);

const char* autoGradeSceneName(AutoGradeScene scene);
const char* autoEditModeLabel(SteepAutoEditMode mode);
const char* autoEditModeName(SteepAutoEditMode mode);

// Produces the same scene-analyzed profile used by the file-browser Auto Edit
// actions while preserving the source image's framing and alignment.
void buildSteepAutoEditParams(
    Thumbnail& thumbnail,
    SteepAutoEditMode mode,
    const rtengine::procparams::ProcParams& source,
    rtengine::procparams::ProcParams& result);

// Same, but hands back what the analysis saw, for callers that report on it.
AutoGradeFeatures buildSteepAutoEditParamsFeatures(
    Thumbnail& thumbnail,
    SteepAutoEditMode mode,
    const rtengine::procparams::ProcParams& source,
    rtengine::procparams::ProcParams& result);
