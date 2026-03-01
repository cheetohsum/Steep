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
#include "modebuttonbar.h"
#include "multilangmgr.h"
#include "rtimage.h"

ModeButtonBar::ModeButtonBar() :
    Gtk::Box(Gtk::ORIENTATION_HORIZONTAL),
    activeMode(EditorMode::EDIT),
    blockSignal(false)
{
    set_name("ModeButtonBar");
    get_style_context()->add_class("ModeButtonBar");
    set_homogeneous(true);
    set_spacing(0);

    auto createButton = [this](const Glib::ustring& iconName, const Glib::ustring& label, EditorMode mode) -> Gtk::ToggleButton* {
        auto* btn = Gtk::manage(new Gtk::ToggleButton());
        auto* img = Gtk::manage(new RTImage(iconName));
        btn->set_image(*img);
        btn->set_always_show_image(true);
        btn->set_tooltip_text(label);
        btn->get_style_context()->add_class("ModeButton");
        btn->signal_toggled().connect(sigc::bind(
            sigc::mem_fun(*this, &ModeButtonBar::onButtonToggled), btn, mode));
        pack_start(*btn, Gtk::PACK_EXPAND_WIDGET);
        return btn;
    };

    presetsButton = createButton("mode-presets", M("MODE_PRESETS"), EditorMode::PRESETS);
    editButton    = createButton("mode-edit",    M("MODE_EDIT"),    EditorMode::EDIT);
    cropButton    = createButton("mode-crop",    M("MODE_CROP"),    EditorMode::CROPPING);
    maskButton    = createButton("mode-mask",    M("MODE_MASK"),    EditorMode::MASK);

    // Default to Edit mode
    blockSignal = true;
    editButton->set_active(true);
    blockSignal = false;

    show_all();
}

sigc::signal<void, EditorMode> ModeButtonBar::signal_mode_changed()
{
    return modeChangedSignal;
}

void ModeButtonBar::setActiveMode(EditorMode mode)
{
    if (mode == activeMode) {
        return;
    }

    blockSignal = true;

    presetsButton->set_active(mode == EditorMode::PRESETS);
    editButton->set_active(mode == EditorMode::EDIT);
    cropButton->set_active(mode == EditorMode::CROPPING);
    maskButton->set_active(mode == EditorMode::MASK);

    activeMode = mode;
    blockSignal = false;

    modeChangedSignal.emit(mode);
}

EditorMode ModeButtonBar::getActiveMode() const
{
    return activeMode;
}

void ModeButtonBar::setModeVisible(EditorMode mode, bool visible)
{
    Gtk::ToggleButton* btn = nullptr;
    switch (mode) {
        case EditorMode::PRESETS: btn = presetsButton; break;
        case EditorMode::EDIT:    btn = editButton; break;
        case EditorMode::CROPPING:    btn = cropButton; break;
        case EditorMode::MASK:    btn = maskButton; break;
    }
    if (btn) {
        if (visible) {
            btn->show();
        } else {
            btn->hide();
        }
    }
}

void ModeButtonBar::onButtonToggled(Gtk::ToggleButton* button, EditorMode mode)
{
    if (blockSignal) {
        return;
    }

    if (button->get_active()) {
        // Deactivate all other buttons
        blockSignal = true;
        if (button != presetsButton) presetsButton->set_active(false);
        if (button != editButton)    editButton->set_active(false);
        if (button != cropButton)    cropButton->set_active(false);
        if (button != maskButton)    maskButton->set_active(false);
        blockSignal = false;

        activeMode = mode;
        modeChangedSignal.emit(mode);
    } else {
        // Don't allow deactivating the active button by clicking it again
        blockSignal = true;
        button->set_active(true);
        blockSignal = false;
    }
}
