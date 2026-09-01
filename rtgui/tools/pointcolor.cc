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
    sectionLabel_ = Gtk::manage(new Gtk::Label());
    sectionLabel_->set_markup("<b>\xe2\x96\xb8 Point Color</b>");
    sectionLabel_->set_xalign(0.0);
    sectionLabel_->get_style_context()->add_class("tool-heading-label");
    auto* labelEvt = Gtk::manage(new Gtk::EventBox());
    labelEvt->add(*sectionLabel_);
    labelEvt->signal_button_press_event().connect([this](GdkEventButton*) -> bool {
        toggleContent();
        return true;
    });
    getSummaryBox()->pack_start(*labelEvt, Gtk::PACK_SHRINK, 2);

    // Collapsible content box
    toolContent_ = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 0));

    // Toolbar: [Eyedropper] [+] [-]
    toolbarBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 4));
    toolbarBox->set_name("PointColorToolbar");
    toolbarBox->set_halign(Gtk::ALIGN_CENTER);

    pickButton = Gtk::manage(new Gtk::Button());
    auto* pickImg = Gtk::manage(new RTImage("color-picker"));
    pickButton->set_image(*pickImg);
    pickButton->set_always_show_image(true);
    pickButton->set_tooltip_text(M("TP_POINTCOLOR_PICK"));
    pickButton->signal_clicked().connect(sigc::mem_fun(*this, &PointColor::onPickTarget));
    toolbarBox->pack_start(*pickButton, Gtk::PACK_SHRINK);

    addButton = Gtk::manage(new Gtk::Button());
    addButton->set_image(*Gtk::manage(new RTImage("add-small")));
    addButton->set_tooltip_text(M("TP_POINTCOLOR_ADD_TARGET"));
    addButton->signal_clicked().connect(sigc::mem_fun(*this, &PointColor::onAddTarget));
    toolbarBox->pack_start(*addButton, Gtk::PACK_SHRINK);

    removeButton = Gtk::manage(new Gtk::Button());
    removeButton->set_image(*Gtk::manage(new RTImage("remove-small")));
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

    targetList->set_no_show_all(true);
    targetList->hide();
    toolContent_->pack_start(*targetList, Gtk::PACK_SHRINK, 0);

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
        sectionLabel_->set_markup("<b>\xe2\x96\xbe Point Color</b>");
        toolContent_->set_no_show_all(false);
        toolContent_->show_all();
        toolContent_->set_no_show_all(true);
    } else {
        sectionLabel_->set_markup("<b>\xe2\x96\xb8 Point Color</b>");
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

    if (targets.empty()) {
        targetList->hide();
        setControlsSensitive(false);
    } else {
        targetList->show();
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
