/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2024 RawTherapee team
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

#include <memory>

enum class EditorMode {
    PRESETS,
    EDIT,
    CROPPING,
    MASK
};

class ModeButtonBar : public Gtk::Box
{
public:
    ModeButtonBar();

    sigc::signal<void, EditorMode> signal_mode_changed();

    void setActiveMode(EditorMode mode);
    EditorMode getActiveMode() const;

    void setModeVisible(EditorMode mode, bool visible);

private:
    void onButtonToggled(Gtk::ToggleButton* button, EditorMode mode);

    Gtk::ToggleButton* presetsButton;
    Gtk::ToggleButton* editButton;
    Gtk::ToggleButton* cropButton;
    Gtk::ToggleButton* maskButton;

    EditorMode activeMode;
    bool blockSignal;

    // Right-click on the Edit button opens a slider that scales the setting
    // rows' type. Built on first use.
    void showPillScalePopover();
    std::unique_ptr<Gtk::Popover> pillScalePopover_;
    Gtk::Scale* pillScaleSlider_ = nullptr;

    sigc::signal<void, EditorMode> modeChangedSignal;
};
