/*
 *  This file is part of RawTherapee.
 *
 *  RawTherapee is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */
#pragma once

#include <array>

#include "rtengine/procparams.h"

class Thumbnail;

enum class SteepAutoEditMode {
    Neutral,    // technical auto edit only
    Grade,      // auto edit + color grade
    GradeFilm,  // auto edit + film stock ("Film Lab")
    GradedFilm  // auto edit + color grade + film stock, harmonized
};

// How strongly a frame shows each trait, 0..1, judged independently.
//
// These replace a winner-takes-all label. The six scenes were never mutually
// exclusive -- a portrait can be shot at golden hour, a landscape can be a
// night scene -- but they were tested in a fixed order by an if/else chain, so
// whichever came first swallowed the frame whole and the rest of what the
// picture was got discarded. Scores let a frame be 0.6 of one thing and 0.4 of
// another, and let the look be blended to match.
struct AutoSceneScores {
    double portrait = 0.0;  // faces, present and central
    double lowSun = 0.0;    // warm light, high in the frame: golden hour
    double open = 0.0;      // sky and foliage: landscape
    double dark = 0.0;      // renders low key
    double urban = 0.0;     // dense edges, little foliage

    // The strongest trait and how far it leads. A frame with nothing above the
    // noise floor is simply neutral, and should be graded as one.
    double strongest() const
    {
        double best = portrait;
        best = lowSun > best ? lowSun : best;
        best = open > best ? open : best;
        best = dark > best ? dark : best;
        best = urban > best ? urban : best;
        return best;
    }
};

// Kept as the dominant trait, for logging, auto-cull and anything that wants
// one word for what a picture is. The grading now reads the scores.
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

    // Warm pixels binned by luminance, as a share of the whole frame. The
    // plain brightWarmFraction above counts warm pixels brighter than 0.55 on
    // the NEUTRAL render, where a real frame's median sits near 0.18 and so
    // almost nothing clears the bar -- which is why golden hour never fired.
    // Keeping the distribution lets the same question be re-asked at the
    // exposure Auto Edit actually committed.
    std::array<float, 32> warmLumaHistogram{};

    // Of the frame's warm pixels, the share sitting below its median
    // luminance. Low sun lights the bright end and leaves the shade cool; a
    // colour cast tints everything, shadows included. This is what separates
    // "the camera got the white balance wrong" from "the light really was
    // that colour", which the white balance readings alone cannot do.
    double warmShadowShare = 0.0;


    AutoSceneScores scores;
};

// Share of the frame that is warm and bright once `gain` (a display-space
// multiplier, i.e. 2^(EV/2.2)) has been applied to the neutral render.
double brightWarmFractionAtGain(const AutoGradeFeatures& features, double gain);

// The picture as Auto Edit's tonal stage actually rendered it.
//
// This exists because every look decision used to be made against the neutral
// render, which is far darker than the finished frame: measured across a real
// library, 92% of frames read a neutral median below 0.30, so a threshold like
// "medianLuma < 0.30 means this is a dark negative" fired on almost everything
// and stopped being a decision at all. These are the numbers the look stages
// must judge, because they describe the picture the look is going onto.
struct AutoEditRender {
    bool valid = false;
    double mid = 0.30;               // luma median as the tone curve receives it
    double p10 = 0.06;
    double p90 = 0.70;
    double p98 = 0.90;
    double range = 0.50;             // p90 - p10
    double shadowFraction = 0.0;     // share of luma below 0.16
    double highlightFraction = 0.0;  // share of luma above 0.84
    double clippedFraction = 0.0;    // channel samples at or above white
    double exposure = 0.0;           // EV the tonal stage committed
    double gain = 1.0;               // that exposure as a display-space multiplier

    // Where the tonal stage was aiming the median, and the most it was
    // allowed to expose. The verification pass needs both: it re-asks the
    // same question of the finished picture, and it must not answer it
    // differently just because the look stage ran in between.
    double midTarget = 0.42;
    double exposureCeiling = 2.75;
    double chimpMove = 0.0;          // EV the render-space correction applied
};

// Reads the picture with a neutral profile and describes it. Also used by
// auto-cull, which judges a frame on the same statistics.
AutoGradeFeatures analyzeSteepAutoGrade(Thumbnail& thumbnail);

const char* autoGradeSceneName(AutoGradeScene scene);
const char* autoEditModeLabel(SteepAutoEditMode mode);
const char* autoEditModeName(SteepAutoEditMode mode);

// Runs Auto Edit over named frames at startup and traces every decision, so a
// change to the tonal rules can be checked against real photographs without
// driving the browser by hand. Does nothing unless STEEP_AUTOEDIT_SELFTEST is
// set; see the definition for the variables it reads.
void runSteepAutoEditSelfTest();

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
