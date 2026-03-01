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
#pragma once

#include <gtkmm.h>

#include "guiutils.h"
#include "toolpanel.h"
#include "widgets/basic/adjuster.h"

namespace rtengine { class StagedImageProcessor; }

class AIDenoise final :
    public ToolParamBlock,
    public AdjusterListener,
    public FoldableToolPanel
{

protected:
    Adjuster* isoConditioning;
    Adjuster* blend;
    AdvancedSection* advancedSection;
    Gtk::CheckButton* useGpu;
    Gtk::Button* denoiseBtn;
    Gtk::Button* cancelBtn;
    Gtk::Label* statusLabel;

    Glib::ustring imagePath_;
    rtengine::StagedImageProcessor* ipc_;

public:
    static const Glib::ustring TOOL_NAME;

    AIDenoise ();

    void read           (const rtengine::procparams::ProcParams* pp, const ParamsEdited* pedited = nullptr) override;
    void write          (rtengine::procparams::ProcParams* pp, ParamsEdited* pedited = nullptr) override;
    void setDefaults    (const rtengine::procparams::ProcParams* defParams, const ParamsEdited* pedited = nullptr) override;
    void setBatchMode   (bool batchMode) override;

    void adjusterChanged (Adjuster* a, double newval) override;
    void enabledChanged  () override;

    void trimValues          (rtengine::procparams::ProcParams* pp) override;

    void setImagePath (const Glib::ustring& path);
    void setImProcCoordinator (rtengine::StagedImageProcessor* ipc);
    void onDenoiseClicked ();
    void onCancelClicked ();
    void updateStatus (const Glib::ustring& text);
};
