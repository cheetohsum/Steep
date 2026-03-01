/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2024
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
#include "options.h"
#include "widgets/basic/adjuster.h"

class WatermarkPanel : public Gtk::Grid, public AdjusterListener
{
    Gtk::CheckButton* enableChk;
    Gtk::Entry* textEntry;
    Gtk::FontButton* fontButton;
    MyComboBoxText* sizeMode;
    Adjuster* fontSizeAdj;
    Adjuster* sizePercentAdj;
    Gtk::ColorButton* textColorBtn;
    Adjuster* opacityAdj;

    // Stroke
    Gtk::CheckButton* strokeEnableChk;
    Gtk::ColorButton* strokeColorBtn;
    Adjuster* strokeWidthAdj;

    // Shadow
    Gtk::CheckButton* shadowEnableChk;
    Gtk::ColorButton* shadowColorBtn;
    Adjuster* shadowOffsetXAdj;
    Adjuster* shadowOffsetYAdj;
    Adjuster* shadowBlurAdj;

    // Position
    MyComboBoxText* positionCombo;
    Adjuster* marginXAdj;
    Adjuster* marginYAdj;
    Adjuster* rotationAdj;

    // Preview
    Gtk::Button* previewBtn;
    Gtk::Window* previewWindow;

    void updateSensitivity();
    void sizeModeChanged();
    void showPreview();

public:
    WatermarkPanel();
    ~WatermarkPanel() override;

    void init(const WatermarkOptions& opts);
    WatermarkOptions getOptions();

    void adjusterChanged(Adjuster* a, double newval) override;
};
