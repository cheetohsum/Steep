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
#include <algorithm>

#include "editwidgets.h"
#include "eventmapper.h"
#include "perspective.h"

#include "rtimage.h"
#include "rtsurface.h"

#include "rtengine/procparams.h"
#include "rtengine/rt_math.h"

using namespace rtengine;
using namespace rtengine::procparams;

const Glib::ustring PerspCorrection::TOOL_NAME = "perspective";

namespace
{

void controlLinesToValues(const std::vector<rtengine::ControlLine>& lines,
        std::vector<int>& values, std::vector<int>& types)
{
    values.clear();
    types.clear();

    for (auto&& line : lines) {
        values.push_back(line.x1);
        values.push_back(line.y1);
        values.push_back(line.x2);
        values.push_back(line.y2);

        int type = -1;
        switch (line.type) {
            case rtengine::ControlLine::VERTICAL:
                type = 0;
                break;
            case rtengine::ControlLine::HORIZONTAL:
                type = 1;
                break;
        }
        types.push_back(type);
    }
}

std::vector<rtengine::ControlLine> valuesToControlLines(
        const std::vector<int>& values, const std::vector<int>& types)
{
    int line_count = min(values.size() / 4, types.size());
    std::vector<rtengine::ControlLine> lines(line_count);

    auto values_iter = values.begin();
    auto types_iter = types.begin();
    for (auto&& line : lines) {
        line.x1 = *(values_iter++);
        line.y1 = *(values_iter++);
        line.x2 = *(values_iter++);
        line.y2 = *(values_iter++);

        switch (*(types_iter++)) {
            case 0:
                line.type = rtengine::ControlLine::VERTICAL;
                break;
            case 1:
                line.type = rtengine::ControlLine::HORIZONTAL;
                break;
        }
    }

    return lines;
}

}

PerspCorrection::PerspCorrection () : FoldableToolPanel(this, TOOL_NAME, M("TP_PERSPECTIVE_LABEL"))
{

    auto mapper = ProcEventMapper::getInstance();
    // Normal events.
    EvPerspCamAngle = mapper->newEvent(TRANSFORM, "HISTORY_MSG_PERSP_CAM_ANGLE");
    EvPerspCamFocalLength = mapper->newEvent(TRANSFORM, "HISTORY_MSG_PERSP_CAM_FL");
    EvPerspCamShift = mapper->newEvent(TRANSFORM, "HISTORY_MSG_PERSP_CAM_SHIFT");
    EvPerspMethod = mapper->newEvent(TRANSFORM, "HISTORY_MSG_PERSP_METHOD");
    EvPerspProjAngle = mapper->newEvent(TRANSFORM, "HISTORY_MSG_PERSP_PROJ_ANGLE");
    EvPerspProjRotate = mapper->newEvent(TRANSFORM, "HISTORY_MSG_PERSP_PROJ_ROTATE");
    EvPerspProjShift = mapper->newEvent(TRANSFORM, "HISTORY_MSG_PERSP_PROJ_SHIFT");
    EvPerspMeshWarp = mapper->newEvent(TRANSFORM, "HISTORY_MSG_PERSP_MESH");
    EvPerspRender = mapper->newEvent(TRANSFORM, "GENERAL_NA");
    // Void events.
    EvPerspCamAngleVoid = mapper->newEvent(M_VOID, "HISTORY_MSG_PERSP_CAM_ANGLE");
    EvPerspCamFocalLengthVoid = mapper->newEvent(M_VOID, "HISTORY_MSG_PERSP_CAM_FL");
    EvPerspCamShiftVoid = mapper->newEvent(M_VOID, "HISTORY_MSG_PERSP_CAM_SHIFT");
    EvPerspProjAngleVoid = mapper->newEvent(M_VOID, "HISTORY_MSG_PERSP_PROJ_ANGLE");
    EvPerspProjRotateVoid = mapper->newEvent(M_VOID, "HISTORY_MSG_PERSP_PROJ_ROTATE");
    EvPerspProjShiftVoid = mapper->newEvent(M_VOID, "HISTORY_MSG_PERSP_PROJ_SHIFT");
    setCamBasedEventsActive();
    EvPerspControlLines = mapper->newEvent(M_VOID, "HISTORY_MSG_PERSP_CTRL_LINE");

    lens_geom_listener = nullptr;
    panel_listener = nullptr;
    metadata = nullptr;

    RTImage* const ipers_draw = Gtk::manage (new RTImage ("draw", Gtk::ICON_SIZE_BUTTON));
    RTImage* const ipers_trash = Gtk::manage (new RTImage ("trash-empty", Gtk::ICON_SIZE_BUTTON));
    RTImage* const ipers_apply = Gtk::manage (new RTImage ("tick", Gtk::ICON_SIZE_BUTTON));

    RTImage* const ipersHL = Gtk::manage (new RTImage ("perspective-horizontal-left-small"));
    RTImage* const ipersHR = Gtk::manage (new RTImage ("perspective-horizontal-right-small"));
    RTImage* const ipersVL = Gtk::manage (new RTImage ("perspective-vertical-bottom-small"));
    RTImage* const ipersVR = Gtk::manage (new RTImage ("perspective-vertical-top-small"));

    RTImage* const ipers_auto_pitch = Gtk::manage (new RTImage ("perspective-vertical-bottom", Gtk::ICON_SIZE_BUTTON));
    RTImage* const ipers_auto_yaw = Gtk::manage (new RTImage ("perspective-horizontal-left", Gtk::ICON_SIZE_BUTTON));
    RTImage* const ipers_auto_pitch_yaw = Gtk::manage (new RTImage ("perspective-horizontal-vertical", Gtk::ICON_SIZE_BUTTON));

    RTImage* const ipers_cam_yaw_left = Gtk::manage (new RTImage ("perspective-horizontal-left-small"));
    RTImage* const ipers_cam_yaw_right = Gtk::manage (new RTImage ("perspective-horizontal-right-small"));
    RTImage* const ipers_cam_pitch_left = Gtk::manage (new RTImage ("perspective-vertical-bottom-small"));
    RTImage* const ipers_cam_pitch_right = Gtk::manage (new RTImage ("perspective-vertical-top-small"));
    RTImage* const ipers_proj_yaw_left = Gtk::manage (new RTImage ("perspective-horizontal-left-small"));
    RTImage* const ipers_proj_yaw_right = Gtk::manage (new RTImage ("perspective-horizontal-right-small"));
    RTImage* const ipers_proj_pitch_left = Gtk::manage (new RTImage ("perspective-vertical-bottom-small"));
    RTImage* const ipers_proj_pitch_right = Gtk::manage (new RTImage ("perspective-vertical-top-small"));
    RTImage* const ipers_rotate_left = Gtk::manage(new RTImage("rotate-right-small"));
    RTImage* const ipers_rotate_right = Gtk::manage(new RTImage("rotate-left-small"));

    Gtk::Grid* method_hbox = Gtk::manage(new Gtk::Grid());
    method_hbox->get_style_context()->add_class("grid-spacing");
    setExpandAlignProperties(method_hbox, true, false, Gtk::ALIGN_FILL, Gtk::ALIGN_CENTER);
    Gtk::Label* method_label = Gtk::manage(new Gtk::Label(M("TP_PERSPECTIVE_METHOD") + ": "));
    setExpandAlignProperties(method_label, false, false, Gtk::ALIGN_START, Gtk::ALIGN_CENTER);
    method = Gtk::manage (new MyComboBoxText ());
    method->append (M("TP_PERSPECTIVE_METHOD_SIMPLE"));
    method->append (M("TP_PERSPECTIVE_METHOD_CAMERA_BASED"));
    setExpandAlignProperties(method, true, false, Gtk::ALIGN_FILL, Gtk::ALIGN_CENTER);
    method_hbox->attach(*method_label, 0, 0, 1, 1);
    method_hbox->attach(*method, 1, 0, 1, 1);

    simple = Gtk::manage( new Gtk::Box(Gtk::ORIENTATION_VERTICAL) );

    vert = Gtk::manage (new Adjuster (M("TP_PERSPECTIVE_VERTICAL"), -100, 100, 0.1, 0, ipersVL, ipersVR));
    vert->setAdjusterListener (this);

    horiz = Gtk::manage (new Adjuster (M("TP_PERSPECTIVE_HORIZONTAL"), -100, 100, 0.1, 0, ipersHL, ipersHR));
    horiz->setAdjusterListener (this);

    camera_based = Gtk::manage( new Gtk::Box(Gtk::ORIENTATION_VERTICAL) );

    Gtk::Frame* camera_frame = Gtk::manage (new Gtk::Frame
            (M("TP_PERSPECTIVE_CAMERA_FRAME")));
    camera_frame->set_label_align(0.025, 0.5);

    Gtk::Box* camera_vbox = Gtk::manage (new Gtk::Box(Gtk::ORIENTATION_VERTICAL));

    camera_focal_length = Gtk::manage (new Adjuster (M("TP_PERSPECTIVE_CAMERA_FOCAL_LENGTH"), 0.5, 2000, 0.01, PerspectiveParams::DEFAULT_CAMERA_FOCAL_LENGTH));
    camera_focal_length->setAdjusterListener (this);

    camera_crop_factor = Gtk::manage (new Adjuster (M("TP_PERSPECTIVE_CAMERA_CROP_FACTOR"), 0.1, 30, 0.01, PerspectiveParams::DEFAULT_CAMERA_CROP_FACTOR));
    camera_crop_factor->setAdjusterListener (this);

    camera_shift_horiz = Gtk::manage (new Adjuster (M("TP_PERSPECTIVE_CAMERA_SHIFT_HORIZONTAL"), -50, 50, 0.01, 0));
    camera_shift_horiz->setAdjusterListener (this);

    camera_shift_vert = Gtk::manage (new Adjuster (M("TP_PERSPECTIVE_CAMERA_SHIFT_VERTICAL"), -50, 50, 0.01, 0));
    camera_shift_vert->setAdjusterListener (this);

    camera_roll = Gtk::manage (new Adjuster (M("TP_PERSPECTIVE_CAMERA_ROLL"), -45, 45, 0.01, 0));
    camera_roll->setAdjusterListener (this);

    camera_pitch = Gtk::manage (new Adjuster (M("TP_PERSPECTIVE_CAMERA_PITCH"),
                -60, 60, 0.1, 0, ipers_cam_pitch_left, ipers_cam_pitch_right));
    camera_pitch->setAdjusterListener (this);

    camera_yaw = Gtk::manage (new Adjuster (M("TP_PERSPECTIVE_CAMERA_YAW"),
                -60, 60, 0.1, 0, ipers_cam_yaw_left, ipers_cam_yaw_right));
    camera_yaw->setAdjusterListener (this);

    // Begin control lines interface.
    lines_button_apply = Gtk::manage (new Gtk::Button());
    lines_button_apply->set_image(*ipers_apply);
    lines_button_apply->set_tooltip_text(M("GENERAL_APPLY"));
    lines_button_apply->set_sensitive(false);
    lines_button_apply->signal_pressed().connect(sigc::mem_fun(
            *this, &::PerspCorrection::linesApplyButtonPressed));

    lines_button_edit = Gtk::manage (new Gtk::ToggleButton());
    lines_button_edit->set_image(*ipers_draw);
    lines_button_edit->set_tooltip_text(M("GENERAL_EDIT"));
    lines_button_edit->signal_toggled().connect(sigc::mem_fun(
            *this, &::PerspCorrection::linesEditButtonPressed));

    lines_button_erase = Gtk::manage (new Gtk::Button());
    lines_button_erase->set_image(*ipers_trash);
    lines_button_erase->set_tooltip_text(M("GENERAL_DELETE_ALL"));
    lines_button_erase->set_sensitive(false);
    lines_button_erase->signal_pressed().connect(sigc::mem_fun(
            *this, &::PerspCorrection::linesEraseButtonPressed));

    lines = std::unique_ptr<ControlLineManager>(new ControlLineManager());
    lines->callbacks = std::make_shared<LinesCallbacks>(this);

    dragSubscriber_ = std::unique_ptr<PerspectiveDragSubscriber>(new PerspectiveDragSubscriber());
    dragSubscriber_->setCallback(this);

    gridSubscriber_ = std::unique_ptr<VertexGridSubscriber>(new VertexGridSubscriber());
    gridSubscriber_->setCallback(this);

    Gtk::Box* control_lines_box = Gtk::manage (new Gtk::Box());
    Gtk::Label* control_lines_label = Gtk::manage (new Gtk::Label (M("TP_PERSPECTIVE_CONTROL_LINES") + ": "));
    control_lines_label->set_tooltip_markup( M("TP_PERSPECTIVE_CONTROL_LINES_TOOLTIP") );
    control_lines_box->pack_start(*control_lines_label, Gtk::PACK_SHRINK);
    control_lines_box->pack_start(*lines_button_edit);
    control_lines_box->pack_start(*lines_button_apply);
    control_lines_box->pack_start(*lines_button_erase);
    // End control lines interface.

    auto_pitch = Gtk::manage (new Gtk::Button ());
    auto_pitch->set_image(*ipers_auto_pitch);
    auto_pitch->signal_pressed().connect( sigc::bind(sigc::mem_fun(*this, &PerspCorrection::autoCorrectionPressed), auto_pitch) );

    auto_yaw = Gtk::manage (new Gtk::Button ());
    auto_yaw->set_image(*ipers_auto_yaw);
    auto_yaw->signal_pressed().connect( sigc::bind(sigc::mem_fun(*this, &PerspCorrection::autoCorrectionPressed), auto_yaw) );

    auto_pitch_yaw = Gtk::manage (new Gtk::Button ());
    auto_pitch_yaw->set_image(*ipers_auto_pitch_yaw);
    auto_pitch_yaw->signal_pressed().connect( sigc::bind(sigc::mem_fun(*this, &PerspCorrection::autoCorrectionPressed), auto_pitch_yaw) );

    Gtk::Frame* pca_frame = Gtk::manage (new Gtk::Frame
            (M("TP_PERSPECTIVE_POST_CORRECTION_ADJUSTMENT_FRAME")));
    pca_frame->set_label_align(0.025, 0.5);

    Gtk::Box* pca_vbox = Gtk::manage (new Gtk::Box(Gtk::ORIENTATION_VERTICAL));

    projection_shift_horiz = Gtk::manage (new Adjuster (M("TP_PERSPECTIVE_PROJECTION_SHIFT_HORIZONTAL"), -100, 100, 0.01, 0));
    projection_shift_horiz->setAdjusterListener (this);

    projection_shift_vert = Gtk::manage (new Adjuster (M("TP_PERSPECTIVE_PROJECTION_SHIFT_VERTICAL"), -100, 100, 0.01, 0));
    projection_shift_vert->setAdjusterListener (this);

    projection_rotate = Gtk::manage (new Adjuster (M("TP_PERSPECTIVE_PROJECTION_ROTATE"), -45, 45, 0.01, 0, ipers_rotate_left, ipers_rotate_right));
    projection_rotate->setAdjusterListener (this);

    Gtk::Frame* recovery_frame = Gtk::manage (new Gtk::Frame
            (M("TP_PERSPECTIVE_RECOVERY_FRAME")));
    recovery_frame->set_label_align(0.025, 0.5);

    Gtk::Box* recovery_vbox = Gtk::manage (new Gtk::Box(Gtk::ORIENTATION_VERTICAL));

    projection_pitch = Gtk::manage (new Adjuster (M("TP_PERSPECTIVE_PROJECTION_PITCH"), -60, 60, 0.1, 0, ipers_proj_pitch_left, ipers_proj_pitch_right));
    projection_pitch->setAdjusterListener (this);

    projection_yaw = Gtk::manage (new Adjuster (M("TP_PERSPECTIVE_PROJECTION_YAW"), -60, 60, 0.1, 0, ipers_proj_yaw_left, ipers_proj_yaw_right));
    projection_yaw->setAdjusterListener (this);

    getSummaryBox()->pack_start (*horiz);
    getSummaryBox()->pack_start (*vert);

    camera_vbox->pack_start (*camera_focal_length);
    camera_vbox->pack_start (*camera_crop_factor);
    camera_vbox->pack_start (*camera_shift_horiz);
    camera_vbox->pack_start (*camera_shift_vert);
    camera_vbox->pack_start (*camera_roll);
    camera_vbox->pack_start (*camera_pitch);
    camera_vbox->pack_start (*camera_yaw);
    camera_vbox->pack_start (*Gtk::manage (new Gtk::Separator(Gtk::ORIENTATION_HORIZONTAL)));
    camera_vbox->pack_start (*control_lines_box);
    camera_frame->add(*camera_vbox);
    camera_based->pack_start(*camera_frame);

    pca_vbox->pack_start (*projection_shift_horiz);
    pca_vbox->pack_start (*projection_shift_vert);
    pca_vbox->pack_start (*projection_rotate);
    pca_frame->add(*pca_vbox);
    camera_based->pack_start(*pca_frame);

    recovery_vbox->pack_start (*projection_yaw);
    recovery_vbox->pack_start (*projection_pitch);
    recovery_frame->add(*recovery_vbox);
    camera_based->pack_start(*recovery_frame);

    getSummaryBox()->show_all();

    advancedSection = Gtk::manage(new AdvancedSection());
    pack_start(*advancedSection, Gtk::PACK_SHRINK, 0);
    Gtk::Box* const advBox = advancedSection->getContentBox();

    advBox->pack_start(*method_hbox);
    advBox->pack_start(*camera_based);

    horiz->setLogScale(2, 0);
    vert->setLogScale(2, 0);
    camera_focal_length->setLogScale(4000, 0.5);
    camera_crop_factor->setLogScale(300, 0.1);

    method->connect(method->signal_changed().connect(sigc::mem_fun(*this, &PerspCorrection::methodChanged)));

    show_all();
}

void PerspCorrection::read (const ProcParams* pp, const ParamsEdited* pedited)
{

    disableListener ();

    if (pedited) {
        horiz->setEditedState (pedited->perspective.horizontal ? Edited : UnEdited);
        vert->setEditedState (pedited->perspective.vertical ? Edited : UnEdited);
        camera_crop_factor->setEditedState (pedited->perspective.camera_crop_factor ? Edited : UnEdited);
        camera_focal_length->setEditedState (pedited->perspective.camera_focal_length ? Edited : UnEdited);
        camera_pitch->setEditedState (pedited->perspective.camera_pitch ? Edited : UnEdited);
        camera_roll->setEditedState (pedited->perspective.camera_roll ? Edited : UnEdited);
        camera_shift_horiz->setEditedState (pedited->perspective.camera_shift_horiz ? Edited : UnEdited);
        camera_shift_vert->setEditedState (pedited->perspective.camera_shift_vert ? Edited : UnEdited);
        camera_yaw->setEditedState (pedited->perspective.camera_yaw ? Edited : UnEdited);
        projection_pitch->setEditedState (pedited->perspective.projection_pitch ? Edited : UnEdited);
        projection_rotate->setEditedState (pedited->perspective.projection_rotate ? Edited : UnEdited);
        projection_shift_horiz->setEditedState (pedited->perspective.projection_shift_horiz ? Edited : UnEdited);
        projection_shift_vert->setEditedState (pedited->perspective.projection_shift_vert ? Edited : UnEdited);
        projection_yaw->setEditedState (pedited->perspective.projection_yaw ? Edited : UnEdited);
        lines->setEdited (pedited->perspective.control_lines);
    }

    horiz->setValue (pp->perspective.horizontal);
    vert->setValue (pp->perspective.vertical);
    setFocalLengthValue (pp, metadata);
    camera_pitch->setValue (pp->perspective.camera_pitch);
    camera_roll->setValue (pp->perspective.camera_roll);
    camera_shift_horiz->setValue (pp->perspective.camera_shift_horiz);
    camera_shift_vert->setValue (pp->perspective.camera_shift_vert);
    camera_yaw->setValue (pp->perspective.camera_yaw);
    projection_pitch->setValue (pp->perspective.projection_pitch);
    projection_rotate->setValue (pp->perspective.projection_rotate);
    projection_shift_horiz->setValue (pp->perspective.projection_shift_horiz);
    projection_shift_vert->setValue (pp->perspective.projection_shift_vert);
    projection_yaw->setValue (pp->perspective.projection_yaw);
    lines->setLines(valuesToControlLines(pp->perspective.control_line_values,
            pp->perspective.control_line_types));

    if (pedited && !pedited->perspective.method) {
        method->set_active (2);
    } else if (pp->perspective.method == "simple") {
        method->set_active (0);
    } else if (pp->perspective.method == "camera_based") {
        method->set_active (1);
    }

    if (gridSubscriber_) {
        if (pp->perspective.mesh_grid_size == VertexGridSubscriber::GRID_SIZE
            && static_cast<int>(pp->perspective.mesh_offsets_x.size()) == VertexGridSubscriber::VERTEX_COUNT) {
            for (int i = 0; i < VertexGridSubscriber::VERTEX_COUNT; ++i) {
                gridSubscriber_->vertices_[i].dx = pp->perspective.mesh_offsets_x[i];
                gridSubscriber_->vertices_[i].dy = pp->perspective.mesh_offsets_y[i];
            }
        } else {
            // No mesh data — reset all vertices to zero displacement
            for (int i = 0; i < VertexGridSubscriber::VERTEX_COUNT; ++i) {
                gridSubscriber_->vertices_[i].dx = 0;
                gridSubscriber_->vertices_[i].dy = 0;
            }
        }
        // Refresh on-canvas geometry positions
        EditDataProvider* provider = gridSubscriber_->getEditProvider();
        if (provider) {
            int iw, ih;
            provider->getImageSize(iw, ih);
            if (iw > 0 && ih > 0) {
                gridSubscriber_->updateGeometry(iw, ih);
            }
        }
    }

    updateApplyDeleteButtons();

    enableListener ();
}

void PerspCorrection::write (ProcParams* pp, ParamsEdited* pedited)
{
    // If any of these are non-zero, the focal length and crop factor must be
    // updated to ensure they won't be auto-filled from metadata later. This
    // prevents surprise changes to the perspective correction results.
    const bool update_fl = camera_pitch->getValue() != 0 ||
                           camera_yaw->getValue() != 0 ||
                           projection_pitch->getValue() != 0 ||
                           projection_yaw->getValue() != 0;

    pp->perspective.render = render;

    pp->perspective.horizontal  = horiz->getValue ();
    pp->perspective.vertical = vert->getValue ();
    if (update_fl || pp->perspective.camera_crop_factor > 0 ||
        std::abs(camera_crop_factor->getValue() - PerspectiveParams::DEFAULT_CAMERA_CROP_FACTOR) > 1e-5) {
        // Update if update_fl is true or the crop factor has previously been
        // set or if the adjuster has changed from the default value.
        pp->perspective.camera_crop_factor = camera_crop_factor->getValue ();
    }
    if (update_fl || pp->perspective.camera_focal_length > 0 ||
        std::abs(camera_focal_length->getValue() - PerspectiveParams::DEFAULT_CAMERA_FOCAL_LENGTH) > 1e-4) {
        // Update if update_fl is true or the focal length has previously been
        // set or if the adjuster has changed from the default value.
        pp->perspective.camera_focal_length = camera_focal_length->getValue ();
    }
    pp->perspective.camera_pitch = camera_pitch->getValue ();
    pp->perspective.camera_roll = camera_roll->getValue ();
    pp->perspective.camera_shift_horiz = camera_shift_horiz->getValue ();
    pp->perspective.camera_shift_vert = camera_shift_vert->getValue ();
    pp->perspective.camera_yaw = camera_yaw->getValue ();
    pp->perspective.projection_pitch = projection_pitch->getValue ();
    pp->perspective.projection_rotate = projection_rotate->getValue ();
    pp->perspective.projection_shift_horiz = projection_shift_horiz->getValue ();
    pp->perspective.projection_shift_vert = projection_shift_vert->getValue ();
    pp->perspective.projection_yaw = projection_yaw->getValue ();

    std::vector<rtengine::ControlLine> control_lines;
    lines->toControlLines(control_lines);
    controlLinesToValues(control_lines, pp->perspective.control_line_values,
            pp->perspective.control_line_types);

    if (gridSubscriber_) {
        const auto& verts = gridSubscriber_->vertices_;
        bool anyNonZero = false;
        pp->perspective.mesh_offsets_x.resize(VertexGridSubscriber::VERTEX_COUNT);
        pp->perspective.mesh_offsets_y.resize(VertexGridSubscriber::VERTEX_COUNT);
        for (int i = 0; i < VertexGridSubscriber::VERTEX_COUNT; ++i) {
            pp->perspective.mesh_offsets_x[i] = verts[i].dx;
            pp->perspective.mesh_offsets_y[i] = verts[i].dy;
            if (verts[i].dx != 0 || verts[i].dy != 0) anyNonZero = true;
        }
        pp->perspective.mesh_grid_size = anyNonZero ? VertexGridSubscriber::GRID_SIZE : 0;
    }

    if (method->get_active_row_number() == 0) {
        pp->perspective.method = "simple";
    } else if (method->get_active_row_number() == 1) {
        pp->perspective.method = "camera_based";
    }

    if (pedited) {
        pedited->perspective.method =  method->get_active_row_number() != 2;
        pedited->perspective.horizontal = horiz->getEditedState ();
        pedited->perspective.vertical = vert->getEditedState ();
        pedited->perspective.camera_crop_factor= camera_crop_factor->getEditedState ();
        pedited->perspective.camera_focal_length = camera_focal_length->getEditedState ();
        pedited->perspective.camera_pitch = camera_pitch->getEditedState();
        pedited->perspective.camera_roll = camera_roll->getEditedState();
        pedited->perspective.camera_shift_horiz = camera_shift_horiz->getEditedState();
        pedited->perspective.camera_shift_vert = camera_shift_vert->getEditedState();
        pedited->perspective.camera_yaw = camera_yaw->getEditedState();
        pedited->perspective.projection_pitch = projection_pitch->getEditedState();
        pedited->perspective.projection_rotate = projection_rotate->getEditedState();
        pedited->perspective.projection_shift_horiz = projection_shift_horiz->getEditedState();
        pedited->perspective.projection_shift_vert = projection_shift_vert->getEditedState();
        pedited->perspective.projection_yaw = projection_yaw->getEditedState();
        pedited->perspective.control_lines = lines->getEdited();
        pedited->perspective.mesh_warp = true;
    }
}

void PerspCorrection::setDefaults (const ProcParams* defParams, const ParamsEdited* pedited)
{

    horiz->setDefault (defParams->perspective.horizontal);
    vert->setDefault (defParams->perspective.vertical);
    camera_crop_factor->setDefault(defParams->perspective.camera_crop_factor > 0
                                       ? defParams->perspective.camera_crop_factor
                                       : PerspectiveParams::DEFAULT_CAMERA_CROP_FACTOR);
    camera_focal_length->setDefault(defParams->perspective.camera_focal_length > 0
                                        ? defParams->perspective.camera_focal_length
                                        : PerspectiveParams::DEFAULT_CAMERA_FOCAL_LENGTH);
    camera_pitch->setDefault (defParams->perspective.camera_pitch);
    camera_roll->setDefault (defParams->perspective.camera_roll);
    camera_shift_horiz->setDefault (defParams->perspective.camera_shift_horiz);
    camera_shift_vert->setDefault (defParams->perspective.camera_shift_vert);
    camera_yaw->setDefault (defParams->perspective.camera_yaw);
    projection_pitch->setDefault (defParams->perspective.projection_pitch);
    projection_rotate->setDefault (defParams->perspective.projection_rotate);
    projection_shift_horiz->setDefault (defParams->perspective.projection_shift_horiz);
    projection_shift_vert->setDefault (defParams->perspective.projection_shift_vert);
    projection_yaw->setDefault (defParams->perspective.projection_yaw);

    if (pedited) {
        horiz->setDefaultEditedState (pedited->perspective.horizontal ? Edited : UnEdited);
        vert->setDefaultEditedState (pedited->perspective.vertical ? Edited : UnEdited);
        camera_crop_factor->setDefaultEditedState (pedited->perspective.camera_crop_factor ? Edited : UnEdited);
        camera_focal_length->setDefaultEditedState (pedited->perspective.camera_focal_length ? Edited : UnEdited);
        camera_pitch->setDefaultEditedState (pedited->perspective.camera_pitch ? Edited : UnEdited);
        camera_roll->setDefaultEditedState (pedited->perspective.camera_roll ? Edited : UnEdited);
        camera_shift_horiz->setDefaultEditedState (pedited->perspective.camera_shift_horiz ? Edited : UnEdited);
        camera_shift_vert->setDefaultEditedState (pedited->perspective.camera_shift_vert ? Edited : UnEdited);
        camera_yaw->setDefaultEditedState (pedited->perspective.camera_yaw ? Edited : UnEdited);
        projection_pitch->setDefaultEditedState (pedited->perspective.projection_pitch ? Edited : UnEdited);
        projection_rotate->setDefaultEditedState (pedited->perspective.projection_rotate ? Edited : UnEdited);
        projection_shift_horiz->setDefaultEditedState (pedited->perspective.projection_shift_horiz ? Edited : UnEdited);
        projection_shift_vert->setDefaultEditedState (pedited->perspective.projection_shift_vert ? Edited : UnEdited);
        projection_yaw->setDefaultEditedState (pedited->perspective.projection_yaw ? Edited : UnEdited);
    } else {
        horiz->setDefaultEditedState (Irrelevant);
        vert->setDefaultEditedState (Irrelevant);
        camera_crop_factor->setDefaultEditedState (Irrelevant);
        camera_focal_length->setDefaultEditedState (Irrelevant);
        camera_pitch->setDefaultEditedState (Irrelevant);
        camera_roll->setDefaultEditedState (Irrelevant);
        camera_shift_horiz->setDefaultEditedState (Irrelevant);
        camera_shift_vert->setDefaultEditedState (Irrelevant);
        camera_yaw->setDefaultEditedState (Irrelevant);
        projection_pitch->setDefaultEditedState (Irrelevant);
        projection_rotate->setDefaultEditedState (Irrelevant);
        projection_shift_horiz->setDefaultEditedState (Irrelevant);
        projection_shift_vert->setDefaultEditedState (Irrelevant);
        projection_yaw->setDefaultEditedState (Irrelevant);
    }
}

void PerspCorrection::adjusterChanged(Adjuster* a, double newval)
{
    if (listener) {
        if (a == horiz || a == vert) {
            // H/V sliders only work in simple mode — switch silently
            if (method->get_active_row_number() != 0) {
                method->block(true);
                method->set_active(0);
                method->block(false);
            }
            listener->panelChanged (EvPerspCorr,
                    Glib::ustring::compose("%1=%2\n%3=%4",
                        M("TP_PERSPECTIVE_HORIZONTAL"),
                        horiz->getValue(),
                        M("TP_PERSPECTIVE_VERTICAL"),
                        vert->getValue()));
        } else if (a == camera_focal_length || a == camera_crop_factor) {
            listener->panelChanged (*event_persp_cam_focal_length,
                    Glib::ustring::compose("%1=%2\n%3=%4",
                        M("TP_PERSPECTIVE_CAMERA_FOCAL_LENGTH"),
                        camera_focal_length->getValue(),
                        M("TP_PERSPECTIVE_CAMERA_CROP_FACTOR"),
                        camera_crop_factor->getValue()));
        } else if (a == camera_shift_horiz || a == camera_shift_vert) {
            listener->panelChanged (*event_persp_cam_shift,
                    Glib::ustring::compose("%1=%2\n%3=%4",
                        M("TP_PERSPECTIVE_CAMERA_SHIFT_HORIZONTAL"),
                        camera_shift_horiz->getValue(),
                        M("TP_PERSPECTIVE_CAMERA_SHIFT_VERTICAL"),
                        camera_shift_vert->getValue()));
        } else if (a == camera_pitch || a == camera_roll|| a == camera_yaw) {
            listener->panelChanged (*event_persp_cam_angle,
                    Glib::ustring::compose("%1=%2\n%3=%4\n%5=%6",
                        M("TP_PERSPECTIVE_CAMERA_ROLL"),
                        camera_roll->getValue(),
                        M("TP_PERSPECTIVE_CAMERA_YAW"),
                        camera_yaw->getValue(),
                        M("TP_PERSPECTIVE_CAMERA_PITCH"),
                        camera_pitch->getValue()));
        } else if (a == projection_shift_horiz || a == projection_shift_vert) {
            listener->panelChanged (*event_persp_proj_shift,
                    Glib::ustring::compose("%1=%2\n%3=%4",
                        M("TP_PERSPECTIVE_PROJECTION_SHIFT_HORIZONTAL"),
                        projection_shift_horiz->getValue(),
                        M("TP_PERSPECTIVE_PROJECTION_SHIFT_VERTICAL"),
                        projection_shift_vert->getValue()));
        } else if (a == projection_rotate) {
            listener->panelChanged (*event_persp_proj_rotate,
                    Glib::ustring::format(projection_rotate->getValue()));
        } else if (a == projection_pitch || a == projection_yaw) {
            listener->panelChanged (*event_persp_proj_angle,
                    Glib::ustring::compose("%1=%2\n%3=%4",
                        M("TP_PERSPECTIVE_PROJECTION_PITCH"),
                        projection_pitch->getValue(),
                        M("TP_PERSPECTIVE_PROJECTION_YAW"),
                        projection_yaw->getValue()));
        }
    }
}

void PerspCorrection::applyControlLines(void)
{
    if (!lens_geom_listener) {
        return;
    }

    std::vector<rtengine::ControlLine> control_lines;
    double rot = camera_roll->getValue();
    double pitch = camera_pitch->getValue();
    double yaw = camera_yaw->getValue();

    lines->toControlLines(control_lines);

    lens_geom_listener->autoPerspRequested(
            lines->getVerticalCount() >= MIN_VERT_LINES,
            lines->getHorizontalCount() >= MIN_HORIZ_LINES,
            rot, pitch, yaw, &control_lines);

    disableListener();
    camera_pitch->setValue(pitch);
    camera_roll->setValue(rot);
    camera_yaw->setValue(yaw);
    enableListener();

    adjusterChanged(camera_pitch, pitch);
}

void PerspCorrection::tweakParams(rtengine::procparams::ProcParams &pparams)
{
    pparams.perspective.render = render;
}

void PerspCorrection::autoCorrectionPressed(Gtk::Button* b)
{
    if (!lens_geom_listener) {
        return;
    }

    double rot = 0;
    double pitch = 0;
    double yaw = 0;

    if (b == auto_pitch) {
        lens_geom_listener->autoPerspRequested(true, false, rot, pitch, yaw);
    } else if (b == auto_yaw) {
        lens_geom_listener->autoPerspRequested(false, true, rot, pitch, yaw);
    } else if (b == auto_pitch_yaw) {
        lens_geom_listener->autoPerspRequested(true, true, rot, pitch, yaw);
    }

    disableListener();
    camera_pitch->setValue(pitch);
    camera_roll->setValue(rot);
    camera_yaw->setValue(yaw);
    enableListener();

    adjusterChanged(camera_pitch, pitch);
}

void PerspCorrection::methodChanged (void)
{

    if (!batchMode) {
        if (method->get_active_row_number() == 0) {
            simple->show();
            camera_based->hide();
        } else if (method->get_active_row_number() == 1) {
            simple->hide();
            camera_based->show();
        }

        // If no longer in camera-based mode and control lines are being edited.
        if (method->get_active_row_number() != 1 && lines_button_edit->get_active()) {
            lines_button_edit->set_active(false);
        }
    }

    if (listener) {
        listener->panelChanged (EvPerspMethod, method->get_active_text ());
    }

}

void PerspCorrection::setAdjusterBehavior (
    bool badd,
    bool camera_focal_length_add,
    bool camera_shift_add,
    bool camera_angle_add,
    bool projection_angle_add,
    bool projection_shift_add,
    bool projection_rotate_add
)
{

    horiz->setAddMode(badd);
    vert->setAddMode(badd);
    camera_crop_factor->setAddMode(camera_focal_length_add);
    camera_focal_length->setAddMode(camera_focal_length_add);
    camera_pitch->setAddMode(camera_angle_add);
    camera_roll->setAddMode(camera_angle_add);
    camera_shift_horiz->setAddMode(camera_shift_add);
    camera_shift_vert->setAddMode(camera_shift_add);
    camera_yaw->setAddMode(camera_angle_add);
    projection_pitch->setAddMode(projection_angle_add);
    projection_rotate->setAddMode(projection_rotate_add);
    projection_shift_horiz->setAddMode(projection_shift_add);
    projection_shift_vert->setAddMode(projection_shift_add);
    projection_yaw->setAddMode(projection_angle_add);
}

void PerspCorrection::setControlLineEditMode(bool active)
{
    // Only camera-based mode supports control lines, so the mode must be
    // switched if not in camera-based mode. Block signal to avoid
    // methodChanged() hiding/showing containers.
    if (method->get_active_row_number() != 1) {
        method->block(true);
        method->set_active(1);
        method->block(false);
        if (listener) {
            listener->panelChanged(EvPerspMethod, method->get_active_text());
        }
    }

    lines_button_edit->set_active(active);
}

void PerspCorrection::setDragEditMode(bool active)
{
    // Drag mode uses simple H/V perspective — ensure simple mode is selected
    if (active && method->get_active_row_number() != 0) {
        method->block(true);
        method->set_active(0);
        method->block(false);
    }

    dragSubscriber_->setActive(active);

    if (active && panel_listener) {
        panel_listener->controlLineEditModeChanged(true);
    } else if (!active && panel_listener) {
        panel_listener->controlLineEditModeChanged(false);
    }
}

void PerspCorrection::setGridEditMode(bool active)
{
    // Grid mode uses simple H/V perspective — ensure simple mode is selected
    if (active && method->get_active_row_number() != 0) {
        method->block(true);
        method->set_active(0);
        method->block(false);
    }

    gridSubscriber_->setActive(active);

    if (panel_listener) {
        panel_listener->controlLineEditModeChanged(active);
    }
}

void PerspCorrection::setMetadata (const rtengine::FramesMetaData* metadata)
{
    this->metadata = metadata;
}

void PerspCorrection::trimValues (rtengine::procparams::ProcParams* pp)
{

    horiz->trimValue(pp->perspective.horizontal);
    vert->trimValue(pp->perspective.vertical);
    // Only update crop factor and focal length if they have been manually set.
    if (pp->perspective.camera_crop_factor > 0) {
        camera_crop_factor->trimValue(pp->perspective.camera_crop_factor);
    }
    if (pp->perspective.camera_focal_length > 0) {
        camera_focal_length->trimValue(pp->perspective.camera_focal_length);
    }
    camera_pitch->trimValue(pp->perspective.camera_pitch);
    camera_roll->trimValue(pp->perspective.camera_roll);
    camera_shift_horiz->trimValue(pp->perspective.camera_shift_horiz);
    camera_shift_vert->trimValue(pp->perspective.camera_shift_vert);
    camera_yaw->trimValue(pp->perspective.camera_yaw);
    projection_pitch->trimValue(pp->perspective.projection_pitch);
    projection_rotate->trimValue(pp->perspective.projection_rotate);
    projection_shift_horiz->trimValue(pp->perspective.projection_shift_horiz);
    projection_shift_vert->trimValue(pp->perspective.projection_shift_vert);
    projection_yaw->trimValue(pp->perspective.projection_yaw);
}

void PerspCorrection::setBatchMode (bool batchMode)
{

    ToolPanel::setBatchMode (batchMode);
    horiz->showEditedCB ();
    vert->showEditedCB ();
    camera_crop_factor->showEditedCB ();
    camera_focal_length->showEditedCB ();
    camera_pitch->showEditedCB ();
    camera_roll->showEditedCB ();
    camera_shift_horiz->showEditedCB ();
    camera_shift_vert->showEditedCB ();
    camera_yaw->showEditedCB ();
    projection_pitch->showEditedCB ();
    projection_rotate->showEditedCB ();
    projection_shift_horiz->showEditedCB ();
    projection_shift_vert->showEditedCB ();
    projection_yaw->showEditedCB ();

    lines_button_edit->set_sensitive(false);
    auto_pitch->set_sensitive(false);
    auto_yaw->set_sensitive(false);
    auto_pitch_yaw->set_sensitive(false);

    method->append (M("GENERAL_UNCHANGED"));
    advancedSection->setBatchMode(batchMode);
}

void PerspCorrection::setFocalLengthValue (const ProcParams* pparams, const FramesMetaData* metadata)
{
    double pp_crop_factor = pparams->perspective.camera_crop_factor;
    double pp_focal_length = pparams->perspective.camera_focal_length;
    double default_crop_factor = PerspectiveParams::DEFAULT_CAMERA_CROP_FACTOR;
    double default_focal_length = PerspectiveParams::DEFAULT_CAMERA_FOCAL_LENGTH;

    // If any of these values are non-zero, don't set the crop factor or focal
    // length from metadata to avoid a surprise change in perspective correction
    // results.
    if (pparams->perspective.camera_pitch != 0 ||
        pparams->perspective.camera_yaw != 0 ||
        pparams->perspective.projection_pitch != 0 ||
        pparams->perspective.projection_yaw != 0) {
        if (pp_crop_factor <= 0) {
            pp_crop_factor = PerspectiveParams::DEFAULT_CAMERA_CROP_FACTOR;
        }
        if (pp_focal_length <= 0) {
            pp_focal_length = PerspectiveParams::DEFAULT_CAMERA_FOCAL_LENGTH;
        }
    }

    // Defaults from metadata.
    if (metadata && (pp_crop_factor <= 0 || pp_focal_length <= 0)) {
        const double fl = metadata->getFocalLen();
        const double fl35 = metadata->getFocalLen35mm();

        if (fl <= 0) {
            if (fl35 <= 0) {
                // No focal length data.
            } else {
                // 35mm focal length available.
                default_focal_length = fl35;
            }
        } else {
            if (fl35 <= 0) {
                // Focal length available.
                default_focal_length = fl;
            } else {
                // Focal length and 35mm equivalent available.
                default_focal_length = fl;
                default_crop_factor = fl35 / fl;
            }
        }
    }

    // Change value if those from the ProcParams are invalid.
    if (pp_crop_factor > 0) {
        camera_crop_factor->setValue(pp_crop_factor);
    } else {
        camera_crop_factor->setDefault(default_crop_factor);
        camera_crop_factor->setValue(default_crop_factor);
    }
    if (pp_focal_length > 0) {
        camera_focal_length->setValue(pp_focal_length);
    } else {
        camera_focal_length->setDefault(default_focal_length);
        camera_focal_length->setValue(default_focal_length);
    }
}

void PerspCorrection::meshChanged()
{
    if (listener) {
        listener->panelChanged(EvPerspMeshWarp, M("HISTORY_MSG_PERSP_MESH"));
    }
}

void PerspCorrection::switchOffEditMode(void)
{
    lines_button_edit->set_active(false);
    dragSubscriber_->setActive(false);
}

void PerspCorrection::hideAdvancedSection()
{
    if (advancedSection) {
        advancedSection->set_no_show_all(true);
        advancedSection->hide();
    }
}

void PerspCorrection::exposeAutoButtons()
{
    // Auto buttons removed from UI
}

void PerspCorrection::runAutoCorrection()
{
    if (method->get_active_row_number() != 1) {
        method->block(true);
        method->set_active(1);
        method->block(false);
    }
    autoCorrectionPressed(auto_pitch_yaw);
}

void PerspCorrection::setEditProvider(EditDataProvider* provider)
{
    lines->setEditProvider(provider);
    dragSubscriber_->setEditProvider(provider);
    gridSubscriber_->setEditProvider(provider);
}

void PerspCorrection::lineChanged(void)
{
    updateApplyDeleteButtons();

    if (listener) {
        listener->panelChanged(EvPerspControlLines, M("HISTORY_CHANGED"));
    }
}

void PerspCorrection::updateApplyDeleteButtons()
{
    if (batchMode) {
        return;
    }

    bool edit_mode = lines_button_edit->get_active();
    bool enough_lines = lines->getHorizontalCount() >= MIN_HORIZ_LINES || lines->getVerticalCount() >= MIN_VERT_LINES;
    const auto tooltip = M("GENERAL_APPLY")
        + ((edit_mode && !enough_lines) ? "\n\n" + M("TP_PERSPECTIVE_CONTROL_LINE_APPLY_INVALID_TOOLTIP") : "");

    lines_button_apply->set_sensitive(edit_mode && enough_lines);
    lines_button_apply->set_tooltip_text(tooltip);
    lines_button_erase->set_sensitive(edit_mode && lines->size() > 0);
}

void PerspCorrection::linesApplyButtonPressed(void)
{
    if (method->get_active_row_number() == 1) {
        // Calculate perspective distortion if in camera-based mode.
        applyControlLines();
    }
    lines_button_edit->set_active(false);
}

void PerspCorrection::linesEditButtonPressed(void)
{
    if (lines_button_edit->get_active()) { // Enter edit mode.
        lines->setActive(true);
        lines->setDrawMode(true);
        render = false;
        if (listener) {
            listener->setTweakOperator(this);
            listener->refreshPreview(EvPerspRender);
        }
        lines_button_apply->set_sensitive(true);
        lines_button_erase->set_sensitive(true);
        setCamBasedEventsActive(false);
        if (panel_listener) {
            panel_listener->controlLineEditModeChanged(true);
        }
    } else { // Leave edit mode.
        setCamBasedEventsActive(true);
        lines_button_apply->set_sensitive(false);
        lines_button_erase->set_sensitive(false);
        render = true;
        if (listener) {
            listener->unsetTweakOperator(this);
            listener->refreshPreview(EvPerspRender);
        }
        lines->releaseEdit();
        lines->setDrawMode(false);
        lines->setActive(false);
        if (panel_listener) {
            panel_listener->controlLineEditModeChanged(false);
        }
    }
    updateApplyDeleteButtons();
}

void PerspCorrection::linesEraseButtonPressed(void)
{
    lines->removeAll();
}

void PerspCorrection::requestApplyControlLines(void)
{
    if (lines_button_apply->is_sensitive()) {
        linesApplyButtonPressed();
    } else if (lines_button_edit->get_active()) {
        lines_button_edit->set_active(false);
    }
}

void PerspCorrection::setCamBasedEventsActive(bool active)
{
    if (active) {
        event_persp_cam_focal_length = &EvPerspCamFocalLength;
        event_persp_cam_shift = &EvPerspCamShift;
        event_persp_cam_angle = &EvPerspCamAngle;
        event_persp_proj_shift = &EvPerspProjShift;
        event_persp_proj_rotate = &EvPerspProjRotate;
        event_persp_proj_angle = &EvPerspProjAngle;
    } else {
        event_persp_cam_focal_length = &EvPerspCamFocalLengthVoid;
        event_persp_cam_shift = &EvPerspCamShiftVoid;
        event_persp_cam_angle = &EvPerspCamAngleVoid;
        event_persp_proj_shift = &EvPerspProjShiftVoid;
        event_persp_proj_rotate = &EvPerspProjRotateVoid;
        event_persp_proj_angle = &EvPerspProjAngleVoid;
    }
}

LinesCallbacks::LinesCallbacks(PerspCorrection* tool):
    tool(tool)
{
}

void LinesCallbacks::lineChanged(void)
{
    if (tool) {
        tool->lineChanged();
    }
}

void LinesCallbacks::switchOffEditMode(void)
{
    if (tool) {
        tool->switchOffEditMode();
    }
}

// --- PerspectiveDragSubscriber implementation ---

PerspectiveDragSubscriber::PerspectiveDragSubscriber()
    : EditSubscriber(ET_OBJECTS),
      canvas_area_(new EditRectangle())
{
    // Invisible hit-test rectangle covering the whole image
    canvas_area_->filled = true;
    canvas_area_->topLeft = rtengine::Coord(0, 0);
    canvas_area_->bottomRight = rtengine::Coord(1, 1); // updated in setActive
    mouseOverGeometry.push_back(canvas_area_.get());

    // Create reference grid lines (4 per axis at 20/40/60/80%)
    for (int i = 0; i < GRID_LINES; ++i) {
        auto hLine = std::unique_ptr<Line>(new Line());
        hLine->innerLineWidth = 1.0f;
        hLine->setInnerLineColor(0.6, 0.6, 0.6);
        hLine->setOuterLineColor(0.15, 0.15, 0.15);
        hLine->opacity = 30.f;
        hLine->datum = Geometry::IMAGE;
        visibleGeometry.push_back(hLine.get());
        gridLines_.push_back(std::move(hLine));

        auto vLine = std::unique_ptr<Line>(new Line());
        vLine->innerLineWidth = 1.0f;
        vLine->setInnerLineColor(0.6, 0.6, 0.6);
        vLine->setOuterLineColor(0.15, 0.15, 0.15);
        vLine->opacity = 30.f;
        vLine->datum = Geometry::IMAGE;
        visibleGeometry.push_back(vLine.get());
        gridLines_.push_back(std::move(vLine));
    }

    // Center crosshair lines (more prominent, dashed)
    centerH_ = std::unique_ptr<Line>(new Line());
    centerH_->innerLineWidth = 1.2f;
    centerH_->setInnerLineColor(0.8, 0.8, 0.4);  // muted yellow
    centerH_->setOuterLineColor(0.1, 0.1, 0.1);
    centerH_->opacity = 50.f;
    centerH_->setDashed(true);
    centerH_->datum = Geometry::IMAGE;
    visibleGeometry.push_back(centerH_.get());

    centerV_ = std::unique_ptr<Line>(new Line());
    centerV_->innerLineWidth = 1.2f;
    centerV_->setInnerLineColor(0.8, 0.8, 0.4);
    centerV_->setOuterLineColor(0.1, 0.1, 0.1);
    centerV_->opacity = 50.f;
    centerV_->setDashed(true);
    centerV_->datum = Geometry::IMAGE;
    visibleGeometry.push_back(centerV_.get());
}

void PerspectiveDragSubscriber::updateGridGeometry(int iw, int ih, double hPersp, double vPersp)
{
    // Map perspective values (-100..+100) to corner-shift factors.
    // Positive H: right side compresses vertically (lines converge right).
    // Positive V: top side compresses horizontally (lines converge up).
    const double kh = hPersp / 100.0 * 0.20;
    const double kv = vPersp / 100.0 * 0.20;

    // Four corners of the warped rectangle (bilinear trapezoid)
    const double tl_x = 0.0  + kv * iw,  tl_y = 0.0  + kh * ih;
    const double tr_x = (double)iw - kv * iw,  tr_y = 0.0  - kh * ih;
    const double bl_x = 0.0  - kv * iw,  bl_y = (double)ih - kh * ih;
    const double br_x = (double)iw + kv * iw,  br_y = (double)ih + kh * ih;

    // Helper: bilinear interpolation of corners at (u, v) in [0,1]²
    auto mapPoint = [&](double u, double v) -> rtengine::Coord {
        double x = (1 - v) * ((1 - u) * tl_x + u * tr_x)
                 +      v  * ((1 - u) * bl_x + u * br_x);
        double y = (1 - v) * ((1 - u) * tl_y + u * tr_y)
                 +      v  * ((1 - u) * bl_y + u * br_y);
        return rtengine::Coord(static_cast<int>(x), static_cast<int>(y));
    };

    for (int i = 0; i < GRID_LINES; ++i) {
        double frac = (i + 1.0) / (GRID_LINES + 1.0);

        // Horizontal line at v=frac, u goes 0→1
        gridLines_[i * 2]->begin = mapPoint(0.0, frac);
        gridLines_[i * 2]->end   = mapPoint(1.0, frac);

        // Vertical line at u=frac, v goes 0→1
        gridLines_[i * 2 + 1]->begin = mapPoint(frac, 0.0);
        gridLines_[i * 2 + 1]->end   = mapPoint(frac, 1.0);
    }

    // Center crosshair
    centerH_->begin = mapPoint(0.0, 0.5);
    centerH_->end   = mapPoint(1.0, 0.5);
    centerV_->begin = mapPoint(0.5, 0.0);
    centerV_->end   = mapPoint(0.5, 1.0);
}

void PerspectiveDragSubscriber::setActive(bool active)
{
    EditDataProvider* provider = getEditProvider();
    if (!provider) return;

    bool isActive = (this == provider->getCurrSubscriber());
    if (isActive == active) return;

    if (active) {
        int iw, ih;
        provider->getImageSize(iw, ih);
        canvas_area_->bottomRight = rtengine::Coord(iw, ih);
        double h = perspective_ ? perspective_->horiz->getValue() : 0.0;
        double v = perspective_ ? perspective_->vert->getValue() : 0.0;
        updateGridGeometry(iw, ih, h, v);
        subscribe();
    } else {
        unsubscribe();
    }
}

bool PerspectiveDragSubscriber::button1Pressed(int modifierKey)
{
    EditDataProvider* dp = getEditProvider();
    if (!dp || !perspective_) return false;

    // Store initial slider values so drag is cumulative from start
    startHoriz_ = perspective_->horiz->getValue();
    startVert_ = perspective_->vert->getValue();
    action = Action::DRAGGING;
    return false; // no redraw needed yet
}

bool PerspectiveDragSubscriber::drag1(int modifierKey)
{
    EditDataProvider* dp = getEditProvider();
    if (!dp || !perspective_) return false;

    int iw, ih;
    dp->getImageSize(iw, ih);
    if (iw <= 0 || ih <= 0) return false;

    // Map drag distance to perspective values.
    // A drag across half the image = 25 units of perspective change.
    double horizDelta = (dp->deltaImage.x / (double)iw) * 50.0;
    double vertDelta = (dp->deltaImage.y / (double)ih) * 50.0;

    double newH = std::max(-100.0, std::min(100.0, startHoriz_ + horizDelta));
    double newV = std::max(-100.0, std::min(100.0, startVert_ + vertDelta));

    // Update slider visuals
    perspective_->horiz->setValue(newH);
    perspective_->vert->setValue(newV);

    // Update grid overlay to reflect new perspective
    updateGridGeometry(iw, ih, newH, newV);

    // Trigger reprocessing
    perspective_->adjusterChanged(perspective_->horiz, newH);

    return true; // redraw canvas to show updated grid
}

bool PerspectiveDragSubscriber::button1Released()
{
    action = Action::NONE;
    return false;
}

bool PerspectiveDragSubscriber::mouseOver(int modifierKey)
{
    return false;
}

CursorShape PerspectiveDragSubscriber::getCursor(int objectID, int xPos, int yPos) const
{
    return CSHandOpen;
}

void PerspectiveDragSubscriber::switchOffEditMode()
{
    action = Action::NONE;
    if (getEditProvider()) {
        unsubscribe();
    }
}

// VertexGridSubscriber implementation

VertexGridSubscriber::VertexGridSubscriber()
    : EditSubscriber(ET_OBJECTS),
      canvas_area_(new EditRectangle())
{
    // Initialize grid vertex positions (10x10 grid at even intervals)
    for (int row = 0; row < GRID_SIZE; ++row) {
        for (int col = 0; col < GRID_SIZE; ++col) {
            int idx = row * GRID_SIZE + col;
            vertices_[idx].u = col / (double)(GRID_SIZE - 1);
            vertices_[idx].v = row / (double)(GRID_SIZE - 1);
            vertices_[idx].dx = 0;
            vertices_[idx].dy = 0;
            vertices_[idx].selected = false;
        }
    }

    // Keep the visible handles compact while using a larger invisible target
    // so modifier-click selection is forgiving at any display scale.
    for (int i = 0; i < VERTEX_COUNT; ++i) {
        auto circle = std::unique_ptr<Circle>(new Circle());
        circle->radius = HANDLE_RADIUS;
        circle->filled = true;
        circle->radiusInImageSpace = false;
        circle->datum = Geometry::IMAGE;
        circle->setActive(true);
        circle->setAutoColor(true);
        circle->opacity = 75.f;
        circle->state = Geometry::NORMAL;
        circle->innerLineWidth = 1.0f;
        visibleGeometry.push_back(circle.get());
        vertexCircles_.push_back(std::move(circle));

        auto hitCircle = std::unique_ptr<Circle>(new Circle());
        hitCircle->radius = HANDLE_HIT_RADIUS;
        hitCircle->filled = true;
        hitCircle->radiusInImageSpace = false;
        hitCircle->datum = Geometry::IMAGE;
        hitCircle->setActive(true);
        mouseOverGeometry.push_back(hitCircle.get());
        vertexHitCircles_.push_back(std::move(hitCircle));
    }

    // Create grid lines: horizontal + vertical segments
    for (int row = 0; row < GRID_SIZE; ++row) {
        for (int col = 0; col < GRID_SIZE - 1; ++col) {
            auto line = std::unique_ptr<Line>(new Line());
            line->innerLineWidth = 1.0f;
            line->setInnerLineColor(0.6, 0.6, 0.6);
            line->setOuterLineColor(0.15, 0.15, 0.15);
            line->opacity = 35.f;
            line->datum = Geometry::IMAGE;
            visibleGeometry.push_back(line.get());
            gridLines_.push_back(std::move(line));
        }
    }
    for (int col = 0; col < GRID_SIZE; ++col) {
        for (int row = 0; row < GRID_SIZE - 1; ++row) {
            auto line = std::unique_ptr<Line>(new Line());
            line->innerLineWidth = 1.0f;
            line->setInnerLineColor(0.6, 0.6, 0.6);
            line->setOuterLineColor(0.15, 0.15, 0.15);
            line->opacity = 35.f;
            line->datum = Geometry::IMAGE;
            visibleGeometry.push_back(line.get());
            gridLines_.push_back(std::move(line));
        }
    }

    // Invisible canvas for click-on-empty deselect (lowest priority)
    canvas_area_->filled = true;
    canvas_area_->topLeft = rtengine::Coord(0, 0);
    canvas_area_->bottomRight = rtengine::Coord(1, 1);
    mouseOverGeometry.insert(mouseOverGeometry.begin(), canvas_area_.get());
}

void VertexGridSubscriber::updateGeometry(int iw, int ih)
{
    for (int i = 0; i < VERTEX_COUNT; ++i) {
        double px = (vertices_[i].u + vertices_[i].dx) * iw;
        double py = (vertices_[i].v + vertices_[i].dy) * ih;
        vertexCircles_[i]->center = rtengine::Coord(static_cast<int>(px), static_cast<int>(py));
        vertexHitCircles_[i]->center = vertexCircles_[i]->center;
        vertexCircles_[i]->state = vertices_[i].selected ? Geometry::ACTIVE : Geometry::NORMAL;
    }

    // Update grid lines between adjacent vertices
    int li = 0;
    for (int r = 0; r < GRID_SIZE; ++r) {
        for (int c = 0; c < GRID_SIZE - 1; ++c) {
            int a = r * GRID_SIZE + c, b = a + 1;
            gridLines_[li]->begin = vertexCircles_[a]->center;
            gridLines_[li]->end   = vertexCircles_[b]->center;
            li++;
        }
    }
    for (int c = 0; c < GRID_SIZE; ++c) {
        for (int r = 0; r < GRID_SIZE - 1; ++r) {
            int a = r * GRID_SIZE + c, b = a + GRID_SIZE;
            gridLines_[li]->begin = vertexCircles_[a]->center;
            gridLines_[li]->end   = vertexCircles_[b]->center;
            li++;
        }
    }
}

void VertexGridSubscriber::setActive(bool active)
{
    EditDataProvider* provider = getEditProvider();
    if (!provider) return;

    bool isActive = (this == provider->getCurrSubscriber());
    if (isActive == active) return;

    if (active) {
        int iw, ih;
        provider->getImageSize(iw, ih);
        canvas_area_->bottomRight = rtengine::Coord(iw, ih);
        clearSelection();
        updateGeometry(iw, ih);
        subscribe();
    } else {
        unsubscribe();
    }
}

void VertexGridSubscriber::selectVertex(int index, bool addToSelection, bool rangeSelect)
{
    if (index < 0 || index >= VERTEX_COUNT) return;

    if (rangeSelect && lastSelectedIdx_ >= 0 && lastSelectedIdx_ < VERTEX_COUNT) {
        int row1 = lastSelectedIdx_ / GRID_SIZE;
        int col1 = lastSelectedIdx_ % GRID_SIZE;
        int row2 = index / GRID_SIZE;
        int col2 = index % GRID_SIZE;
        int rMin = std::min(row1, row2), rMax = std::max(row1, row2);
        int cMin = std::min(col1, col2), cMax = std::max(col1, col2);
        for (int r = rMin; r <= rMax; ++r) {
            for (int c = cMin; c <= cMax; ++c) {
                vertices_[r * GRID_SIZE + c].selected = true;
            }
        }
        // Keep the original anchor so consecutive Shift-clicks extend from
        // the first plain-clicked vertex instead of walking the anchor.
    } else if (addToSelection) {
        vertices_[index].selected = !vertices_[index].selected;
        lastSelectedIdx_ = index;
    } else {
        for (auto& vtx : vertices_) {
            vtx.selected = false;
        }
        vertices_[index].selected = true;
        lastSelectedIdx_ = index;
    }

    for (int i = 0; i < VERTEX_COUNT; ++i) {
        vertexCircles_[i]->state = vertices_[i].selected ? Geometry::ACTIVE : Geometry::NORMAL;
    }
}

void VertexGridSubscriber::clearSelection()
{
    for (auto& vtx : vertices_) {
        vtx.selected = false;
    }
    for (int i = 0; i < VERTEX_COUNT; ++i) {
        vertexCircles_[i]->state = Geometry::NORMAL;
    }
    lastSelectedIdx_ = -1;
}

int VertexGridSubscriber::vertexAtObject(int objectID) const
{
    if (objectID >= 1 && objectID <= VERTEX_COUNT) {
        return objectID - 1;
    }
    return -1;
}

bool VertexGridSubscriber::button1Pressed(int modifierKey)
{
    EditDataProvider* dp = getEditProvider();
    if (!dp || !perspective_) return false;

    // Merge both GTK modifier sources. Button-event state can lag behind a
    // modifier transition on some Windows and Linux input backends.
    GdkModifierType currentModifiers = static_cast<GdkModifierType>(0);
    if (gtk_get_current_event_state(&currentModifiers)) {
        modifierKey |= static_cast<int>(currentModifiers);
    }
    if (auto* display = gdk_display_get_default()) {
        if (auto* keymap = gdk_keymap_get_for_display(display)) {
            modifierKey |= static_cast<int>(gdk_keymap_get_modifier_state(keymap));
        }
    }

    int objectID = dp->getObject();
    int vertexIdx = vertexAtObject(objectID);
    didDrag_ = false;
    pendingSingleSelect_ = -1;
    const bool shift = (modifierKey & GDK_SHIFT_MASK) != 0;
    const bool ctrl = (modifierKey & GDK_CONTROL_MASK) != 0;
    const bool additive = shift || ctrl;

    if (vertexIdx >= 0) {
        if (!shift && !ctrl && vertices_[vertexIdx].selected) {
            // Clicked an already-selected vertex without modifiers:
            // defer single-select to release so drag keeps multi-selection
            pendingSingleSelect_ = vertexIdx;
        } else if (shift) {
            // Shift-click selects the inclusive rectangle between the original
            // anchor and this vertex. Existing selections remain active.
            selectVertex(vertexIdx, true, true);
        } else {
            // Ctrl-click toggles one vertex without clearing the selection.
            selectVertex(vertexIdx, ctrl, false);
        }

        // Save start displacements for all vertices
        for (int i = 0; i < VERTEX_COUNT; ++i) {
            startDx_[i] = vertices_[i].dx;
            startDy_[i] = vertices_[i].dy;
        }
        action = Action::DRAGGING;
    } else {
        // A narrow miss while adding points should not destroy the current
        // selection. Plain clicks on the canvas still clear it.
        if (!additive) {
            clearSelection();
            lastSelectedIdx_ = -1;
        }
        action = Action::NONE;
    }

    return true;
}

bool VertexGridSubscriber::drag1(int modifierKey)
{
    if (action != Action::DRAGGING) return false;

    EditDataProvider* dp = getEditProvider();
    if (!dp || !perspective_) return false;

    didDrag_ = true;
    pendingSingleSelect_ = -1; // cancel deferred single-select since user is dragging

    int iw, ih;
    dp->getImageSize(iw, ih);
    if (iw <= 0 || ih <= 0) return false;

    double ndx = dp->deltaImage.x / static_cast<double>(iw);
    double ndy = dp->deltaImage.y / static_cast<double>(ih);

    for (int i = 0; i < VERTEX_COUNT; ++i) {
        if (vertices_[i].selected) {
            vertices_[i].dx = rtengine::LIM(startDx_[i] + ndx, -0.5, 0.5);
            vertices_[i].dy = rtengine::LIM(startDy_[i] + ndy, -0.5, 0.5);
        }
    }
    updateGeometry(iw, ih);
    perspective_->meshChanged();
    return true;
}

bool VertexGridSubscriber::button1Released()
{
    // If user clicked an already-selected vertex without dragging,
    // deselect all others and select only that vertex
    if (pendingSingleSelect_ >= 0 && !didDrag_) {
        selectVertex(pendingSingleSelect_, false, false);
    }
    pendingSingleSelect_ = -1;
    didDrag_ = false;
    action = Action::NONE;
    return false;
}

bool VertexGridSubscriber::mouseOver(int modifierKey)
{
    return false;
}

CursorShape VertexGridSubscriber::getCursor(int objectID, int xPos, int yPos) const
{
    int vertexIdx = vertexAtObject(objectID);
    if (vertexIdx >= 0) {
        return CSHandOpen;
    }
    return CSArrow;
}

void VertexGridSubscriber::switchOffEditMode()
{
    action = Action::NONE;
    clearSelection();
    if (getEditProvider()) {
        unsubscribe();
    }
}
