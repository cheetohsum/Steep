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
#include <iomanip>
#include <cmath>

#include "rotate.h"

#include "guiutils.h"
#include "lensgeomlistener.h"
#include "rtimage.h"

#include "rtengine/procparams.h"

using namespace rtengine;
using namespace rtengine::procparams;

const Glib::ustring Rotate::TOOL_NAME = "rotate";

Rotate::Rotate () : FoldableToolPanel(this, TOOL_NAME, M("TP_ROTATE_LABEL"))
{

    rlistener = nullptr;

    // The Level heading names it; the pill shows only the angle in degrees,
    // the bipolar fill growing from 0 either way.
    degree = Gtk::manage (new Adjuster ("", -45, 45, 0.01, 0));
    degree->setAdjusterListener (this);
    degree->setDelay(12, 32);
    degree->hideResetButton();
    degree->hideSpinButton();
    degree->setValueSuffix("°");
    degree->set_tooltip_text(M("TP_ROTATE_ANGLE_TOOLTIP"));
    degree->set_halign(Gtk::ALIGN_FILL);
    degree->set_margin_start(6);
    degree->set_margin_end(6);
    pack_start (*degree, false, false);

    // Auto level and Straighten: an icon pair the Level heading carries on
    // its right (getActionsWidget), like the turn/flip icons on the Rotate
    // row above it.
    levelActions = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 0));
    makeSegmented(*levelActions);
    levelActions->get_style_context()->add_class("rotate-actions");
    levelActions->set_valign(Gtk::ALIGN_CENTER);

    autoLevel = Gtk::manage(new Gtk::Button());
    autoLevel->set_image(*Gtk::manage(new RTImage("auto-level", Gtk::ICON_SIZE_BUTTON)));
    autoLevel->set_tooltip_text(M("TP_ROTATE_AUTO_LEVEL_TOOLTIP"));
    autoLevel->signal_clicked().connect(sigc::mem_fun(*this, &Rotate::autoLevelPressed));
    levelActions->pack_start(*autoLevel, Gtk::PACK_SHRINK);

    selectStraight = Gtk::manage(new Gtk::Button());
    selectStraight->set_image(*Gtk::manage(new RTImage("rotate-straighten-small", Gtk::ICON_SIZE_BUTTON)));
    selectStraight->set_tooltip_text(M("TP_ROTATE_STRAIGHTEN_TOOLTIP"));
    selectStraight->signal_clicked().connect([this]() {
        if (rlistener) {
            rlistener->straightenRequested();
        }
    });
    levelActions->pack_start(*selectStraight, Gtk::PACK_SHRINK);

    // Shown by the Level heading (getStatusWidget), so a result never
    // pushes the rows below it down and back up again.
    autoLevelStatus = Gtk::manage(new Gtk::Label());
    autoLevelStatus->set_xalign(1.0f);
    autoLevelStatus->set_ellipsize(Pango::ELLIPSIZE_END);
    autoLevelStatus->get_style_context()->add_class("dim-label");
    autoLevelStatus->get_style_context()->add_class("level-status");
    autoLevelStatus->set_no_show_all(true);

    degree->setLogScale(2, 0);

    setExpandable(false);
    setFlatMode(true);
    show_all ();
    levelActions->show_all();
}

Rotate::~Rotate ()
{
    autoLevelStatusConn.disconnect();
}

void Rotate::read (const ProcParams* pp, const ParamsEdited* pedited)
{

    disableListener ();

    if (pedited) {
        degree->setEditedState (pedited->rotate.degree ? Edited : UnEdited);
    }

    degree->setValue (pp->rotate.degree);

    enableListener ();
}

void Rotate::write (ProcParams* pp, ParamsEdited* pedited)
{

    pp->rotate.degree = degree->getValue ();

    if (pedited) {
        pedited->rotate.degree = degree->getEditedState ();
    }
}

void Rotate::setDefaults (const ProcParams* defParams, const ParamsEdited* pedited)
{

    degree->setDefault (defParams->rotate.degree);

    if (pedited) {
        degree->setDefaultEditedState (pedited->rotate.degree ? Edited : UnEdited);
    } else {
        degree->setDefaultEditedState (Irrelevant);
    }
}

void Rotate::adjusterChanged(Adjuster* a, double newval)
{
    if (listener) {
        listener->panelChanged(EvROTDegree, Glib::ustring::format (std::setw(3), std::fixed, std::setprecision(2), degree->getValue()));
    }
}

void Rotate::straighten (double deg)
{

    degree->setValue (degree->getValue() + deg);
    degree->setEditedState (Edited);

    if (listener) {
        listener->panelChanged (EvROTDegree, Glib::ustring::format (std::setw(3), std::fixed, std::setprecision(2), degree->getValue()));
    }
}

void Rotate::autoLevelPressed ()
{
    if (!rlistener || batchMode) {
        return;
    }

    double correction = 0.0;
    const bool detected = rlistener->autoLevelRequested(correction);

    if (!detected || std::abs(degree->getValue() + correction) > 45.0) {
        showAutoLevelStatus(M("TP_ROTATE_AUTO_LEVEL_FAILED"));
        return;
    }

    if (std::abs(correction) < 0.015) {
        showAutoLevelStatus(M("TP_ROTATE_AUTO_LEVEL_ALREADY"));
        return;
    }

    straighten(correction);
    showAutoLevelStatus(Glib::ustring::compose(
        M("TP_ROTATE_AUTO_LEVEL_APPLIED"),
        Glib::ustring::format(std::fixed, std::setprecision(2), correction)));
}

void Rotate::showAutoLevelStatus (const Glib::ustring& message)
{
    autoLevelStatusConn.disconnect();
    autoLevelStatus->set_text(message);
    autoLevelStatus->show();
    autoLevelStatusConn = Glib::signal_timeout().connect([this]() {
        autoLevelStatus->hide();
        return false;
    }, 3500);
}

void Rotate::setBatchMode (bool batchMode)
{

    ToolPanel::setBatchMode (batchMode);
    degree->showEditedCB ();
}

void Rotate::setAdjusterBehavior (bool rotadd)
{

    degree->setAddMode(rotadd);
}

void Rotate::trimValues (rtengine::procparams::ProcParams* pp)
{

    degree->trimValue(pp->rotate.degree);
}

void Rotate::setLevelingGridCallback (std::function<void(bool)> cb)
{
    degree->setInteractionCallback(std::move(cb));
}
