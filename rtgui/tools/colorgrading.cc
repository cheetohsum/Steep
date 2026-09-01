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
#include "colorgrading.h"

#include <iomanip>

#include "options.h"

#include "rtengine/procparams.h"

using namespace rtengine;
using namespace rtengine::procparams;

const Glib::ustring ColorGrading::TOOL_NAME = "colorgrading";

ColorGrading::ColorGrading() : FoldableToolPanel(this, TOOL_NAME, M("TP_COLORGRADING_LABEL"), false, true),
    contentExpanded_(false)
{
    // Clickable section label (collapsed by default)
    sectionLabel_ = Gtk::manage(new Gtk::Label());
    sectionLabel_->set_markup("<b>\xe2\x96\xb8 Grading</b>");
    sectionLabel_->set_xalign(0.0);
    sectionLabel_->get_style_context()->add_class("tool-heading-label");
    auto* labelEvt = Gtk::manage(new Gtk::EventBox());
    labelEvt->add(*sectionLabel_);
    labelEvt->signal_button_press_event().connect([this](GdkEventButton*) -> bool {
        toggleContent();
        return true;
    });
    getSummaryBox()->pack_start(*labelEvt, Gtk::PACK_SHRINK, 2);

    // Collapsible content box
    toolContent_ = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 0));

    // Three color wheels in a horizontal box
    auto* wheelsBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 4));
    wheelsBox->set_homogeneous(true);

    shadowsWheel = Gtk::manage(new ColorWheel(M("TP_COLORGRADING_SHADOWS")));
    shadowsWheel->setListener(this);
    wheelsBox->pack_start(*shadowsWheel, Gtk::PACK_EXPAND_WIDGET, 0);

    midtonesWheel = Gtk::manage(new ColorWheel(M("TP_COLORGRADING_MIDTONES")));
    midtonesWheel->setListener(this);
    wheelsBox->pack_start(*midtonesWheel, Gtk::PACK_EXPAND_WIDGET, 0);

    highlightsWheel = Gtk::manage(new ColorWheel(M("TP_COLORGRADING_HIGHLIGHTS")));
    highlightsWheel->setListener(this);
    wheelsBox->pack_start(*highlightsWheel, Gtk::PACK_EXPAND_WIDGET, 0);

    toolContent_->pack_start(*wheelsBox, Gtk::PACK_SHRINK, 0);

    // Luminance adjusters stacked vertically under the wheels
    shadowsLum = Gtk::manage(new Adjuster(M("TP_COLORGRADING_SHADOW_LUM"), -100., 100., 1., 0.));
    shadowsLum->setAdjusterListener(this);
    toolContent_->pack_start(*shadowsLum, Gtk::PACK_SHRINK, 0);

    midtonesLum = Gtk::manage(new Adjuster(M("TP_COLORGRADING_MID_LUM"), -100., 100., 1., 0.));
    midtonesLum->setAdjusterListener(this);
    toolContent_->pack_start(*midtonesLum, Gtk::PACK_SHRINK, 0);

    highlightsLum = Gtk::manage(new Adjuster(M("TP_COLORGRADING_HIGH_LUM"), -100., 100., 1., 0.));
    highlightsLum->setAdjusterListener(this);
    toolContent_->pack_start(*highlightsLum, Gtk::PACK_SHRINK, 0);

    // Blending and Balance
    blending = Gtk::manage(new Adjuster(M("TP_COLORGRADING_BLENDING"), 0., 100., 1., 50.));
    blending->setAdjusterListener(this);
    toolContent_->pack_start(*blending, Gtk::PACK_SHRINK, 0);

    balance = Gtk::manage(new Adjuster(M("TP_COLORGRADING_BALANCE"), -100., 100., 1., 0.));
    balance->setAdjusterListener(this);
    toolContent_->pack_start(*balance, Gtk::PACK_SHRINK, 0);

    // Start hidden
    toolContent_->set_no_show_all(true);
    toolContent_->hide();
    getSummaryBox()->pack_start(*toolContent_, Gtk::PACK_SHRINK, 0);
    getSummaryBox()->show_all();

    // Advanced section with global wheel (visible only when content expanded)
    advancedSection = Gtk::manage(new AdvancedSection());
    advancedSection->set_no_show_all(true);
    advancedSection->hide();
    pack_start(*advancedSection, Gtk::PACK_SHRINK, 0);
    Gtk::Box* const advBox = advancedSection->getContentBox();

    auto* globalBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 0));
    globalWheel = Gtk::manage(new ColorWheel(M("TP_COLORGRADING_GLOBAL")));
    globalWheel->setListener(this);
    globalWheel->set_size_request(80, -1);
    globalBox->pack_start(*globalWheel, Gtk::PACK_SHRINK, 0);
    advBox->pack_start(*globalBox, Gtk::PACK_SHRINK, 0);

    globalLum = Gtk::manage(new Adjuster(M("TP_COLORGRADING_GLOBAL_LUM"), -100., 100., 1., 0.));
    globalLum->setAdjusterListener(this);
    advBox->pack_start(*globalLum, Gtk::PACK_SHRINK, 0);

    show_all_children();
}

void ColorGrading::toggleContent()
{
    contentExpanded_ = !contentExpanded_;
    if (contentExpanded_) {
        sectionLabel_->set_markup("<b>\xe2\x96\xbe Grading</b>");
        toolContent_->set_no_show_all(false);
        toolContent_->show_all();
        toolContent_->set_no_show_all(true);
        advancedSection->set_no_show_all(false);
        advancedSection->show_all();
        advancedSection->set_no_show_all(true);
    } else {
        sectionLabel_->set_markup("<b>\xe2\x96\xb8 Grading</b>");
        toolContent_->hide();
        advancedSection->hide();
    }
}

void ColorGrading::read(const ProcParams* pp, const ParamsEdited* pedited)
{
    disableListener();

    if (pedited) {
        set_inconsistent(multiImage && !pedited->colorGrading.enabled);
        shadowsLum->setEditedState(pedited->colorGrading.shadowsLum ? Edited : UnEdited);
        midtonesLum->setEditedState(pedited->colorGrading.midtonesLum ? Edited : UnEdited);
        highlightsLum->setEditedState(pedited->colorGrading.highlightsLum ? Edited : UnEdited);
        globalLum->setEditedState(pedited->colorGrading.globalLum ? Edited : UnEdited);
        blending->setEditedState(pedited->colorGrading.blending ? Edited : UnEdited);
        balance->setEditedState(pedited->colorGrading.balance ? Edited : UnEdited);
    }

    setEnabled(pp->colorGrading.enabled);
    shadowsWheel->setHueSat(pp->colorGrading.shadowsHue, pp->colorGrading.shadowsSat, false);
    midtonesWheel->setHueSat(pp->colorGrading.midtonesHue, pp->colorGrading.midtonesSat, false);
    highlightsWheel->setHueSat(pp->colorGrading.highlightsHue, pp->colorGrading.highlightsSat, false);
    globalWheel->setHueSat(pp->colorGrading.globalHue, pp->colorGrading.globalSat, false);

    shadowsLum->setValue(pp->colorGrading.shadowsLum);
    midtonesLum->setValue(pp->colorGrading.midtonesLum);
    highlightsLum->setValue(pp->colorGrading.highlightsLum);
    globalLum->setValue(pp->colorGrading.globalLum);
    blending->setValue(pp->colorGrading.blending);
    balance->setValue(pp->colorGrading.balance);

    enableListener();
}

void ColorGrading::write(ProcParams* pp, ParamsEdited* pedited)
{
    pp->colorGrading.enabled = getEnabled();
    pp->colorGrading.shadowsHue = shadowsWheel->getHue();
    pp->colorGrading.shadowsSat = shadowsWheel->getSaturation();
    pp->colorGrading.midtonesHue = midtonesWheel->getHue();
    pp->colorGrading.midtonesSat = midtonesWheel->getSaturation();
    pp->colorGrading.highlightsHue = highlightsWheel->getHue();
    pp->colorGrading.highlightsSat = highlightsWheel->getSaturation();
    pp->colorGrading.globalHue = globalWheel->getHue();
    pp->colorGrading.globalSat = globalWheel->getSaturation();
    pp->colorGrading.shadowsLum = shadowsLum->getValue();
    pp->colorGrading.midtonesLum = midtonesLum->getValue();
    pp->colorGrading.highlightsLum = highlightsLum->getValue();
    pp->colorGrading.globalLum = globalLum->getValue();
    pp->colorGrading.blending = blending->getValue();
    pp->colorGrading.balance = balance->getValue();

    if (pedited) {
        pedited->colorGrading.enabled = !get_inconsistent();
        pedited->colorGrading.shadowsHue = shadowsWheel->getEdited();
        pedited->colorGrading.shadowsSat = shadowsWheel->getEdited();
        pedited->colorGrading.shadowsLum = shadowsLum->getEditedState();
        pedited->colorGrading.midtonesHue = midtonesWheel->getEdited();
        pedited->colorGrading.midtonesSat = midtonesWheel->getEdited();
        pedited->colorGrading.midtonesLum = midtonesLum->getEditedState();
        pedited->colorGrading.highlightsHue = highlightsWheel->getEdited();
        pedited->colorGrading.highlightsSat = highlightsWheel->getEdited();
        pedited->colorGrading.highlightsLum = highlightsLum->getEditedState();
        pedited->colorGrading.globalHue = globalWheel->getEdited();
        pedited->colorGrading.globalSat = globalWheel->getEdited();
        pedited->colorGrading.globalLum = globalLum->getEditedState();
        pedited->colorGrading.blending = blending->getEditedState();
        pedited->colorGrading.balance = balance->getEditedState();
    }
}

void ColorGrading::setDefaults(const ProcParams* defParams, const ParamsEdited* pedited)
{
    shadowsLum->setDefault(defParams->colorGrading.shadowsLum);
    midtonesLum->setDefault(defParams->colorGrading.midtonesLum);
    highlightsLum->setDefault(defParams->colorGrading.highlightsLum);
    globalLum->setDefault(defParams->colorGrading.globalLum);
    blending->setDefault(defParams->colorGrading.blending);
    balance->setDefault(defParams->colorGrading.balance);
}

void ColorGrading::setBatchMode(bool batchMode)
{
    ToolPanel::setBatchMode(batchMode);
    shadowsLum->showEditedCB();
    midtonesLum->showEditedCB();
    highlightsLum->showEditedCB();
    globalLum->showEditedCB();
    blending->showEditedCB();
    balance->showEditedCB();
}

void ColorGrading::trimValues(ProcParams* pp)
{
    shadowsLum->trimValue(pp->colorGrading.shadowsLum);
    midtonesLum->trimValue(pp->colorGrading.midtonesLum);
    highlightsLum->trimValue(pp->colorGrading.highlightsLum);
    globalLum->trimValue(pp->colorGrading.globalLum);
    blending->trimValue(pp->colorGrading.blending);
    balance->trimValue(pp->colorGrading.balance);
}

void ColorGrading::adjusterChanged(Adjuster* a, double newval)
{
    autoEnable();
    if (listener && getEnabled()) {
        if (a == blending) {
            listener->panelChanged(EvColorGradingBlending, Glib::ustring::format(std::fixed, std::setprecision(0), newval));
        } else if (a == balance) {
            listener->panelChanged(EvColorGradingBalance, Glib::ustring::format(std::fixed, std::setprecision(0), newval));
        } else if (a == shadowsLum) {
            listener->panelChanged(EvColorGradingShadows, Glib::ustring::format(std::fixed, std::setprecision(0), newval));
        } else if (a == midtonesLum) {
            listener->panelChanged(EvColorGradingMidtones, Glib::ustring::format(std::fixed, std::setprecision(0), newval));
        } else if (a == highlightsLum) {
            listener->panelChanged(EvColorGradingHighlights, Glib::ustring::format(std::fixed, std::setprecision(0), newval));
        } else if (a == globalLum) {
            listener->panelChanged(EvColorGradingGlobal, Glib::ustring::format(std::fixed, std::setprecision(0), newval));
        }
    }
}

void ColorGrading::colorWheelChanged(ColorWheel* source, double hue, double saturation)
{
    autoEnable();
    if (listener && getEnabled()) {
        if (source == shadowsWheel) {
            listener->panelChanged(EvColorGradingShadows, M("GENERAL_CHANGED"));
        } else if (source == midtonesWheel) {
            listener->panelChanged(EvColorGradingMidtones, M("GENERAL_CHANGED"));
        } else if (source == highlightsWheel) {
            listener->panelChanged(EvColorGradingHighlights, M("GENERAL_CHANGED"));
        } else if (source == globalWheel) {
            listener->panelChanged(EvColorGradingGlobal, M("GENERAL_CHANGED"));
        }
    }
}

void ColorGrading::enabledChanged()
{
    if (listener) {
        if (get_inconsistent()) {
            listener->panelChanged(EvColorGradingEnabled, M("GENERAL_UNCHANGED"));
        } else if (getEnabled()) {
            listener->panelChanged(EvColorGradingEnabled, M("GENERAL_ENABLED"));
        } else {
            listener->panelChanged(EvColorGradingEnabled, M("GENERAL_DISABLED"));
        }
    }
}
