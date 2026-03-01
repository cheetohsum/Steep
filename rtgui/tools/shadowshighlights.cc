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
#include "shadowshighlights.h"

#include "eventmapper.h"

#include "rtengine/procparams.h"

using namespace rtengine;
using namespace rtengine::procparams;

const Glib::ustring ShadowsHighlights::TOOL_NAME = "shadowshighlights";

ShadowsHighlights::ShadowsHighlights () : FoldableToolPanel(this, TOOL_NAME, M("TP_SHADOWSHLIGHTS_LABEL"), false, true)
{
    auto m = ProcEventMapper::getInstance();
    EvSHColorspace = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_SH_COLORSPACE");

    highlights   = Gtk::manage (new Adjuster (M("TP_SHADOWSHLIGHTS_HIGHLIGHTS"), 0, 100, 1, 0));
    // Highlights gradient: mid → light
    highlights->setSliderGradient({
        GradientMilestone(0.0, 0.5, 0.5, 0.5),
        GradientMilestone(1.0, 0.95, 0.95, 0.95)
    });
    h_tonalwidth = Gtk::manage (new Adjuster (M("TP_SHADOWSHLIGHTS_HLTONALW"), 10, 100, 1, 70));
    shadows      = Gtk::manage (new Adjuster (M("TP_SHADOWSHLIGHTS_SHADOWS"), 0, 100, 1, 0));
    // Shadows gradient: dark → mid
    shadows->setSliderGradient({
        GradientMilestone(0.0, 0.1, 0.1, 0.1),
        GradientMilestone(1.0, 0.5, 0.5, 0.5)
    });
    s_tonalwidth = Gtk::manage (new Adjuster (M("TP_SHADOWSHLIGHTS_SHTONALW"), 10, 100, 1, 30));
    radius = Gtk::manage (new Adjuster (M("TP_SHADOWSHLIGHTS_RADIUS"), 1, 100, 1, 40));

    Gtk::Grid* hb = Gtk::manage (new Gtk::Grid ());
    hb->get_style_context()->add_class("grid-spacing");
    setExpandAlignProperties(hb, true, false, Gtk::ALIGN_FILL, Gtk::ALIGN_CENTER);
    Gtk::Label* csLabel = Gtk::manage(new Gtk::Label(M("TP_DIRPYRDENOISE_MAIN_COLORSPACE") + ": "));
    setExpandAlignProperties(csLabel, false, false, Gtk::ALIGN_START, Gtk::ALIGN_CENTER);
    colorspace = Gtk::manage(new MyComboBoxText());
    colorspace->append(M("TP_DIRPYRDENOISE_MAIN_COLORSPACE_RGB"));
    colorspace->append(M("TP_DIRPYRDENOISE_MAIN_COLORSPACE_LAB"));
    setExpandAlignProperties(colorspace, true, false, Gtk::ALIGN_FILL, Gtk::ALIGN_CENTER);
    hb->attach(*csLabel, 0, 0, 1, 1);
    hb->attach(*colorspace, 1, 0, 1, 1);

    // Visible items (in summary box)
    getSummaryBox()->pack_start (*shadows);
    getSummaryBox()->show_all();

    // Advanced items
    advancedSection = Gtk::manage(new AdvancedSection());
    pack_start(*advancedSection, Gtk::PACK_SHRINK, 0);
    Gtk::Box* const advBox = advancedSection->getContentBox();

    advBox->pack_start(*highlights);
    advBox->pack_start(*hb);
    advBox->pack_start(*h_tonalwidth);
    advBox->pack_start(*s_tonalwidth);
    advBox->pack_start(*radius);

    radius->setAdjusterListener (this);
    highlights->setAdjusterListener (this);
    h_tonalwidth->setAdjusterListener (this);
    shadows->setAdjusterListener (this);
    s_tonalwidth->setAdjusterListener (this);

    colorspace->signal_changed().connect(sigc::mem_fun(*this, &ShadowsHighlights::colorspaceChanged));
    
    show_all_children ();
}

void ShadowsHighlights::read (const ProcParams* pp, const ParamsEdited* pedited)
{

    disableListener ();

    if (pedited) {
        radius->setEditedState       (pedited->sh.radius ? Edited : UnEdited);
        highlights->setEditedState   (pedited->sh.highlights ? Edited : UnEdited);
        h_tonalwidth->setEditedState (pedited->sh.htonalwidth ? Edited : UnEdited);
        shadows->setEditedState      (pedited->sh.shadows ? Edited : UnEdited);
        s_tonalwidth->setEditedState (pedited->sh.stonalwidth ? Edited : UnEdited);
        set_inconsistent             (multiImage && !pedited->sh.enabled);

    }

    setEnabled (pp->sh.enabled);

    radius->setValue        (pp->sh.radius);
    highlights->setValue    (pp->sh.highlights);
    h_tonalwidth->setValue  (pp->sh.htonalwidth);
    shadows->setValue       (pp->sh.shadows);
    s_tonalwidth->setValue  (pp->sh.stonalwidth);

    if (pedited && !pedited->sh.lab) {
        colorspace->set_active(2);
    } else if (pp->sh.lab) {
        colorspace->set_active(1);
    } else {
        colorspace->set_active(0);
    }
    
    enableListener ();
}

void ShadowsHighlights::write (ProcParams* pp, ParamsEdited* pedited)
{

    pp->sh.radius        = (int)radius->getValue ();
    pp->sh.highlights    = (int)highlights->getValue ();
    pp->sh.htonalwidth   = (int)h_tonalwidth->getValue ();
    pp->sh.shadows       = (int)shadows->getValue ();
    pp->sh.stonalwidth   = (int)s_tonalwidth->getValue ();
    pp->sh.enabled       = getEnabled();

    if (colorspace->get_active_row_number() == 0) {
        pp->sh.lab = false;
    } else if (colorspace->get_active_row_number() == 1) {
        pp->sh.lab = true;
    }

    if (pedited) {
        pedited->sh.radius          = radius->getEditedState ();
        pedited->sh.highlights      = highlights->getEditedState ();
        pedited->sh.htonalwidth     = h_tonalwidth->getEditedState ();
        pedited->sh.shadows         = shadows->getEditedState ();
        pedited->sh.stonalwidth     = s_tonalwidth->getEditedState ();
        pedited->sh.enabled         = !get_inconsistent();
        pedited->sh.lab = colorspace->get_active_row_number() != 2;
    }
}

void ShadowsHighlights::setDefaults (const ProcParams* defParams, const ParamsEdited* pedited)
{

    radius->setDefault (defParams->sh.radius);
    highlights->setDefault (defParams->sh.highlights);
    h_tonalwidth->setDefault (defParams->sh.htonalwidth);
    shadows->setDefault (defParams->sh.shadows);
    s_tonalwidth->setDefault (defParams->sh.stonalwidth);

    if (pedited) {
        radius->setDefaultEditedState       (pedited->sh.radius ? Edited : UnEdited);
        highlights->setDefaultEditedState   (pedited->sh.highlights ? Edited : UnEdited);
        h_tonalwidth->setDefaultEditedState (pedited->sh.htonalwidth ? Edited : UnEdited);
        shadows->setDefaultEditedState      (pedited->sh.shadows ? Edited : UnEdited);
        s_tonalwidth->setDefaultEditedState (pedited->sh.stonalwidth ? Edited : UnEdited);
    } else {
        radius->setDefaultEditedState       (Irrelevant);
        highlights->setDefaultEditedState   (Irrelevant);
        h_tonalwidth->setDefaultEditedState (Irrelevant);
        shadows->setDefaultEditedState      (Irrelevant);
        s_tonalwidth->setDefaultEditedState (Irrelevant);
    }
}

void ShadowsHighlights::adjusterChanged (Adjuster* a, double newval)
{
    autoEnable();
    if (listener && getEnabled()) {
        const Glib::ustring costr = Glib::ustring::format ((int)a->getValue());

        if (a == highlights) {
            listener->panelChanged (EvSHHighlights, costr);
        } else if (a == h_tonalwidth) {
            listener->panelChanged (EvSHHLTonalW, costr);
        } else if (a == shadows) {
            listener->panelChanged (EvSHShadows, costr);
        } else if (a == s_tonalwidth) {
            listener->panelChanged (EvSHSHTonalW, costr);
        } else if (a == radius) {
            listener->panelChanged (EvSHRadius, costr);
        }
    }
}

void ShadowsHighlights::enabledChanged ()
{

    if (listener) {
        if (get_inconsistent()) {
            listener->panelChanged (EvSHEnabled, M("GENERAL_UNCHANGED"));
        } else if (getEnabled()) {
            listener->panelChanged (EvSHEnabled, M("GENERAL_ENABLED"));
        } else {
            listener->panelChanged (EvSHEnabled, M("GENERAL_DISABLED"));
        }
    }
}

void ShadowsHighlights::colorspaceChanged()
{
    if (listener && (multiImage || getEnabled()) ) {
        listener->panelChanged(EvSHColorspace, colorspace->get_active_text());
    }
}

void ShadowsHighlights::setBatchMode (bool batchMode)
{

    ToolPanel::setBatchMode (batchMode);
    advancedSection->setBatchMode(batchMode);
    radius->showEditedCB ();
    highlights->showEditedCB ();
    h_tonalwidth->showEditedCB ();
    shadows->showEditedCB ();
    s_tonalwidth->showEditedCB ();
    colorspace->append(M("GENERAL_UNCHANGED"));
}

void ShadowsHighlights::setAdjusterBehavior (bool hadd, bool sadd)
{

    highlights->setAddMode(hadd);
    shadows->setAddMode(sadd);
}

void ShadowsHighlights::trimValues (rtengine::procparams::ProcParams* pp)
{

    highlights->trimValue(pp->sh.highlights);
    shadows->trimValue(pp->sh.shadows);
}
