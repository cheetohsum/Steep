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
#include "texture.h"

#include "eventmapper.h"

#include "rtengine/procparams.h"

using namespace rtengine;
using namespace rtengine::procparams;

const Glib::ustring Texture::TOOL_NAME = "texture";

Texture::Texture(): FoldableToolPanel(this, TOOL_NAME, M("TP_TEXTURE_LABEL"), false, true),
    detailExpanded_(false)
{
    auto m = ProcEventMapper::getInstance();
    EvTextureEnabled = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_TEXTURE_ENABLED");
    EvTextureAmount = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_TEXTURE_AMOUNT");
    EvTextureRadius = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_TEXTURE_RADIUS");

    amount = Gtk::manage(new Adjuster(M("TP_TEXTURE_AMOUNT"), -100., 100., 1., 0.));
    radius = Gtk::manage(new Adjuster(M("TP_TEXTURE_RADIUS"), 5., 50., 1., 20.));

    amount->setAdjusterListener(this);
    radius->setAdjusterListener(this);

    // Gradient: subtle blue-gray structure feel
    amount->setSliderGradient({
        GradientMilestone(0.0, 0.25, 0.28, 0.35),
        GradientMilestone(0.5, 0.45, 0.45, 0.45),
        GradientMilestone(1.0, 0.65, 0.72, 0.80)
    });

    // Label-as-toggle: click the slider label to expand/collapse detail
    amount->setLabel(Glib::ustring("\u25B8 ") + M("TP_TEXTURE_AMOUNT"));
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

void Texture::toggleDetail()
{
    detailExpanded_ = !detailExpanded_;
    amount->setLabel(Glib::ustring(detailExpanded_ ? "\u25BE " : "\u25B8 ") + M("TP_TEXTURE_AMOUNT"));
    detailRevealer_->set_reveal_child(detailExpanded_);
}

void Texture::read(const ProcParams *pp, const ParamsEdited *pedited)
{
    disableListener();

    if (pedited) {
        amount->setEditedState(pedited->texture.amount ? Edited : UnEdited);
        radius->setEditedState(pedited->texture.radius ? Edited : UnEdited);
        set_inconsistent(multiImage && !pedited->texture.enabled);
    }

    setEnabled(pp->texture.enabled);
    amount->setValue(pp->texture.amount);
    radius->setValue(pp->texture.radius);

    enableListener();
}

void Texture::write(ProcParams *pp, ParamsEdited *pedited)
{
    pp->texture.amount = amount->getValue();
    pp->texture.radius = radius->getValue();
    pp->texture.enabled = getEnabled();

    if (pedited) {
        pedited->texture.amount = amount->getEditedState();
        pedited->texture.radius = radius->getEditedState();
        pedited->texture.enabled = !get_inconsistent();
    }
}

void Texture::setDefaults(const ProcParams *defParams, const ParamsEdited *pedited)
{
    amount->setDefault(defParams->texture.amount);
    radius->setDefault(defParams->texture.radius);

    if (pedited) {
        amount->setDefaultEditedState(pedited->texture.amount ? Edited : UnEdited);
        radius->setDefaultEditedState(pedited->texture.radius ? Edited : UnEdited);
    } else {
        amount->setDefaultEditedState(Irrelevant);
        radius->setDefaultEditedState(Irrelevant);
    }
}

void Texture::adjusterChanged(Adjuster* a, double newval)
{
    autoEnable();
    if (listener && getEnabled()) {
        if (a == amount) {
            listener->panelChanged(EvTextureAmount, a->getTextValue());
        } else if (a == radius) {
            listener->panelChanged(EvTextureRadius, a->getTextValue());
        }
    }
}

void Texture::enabledChanged()
{
    if (listener) {
        if (get_inconsistent()) {
            listener->panelChanged(EvTextureEnabled, M("GENERAL_UNCHANGED"));
        } else if (getEnabled()) {
            listener->panelChanged(EvTextureEnabled, M("GENERAL_ENABLED"));
        } else {
            listener->panelChanged(EvTextureEnabled, M("GENERAL_DISABLED"));
        }
    }
}

void Texture::setBatchMode(bool batchMode)
{
    ToolPanel::setBatchMode(batchMode);

    amount->showEditedCB();
    radius->showEditedCB();
}
