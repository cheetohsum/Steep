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

#include <array>

#include <gtkmm.h>

#include "editcallbacks.h"
#include "toolpanel.h"
#include "widgets/basic/adjuster.h"

/// A virtual light placed on the photograph. It reads the AI subject mask and
/// the estimated scene depth, so it can model the subject's surfaces, light it
/// apart from its surroundings and move the background on its own.
class Relight final :
    public ToolParamBlock,
    public AdjusterListener,
    public FoldableToolPanel,
    public EditSubscriber
{
public:
    static const Glib::ustring TOOL_NAME;

    Relight();
    ~Relight() override;

    void read(const rtengine::procparams::ProcParams* pp, const ParamsEdited* pedited = nullptr) override;
    void write(rtengine::procparams::ProcParams* pp, ParamsEdited* pedited = nullptr) override;
    void setDefaults(const rtengine::procparams::ProcParams* defParams, const ParamsEdited* pedited = nullptr) override;
    void setBatchMode(bool batchMode) override;
    void enabledChanged() override;

    void adjusterChanged(Adjuster* a, double newval) override;
    void adjusterAutoToggled(Adjuster* a, bool newval) override {}

    void setEditProvider(EditDataProvider* provider) override;

    // EditSubscriber interface: the light is a handle on the image.
    CursorShape getCursor(int objectID, int xPos, int yPos) const override;
    bool mouseOver(int modifierKey) override;
    bool button1Pressed(int modifierKey) override;
    bool button1Released() override;
    bool drag1(int modifierKey) override;
    void switchOffEditMode() override;

private:
    Adjuster* intensity;
    Adjuster* lightX;
    Adjuster* lightY;
    Adjuster* distance;
    Adjuster* reach;
    Adjuster* subjectFocus;
    Adjuster* background;
    Adjuster* shadows;
    Adjuster* shape;
    Adjuster* warmth;
    Gtk::ToggleButton* edit;
    sigc::connection editConn;

    int lastObject;
    rtengine::Coord draggedLight;

    std::array<Adjuster*, 10> adjusters() const;
    void editToggled();
    void updateGeometry();

    rtengine::ProcEvent EvEnabled;
    rtengine::ProcEvent EvIntensity;
    rtengine::ProcEvent EvPosition;
    rtengine::ProcEvent EvDistance;
    rtengine::ProcEvent EvReach;
    rtengine::ProcEvent EvSubjectFocus;
    rtengine::ProcEvent EvBackground;
    rtengine::ProcEvent EvShadows;
    rtengine::ProcEvent EvShape;
    rtengine::ProcEvent EvWarmth;
};
