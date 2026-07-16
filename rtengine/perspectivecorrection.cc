/* -*- C++ -*-
 *
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2019 Alberto Griggio <alberto.griggio@gmail.com>
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
 *  along with RawTherapee.  If not, see <http://www.gnu.org/licenses/>.
 */

// taken from darktable (src/iop/ashift.c)
/*
  This file is part of darktable,
  copyright (c) 2016 Ulrich Pegelow.

  darktable is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  darktable is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
// Inspiration to this module comes from the program ShiftN (http://www.shiftn.de) by
// Marcus Hebel.

// Thanks to Marcus for his support when implementing part of the ShiftN functionality
// to darktable.


#include "perspectivecorrection.h"
#include "improcfun.h"
#include "procparams.h"
#include "rt_math.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string.h>
#include <math.h>
#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <utility>
#include <vector>

#include "rtgui/threadutils.h"
#include "colortemp.h"
#include "imagefloat.h"
#include "settings.h"

namespace rtengine { extern const Settings *settings; }

#define _(msg) (msg)
#define dt_control_log(msg) \
    if (settings->verbose) { \
        printf("%s\n", msg);       \
        fflush(stdout);            \
    }


namespace rtengine {

namespace {

inline int mat3inv(float *const dst, const float *const src)
{
    std::array<std::array<float, 3>, 3> tmpsrc;
    std::array<std::array<float, 3>, 3> tmpdst;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            tmpsrc[i][j] = src[3 * i + j];
        }
    }
    if (invertMatrix(tmpsrc, tmpdst)) {
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                dst[3 * i + j] = tmpdst[i][j];
            }
        }
        return 0;
    } else {
        return 1;
    }
}


// the darktable ashift iop (adapted to RT), which does most of the work
#include "ashift_dt.c"


} // namespace


namespace {

/*
std::vector<Coord2D> get_corners(int w, int h)
{
    int x1 = 0, y1 = 0;
    int x2 = w, y2 = h;

    std::vector<Coord2D> corners = {
        Coord2D(x1, y1),
        Coord2D(x1, y2),
        Coord2D(x2, y2),
        Coord2D(x2, y1)
    };
    return corners;
}
*/

void init_dt_structures(dt_iop_ashift_params_t *p, dt_iop_ashift_gui_data_t *g,
                        const procparams::PerspectiveParams *params)
{
    dt_iop_ashift_params_t dp = {
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        DEFAULT_F_LENGTH,
        1.f,
        0.0f,
        1.0f,
        ASHIFT_MODE_SPECIFIC,
        0,
        ASHIFT_CROP_OFF,
        0.0f,
        1.0f,
        0.0f,
        1.0f,
        0.0f,
        0.0f
    };
    *p = dp;

    g->buf = NULL;
    g->buf_width = 0;
    g->buf_height = 0;
    g->buf_x_off = 0;
    g->buf_y_off = 0;
    g->buf_scale = 1.0f;
    g->buf_hash = 0;
    g->isflipped = 0;
    g->lastfit = ASHIFT_FIT_NONE;
    g->fitting = 0;
    g->lines = NULL;
    g->lines_count =0;
    g->horizontal_count = 0;
    g->vertical_count = 0;
    g->grid_hash = 0;
    g->lines_hash = 0;
    g->rotation_range = ROTATION_RANGE_SOFT;
    g->lensshift_v_range = LENSSHIFT_RANGE_SOFT;
    g->lensshift_h_range = LENSSHIFT_RANGE_SOFT;
    g->shear_range = SHEAR_RANGE_SOFT;
    g->camera_pitch_range = CAMERA_ANGLE_RANGE_SOFT;
    g->camera_yaw_range = CAMERA_ANGLE_RANGE_SOFT;
    g->lines_suppressed = 0;
    g->lines_version = 0;
    g->show_guides = 0;
    g->isselecting = 0;
    g->isdeselecting = 0;
    g->isbounding = ASHIFT_BOUNDING_OFF;
    g->near_delta = 0;
    g->selecting_lines_version = 0;
    g->points = NULL;
    g->points_idx = NULL;
    g->points_lines_count = 0;
    g->points_version = 0;
    g->jobcode = ASHIFT_JOBCODE_NONE;
    g->jobparams = 0;
    g->adjust_crop = FALSE;
    g->lastx = g->lasty = -1.0f;
    g->crop_cx = g->crop_cy = 1.0f;

    if (params) {
        p->rotation = params->camera_roll;
        p->lensshift_v = params->camera_shift_vert;
        p->lensshift_h = params->camera_shift_horiz;
        p->f_length = params->camera_focal_length;
        p->crop_factor = params->camera_crop_factor;
        p->camera_pitch = params->camera_pitch;
        p->camera_yaw = params->camera_yaw;
    }
}


/*
void get_view_size(int w, int h, const procparams::PerspectiveParams &params, double &cw, double &ch)
{
    double min_x = RT_INFINITY, max_x = -RT_INFINITY;
    double min_y = RT_INFINITY, max_y = -RT_INFINITY;

    auto corners = get_corners(w, h);

    float homo[3][3];
    homography((float *)homo, params.angle, params.vertical / 100.0, -params.horizontal / 100.0, params.shear / 100.0, params.flength * params.cropfactor, 100.f, params.aspect, w, h, ASHIFT_HOMOGRAPH_FORWARD);
    
    for (auto &c : corners) {
        float pin[3] = { float(c.x), float(c.y), 1.f };
        float pout[3];
        mat3mulv(pout, (float *)homo, pin);
        double x = pout[0] / pout[2];
        double y = pout[1] / pout[2];
        min_x = min(min_x, x);
        max_x = max(max_x, x);
        min_y = min(min_y, y);
        max_y = max(max_y, y);
    }

    cw = max_x - min_x;
    ch = max_y - min_y;
}    
*/

/**
 * Allocates a new array and populates it with ashift lines corresponding to the
 * provided control lines.
 */
std::unique_ptr<dt_iop_ashift_line_t[]> toAshiftLines(const std::vector<ControlLine> *lines)
{
    std::unique_ptr<dt_iop_ashift_line_t[]> retval(new dt_iop_ashift_line_t[lines->size()]);

    for (size_t i = 0; i < lines->size(); i++) {
        const float x1 = (*lines)[i].x1;
        const float y1 = (*lines)[i].y1;
        const float x2 = (*lines)[i].x2;
        const float y2 = (*lines)[i].y2;
        retval[i].p1[0] = x1;
        retval[i].p1[1] = y1;
        retval[i].p1[2] = 1.0f;
        retval[i].p2[0] = x2;
        retval[i].p2[1] = y2;
        retval[i].p2[2] = 1.0f;
        retval[i].length = sqrt((x1 - x2) * (x1 - x2) + (y1 - y2) * (y1 - y2));
        retval[i].width = 1.0f;
        retval[i].weight = retval[i].length;
        if ((*lines)[i].type == ControlLine::HORIZONTAL) {
            retval[i].type = ASHIFT_LINE_HORIZONTAL_SELECTED;
        } else if ((*lines)[i].type == ControlLine::VERTICAL) {
            retval[i].type = ASHIFT_LINE_VERTICAL_SELECTED;
        } else {
            retval[i].type = ASHIFT_LINE_IRRELEVANT;
        }
    }

    return retval;
}

void cleanupDtStructures(dt_iop_ashift_gui_data_t *g)
{
    if (g->lines) {
        free(g->lines);
        g->lines = nullptr;
    }
    if (g->points) {
        free(g->points);
        g->points = nullptr;
    }
    if (g->points_idx) {
        free(g->points_idx);
        g->points_idx = nullptr;
    }
    if (g->buf) {
        free(g->buf);
        g->buf = nullptr;
    }
}

bool prepareAnalysisBuffer(
    ImageSource *src,
    const procparams::ProcParams *pparams,
    int transform,
    int full_width,
    int full_height,
    bool apply_rotation,
    dt_iop_ashift_gui_data_t *g)
{
    const int skip = std::max(static_cast<int>(std::max(full_width, full_height) / 900.f + 0.5f), 1);
    PreviewProps pp(0, 0, full_width, full_height, skip);
    int width = 0;
    int height = 0;
    src->getSize(pp, width, height);
    std::unique_ptr<Imagefloat> img(new Imagefloat(width, height));

    ProcParams neutral;
    neutral.raw.bayersensor.method = RAWParams::BayerSensor::getMethodString(RAWParams::BayerSensor::Method::FAST);
    neutral.raw.xtranssensor.method = RAWParams::XTransSensor::getMethodString(RAWParams::XTransSensor::Method::FAST);
    neutral.icm.outputProfile = ColorManagementParams::NoICMString;
    src->getImage(src->getWB(), transform, img.get(), pp, neutral.toneCurve, neutral.raw);
    src->convertColorSpace(img.get(), pparams->icm, src->getWB());

    neutral.commonTrans.autofill = false;
    if (apply_rotation) {
        neutral.rotate = pparams->rotate;
    }
    neutral.distortion = pparams->distortion;
    neutral.distortion.defish = pparams->distortion.defish;
    neutral.distortion.focal_length = pparams->distortion.focal_length;
    neutral.perspective.camera_focal_length = pparams->perspective.camera_focal_length;
    neutral.perspective.camera_crop_factor = pparams->perspective.camera_crop_factor;
    neutral.perspective.method = pparams->perspective.method;
    neutral.lensProf = pparams->lensProf;

    ImProcFunctions ipf(&neutral, true);
    if (ipf.needsTransform(width, height, src->getRotateDegree(), src->getMetaData())) {
        std::unique_ptr<Imagefloat> transformed(new Imagefloat(width, height));
        ipf.transform(
            img.get(), transformed.get(), 0, 0, 0, 0, width, height, width, height,
            src->getMetaData(), src->getRotateDegree(), false);
        img = std::move(transformed);
    }

    g->buf = static_cast<float *>(malloc(sizeof(float) * width * height * 4));
    if (!g->buf) {
        return false;
    }
    g->buf_width = width;
    g->buf_height = height;

    img->normalizeFloatTo1();

#ifdef _OPENMP
#   pragma omp parallel for
#endif
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int i = (y * width + x) * 4;
            g->buf[i] = img->r(y, x);
            g->buf[i + 1] = img->g(y, x);
            g->buf[i + 2] = img->b(y, x);
            g->buf[i + 3] = 1.f;
        }
    }

    return true;
}

struct LevelSample
{
    double angle;
    double length;
};

struct LevelEvidence
{
    double angle = 0.0;
    double confidence = 0.0;
    double total_length = 0.0;
    double longest_line = 0.0;
    double deviation = 0.0;
    int line_count = 0;
    bool valid = false;
};

double weightedMedian(std::vector<LevelSample> samples)
{
    if (samples.empty()) {
        return 0.0;
    }

    std::sort(samples.begin(), samples.end(), [](const LevelSample& lhs, const LevelSample& rhs) {
        return lhs.angle < rhs.angle;
    });

    double total_weight = 0.0;
    for (const auto& sample : samples) {
        total_weight += sample.length;
    }

    const double midpoint = total_weight * 0.5;
    double accumulated = 0.0;
    for (const auto& sample : samples) {
        accumulated += sample.length;
        if (accumulated >= midpoint) {
            return sample.angle;
        }
    }

    return samples.back().angle;
}

double weightedDeviation(const std::vector<LevelSample>& samples, double median)
{
    std::vector<LevelSample> deviations;
    deviations.reserve(samples.size());
    for (const auto& sample : samples) {
        deviations.push_back({std::abs(sample.angle - median), sample.length});
    }
    return weightedMedian(std::move(deviations));
}

bool isLikelyImageBorder(const dt_iop_ashift_line_t& line, bool vertical, int width, int height)
{
    const double x_margin = std::max(2.0, width * 0.015);
    const double y_margin = std::max(2.0, height * 0.015);

    if (vertical) {
        return (line.p1[0] <= x_margin && line.p2[0] <= x_margin)
            || (line.p1[0] >= width - x_margin && line.p2[0] >= width - x_margin);
    }

    return (line.p1[1] <= y_margin && line.p2[1] <= y_margin)
        || (line.p1[1] >= height - y_margin && line.p2[1] >= height - y_margin);
}

double lineLevelAngle(const dt_iop_ashift_line_t& line, bool vertical)
{
    double angle = std::atan2(line.p2[1] - line.p1[1], line.p2[0] - line.p1[0]) * 180.0 / RT_PI;
    while (angle <= -90.0) {
        angle += 180.0;
    }
    while (angle > 90.0) {
        angle -= 180.0;
    }

    if (vertical) {
        angle += angle < 0.0 ? 90.0 : -90.0;
    }
    return angle;
}

LevelEvidence analyzeLevelEvidence(const dt_iop_ashift_gui_data_t& g, bool vertical)
{
    LevelEvidence result;
    const double extent = vertical ? g.lines_in_height : g.lines_in_width;
    if (!g.lines || g.lines_count <= 0 || extent <= 0.0) {
        return result;
    }

    int selected_count = 0;
    for (int i = 0; i < g.lines_count; ++i) {
        const auto& line = g.lines[i];
        const bool relevant = (line.type & ASHIFT_LINE_RELEVANT) != 0;
        const bool line_vertical = (line.type & ASHIFT_LINE_DIRVERT) != 0;
        if (relevant && line_vertical == vertical && (line.type & ASHIFT_LINE_SELECTED)
                && !isLikelyImageBorder(line, vertical, g.lines_in_width, g.lines_in_height)) {
            ++selected_count;
        }
    }

    // Darktable's RANSAC deliberately clears selection when only one or two
    // segments exist. In that case, retain the detector's relevant lines and
    // let the weighted median and dominance gates below make the decision.
    const bool selected_only = selected_count >= 2;
    std::vector<LevelSample> samples;
    for (int i = 0; i < g.lines_count; ++i) {
        const auto& line = g.lines[i];
        const bool relevant = (line.type & ASHIFT_LINE_RELEVANT) != 0;
        const bool line_vertical = (line.type & ASHIFT_LINE_DIRVERT) != 0;
        const bool selected = (line.type & ASHIFT_LINE_SELECTED) != 0;
        if (!relevant || line_vertical != vertical || (selected_only && !selected)
                || isLikelyImageBorder(line, vertical, g.lines_in_width, g.lines_in_height)) {
            continue;
        }
        samples.push_back({lineLevelAngle(line, vertical), std::max(1.0f, line.length)});
    }

    if (samples.empty()) {
        return result;
    }

    double raw_length = 0.0;
    for (const auto& sample : samples) {
        raw_length += sample.length;
    }

    double median = weightedMedian(samples);
    double deviation = weightedDeviation(samples, median);
    const double trim_radius = std::min(3.5, std::max(0.75, deviation * 2.75));

    std::vector<LevelSample> inliers;
    inliers.reserve(samples.size());
    for (const auto& sample : samples) {
        if (std::abs(sample.angle - median) <= trim_radius) {
            inliers.push_back(sample);
        }
    }
    if (inliers.empty()) {
        return result;
    }

    median = weightedMedian(inliers);
    deviation = weightedDeviation(inliers, median);
    for (const auto& sample : inliers) {
        result.total_length += sample.length;
        result.longest_line = std::max(result.longest_line, sample.length);
    }
    result.angle = median;
    result.deviation = deviation;
    result.line_count = static_cast<int>(inliers.size());

    const double coverage = result.total_length / extent;
    const double coverage_score = std::min(1.0, coverage);
    const double count_score = std::min(1.0, result.line_count / 6.0);
    const double agreement_score = std::max(0.0, 1.0 - deviation / 2.5);
    const double dominance = raw_length > 0.0 ? result.total_length / raw_length : 0.0;
    result.confidence = 0.38 * coverage_score
        + 0.18 * count_score
        + 0.30 * agreement_score
        + 0.14 * dominance;
    if (result.line_count == 1) {
        result.confidence *= 0.86;
    }

    const bool enough_geometry = result.line_count >= 2
        || (!vertical && result.line_count == 1 && result.longest_line >= extent * 0.55);
    result.valid = enough_geometry
        && coverage >= 0.28
        && dominance >= 0.60
        && deviation <= 2.5
        && std::abs(result.angle) <= 15.0
        && result.confidence >= 0.52;
    return result;
}

} // namespace


PerspectiveCorrection::Params PerspectiveCorrection::autocompute(ImageSource *src, bool corr_pitch, bool corr_yaw, const procparams::ProcParams *pparams, const FramesMetaData *metadata, const std::vector<ControlLine> *control_lines)
{
    auto pcp = procparams::PerspectiveParams(pparams->perspective);
    procparams::PerspectiveParams dflt;
    /*
    pcp.horizontal = dflt.horizontal;
    pcp.vertical = dflt.vertical;
    pcp.angle = dflt.angle;
    pcp.shear = dflt.shear;
    */
    pcp.camera_pitch = dflt.camera_pitch;
    pcp.camera_roll = dflt.camera_roll;
    pcp.camera_yaw = dflt.camera_yaw;
    
    dt_iop_ashift_params_t p;
    dt_iop_ashift_gui_data_t g;
    init_dt_structures(&p, &g, &pparams->perspective);
    dt_iop_module_t module;
    module.gui_data = &g;
    module.is_raw = src->isRAW();

    int tr = getCoarseBitMask(pparams->coarse);
    int fw, fh;
    src->getFullSize(fw, fh, tr);
    bool analysis_buffer_ready = true;
    if (control_lines == nullptr) {
        analysis_buffer_ready = prepareAnalysisBuffer(src, pparams, tr, fw, fh, true, &g);
    }

    dt_iop_ashift_fitaxis_t fitaxis = ASHIFT_FIT_NONE;
    if (corr_pitch && corr_yaw) {
        fitaxis = ASHIFT_FIT_BOTH_SHEAR;
    } else if (corr_pitch) {
        fitaxis = ASHIFT_FIT_VERTICALLY;
    } else if (corr_yaw) {
        fitaxis = ASHIFT_FIT_HORIZONTALLY;
    }

    // reset the pseudo-random seed for repeatability -- ashift_dt uses rand()
    // internally!
    srand(1);
    
    bool res;
    if (control_lines == nullptr) {
        res = analysis_buffer_ready
            && do_get_structure(&module, &p, ASHIFT_ENHANCE_EDGES)
            && do_fit(&module, &p, fitaxis);
    } else {
        std::unique_ptr<dt_iop_ashift_line_t[]> ashift_lines = toAshiftLines(control_lines);
        dt_iop_ashift_gui_data_t *g = module.gui_data;
        g->lines_count = control_lines->size();
        g->lines = ashift_lines.get();
        g->lines_in_height = fh;
        g->lines_in_width = fw;
        update_lines_count(g->lines, g->lines_count, &(g->vertical_count), &(g->horizontal_count));
        res = do_fit(&module, &p, fitaxis, 2);
        g->lines = nullptr;
    }
    Params retval = {
        .angle = p.rotation,
        .pitch = p.camera_pitch,
        .yaw = p.camera_yaw
    };

    cleanupDtStructures(&g);

    if (!res) {
        retval.angle = pparams->perspective.camera_roll;
        retval.pitch = pparams->perspective.camera_pitch;
        retval.yaw = pparams->perspective.camera_yaw;
    }
    return retval;
}

PerspectiveCorrection::AutoLevelResult PerspectiveCorrection::autoLevel(
    ImageSource *src,
    const procparams::ProcParams *pparams)
{
    AutoLevelResult result;
    if (!src || !pparams) {
        return result;
    }

    dt_iop_ashift_params_t params;
    dt_iop_ashift_gui_data_t gui;
    init_dt_structures(&params, &gui, nullptr);
    dt_iop_module_t module;
    module.gui_data = &gui;
    module.is_raw = src->isRAW();

    const int transform = getCoarseBitMask(pparams->coarse);
    int full_width = 0;
    int full_height = 0;
    src->getFullSize(full_width, full_height, transform);

    const bool prepared = prepareAnalysisBuffer(
        src, pparams, transform, full_width, full_height, false, &gui);
    if (prepared) {
        // Keep ashift's RANSAC selection deterministic across repeated runs.
        srand(1);
        if (do_get_structure(&module, &params, ASHIFT_ENHANCE_EDGES)) {
            const LevelEvidence horizontal = analyzeLevelEvidence(gui, false);
            const LevelEvidence vertical = analyzeLevelEvidence(gui, true);

            if (horizontal.valid && vertical.valid) {
                const double disagreement = std::abs(horizontal.angle - vertical.angle);
                if (disagreement <= 1.25) {
                    const double horizontal_weight = horizontal.confidence * 1.08;
                    const double vertical_weight = vertical.confidence;
                    result.angle = (horizontal.angle * horizontal_weight + vertical.angle * vertical_weight)
                        / (horizontal_weight + vertical_weight);
                    result.confidence = std::min(
                        1.0,
                        (horizontal.confidence + vertical.confidence) * 0.5
                            + 0.08 * (1.0 - disagreement / 1.25));
                    result.horizontal_lines = horizontal.line_count;
                    result.vertical_lines = vertical.line_count;
                    result.success = true;
                } else if (horizontal.confidence + 0.06 - vertical.confidence >= 0.12) {
                    result.angle = horizontal.angle;
                    result.confidence = horizontal.confidence;
                    result.horizontal_lines = horizontal.line_count;
                    result.success = true;
                } else if (vertical.confidence - (horizontal.confidence + 0.06) >= 0.16) {
                    result.angle = vertical.angle;
                    result.confidence = vertical.confidence;
                    result.vertical_lines = vertical.line_count;
                    result.success = true;
                }
            } else if (horizontal.valid) {
                result.angle = horizontal.angle;
                result.confidence = horizontal.confidence;
                result.horizontal_lines = horizontal.line_count;
                result.success = true;
            } else if (vertical.valid) {
                result.angle = vertical.angle;
                result.confidence = vertical.confidence;
                result.vertical_lines = vertical.line_count;
                result.success = true;
            }
        }
    }

    if (result.success) {
        // Analysis deliberately uses the unrotated source. Convert its stable
        // absolute target into the residual expected by the Rotation control.
        result.angle -= pparams->rotate.degree;
    }

    cleanupDtStructures(&gui);
    return result;
}


/*
void PerspectiveCorrection::autocrop(int width, int height, bool fixratio, const procparams::PerspectiveParams &params, const FramesMetaData *metadata, int &x, int &y, int &w, int &h)
{
    auto pp = import_meta(params, metadata);
    double cw, ch;
    get_view_size(width, height, params, cw, ch);
    double s = min(double(width)/cw, double(height)/ch);
    dt_iop_ashift_params_t p;
    dt_iop_ashift_gui_data_t g;
    init_dt_structures(&p, &g, &pp);
    dt_iop_module_t module = { &g, false };
    g.buf_width = width;
    g.buf_height = height;
    p.cropmode = fixratio ? ASHIFT_CROP_ASPECT : ASHIFT_CROP_LARGEST;
    do_crop(&module, &p);
    cw *= s;
    ch *= s;
    double ox = p.cl * cw;
    double oy = p.ct * ch;
    x = ox - (cw - width)/2.0 + 0.5;
    y = oy - (ch - height)/2.0 + 0.5;
    w = (p.cr - p.cl) * cw;
    h = (p.cb - p.ct) * ch;
}
*/

} // namespace rtengine
