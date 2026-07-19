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
#include "rgbcurves.h"

#include "guiutils.h"
#include "options.h"
#include "widgets/curves/curveeditor.h"
#include "widgets/curves/curveeditorgroup.h"

#include "rtengine/diagonalcurvetypes.h"
#include "rtengine/procparams.h"

using namespace rtengine;
using namespace rtengine::procparams;

const Glib::ustring RGBCurves::TOOL_NAME = "rgbcurves";

RGBCurves::RGBCurves () : FoldableToolPanel(this, TOOL_NAME, M("TP_RGBCURVES_LABEL"), false, true), lastLumamode(false)
{
    auto& options = App::get().mut_options();
    // Pass blank=1 with empty label to suppress the "Curves:" header text
    curveEditorG = new CurveEditorGroup (options.lastRgbCurvesDir, "", 1);
    curveEditorG->setCurveListener (this);
    curveEditorG->set_name("CurveToolGroup");

    // Master (All) curve — no gradient bars, white curve line
    Mshape = static_cast<DiagonalCurveEditor*>(curveEditorG->addCurve(CT_Diagonal, M("TP_RGBCURVES_ALL")));
    Mshape->setCurveDrawColor(0.85, 0.85, 0.85);  // white/light gray

    // Red channel — red curve line
    Rshape = static_cast<DiagonalCurveEditor*>(curveEditorG->addCurve(CT_Diagonal, M("TP_RGBCURVES_RED")));
    Rshape->setEditID(EUID_RGB_R, BT_SINGLEPLANE_FLOAT);
    Rshape->setCurveDrawColor(0.9, 0.2, 0.2);

    // Green channel — green curve line
    Gshape = static_cast<DiagonalCurveEditor*>(curveEditorG->addCurve(CT_Diagonal, M("TP_RGBCURVES_GREEN")));
    Gshape->setEditID(EUID_RGB_G, BT_SINGLEPLANE_FLOAT);
    Gshape->setCurveDrawColor(0.2, 0.8, 0.2);

    // Blue channel — blue curve line
    Bshape = static_cast<DiagonalCurveEditor*>(curveEditorG->addCurve(CT_Diagonal, M("TP_RGBCURVES_BLUE")));
    Bshape->setEditID(EUID_RGB_B, BT_SINGLEPLANE_FLOAT);
    Bshape->setCurveDrawColor(0.3, 0.4, 0.9);

    // Master curve shows R/G/B curves as overlays
    Mshape->setOverlayCurveEditors({Rshape, Gshape, Bshape});

    // This will add the reset button at the end of the curveType buttons
    curveEditorG->curveListComplete();

    // Hide the "Curves:" label and reset button for clean Lightroom look
    curveEditorG->hideHeaderWidgets();

    // Explicit channel buttons; right-click still opens the curve-type menu.
    Mshape->enableCompactMode("RGB", "curve-channel-master");
    Rshape->enableCompactMode("R", "curve-channel-red");
    Gshape->enableCompactMode("G", "curve-channel-green");
    Bshape->enableCompactMode("B", "curve-channel-blue");

    // Hide button boxes (+/-, edit, copy/paste/load/save) and coord adjusters (I:/O:)
    curveEditorG->setCompactDisplay(true);

    // Keep channel buttons compact and centered.
    for (auto* editor : {(CurveEditor*)Mshape, (CurveEditor*)Rshape, (CurveEditor*)Gshape, (CurveEditor*)Bshape}) {
        setExpandAlignProperties(editor->getButtonGroup(), false, false, Gtk::ALIGN_CENTER, Gtk::ALIGN_CENTER);
    }

    // Find the header row and center channels while keeping the cog right-aligned.
    const int PRIO = GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 200;
    Gtk::Grid* headerRow = nullptr;
    for (auto* child : curveEditorG->get_children()) {
        if (auto* grid = dynamic_cast<Gtk::Grid*>(child)) {
            headerRow = grid;
            break;
        }
    }
    if (headerRow) {
        setExpandAlignProperties(headerRow, true, false, Gtk::ALIGN_FILL, Gtk::ALIGN_CENTER);

        // Make the curveGroupLabel visible but empty, expanding to act as left spacer
        for (auto* child : headerRow->get_children()) {
            if (auto* lbl = dynamic_cast<Gtk::Label*>(child)) {
                lbl->set_no_show_all(false);
                lbl->set_label("");
                lbl->set_hexpand(true);
                lbl->show();
                break;
            }
        }

        // Right spacer balances the expanding label so the channel cluster
        // (and the cog directly after B) stays centered.
        auto* rightSpacer = Gtk::manage(new Gtk::Box());
        setExpandAlignProperties(rightSpacer, true, false, Gtk::ALIGN_FILL, Gtk::ALIGN_CENTER);
        headerRow->attach(*rightSpacer, 7, 0, 1, 1);

        // Create cog button — toggles editing controls visibility
        auto* cogBtn = Gtk::manage(new Gtk::Button());
        auto* cogLabel = Gtk::manage(new Gtk::Label("\xe2\x9a\x99")); // ⚙ gear symbol (U+2699)
        cogBtn->add(*cogLabel);
        cogBtn->set_relief(Gtk::RELIEF_NONE);
        cogBtn->set_can_focus(false);
        cogBtn->set_tooltip_text(M("TP_RGBCURVES_CHANNEL_OPTIONS"));
        cogBtn->get_style_context()->add_class("curve-cog-btn");
        {
            auto css = Gtk::CssProvider::create();
            try {
                css->load_from_data(
                    ".curve-cog-btn { min-height: 0; min-width: 0; padding: 1px 4px; margin: 0;"
                    "  background: transparent; background-image: none;"
                    "  border: none; box-shadow: none; }"
                    " .curve-cog-btn label { font-size: 16px; color: #999; }"
                    " .curve-cog-btn:hover label { color: #ddd; }");
                cogBtn->get_style_context()->add_provider(css, PRIO);
            } catch (...) {}
        }
        // The cog sits immediately to the right of the B channel chip
        setExpandAlignProperties(cogBtn, false, false, Gtk::ALIGN_START, Gtk::ALIGN_CENTER);
        cogBtn->set_margin_start(2);
        headerRow->attach(*cogBtn, 6, 0, 1, 1);

        cogBtn->signal_clicked().connect([this]() {
            curveEditorG->toggleCompactDisplay();
        });
    }

    // Shrink tool button icons when revealed via the cog
    {
        auto curveCss = Gtk::CssProvider::create();
        try {
            curveCss->load_from_data(
                "#CurveToolGroup .curve-buttonbox button { padding: 1px; margin: 0; "
                "  min-height: 18px; min-width: 18px; }"
                " #CurveToolGroup .curve-buttonbox image { -gtk-icon-size: 14px; }");
            Gtk::StyleContext::add_provider_for_screen(
                Gdk::Screen::get_default(), curveCss, PRIO);
        } catch (...) {}
    }

    // Keep the curve graph inside the right sidebar instead of letting GTK
    // expand it under the vertical scrollbar.
    curveEditorG->setCurveGraphSize(230);

    // Set all curves to Spline identity so the graph is always available
    const std::vector<double> splineIdentity{DCT_Spline, 0.0, 0.0, 1.0, 1.0};
    Mshape->setCurve(splineIdentity);
    Rshape->setCurve(splineIdentity);
    Gshape->setCurve(splineIdentity);
    Bshape->setCurve(splineIdentity);

    // Open the master curve by default
    Mshape->openIfNonlinear();

    pack_start (*curveEditorG, Gtk::PACK_SHRINK, 4);

}

RGBCurves::~RGBCurves ()
{
    delete curveEditorG;
}

void RGBCurves::read (const ProcParams* pp, const ParamsEdited* pedited)
{

    disableListener ();

    if (pedited) {
        Mshape->setUnChanged (!pedited->rgbCurves.mastercurve);
        Rshape->setUnChanged (!pedited->rgbCurves.rcurve);
        Gshape->setUnChanged (!pedited->rgbCurves.gcurve);
        Bshape->setUnChanged (!pedited->rgbCurves.bcurve);
        set_inconsistent(multiImage && !pedited->rgbCurves.enabled);
    }

    lastLumamode = pp->rgbCurves.lumamode;

    // Ensure all curves are Spline type (Lightroom-style: always show point curve)
    auto ensureSpline = [](const std::vector<double>& curve) -> std::vector<double> {
        if (curve.empty() || curve[0] == DCT_Linear) {
            return {DCT_Spline, 0.0, 0.0, 1.0, 1.0};
        }
        return curve;
    };

    Mshape->setCurve         (ensureSpline(pp->rgbCurves.mastercurve));
    Rshape->setCurve         (ensureSpline(pp->rgbCurves.rcurve));
    Gshape->setCurve         (ensureSpline(pp->rgbCurves.gcurve));
    Bshape->setCurve         (ensureSpline(pp->rgbCurves.bcurve));

    // Always open the master curve so the graph is visible (Lightroom-style)
    Mshape->openIfNonlinear();

    setEnabled(pp->rgbCurves.enabled);

    enableListener ();
}

void RGBCurves::setEditProvider (EditDataProvider *provider)
{
    Mshape->setEditProvider(provider);
    Rshape->setEditProvider(provider);
    Gshape->setEditProvider(provider);
    Bshape->setEditProvider(provider);
}

void RGBCurves::autoOpenCurve  ()
{
    // Open up the first curve if selected
    bool active = Mshape->openIfNonlinear();

    if (!active) {
        active = Rshape->openIfNonlinear();
    }

    if (!active) {
        active = Gshape->openIfNonlinear();
    }

    if (!active) {
        Bshape->openIfNonlinear();
    }
}

void RGBCurves::write (ProcParams* pp, ParamsEdited* pedited)
{
    pp->rgbCurves.enabled = getEnabled();
    pp->rgbCurves.mastercurve    = Mshape->getCurve ();
    pp->rgbCurves.rcurve         = Rshape->getCurve ();
    pp->rgbCurves.gcurve         = Gshape->getCurve ();
    pp->rgbCurves.bcurve         = Bshape->getCurve ();
    pp->rgbCurves.lumamode       = lastLumamode;  // preserve existing value

    if (pedited) {
        pedited->rgbCurves.enabled = !get_inconsistent();
        pedited->rgbCurves.mastercurve = !Mshape->isUnChanged ();
        pedited->rgbCurves.rcurve    = !Rshape->isUnChanged ();
        pedited->rgbCurves.gcurve    = !Gshape->isUnChanged ();
        pedited->rgbCurves.bcurve    = !Bshape->isUnChanged ();
        pedited->rgbCurves.lumamode  = false;
    }
}


/*
 * Curve listener
 *
 * If more than one curve has been added, the curve listener is automatically
 * set to 'multi=true', and send a pointer of the modified curve in a parameter
 */
void RGBCurves::curveChanged (CurveEditor* ce)
{

    if (listener && getEnabled()) {
        if (ce == Mshape) {
            listener->panelChanged (EvRGBMasterCurve, M("HISTORY_CUSTOMCURVE"));
        }

        if (ce == Rshape) {
            listener->panelChanged (EvRGBrCurve, M("HISTORY_CUSTOMCURVE"));
        }

        if (ce == Gshape) {
            listener->panelChanged (EvRGBgCurve, M("HISTORY_CUSTOMCURVE"));
        }

        if (ce == Bshape) {
            listener->panelChanged (EvRGBbCurve, M("HISTORY_CUSTOMCURVE"));
        }
    }
}

void RGBCurves::lumamodeChanged ()
{
}

void RGBCurves::setBatchMode (bool batchMode)
{

    ToolPanel::setBatchMode (batchMode);
    curveEditorG->setBatchMode (batchMode);
}


void RGBCurves::updateCurveBackgroundHistogram(
    const LUTu& histToneCurve,
    const LUTu& histLCurve,
    const LUTu& histCCurve,
    const LUTu& histLCAM,
    const LUTu& histCCAM,
    const LUTu& histRed,
    const LUTu& histGreen,
    const LUTu& histBlue,
    const LUTu& histLuma,
    const LUTu& histLRETI
)
{
    Mshape->updateBackgroundHistogram(histLuma);
    Rshape->updateBackgroundHistogram(histRed);
    Gshape->updateBackgroundHistogram(histGreen);
    Bshape->updateBackgroundHistogram(histBlue);
}


void RGBCurves::enabledChanged()
{
    if (listener) {
        if (get_inconsistent()) {
            listener->panelChanged(EvRGBEnabled, M("GENERAL_UNCHANGED"));
        } else if (getEnabled()) {
            listener->panelChanged(EvRGBEnabled, M("GENERAL_ENABLED"));
        } else {
            listener->panelChanged(EvRGBEnabled, M("GENERAL_DISABLED"));
        }
    }
}
