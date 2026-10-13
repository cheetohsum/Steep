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
#include "pointcolor.h"

#include <cmath>

#include "guiutils.h"
#include "rtimage.h"
#include "rtengine/color.h"
#include "rtengine/procparams.h"

using namespace rtengine;
using namespace rtengine::procparams;

const Glib::ustring PointColor::TOOL_NAME = "pointcolor";

PointColor::PointColor() :
    FoldableToolPanel(this, TOOL_NAME, M("TP_POINTCOLOR_LABEL"), false, true),
    activeTarget(-1),
    pickListener(nullptr),
    internalUpdate(false),
    contentExpanded_(false)
{
    // Clickable section label (collapsed by default)
    sectionHeader_ = Gtk::manage(new SectionHeader(M("TP_POINTCOLOR_SECTION")));
    sectionHeader_->set_halign(Gtk::ALIGN_FILL);
    sectionHeader_->set_hexpand(true);
    sectionHeader_->setOnToggled([this](bool) { toggleContent(); });
    getSummaryBox()->pack_start(*sectionHeader_, Gtk::PACK_SHRINK, 2);

    // Collapsible content box
    toolContent_ = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 0));

    // Row: [swatches of the picked colours]  ...  [pick] [+] [-]
    toolbarBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 4));
    toolbarBox->set_name("PointColorToolbar");
    swatchBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    toolbarBox->pack_start(*swatchBox, Gtk::PACK_EXPAND_WIDGET);

    pickButton = Gtk::manage(new Gtk::Button());
    auto* pickImg = Gtk::manage(new RTImage("color-picker"));
    pickButton->set_image(*pickImg);
    pickButton->set_always_show_image(true);
    pickButton->set_relief(Gtk::RELIEF_NONE);
    pickButton->set_tooltip_text(M("TP_POINTCOLOR_PICK"));
    pickButton->signal_clicked().connect(sigc::mem_fun(*this, &PointColor::onPickTarget));
    toolbarBox->pack_start(*pickButton, Gtk::PACK_SHRINK);

    addButton = Gtk::manage(new Gtk::Button());
    addButton->set_image(*Gtk::manage(new RTImage("add-small")));
    addButton->set_relief(Gtk::RELIEF_NONE);
    addButton->set_tooltip_text(M("TP_POINTCOLOR_ADD_TARGET"));
    addButton->signal_clicked().connect(sigc::mem_fun(*this, &PointColor::onAddTarget));
    toolbarBox->pack_start(*addButton, Gtk::PACK_SHRINK);

    removeButton = Gtk::manage(new Gtk::Button());
    removeButton->set_image(*Gtk::manage(new RTImage("remove-small")));
    removeButton->set_relief(Gtk::RELIEF_NONE);
    removeButton->set_tooltip_text(M("TP_POINTCOLOR_REMOVE_TARGET"));
    removeButton->signal_clicked().connect(sigc::mem_fun(*this, &PointColor::onRemoveTarget));
    toolbarBox->pack_start(*removeButton, Gtk::PACK_SHRINK);

    toolbarBox->set_margin_bottom(3);
    toolContent_->pack_start(*toolbarBox, Gtk::PACK_SHRINK, 0);

    // Target list
    targetStore = Gtk::ListStore::create(targetColumns);
    targetList = Gtk::manage(new Gtk::TreeView(targetStore));
    targetList->set_name("PointColorTargetList");
    targetList->set_headers_visible(false);
    targetList->append_column("", targetColumns.label);
    selectionConn_ = targetList->get_selection()->signal_changed().connect(
        sigc::mem_fun(*this, &PointColor::onTargetSelected));

    // Custom cell renderer for color dot
    auto* col = targetList->get_column(0);
    if (col) {
        col->set_cell_data_func(*col->get_first_cell(),
            [this](Gtk::CellRenderer* cell, const Gtk::TreeModel::iterator& iter) {
                double hue = (*iter)[targetColumns.hue];
                Glib::ustring label = (*iter)[targetColumns.label];

                float R, G, B;
                Color::hsv2rgb01(static_cast<float>(hue), 0.75f, 0.85f, R, G, B);

                // Use pango markup for colored bullet
                char markup[256];
                snprintf(markup, sizeof(markup),
                    "<span foreground=\"#%02x%02x%02x\">\xe2\x97\x8f</span> %s",
                    (int)(R * 255), (int)(G * 255), (int)(B * 255),
                    label.c_str());

                auto* textCell = dynamic_cast<Gtk::CellRendererText*>(cell);
                if (textCell) {
                    textCell->property_markup() = Glib::ustring(markup);
                }
            });
    }

    // The list stays as the selection model; the swatches are what shows.
    targetList->set_no_show_all(true);
    targetList->hide();
    toolContent_->pack_start(*targetList, Gtk::PACK_SHRINK, 0);

    hueRing = Gtk::manage(new Gtk::DrawingArea());
    hueRing->set_size_request(-1, 132);
    hueRing->add_events(Gdk::BUTTON_PRESS_MASK | Gdk::BUTTON_RELEASE_MASK | Gdk::POINTER_MOTION_MASK);
    hueRing->set_tooltip_text(M("TP_POINTCOLOR_RING_TOOLTIP"));
    hueRing->signal_draw().connect(sigc::mem_fun(*this, &PointColor::drawHueRing));
    hueRing->signal_button_press_event().connect([this](GdkEventButton* e) {
        ringDragging = e->button == 1 && hueRingPointer(e->x, e->y);
        return ringDragging;
    });
    hueRing->signal_motion_notify_event().connect([this](GdkEventMotion* e) {
        return ringDragging && hueRingPointer(e->x, e->y);
    });
    hueRing->signal_button_release_event().connect([this](GdkEventButton*) {
        ringDragging = false;
        return false;
    });
    toolContent_->pack_start(*hueRing, Gtk::PACK_SHRINK, 4);

    // Center Hue selector (0..360 degrees displayed, stored as 0..1)
    centerHueAdj = Gtk::manage(new Adjuster(M("TP_POINTCOLOR_CENTER_HUE"), 0., 360., 1., 0.));
    centerHueAdj->setAdjusterListener(this);
    {
        std::vector<GradientMilestone> ms;
        float R, G, B;
        for (int i = 0; i <= 6; ++i) {
            float h = i / 6.0f;
            Color::hsv2rgb01(h, 0.75f, 0.65f, R, G, B);
            ms.push_back(GradientMilestone(h, R, G, B));
        }
        centerHueAdj->setSliderGradient(ms);
    }
    toolContent_->pack_start(*centerHueAdj, Gtk::PACK_SHRINK, 0);
    centerHueAdj->set_no_show_all(true);   // the ring sets the hue

    hueShiftAdj = Gtk::manage(new Adjuster(M("TP_POINTCOLOR_HUESHIFT"), -100., 100., 1., 0.));
    hueShiftAdj->setAdjusterListener(this);
    toolContent_->pack_start(*hueShiftAdj, Gtk::PACK_SHRINK, 0);

    saturationAdj = Gtk::manage(new Adjuster(M("TP_POINTCOLOR_SATURATION"), -100., 100., 1., 0.));
    saturationAdj->setAdjusterListener(this);
    toolContent_->pack_start(*saturationAdj, Gtk::PACK_SHRINK, 0);

    luminanceAdj = Gtk::manage(new Adjuster(M("TP_POINTCOLOR_LUMINANCE"), -100., 100., 1., 0.));
    luminanceAdj->setAdjusterListener(this);
    toolContent_->pack_start(*luminanceAdj, Gtk::PACK_SHRINK, 0);

    rangeAdj = Gtk::manage(new Adjuster(M("TP_POINTCOLOR_RANGE"), 1., 100., 1., 50.));
    rangeAdj->setAdjusterListener(this);
    toolContent_->pack_start(*rangeAdj, Gtk::PACK_SHRINK, 0);

    // Start hidden
    toolContent_->set_no_show_all(true);
    toolContent_->hide();
    getSummaryBox()->pack_start(*toolContent_, Gtk::PACK_SHRINK, 0);

    // Start with controls disabled
    setControlsSensitive(false);

    getSummaryBox()->show_all();
}

void PointColor::toggleContent()
{
    contentExpanded_ = !contentExpanded_;
    if (contentExpanded_) {
        sectionHeader_->setExpanded(true);
        toolContent_->set_no_show_all(false);
        toolContent_->show_all();
        toolContent_->set_no_show_all(true);
    } else {
        sectionHeader_->setExpanded(false);
        toolContent_->hide();
    }
}

void PointColor::setControlsSensitive(bool sensitive)
{
    centerHueAdj->set_sensitive(sensitive);
    hueShiftAdj->set_sensitive(sensitive);
    saturationAdj->set_sensitive(sensitive);
    luminanceAdj->set_sensitive(sensitive);
    rangeAdj->set_sensitive(sensitive);
    removeButton->set_sensitive(sensitive);
}

Glib::ustring PointColor::hueToColorName(double hue01)
{
    double deg = hue01 * 360.0;
    if (deg < 15 || deg >= 345) return "Red";
    if (deg < 45) return "Orange";
    if (deg < 75) return "Yellow";
    if (deg < 150) return "Green";
    if (deg < 195) return "Cyan";
    if (deg < 255) return "Blue";
    if (deg < 285) return "Purple";
    if (deg < 345) return "Magenta";
    return "Red";
}

void PointColor::updateColorSwatch()
{
    // colorSwatch removed
}

void PointColor::updateTargetList()
{
    // Block selection signal to prevent re-entrant saves during rebuild
    selectionConn_.block();

    targetStore->clear();

    for (size_t i = 0; i < targets.size(); ++i) {
        auto row = *(targetStore->append());
        row[targetColumns.index] = (int)i;
        int deg = (int)(targets[i].centerHue * 360.0 + 0.5) % 360;

        Glib::ustring label = Glib::ustring::compose(
            "%1 (%2\xc2\xb0)", hueToColorName(targets[i].centerHue), deg);

        // Append edit summary if any adjustments are non-zero
        Glib::ustring edits;
        if (std::abs(targets[i].hueShift) >= 0.5) {
            edits += Glib::ustring::compose("H%1", (int)targets[i].hueShift);
        }
        if (std::abs(targets[i].saturation) >= 0.5) {
            if (!edits.empty()) edits += " ";
            edits += Glib::ustring::compose("S%1", (int)targets[i].saturation);
        }
        if (std::abs(targets[i].luminance) >= 0.5) {
            if (!edits.empty()) edits += " ";
            edits += Glib::ustring::compose("L%1", (int)targets[i].luminance);
        }
        if (!edits.empty()) {
            label += "  " + edits;
        }

        row[targetColumns.label] = label;
        row[targetColumns.hue] = targets[i].centerHue;
    }

    for (Gtk::Widget* child : swatchBox->get_children()) {
        swatchBox->remove(*child);
    }

    for (size_t i = 0; i < targets.size(); ++i) {
        const int idx = static_cast<int>(i);
        auto* swatch = Gtk::manage(new Gtk::DrawingArea());
        swatch->set_size_request(30, 30);
        swatch->signal_draw().connect([this, idx, swatch](const Cairo::RefPtr<Cairo::Context>& cr) {
            if (idx >= static_cast<int>(targets.size())) {
                return true;
            }

            Gdk::RGBA before, after;
            beforeAfter(targets[idx], before, after);
            const double w = swatch->get_allocated_width();
            const double h = swatch->get_allocated_height();
            const double r = std::min(w, h) / 2.0 - 3.0;

            if (idx == activeTarget) {
                const Gdk::RGBA ring = themeColor(*swatch, "steep_accent", Gdk::RGBA("#2d7fd3"));
                cr->set_source_rgb(ring.get_red(), ring.get_green(), ring.get_blue());
                cr->set_line_width(1.5);
                cr->arc(w / 2, h / 2, r + 2.25, 0, 2 * M_PI);
                cr->stroke();
            }

            cr->set_source_rgb(before.get_red(), before.get_green(), before.get_blue());
            cr->arc(w / 2, h / 2, r, 0, 2 * M_PI);
            cr->fill_preserve();
            cr->set_source_rgba(0, 0, 0, 0.25);
            cr->set_line_width(1.0);
            cr->stroke();
            return true;
        });

        auto* box = Gtk::manage(new Gtk::EventBox());
        box->add(*swatch);
        box->set_tooltip_text(hueToColorName(targets[i].centerHue));
        box->signal_button_press_event().connect([this, idx](GdkEventButton*) {
            selectTarget(idx);
            return true;
        });
        box->show_all();
        swatchBox->pack_start(*box, Gtk::PACK_SHRINK);
    }

    hueRing->queue_draw();

    if (targets.empty()) {
        targetList->hide();
        setControlsSensitive(false);
    } else {
        // Select active target
        if (activeTarget >= 0 && activeTarget < (int)targets.size()) {
            auto path = Gtk::TreePath(Glib::ustring::format(activeTarget));
            targetList->get_selection()->select(path);
        }
    }

    selectionConn_.unblock();
}

void PointColor::loadTargetToControls(int idx)
{
    if (idx < 0 || idx >= (int)targets.size()) {
        return;
    }

    internalUpdate = true;
    centerHueAdj->setValue(targets[idx].centerHue * 360.0);
    hueShiftAdj->setValue(targets[idx].hueShift);
    saturationAdj->setValue(targets[idx].saturation);
    luminanceAdj->setValue(targets[idx].luminance);
    rangeAdj->setValue(targets[idx].range);
    internalUpdate = false;

    setControlsSensitive(true);
    updateColorSwatch();
}

void PointColor::saveControlsToTarget(int idx)
{
    if (idx < 0 || idx >= (int)targets.size()) {
        return;
    }

    targets[idx].centerHue = centerHueAdj->getValue() / 360.0;
    targets[idx].hueShift = hueShiftAdj->getValue();
    targets[idx].saturation = saturationAdj->getValue();
    targets[idx].luminance = luminanceAdj->getValue();
    targets[idx].range = rangeAdj->getValue();
}

void PointColor::onAddTarget()
{
    // Save current target first
    if (activeTarget >= 0 && activeTarget < (int)targets.size()) {
        saveControlsToTarget(activeTarget);
    }

    PointColorTarget t;
    t.centerHue = 0;
    t.range = 50;
    targets.push_back(t);
    activeTarget = (int)targets.size() - 1;

    updateTargetList();
    loadTargetToControls(activeTarget);

    autoEnable();
    if (listener && getEnabled()) {
        listener->panelChanged(EvPointColorTargetAdd, M("GENERAL_ADDED"));
    }
}

void PointColor::onRemoveTarget()
{
    if (activeTarget < 0 || activeTarget >= (int)targets.size()) {
        return;
    }

    targets.erase(targets.begin() + activeTarget);
    if (targets.empty()) {
        activeTarget = -1;
        setControlsSensitive(false);
    } else {
        activeTarget = std::min(activeTarget, (int)targets.size() - 1);
    }

    updateTargetList();
    if (activeTarget >= 0) {
        loadTargetToControls(activeTarget);
    }
    updateColorSwatch();

    autoEnable();
    if (listener && getEnabled()) {
        listener->panelChanged(EvPointColorTargetAdd, M("GENERAL_REMOVED"));
    }
}

void PointColor::onPickTarget()
{
    if (pickListener) {
        pickListener->pointColorPickRequested();
    }
}

void PointColor::addTargetFromPick(float h, float s, float v)
{
    // Save current target first
    if (activeTarget >= 0 && activeTarget < (int)targets.size()) {
        saveControlsToTarget(activeTarget);
    }

    PointColorTarget t;
    t.centerHue = h;
    t.range = 50;
    targets.push_back(t);
    activeTarget = (int)targets.size() - 1;

    updateTargetList();
    loadTargetToControls(activeTarget);

    autoEnable();
    if (listener && getEnabled()) {
        listener->panelChanged(EvPointColorTargetAdd, M("GENERAL_ADDED"));
    }
}

void PointColor::onTargetSelected()
{
    auto iter = targetList->get_selection()->get_selected();
    if (!iter) {
        return;
    }

    int newIdx = (*iter)[targetColumns.index];
    if (newIdx == activeTarget) {
        return;
    }

    // Save current before switching
    if (activeTarget >= 0 && activeTarget < (int)targets.size()) {
        saveControlsToTarget(activeTarget);
    }

    activeTarget = newIdx;
    loadTargetToControls(activeTarget);
}

void PointColor::selectTarget(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(targets.size()) || idx == activeTarget) {
        return;
    }

    if (activeTarget >= 0 && activeTarget < static_cast<int>(targets.size())) {
        saveControlsToTarget(activeTarget);
    }

    activeTarget = idx;
    loadTargetToControls(activeTarget);

    for (Gtk::Widget* child : swatchBox->get_children()) {
        child->queue_draw();
        if (auto* box = dynamic_cast<Gtk::EventBox*>(child)) {
            if (box->get_child()) {
                box->get_child()->queue_draw();
            }
        }
    }

    hueRing->queue_draw();
}

// The engine's own arithmetic (improcfun.cc, Point Color) at full weight,
// applied to a representative mid colour of the target hue.
void PointColor::beforeAfter(const PointColorTarget& tgt, Gdk::RGBA& before, Gdk::RGBA& after) const
{
    float r, g, b;
    float h = static_cast<float>(tgt.centerHue);
    float s = 0.55f;
    float v = 0.72f;
    Color::hsv2rgb01(h, s, v, r, g, b);
    before.set_rgba(r, g, b, 1.0);

    h += static_cast<float>(tgt.hueShift) / 200.f;
    h -= std::floor(h);
    const float satParam = static_cast<float>(tgt.saturation) / 100.f;
    s = satParam > 0 ? (1.f - satParam) * s + satParam * (1.f - (1.f - s) * (1.f - s)) : s * (1.f + satParam);
    float valParam = static_cast<float>(tgt.luminance) / 200.f;
    valParam *= 1.f - std::pow(1.f - std::min(s, 1.f), 4.f);
    v = valParam > 0 ? (1.f - valParam) * v + valParam * (1.f - (1.f - v) * (1.f - v)) : v * (1.f + valParam);
    Color::hsv2rgb01(h, std::max(0.f, std::min(1.f, s)), std::max(0.f, std::min(1.f, v)), r, g, b);
    after.set_rgba(r, g, b, 1.0);
}

bool PointColor::drawHueRing(const Cairo::RefPtr<Cairo::Context>& cr)
{
    const double h = hueRing->get_allocated_height();
    const double outer = h / 2.0 - 4.0;
    const double inner = outer * 0.64;
    const double cx = outer + 4.0;
    const double cy = h / 2.0;
    const bool haveTarget = activeTarget >= 0 && activeTarget < static_cast<int>(targets.size());

    // Hue ring, red at the top, clockwise -- dimmed, so the reach stands out.
    for (int i = 0; i < 180; ++i) {
        const double a0 = (i / 180.0) * 2 * M_PI - M_PI / 2;
        const double a1 = ((i + 1.5) / 180.0) * 2 * M_PI - M_PI / 2;
        float r, g, b;
        Color::hsv2rgb01(i / 180.f, 0.6f, 0.75f, r, g, b);
        cr->set_source_rgba(r, g, b, haveTarget ? 0.45 : 0.8);
        cr->begin_new_path();
        cr->arc(cx, cy, outer, a0, a1);
        cr->arc_negative(cx, cy, inner, a1, a0);
        cr->close_path();
        cr->fill();
    }

    if (!haveTarget) {
        return true;
    }

    const PointColorTarget& tgt = targets[activeTarget];
    const double centre = tgt.centerHue * 2 * M_PI - M_PI / 2;
    // The engine weighs hue by a gaussian of sigma = range/100 * 0.15; the
    // wedge covers +-2 sigma, where the weight is down to an eighth.
    const double reach = std::min(M_PI, 2.0 * (tgt.range / 100.0 * 0.15) * 2 * M_PI);

    // The reached hues at full strength.
    for (int i = 0; i <= 60; ++i) {
        const double a0 = centre - reach + (2 * reach) * i / 61.0;
        const double a1 = centre - reach + (2 * reach) * (i + 1.2) / 61.0;
        float r, g, b;
        double hue = (a0 + M_PI / 2) / (2 * M_PI);
        hue -= std::floor(hue);
        Color::hsv2rgb01(static_cast<float>(hue), 0.75f, 0.85f, r, g, b);
        cr->set_source_rgb(r, g, b);
        cr->begin_new_path();
        cr->arc(cx, cy, outer, a0, a1);
        cr->arc_negative(cx, cy, inner, a1, a0);
        cr->close_path();
        cr->fill();
    }

    const Gdk::RGBA accent = themeColor(*hueRing, "steep_accent", Gdk::RGBA("#2d7fd3"));
    cr->set_source_rgb(accent.get_red(), accent.get_green(), accent.get_blue());
    cr->set_line_width(2.0);
    cr->begin_new_path();
    cr->arc(cx, cy, outer, centre - reach, centre + reach);
    cr->arc_negative(cx, cy, inner, centre + reach, centre - reach);
    cr->close_path();
    cr->stroke();

    Gdk::RGBA before, after;
    beforeAfter(tgt, before, after);

    // Handle on the target hue.
    const double hr = (outer + inner) / 2.0;
    cr->arc(cx + hr * std::cos(centre), cy + hr * std::sin(centre), 6.0, 0, 2 * M_PI);
    cr->set_source_rgb(before.get_red(), before.get_green(), before.get_blue());
    cr->fill_preserve();
    cr->set_source_rgb(1, 1, 1);
    cr->set_line_width(2.0);
    cr->stroke();

    // Centre: before above, after below.
    const double core = inner - 6.0;
    cr->set_source_rgb(after.get_red(), after.get_green(), after.get_blue());
    cr->arc(cx, cy, core, 0, 2 * M_PI);
    cr->fill();
    cr->set_source_rgb(before.get_red(), before.get_green(), before.get_blue());
    cr->arc(cx, cy, core, M_PI, 2 * M_PI);
    cr->close_path();
    cr->fill();

    // Before / after, named, to the right of the ring.
    const Gdk::RGBA dim = themeColor(*hueRing, "steep_text_dim", Gdk::RGBA("#8a92a1"));
    const double tx = cx + outer + 18.0;
    const auto label = [&](const Glib::ustring& text, double y, const Gdk::RGBA& swatchColour) {
        auto layout = hueRing->create_pango_layout(text);
        cr->set_source_rgb(dim.get_red(), dim.get_green(), dim.get_blue());
        cr->move_to(tx, y);
        layout->show_in_cairo_context(cr);
        int lw, lh;
        layout->get_pixel_size(lw, lh);
        cr->set_source_rgb(swatchColour.get_red(), swatchColour.get_green(), swatchColour.get_blue());
        cr->rectangle(tx, y + lh + 3, 56, 20);
        cr->fill();
    };
    label(M("TP_POINTCOLOR_BEFORE"), cy - 52, before);
    label(M("TP_POINTCOLOR_AFTER"), cy + 4, after);
    return true;
}

bool PointColor::hueRingPointer(double x, double y)
{
    if (activeTarget < 0 || activeTarget >= static_cast<int>(targets.size())) {
        return false;
    }

    const double h = hueRing->get_allocated_height();
    const double outer = h / 2.0 - 4.0;
    const double cx = outer + 4.0;
    const double cy = h / 2.0;
    const double dx = x - cx;
    const double dy = y - cy;

    if (!ringDragging && std::hypot(dx, dy) > outer + 4.0) {
        return false;
    }

    // Clockwise from the top, as drawn.
    double hue = (std::atan2(dy, dx) + M_PI / 2) / (2 * M_PI);
    hue -= std::floor(hue);
    centerHueAdj->setValue(hue * 360.0);
    adjusterChanged(centerHueAdj, hue * 360.0);
    hueRing->queue_draw();
    return true;
}

void PointColor::read(const ProcParams* pp, const ParamsEdited* pedited)
{
    disableListener();

    if (pedited) {
        set_inconsistent(multiImage && !pedited->pointcolor.enabled);
        if (pedited->pointcolor.targets) {
            centerHueAdj->setEditedState(Edited);
            hueShiftAdj->setEditedState(Edited);
            saturationAdj->setEditedState(Edited);
            luminanceAdj->setEditedState(Edited);
            rangeAdj->setEditedState(Edited);
        } else {
            centerHueAdj->setEditedState(UnEdited);
            hueShiftAdj->setEditedState(UnEdited);
            saturationAdj->setEditedState(UnEdited);
            luminanceAdj->setEditedState(UnEdited);
            rangeAdj->setEditedState(UnEdited);
        }
    }

    setEnabled(pp->pointcolor.enabled);
    targets = pp->pointcolor.targets;
    activeTarget = pp->pointcolor.activeTarget;

    // Clamp activeTarget
    if (activeTarget >= (int)targets.size()) {
        activeTarget = targets.empty() ? -1 : (int)targets.size() - 1;
    }

    updateTargetList();
    if (activeTarget >= 0) {
        loadTargetToControls(activeTarget);
    } else {
        setControlsSensitive(false);
    }
    updateColorSwatch();

    enableListener();
}

void PointColor::write(ProcParams* pp, ParamsEdited* pedited)
{
    // Save current controls to active target
    if (activeTarget >= 0 && activeTarget < (int)targets.size()) {
        saveControlsToTarget(activeTarget);
    }

    pp->pointcolor.enabled = getEnabled();
    pp->pointcolor.targets = targets;
    pp->pointcolor.activeTarget = activeTarget;

    if (pedited) {
        pedited->pointcolor.enabled = !get_inconsistent();
        pedited->pointcolor.targets = true;
    }
}

void PointColor::setDefaults(const ProcParams* defParams, const ParamsEdited* pedited)
{
    centerHueAdj->setDefault(0.);
    hueShiftAdj->setDefault(0.);
    saturationAdj->setDefault(0.);
    luminanceAdj->setDefault(0.);
    rangeAdj->setDefault(50.);

    if (pedited) {
        centerHueAdj->setDefaultEditedState(pedited->pointcolor.targets ? Edited : UnEdited);
        hueShiftAdj->setDefaultEditedState(pedited->pointcolor.targets ? Edited : UnEdited);
        saturationAdj->setDefaultEditedState(pedited->pointcolor.targets ? Edited : UnEdited);
        luminanceAdj->setDefaultEditedState(pedited->pointcolor.targets ? Edited : UnEdited);
        rangeAdj->setDefaultEditedState(pedited->pointcolor.targets ? Edited : UnEdited);
    } else {
        centerHueAdj->setDefaultEditedState(Irrelevant);
        hueShiftAdj->setDefaultEditedState(Irrelevant);
        saturationAdj->setDefaultEditedState(Irrelevant);
        luminanceAdj->setDefaultEditedState(Irrelevant);
        rangeAdj->setDefaultEditedState(Irrelevant);
    }
}

void PointColor::setBatchMode(bool batchMode)
{
    ToolPanel::setBatchMode(batchMode);

    // Hide interactive elements in batch mode
    toolbarBox->hide();
    targetList->hide();

    centerHueAdj->showEditedCB();
    hueShiftAdj->showEditedCB();
    saturationAdj->showEditedCB();
    luminanceAdj->showEditedCB();
    rangeAdj->showEditedCB();
}

void PointColor::trimValues(ProcParams* pp)
{
    for (auto& t : pp->pointcolor.targets) {
        t.centerHue = std::max(0.0, std::min(1.0, t.centerHue));
        t.hueShift = std::max(-100.0, std::min(100.0, t.hueShift));
        t.saturation = std::max(-100.0, std::min(100.0, t.saturation));
        t.luminance = std::max(-100.0, std::min(100.0, t.luminance));
        t.range = std::max(1.0, std::min(100.0, t.range));
    }
}

void PointColor::adjusterChanged(Adjuster* a, double newval)
{
    if (internalUpdate) {
        return;
    }

    if (a == centerHueAdj) {
        if (activeTarget >= 0 && activeTarget < (int)targets.size()) {
            targets[activeTarget].centerHue = newval / 360.0;
            // Update the list row for this target
            updateTargetList();
        }
        updateColorSwatch();
    }

    // Save to active target
    if (activeTarget >= 0 && activeTarget < (int)targets.size()) {
        saveControlsToTarget(activeTarget);
    }

    hueRing->queue_draw();
    autoEnable();
    if (listener && getEnabled()) {
        Glib::ustring val = Glib::ustring::format(newval);
        if (a == centerHueAdj) {
            listener->panelChanged(EvPointColorHue, val);
        } else if (a == hueShiftAdj) {
            listener->panelChanged(EvPointColorHueShift, val);
        } else if (a == saturationAdj) {
            listener->panelChanged(EvPointColorSaturation, val);
        } else if (a == luminanceAdj) {
            listener->panelChanged(EvPointColorLuminance, val);
        } else if (a == rangeAdj) {
            listener->panelChanged(EvPointColorRange, val);
        }
    }
}

void PointColor::enabledChanged()
{
    if (listener) {
        if (get_inconsistent()) {
            listener->panelChanged(EvPointColorEnabled, M("GENERAL_UNCHANGED"));
        } else if (getEnabled()) {
            listener->panelChanged(EvPointColorEnabled, M("GENERAL_ENABLED"));
        } else {
            listener->panelChanged(EvPointColorEnabled, M("GENERAL_DISABLED"));
        }
    }
}
