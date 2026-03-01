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
#include "zoompanel.h"
#include "multilangmgr.h"
#include "imagearea.h"
#include "rtimage.h"

namespace {

// Extract just the label portion from a localized string that may contain
// "Label\nShortcut: ..." — returns everything before the first \n.
Glib::ustring labelOnly (const Glib::ustring& s)
{
    auto pos = s.find ('\n');
    return pos == Glib::ustring::npos ? s : s.substr (0, pos);
}

} // namespace

ZoomPanel::ZoomPanel (ImageArea* iarea) : iarea(iarea), sliderUpdateInProgress(false)
{
    set_name ("EditorZoomPanel");

    // Main toolbar: single magnifier button + zoom label
    zoomBtn = Gtk::manage (new Gtk::MenuButton ());
    zoomBtn->set_image (*Gtk::manage (new RTImage ("magnifier-1to1", Gtk::ICON_SIZE_LARGE_TOOLBAR)));
    zoomBtn->set_relief (Gtk::RELIEF_NONE);
    zoomBtn->set_tooltip_markup (M ("ZOOMPANEL_ZOOM100"));
    setExpandAlignProperties (zoomBtn, false, false, Gtk::ALIGN_CENTER, Gtk::ALIGN_FILL);

    zoomLabel = Gtk::manage (new Gtk::Label ());
    setExpandAlignProperties (zoomLabel, false, false, Gtk::ALIGN_CENTER, Gtk::ALIGN_FILL);

    attach_next_to (*zoomBtn, Gtk::POS_RIGHT, 1, 1);
    attach_next_to (*zoomLabel, Gtk::POS_RIGHT, 1, 1);

    // Build popover content
    zoomPopover = Gtk::manage (new Gtk::Popover ());
    zoomPopover->set_name ("ZoomPopover");

    Gtk::Box* popBox = Gtk::manage (new Gtk::Box (Gtk::ORIENTATION_VERTICAL, 2));
    popBox->set_margin_top (6);
    popBox->set_margin_bottom (6);
    popBox->set_margin_start (2);
    popBox->set_margin_end (2);

    // Zoom slider (logarithmic feel: 1% to 1600%)
    // Use a linear scale on the log of zoom: log2(0.01)=-6.64, log2(16)=4
    zoomSlider = Gtk::manage (new Gtk::Scale (Gtk::ORIENTATION_HORIZONTAL));
    zoomSlider->set_range (-6.64, 4.0);  // log2 of zoom range
    zoomSlider->set_value (0.0);          // log2(1.0) = 0 => 100%
    zoomSlider->set_draw_value (false);
    zoomSlider->set_size_request (200, -1);
    zoomSlider->signal_value_changed().connect ([this]() {
        if (sliderUpdateInProgress) {
            return;
        }
        if (this->iarea->mainCropWindow) {
            double zoom = std::pow (2.0, zoomSlider->get_value ());
            this->iarea->mainCropWindow->setZoom (zoom);
        }
    });

    // Zoom In button — label only, shortcut in tooltip
    zoomIn = Gtk::manage (new Gtk::Button ());
    {
        Gtk::Box* hbox = Gtk::manage (new Gtk::Box (Gtk::ORIENTATION_HORIZONTAL, 8));
        hbox->pack_start (*Gtk::manage (new RTImage ("magnifier-plus", Gtk::ICON_SIZE_LARGE_TOOLBAR)), false, false);
        hbox->pack_start (*Gtk::manage (new Gtk::Label (labelOnly (M ("ZOOMPANEL_ZOOMIN")))), false, false);
        zoomIn->add (*hbox);
    }
    zoomIn->set_relief (Gtk::RELIEF_NONE);
    zoomIn->set_tooltip_markup (M ("ZOOMPANEL_ZOOMIN"));
    zoomIn->signal_clicked().connect ([this]() {
        zoomPopover->popdown ();
        zoomInClicked ();
    });

    // Zoom Out button
    zoomOut = Gtk::manage (new Gtk::Button ());
    {
        Gtk::Box* hbox = Gtk::manage (new Gtk::Box (Gtk::ORIENTATION_HORIZONTAL, 8));
        hbox->pack_start (*Gtk::manage (new RTImage ("magnifier-minus", Gtk::ICON_SIZE_LARGE_TOOLBAR)), false, false);
        hbox->pack_start (*Gtk::manage (new Gtk::Label (labelOnly (M ("ZOOMPANEL_ZOOMOUT")))), false, false);
        zoomOut->add (*hbox);
    }
    zoomOut->set_relief (Gtk::RELIEF_NONE);
    zoomOut->set_tooltip_markup (M ("ZOOMPANEL_ZOOMOUT"));
    zoomOut->signal_clicked().connect ([this]() {
        zoomPopover->popdown ();
        zoomOutClicked ();
    });

    // Fit Screen button
    zoomFit = Gtk::manage (new Gtk::Button ());
    {
        Gtk::Box* hbox = Gtk::manage (new Gtk::Box (Gtk::ORIENTATION_HORIZONTAL, 8));
        hbox->pack_start (*Gtk::manage (new RTImage ("magnifier-fit", Gtk::ICON_SIZE_LARGE_TOOLBAR)), false, false);
        hbox->pack_start (*Gtk::manage (new Gtk::Label (labelOnly (M ("ZOOMPANEL_ZOOMFITSCREEN")))), false, false);
        zoomFit->add (*hbox);
    }
    zoomFit->set_relief (Gtk::RELIEF_NONE);
    zoomFit->set_tooltip_markup (M ("ZOOMPANEL_ZOOMFITSCREEN"));
    zoomFit->signal_clicked().connect ([this]() {
        zoomPopover->popdown ();
        zoomFitClicked ();
    });

    // Fit Crop button
    zoomFitCrop = Gtk::manage (new Gtk::Button ());
    {
        Gtk::Box* hbox = Gtk::manage (new Gtk::Box (Gtk::ORIENTATION_HORIZONTAL, 8));
        hbox->pack_start (*Gtk::manage (new RTImage ("magnifier-crop", Gtk::ICON_SIZE_LARGE_TOOLBAR)), false, false);
        hbox->pack_start (*Gtk::manage (new Gtk::Label (labelOnly (M ("ZOOMPANEL_ZOOMFITCROPSCREEN")))), false, false);
        zoomFitCrop->add (*hbox);
    }
    zoomFitCrop->set_relief (Gtk::RELIEF_NONE);
    zoomFitCrop->set_tooltip_markup (M ("ZOOMPANEL_ZOOMFITCROPSCREEN"));
    zoomFitCrop->signal_clicked().connect ([this]() {
        zoomPopover->popdown ();
        zoomFitCropClicked ();
    });

    // 100% button
    zoom11 = Gtk::manage (new Gtk::Button ());
    {
        Gtk::Box* hbox = Gtk::manage (new Gtk::Box (Gtk::ORIENTATION_HORIZONTAL, 8));
        hbox->pack_start (*Gtk::manage (new RTImage ("magnifier-1to1", Gtk::ICON_SIZE_LARGE_TOOLBAR)), false, false);
        hbox->pack_start (*Gtk::manage (new Gtk::Label (labelOnly (M ("ZOOMPANEL_ZOOM100")))), false, false);
        zoom11->add (*hbox);
    }
    zoom11->set_relief (Gtk::RELIEF_NONE);
    zoom11->set_tooltip_markup (M ("ZOOMPANEL_ZOOM100"));
    zoom11->signal_clicked().connect ([this]() {
        zoomPopover->popdown ();
        zoom11Clicked ();
    });

    // Separators
    Gtk::Separator* sep1 = Gtk::manage (new Gtk::Separator (Gtk::ORIENTATION_HORIZONTAL));
    Gtk::Separator* sep2 = Gtk::manage (new Gtk::Separator (Gtk::ORIENTATION_HORIZONTAL));

    // Detail Window button
    newCrop = Gtk::manage (new Gtk::Button ());
    {
        Gtk::Box* hbox = Gtk::manage (new Gtk::Box (Gtk::ORIENTATION_HORIZONTAL, 8));
        hbox->pack_start (*Gtk::manage (new RTImage ("window-add", Gtk::ICON_SIZE_LARGE_TOOLBAR)), false, false);
        hbox->pack_start (*Gtk::manage (new Gtk::Label (M ("ZOOMPANEL_NEWCROPWINDOW"))), false, false);
        newCrop->add (*hbox);
    }
    newCrop->set_relief (Gtk::RELIEF_NONE);
    newCrop->signal_clicked().connect ([this]() {
        zoomPopover->popdown ();
        newCropClicked ();
    });

    // Pack into popover
    popBox->pack_start (*zoomSlider, false, false, 2);
    popBox->pack_start (*sep1, false, false, 4);
    popBox->pack_start (*zoomIn, false, false);
    popBox->pack_start (*zoomOut, false, false);
    popBox->pack_start (*zoomFit, false, false);
    popBox->pack_start (*zoomFitCrop, false, false);
    popBox->pack_start (*zoom11, false, false);
    popBox->pack_start (*sep2, false, false, 4);
    popBox->pack_start (*newCrop, false, false);

    popBox->show_all ();
    zoomPopover->add (*popBox);
    zoomBtn->set_popover (*zoomPopover);

    show_all_children ();

    zoomLabel->set_text (M("ZOOMPANEL_100"));
}

void ZoomPanel::zoomInClicked ()
{

    if (iarea->mainCropWindow) {
        iarea->mainCropWindow->zoomIn ();
    }
}

void ZoomPanel::zoomOutClicked ()
{

    if (iarea->mainCropWindow) {
        iarea->mainCropWindow->zoomOut ();
    }
}

void ZoomPanel::zoomFitClicked ()
{

    if (iarea->mainCropWindow) {
        iarea->mainCropWindow->zoomFit ();
    }
}

void ZoomPanel::zoomFitCropClicked ()
{

    if (iarea->mainCropWindow) {
        iarea->mainCropWindow->zoomFitCrop ();
    }
}

void ZoomPanel::zoom11Clicked ()
{

    if (iarea->mainCropWindow) {
        iarea->mainCropWindow->zoom11 ();
    }
}

void ZoomPanel::refreshZoomLabel ()
{

    if (iarea->mainCropWindow) {
        int z = (int)(iarea->mainCropWindow->getZoom () * 100);

        if (z < 100) {
            zoomLabel->set_text (Glib::ustring::compose(" %1%%", z));
        } else {
            zoomLabel->set_text (Glib::ustring::compose("%1%%", z));
        }

        // Sync slider position
        double zoom = iarea->mainCropWindow->getZoom ();
        if (zoom > 0) {
            sliderUpdateInProgress = true;
            zoomSlider->set_value (std::log2 (zoom));
            sliderUpdateInProgress = false;
        }
    }
}

void ZoomPanel::newCropClicked ()
{

    iarea->addCropWindow ();
}
