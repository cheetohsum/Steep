/** -*- C++ -*-
 *
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

#include "editcallbacks.h"
#include "guiutils.h"
#include "toolpanel.h"
#include "widgets/basic/adjuster.h"

#include <gtkmm.h>

class TiltShift final :
    public ToolParamBlock,
    public AdjusterListener,
    public FoldableToolPanel,
    public EditSubscriber
{
private:
    int lastObject;

    Gtk::Box *editHBox;
    Gtk::ToggleButton *edit;
    Adjuster *amount;
    Adjuster *focusPos;
    Adjuster *focusWidth;
    Adjuster *feather;
    Adjuster *angle;
    Gtk::Box* detailContent_;
    Gtk::Revealer* detailRevealer_;
    bool detailExpanded_;
    sigc::connection editConn;

    // Drag state
    rtengine::Coord draggedCenter;
    double draggedFeatherOffset;
    bool dragRotating_;          // true when dragging near line endpoints to rotate
    double dragStartAngle_;      // angle at drag start (degrees)

    rtengine::ProcEvent EvTiltShiftEnabled;
    rtengine::ProcEvent EvTiltShiftAmount;
    rtengine::ProcEvent EvTiltShiftFocusPos;
    rtengine::ProcEvent EvTiltShiftFocusWidth;
    rtengine::ProcEvent EvTiltShiftFeather;
    rtengine::ProcEvent EvTiltShiftAngle;

    void toggleDetail();
    void editToggled();
    void releaseEdit();

public:
    static const Glib::ustring TOOL_NAME;

    TiltShift();
    ~TiltShift() override;

    void read(const rtengine::procparams::ProcParams *pp, const ParamsEdited *pedited=nullptr) override;
    void write(rtengine::procparams::ProcParams *pp, ParamsEdited *pedited=nullptr) override;
    void setDefaults(const rtengine::procparams::ProcParams *defParams, const ParamsEdited *pedited=nullptr) override;
    void setBatchMode(bool batchMode) override;

    void adjusterChanged(Adjuster *a, double newval) override;
    void enabledChanged() override;

    void updateGeometry(int focusPosVal, int focusWidthVal, int featherVal, int angleVal, int fullWidth=-1, int fullHeight=-1);

    void setEditProvider(EditDataProvider *provider) override;

    // EditSubscriber interface
    CursorShape getCursor(int objectID, int xPos, int yPos) const override;
    bool mouseOver(int modifierKey) override;
    bool button1Pressed(int modifierKey) override;
    bool button1Released() override;
    bool drag1(int modifierKey) override;
    void switchOffEditMode() override;
};
