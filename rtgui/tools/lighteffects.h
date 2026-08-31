/*
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
#pragma once

#include <gtkmm.h>

#include "toolpanel.h"
#include "widgets/basic/adjuster.h"

/// Glow, halation and lens flare — three ways of spreading light out of the
/// highlights, sharing one threshold.
class LightEffects final :
    public ToolParamBlock,
    public AdjusterListener,
    public FoldableToolPanel
{
public:
    static const Glib::ustring TOOL_NAME;

    LightEffects();

    void read(const rtengine::procparams::ProcParams* pp, const ParamsEdited* pedited = nullptr) override;
    void write(rtengine::procparams::ProcParams* pp, ParamsEdited* pedited = nullptr) override;
    void setDefaults(const rtengine::procparams::ProcParams* defParams, const ParamsEdited* pedited = nullptr) override;
    void setBatchMode(bool batchMode) override;
    void enabledChanged() override;

    void adjusterChanged(Adjuster* a, double newval) override;
    void adjusterAutoToggled(Adjuster* a, bool newval) override {}

private:
    Adjuster* threshold;
    Adjuster* glow;
    Adjuster* glowRadius;
    Adjuster* halation;
    Adjuster* halationSize;
    Adjuster* halationWarmth;
    Adjuster* flare;
    Adjuster* flareLength;
    Adjuster* flareAngle;

    rtengine::ProcEvent EvEnabled;
    rtengine::ProcEvent EvThreshold;
    rtengine::ProcEvent EvGlow;
    rtengine::ProcEvent EvGlowRadius;
    rtengine::ProcEvent EvHalation;
    rtengine::ProcEvent EvHalationSize;
    rtengine::ProcEvent EvHalationWarmth;
    rtengine::ProcEvent EvFlare;
    rtengine::ProcEvent EvFlareLength;
    rtengine::ProcEvent EvFlareAngle;
};
