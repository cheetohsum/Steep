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
    sectionLabel_ = Gtk::manage(new Gtk::Label());
    sectionLabel_->set_markup("<b>\xe2\x96\xb8 Color Mixer</b>");
    sectionLabel_->set_xalign(0.0);
    sectionLabel_->get_style_context()->add_class("tool-section-label");
    auto* labelEvt = Gtk::manage(new Gtk::EventBox());
    labelEvt->add(*sectionLabel_);
    labelEvt->signal_button_press_event().connect([this](GdkEventButton*) -> bool {
        toggleContent();
        return true;
    });
    getSummaryBox()->pack_start(*labelEvt, Gtk::PACK_SHRINK, 2);

    // Collapsible content box
    toolContent_ = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 0));

    // Channel bar — 8 colored dot buttons using DrawingArea circles
    channelBar = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 3));
    for (int i = 0; i < 8; ++i) {
        float cR, cG, cB;
        Color::hsv2rgb01(static_cast<float>(CHANNEL_HUES[i]), 0.75f, 0.65f, cR, cG, cB);
        const double dR = cR, dG = cG, dB = cB;

        channelDots[i] = Gtk::manage(new Gtk::DrawingArea());
        channelDots[i]->set_size_request(16, 16);
        channelDots[i]->signal_draw().connect([this, i, dR, dG, dB](const Cairo::RefPtr<Cairo::Context>& cr) -> bool {
            int w = channelDots[i]->get_allocated_width();
            int h = channelDots[i]->get_allocated_height();
            double r = std::min(w, h) / 2.0 - 1.0;
            double cx = w / 2.0, cy = h / 2.0;

            // Filled color circle
            cr->arc(cx, cy, r, 0, 2 * M_PI);
            cr->set_source_rgb(dR, dG, dB);
            cr->fill_preserve();

            // Border: white if selected, dim otherwise
            if (activeChannel == i) {
                cr->set_source_rgb(1.0, 1.0, 1.0);
                cr->set_line_width(1.5);
            } else {
                cr->set_source_rgba(1.0, 1.0, 1.0, 0.2);
                cr->set_line_width(0.5);
            }
            cr->stroke();
            return true;
        });

        auto* evBox = Gtk::manage(new Gtk::EventBox());
        evBox->add(*channelDots[i]);
        evBox->set_tooltip_text(M(CHANNEL_KEYS[i]));
        evBox->signal_button_press_event().connect([this, i](GdkEventButton*) -> bool {
            onChannelSelected(i);
            // Redraw all dots to update selection ring
            for (int j = 0; j < 8; ++j) {
                channelDots[j]->queue_draw();
            }
            return true;
        });

        channelBar->pack_start(*evBox, Gtk::PACK_EXPAND_WIDGET, 0);
    }
    toolContent_->pack_start(*channelBar, Gtk::PACK_SHRINK, 0);

    // Three H/S/L sliders for the active channel
    hueAdj = Gtk::manage(new Adjuster(M("TP_HSVEQUALIZER_HUE"), -100., 100., 1., 0.));
    satAdj = Gtk::manage(new Adjuster(M("TP_HSVEQUALIZER_SAT"), -100., 100., 1., 0.));
    lumAdj = Gtk::manage(new Adjuster(M("TP_HSVEQUALIZER_VAL"), -100., 100., 1., 0.));
    hueAdj->setAdjusterListener(this);
    satAdj->setAdjusterListener(this);
    lumAdj->setAdjusterListener(this);
    toolContent_->pack_start(*hueAdj, Gtk::PACK_SHRINK, 0);
    toolContent_->pack_start(*satAdj, Gtk::PACK_SHRINK, 0);
    toolContent_->pack_start(*lumAdj, Gtk::PACK_SHRINK, 0);

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
        sectionLabel_->set_markup("<b>\xe2\x96\xbe Color Mixer</b>");
        toolContent_->set_no_show_all(false);
        toolContent_->show_all();
        toolContent_->set_no_show_all(true);
        advancedSection->set_no_show_all(false);
        advancedSection->show_all();
        advancedSection->set_no_show_all(true);
    } else {
        sectionLabel_->set_markup("<b>\xe2\x96\xb8 Color Mixer</b>");
        toolContent_->hide();
        advancedSection->hide();
    }
}

void HSVEqualizer::updateActiveSliderGradients()
{
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
    }

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
