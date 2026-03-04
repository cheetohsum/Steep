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
#include "lensblur.h"

#include "eventmapper.h"
#include "rtimage.h"

#include "rtengine/procparams.h"

using namespace rtengine;
using namespace rtengine::procparams;

const Glib::ustring LensBlur::TOOL_NAME = "lensblur";

LensBlur::LensBlur(): FoldableToolPanel(this, TOOL_NAME, M("TP_LENSBLUR_LABEL"), false, true)
{
    auto m = ProcEventMapper::getInstance();
    EvLensBlurEnabled = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_LENSBLUR_ENABLED");
    EvLensBlurAmount = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_LENSBLUR_AMOUNT");
    EvLensBlurShape = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_LENSBLUR_SHAPE");
    EvLensBlurCatEye = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_LENSBLUR_CATEYE");
    EvLensBlurBokeh = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_LENSBLUR_BOKEH");
    EvLensBlurDepth = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_LENSBLUR_DEPTH");
    EvLensBlurRange = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_LENSBLUR_RANGE");

    auto *summaryBox = getSummaryBox();

    bokehExpanded = false;
    focusExpanded = false;

    // Main blur amount slider
    amount = Gtk::manage(new Adjuster(M("TP_LENSBLUR_AMOUNT"), 0., 100., 1., 0.));
    amount->setAdjusterListener(this);
    summaryBox->pack_start(*amount);

    // --- Bokeh section (clickable header) ---
    bokehLabel = Gtk::manage(new Gtk::Label());
    bokehLabel->set_markup(Glib::ustring("<b>\u25B8 ") + M("TP_LENSBLUR_BOKEH_SECTION") + "</b>");
    bokehLabel->set_halign(Gtk::ALIGN_START);
    bokehLabel->set_margin_top(8);
    bokehLabel->set_margin_bottom(2);

    Gtk::Button *bokehHeader = Gtk::manage(new Gtk::Button());
    bokehHeader->set_name("CollapsibleHeader");
    bokehHeader->set_relief(Gtk::RELIEF_NONE);
    bokehHeader->set_can_focus(false);
    bokehHeader->set_halign(Gtk::ALIGN_START);
    bokehHeader->add(*bokehLabel);
    bokehHeader->signal_clicked().connect(
        sigc::mem_fun(*this, &LensBlur::toggleBokeh));
    summaryBox->pack_start(*bokehHeader, Gtk::PACK_SHRINK);

    // Bokeh content (hidden by default)
    bokehContent = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
    bokehContent->set_no_show_all(true);

    // Bokeh shape icon buttons
    Gtk::Box *shapeBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 1));
    shapeBox->set_halign(Gtk::ALIGN_CENTER);

    shapeCircle = Gtk::manage(new Gtk::ToggleButton());
    shapeCircle->add(*Gtk::manage(new RTImage("bokeh-circle", Gtk::ICON_SIZE_BUTTON)));
    shapeCircle->set_tooltip_text(M("TP_LENSBLUR_SHAPE_CIRCLE"));
    shapeCircle->set_active(true);
    shapeCircle->set_relief(Gtk::RELIEF_NONE);

    shape5blade = Gtk::manage(new Gtk::ToggleButton());
    shape5blade->add(*Gtk::manage(new RTImage("bokeh-5blade", Gtk::ICON_SIZE_BUTTON)));
    shape5blade->set_tooltip_text(M("TP_LENSBLUR_SHAPE_5BLADE"));
    shape5blade->set_relief(Gtk::RELIEF_NONE);

    shape6blade = Gtk::manage(new Gtk::ToggleButton());
    shape6blade->add(*Gtk::manage(new RTImage("bokeh-6blade", Gtk::ICON_SIZE_BUTTON)));
    shape6blade->set_tooltip_text(M("TP_LENSBLUR_SHAPE_6BLADE"));
    shape6blade->set_relief(Gtk::RELIEF_NONE);

    shape7blade = Gtk::manage(new Gtk::ToggleButton());
    shape7blade->add(*Gtk::manage(new RTImage("bokeh-7blade", Gtk::ICON_SIZE_BUTTON)));
    shape7blade->set_tooltip_text(M("TP_LENSBLUR_SHAPE_7BLADE"));
    shape7blade->set_relief(Gtk::RELIEF_NONE);

    shape8blade = Gtk::manage(new Gtk::ToggleButton());
    shape8blade->add(*Gtk::manage(new RTImage("bokeh-8blade", Gtk::ICON_SIZE_BUTTON)));
    shape8blade->set_tooltip_text(M("TP_LENSBLUR_SHAPE_8BLADE"));
    shape8blade->set_relief(Gtk::RELIEF_NONE);

    shapeBox->pack_start(*shapeCircle, Gtk::PACK_SHRINK);
    shapeBox->pack_start(*shape5blade, Gtk::PACK_SHRINK);
    shapeBox->pack_start(*shape6blade, Gtk::PACK_SHRINK);
    shapeBox->pack_start(*shape7blade, Gtk::PACK_SHRINK);
    shapeBox->pack_start(*shape8blade, Gtk::PACK_SHRINK);
    bokehContent->pack_start(*shapeBox, Gtk::PACK_SHRINK);

    shapeCircle->signal_toggled().connect(sigc::bind(sigc::mem_fun(*this, &LensBlur::shapeToggled), shapeCircle, Glib::ustring("circle")));
    shape5blade->signal_toggled().connect(sigc::bind(sigc::mem_fun(*this, &LensBlur::shapeToggled), shape5blade, Glib::ustring("5blade")));
    shape6blade->signal_toggled().connect(sigc::bind(sigc::mem_fun(*this, &LensBlur::shapeToggled), shape6blade, Glib::ustring("6blade")));
    shape7blade->signal_toggled().connect(sigc::bind(sigc::mem_fun(*this, &LensBlur::shapeToggled), shape7blade, Glib::ustring("7blade")));
    shape8blade->signal_toggled().connect(sigc::bind(sigc::mem_fun(*this, &LensBlur::shapeToggled), shape8blade, Glib::ustring("8blade")));

    cateye = Gtk::manage(new Adjuster(M("TP_LENSBLUR_CATEYE"), 0., 100., 1., 0.));
    cateye->setAdjusterListener(this);
    bokehContent->pack_start(*cateye);

    bokeh = Gtk::manage(new Adjuster(M("TP_LENSBLUR_BOKEH"), 0., 100., 1., 50.));
    bokeh->setAdjusterListener(this);
    bokehContent->pack_start(*bokeh);

    summaryBox->pack_start(*bokehContent, Gtk::PACK_SHRINK);

    // --- Focus Range section (clickable header) ---
    focusLabel = Gtk::manage(new Gtk::Label());
    focusLabel->set_markup(Glib::ustring("<b>\u25B8 ") + M("TP_LENSBLUR_FOCUSRANGE_SECTION") + "</b>");
    focusLabel->set_halign(Gtk::ALIGN_START);
    focusLabel->set_margin_top(8);
    focusLabel->set_margin_bottom(2);

    Gtk::Button *focusHeader = Gtk::manage(new Gtk::Button());
    focusHeader->set_name("CollapsibleHeader");
    focusHeader->set_relief(Gtk::RELIEF_NONE);
    focusHeader->set_can_focus(false);
    focusHeader->set_halign(Gtk::ALIGN_START);
    focusHeader->add(*focusLabel);
    focusHeader->signal_clicked().connect(
        sigc::mem_fun(*this, &LensBlur::toggleFocus));
    summaryBox->pack_start(*focusHeader, Gtk::PACK_SHRINK);

    // Focus content (hidden by default)
    focusContent = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
    focusContent->set_no_show_all(true);

    depth = Gtk::manage(new Adjuster(M("TP_LENSBLUR_DEPTH"), 0., 100., 1., 50.));
    depth->setAdjusterListener(this);
    std::vector<GradientMilestone> depthGrad;
    depthGrad.push_back(GradientMilestone(0.0, 0.15, 0.15, 0.15));
    depthGrad.push_back(GradientMilestone(1.0, 0.85, 0.85, 0.85));
    depth->setSliderGradient(depthGrad);
    focusContent->pack_start(*depth);

    Gtk::Box *nearFarBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL));
    Gtk::Label *nearLabel = Gtk::manage(new Gtk::Label(M("TP_LENSBLUR_NEAR")));
    nearLabel->set_halign(Gtk::ALIGN_START);
    nearLabel->get_style_context()->add_class("dim-label");
    Gtk::Label *farLabel = Gtk::manage(new Gtk::Label(M("TP_LENSBLUR_FAR")));
    farLabel->set_halign(Gtk::ALIGN_END);
    farLabel->get_style_context()->add_class("dim-label");
    nearFarBox->pack_start(*nearLabel, Gtk::PACK_EXPAND_WIDGET);
    nearFarBox->pack_end(*farLabel, Gtk::PACK_EXPAND_WIDGET);
    nearFarBox->set_margin_top(-2);
    focusContent->pack_start(*nearFarBox, Gtk::PACK_SHRINK);

    range = Gtk::manage(new Adjuster(M("TP_LENSBLUR_RANGE"), 0., 100., 1., 30.));
    range->setAdjusterListener(this);
    focusContent->pack_start(*range);

    summaryBox->pack_start(*focusContent, Gtk::PACK_SHRINK);

    summaryBox->show_all();
}

void LensBlur::shapeToggled(Gtk::ToggleButton *btn, const Glib::ustring &shapeName)
{
    static bool shapeUpdating = false;

    if (shapeUpdating) {
        return;
    }

    if (!btn->get_active()) {
        // Programmatic deactivation — ignore
        return;
    }

    shapeUpdating = true;

    // Deactivate all other shape buttons
    Gtk::ToggleButton *allBtns[] = {shapeCircle, shape5blade, shape6blade, shape7blade, shape8blade};
    for (auto *b : allBtns) {
        if (b != btn) {
            b->set_active(false);
        }
    }

    shapeUpdating = false;

    autoEnable();
    if (listener && getEnabled()) {
        listener->panelChanged(EvLensBlurShape, shapeName);
    }
}

void LensBlur::read(const ProcParams *pp, const ParamsEdited *pedited)
{
    disableListener();

    if (pedited) {
        amount->setEditedState(pedited->lensBlur.amount ? Edited : UnEdited);
        cateye->setEditedState(pedited->lensBlur.cateye ? Edited : UnEdited);
        bokeh->setEditedState(pedited->lensBlur.bokeh ? Edited : UnEdited);
        depth->setEditedState(pedited->lensBlur.depth ? Edited : UnEdited);
        range->setEditedState(pedited->lensBlur.range ? Edited : UnEdited);
        set_inconsistent(multiImage && !pedited->lensBlur.enabled);
    }

    setEnabled(pp->lensBlur.enabled);
    amount->setValue(pp->lensBlur.amount);
    cateye->setValue(pp->lensBlur.cateye);
    bokeh->setValue(pp->lensBlur.bokeh);
    depth->setValue(pp->lensBlur.depth);
    range->setValue(pp->lensBlur.range);

    // Set active shape button (use lambda to batch-update without triggering signals)
    {
        // Block signals by deactivating all first, then activating the right one
        Gtk::ToggleButton *allBtns[] = {shapeCircle, shape5blade, shape6blade, shape7blade, shape8blade};
        for (auto *b : allBtns) {
            b->set_active(false);
        }
        if (pp->lensBlur.shape == "5blade") shape5blade->set_active(true);
        else if (pp->lensBlur.shape == "6blade") shape6blade->set_active(true);
        else if (pp->lensBlur.shape == "7blade") shape7blade->set_active(true);
        else if (pp->lensBlur.shape == "8blade") shape8blade->set_active(true);
        else shapeCircle->set_active(true);
    }

    enableListener();
}

void LensBlur::write(ProcParams *pp, ParamsEdited *pedited)
{
    pp->lensBlur.amount = amount->getValue();
    pp->lensBlur.cateye = cateye->getValue();
    pp->lensBlur.bokeh = bokeh->getValue();
    pp->lensBlur.depth = depth->getValue();
    pp->lensBlur.range = range->getValue();
    pp->lensBlur.enabled = getEnabled();

    if (shape5blade->get_active()) pp->lensBlur.shape = "5blade";
    else if (shape6blade->get_active()) pp->lensBlur.shape = "6blade";
    else if (shape7blade->get_active()) pp->lensBlur.shape = "7blade";
    else if (shape8blade->get_active()) pp->lensBlur.shape = "8blade";
    else pp->lensBlur.shape = "circle";

    if (pedited) {
        pedited->lensBlur.amount = amount->getEditedState();
        pedited->lensBlur.shape = true;
        pedited->lensBlur.cateye = cateye->getEditedState();
        pedited->lensBlur.bokeh = bokeh->getEditedState();
        pedited->lensBlur.depth = depth->getEditedState();
        pedited->lensBlur.range = range->getEditedState();
        pedited->lensBlur.enabled = !get_inconsistent();
    }
}

void LensBlur::setDefaults(const ProcParams *defParams, const ParamsEdited *pedited)
{
    amount->setDefault(defParams->lensBlur.amount);
    cateye->setDefault(defParams->lensBlur.cateye);
    bokeh->setDefault(defParams->lensBlur.bokeh);
    depth->setDefault(defParams->lensBlur.depth);
    range->setDefault(defParams->lensBlur.range);

    if (pedited) {
        amount->setDefaultEditedState(pedited->lensBlur.amount ? Edited : UnEdited);
        cateye->setDefaultEditedState(pedited->lensBlur.cateye ? Edited : UnEdited);
        bokeh->setDefaultEditedState(pedited->lensBlur.bokeh ? Edited : UnEdited);
        depth->setDefaultEditedState(pedited->lensBlur.depth ? Edited : UnEdited);
        range->setDefaultEditedState(pedited->lensBlur.range ? Edited : UnEdited);
    } else {
        amount->setDefaultEditedState(Irrelevant);
        cateye->setDefaultEditedState(Irrelevant);
        bokeh->setDefaultEditedState(Irrelevant);
        depth->setDefaultEditedState(Irrelevant);
        range->setDefaultEditedState(Irrelevant);
    }
}

void LensBlur::adjusterChanged(Adjuster *a, double newval)
{
    autoEnable();
    if (listener && getEnabled()) {
        if (a == amount) {
            listener->panelChanged(EvLensBlurAmount, a->getTextValue());
        } else if (a == cateye) {
            listener->panelChanged(EvLensBlurCatEye, a->getTextValue());
        } else if (a == bokeh) {
            listener->panelChanged(EvLensBlurBokeh, a->getTextValue());
        } else if (a == depth) {
            listener->panelChanged(EvLensBlurDepth, a->getTextValue());
        } else if (a == range) {
            listener->panelChanged(EvLensBlurRange, a->getTextValue());
        }
    }
}

void LensBlur::toggleBokeh()
{
    bokehExpanded = !bokehExpanded;
    if (bokehExpanded) {
        bokehLabel->set_markup(Glib::ustring("<b>\u25BE ") + M("TP_LENSBLUR_BOKEH_SECTION") + "</b>");
        bokehContent->set_no_show_all(false);
        bokehContent->show_all();
        bokehContent->set_no_show_all(true);
    } else {
        bokehLabel->set_markup(Glib::ustring("<b>\u25B8 ") + M("TP_LENSBLUR_BOKEH_SECTION") + "</b>");
        bokehContent->hide();
    }
}

void LensBlur::toggleFocus()
{
    focusExpanded = !focusExpanded;
    if (focusExpanded) {
        focusLabel->set_markup(Glib::ustring("<b>\u25BE ") + M("TP_LENSBLUR_FOCUSRANGE_SECTION") + "</b>");
        focusContent->set_no_show_all(false);
        focusContent->show_all();
        focusContent->set_no_show_all(true);
    } else {
        focusLabel->set_markup(Glib::ustring("<b>\u25B8 ") + M("TP_LENSBLUR_FOCUSRANGE_SECTION") + "</b>");
        focusContent->hide();
    }
}

void LensBlur::enabledChanged()
{
    if (listener) {
        if (get_inconsistent()) {
            listener->panelChanged(EvLensBlurEnabled, M("GENERAL_UNCHANGED"));
        } else if (getEnabled()) {
            listener->panelChanged(EvLensBlurEnabled, M("GENERAL_ENABLED"));
        } else {
            listener->panelChanged(EvLensBlurEnabled, M("GENERAL_DISABLED"));
        }
    }
}

void LensBlur::setBatchMode(bool batchMode)
{
    ToolPanel::setBatchMode(batchMode);

    amount->showEditedCB();
    cateye->showEditedCB();
    bokeh->showEditedCB();
    depth->showEditedCB();
    range->showEditedCB();
}
