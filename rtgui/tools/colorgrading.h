/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2024 RawTherapee team
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

#include <gtkmm.h>

#include "toolpanel.h"
#include "widgets/basic/adjuster.h"
#include "widgets/basic/colorwheel.h"

class ColorGrading final :
    public ToolParamBlock,
    public FoldableToolPanel,
    public ColorWheelListener,
    public AdjusterListener
{
public:
    static const Glib::ustring TOOL_NAME;

    ColorGrading();

    void read(const rtengine::procparams::ProcParams* pp, const ParamsEdited* pedited = nullptr) override;
    void write(rtengine::procparams::ProcParams* pp, ParamsEdited* pedited = nullptr) override;
    void setDefaults(const rtengine::procparams::ProcParams* defParams, const ParamsEdited* pedited = nullptr) override;
    void setBatchMode(bool batchMode) override;
    void trimValues(rtengine::procparams::ProcParams* pp) override;

    void adjusterChanged(Adjuster* a, double newval) override;
    void colorWheelChanged(ColorWheel* source, double hue, double saturation) override;
    void enabledChanged() override;

private:
    ColorWheel* shadowsWheel;
    ColorWheel* midtonesWheel;
    ColorWheel* highlightsWheel;
    ColorWheel* globalWheel;

    Adjuster* shadowsLum;
    Adjuster* midtonesLum;
    Adjuster* highlightsLum;
    Adjuster* globalLum;
    Adjuster* blending;
    Adjuster* balance;

    AdvancedSection* advancedSection;

    Gtk::Label* sectionLabel_;
    Gtk::Box* toolContent_;
    bool contentExpanded_;
    void toggleContent();
};
