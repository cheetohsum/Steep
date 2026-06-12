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
#include "defringe.h"

#include "eventmapper.h"
#include "options.h"
#include "widgets/curves/curveeditor.h"
#include "widgets/curves/curveeditorgroup.h"

#include "rtengine/color.h"
#include "rtengine/procparams.h"

#include <cmath>
#include <iomanip>

using namespace rtengine;
using namespace rtengine::procparams;

const Glib::ustring Defringe::TOOL_NAME = "defringe";

Defringe::Defringe () : FoldableToolPanel(this, TOOL_NAME, M("TP_DEFRINGE_LABEL"), false, true)
{
    auto m = ProcEventMapper::getInstance();
    EvDefringePurpleAmount = m->newEvent(DEMOSAIC, "HISTORY_MSG_DEFRINGE_PURPLE_AMOUNT");
    EvDefringePurpleHueLow = m->newEvent(DEMOSAIC, "HISTORY_MSG_DEFRINGE_PURPLE_HUE_LOW");
    EvDefringePurpleHueHigh = m->newEvent(DEMOSAIC, "HISTORY_MSG_DEFRINGE_PURPLE_HUE_HIGH");
    EvDefringeGreenAmount = m->newEvent(DEMOSAIC, "HISTORY_MSG_DEFRINGE_GREEN_AMOUNT");
    EvDefringeGreenHueLow = m->newEvent(DEMOSAIC, "HISTORY_MSG_DEFRINGE_GREEN_HUE_LOW");
    EvDefringeGreenHueHigh = m->newEvent(DEMOSAIC, "HISTORY_MSG_DEFRINGE_GREEN_HUE_HIGH");

    defringeExpanded = false;
    hueExpanded = false;

    // Keep curve objects alive for compatibility but don't display them
    std::vector<GradientMilestone> bottomMilestones;
    float R, G, B;
    for (int i = 0; i < 7; i++) {
        float x = i / 6.f;
        Color::hsv2rgb01(x, 0.5f, 0.5f, R, G, B);
        bottomMilestones.push_back( GradientMilestone(double(x), double(R), double(G), double(B)) );
    }

    auto& options = App::get().mut_options();
    curveEditorPF = new CurveEditorGroup (options.lastPFCurvesDir);
    curveEditorPF->setCurveListener (this);
    chshape = static_cast<FlatCurveEditor*>(curveEditorPF->addCurve(CT_Flat, M("TP_PFCURVE_CURVEEDITOR_CH")));
    chshape->setTooltip(M("TP_PFCURVE_CURVEEDITOR_CH_TOOLTIP"));
    chshape->setIdentityValue(0.);
    chshape->setBottomBarBgGradient(bottomMilestones);
    chshape->setCurveColorProvider(this, 1);
    curveEditorPF->curveListComplete();
    // Curve is NOT packed — kept only for param serialization

    radius  = Gtk::manage (new Adjuster (M("TP_DEFRINGE_RADIUS"), 0.5, 5.0, 0.1, 2.0));
    threshold    = Gtk::manage (new Adjuster (M("TP_DEFRINGE_THRESHOLD"), 0, 100, 1, 13));
    radius->setAdjusterListener (this);
    threshold->setAdjusterListener (this);

    auto *summaryBox = getSummaryBox();

    // --- Collapsible Defringe header ---
    defringeLabel = Gtk::manage(new Gtk::Label());
    defringeLabel->set_markup(Glib::ustring("<b>\u25B8 ") + M("TP_DEFRINGE_LABEL") + "</b>");
    defringeLabel->set_halign(Gtk::ALIGN_START);
    defringeLabel->set_margin_top(4);
    defringeLabel->set_margin_bottom(2);

    Gtk::Button *defringeHeader = Gtk::manage(new Gtk::Button());
    defringeHeader->set_name("CollapsibleHeader");
    defringeHeader->set_relief(Gtk::RELIEF_NONE);
    defringeHeader->set_can_focus(false);
    defringeHeader->set_halign(Gtk::ALIGN_START);
    defringeHeader->add(*defringeLabel);
    defringeHeader->signal_clicked().connect(
        sigc::mem_fun(*this, &Defringe::toggleDefringe));
    summaryBox->pack_start(*defringeHeader, Gtk::PACK_SHRINK);

    // Content box for all defringe content (hidden by default)
    defringeContent = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
    defringeContent->set_no_show_all(true);

    defringeContent->pack_start(*radius);
    defringeContent->pack_start(*threshold);

    // --- Collapsible Hue section (nested inside defringe content) ---
    hueLabel = Gtk::manage(new Gtk::Label());
    hueLabel->set_markup(Glib::ustring("<b>\u25B8 ") + M("TP_DEFRINGE_HUE_SECTION") + "</b>");
    hueLabel->set_halign(Gtk::ALIGN_START);
    hueLabel->set_margin_top(8);
    hueLabel->set_margin_bottom(2);

    Gtk::Button *hueHeader = Gtk::manage(new Gtk::Button());
    hueHeader->set_name("CollapsibleHeader");
    hueHeader->set_relief(Gtk::RELIEF_NONE);
    hueHeader->set_can_focus(false);
    hueHeader->set_halign(Gtk::ALIGN_START);
    hueHeader->add(*hueLabel);
    hueHeader->signal_clicked().connect(
        sigc::mem_fun(*this, &Defringe::toggleHue));
    defringeContent->pack_start(*hueHeader, Gtk::PACK_SHRINK);

    hueContent = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
    hueContent->set_no_show_all(true);

    // --- Purple/Green colored dot toggle buttons ---
    Gtk::Box *colorBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    colorBox->set_margin_top(4);
    colorBox->set_margin_bottom(2);

    purpleBtn = Gtk::manage(new Gtk::ToggleButton());
    purpleBtn->set_active(true);
    purpleBtn->set_size_request(20, 20);
    purpleBtn->set_label("P");

    greenBtn = Gtk::manage(new Gtk::ToggleButton());
    greenBtn->set_active(false);
    greenBtn->set_size_request(20, 20);
    greenBtn->set_label("G");

    colorBox->pack_start(*purpleBtn, Gtk::PACK_SHRINK);
    colorBox->pack_start(*greenBtn, Gtk::PACK_SHRINK);
    hueContent->pack_start(*colorBox, Gtk::PACK_SHRINK);

    // --- Purple adjusters ---
    purpleBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
    purpleAmount = Gtk::manage(new Adjuster(M("TP_DEFRINGE_AMOUNT"), 0., 20., 0.1, 0.));
    purpleAmount->setAdjusterListener(this);
    purpleBox->pack_start(*purpleAmount);

    // Purple hue gradient: magenta-pink → deep purple → blue-purple
    purpleHueLow = Gtk::manage(new Adjuster(M("TP_DEFRINGE_HUE_LOW"), 0., 100., 1., 30.));
    purpleHueLow->setAdjusterListener(this);
    std::vector<GradientMilestone> purpleGrad;
    purpleGrad.push_back(GradientMilestone(0.0, 0.85, 0.20, 0.55));  // magenta-pink
    purpleGrad.push_back(GradientMilestone(0.25, 0.70, 0.15, 0.75)); // reddish purple
    purpleGrad.push_back(GradientMilestone(0.5, 0.55, 0.10, 0.85));  // purple
    purpleGrad.push_back(GradientMilestone(0.75, 0.40, 0.15, 0.90)); // blue-purple
    purpleGrad.push_back(GradientMilestone(1.0, 0.25, 0.20, 0.85));  // indigo
    purpleHueLow->setSliderGradient(purpleGrad);
    purpleBox->pack_start(*purpleHueLow);

    purpleHueHigh = Gtk::manage(new Adjuster(M("TP_DEFRINGE_HUE_HIGH"), 0., 100., 1., 70.));
    purpleHueHigh->setAdjusterListener(this);
    purpleHueHigh->setSliderGradient(purpleGrad);
    purpleBox->pack_start(*purpleHueHigh);
    hueContent->pack_start(*purpleBox);

    // --- Green adjusters ---
    greenBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
    greenAmount = Gtk::manage(new Adjuster(M("TP_DEFRINGE_AMOUNT"), 0., 20., 0.1, 0.));
    greenAmount->setAdjusterListener(this);
    greenBox->pack_start(*greenAmount);

    // Green hue gradient: yellow-green → green → teal-green
    greenHueLow = Gtk::manage(new Adjuster(M("TP_DEFRINGE_HUE_LOW"), 0., 100., 1., 30.));
    greenHueLow->setAdjusterListener(this);
    std::vector<GradientMilestone> greenGrad;
    greenGrad.push_back(GradientMilestone(0.0, 0.60, 0.75, 0.10));   // yellow-green
    greenGrad.push_back(GradientMilestone(0.25, 0.35, 0.80, 0.15));  // lime green
    greenGrad.push_back(GradientMilestone(0.5, 0.15, 0.75, 0.30));   // green
    greenGrad.push_back(GradientMilestone(0.75, 0.10, 0.65, 0.45));  // teal-green
    greenGrad.push_back(GradientMilestone(1.0, 0.10, 0.55, 0.55));   // cyan-green
    greenHueLow->setSliderGradient(greenGrad);
    greenBox->pack_start(*greenHueLow);

    greenHueHigh = Gtk::manage(new Adjuster(M("TP_DEFRINGE_HUE_HIGH"), 0., 100., 1., 70.));
    greenHueHigh->setAdjusterListener(this);
    greenHueHigh->setSliderGradient(greenGrad);
    greenBox->pack_start(*greenHueHigh);
    hueContent->pack_start(*greenBox);

    // Green hidden by default within hue section
    greenBox->set_no_show_all(true);

    defringeContent->pack_start(*hueContent, Gtk::PACK_SHRINK);
    summaryBox->pack_start(*defringeContent, Gtk::PACK_SHRINK);

    summaryBox->show_all();

    purpleBtn->signal_toggled().connect(sigc::mem_fun(*this, &Defringe::purpleBtnToggled));
    greenBtn->signal_toggled().connect(sigc::mem_fun(*this, &Defringe::greenBtnToggled));
}

Defringe::~Defringe ()
{
    delete curveEditorPF;
}

void Defringe::purpleBtnToggled()
{
    if (purpleBtn->get_active()) {
        greenBtn->set_active(false);
    } else if (!greenBtn->get_active()) {
        purpleBtn->set_active(true);
        return;
    }
    updateModeDisplay();
}

void Defringe::greenBtnToggled()
{
    if (greenBtn->get_active()) {
        purpleBtn->set_active(false);
    } else if (!purpleBtn->get_active()) {
        greenBtn->set_active(true);
        return;
    }
    updateModeDisplay();
}

void Defringe::updateModeDisplay()
{
    if (purpleBtn->get_active()) {
        purpleBox->set_no_show_all(false);
        purpleBox->show_all();
        greenBox->set_no_show_all(true);
        greenBox->hide();
    } else {
        purpleBox->set_no_show_all(true);
        purpleBox->hide();
        greenBox->set_no_show_all(false);
        greenBox->show_all();
    }
}

void Defringe::toggleDefringe()
{
    defringeExpanded = !defringeExpanded;
    if (defringeExpanded) {
        defringeLabel->set_markup(Glib::ustring("<b>\u25BE ") + M("TP_DEFRINGE_LABEL") + "</b>");
        defringeContent->set_no_show_all(false);
        defringeContent->show_all();
        defringeContent->set_no_show_all(true);
        // Restore hue collapsed state
        if (!hueExpanded) {
            hueContent->hide();
        } else {
            updateModeDisplay();
        }
    } else {
        defringeLabel->set_markup(Glib::ustring("<b>\u25B8 ") + M("TP_DEFRINGE_LABEL") + "</b>");
        defringeContent->hide();
    }
}

void Defringe::toggleHue()
{
    hueExpanded = !hueExpanded;
    if (hueExpanded) {
        hueLabel->set_markup(Glib::ustring("<b>\u25BE ") + M("TP_DEFRINGE_HUE_SECTION") + "</b>");
        hueContent->set_no_show_all(false);
        hueContent->show_all();
        hueContent->set_no_show_all(true);
        // Restore correct purple/green visibility
        updateModeDisplay();
    } else {
        hueLabel->set_markup(Glib::ustring("<b>\u25B8 ") + M("TP_DEFRINGE_HUE_SECTION") + "</b>");
        hueContent->hide();
    }
}

void Defringe::colorForValue (double valX, double valY, enum ColorCaller::ElemType elemType, int callerId, ColorCaller *caller)
{

    float R = 0.f, G = 0.f, B = 0.f;

    if (elemType == ColorCaller::CCET_VERTICAL_BAR) {
        valY = 0.5;
    }

    if (callerId == 1) { // ch
        Color::hsv2rgb01(float(valX), float(valY), 0.5f, R, G, B);
    }

    caller->ccRed = double(R);
    caller->ccGreen = double(G);
    caller->ccBlue = double(B);
}

void Defringe::read (const ProcParams* pp, const ParamsEdited* pedited)
{

    disableListener ();

    if (pedited) {
        radius->setEditedState    ( pedited->defringe.radius ? Edited : UnEdited);
        threshold->setEditedState ( pedited->defringe.threshold ? Edited : UnEdited);
        set_inconsistent          (multiImage && !pedited->defringe.enabled);
        chshape->setUnChanged     (!pedited->defringe.huecurve);
        purpleAmount->setEditedState(pedited->defringe.purpleAmount ? Edited : UnEdited);
        purpleHueLow->setEditedState(pedited->defringe.purpleHueLow ? Edited : UnEdited);
        purpleHueHigh->setEditedState(pedited->defringe.purpleHueHigh ? Edited : UnEdited);
        greenAmount->setEditedState(pedited->defringe.greenAmount ? Edited : UnEdited);
        greenHueLow->setEditedState(pedited->defringe.greenHueLow ? Edited : UnEdited);
        greenHueHigh->setEditedState(pedited->defringe.greenHueHigh ? Edited : UnEdited);
    }

    setEnabled(pp->defringe.enabled);

    radius->setValue    (pp->defringe.radius);
    threshold->setValue (pp->defringe.threshold);
    chshape->setCurve   (pp->defringe.huecurve);
    purpleAmount->setValue(pp->defringe.purpleAmount);
    purpleHueLow->setValue(pp->defringe.purpleHueLow);
    purpleHueHigh->setValue(pp->defringe.purpleHueHigh);
    greenAmount->setValue(pp->defringe.greenAmount);
    greenHueLow->setValue(pp->defringe.greenHueLow);
    greenHueHigh->setValue(pp->defringe.greenHueHigh);

    enableListener ();
}


void Defringe::autoOpenCurve ()
{
    // WARNING: The following line won't work, since linear is a flat curve at 0.
    // chshape->openIfNonlinear();
}

void Defringe::write (ProcParams* pp, ParamsEdited* pedited)
{

    pp->defringe.radius    = radius->getValue ();
    pp->defringe.threshold = (int)threshold->getValue ();
    pp->defringe.enabled   = getEnabled();
    pp->defringe.huecurve  = chshape->getCurve ();
    pp->defringe.purpleAmount = purpleAmount->getValue();
    pp->defringe.purpleHueLow = purpleHueLow->getValue();
    pp->defringe.purpleHueHigh = purpleHueHigh->getValue();
    pp->defringe.greenAmount = greenAmount->getValue();
    pp->defringe.greenHueLow = greenHueLow->getValue();
    pp->defringe.greenHueHigh = greenHueHigh->getValue();

    if (pedited) {
        pedited->defringe.radius    = radius->getEditedState ();
        pedited->defringe.threshold = threshold->getEditedState ();
        pedited->defringe.enabled   = !get_inconsistent();
        pedited->defringe.huecurve  = !chshape->isUnChanged ();
        pedited->defringe.purpleAmount = purpleAmount->getEditedState();
        pedited->defringe.purpleHueLow = purpleHueLow->getEditedState();
        pedited->defringe.purpleHueHigh = purpleHueHigh->getEditedState();
        pedited->defringe.greenAmount = greenAmount->getEditedState();
        pedited->defringe.greenHueLow = greenHueLow->getEditedState();
        pedited->defringe.greenHueHigh = greenHueHigh->getEditedState();
    }
}

void Defringe::setDefaults (const ProcParams* defParams, const ParamsEdited* pedited)
{

    radius->setDefault (defParams->defringe.radius);
    threshold->setDefault (defParams->defringe.threshold);
    purpleAmount->setDefault(defParams->defringe.purpleAmount);
    purpleHueLow->setDefault(defParams->defringe.purpleHueLow);
    purpleHueHigh->setDefault(defParams->defringe.purpleHueHigh);
    greenAmount->setDefault(defParams->defringe.greenAmount);
    greenHueLow->setDefault(defParams->defringe.greenHueLow);
    greenHueHigh->setDefault(defParams->defringe.greenHueHigh);

    if (pedited) {
        radius->setDefaultEditedState (pedited->defringe.radius ? Edited : UnEdited);
        threshold->setDefaultEditedState (pedited->defringe.threshold ? Edited : UnEdited);
        purpleAmount->setDefaultEditedState(pedited->defringe.purpleAmount ? Edited : UnEdited);
        purpleHueLow->setDefaultEditedState(pedited->defringe.purpleHueLow ? Edited : UnEdited);
        purpleHueHigh->setDefaultEditedState(pedited->defringe.purpleHueHigh ? Edited : UnEdited);
        greenAmount->setDefaultEditedState(pedited->defringe.greenAmount ? Edited : UnEdited);
        greenHueLow->setDefaultEditedState(pedited->defringe.greenHueLow ? Edited : UnEdited);
        greenHueHigh->setDefaultEditedState(pedited->defringe.greenHueHigh ? Edited : UnEdited);
    } else {
        radius->setDefaultEditedState (Irrelevant);
        threshold->setDefaultEditedState (Irrelevant);
        purpleAmount->setDefaultEditedState(Irrelevant);
        purpleHueLow->setDefaultEditedState(Irrelevant);
        purpleHueHigh->setDefaultEditedState(Irrelevant);
        greenAmount->setDefaultEditedState(Irrelevant);
        greenHueLow->setDefaultEditedState(Irrelevant);
        greenHueHigh->setDefaultEditedState(Irrelevant);
    }
}
void Defringe::curveChanged ()
{

    if (listener && getEnabled()) {
        listener->panelChanged (EvPFCurve, M("HISTORY_CUSTOMCURVE"));
    }
}

void Defringe::adjusterChanged(Adjuster* a, double newval)
{
    autoEnable();
    if (listener && getEnabled()) {

        if (a == radius) {
            listener->panelChanged (EvDefringeRadius, Glib::ustring::format (std::setw(2), std::fixed, std::setprecision(1), a->getValue()));
        } else if (a == threshold) {
            listener->panelChanged (EvDefringeThreshold, Glib::ustring::format ((int)a->getValue()));
        } else if (a == purpleAmount) {
            listener->panelChanged(EvDefringePurpleAmount, a->getTextValue());
        } else if (a == purpleHueLow) {
            listener->panelChanged(EvDefringePurpleHueLow, a->getTextValue());
        } else if (a == purpleHueHigh) {
            listener->panelChanged(EvDefringePurpleHueHigh, a->getTextValue());
        } else if (a == greenAmount) {
            listener->panelChanged(EvDefringeGreenAmount, a->getTextValue());
        } else if (a == greenHueLow) {
            listener->panelChanged(EvDefringeGreenHueLow, a->getTextValue());
        } else if (a == greenHueHigh) {
            listener->panelChanged(EvDefringeGreenHueHigh, a->getTextValue());
        }
    }
}

void Defringe::enabledChanged ()
{
    if (listener) {
        if (get_inconsistent()) {
            listener->panelChanged (EvDefringeEnabled, M("GENERAL_UNCHANGED"));
        } else if (getEnabled()) {
            listener->panelChanged (EvDefringeEnabled, M("GENERAL_ENABLED"));
        } else {
            listener->panelChanged (EvDefringeEnabled, M("GENERAL_DISABLED"));
        }
    }
}

void Defringe::setBatchMode (bool batchMode)
{

    ToolPanel::setBatchMode (batchMode);
    radius->showEditedCB ();
    threshold->showEditedCB ();
    purpleAmount->showEditedCB();
    purpleHueLow->showEditedCB();
    purpleHueHigh->showEditedCB();
    greenAmount->showEditedCB();
    greenHueLow->showEditedCB();
    greenHueHigh->showEditedCB();
}
