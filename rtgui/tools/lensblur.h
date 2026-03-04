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

#include "toolpanel.h"
#include "widgets/basic/adjuster.h"
#include "guiutils.h"

#include <gtkmm.h>

class LensBlur final : public ToolParamBlock, public AdjusterListener, public FoldableToolPanel
{
private:
    Adjuster *amount;
    Adjuster *cateye;
    Adjuster *bokeh;
    Adjuster *depth;
    Adjuster *range;

    // Bokeh shape selector
    Gtk::ToggleButton *shapeCircle;
    Gtk::ToggleButton *shape5blade;
    Gtk::ToggleButton *shape6blade;
    Gtk::ToggleButton *shape7blade;
    Gtk::ToggleButton *shape8blade;

    // Collapsible section containers
    Gtk::Box *bokehContent;
    Gtk::Box *focusContent;
    Gtk::Label *bokehLabel;
    Gtk::Label *focusLabel;
    bool bokehExpanded;
    bool focusExpanded;

    rtengine::ProcEvent EvLensBlurEnabled;
    rtengine::ProcEvent EvLensBlurAmount;
    rtengine::ProcEvent EvLensBlurShape;
    rtengine::ProcEvent EvLensBlurCatEye;
    rtengine::ProcEvent EvLensBlurBokeh;
    rtengine::ProcEvent EvLensBlurDepth;
    rtengine::ProcEvent EvLensBlurRange;

    void shapeToggled(Gtk::ToggleButton *btn, const Glib::ustring &shapeName);
    void toggleBokeh();
    void toggleFocus();

public:
    static const Glib::ustring TOOL_NAME;

    LensBlur();

    void read(const rtengine::procparams::ProcParams *pp, const ParamsEdited *pedited = nullptr) override;
    void write(rtengine::procparams::ProcParams *pp, ParamsEdited *pedited = nullptr) override;
    void setDefaults(const rtengine::procparams::ProcParams *defParams, const ParamsEdited *pedited = nullptr) override;
    void setBatchMode(bool batchMode) override;

    void adjusterChanged(Adjuster *a, double newval) override;
    void enabledChanged() override;
};
