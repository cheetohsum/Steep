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

#include <cmath>

#include "rtengine/rt_math.h"

namespace {

// Extract just the label portion from a localized string that may contain
// "Label\nShortcut: ..." — returns everything before the first \n.
Glib::ustring labelOnly (const Glib::ustring& s)
{
    auto pos = s.find ('\n');
    return pos == Glib::ustring::npos ? s : s.substr (0, pos);
}

} // namespace

ZoomPanel::ZoomPanel (ImageArea* iarea) : iarea(iarea), sliderUpdateInProgress(false), currentZoomText("100")
{
    set_name ("EditorZoomPanel");

    // Main button: Cairo-drawn magnifying glass with zoom % inside lens
    zoomBtn = Gtk::manage (new Gtk::MenuButton ());
    zoomBtn->set_relief (Gtk::RELIEF_NONE);
    zoomBtn->set_tooltip_markup (M ("ZOOMPANEL_ZOOMMENU"));
    setExpandAlignProperties (zoomBtn, false, false, Gtk::ALIGN_CENTER, Gtk::ALIGN_FILL);

    zoomDraw = Gtk::manage (new Gtk::DrawingArea ());
    zoomDraw->set_size_request (32, 30);
    zoomDraw->signal_draw().connect (sigc::mem_fun (*this, &ZoomPanel::onDrawZoom));
    zoomBtn->add (*zoomDraw);

    attach_next_to (*zoomBtn, Gtk::POS_RIGHT, 1, 1);

    // Build popover content
    zoomPopover = Gtk::manage (new Gtk::Popover ());
    zoomPopover->set_name ("ZoomPopover");
    zoomPopover->set_constrain_to (Gtk::POPOVER_CONSTRAINT_WINDOW);
    // Styling lives in themes/common/widgets.css (#ZoomPopover)

    Gtk::Box* popBox = Gtk::manage (new Gtk::Box (Gtk::ORIENTATION_VERTICAL, 2));
    popBox->set_margin_top (10);
    popBox->set_margin_bottom (10);
    popBox->set_margin_start (10);
    popBox->set_margin_end (10);

    // Zoom slider (logarithmic feel: 1% to 1600%)
    zoomSlider = Gtk::manage (new Gtk::Scale (Gtk::ORIENTATION_HORIZONTAL));
    zoomSlider->set_range (-6.64, 4.0);
    zoomSlider->set_value (0.0);
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

    // Zoom In button
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
    popBox->pack_start (*sep2, false, false, 4);
    popBox->pack_start (*newCrop, false, false);

    popBox->show_all ();
    zoomPopover->add (*popBox);
    zoomBtn->set_popover (*zoomPopover);

    show_all_children ();
}

bool ZoomPanel::onDrawZoom (const Cairo::RefPtr<Cairo::Context>& cr)
{
    const int w = zoomDraw->get_allocated_width ();
    const int h = zoomDraw->get_allocated_height ();

    // The lens is drawn as large as the widget allows and the handle is kept
    // to a short stub in the bottom-right corner: the roomier the glass, the
    // better three- and four-digit zoom values sit inside it.
    const double r = std::min (w * 0.40, h * 0.40);
    const double cx = r + 1.8;
    const double cy = r + 1.4;

    // Soft accent halo lifts the lens off the toolbar
    cr->set_source_rgba (0.392, 0.627, 1.0, 0.12);
    cr->set_line_width (4.0);
    cr->arc (cx, cy, r + 1.2, 0, 2 * M_PI);
    cr->stroke ();

    // Glass fill: bright toward the top-left, falls off to a faint tint
    auto glass = Cairo::RadialGradient::create (cx - r * 0.35, cy - r * 0.35, r * 0.15, cx, cy, r);
    glass->add_color_stop_rgba (0.0, 0.75, 0.84, 1.0, 0.20);
    glass->add_color_stop_rgba (1.0, 0.40, 0.56, 0.85, 0.06);
    cr->set_source (glass);
    cr->arc (cx, cy, r - 0.6, 0, 2 * M_PI);
    cr->fill ();

    // Handle — drawn before the ring so the ring caps it off cleanly
    const double diag = std::cos (M_PI / 4.0);
    const double hx1 = cx + (r + 0.5) * diag;
    const double hy1 = cy + (r + 0.5) * diag;
    const double hx2 = std::min (cx + (r + 7.5) * diag, w - 2.4);
    const double hy2 = std::min (cy + (r + 7.5) * diag, h - 2.4);
    cr->set_line_cap (Cairo::LINE_CAP_ROUND);
    cr->set_source_rgba (0.0, 0.0, 0.0, 0.30);
    cr->set_line_width (4.2);
    cr->move_to (hx1 + 0.8, hy1 + 1.0);
    cr->line_to (hx2 + 0.8, hy2 + 1.0);
    cr->stroke ();
    cr->set_source_rgba (0.82, 0.87, 0.94, 0.95);
    cr->set_line_width (3.4);
    cr->move_to (hx1, hy1);
    cr->line_to (hx2, hy2);
    cr->stroke ();

    // Lens ring
    cr->set_source_rgba (0.82, 0.87, 0.94, 0.92);
    cr->set_line_width (2.0);
    cr->arc (cx, cy, r, 0, 2 * M_PI);
    cr->stroke ();

    // Specular highlight arc at the top-left of the glass
    cr->set_source_rgba (1.0, 1.0, 1.0, 0.35);
    cr->set_line_width (1.4);
    cr->arc (cx, cy, r - 2.4, M_PI * 1.02, M_PI * 1.44);
    cr->stroke ();

    // Zoom value centered in the lens. The font shrinks until the digits fit
    // the chord available inside the ring, so "8" and "1600" both stay in.
    auto layout = zoomDraw->create_pango_layout (currentZoomText);
    auto font = Pango::FontDescription ("Sans Bold");
    const double maxTextWidth = 2.0 * (r - 2.2);
    const double maxTextHeight = 2.0 * (r - 1.4);
    int tw = 0, th = 0;
    double fontSize = 11.0;

    while (true) {
        font.set_absolute_size (fontSize * Pango::SCALE);
        layout->set_font_description (font);
        layout->get_pixel_size (tw, th);

        if (fontSize <= 6.0 || (tw <= maxTextWidth && th <= maxTextHeight)) {
            break;
        }

        fontSize -= 0.5;
    }

    const double tx = std::round (cx - tw / 2.0);
    const double ty = std::round (cy - th / 2.0);

    cr->set_source_rgba (0.0, 0.0, 0.0, 0.55);
    cr->move_to (tx + 1.2, ty + 1.4);
    layout->show_in_cairo_context (cr);
    cr->set_source_rgba (0.0, 0.0, 0.0, 0.30);
    cr->move_to (tx + 0.6, ty + 0.8);
    layout->show_in_cairo_context (cr);

    cr->set_source_rgba (0.97, 0.98, 1.0, 0.98);
    cr->move_to (tx, ty);
    layout->show_in_cairo_context (cr);

    return true;
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
        currentZoomText = Glib::ustring::compose ("%1", z);
        zoomDraw->queue_draw ();
        zoomBtn->queue_draw ();

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
