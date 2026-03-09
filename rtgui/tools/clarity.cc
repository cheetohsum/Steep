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
#include "clarity.h"

#include "eventmapper.h"

#include "rtengine/procparams.h"

using namespace rtengine;
using namespace rtengine::procparams;

const Glib::ustring Clarity::TOOL_NAME = "clarity";

Clarity::Clarity(): FoldableToolPanel(this, TOOL_NAME, M("TP_CLARITY_LABEL"), false, true),
    detailExpanded_(false)
{
    auto m = ProcEventMapper::getInstance();
    EvClarityEnabled = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_CLARITY_ENABLED");
    EvClarityAmount = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_CLARITY_AMOUNT");
    EvClarityRadius = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_CLARITY_RADIUS");

    amount = Gtk::manage(new Adjuster(M("TP_CLARITY_AMOUNT"), -100., 100., 1., 0.));
    radius = Gtk::manage(new Adjuster(M("TP_CLARITY_RADIUS"), 50., 200., 1., 100.));

    amount->setAdjusterListener(this);
    radius->setAdjusterListener(this);

    // Gradient: soft haze → crisp clarity
    amount->setSliderGradient({
        GradientMilestone(0.0, 0.35, 0.35, 0.40),
        GradientMilestone(0.5, 0.50, 0.50, 0.50),
        GradientMilestone(1.0, 0.75, 0.80, 0.88)
    });

    // Label-as-toggle: click the slider label to expand/collapse detail
    amount->setLabel(Glib::ustring("\u25B8 ") + M("TP_CLARITY_AMOUNT"));
    amount->setLabelClickCallback([this]() { toggleDetail(); });

    auto *summaryBox = getSummaryBox();
    summaryBox->pack_start(*amount);

    detailContent_ = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
    detailContent_->pack_start(*radius);
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

void Clarity::toggleDetail()
{
    detailExpanded_ = !detailExpanded_;
    amount->setLabel(Glib::ustring(detailExpanded_ ? "\u25BE " : "\u25B8 ") + M("TP_CLARITY_AMOUNT"));
    detailRevealer_->set_reveal_child(detailExpanded_);
}

void Clarity::read(const ProcParams *pp, const ParamsEdited *pedited)
{
    disableListener();

    if (pedited) {
        amount->setEditedState(pedited->clarity.amount ? Edited : UnEdited);
        radius->setEditedState(pedited->clarity.radius ? Edited : UnEdited);
        set_inconsistent(multiImage && !pedited->clarity.enabled);
    }

    setEnabled(pp->clarity.enabled);
    amount->setValue(pp->clarity.amount);
    radius->setValue(pp->clarity.radius);

    enableListener();
}

void Clarity::write(ProcParams *pp, ParamsEdited *pedited)
{
    pp->clarity.amount = amount->getValue();
    pp->clarity.radius = radius->getValue();
    pp->clarity.enabled = getEnabled();

    if (pedited) {
        pedited->clarity.amount = amount->getEditedState();
        pedited->clarity.radius = radius->getEditedState();
        pedited->clarity.enabled = !get_inconsistent();
    }
}

void Clarity::setDefaults(const ProcParams *defParams, const ParamsEdited *pedited)
{
    amount->setDefault(defParams->clarity.amount);
    radius->setDefault(defParams->clarity.radius);

    if (pedited) {
        amount->setDefaultEditedState(pedited->clarity.amount ? Edited : UnEdited);
        radius->setDefaultEditedState(pedited->clarity.radius ? Edited : UnEdited);
    } else {
        amount->setDefaultEditedState(Irrelevant);
        radius->setDefaultEditedState(Irrelevant);
    }
}

void Clarity::adjusterChanged(Adjuster* a, double newval)
{
    autoEnable();
    if (listener && getEnabled()) {
        if (a == amount) {
            listener->panelChanged(EvClarityAmount, a->getTextValue());
        } else if (a == radius) {
            listener->panelChanged(EvClarityRadius, a->getTextValue());
        }
    }
}

void Clarity::enabledChanged()
{
    if (listener) {
        if (get_inconsistent()) {
            listener->panelChanged(EvClarityEnabled, M("GENERAL_UNCHANGED"));
        } else if (getEnabled()) {
            listener->panelChanged(EvClarityEnabled, M("GENERAL_ENABLED"));
        } else {
            listener->panelChanged(EvClarityEnabled, M("GENERAL_DISABLED"));
        }
    }
}

void Clarity::setBatchMode(bool batchMode)
{
    ToolPanel::setBatchMode(batchMode);

    amount->showEditedCB();
    radius->showEditedCB();
}
