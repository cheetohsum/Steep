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

#ifdef RT_AI_MASKING

#include "tools/locallabtools.h"

class LocallabAIMask :
    public Gtk::Box,
    public LocallabTool
{
private:
    MyComboBoxText* const aiMaskClassCombo;
    MyComboBoxText* const aiMaskShapeOpCombo;
    Adjuster* const aiMaskThreshold;
    Adjuster* const aiMaskFeather;
    Adjuster* const aiMaskBlur;
    Gtk::CheckButton* const aiMaskInvert;
    Adjuster* const aiMaskOpacity;
    Adjuster* const aiMaskRefineRadius;
    Adjuster* const aiMaskRefineEps;

    sigc::connection aiMaskClassConn, aiMaskInvertConn, aiMaskShapeOpConn;

    rtengine::ProcEvent EvlocallabAIMask;

public:
    LocallabAIMask();
    ~LocallabAIMask();

    bool isMaskViewActive() override;
    void resetMaskView() override;
    void getMaskView(int &colorMask, int &colorMaskinv, int &expMask, int &expMaskinv,
                     int &shMask, int &shMaskinv, int &vibMask, int &softMask,
                     int &blMask, int &tmMask, int &retiMask, int &sharMask,
                     int &lcMask, int &cbMask, int &logMask, int &maskMask,
                     int &cieMask) override;

    Gtk::ToggleButton *getPreviewDeltaEButton() const override;
    sigc::connection *getPreviewDeltaEButtonConnection() override;

    void updateAdviceTooltips(const bool showTooltips) override;

    void disableListener() override;
    void enableListener() override;
    void read(const rtengine::procparams::ProcParams* pp, const ParamsEdited* pedited = nullptr) override;
    void write(rtengine::procparams::ProcParams* pp, ParamsEdited* pedited = nullptr) override;
    void setDefaults(const rtengine::procparams::ProcParams* defParams, const ParamsEdited* pedited = nullptr) override;
    void adjusterChanged(Adjuster* a, double newval) override;

private:
    void complexityModeChanged();
    void enabledChanged() override;
    void convertParamToNormal() override;
    void convertParamToSimple() override;
    void updateGUIToMode(const modeType complexity) override;

    void aiMaskClassChanged();
    void aiMaskShapeOpChanged();
    void aiMaskInvertChanged();
};

#endif // RT_AI_MASKING
