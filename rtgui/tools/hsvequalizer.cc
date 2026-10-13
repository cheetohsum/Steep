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
 *
 *  2010 Ilya Popov <ilia_popov@rambler.ru>
 */
#include "hsvequalizer.h"

#include "rtengine/rt_math.h"

#include "options.h"
#include "widgets/curves/curveeditor.h"
#include "widgets/curves/curveeditorgroup.h"

#include "rtengine/color.h"
#include "rtengine/procparams.h"

using namespace rtengine;
using namespace rtengine::procparams;

const Glib::ustring HSVEqualizer::TOOL_NAME = "hsvequalizer";

namespace {
// Color channel hue centers (matching Lightroom), normalized 0..1
const double CHANNEL_HUES[8] = {
    0.000,  // Red       (0 deg)
    0.083,  // Orange    (30 deg)
    0.167,  // Yellow    (60 deg)
    0.333,  // Green     (120 deg)
    0.500,  // Aqua      (180 deg)
    0.667,  // Blue      (240 deg)
    0.778,  // Purple    (280 deg)
    0.889   // Magenta   (320 deg)
};

const char* CHANNEL_KEYS[8] = {
    "TP_HSVEQUALIZER_RED",
    "TP_HSVEQUALIZER_ORANGE",
    "TP_HSVEQUALIZER_YELLOW",
    "TP_HSVEQUALIZER_GREEN",
    "TP_HSVEQUALIZER_AQUA",
    "TP_HSVEQUALIZER_BLUE",
    "TP_HSVEQUALIZER_PURPLE",
    "TP_HSVEQUALIZER_MAGENTA"
};
} // namespace

HSVEqualizer::HSVEqualizer () : FoldableToolPanel(this, TOOL_NAME, M("TP_HSVEQUALIZER_LABEL"), false, true),
    activeChannel(0),
    contentExpanded_(false)
{
    hueValues.fill(0.0);
    satValues.fill(0.0);
    lumValues.fill(0.0);

    std::vector<GradientMilestone> bottomMilestones;
    float R, G, B;

    for (int i = 0; i < 7; i++) {
        float x = i / 6.0;
        Color::hsv2rgb01(x, 0.5f, 0.5f, R, G, B);
        bottomMilestones.push_back( GradientMilestone(double(x), double(R), double(G), double(B)) );
    }

    // Clickable section label (collapsed by default)
    sectionHeader_ = Gtk::manage(new SectionHeader(M("TP_HSVEQUALIZER_SECTION")));
    sectionHeader_->set_halign(Gtk::ALIGN_FILL);
    sectionHeader_->set_hexpand(true);
    sectionHeader_->setOnToggled([this](bool) { toggleContent(); });
    getSummaryBox()->pack_start(*sectionHeader_, Gtk::PACK_SHRINK, 2);

    // Collapsible content box
    toolContent_ = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 0));

    // Channel bar — 8 colored dot buttons using DrawingArea circles
    channelBar = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 3));
    for (int i = 0; i < 8; ++i) {
        float cR, cG, cB;
        Color::hsv2rgb01(static_cast<float>(CHANNEL_HUES[i]), 0.75f, 0.65f, cR, cG, cB);
        const double dR = cR, dG = cG, dB = cB;

        channelDots[i] = Gtk::manage(new Gtk::DrawingArea());
        channelDots[i]->set_size_request(30, 36);
        channelDots[i]->signal_draw().connect([this, i, dR, dG, dB](const Cairo::RefPtr<Cairo::Context>& cr) -> bool {
            const int w = channelDots[i]->get_allocated_width();
            const double size = 24.0;
            const double x = (w - size) / 2.0;
            const double y = 3.0;
            const double top = 8.0;
            const double bottom = 3.5;
            const bool hovered = hoveredChannel == i;
            const bool active = activeChannel == i;

            // An arch: rounder top corners than bottom, as every container.
            const auto tile = [&](double grow) {
                const double x0 = x - grow, y0 = y - grow, s = size + 2 * grow;
                const double t0 = top + grow, b0 = bottom + grow;
                cr->begin_new_path();
                cr->arc(x0 + s - t0, y0 + t0, t0, -M_PI / 2, 0);
                cr->arc(x0 + s - b0, y0 + s - b0, b0, 0, M_PI / 2);
                cr->arc(x0 + b0, y0 + s - b0, b0, M_PI / 2, M_PI);
                cr->arc(x0 + t0, y0 + t0, t0, M_PI, 1.5 * M_PI);
                cr->close_path();
            };

            if (active || hovered) {
                const Gdk::RGBA ring = themeColor(*channelDots[i], active ? "steep_accent" : "steep_wash",
                                                  Gdk::RGBA("#2d7fd3"));
                tile(2.5);
                cr->set_source_rgba(ring.get_red(), ring.get_green(), ring.get_blue(), active ? 1.0 : 0.35);
                cr->set_line_width(1.5);
                cr->stroke();
            }

            tile(0);
            cr->set_source_rgb(dR, dG, dB);
            cr->fill_preserve();
            cr->set_source_rgba(0, 0, 0, 0.25);
            cr->set_line_width(1.0);
            cr->stroke();

            // A colour that has been moved carries a small mark beneath it.
            if (hueValues[i] != 0.0 || satValues[i] != 0.0 || lumValues[i] != 0.0) {
                const Gdk::RGBA mark = themeColor(*channelDots[i], "steep_accent", Gdk::RGBA("#2d7fd3"));
                cr->set_source_rgb(mark.get_red(), mark.get_green(), mark.get_blue());
                cr->arc(w / 2.0, y + size + 6.0, 2.0, 0, 2 * M_PI);
                cr->fill();
            }

            return true;
        });

        auto* evBox = Gtk::manage(new Gtk::EventBox());
        evBox->add(*channelDots[i]);
        evBox->set_tooltip_text(M(CHANNEL_KEYS[i]));
        evBox->add_events(Gdk::ENTER_NOTIFY_MASK | Gdk::LEAVE_NOTIFY_MASK);
        evBox->signal_enter_notify_event().connect([this, i](GdkEventCrossing*) -> bool {
            hoveredChannel = i;
            channelDots[i]->queue_draw();
            return false;
        });
        evBox->signal_leave_notify_event().connect([this, i](GdkEventCrossing*) -> bool {
            if (hoveredChannel == i) {
                hoveredChannel = -1;
                channelDots[i]->queue_draw();
            }

            return false;
        });
        evBox->signal_button_press_event().connect([this, i](GdkEventButton*) -> bool {
            onChannelSelected(i);
            setAllMode(false);
            redrawSwatches();
            return true;
        });

        channelBar->pack_start(*evBox, Gtk::PACK_EXPAND_WIDGET, 0);
    }
    // One colour | All colours
    auto* modeRow = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 0));
    oneColourBtn = Gtk::manage(new Gtk::ToggleButton(M("TP_HSVEQUALIZER_ONECOLOUR")));
    allColoursBtn = Gtk::manage(new Gtk::ToggleButton(M("TP_HSVEQUALIZER_ALLCOLOURS")));
    modeRow->pack_start(*oneColourBtn, Gtk::PACK_SHRINK);
    modeRow->pack_start(*allColoursBtn, Gtk::PACK_SHRINK);
    makeSegmented(*modeRow);
    modeRow->set_halign(Gtk::ALIGN_CENTER);
    modeRow->set_margin_bottom(4);
    oneColourBtn->set_active(true);
    oneColourBtn->signal_toggled().connect([this]() {
        if (!modeBlock) {
            setAllMode(!oneColourBtn->get_active());
        }
    });
    allColoursBtn->signal_toggled().connect([this]() {
        if (!modeBlock) {
            setAllMode(allColoursBtn->get_active());
        }
    });
    toolContent_->pack_start(*modeRow, Gtk::PACK_SHRINK, 0);

    toolContent_->pack_start(*channelBar, Gtk::PACK_SHRINK, 0);

    // One colour: its name, a reset, and its three sliders.
    oneColourBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 0));
    auto* nameRow = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 4));
    channelName = Gtk::manage(new Gtk::Label(M(CHANNEL_KEYS[0]), Gtk::ALIGN_START));
    channelName->get_style_context()->add_class("tool-heading-label");
    nameRow->pack_start(*channelName, Gtk::PACK_EXPAND_WIDGET);
    Gtk::Button* channelReset = makeResetButton(M("TP_HSVEQUALIZER_RESETCOLOUR"));
    channelReset->signal_clicked().connect(sigc::mem_fun(*this, &HSVEqualizer::resetActiveChannel));
    nameRow->pack_start(*channelReset, Gtk::PACK_SHRINK);
    oneColourBox->pack_start(*nameRow, Gtk::PACK_SHRINK, 0);

    hueAdj = Gtk::manage(new Adjuster(M("TP_HSVEQUALIZER_HUE"), -100., 100., 1., 0.));
    satAdj = Gtk::manage(new Adjuster(M("TP_HSVEQUALIZER_SAT"), -100., 100., 1., 0.));
    lumAdj = Gtk::manage(new Adjuster(M("TP_HSVEQUALIZER_VAL"), -100., 100., 1., 0.));
    hueAdj->setAdjusterListener(this);
    satAdj->setAdjusterListener(this);
    lumAdj->setAdjusterListener(this);
    oneColourBox->pack_start(*hueAdj, Gtk::PACK_SHRINK, 0);
    oneColourBox->pack_start(*satAdj, Gtk::PACK_SHRINK, 0);
    oneColourBox->pack_start(*lumAdj, Gtk::PACK_SHRINK, 0);
    toolContent_->pack_start(*oneColourBox, Gtk::PACK_SHRINK, 0);

    // All colours: Hue | Saturation | Luminance, then a slider per colour.
    allColoursBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 0));
    auto* propRow = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 0));
    const char* propKeys[3] = {"TP_HSVEQUALIZER_HUE", "TP_HSVEQUALIZER_SAT", "TP_HSVEQUALIZER_VAL"};

    for (int p = 0; p < 3; ++p) {
        propertyBtns[p] = Gtk::manage(new Gtk::ToggleButton(M(propKeys[p])));
        propRow->pack_start(*propertyBtns[p], Gtk::PACK_SHRINK);
        propertyBtns[p]->signal_toggled().connect([this, p]() {
            if (!modeBlock && propertyBtns[p]->get_active()) {
                setAllProperty(p);
            } else if (!modeBlock && p == allProperty) {
                modeBlock = true;
                propertyBtns[p]->set_active(true);   // one is always chosen
                modeBlock = false;
            }
        });
    }

    makeSegmented(*propRow);
    propRow->set_halign(Gtk::ALIGN_START);
    propRow->set_margin_bottom(4);
    modeBlock = true;   // the sliders it would load do not exist yet
    propertyBtns[0]->set_active(true);
    modeBlock = false;
    allColoursBox->pack_start(*propRow, Gtk::PACK_SHRINK, 0);

    for (int i = 0; i < 8; ++i) {
        allAdj[i] = Gtk::manage(new Adjuster(M(CHANNEL_KEYS[i]), -100., 100., 1., 0.));
        allAdj[i]->setAdjusterListener(this);
        allColoursBox->pack_start(*allAdj[i], Gtk::PACK_SHRINK, 0);
    }

    allColoursBox->set_no_show_all(true);
    toolContent_->pack_start(*allColoursBox, Gtk::PACK_SHRINK, 0);

    // Start hidden
    toolContent_->set_no_show_all(true);
    toolContent_->hide();
    getSummaryBox()->pack_start(*toolContent_, Gtk::PACK_SHRINK, 0);
    getSummaryBox()->show_all();

    // Set initial gradients for channel 0
    updateActiveSliderGradients();

    // Curves in AdvancedSection
    advancedSection = Gtk::manage(new AdvancedSection());
    advancedSection->set_no_show_all(true);
    advancedSection->hide();

    curveEditorG = new CurveEditorGroup (App::get().mut_options().lastHsvCurvesDir, M("TP_HSVEQUALIZER_CHANNEL"));
    curveEditorG->setCurveListener (this);

    hshape = static_cast<FlatCurveEditor*>(curveEditorG->addCurve(CT_Flat, M("TP_HSVEQUALIZER_HUE")));
    hshape->setEditID(EUID_HSV_H, BT_SINGLEPLANE_FLOAT);
    hshape->setBottomBarBgGradient(bottomMilestones);
    hshape->setCurveColorProvider(this, 1);

    sshape = static_cast<FlatCurveEditor*>(curveEditorG->addCurve(CT_Flat, M("TP_HSVEQUALIZER_SAT")));
    sshape->setEditID(EUID_HSV_S, BT_SINGLEPLANE_FLOAT);
    sshape->setBottomBarBgGradient(bottomMilestones);
    sshape->setCurveColorProvider(this, 2);

    vshape = static_cast<FlatCurveEditor*>(curveEditorG->addCurve(CT_Flat, M("TP_HSVEQUALIZER_VAL")));
    vshape->setEditID(EUID_HSV_V, BT_SINGLEPLANE_FLOAT);
    vshape->setBottomBarBgGradient(bottomMilestones);
    vshape->setCurveColorProvider(this, 3);

    curveEditorG->curveListComplete();
    advancedSection->getContentBox()->pack_start(*curveEditorG, Gtk::PACK_SHRINK, 4);
    pack_start(*advancedSection, Gtk::PACK_SHRINK, 0);
}

void HSVEqualizer::toggleContent()
{
    contentExpanded_ = !contentExpanded_;
    if (contentExpanded_) {
        sectionHeader_->setExpanded(true);
        toolContent_->set_no_show_all(false);
        toolContent_->show_all();
        toolContent_->set_no_show_all(true);
        advancedSection->set_no_show_all(false);
        advancedSection->show_all();
        advancedSection->set_no_show_all(true);
    } else {
        sectionHeader_->setExpanded(false);
        toolContent_->hide();
        advancedSection->hide();
    }
}

std::vector<GradientMilestone> HSVEqualizer::gradientFor(int property, int channel) const
{
    float R, G, B;
    const float hue = static_cast<float>(CHANNEL_HUES[channel]);
    std::vector<GradientMilestone> ms;

    if (property == 0) {
        float h0 = hue - 0.083f; if (h0 < 0) h0 += 1.0f;
        float h1 = hue + 0.083f; if (h1 > 1) h1 -= 1.0f;
        Color::hsv2rgb01(h0, 0.7f, 0.55f, R, G, B);
        ms.push_back(GradientMilestone(0.0, R, G, B));
        Color::hsv2rgb01(hue, 0.7f, 0.55f, R, G, B);
        ms.push_back(GradientMilestone(0.5, R, G, B));
        Color::hsv2rgb01(h1, 0.7f, 0.55f, R, G, B);
        ms.push_back(GradientMilestone(1.0, R, G, B));
    } else if (property == 1) {
        ms.push_back(GradientMilestone(0.0, 0.5, 0.5, 0.5));
        Color::hsv2rgb01(hue, 1.0f, 0.6f, R, G, B);
        ms.push_back(GradientMilestone(1.0, R, G, B));
    } else {
        Color::hsv2rgb01(hue, 0.6f, 0.2f, R, G, B);
        ms.push_back(GradientMilestone(0.0, R, G, B));
        Color::hsv2rgb01(hue, 0.6f, 0.8f, R, G, B);
        ms.push_back(GradientMilestone(1.0, R, G, B));
    }

    return ms;
}

std::array<double, 8>& HSVEqualizer::propertyValues(int property)
{
    return property == 0 ? hueValues : property == 1 ? satValues : lumValues;
}

void HSVEqualizer::redrawSwatches()
{
    for (auto* dot : channelDots) {
        dot->queue_draw();
    }
}

void HSVEqualizer::loadAllSliders()
{
    const auto& values = propertyValues(allProperty);
    disableListener();

    for (int i = 0; i < 8; ++i) {
        allAdj[i]->setSliderGradient(gradientFor(allProperty, i));
        allAdj[i]->setValue(values[i]);
    }

    enableListener();
}

void HSVEqualizer::setAllMode(bool all)
{
    allMode = all;
    modeBlock = true;
    oneColourBtn->set_active(!all);
    allColoursBtn->set_active(all);
    modeBlock = false;

    // no_show_all keeps the hidden view hidden through the page's show_all.
    if (all) {
        loadAllSliders();
        oneColourBox->hide();
        oneColourBox->set_no_show_all(true);
        allColoursBox->set_no_show_all(false);
        allColoursBox->show_all();
    } else {
        allColoursBox->hide();
        allColoursBox->set_no_show_all(true);
        oneColourBox->set_no_show_all(false);
        oneColourBox->show_all();
    }
}

void HSVEqualizer::setAllProperty(int property)
{
    allProperty = property;
    modeBlock = true;

    for (int p = 0; p < 3; ++p) {
        propertyBtns[p]->set_active(p == property);
    }

    modeBlock = false;
    loadAllSliders();
}

void HSVEqualizer::resetActiveChannel()
{
    hueValues[activeChannel] = satValues[activeChannel] = lumValues[activeChannel] = 0.0;
    disableListener();
    hueAdj->setValue(0.0);
    satAdj->setValue(0.0);
    lumAdj->setValue(0.0);
    enableListener();
    redrawSwatches();

    if (listener && getEnabled()) {
        listener->panelChanged(EvHSVEqSliders, M("GENERAL_RESET"));
    }
}

void HSVEqualizer::updateActiveSliderGradients()
{
    channelName->set_text(M(CHANNEL_KEYS[activeChannel]));

    float R, G, B;
    float hue = static_cast<float>(CHANNEL_HUES[activeChannel]);

    // Hue slider: gradient from hue-30deg to hue+30deg
    {
        std::vector<GradientMilestone> ms;
        float h0 = hue - 0.083f; if (h0 < 0) h0 += 1.0f;
        float h1 = hue + 0.083f; if (h1 > 1) h1 -= 1.0f;

        Color::hsv2rgb01(h0, 0.7f, 0.55f, R, G, B);
        ms.push_back(GradientMilestone(0.0, R, G, B));
        Color::hsv2rgb01(hue, 0.7f, 0.55f, R, G, B);
        ms.push_back(GradientMilestone(0.5, R, G, B));
        Color::hsv2rgb01(h1, 0.7f, 0.55f, R, G, B);
        ms.push_back(GradientMilestone(1.0, R, G, B));

        hueAdj->setSliderGradient(ms);
    }

    // Saturation slider: gray to fully saturated
    {
        std::vector<GradientMilestone> ms;
        ms.push_back(GradientMilestone(0.0, 0.5, 0.5, 0.5));
        Color::hsv2rgb01(hue, 1.0f, 0.6f, R, G, B);
        ms.push_back(GradientMilestone(1.0, R, G, B));

        satAdj->setSliderGradient(ms);
    }

    // Luminance slider: dark to bright
    {
        std::vector<GradientMilestone> ms;
        Color::hsv2rgb01(hue, 0.6f, 0.2f, R, G, B);
        ms.push_back(GradientMilestone(0.0, R, G, B));
        Color::hsv2rgb01(hue, 0.6f, 0.8f, R, G, B);
        ms.push_back(GradientMilestone(1.0, R, G, B));

        lumAdj->setSliderGradient(ms);
    }
}

void HSVEqualizer::onChannelSelected(int channel)
{
    if (channel == activeChannel) {
        return;  // Already selected
    }

    // Save current slider values to the previous channel
    hueValues[activeChannel] = hueAdj->getValue();
    satValues[activeChannel] = satAdj->getValue();
    lumValues[activeChannel] = lumAdj->getValue();

    activeChannel = channel;

    // Load values for the new channel (without triggering events)
    disableListener();
    hueAdj->setValue(hueValues[activeChannel]);
    satAdj->setValue(satValues[activeChannel]);
    lumAdj->setValue(lumValues[activeChannel]);
    enableListener();

    updateActiveSliderGradients();
}

HSVEqualizer::~HSVEqualizer ()
{
    delete curveEditorG;
}

std::vector<double> HSVEqualizer::slidersToFlatCurve(const std::array<double, 8>& shifts) const
{
    // Generate FlatCurve control points from slider values
    // FCT_MinMaxCPoints format: [type, x1, y1, leftTangent, rightTangent, ...]
    std::vector<double> curve;
    curve.push_back(static_cast<double>(FCT_MinMaxCPoints));

    for (int i = 0; i < 8; ++i) {
        double x = CHANNEL_HUES[i];
        double y = 0.5 + shifts[i] / 200.0;  // Map -100..100 to 0..1
        curve.push_back(x);
        curve.push_back(y);
        curve.push_back(0.35);  // left tangent
        curve.push_back(0.35);  // right tangent
    }

    return curve;
}

void HSVEqualizer::read (const ProcParams* pp, const ParamsEdited* pedited)
{
    disableListener ();

    if (pedited) {
        hshape->setUnChanged (!pedited->hsvequalizer.hcurve);
        sshape->setUnChanged (!pedited->hsvequalizer.scurve);
        vshape->setUnChanged (!pedited->hsvequalizer.vcurve);
        set_inconsistent(multiImage && !pedited->hsvequalizer.enabled);
    }

    hshape->setCurve(pp->hsvequalizer.hcurve);
    sshape->setCurve(pp->hsvequalizer.scurve);
    vshape->setCurve(pp->hsvequalizer.vcurve);
    setEnabled(pp->hsvequalizer.enabled);

    // Read slider values into internal arrays
    for (int i = 0; i < 8; ++i) {
        hueValues[i] = pp->hsvequalizer.hueShifts[i];
        satValues[i] = pp->hsvequalizer.satShifts[i];
        lumValues[i] = pp->hsvequalizer.lumShifts[i];
    }

    // Update visible sliders for the active channel
    hueAdj->setValue(hueValues[activeChannel]);
    satAdj->setValue(satValues[activeChannel]);
    lumAdj->setValue(lumValues[activeChannel]);

    enableListener ();

    if (allMode) {
        loadAllSliders();
    }

    redrawSwatches();
}

void HSVEqualizer::setEditProvider (EditDataProvider *provider)
{
    hshape->setEditProvider(provider);
    sshape->setEditProvider(provider);
    vshape->setEditProvider(provider);
}

void HSVEqualizer::autoOpenCurve ()
{
    bool active = hshape->openIfNonlinear();

    if (!active) {
        active = sshape->openIfNonlinear();
    }

    if (!active) {
        vshape->openIfNonlinear();
    }
}

void HSVEqualizer::write (ProcParams* pp, ParamsEdited* pedited)
{
    pp->hsvequalizer.enabled = getEnabled();

    // Always use slider mode now
    pp->hsvequalizer.mode = "Sliders";

    // Sync visible sliders back to arrays
    hueValues[activeChannel] = hueAdj->getValue();
    satValues[activeChannel] = satAdj->getValue();
    lumValues[activeChannel] = lumAdj->getValue();

    // Save all channel values
    for (int i = 0; i < 8; ++i) {
        pp->hsvequalizer.hueShifts[i] = hueValues[i];
        pp->hsvequalizer.satShifts[i] = satValues[i];
        pp->hsvequalizer.lumShifts[i] = lumValues[i];
    }

    // Convert sliders to curve data for engine processing
    pp->hsvequalizer.hcurve = slidersToFlatCurve(pp->hsvequalizer.hueShifts);
    pp->hsvequalizer.scurve = slidersToFlatCurve(pp->hsvequalizer.satShifts);
    pp->hsvequalizer.vcurve = slidersToFlatCurve(pp->hsvequalizer.lumShifts);

    if (pedited) {
        pedited->hsvequalizer.hcurve = !hshape->isUnChanged();
        pedited->hsvequalizer.scurve = !sshape->isUnChanged();
        pedited->hsvequalizer.vcurve = !vshape->isUnChanged();
        pedited->hsvequalizer.enabled = !get_inconsistent();
        pedited->hsvequalizer.mode = true;
        pedited->hsvequalizer.hueShifts = true;
        pedited->hsvequalizer.satShifts = true;
        pedited->hsvequalizer.lumShifts = true;
    }
}

void HSVEqualizer::curveChanged (CurveEditor* ce)
{
    if (listener && getEnabled()) {
        if (ce == hshape) {
            listener->panelChanged (EvHSVEqualizerH, M("HISTORY_CUSTOMCURVE"));
        }

        if (ce == sshape) {
            listener->panelChanged (EvHSVEqualizerS, M("HISTORY_CUSTOMCURVE"));
        }

        if (ce == vshape) {
            listener->panelChanged (EvHSVEqualizerV, M("HISTORY_CUSTOMCURVE"));
        }
    }
}

void HSVEqualizer::adjusterChanged(Adjuster* a, double newval)
{
    // Write slider value back to active channel's storage
    if (a == hueAdj) {
        hueValues[activeChannel] = newval;
    } else if (a == satAdj) {
        satValues[activeChannel] = newval;
    } else if (a == lumAdj) {
        lumValues[activeChannel] = newval;
    } else {
        for (int i = 0; i < 8; ++i) {
            if (a == allAdj[i]) {
                propertyValues(allProperty)[i] = newval;

                // Keep the one-colour sliders in step for the selected colour.
                if (i == activeChannel) {
                    Adjuster* same = allProperty == 0 ? hueAdj : allProperty == 1 ? satAdj : lumAdj;
                    disableListener();
                    same->setValue(newval);
                    enableListener();
                }

                break;
            }
        }
    }

    redrawSwatches();
    autoEnable();
    if (listener && getEnabled()) {
        listener->panelChanged(EvHSVEqSliders, M("GENERAL_CHANGED"));
    }
}

void HSVEqualizer::colorForValue (double valX, double valY, enum ColorCaller::ElemType elemType, int callerId, ColorCaller* caller)
{
    float r, g, b;

    if (elemType == ColorCaller::CCET_VERTICAL_BAR) {
        valY = 0.5;
    }

    if (callerId == 1) {        // Hue = f(Hue)
        float h = float((valY - 0.5) * 2. + valX);

        if (h > 1.0f) {
            h -= 1.0f;
        } else if (h < 0.0f) {
            h += 1.0f;
        }

        Color::hsv2rgb01(h, 0.5f, 0.5f, r, g, b);
        caller->ccRed = double(r);
        caller->ccGreen = double(g);
        caller->ccBlue = double(b);
    } else if (callerId == 2) { // Saturation = f(Hue)
        Color::hsv2rgb01(float(valX), float(valY), 0.5f, r, g, b);
        caller->ccRed = double(r);
        caller->ccGreen = double(g);
        caller->ccBlue = double(b);
    } else if (callerId == 3) { // Value = f(Hue)
        Color::hsv2rgb01(float(valX), 0.5f, float(valY), r, g, b);
        caller->ccRed = double(r);
        caller->ccGreen = double(g);
        caller->ccBlue = double(b);
    }
}

void HSVEqualizer::setBatchMode (bool batchMode)
{
    ToolPanel::setBatchMode (batchMode);
    curveEditorG->setBatchMode (batchMode);
    hueAdj->showEditedCB();
    satAdj->showEditedCB();
    lumAdj->showEditedCB();

    for (auto* a : allAdj) {
        a->showEditedCB();
    }
}

void HSVEqualizer::enabledChanged()
{
    if (listener) {
        if (get_inconsistent()) {
            listener->panelChanged (EvHSVEqEnabled, M("GENERAL_UNCHANGED"));
        } else if (getEnabled()) {
            listener->panelChanged (EvHSVEqEnabled, M("GENERAL_ENABLED"));
        } else {
            listener->panelChanged (EvHSVEqEnabled, M("GENERAL_DISABLED"));
        }
    }
}
