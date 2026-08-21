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
#include "grain.h"

#include "eventmapper.h"

#include "rtengine/procparams.h"

using namespace rtengine;
using namespace rtengine::procparams;

const Glib::ustring Grain::TOOL_NAME = "grain";

Grain::Grain(): FoldableToolPanel(this, TOOL_NAME, M("TP_GRAIN_LABEL"), false, true),
    detailExpanded_(false)
{
    auto m = ProcEventMapper::getInstance();
    EvGrainEnabled = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_GRAIN_ENABLED");
    EvGrainStrength = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_GRAIN_STRENGTH");
    EvGrainISO = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_GRAIN_ISO");
    EvGrainScale = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_GRAIN_SCALE");
    EvGrainColor = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_GRAIN_COLOR");

    strength = Gtk::manage(new Adjuster(M("TP_GRAIN_STRENGTH"), 0., 100., 1., 0.));
    iso = Gtk::manage(new Adjuster(M("TP_GRAIN_ISO"), 20., 6400., 10., 400.));
    scale = Gtk::manage(new Adjuster(M("TP_GRAIN_SCALE"), 0., 100., 1., 100.));
    color = Gtk::manage(new Adjuster(M("TP_GRAIN_COLOR"), 0., 100., 1., 0.));

    strength->setAdjusterListener(this);
    iso->setAdjusterListener(this);
    scale->setAdjusterListener(this);
    color->setAdjusterListener(this);

    // Gradient: warm film grain feel
    strength->setSliderGradient({
        GradientMilestone(0.0, 0.35, 0.33, 0.30),
        GradientMilestone(1.0, 0.75, 0.65, 0.50)
    });

    // Label-as-toggle: click the slider label to expand/collapse detail
    strength->setLabel(Glib::ustring("\u25B8 ") + M("TP_GRAIN_STRENGTH"));
    strength->setLabelClickCallback([this]() { toggleDetail(); });

    auto *summaryBox = getSummaryBox();
    summaryBox->pack_start(*strength);

    detailContent_ = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
    detailContent_->pack_start(*iso);
    detailContent_->pack_start(*scale);
    detailContent_->pack_start(*color);
    detailContent_->show_all();

    detailRevealer_ = Gtk::manage(new Gtk::Revealer());
    detailRevealer_->set_transition_type(Gtk::REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    detailRevealer_->set_transition_duration(200);
    detailRevealer_->set_reveal_child(false);
    detailRevealer_->add(*detailContent_);
    detailRevealer_->show();
    summaryBox->pack_start(*detailRevealer_, Gtk::PACK_SHRINK);

    summaryBox->show_all();
}

void Grain::toggleDetail()
{
    detailExpanded_ = !detailExpanded_;
    strength->setLabel(Glib::ustring(detailExpanded_ ? "\u25BE " : "\u25B8 ") + M("TP_GRAIN_STRENGTH"));
    detailRevealer_->set_reveal_child(detailExpanded_);
}

void Grain::read(const ProcParams *pp, const ParamsEdited *pedited)
{
    disableListener();

    if (pedited) {
        strength->setEditedState(pedited->grain.strength ? Edited : UnEdited);
        iso->setEditedState(pedited->grain.iso ? Edited : UnEdited);
        scale->setEditedState(pedited->grain.scale ? Edited : UnEdited);
        color->setEditedState(pedited->grain.color ? Edited : UnEdited);
        set_inconsistent(multiImage && !pedited->grain.enabled);
    }

    setEnabled(pp->grain.enabled);
    strength->setValue(pp->grain.strength);
    iso->setValue(pp->grain.iso);
    scale->setValue(pp->grain.scale);
    color->setValue(pp->grain.color);

    enableListener();
}

void Grain::write(ProcParams *pp, ParamsEdited *pedited)
{
    pp->grain.strength = strength->getValue();
    pp->grain.iso = iso->getValue();
    pp->grain.scale = scale->getValue();
    pp->grain.color = color->getValue();
    pp->grain.enabled = getEnabled();

    if (pedited) {
        pedited->grain.strength = strength->getEditedState();
        pedited->grain.iso = iso->getEditedState();
        pedited->grain.scale = scale->getEditedState();
        pedited->grain.color = color->getEditedState();
        pedited->grain.enabled = !get_inconsistent();
    }
}

void Grain::setDefaults(const ProcParams *defParams, const ParamsEdited *pedited)
{
    strength->setDefault(defParams->grain.strength);
    iso->setDefault(defParams->grain.iso);
    scale->setDefault(defParams->grain.scale);
    color->setDefault(defParams->grain.color);

    if (pedited) {
        strength->setDefaultEditedState(pedited->grain.strength ? Edited : UnEdited);
        iso->setDefaultEditedState(pedited->grain.iso ? Edited : UnEdited);
        scale->setDefaultEditedState(pedited->grain.scale ? Edited : UnEdited);
        color->setDefaultEditedState(pedited->grain.color ? Edited : UnEdited);
    } else {
        strength->setDefaultEditedState(Irrelevant);
        iso->setDefaultEditedState(Irrelevant);
        scale->setDefaultEditedState(Irrelevant);
        color->setDefaultEditedState(Irrelevant);
    }
}

void Grain::adjusterChanged(Adjuster* a, double newval)
{
    autoEnable();
    if (listener && getEnabled()) {
        if (a == strength) {
            listener->panelChanged(EvGrainStrength, a->getTextValue());
        } else if (a == iso) {
            listener->panelChanged(EvGrainISO, a->getTextValue());
        } else if (a == scale) {
            listener->panelChanged(EvGrainScale, a->getTextValue());
        } else if (a == color) {
            listener->panelChanged(EvGrainColor, a->getTextValue());
        }
    }
}

void Grain::enabledChanged()
{
    if (listener) {
        if (get_inconsistent()) {
            listener->panelChanged(EvGrainEnabled, M("GENERAL_UNCHANGED"));
        } else if (getEnabled()) {
            listener->panelChanged(EvGrainEnabled, M("GENERAL_ENABLED"));
        } else {
            listener->panelChanged(EvGrainEnabled, M("GENERAL_DISABLED"));
        }
    }
}

void Grain::setBatchMode(bool batchMode)
{
    ToolPanel::setBatchMode(batchMode);

    strength->showEditedCB();
    iso->showEditedCB();
    scale->showEditedCB();
    color->showEditedCB();
}
