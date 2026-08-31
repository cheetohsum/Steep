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
#include "lighteffects.h"

#include "eventmapper.h"

#include "rtengine/procparams.h"

using namespace rtengine;
using namespace rtengine::procparams;

const Glib::ustring LightEffects::TOOL_NAME = "lighteffects";

LightEffects::LightEffects():
    FoldableToolPanel(this, TOOL_NAME, M("TP_LIGHTEFFECTS_LABEL"), false, true)
{
    auto m = ProcEventMapper::getInstance();
    EvEnabled        = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_LIGHTEFFECTS_ENABLED");
    EvThreshold      = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_LIGHTEFFECTS_THRESHOLD");
    EvGlow           = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_LIGHTEFFECTS_GLOW");
    EvGlowRadius     = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_LIGHTEFFECTS_GLOWRADIUS");
    EvHalation       = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_LIGHTEFFECTS_HALATION");
    EvHalationSize   = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_LIGHTEFFECTS_HALATIONSIZE");
    EvHalationWarmth = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_LIGHTEFFECTS_HALATIONWARMTH");
    EvFlare          = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_LIGHTEFFECTS_FLARE");
    EvFlareLength    = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_LIGHTEFFECTS_FLARELENGTH");
    EvFlareAngle     = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_LIGHTEFFECTS_FLAREANGLE");

    glow           = Gtk::manage(new Adjuster(M("TP_LIGHTEFFECTS_AMOUNT"), 0., 100., 1., 0.));
    glowRadius     = Gtk::manage(new Adjuster(M("TP_LIGHTEFFECTS_GLOWRADIUS"), 0., 100., 1., 25.));
    halation       = Gtk::manage(new Adjuster(M("TP_LIGHTEFFECTS_AMOUNT"), 0., 100., 1., 0.));
    halationSize   = Gtk::manage(new Adjuster(M("TP_LIGHTEFFECTS_HALATIONSIZE"), 0., 100., 1., 30.));
    halationWarmth = Gtk::manage(new Adjuster(M("TP_LIGHTEFFECTS_HALATIONWARMTH"), 0., 100., 1., 70.));
    flare          = Gtk::manage(new Adjuster(M("TP_LIGHTEFFECTS_AMOUNT"), 0., 100., 1., 0.));
    flareLength    = Gtk::manage(new Adjuster(M("TP_LIGHTEFFECTS_FLARELENGTH"), 0., 100., 1., 40.));
    flareAngle     = Gtk::manage(new Adjuster(M("TP_LIGHTEFFECTS_FLAREANGLE"), -180., 180., 1., 0.));
    threshold      = Gtk::manage(new Adjuster(M("TP_LIGHTEFFECTS_THRESHOLD"), 0., 100., 1., 70.));

    for (Adjuster* a : {threshold, glow, glowRadius, halation, halationSize,
                        halationWarmth, flare, flareLength, flareAngle}) {
        a->setAdjusterListener(this);
    }

    // Halation warmth runs neutral -> the red-orange of light bouncing off
    // the film base, so show that on the slider itself.
    halationWarmth->setSliderGradient({
        GradientMilestone(0.0, 0.78, 0.78, 0.78),
        GradientMilestone(1.0, 1.00, 0.42, 0.20)
    });

    glow->set_tooltip_text(M("TP_LIGHTEFFECTS_GLOW_TOOLTIP"));
    halation->set_tooltip_text(M("TP_LIGHTEFFECTS_HALATION_TOOLTIP"));
    flare->set_tooltip_text(M("TP_LIGHTEFFECTS_FLARE_TOOLTIP"));
    threshold->set_tooltip_text(M("TP_LIGHTEFFECTS_THRESHOLD_TOOLTIP"));

    // Named sections rather than divider rules: three effects share this
    // panel and the sliders alone do not say which belongs to which.
    const auto section = [this](const char* labelKey) {
        Gtk::Label* heading = Gtk::manage(new Gtk::Label(M(labelKey)));
        heading->set_halign(Gtk::ALIGN_START);
        heading->get_style_context()->add_class("tool-section-label");
        pack_start(*heading, Gtk::PACK_SHRINK);
    };

    section("TP_LIGHTEFFECTS_GLOW");
    pack_start(*glow);
    pack_start(*glowRadius);

    section("TP_LIGHTEFFECTS_HALATION");
    pack_start(*halation);
    pack_start(*halationSize);
    pack_start(*halationWarmth);

    section("TP_LIGHTEFFECTS_FLARE");
    pack_start(*flare);
    pack_start(*flareLength);
    pack_start(*flareAngle);

    section("TP_LIGHTEFFECTS_SHARED");
    pack_start(*threshold);

    show_all();
}

void LightEffects::read(const ProcParams* pp, const ParamsEdited* pedited)
{
    disableListener();

    if (pedited) {
        threshold->setEditedState(pedited->lightEffects.threshold ? Edited : UnEdited);
        glow->setEditedState(pedited->lightEffects.glow ? Edited : UnEdited);
        glowRadius->setEditedState(pedited->lightEffects.glowRadius ? Edited : UnEdited);
        halation->setEditedState(pedited->lightEffects.halation ? Edited : UnEdited);
        halationSize->setEditedState(pedited->lightEffects.halationSize ? Edited : UnEdited);
        halationWarmth->setEditedState(pedited->lightEffects.halationWarmth ? Edited : UnEdited);
        flare->setEditedState(pedited->lightEffects.flare ? Edited : UnEdited);
        flareLength->setEditedState(pedited->lightEffects.flareLength ? Edited : UnEdited);
        flareAngle->setEditedState(pedited->lightEffects.flareAngle ? Edited : UnEdited);
        set_inconsistent(multiImage && !pedited->lightEffects.enabled);
    }

    setEnabled(pp->lightEffects.enabled);
    threshold->setValue(pp->lightEffects.threshold);
    glow->setValue(pp->lightEffects.glow);
    glowRadius->setValue(pp->lightEffects.glowRadius);
    halation->setValue(pp->lightEffects.halation);
    halationSize->setValue(pp->lightEffects.halationSize);
    halationWarmth->setValue(pp->lightEffects.halationWarmth);
    flare->setValue(pp->lightEffects.flare);
    flareLength->setValue(pp->lightEffects.flareLength);
    flareAngle->setValue(pp->lightEffects.flareAngle);

    enableListener();
}

void LightEffects::write(ProcParams* pp, ParamsEdited* pedited)
{
    pp->lightEffects.threshold = threshold->getValue();
    pp->lightEffects.glow = glow->getValue();
    pp->lightEffects.glowRadius = glowRadius->getValue();
    pp->lightEffects.halation = halation->getValue();
    pp->lightEffects.halationSize = halationSize->getValue();
    pp->lightEffects.halationWarmth = halationWarmth->getValue();
    pp->lightEffects.flare = flare->getValue();
    pp->lightEffects.flareLength = flareLength->getValue();
    pp->lightEffects.flareAngle = flareAngle->getValue();
    pp->lightEffects.enabled = getEnabled();

    if (pedited) {
        pedited->lightEffects.threshold = threshold->getEditedState();
        pedited->lightEffects.glow = glow->getEditedState();
        pedited->lightEffects.glowRadius = glowRadius->getEditedState();
        pedited->lightEffects.halation = halation->getEditedState();
        pedited->lightEffects.halationSize = halationSize->getEditedState();
        pedited->lightEffects.halationWarmth = halationWarmth->getEditedState();
        pedited->lightEffects.flare = flare->getEditedState();
        pedited->lightEffects.flareLength = flareLength->getEditedState();
        pedited->lightEffects.flareAngle = flareAngle->getEditedState();
        pedited->lightEffects.enabled = !get_inconsistent();
    }
}

void LightEffects::setDefaults(const ProcParams* defParams, const ParamsEdited* pedited)
{
    threshold->setDefault(defParams->lightEffects.threshold);
    glow->setDefault(defParams->lightEffects.glow);
    glowRadius->setDefault(defParams->lightEffects.glowRadius);
    halation->setDefault(defParams->lightEffects.halation);
    halationSize->setDefault(defParams->lightEffects.halationSize);
    halationWarmth->setDefault(defParams->lightEffects.halationWarmth);
    flare->setDefault(defParams->lightEffects.flare);
    flareLength->setDefault(defParams->lightEffects.flareLength);
    flareAngle->setDefault(defParams->lightEffects.flareAngle);

    if (pedited) {
        threshold->setDefaultEditedState(pedited->lightEffects.threshold ? Edited : UnEdited);
        glow->setDefaultEditedState(pedited->lightEffects.glow ? Edited : UnEdited);
        glowRadius->setDefaultEditedState(pedited->lightEffects.glowRadius ? Edited : UnEdited);
        halation->setDefaultEditedState(pedited->lightEffects.halation ? Edited : UnEdited);
        halationSize->setDefaultEditedState(pedited->lightEffects.halationSize ? Edited : UnEdited);
        halationWarmth->setDefaultEditedState(pedited->lightEffects.halationWarmth ? Edited : UnEdited);
        flare->setDefaultEditedState(pedited->lightEffects.flare ? Edited : UnEdited);
        flareLength->setDefaultEditedState(pedited->lightEffects.flareLength ? Edited : UnEdited);
        flareAngle->setDefaultEditedState(pedited->lightEffects.flareAngle ? Edited : UnEdited);
    } else {
        for (Adjuster* a : {threshold, glow, glowRadius, halation, halationSize,
                            halationWarmth, flare, flareLength, flareAngle}) {
            a->setDefaultEditedState(Irrelevant);
        }
    }
}

void LightEffects::adjusterChanged(Adjuster* a, double newval)
{
    autoEnable();

    if (!listener || !getEnabled()) {
        return;
    }

    if (a == threshold) {
        listener->panelChanged(EvThreshold, a->getTextValue());
    } else if (a == glow) {
        listener->panelChanged(EvGlow, a->getTextValue());
    } else if (a == glowRadius) {
        listener->panelChanged(EvGlowRadius, a->getTextValue());
    } else if (a == halation) {
        listener->panelChanged(EvHalation, a->getTextValue());
    } else if (a == halationSize) {
        listener->panelChanged(EvHalationSize, a->getTextValue());
    } else if (a == halationWarmth) {
        listener->panelChanged(EvHalationWarmth, a->getTextValue());
    } else if (a == flare) {
        listener->panelChanged(EvFlare, a->getTextValue());
    } else if (a == flareLength) {
        listener->panelChanged(EvFlareLength, a->getTextValue());
    } else if (a == flareAngle) {
        listener->panelChanged(EvFlareAngle, a->getTextValue());
    }
}

void LightEffects::enabledChanged()
{
    if (!listener) {
        return;
    }

    if (get_inconsistent()) {
        listener->panelChanged(EvEnabled, M("GENERAL_UNCHANGED"));
    } else if (getEnabled()) {
        listener->panelChanged(EvEnabled, M("GENERAL_ENABLED"));
    } else {
        listener->panelChanged(EvEnabled, M("GENERAL_DISABLED"));
    }
}

void LightEffects::setBatchMode(bool batchMode)
{
    ToolPanel::setBatchMode(batchMode);

    for (Adjuster* a : {threshold, glow, glowRadius, halation, halationSize,
                        halationWarmth, flare, flareLength, flareAngle}) {
        a->showEditedCB();
    }
}
