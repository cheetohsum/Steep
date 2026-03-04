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
 *
 *  2010 Ilya Popov <ilia_popov@rambler.ru>
 */
#pragma once

#include <array>
#include <gtkmm.h>

#include "colorprovider.h"
#include "curvelistener.h"
#include "guiutils.h"
#include "toolpanel.h"
#include "widgets/basic/adjuster.h"

class CurveEditor;
class CurveEditorGroup;
class FlatCurveEditor;

class HSVEqualizer final :
    public ToolParamBlock,
    public FoldableToolPanel,
    public CurveListener,
    public ColorProvider,
    public AdjusterListener
{

protected:

    CurveEditorGroup*  curveEditorG;
    FlatCurveEditor*   hshape;
    FlatCurveEditor*   sshape;
    FlatCurveEditor*   vshape;

    // Color dot channel selector
    Gtk::Box* channelBar;
    std::array<Gtk::DrawingArea*, 8> channelDots;
    int activeChannel;

    // Per-channel H/S/L sliders
    Adjuster* hueAdj;
    Adjuster* satAdj;
    Adjuster* lumAdj;

    // Internal storage for all 8 channels
    std::array<double, 8> hueValues;
    std::array<double, 8> satValues;
    std::array<double, 8> lumValues;

    AdvancedSection* advancedSection;

    Gtk::Label* sectionLabel_;
    Gtk::Box* toolContent_;
    bool contentExpanded_;
    void toggleContent();

    void onChannelSelected(int channel);
    void updateActiveSliderGradients();
    std::vector<double> slidersToFlatCurve(const std::array<double, 8>& shifts) const;

public:
    static const Glib::ustring TOOL_NAME;

    HSVEqualizer ();
    ~HSVEqualizer () override;

    void read            (const rtengine::procparams::ProcParams* pp, const ParamsEdited* pedited = nullptr) override;
    void write           (rtengine::procparams::ProcParams* pp, ParamsEdited* pedited = nullptr) override;
    void curveChanged    (CurveEditor* ce) override;
    void setBatchMode    (bool batchMode) override;
    void setEditProvider (EditDataProvider *provider) override;
    void autoOpenCurve   () override;
    void colorForValue (double valX, double valY, enum ColorCaller::ElemType elemType, int callerId, ColorCaller* caller) override;

    void adjusterChanged(Adjuster* a, double newval) override;
    void enabledChanged() override;
};
