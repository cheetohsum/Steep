
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
#include "toolbar.h"
#include "multilangmgr.h"
#include "guiutils.h"
#include "lockablecolorpicker.h"
#include "rtimage.h"

ToolBar::ToolBar () : showColPickers(true), listener (nullptr), pickerListener(nullptr)
{

    editingMode = false;
    handimg.reset(new RTImage("hand-open", Gtk::ICON_SIZE_LARGE_TOOLBAR));
    editinghandimg.reset(new RTImage("crosshair-adjust", Gtk::ICON_SIZE_LARGE_TOOLBAR));

    handTool = Gtk::manage (new Gtk::ToggleButton ());
    handTool->add (*handimg);
    handimg->show ();
    handTool->set_relief(Gtk::RELIEF_NONE);
    handTool->show ();

    // Hand tool not packed — it's always active by default, button hidden to save space
    // pack_start (*handTool);

    wbTool = Gtk::manage (new Gtk::ToggleButton ());
    Gtk::Image* wbimg = Gtk::manage (new RTImage ("color-picker", Gtk::ICON_SIZE_LARGE_TOOLBAR));
    wbTool->add (*wbimg);
    wbimg->show ();
    wbTool->set_relief(Gtk::RELIEF_NONE);
    wbTool->show ();

    // wbTool and colPickerTool are NOT packed here — they get packed
    // into the Color tool group by ToolPanelCoordinator.

    showcolpickersimg.reset(new RTImage("color-picker-bars", Gtk::ICON_SIZE_LARGE_TOOLBAR));
    hidecolpickersimg.reset(new RTImage("color-picker-hide", Gtk::ICON_SIZE_LARGE_TOOLBAR));

    colPickerTool = Gtk::manage (new Gtk::ToggleButton ());
    colPickerTool->add (*showcolpickersimg);
    showcolpickersimg->show ();
    colPickerTool->set_relief(Gtk::RELIEF_NONE);
    colPickerTool->show ();

    // Reference so Gtk::manage doesn't destroy them before they're packed
    wbTool->reference();
    colPickerTool->reference();

    cropTool = Gtk::manage (new Gtk::ToggleButton ());
    Gtk::Image* cropimg = Gtk::manage (new RTImage ("crop", Gtk::ICON_SIZE_LARGE_TOOLBAR));
    cropTool->add (*cropimg);
    cropimg->show ();
    cropTool->set_relief(Gtk::RELIEF_NONE);
    cropTool->show ();

    pack_start (*cropTool);

    // One-click auto level, left of the manual line-level (straighten) tool
    autoLevelBtn = Gtk::manage (new Gtk::Button ());
    Gtk::Image* autoLevelImg = Gtk::manage (new RTImage ("auto-level", Gtk::ICON_SIZE_LARGE_TOOLBAR));
    autoLevelBtn->add (*autoLevelImg);
    autoLevelImg->show ();
    autoLevelBtn->set_relief(Gtk::RELIEF_NONE);
    autoLevelBtn->set_tooltip_markup (M("TP_ROTATE_AUTO_LEVEL_TOOLTIP"));
    autoLevelBtn->show ();
    autoLevelBtn->signal_clicked().connect([this]() {
        if (autoLevelHandler_) {
            autoLevelHandler_();
        }
    });
    pack_start (*autoLevelBtn);

    straTool = Gtk::manage (new Gtk::ToggleButton ());
    Gtk::Image* straimg = Gtk::manage (new RTImage ("rotate-straighten", Gtk::ICON_SIZE_LARGE_TOOLBAR));
    straTool->add (*straimg);
    straimg->show ();
    straTool->set_relief(Gtk::RELIEF_NONE);
    straTool->show ();

    pack_start (*straTool);

    perspTool = Gtk::manage(new Gtk::ToggleButton());
    Gtk::Image* perspimg = Gtk::manage(new RTImage("perspective-vertical-bottom", Gtk::ICON_SIZE_LARGE_TOOLBAR));
    perspTool->set_image(*perspimg);
    perspTool->set_relief(Gtk::RELIEF_NONE);
    pack_start(*perspTool);

    perspGridTool = Gtk::manage(new Gtk::ToggleButton());
    Gtk::Image* perspGridImg = Gtk::manage(new RTImage("perspective-grid", Gtk::ICON_SIZE_LARGE_TOOLBAR));
    perspGridTool->add(*perspGridImg);
    perspGridImg->show();
    perspGridTool->set_relief(Gtk::RELIEF_NONE);
    perspGridTool->show();
    pack_start(*perspGridTool);


    handTool->set_active (true);
    current = TMHand;
    allowNoTool = false;

    handConn = handTool->signal_toggled().connect( sigc::mem_fun(*this, &ToolBar::hand_pressed));
    wbConn   = wbTool->signal_toggled().connect( sigc::mem_fun(*this, &ToolBar::wb_pressed));
    cpConn   = colPickerTool->signal_button_press_event().connect_notify( sigc::mem_fun(*this, &ToolBar::colPicker_pressed));
    cropConn = cropTool->signal_toggled().connect( sigc::mem_fun(*this, &ToolBar::crop_pressed));
    straConn = straTool->signal_toggled().connect( sigc::mem_fun(*this, &ToolBar::stra_pressed));
    perspConn = perspTool->signal_toggled().connect( sigc::mem_fun(*this, &ToolBar::persp_pressed));
    perspGridConn = perspGridTool->signal_toggled().connect( sigc::mem_fun(*this, &ToolBar::persp_grid_pressed));

    handTool->set_tooltip_markup (M("TOOLBAR_TOOLTIP_HAND"));
    wbTool->set_tooltip_markup (M("TOOLBAR_TOOLTIP_WB"));
    colPickerTool->set_tooltip_markup (M("TOOLBAR_TOOLTIP_COLORPICKER"));
    cropTool->set_tooltip_markup (M("TOOLBAR_TOOLTIP_CROP"));
    straTool->set_tooltip_markup (M("TOOLBAR_TOOLTIP_STRAIGHTEN"));
    perspTool->set_tooltip_markup(M("TOOLBAR_TOOLTIP_PERSPECTIVE"));
    perspGridTool->set_tooltip_markup(M("TP_PERSPECTIVE_GRID_TOOLTIP"));
}

//
// Selects the desired tool without notifying the listener
//
void ToolBar::setTool (ToolMode tool)
{

    bool stopEdit;

    {
    ConnectionBlocker handBlocker(handConn);
    ConnectionBlocker straBlocker(straConn);
    ConnectionBlocker cropBlocker(cropConn);
    ConnectionBlocker perspBlocker(perspConn);
    ConnectionBlocker perspGridBlocker(perspGridConn);
    ConnectionBlocker wbWasBlocked(wbTool, wbConn), cpWasBlocked(colPickerTool, cpConn);

    stopEdit = tool == TMHand && (handTool->get_active() || (perspTool && perspTool->get_active()) || (perspGridTool && perspGridTool->get_active())) && editingMode && !blockEdit;

    handTool->set_active (false);

    if (wbTool) {
        wbTool->set_active (false);
    }

    cropTool->set_active (false);
    straTool->set_active (false);
    if (colPickerTool) {
        colPickerTool->set_active (false);
    }
    if (perspTool) {
        perspTool->set_active(false);
    }
    if (perspGridTool) {
        perspGridTool->set_active(false);
    }

    if (tool == TMHand) {
        handTool->set_active (true);
        handTool->grab_focus(); // switch focus to the handTool button
    } else if (tool == TMSpotWB) {
        if (wbTool) {
            wbTool->set_active (true);
        }
    } else if (tool == TMCropSelect) {
        cropTool->set_active (true);
    } else if (tool == TMStraighten) {
        straTool->set_active (true);
    } else if (tool == TMColorPicker) {
        if (colPickerTool) {
            colPickerTool->set_active (true);
        }
    } else if (tool == TMPerspective) {
        if (perspTool) {
            perspTool->set_active(true);
            handTool->set_image(*handimg);
        }
    } else if (tool == TMPerspectiveGrid) {
        if (perspGridTool) {
            perspGridTool->set_active(true);
            handTool->set_image(*handimg);
        }
    }

    current = tool;

    }

    if (stopEdit) {
        stopEditMode();

        if (listener) {
            listener->editModeSwitchedOff();
        }
    }
}

void ToolBar::startEditMode()
{
    if (!editingMode) {
        {
        ConnectionBlocker handBlocker(handConn);
        ConnectionBlocker straBlocker(straConn);
        ConnectionBlocker cropBlocker(cropConn);
        ConnectionBlocker perspBlocker(perspConn);
        ConnectionBlocker perspGridBlocker(perspGridConn);
        ConnectionBlocker wbWasBlocked(wbTool, wbConn), cpWasBlocked(colPickerTool, cpConn);

        if (current != TMHand) {
            if (colPickerTool) {
                colPickerTool->set_active(false);
            }
            if (wbTool) {
                wbTool->set_active (false);
            }

            cropTool->set_active (false);
            straTool->set_active (false);
            if (perspTool) {
                perspTool->set_active(false);
            }
            if (perspGridTool) {
                perspGridTool->set_active(false);
            }
            current = TMHand;
        }
        handTool->set_active (true);

        }

        editingMode = true;
        handTool->set_image(*editinghandimg);
    }

#ifndef NDEBUG
    else {
        printf("Editing mode already active!\n");
    }

#endif
}

void ToolBar::stopEditMode()
{
    if (editingMode) {
        editingMode = false;
        handTool->set_image(*handimg);
    }
}

void ToolBar::hand_pressed ()
{
    {
    ConnectionBlocker handBlocker(handConn);
    ConnectionBlocker straBlocker(straConn);
    ConnectionBlocker cropBlocker(cropConn);
    ConnectionBlocker perspBlocker(perspConn);
    ConnectionBlocker perspGridBlocker(perspGridConn);
    ConnectionBlocker wbWasBlocked(wbTool, wbConn), cpWasBlocked(colPickerTool, cpConn);

    if (editingMode && !blockEdit) {
        stopEditMode();
        if (listener) {
            listener->editModeSwitchedOff ();
        }
    }

    if (colPickerTool) {
        colPickerTool->set_active(false);
    }
    if (wbTool) {
        wbTool->set_active (false);
    }

    cropTool->set_active (false);
    straTool->set_active (false);
    if (perspTool) {
        perspTool->set_active(false);
    }
    if (perspGridTool) {
        perspGridTool->set_active(false);
    }
    handTool->set_active (true);

    if (current != TMHand) {
        current = TMHand;
    } else if (allowNoTool) {
        current = TMNone;
        handTool->set_active(false);
    }

    }

    if (listener) {
        listener->toolSelected (current);
    }
}

void ToolBar::wb_pressed ()
{
    {
    ConnectionBlocker handBlocker(handConn);
    ConnectionBlocker straBlocker(straConn);
    ConnectionBlocker cropBlocker(cropConn);
    ConnectionBlocker perspBlocker(perspConn);
    ConnectionBlocker perspGridBlocker(perspGridConn);
    ConnectionBlocker wbWasBlocked(wbTool, wbConn), cpWasBlocked(colPickerTool, cpConn);

    if (current != TMSpotWB) {
        if (editingMode) {
            stopEditMode();
            if (listener) {
                listener->editModeSwitchedOff ();
            }
        }
        handTool->set_active (false);
        cropTool->set_active (false);
        straTool->set_active (false);
        if (perspTool) {
            perspTool->set_active(false);
        }
        if (perspGridTool) {
            perspGridTool->set_active(false);
        }
        if (colPickerTool) {
            colPickerTool->set_active(false);
        }
        current = TMSpotWB;
    }

    if (wbTool) {
        wbTool->set_active (true);
    }

    }

    if (listener) {
        listener->toolSelected (TMSpotWB);
    }
}

void ToolBar::colPicker_pressed (GdkEventButton* event)
{

    if (event->button == 1) {
        {
        ConnectionBlocker handBlocker(handConn);
        ConnectionBlocker straBlocker(straConn);
        ConnectionBlocker cropBlocker(cropConn);
        ConnectionBlocker perspGridBlocker(perspGridConn);
        ConnectionBlocker wbWasBlocked(wbTool, wbConn);

        cropTool->set_active (false);
        if (wbTool) {
            wbTool->set_active (false);
        }
        straTool->set_active (false);
        if (perspTool) {
            perspTool->set_active(false);
        }
        if (perspGridTool) {
            perspGridTool->set_active(false);
        }

        if (current != TMColorPicker) {
            // Disabling all other tools, enabling the Picker tool and entering the "visible pickers" mode
            if (editingMode && !blockEdit) {
                stopEditMode();
                if (listener) {
                    listener->editModeSwitchedOff ();
                }
            }
            handTool->set_active (false);
            showColorPickers(true);
            current = TMColorPicker;
            if (pickerListener) {
                pickerListener->switchPickerVisibility (showColPickers);
            }
        } else {
            // Disabling the picker tool, enabling the Hand tool and keeping the "visible pickers" mode
            handTool->set_active (true);
            //colPickerTool->set_active (false);  Done by the standard event handler
            current = TMHand;
        }

        }

        if (listener) {
            listener->toolSelected (current);
        }
    } else if (event->button == 3) {
        if (current == TMColorPicker) {
            // Disabling the Picker tool and entering into the "invisible pickers" mode
            ConnectionBlocker handBlocker(handConn);
            ConnectionBlocker cpWasBlocked(cpConn);
            handTool->set_active (true);
            colPickerTool->set_active (false);
            current = TMHand;
            showColorPickers(false);
        } else {
            // The Picker tool is already disabled, entering into the "invisible pickers" mode
            switchColorPickersVisibility();
        }
        if (pickerListener) {
            pickerListener->switchPickerVisibility (showColPickers);
        }
    }
}

bool ToolBar::showColorPickers(bool showCP)
{
    if (showColPickers != showCP) {
        // Inverting the state
        colPickerTool->set_image(showCP ? *showcolpickersimg : *hidecolpickersimg);
        showColPickers = showCP;
        return true;
    }

    return false;
}

void ToolBar::switchColorPickersVisibility()
{
    // Inverting the state
    showColPickers = !showColPickers;
    colPickerTool->set_image(showColPickers ? *showcolpickersimg : *hidecolpickersimg);
}

void ToolBar::crop_pressed ()
{
    {
    ConnectionBlocker handBlocker(handConn);
    ConnectionBlocker straBlocker(straConn);
    ConnectionBlocker cropBlocker(cropConn);
    ConnectionBlocker perspBlocker(perspConn);
    ConnectionBlocker perspGridBlocker(perspGridConn);
    ConnectionBlocker wbWasBlocked(wbTool, wbConn), cpWasBlocked(colPickerTool, cpConn);

    if (editingMode) {
        stopEditMode();
        if (listener) {
            listener->editModeSwitchedOff ();
        }
    }
    handTool->set_active (false);
    if (colPickerTool) {
        colPickerTool->set_active(false);
    }
    if (wbTool) {
        wbTool->set_active (false);
    }

    straTool->set_active (false);
    if (perspTool) {
        perspTool->set_active(false);
    }
    if (perspGridTool) {
        perspGridTool->set_active(false);
    }
    cropTool->set_active (true);

    if (current != TMCropSelect) {
        current = TMCropSelect;
        cropTool->grab_focus ();
    } else if (allowNoTool) {
        current = TMNone;
        cropTool->set_active(false);
    }

    }

    if (listener) {
        listener->toolSelected (current);
    }
}

void ToolBar::stra_pressed ()
{
    {
    ConnectionBlocker handBlocker(handConn);
    ConnectionBlocker straBlocker(straConn);
    ConnectionBlocker cropBlocker(cropConn);
    ConnectionBlocker perspBlocker(perspConn);
    ConnectionBlocker perspGridBlocker(perspGridConn);
    ConnectionBlocker wbWasBlocked(wbTool, wbConn), cpWasBlocked(colPickerTool, cpConn);

    if (editingMode) {
        stopEditMode();
        if (listener) {
            listener->editModeSwitchedOff ();
        }
    }
    handTool->set_active (false);
    if (colPickerTool) {
        colPickerTool->set_active(false);
    }
    if (wbTool) {
        wbTool->set_active (false);
    }

    cropTool->set_active (false);
    if (perspTool) {
        perspTool->set_active(false);
    }
    if (perspGridTool) {
        perspGridTool->set_active(false);
    }
    straTool->set_active (true);

    if (current != TMStraighten) {
        current = TMStraighten;
    } else if (allowNoTool) {
        current = TMNone;
        straTool->set_active(false);
    }

    }

    if (listener) {
        listener->toolSelected (current);
    }
}

void ToolBar::persp_pressed ()
{
    if (listener && !perspTool->get_active()) {
        current = TMHand;
        listener->toolDeselected(TMPerspective);
        return;
    }

    // Unlike other modes, mode switching is handled by the perspective panel.
    {
    ConnectionBlocker handBlocker(handConn);
    ConnectionBlocker straBlocker(straConn);
    ConnectionBlocker cropBlocker(cropConn);
    ConnectionBlocker perspBlocker(perspConn);
    ConnectionBlocker perspGridBlocker(perspGridConn);
    ConnectionBlocker wbWasBlocked(wbTool, wbConn), cpWasBlocked(colPickerTool, cpConn);

    if (perspGridTool) {
        perspGridTool->set_active(false);
    }

    if (editingMode) {
        stopEditMode();
        if (listener) {
            listener->editModeSwitchedOff();
        }
    }

    current = TMPerspective;

    }

    if (listener) {
        listener->toolSelected(TMPerspective);
    }
}

void ToolBar::persp_grid_pressed ()
{
    if (listener && !perspGridTool->get_active()) {
        current = TMHand;
        listener->toolDeselected(TMPerspectiveGrid);
        return;
    }

    {
    ConnectionBlocker handBlocker(handConn);
    ConnectionBlocker straBlocker(straConn);
    ConnectionBlocker cropBlocker(cropConn);
    ConnectionBlocker perspBlocker(perspConn);
    ConnectionBlocker perspGridBlocker(perspGridConn);
    ConnectionBlocker wbWasBlocked(wbTool, wbConn), cpWasBlocked(colPickerTool, cpConn);

    if (perspTool) {
        perspTool->set_active(false);
    }

    if (editingMode) {
        stopEditMode();
        if (listener) {
            listener->editModeSwitchedOff();
        }
    }

    current = TMPerspectiveGrid;

    }

    if (listener) {
        listener->toolSelected(TMPerspectiveGrid);
    }
}

bool ToolBar::handleShortcutKey (GdkEventKey* event)
{

    bool ctrl = event->state & GDK_CONTROL_MASK;
    //bool shift = event->state & GDK_SHIFT_MASK;
    bool alt = event->state & GDK_MOD1_MASK;

    if (!ctrl && !alt) {
        switch(event->keyval) {
        case GDK_KEY_w:
        case GDK_KEY_W:
            if(wbTool) {
                wb_pressed ();
                return true;
            }

            return false;

        case GDK_KEY_c:
        case GDK_KEY_C:
            crop_pressed ();
            return true;

        case GDK_KEY_s:
        case GDK_KEY_S:
            stra_pressed ();
            return true;

        // H shortcut removed — hand tool button is hidden
        }
    } else {
        switch (event->keyval) {
        }
    }

    return false;
}

void ToolBar::hideCropTools()
{
    cropTool->set_no_show_all(true);
    cropTool->hide();
    straTool->set_no_show_all(true);
    straTool->hide();
    if (perspTool) {
        perspTool->set_no_show_all(true);
        perspTool->hide();
    }
    if (perspGridTool) {
        perspGridTool->set_no_show_all(true);
        perspGridTool->hide();
    }
}

void ToolBar::hideHandTool()
{
    handTool->set_no_show_all(true);
    handTool->hide();
}

void ToolBar::setBatchMode()
{
    if (wbTool) {
        wbConn.disconnect();
        auto* wbParent = wbTool->get_parent();
        if (wbParent) {
            auto* wbContainer = dynamic_cast<Gtk::Container*>(wbParent);
            if (wbContainer) wbContainer->remove(*wbTool);
        }
        wbTool = nullptr;
    }
    if (colPickerTool) {
        cpConn.disconnect();
        auto* cpParent = colPickerTool->get_parent();
        if (cpParent) {
            auto* cpContainer = dynamic_cast<Gtk::Container*>(cpParent);
            if (cpContainer) cpContainer->remove(*colPickerTool);
        }
        colPickerTool = nullptr;
    }
    if (perspTool) {
        perspConn.disconnect();
        removeIfThere(this, perspTool, false);
        perspTool = nullptr;
    }
    if (perspGridTool) {
        perspGridConn.disconnect();
        removeIfThere(this, perspGridTool, false);
        perspGridTool = nullptr;
    }

    allowNoTool = true;
    switch (current) {
    case TMHand:
        hand_pressed();
        break;
    case TMCropSelect:
        crop_pressed();
        break;
    case TMStraighten:
        stra_pressed();
        break;
    default:
        break;
    }
}

