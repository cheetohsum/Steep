/*
 *  This file is part of RawTherapee.
 */
#include "pcvignette.h"

#include "rtengine/procparams.h"

using namespace rtengine;
using namespace rtengine::procparams;

const Glib::ustring PCVignette::TOOL_NAME = "pcvignette";

PCVignette::PCVignette () : FoldableToolPanel(this, TOOL_NAME, M("TP_PCVIGNETTE_LABEL"), false, true),
    detailExpanded_(false)
{
    strength = Gtk::manage (new Adjuster (M("TP_PCVIGNETTE_STRENGTH"), -6, 6, 0.01, 0));
    strength->set_tooltip_text (M("TP_PCVIGNETTE_STRENGTH_TOOLTIP"));
    strength->setAdjusterListener (this);

    feather = Gtk::manage (new Adjuster (M("TP_PCVIGNETTE_FEATHER"), 0, 100, 1, 50));
    feather->set_tooltip_text (M("TP_PCVIGNETTE_FEATHER_TOOLTIP"));
    feather->setAdjusterListener (this);

    roundness = Gtk::manage (new Adjuster (M("TP_PCVIGNETTE_ROUNDNESS"), 0, 100, 1, 50));
    roundness->set_tooltip_text (M("TP_PCVIGNETTE_ROUNDNESS_TOOLTIP"));
    roundness->setAdjusterListener (this);

    // Gradient: light center → dark vignette edges
    strength->setSliderGradient({
        GradientMilestone(0.0, 0.15, 0.15, 0.18),
        GradientMilestone(0.5, 0.45, 0.45, 0.45),
        GradientMilestone(1.0, 0.75, 0.75, 0.70)
    });

    // Label-as-toggle: click the slider label to expand/collapse detail
    strength->setLabel(Glib::ustring("\u25B8 ") + M("TP_PCVIGNETTE_STRENGTH"));
    strength->setLabelClickCallback([this]() { toggleDetail(); });

    auto *summaryBox = getSummaryBox();
    summaryBox->pack_start (*strength);

    detailContent_ = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
    detailContent_->set_no_show_all(true);
    detailContent_->pack_start(*feather);
    detailContent_->pack_start(*roundness);
    summaryBox->pack_start(*detailContent_, Gtk::PACK_SHRINK);

    summaryBox->show_all();
}

void PCVignette::toggleDetail()
{
    detailExpanded_ = !detailExpanded_;
    if (detailExpanded_) {
        strength->setLabel(Glib::ustring("\u25BE ") + M("TP_PCVIGNETTE_STRENGTH"));
        detailContent_->set_no_show_all(false);
        detailContent_->show_all();
        detailContent_->set_no_show_all(true);
    } else {
        strength->setLabel(Glib::ustring("\u25B8 ") + M("TP_PCVIGNETTE_STRENGTH"));
        detailContent_->hide();
    }
}

void PCVignette::read (const ProcParams* pp, const ParamsEdited* pedited)
{
    disableListener ();

    if (pedited) {
        strength->setEditedState (pedited->pcvignette.strength ? Edited : UnEdited);
        feather->setEditedState (pedited->pcvignette.feather ? Edited : UnEdited);
        roundness->setEditedState (pedited->pcvignette.roundness ? Edited : UnEdited);
        set_inconsistent (multiImage && !pedited->pcvignette.enabled);
    }

    setEnabled(pp->pcvignette.enabled);
    strength->setValue (pp->pcvignette.strength);
    feather->setValue (pp->pcvignette.feather);
    roundness->setValue (pp->pcvignette.roundness);

    enableListener ();
}

void PCVignette::write (ProcParams* pp, ParamsEdited* pedited)
{
    pp->pcvignette.strength = strength->getValue ();
    pp->pcvignette.feather = feather->getIntValue ();
    pp->pcvignette.roundness = roundness->getIntValue ();
    pp->pcvignette.enabled = getEnabled();

    if (pedited) {
        pedited->pcvignette.strength = strength->getEditedState ();
        pedited->pcvignette.feather = feather->getEditedState ();
        pedited->pcvignette.roundness = roundness->getEditedState ();
        pedited->pcvignette.enabled = !get_inconsistent();
    }
}

void PCVignette::setDefaults (const ProcParams* defParams, const ParamsEdited* pedited)
{
    strength->setDefault (defParams->pcvignette.strength);
    feather->setDefault (defParams->pcvignette.feather);
    roundness->setDefault (defParams->pcvignette.roundness);

    if (pedited) {
        strength->setDefaultEditedState (pedited->pcvignette.strength ? Edited : UnEdited);
        feather->setDefaultEditedState (pedited->pcvignette.feather ? Edited : UnEdited);
        roundness->setDefaultEditedState (pedited->pcvignette.roundness ? Edited : UnEdited);
    } else {
        strength->setDefaultEditedState (Irrelevant);
        feather->setDefaultEditedState (Irrelevant);
        roundness->setDefaultEditedState (Irrelevant);
    }
}

void PCVignette::adjusterChanged(Adjuster* a, double newval)
{
    autoEnable();
    if (listener && getEnabled()) {
        if (a == strength) {
            listener->panelChanged (EvPCVignetteStrength, strength->getTextValue());
        } else if (a == feather) {
            listener->panelChanged (EvPCVignetteFeather, feather->getTextValue());
        } else if (a == roundness) {
            listener->panelChanged (EvPCVignetteRoundness, roundness->getTextValue());
        }
    }
}

void PCVignette::enabledChanged ()
{

    if (listener) {
        if (get_inconsistent()) {
            listener->panelChanged (EvPCVignetteEnabled, M("GENERAL_UNCHANGED"));
        } else if (getEnabled()) {
            listener->panelChanged (EvPCVignetteEnabled, M("GENERAL_ENABLED"));
        } else {
            listener->panelChanged (EvPCVignetteEnabled, M("GENERAL_DISABLED"));
        }
    }
}

void PCVignette::setAdjusterBehavior (bool strengthadd, bool featheradd, bool roundnessadd)
{
    strength->setAddMode(strengthadd);
    feather->setAddMode(featheradd);
    roundness->setAddMode(roundnessadd);
}

void PCVignette::trimValues (rtengine::procparams::ProcParams* pp)
{
    strength->trimValue(pp->pcvignette.strength);
    feather->trimValue(pp->pcvignette.feather);
    roundness->trimValue(pp->pcvignette.roundness);
}

void PCVignette::setBatchMode (bool batchMode)
{
    ToolPanel::setBatchMode (batchMode);
    strength->showEditedCB ();
    feather->showEditedCB ();
    roundness->showEditedCB ();
}
