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
#include <sstream>

#include "preprocess.h"

#include "eventmapper.h"
#include "guiutils.h"
#include "options.h"

#include "rtengine/procparams.h"

using namespace rtengine;
using namespace rtengine::procparams;

const Glib::ustring PreProcess::TOOL_NAME = "preprocess";

PreProcess::PreProcess () : FoldableToolPanel(this, TOOL_NAME, M("TP_PREPROCESS_LABEL"), App::get().options().prevdemo != PD_Sidecar)
{
    const auto& options = App::get().options();

    // Stuck-pixel fix: the summary row, the only part the Detail group shows.
    auto m = ProcEventMapper::getInstance();
    EvStuckPixel = m->newEvent(DARKFRAME, "HISTORY_MSG_STUCKPIX");
    EvStuckPixelSensitivity = m->newEvent(DARKFRAME, "HISTORY_MSG_STUCKPIX_SENSITIVITY");

    stuckPixel = Gtk::manage(new Gtk::CheckButton(M("TP_PREPROCESS_STUCKPIX")));
    stuckPixel->set_tooltip_markup(M("TP_PREPROCESS_STUCKPIX_TOOLTIP"));
    stuckSensitivity = Gtk::manage(new Adjuster(M("TP_PREPROCESS_STUCKPIX_SENSITIVITY"), 0, 100, 1, 50));
    stuckSensitivity->set_tooltip_markup(M("TP_PREPROCESS_STUCKPIX_SENSITIVITY_TOOLTIP"));
    stuckSensitivity->setAdjusterListener(this);
    // every change re-reads the raw file
    stuckSensitivity->setDelay(std::max(options.adjusterMinDelay, options.adjusterMaxDelay));

    Gtk::Box* stuckBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
    stuckBox->pack_start(*stuckPixel, Gtk::PACK_SHRINK, 0);
    stuckBox->pack_start(*stuckSensitivity, Gtk::PACK_SHRINK, 0);
    getSummaryBox()->pack_start(*stuckBox);
    getSummaryBox()->show_all();
    spixelconn = stuckPixel->signal_toggled().connect(sigc::mem_fun(*this, &PreProcess::stuckPixelChanged), true);

    // The classic Bayer-only hot/dead filter stays in the body.
    Gtk::Box* hotdeadPixel = Gtk::manage( new Gtk::Box () );
    hotdeadPixel->set_spacing(4);
    hotPixel = Gtk::manage(new Gtk::CheckButton((M("TP_PREPROCESS_HOTPIXFILT"))));
    deadPixel = Gtk::manage(new Gtk::CheckButton((M("TP_PREPROCESS_DEADPIXFILT"))));

    hotPixel->set_tooltip_markup (M("TP_PREPROCESS_HOTPIXFILT_TOOLTIP"));
    deadPixel->set_tooltip_markup (M("TP_PREPROCESS_DEADPIXFILT_TOOLTIP"));

    hotdeadPixel->pack_start( *hotPixel, Gtk::PACK_SHRINK);
    hotdeadPixel->pack_start( *deadPixel, Gtk::PACK_SHRINK, 0);
    pack_start(*hotdeadPixel, Gtk::PACK_SHRINK, 0);
    hdThreshold = Gtk::manage (new Adjuster (M("TP_RAW_HD"), 20, 200, 2, 100));
    hdThreshold->set_tooltip_markup (M("TP_RAW_HD_TOOLTIP"));
    hdThreshold->setAdjusterListener (this);

    hdThreshold->setDelay(std::max(options.adjusterMinDelay, options.adjusterMaxDelay));

    hdThreshold->show();
    pack_start( *hdThreshold, Gtk::PACK_SHRINK, 4);

//  hotdeadPixel->show();
    hpixelconn = hotPixel->signal_toggled().connect ( sigc::mem_fun(*this, &PreProcess::hotPixelChanged), true);
    dpixelconn = deadPixel->signal_toggled().connect ( sigc::mem_fun(*this, &PreProcess::deadPixelChanged), true);
}

void PreProcess::read(const rtengine::procparams::ProcParams* pp, const ParamsEdited* pedited)
{
    disableListener ();
    spixelconn.block (true);
    hpixelconn.block (true);
    dpixelconn.block (true);

    if(pedited ) {
        stuckPixel->set_inconsistent (!pedited->raw.stuckPixelFix);
        stuckSensitivity->setEditedState (pedited->raw.stuckPixelSensitivity ? Edited : UnEdited);
        hotPixel->set_inconsistent (!pedited->raw.hotPixelFilter);
        deadPixel->set_inconsistent (!pedited->raw.deadPixelFilter);
    }

    lastStuck = pp->raw.stuckPixelFix;
    stuckPixel->set_active (pp->raw.stuckPixelFix);
    stuckSensitivity->setValue (pp->raw.stuckPixelSensitivity);
    stuckSensitivity->set_sensitive (pp->raw.stuckPixelFix || stuckPixel->get_inconsistent());

    lastHot = pp->raw.hotPixelFilter;
    lastDead = pp->raw.deadPixelFilter;
    hotPixel->set_active (pp->raw.hotPixelFilter);
    deadPixel->set_active (pp->raw.deadPixelFilter);
    hdThreshold->setValue (pp->raw.hotdeadpix_thresh);
    spixelconn.block (false);
    hpixelconn.block (false);
    dpixelconn.block (false);
    enableListener ();
}

void PreProcess::write( rtengine::procparams::ProcParams* pp, ParamsEdited* pedited)
{
    pp->raw.stuckPixelFix = stuckPixel->get_active();
    pp->raw.stuckPixelSensitivity = stuckSensitivity->getIntValue();
    pp->raw.hotPixelFilter = hotPixel->get_active();
    pp->raw.deadPixelFilter = deadPixel->get_active();
    pp->raw.hotdeadpix_thresh = hdThreshold->getIntValue();

    if (pedited) {
        pedited->raw.stuckPixelFix = !stuckPixel->get_inconsistent();
        pedited->raw.stuckPixelSensitivity = stuckSensitivity->getEditedState ();
        pedited->raw.hotdeadpix_thresh = hdThreshold->getEditedState ();
        pedited->raw.hotPixelFilter = !hotPixel->get_inconsistent();
        pedited->raw.deadPixelFilter = !deadPixel->get_inconsistent();
    }
}

void PreProcess::adjusterChanged(Adjuster* a, double newval)
{
    if (listener) {
        if (a == hdThreshold) {
            listener->panelChanged (EvPreProcessHotDeadThresh, a->getTextValue() );
        } else if (a == stuckSensitivity && (stuckPixel->get_active() || batchMode)) {
            listener->panelChanged (EvStuckPixelSensitivity, a->getTextValue() );
        }
    }
}

void PreProcess::setBatchMode (bool batchMode)
{
    ToolPanel::setBatchMode (batchMode);
    stuckSensitivity->showEditedCB ();
    hdThreshold->showEditedCB ();
}

void PreProcess::stuckPixelChanged ()
{
    if (batchMode) {
        if (stuckPixel->get_inconsistent()) {
            stuckPixel->set_inconsistent (false);
            spixelconn.block (true);
            stuckPixel->set_active (false);
            spixelconn.block (false);
        } else if (lastStuck) {
            stuckPixel->set_inconsistent (true);
        }

        lastStuck = stuckPixel->get_active ();
    }

    stuckSensitivity->set_sensitive (stuckPixel->get_active() || stuckPixel->get_inconsistent());

    if (listener) {
        listener->panelChanged (EvStuckPixel, stuckPixel->get_active() ? M("GENERAL_ENABLED") : M("GENERAL_DISABLED"));
    }
}

void PreProcess::hotPixelChanged ()
{
    if (batchMode) {
        if (hotPixel->get_inconsistent()) {
            hotPixel->set_inconsistent (false);
            hpixelconn.block (true);
            hotPixel->set_active (false);
            hpixelconn.block (false);
        } else if (lastHot) {
            hotPixel->set_inconsistent (true);
        }

        lastHot = hotPixel->get_active ();
    }

    if (listener) {
        listener->panelChanged (EvPreProcessHotPixel, hotPixel->get_active() ? M("GENERAL_ENABLED") : M("GENERAL_DISABLED"));
    }
}

void PreProcess::deadPixelChanged ()
{
    if (batchMode) {
        if (deadPixel->get_inconsistent()) {
            deadPixel->set_inconsistent (false);
            dpixelconn.block (true);
            deadPixel->set_active (false);
            dpixelconn.block (false);
        } else if (lastDead) {
            deadPixel->set_inconsistent (true);
        }

        lastDead = deadPixel->get_active ();
    }

    if (listener) {
        listener->panelChanged (EvPreProcessDeadPixel, deadPixel->get_active() ? M("GENERAL_ENABLED") : M("GENERAL_DISABLED"));
    }
}
