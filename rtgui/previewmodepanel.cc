/*
 *  This file is part of RawTherapee.
 *
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
#include "previewmodepanel.h"
#include "options.h"
#include "multilangmgr.h"
#include "imagearea.h"

PreviewModePanel::PreviewModePanel (ImageArea* ia) :
    imageArea(ia),
    // Note: RTImage custom class only manages squared icon. To reduce toggle button width,
    // toggle button image is managed by icon name
    nR("square-toggle-red-on-narrow"), ngR("square-toggle-red-off-narrow"),
    nG("square-toggle-green-on-narrow"), ngG("square-toggle-green-off-narrow"),
    nB("square-toggle-blue-on-narrow"), ngB("square-toggle-blue-off-narrow"),
    nL("square-toggle-luminosity-on-narrow"), ngL("square-toggle-luminosity-off-narrow"),
    nBC0("square-toggle-theme-on-narrow"), ngBC0("square-toggle-theme-off-narrow"),
    nBC1("square-toggle-black-on-narrow"), ngBC1("square-toggle-black-off-narrow"),
    nBC2("square-toggle-white-on-narrow"), ngBC2("square-toggle-white-off-narrow"),
    nBC3("square-toggle-gray-on-narrow"), ngBC3("square-toggle-gray-off-narrow")
{
    const auto& options = App::get().options();
    backColor0 = Gtk::manage (new Gtk::ToggleButton ());
    backColor0->get_style_context()->add_class("narrowbutton");
    backColor0->set_relief(Gtk::RELIEF_NONE);
    backColor0->set_tooltip_markup (M("MAIN_TOOLTIP_BACKCOLOR0"));
    backColor0->set_label("T");

    backColor1 = Gtk::manage (new Gtk::ToggleButton ());
    backColor1->get_style_context()->add_class("narrowbutton");
    backColor1->set_relief(Gtk::RELIEF_NONE);
    backColor1->set_tooltip_markup (M("MAIN_TOOLTIP_BACKCOLOR1"));
    backColor1->set_label("B");

    backColor2 = Gtk::manage (new Gtk::ToggleButton ());
    backColor2->get_style_context()->add_class("narrowbutton");
    backColor2->set_relief(Gtk::RELIEF_NONE);
    backColor2->set_tooltip_markup (M("MAIN_TOOLTIP_BACKCOLOR2"));
    backColor2->set_label("W");

    backColor3 = Gtk::manage (new Gtk::ToggleButton ());
    backColor3->get_style_context()->add_class("narrowbutton");
    backColor3->set_relief(Gtk::RELIEF_NONE);
    backColor3->set_tooltip_markup (M("MAIN_TOOLTIP_BACKCOLOR3"));
    backColor3->set_label("G");

    previewR = Gtk::manage (new Gtk::ToggleButton ());
    previewR->get_style_context()->add_class("narrowbutton");
    previewR->set_relief(Gtk::RELIEF_NONE);
    previewR->set_tooltip_markup (M("MAIN_TOOLTIP_PREVIEWR"));
    previewR->set_label("R");

    previewG = Gtk::manage (new Gtk::ToggleButton ());
    previewG->get_style_context()->add_class("narrowbutton");
    previewG->set_relief(Gtk::RELIEF_NONE);
    previewG->set_tooltip_markup (M("MAIN_TOOLTIP_PREVIEWG"));
    previewG->set_label("G");

    previewB = Gtk::manage (new Gtk::ToggleButton ());
    previewB->get_style_context()->add_class("narrowbutton");
    previewB->set_relief(Gtk::RELIEF_NONE);
    previewB->set_tooltip_markup (M("MAIN_TOOLTIP_PREVIEWB"));
    previewB->set_label("B");

    previewL = Gtk::manage (new Gtk::ToggleButton ());
    previewL->get_style_context()->add_class("narrowbutton");
    previewL->set_relief(Gtk::RELIEF_NONE);
    previewL->set_tooltip_markup (M("MAIN_TOOLTIP_PREVIEWL"));
    previewL->set_label("L");

    previewR->set_active (false);
    previewG->set_active (false);
    previewB->set_active (false);
    previewL->set_active (false);

    backColor0->set_active (options.bgcolor == 0);
    backColor1->set_active (options.bgcolor == 1);
    backColor2->set_active (options.bgcolor == 2);
    backColor3->set_active (options.bgcolor == 3);

    // Preview channel buttons (R/G/B/L) moved to Options menu
    // pack_start (*previewR, Gtk::PACK_SHRINK, 0);
    // pack_start (*previewG, Gtk::PACK_SHRINK, 0);
    // pack_start (*previewB, Gtk::PACK_SHRINK, 0);
    // pack_start (*previewL, Gtk::PACK_SHRINK, 0);

    connR = previewR->signal_toggled().connect( sigc::bind(sigc::mem_fun(*this, &PreviewModePanel::buttonToggled), previewR) );
    connG = previewG->signal_toggled().connect( sigc::bind(sigc::mem_fun(*this, &PreviewModePanel::buttonToggled), previewG) );
    connB = previewB->signal_toggled().connect( sigc::bind(sigc::mem_fun(*this, &PreviewModePanel::buttonToggled), previewB) );
    connL = previewL->signal_toggled().connect( sigc::bind(sigc::mem_fun(*this, &PreviewModePanel::buttonToggled), previewL) );

    connbackColor0 = backColor0->signal_toggled().connect( sigc::bind(sigc::mem_fun(*this, &PreviewModePanel::buttonToggled_backColor), backColor0) );
    connbackColor1 = backColor1->signal_toggled().connect( sigc::bind(sigc::mem_fun(*this, &PreviewModePanel::buttonToggled_backColor), backColor1) );
    connbackColor2 = backColor2->signal_toggled().connect( sigc::bind(sigc::mem_fun(*this, &PreviewModePanel::buttonToggled_backColor), backColor2) );
    connbackColor3 = backColor3->signal_toggled().connect( sigc::bind(sigc::mem_fun(*this, &PreviewModePanel::buttonToggled_backColor), backColor3) );

    //show_all ();
}

PreviewModePanel::~PreviewModePanel () {}

//toggle Functions below are for shortcuts
void PreviewModePanel::toggleR ()
{
    previewR->set_active(!previewR->get_active());
}
void PreviewModePanel::toggleG ()
{
    previewG->set_active(!previewG->get_active());
}
void PreviewModePanel::toggleB ()
{
    previewB->set_active(!previewB->get_active());
}
void PreviewModePanel::toggleL ()
{
    previewL->set_active(!previewL->get_active());
}

void PreviewModePanel::togglebackColor0 ()
{
    backColor0->set_active(!backColor0->get_active());
}
void PreviewModePanel::togglebackColor1 ()
{
    backColor1->set_active(!backColor1->get_active());
}
void PreviewModePanel::togglebackColor2 ()
{
    backColor2->set_active(!backColor2->get_active());
}
void PreviewModePanel::togglebackColor3 ()
{
    backColor3->set_active(!backColor3->get_active());
}

void PreviewModePanel::buttonToggled (Gtk::ToggleButton* tbpreview)
{
    connR.block(true);
    connG.block(true);
    connB.block(true);
    connL.block(true);

    // Control state of the others buttons: only 0 or 1 button at a time can remain pressed
    // Note: Only refresh previously toggled button
    if (previewR->get_active() && tbpreview != previewR) {
        previewR->set_active(false);
    }

    if (previewG->get_active() && tbpreview != previewG) {
        previewG->set_active(false);
    }

    if (previewB->get_active() && tbpreview != previewB) {
        previewB->set_active(false);
    }

    if (previewL->get_active() && tbpreview != previewL) {
        previewL->set_active(false);
    }

    connR.block(false);
    connG.block(false);
    connB.block(false);
    connL.block(false);

    imageArea->queue_draw ();

    // this will redraw the linked Before image area
    // which is set when before/after view is enabled
    if (imageArea->iLinkedImageArea != nullptr) {
        imageArea->iLinkedImageArea->queue_draw ();
    }
}

int PreviewModePanel::GetbackColor()
{
    int backColor = 0;

    if (backColor0->get_active ()) {
        backColor = 0;
    }

    if (backColor1->get_active ()) {
        backColor = 1;
    }

    if (backColor2->get_active ()) {
        backColor = 2;
    }

    if (backColor3->get_active ()) {
        backColor = 3;
    }

    return backColor;
}

void PreviewModePanel::togglebackColor()
{
    int backColor = GetbackColor();

    if(backColor == 0) {
        togglebackColor1();
    } else if(backColor == 1) {
        togglebackColor3();
    } else if(backColor == 3) {
        togglebackColor2();
    } else {
        togglebackColor0();
    }
}

void PreviewModePanel::setBackColor(int index)
{
    int current = GetbackColor();
    if (current == index) {
        return;
    }

    switch (index) {
        case 0: togglebackColor0(); break;
        case 1: togglebackColor1(); break;
        case 2: togglebackColor2(); break;
        case 3: togglebackColor3(); break;
    }
}

void PreviewModePanel::buttonToggled_backColor (Gtk::ToggleButton* tbbackColor)
{
    connbackColor0.block(true);
    connbackColor1.block(true);
    connbackColor2.block(true);
    connbackColor3.block(true);

    // Control state of the others buttons: only 1 button at a time shall remain pressed
    // Note: Only refresh previously toggled button
    if (backColor0->get_active() && tbbackColor != backColor0) {
        backColor0->set_active(false);
    }

    if (backColor1->get_active() && tbbackColor != backColor1) {
        backColor1->set_active(false);
    }

    if (backColor2->get_active() && tbbackColor != backColor2) {
        backColor2->set_active(false);
    }

    if (backColor3->get_active() && tbbackColor != backColor3) {
        backColor3->set_active(false);
    }

    // Change image on toggled button
    // Note: Only refresh toggled button if newly active (otherwise keep it active)
    if (tbbackColor == backColor0) {
        if (!backColor0->get_active()) {
            backColor0->set_active(true);
        }
    }

    if (tbbackColor == backColor1) {
        if (!backColor1->get_active()) {
            backColor1->set_active(true);
        }
    }

    if (tbbackColor == backColor2) {
        if (!backColor2->get_active()) {
            backColor2->set_active(true);
        }
    }

    if (tbbackColor == backColor3) {
        if (!backColor3->get_active()) {
            backColor3->set_active(true);
        }
    }

    connbackColor0.block(false);
    connbackColor1.block(false);
    connbackColor2.block(false);
    connbackColor3.block(false);

    //TODO not sure if queue_draw is necessary, but will need to reach to backColor of the Before view
    imageArea->queue_draw ();

    // this will redraw the linked Before image area
    // which is set when before/after view is enabled
    if (imageArea->iLinkedImageArea != nullptr) {
        imageArea->iLinkedImageArea->queue_draw ();
    }
}
