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
#include "smartmaskbar.h"

#include "multilangmgr.h"
#include "rtimage.h"
#include "steeppopup.h"

namespace
{

// rtengine::AISegClass indices; plain ints so this widget builds without
// the engine header (its callers are RT_AI_MASKING-gated, it is not).
constexpr int CLASS_PERSON = 1;
constexpr int CLASS_SKY = 2;
constexpr int CLASS_VEGETATION = 3;
constexpr int CLASS_BUILDING = 4;
constexpr int CLASS_VEHICLE = 5;
constexpr int CLASS_ANIMAL = 6;
constexpr int CLASS_SUBJECT = 8;      // composed: person|vehicle|animal|foreground, dominant regions
constexpr int CLASS_NOT_SUBJECT = 9;  // composed: everything but the subject

// ControlSpotPanel shape codes (on_mask_shape_selected).
constexpr int SHAPE_ELLIPSE = 0;
constexpr int SHAPE_RECTANGLE = 1;
constexpr int SHAPE_GRADIENT = 2;
constexpr int SHAPE_POLY = 3; // freehand lasso with magnetic edge snapping

}

SmartMaskBar::SmartMaskBar() :
    Gtk::Box(Gtk::ORIENTATION_VERTICAL, 0),
    aiMenu_(new steepui::PopupMenu())
{
    set_name("SmartMaskBar");
    set_margin_start(8);
    set_margin_top(2);
    set_margin_bottom(4);

    // Two equal-width columns of square-cornered tiles, matching the Smart
    // Tools grid above rather than the old pill-shaped chip row.
    Gtk::FlowBox* flow = Gtk::manage(new Gtk::FlowBox());
    flow->set_selection_mode(Gtk::SELECTION_NONE);
    flow->set_homogeneous(true);
    flow->set_row_spacing(4);
    flow->set_column_spacing(4);
    flow->set_min_children_per_line(2);
    flow->set_max_children_per_line(2);
    flow->set_hexpand(true);
    flow->set_halign(Gtk::ALIGN_FILL);

    // All AI classes live under one dropdown chip.
    // Subject is the composed pseudo-class (dominant person/vehicle/animal/
    // foreground regions), and Background is its complement — not the model's
    // raw "background stuff" class, which stays reachable per-spot.
    const struct {
        const char* labelKey;
        int classIndex;
    } aiClasses[] = {
        {"TP_LOCALLAB_SMARTMASK_SUBJECT", CLASS_SUBJECT},
        {"TP_LOCALLAB_AIMASK_CLASS_BACKGROUND", CLASS_NOT_SUBJECT},
        {"TP_LOCALLAB_AIMASK_CLASS_SKY", CLASS_SKY},
        {"TP_LOCALLAB_AIMASK_CLASS_PERSON", CLASS_PERSON},
        {"TP_LOCALLAB_AIMASK_CLASS_VEGETATION", CLASS_VEGETATION},
        {"TP_LOCALLAB_AIMASK_CLASS_BUILDING", CLASS_BUILDING},
        {"TP_LOCALLAB_AIMASK_CLASS_VEHICLE", CLASS_VEHICLE},
        {"TP_LOCALLAB_AIMASK_CLASS_ANIMAL", CLASS_ANIMAL},
    };

    for (const auto& entry : aiClasses) {
        const int classIndex = entry.classIndex;
        const Glib::ustring label = M(entry.labelKey);
        Gtk::MenuItem* item = aiMenu_->addItem(label, [this, classIndex]() {
            classRequested_.emit(classIndex);
        });
        aiMenuEntries_.push_back({item, label, classIndex});
    }

    Gtk::Button* aiBtn = makeChip(flow, "mask-ai",
                                  M("TP_LOCALLAB_MASKTYPE_AI") + " ▾",
                                  M("TP_LOCALLAB_SMARTMASK_AI_TOOLTIP"));
    aiBtn->signal_clicked().connect([this, aiBtn]() {
        refreshAIMenuCoverage();
        aiMenu_->popupAtWidget(*aiBtn);
    });

    // The normal mask shapes stay one press away.
    const struct {
        const char* icon;
        const char* labelKey;
        int shape;
    } shapes[] = {
        {"shape-ellipse", "TP_LOCALLAB_ELI", SHAPE_ELLIPSE},
        {"shape-rectangle", "TP_LOCALLAB_RECT", SHAPE_RECTANGLE},
        {"shape-gradient", "TP_LOCALLAB_GRAD", SHAPE_GRADIENT},
        {"shape-polygon", "TP_LOCALLAB_POLY", SHAPE_POLY},
    };

    for (const auto& entry : shapes) {
        const int shape = entry.shape;
        // The lasso draws its own outline, so it gets its own tooltip
        // explaining the magnetic snapping instead of the generic one.
        Gtk::Button* chip = makeChip(flow, entry.icon, M(entry.labelKey),
                                     M(shape == SHAPE_POLY
                                       ? "TP_LOCALLAB_SMARTMASK_LASSO_TOOLTIP"
                                       : "TP_LOCALLAB_SMARTMASK_SHAPE_TOOLTIP"));
        chip->signal_clicked().connect([this, shape]() {
            shapeRequested_.emit(shape);
        });
    }

    // Click-to-select: arm a one-shot picker on the photo.
    Gtk::Button* pickChip = makeChip(flow, "crosshair-adjust",
                                     M("TP_LOCALLAB_SMARTMASK_PICK"),
                                     M("TP_LOCALLAB_SMARTMASK_PICK_TOOLTIP"));
    pickChip->signal_clicked().connect([this]() {
        pickRequested_.emit();
    });

    pack_start(*flow, Gtk::PACK_SHRINK);
}

SmartMaskBar::~SmartMaskBar() = default;

void SmartMaskBar::refreshAIMenuCoverage()
{
    // Below this share of the frame a class is present in name only.
    constexpr float dimBelow = 0.02f;

    for (auto& entry : aiMenuEntries_) {
        const float coverage = coverageProvider_ ? coverageProvider_(entry.classIndex) : -1.f;

        Glib::ustring label = entry.baseLabel;
        double opacity = 1.0;
        if (coverage >= 0.f) {
            if (coverage < dimBelow) {
                opacity = 0.45;
            } else {
                label += Glib::ustring::compose(" · %1%%",
                                                static_cast<int>(coverage * 100.f + 0.5f));
            }
        }

        entry.item->set_opacity(opacity);
        if (Gtk::Label* lbl = dynamic_cast<Gtk::Label*>(entry.item->get_child())) {
            lbl->set_text(label);
        }
    }
}

Gtk::Button* SmartMaskBar::makeChip(Gtk::FlowBox* flow, const Glib::ustring& icon,
                                    const Glib::ustring& label, const Glib::ustring& tooltip)
{
    Gtk::Button* chip = Gtk::manage(new Gtk::Button());

    Gtk::Box* content = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 4));
    content->pack_start(*Gtk::manage(new RTImage(icon)), Gtk::PACK_SHRINK);
    content->pack_start(*Gtk::manage(new Gtk::Label(label)), Gtk::PACK_SHRINK);
    // Centred inside the homogeneous cell so the two columns read as a grid.
    content->set_halign(Gtk::ALIGN_CENTER);
    chip->add(*content);

    chip->set_hexpand(true);
    chip->set_relief(Gtk::RELIEF_NONE);
    chip->set_can_focus(false);
    chip->set_tooltip_text(tooltip);
    chip->get_style_context()->add_class("smart-mask-chip");

    flow->add(*chip);
    return chip;
}
