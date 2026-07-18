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

    degree = Gtk::manage (new Adjuster ("", -45, 45, 0.01, 0));
    degree->setAdjusterListener (this);
    degree->setDelay(12, 32);
    degree->hideResetButton();
    degree->hideSpinButton();
    degree->set_halign(Gtk::ALIGN_FILL);
    degree->set_margin_start(10);
    degree->set_margin_end(10);
    pack_start (*degree, false, false);

    // The Auto Level action moved to the editor toolbar (left of the
    // line-level tool); this panel keeps only the status feedback label.
    autoLevel = nullptr;

    autoLevelStatus = Gtk::manage(new Gtk::Label());
    autoLevelStatus->set_xalign(1.0f);
    autoLevelStatus->set_line_wrap(true);
    autoLevelStatus->set_margin_start(10);
    autoLevelStatus->set_margin_end(10);
    autoLevelStatus->get_style_context()->add_class("dim-label");
    autoLevelStatus->set_no_show_all(true);
    pack_start(*autoLevelStatus, false, false);

    degree->setLogScale(2, 0);

    setExpandable(false);
    setFlatMode(true);
    show_all ();
    autoLevelStatus->hide();
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
