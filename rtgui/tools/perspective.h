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
#pragma once

#include "controllines.h"
#include "editcallbacks.h"
#include "lensgeomlistener.h"
#include "toolpanel.h"
#include "widgets/basic/adjuster.h"

#include "rtengine/tweakoperator.h"

#include <array>
#include <gtkmm.h>

class Circle;
class EditRectangle;
class Line;
class PerspCorrection;

// Interactive drag-based perspective editing.
// Drag vertically to adjust vertical perspective, horizontally for horizontal.
// Displays a reference grid overlay on the image preview.
class PerspectiveDragSubscriber : public EditSubscriber
{
public:
    PerspectiveDragSubscriber();

    void setCallback(PerspCorrection* persp) { perspective_ = persp; }
    void setActive(bool active);

    // EditSubscriber overrides
    bool button1Pressed(int modifierKey) override;
    bool button1Released() override;
    bool drag1(int modifierKey) override;
    bool mouseOver(int modifierKey) override;
    CursorShape getCursor(int objectID, int xPos, int yPos) const override;
    void switchOffEditMode() override;

private:
    static constexpr int GRID_LINES = 4;  // lines per axis (at 20/40/60/80%)

    void updateGridGeometry(int iw, int ih, double hPersp, double vPersp);

    PerspCorrection* perspective_ = nullptr;
    std::unique_ptr<EditRectangle> canvas_area_;
    std::vector<std::unique_ptr<Line>> gridLines_;  // perspective grid
    std::unique_ptr<Line> centerH_;  // center crosshair horizontal
    std::unique_ptr<Line> centerV_;  // center crosshair vertical
    double startHoriz_ = 0;
    double startVert_ = 0;
};

// Interactive vertex grid perspective editing.
// Displays a 7x7 grid of selectable/draggable vertices on the image.
// Users select vertices (especially near corners) and drag to adjust perspective.
class VertexGridSubscriber : public EditSubscriber
{
public:
    VertexGridSubscriber();

    void setCallback(PerspCorrection* persp) { perspective_ = persp; }
    void setActive(bool active);

    // EditSubscriber overrides
    bool button1Pressed(int modifierKey) override;
    bool button1Released() override;
    bool drag1(int modifierKey) override;
    bool mouseOver(int modifierKey) override;
    CursorShape getCursor(int objectID, int xPos, int yPos) const override;
    void switchOffEditMode() override;

private:
    static constexpr int GRID_SIZE = 10;
    static constexpr int VERTEX_COUNT = GRID_SIZE * GRID_SIZE;
    static constexpr int HANDLE_RADIUS = 3;

    struct GridVertex {
        double u, v;           // base position [0,1]²
        double dx = 0, dy = 0; // displacement (normalized)
        bool selected = false;
    };

    void updateGeometry(int iw, int ih);
    void selectVertex(int index, bool addToSelection, bool rangeSelect);
    void clearSelection();
    int vertexAtObject(int objectID) const;

    PerspCorrection* perspective_ = nullptr;
    std::unique_ptr<EditRectangle> canvas_area_;
    std::array<GridVertex, VERTEX_COUNT> vertices_;
    std::vector<std::unique_ptr<Circle>> vertexCircles_;
    std::vector<std::unique_ptr<Line>> gridLines_;
    std::array<double, VERTEX_COUNT> startDx_{}, startDy_{};
    int lastSelectedIdx_ = -1;  // for range selection
    int pendingSingleSelect_ = -1; // deferred single-select on release (if no drag)
    bool didDrag_ = false;         // true once drag1 fires after button1Pressed

    friend class PerspCorrection;
};

class PerspCorrectionPanelListener
{
public:
    virtual ~PerspCorrectionPanelListener() = default;

    virtual void controlLineEditModeChanged(bool active) = 0;
};

class PerspCorrection final :
    public rtengine::TweakOperator,
    public ToolParamBlock,
    public AdjusterListener,
    public FoldableToolPanel
{
    friend class PerspectiveDragSubscriber;
    friend class VertexGridSubscriber;

protected:
    bool render = true;
    MyComboBoxText* method;
    Gtk::Box* simple;
    Adjuster* horiz;
    Adjuster* vert;
    Gtk::Button* auto_pitch;
    Gtk::Button* auto_yaw;
    Gtk::Button* auto_pitch_yaw;
    Gtk::Box* camera_based;
    Adjuster* camera_crop_factor;
    Adjuster* camera_focal_length;
    Adjuster* camera_pitch;
    Adjuster* camera_roll;
    Adjuster* camera_shift_horiz;
    Adjuster* camera_shift_vert;
    Adjuster* camera_yaw;
    std::unique_ptr<ControlLineManager> lines;
    std::unique_ptr<PerspectiveDragSubscriber> dragSubscriber_;
    std::unique_ptr<VertexGridSubscriber> gridSubscriber_;
    Gtk::Button* lines_button_apply;
    Gtk::ToggleButton* lines_button_edit;
    Gtk::Button* lines_button_erase;
    Adjuster* projection_pitch;
    Adjuster* projection_rotate;
    Adjuster* projection_shift_horiz;
    Adjuster* projection_shift_vert;
    Adjuster* projection_yaw;
    rtengine::ProcEvent EvPerspCamFocalLength;
    rtengine::ProcEvent EvPerspCamShift;
    rtengine::ProcEvent EvPerspCamAngle;
    rtengine::ProcEvent EvPerspControlLines;
    rtengine::ProcEvent EvPerspMethod;
    rtengine::ProcEvent EvPerspProjShift;
    rtengine::ProcEvent EvPerspProjRotate;
    rtengine::ProcEvent EvPerspProjAngle;
    rtengine::ProcEvent EvPerspRender;
    rtengine::ProcEvent EvPerspCamFocalLengthVoid;
    rtengine::ProcEvent EvPerspCamShiftVoid;
    rtengine::ProcEvent EvPerspCamAngleVoid;
    rtengine::ProcEvent EvPerspProjShiftVoid;
    rtengine::ProcEvent EvPerspProjRotateVoid;
    rtengine::ProcEvent EvPerspProjAngleVoid;
    rtengine::ProcEvent EvPerspMeshWarp;
    rtengine::ProcEvent* event_persp_cam_focal_length;
    rtengine::ProcEvent* event_persp_cam_shift;
    rtengine::ProcEvent* event_persp_cam_angle;
    rtengine::ProcEvent* event_persp_proj_shift;
    rtengine::ProcEvent* event_persp_proj_rotate;
    rtengine::ProcEvent* event_persp_proj_angle;
    LensGeomListener* lens_geom_listener;
    PerspCorrectionPanelListener* panel_listener;
    const rtengine::FramesMetaData* metadata;
    AdvancedSection* advancedSection;

    void applyControlLines (void);
    void tweakParams(rtengine::procparams::ProcParams &pparams) override;
    void setCamBasedEventsActive (bool active = true);
    void setFocalLengthValue (const rtengine::procparams::ProcParams* pparams, const rtengine::FramesMetaData* metadata);
    void updateApplyDeleteButtons();

public:

    /** Minimum number of horizontal lines for horizontal/full correction. */
    static constexpr std::size_t MIN_HORIZ_LINES = 2;
    /** Minimum number of vertical lines for vertical/full correction. */
    static constexpr std::size_t MIN_VERT_LINES = 2;
    static const Glib::ustring TOOL_NAME;

    PerspCorrection ();

    void read           (const rtengine::procparams::ProcParams* pp, const ParamsEdited* pedited = nullptr) override;
    void write          (rtengine::procparams::ProcParams* pp, ParamsEdited* pedited = nullptr) override;
    void setDefaults    (const rtengine::procparams::ProcParams* defParams, const ParamsEdited* pedited = nullptr) override;
    void setBatchMode   (bool batchMode) override;

    void adjusterChanged (Adjuster* a, double newval) override;
    void autoCorrectionPressed (Gtk::Button* b);
    void lineChanged (void);
    void linesApplyButtonPressed (void);
    void linesEditButtonPressed (void);
    void linesEraseButtonPressed (void);
    void methodChanged (void);
    void requestApplyControlLines(void);
    void setAdjusterBehavior (bool badd,
        bool camera_focal_length_add,
        bool camera_shift_add,
        bool camera_angle_add,
        bool projection_angle_add,
        bool projection_shift_add,
        bool projection_rotate_add);
    void meshChanged();
    void setControlLineEditMode(bool active);
    void setDragEditMode(bool active);
    void setGridEditMode(bool active);
    void setEditProvider (EditDataProvider* provider) override;
    void setLensGeomListener (LensGeomListener* listener)
    {
        lens_geom_listener = listener;
    }
    void setPerspCorrectionPanelListener(PerspCorrectionPanelListener* listener)
    {
        panel_listener = listener;
    }
    void setMetadata (const rtengine::FramesMetaData* metadata);
    void switchOffEditMode (void);
    void hideAdvancedSection ();
    void exposeAutoButtons ();
    void runAutoCorrection ();
    void trimValues          (rtengine::procparams::ProcParams* pp) override;
};

class LinesCallbacks: public ControlLineManager::Callbacks
{
protected:
    PerspCorrection* tool;

public:
    explicit LinesCallbacks(PerspCorrection* tool);
    void lineChanged (void) override;
    void switchOffEditMode (void) override;
};
