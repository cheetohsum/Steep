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
#include "adjuster.h"

#include <sigc++/slot.h>
#include <cmath>

#include "multilangmgr.h"
#include "options.h"
#include "rtimage.h"
#include "rtscalable.h"
#include "rtengine/rt_math.h"

namespace {

constexpr int MIN_RESET_BUTTON_HEIGHT = 12;

constexpr int LABEL_FIXED_WIDTH = 38;
constexpr int SPIN_FIXED_WIDTH = 18;

double one2one(double val)
{
    return val;
}
}

Adjuster::Adjuster(
    Glib::ustring vlabel,
    double vmin,
    double vmax,
    double vstep,
    double vdefault,
    Gtk::Image *imgIcon1,
    Gtk::Image *imgIcon2,
    double2double_fun slider2value,
    double2double_fun value2slider
) :
    adjustmentName(std::move(vlabel)),
    grid(nullptr),
    label(nullptr),
    imageIcon1(imgIcon1),
    imageIcon2(imgIcon2),
    automatic(nullptr),
    adjusterListener(nullptr),
    spinChange(App::get().options().adjusterMinDelay, App::get().options().adjusterMaxDelay),
    sliderChange(App::get().options().adjusterMinDelay, App::get().options().adjusterMaxDelay),
    editedCheckBox(nullptr),
    afterReset(false),
    blocked(false),
    addMode(false),
    vMin(vmin),
    vMax(vmax),
    vStep(vstep),
    logBase(0),
    logPivot(0),
    logAnchorMiddle(false),
    isBipolar_(false),
    value2slider(value2slider ? value2slider : &one2one),
    slider2value(slider2value ? slider2value : &one2one)

{
    set_hexpand(true);
    set_vexpand(false);

    if (imageIcon1) {
        setExpandAlignProperties(imageIcon1, false, false, Gtk::ALIGN_CENTER, Gtk::ALIGN_CENTER);
    }

    if (imageIcon2) {
        setExpandAlignProperties(imageIcon2, false, false, Gtk::ALIGN_CENTER, Gtk::ALIGN_CENTER);
    }

    set_column_spacing(0);
    set_column_homogeneous(false);
    set_row_spacing(0);
    set_row_homogeneous(false);
    set_name("Adjuster");

    // Label text is rendered inside the slider trough (DxO/LR style)
    // No separate Gtk::Label widget needed — label stays nullptr

    reset = Gtk::manage(new Gtk::Button());

    reset->add(*Gtk::manage(new RTImage("undo-small", Gtk::ICON_SIZE_BUTTON)));
    setExpandAlignProperties(reset, false, false, Gtk::ALIGN_CENTER, Gtk::ALIGN_CENTER);
    reset->set_relief(Gtk::RELIEF_NONE);
    reset->set_tooltip_markup(M("ADJUSTER_RESET_TO_DEFAULT"));
    reset->get_style_context()->add_class(GTK_STYLE_CLASS_FLAT);
    reset->set_can_focus(false);

    spin = Gtk::manage(new MySpinButton());

    setExpandAlignProperties(spin, false, true, Gtk::ALIGN_END, Gtk::ALIGN_FILL);
    spin->set_input_purpose(Gtk::INPUT_PURPOSE_DIGITS);
    spin->set_size_request(SPIN_FIXED_WIDTH, -1);
    spin->set_width_chars(4);      // Compact width — just enough for values like "-5.00"
    spin->set_max_width_chars(5);  // Cap natural width

    // Clean display: show "0" instead of "0.00" when value is exactly zero
    spin->signal_output().connect([this]() -> bool {
        const double val = spin->get_adjustment()->get_value();
        if (val == 0.0) {
            spin->set_text("0");
            return true; // we handled the output
        }
        return false; // use default formatting
    });

    // Unified spinbutton CSS: hide +/- buttons, compact font, blend into pill row
    {
        auto css = Gtk::CssProvider::create();
        try {
            css->load_from_data(
                "spinbutton { background: transparent; border: none; box-shadow: none;"
                " font-size: 9px; padding: 0 4px 0 0; margin: 0; min-height: 0; max-width: 36px;"
                " color: rgba(255,255,255,0.65); }"
                " spinbutton entry { background: transparent; border: none; box-shadow: none;"
                " font-size: 9px; padding: 0; margin: 0; min-height: 0;"
                " color: rgba(255,255,255,0.65); caret-color: rgba(255,255,255,0.65); }"
                " spinbutton button { min-width: 0; min-height: 0; max-width: 0;"
                " padding: 0; margin: 0; border: none; background: none; opacity: 0; }"
                " spinbutton button image { min-width: 0; min-height: 0; }"
            );
            spin->get_style_context()->add_provider(
                css, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 200
            );
        } catch (...) {}
    }

    reset->set_size_request(-1, RTScalable::scalePixelSize(spin->get_height() > MIN_RESET_BUTTON_HEIGHT ? spin->get_height() : MIN_RESET_BUTTON_HEIGHT));
    slider = Gtk::manage(new MyHScale());
    setExpandAlignProperties(slider, true, true, Gtk::ALIGN_FILL, Gtk::ALIGN_FILL);
    slider->set_draw_value(false);
    slider->set_has_origin(false); // Disable GTK's built-in trough highlight

    // Grab focus on first click so default handler processes the click immediately
    slider->signal_button_press_event().connect(
        [this](GdkEventButton*) -> bool {
            slider->grab_focus();
            return false; // propagate to default handler
        }, false); // before default handler

    // Inline CSS for slider — taller trough when label is embedded inside
    {
        auto sliderCss = Gtk::CssProvider::create();
        try {
            if (!adjustmentName.empty()) {
                // Label-inside-pill: dark pill drawn by Cairo, GTK trough invisible
                sliderCss->load_from_data(
                    "scale { padding: 0 0; margin: 0; min-height: 34px; }"
                    " scale trough { min-height: 34px; margin: 0; padding: 0;"
                    "   background-color: transparent; background-image: none; border: none; }"
                    " scale slider { min-height: 0; min-width: 0; padding: 17px; margin: 0;"
                    "   background-color: transparent; background-image: none; border: none;"
                    "   box-shadow: none; border-radius: 50%; }"
                );
                slider->setLabelText(adjustmentName);
            } else {
                // No label: compact trough
                sliderCss->load_from_data(
                    "scale { padding: 0 4px; margin: 0; min-height: 0; }"
                    " scale trough { min-height: 3px; margin: 0; padding: 0;"
                    "   background-color: transparent; background-image: none; border: none; }"
                    " scale trough:hover { background-color: rgba(102,153,204,0.06); }"
                    " scale slider { min-height: 0; min-width: 0; padding: 7px; margin: 0;"
                    "   background-color: rgba(0,0,0,0.01); background-image: none; border: none;"
                    "   box-shadow: none; border-radius: 50%; }"
                    " scale slider:hover { background-color: rgba(102,153,204,0.18); background-image: none; box-shadow: 0 0 6px rgba(102,153,204,0.2); }"
                );
            }
            slider->get_style_context()->add_provider(
                sliderCss, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 200
            );
        } catch (...) {
            if (!adjustmentName.empty()) {
                slider->setLabelText(adjustmentName);
            }
        }
    }

    setLimits(vmin, vmax, vstep, vdefault);

    // Detect bipolar range (e.g. -5 to +5) for center-fill slider rendering
    isBipolar_ = (vmin < 0 && vmax > 0);
    if (isBipolar_) {
        slider->get_style_context()->add_class("bipolar");
        slider->set_has_origin(false);  // Disable default left-fill; we draw custom center-fill
    }

    if (adjustmentName.empty()) {
        // No label, everything goes in a single row
        attach_next_to(*slider, Gtk::POS_LEFT, 1, 1);

        if (imageIcon1) {
            attach_next_to(*imageIcon1, *slider, Gtk::POS_LEFT, 1, 1);
        }

        if (imageIcon2) {
            attach_next_to(*imageIcon2, *slider, Gtk::POS_RIGHT, 1, 1);
            attach_next_to(*spin, *imageIcon2, Gtk::POS_RIGHT, 1, 1);
        } else {
            attach_next_to(*spin, *slider, Gtk::POS_RIGHT, 1, 1);
        }

        attach_next_to(*reset, *spin, Gtk::POS_RIGHT, 1, 1);
    } else {
        // Label-inside-slider: spin overlaid on slider in same cell
        // Slider fills the full width — pill covers the entire row, perfectly centered
        set_size_request(200, -1);
        set_margin_top(1);
        set_margin_bottom(1);
        attach(*slider, 0, 0, 1, 1);   // slider fills full width (hexpand=true)
        attach(*spin, 0, 0, 1, 1);     // spin overlays slider, right-aligned

        // Icons: attach hidden (showIcons() can reveal them later)
        int col = 1;
        if (imageIcon1) {
            imageIcon1->set_visible(false);
            imageIcon1->set_no_show_all(true);
            attach(*imageIcon1, col++, 0, 1, 1);
        }
        if (imageIcon2) {
            imageIcon2->set_visible(false);
            imageIcon2->set_no_show_all(true);
            attach(*imageIcon2, col++, 0, 1, 1);
        }

        // Reset: hidden (double-click on label/slider still works for reset)
        reset->set_visible(false);
        reset->set_no_show_all(true);
        attach(*reset, col++, 0, 1, 1);

        // No sub-grid needed
        grid = nullptr;
    }

    defaultVal = ctorDefaultVal = shapeValue(vdefault);
    editedState = defEditedState = Irrelevant;

    spinChange.connect(
        spin->signal_value_changed(),
        sigc::mem_fun(*this, &Adjuster::spinChanged),
        [this]()
        {
            sliderChange.block(true);
            setSliderValue(addMode ? spin->get_value() : this->value2slider(spin->get_value()));
            sliderChange.block(false);
        }
    );
    sliderChange.connect(
        slider->signal_value_changed(),
        sigc::mem_fun(*this, &Adjuster::sliderChanged),
        [this]()
        {
            spinChange.block();
            const double v = shapeValue(getSliderValue());
            spin->set_value(addMode ? v : this->slider2value(v));
            spinChange.unblock();
        }
    );
    reset->signal_button_release_event().connect_notify( sigc::mem_fun(*this, &Adjuster::resetPressed) );

    // Double-click on slider resets to default (after=true so GTK drag handler runs first)
    // Skip reset when click is in the label area and a labelClickCallback is set
    slider->signal_button_press_event().connect(
        [this](GdkEventButton* event) -> bool {
            if (event->type == GDK_2BUTTON_PRESS && event->button == 1) {
                // Don't reset if clicking in the label area (label click handles that)
                if (labelClickCallback_ && slider->getLabelAreaWidth() > 0
                    && event->x < slider->getLabelAreaWidth()) {
                    return false;
                }
                resetValue(false);
                return true;
            }
            return false;
        }, true);

    // Double-click on label also resets to default (only if separate label widget exists)
    if (label) {
        label->add_events(Gdk::BUTTON_PRESS_MASK);
        label->signal_button_press_event().connect(
            [this](GdkEventButton* event) -> bool {
                if (event->type == GDK_2BUTTON_PRESS && event->button == 1) {
                    resetValue(false);
                    return true;
                }
                return false;
            }, false);
    }

    show_all();
}

Adjuster::~Adjuster ()
{

    sliderChange.block();
    spinChange.block();
    adjusterListener = nullptr;

}

void Adjuster::addAutoButton (const Glib::ustring &tooltip)
{
    if (!automatic) {
        automatic = Gtk::manage(new Gtk::CheckButton());
        //automatic->add (*Gtk::manage (new RTImage ("gears")));
        automatic->set_tooltip_markup(tooltip.length() ? Glib::ustring::compose("<b>%1</b>\n\n%2", M("GENERAL_AUTO"), tooltip) : M("GENERAL_AUTO"));
        setExpandAlignProperties(automatic, false, false, Gtk::ALIGN_CENTER, Gtk::ALIGN_CENTER);
        autoChange = automatic->signal_toggled().connect( sigc::mem_fun(*this, &Adjuster::autoToggled) );

        if (grid) {
            // Hombre, adding the checkbox next to the reset button because adding it next to the spin button (as before)
            // would diminish the available size for the label and would require a much heavier reorganization of the grid !
            grid->attach_next_to(*automatic, *reset, Gtk::POS_RIGHT, 1, 1);
        } else {
            attach_next_to(*automatic, *reset, Gtk::POS_RIGHT, 1, 1);
        }
        automatic->show();
    }
}

void Adjuster::delAutoButton ()
{
    if (automatic) {
        if (grid) {
            removeIfThere(grid, automatic);
        } else {
            removeIfThere(this, automatic);
        }
        delete automatic;
        automatic = nullptr;
    }
}

void Adjuster::throwOnButtonRelease(bool throwOnBRelease)
{

    if (throwOnBRelease) {
        if (!buttonReleaseSlider.connected()) {
            buttonReleaseSlider = slider->signal_button_release_event().connect_notify( sigc::mem_fun(*this, &Adjuster::sliderReleased) );
        }

        if (!buttonReleaseSpin.connected()) {
            buttonReleaseSpin = spin->signal_button_release_event().connect_notify( sigc::mem_fun(*this, &Adjuster::spinReleased) );    // Use the same callback hook
        }
    } else {
        if (buttonReleaseSlider.connected()) {
            buttonReleaseSlider.disconnect();
        }

        if (buttonReleaseSpin.connected()) {
            buttonReleaseSpin.disconnect();
        }
    }
}

void Adjuster::setDefault (double def)
{

    defaultVal = shapeValue(def);
}

void Adjuster::setDefaultEditedState (EditedState eState)
{

    defEditedState = eState;
}

void Adjuster::autoToggled ()
{

    if (adjusterListener && !blocked) {
        adjusterListener->adjusterAutoToggled(this, automatic->get_active());
    }
}

void Adjuster::sliderReleased (GdkEventButton* event)
{

    if ((event != nullptr) && (event->button == 1)) {
        sliderChange.cancel();

        notifyListener();
    }
}

void Adjuster::spinReleased (GdkEventButton* event)
{

    if (event) {
        spinChange.cancel();

        notifyListener();
    }
}

void Adjuster::resetValue (bool toInitial)
{
    if (editedState != Irrelevant) {
        editedState = defEditedState;

        if (editedCheckBox) {
            editedChange.block(true);
            editedCheckBox->set_active(defEditedState == Edited);
            editedChange.block(false);
        }

    }

    afterReset = true;

    if (toInitial) {
        // resetting to the initial editing value, when the image has been loaded
        setSliderValue(addMode ? defaultVal : value2slider(defaultVal));
    } else {
        // resetting to the slider default value
        if (addMode) {
            setSliderValue(0.);
        } else {
            setSliderValue(value2slider(ctorDefaultVal));
        }
    }
}

// Please note that it won't change the "Auto" CheckBox's state, if there
void Adjuster::resetPressed (GdkEventButton* event)
{

    if ((event != nullptr) && (event->state & GDK_CONTROL_MASK) && (event->button == 1)) {
        resetValue(true);
    } else {
        resetValue(false);
    }
}

double Adjuster::shapeValue (double a) const
{
    const double pow10 = std::pow(10.0, digits);
    const double val = std::round(a * pow10) / pow10;
    return val == -0.0 ? 0.0 : val;
}

void Adjuster::setLimits (double vmin, double vmax, double vstep, double vdefault)
{
    sliderChange.block(true);
    spinChange.block(true);

    double pow10 = vstep;
    for (digits = 0; std::fabs(pow10 - floor(pow10)) > 0.000000000001; digits++, pow10 *= 10.0);

    const double shapeVal = shapeValue(vdefault);
    spin->set_digits(digits);
    spin->set_increments(vstep, 2.0 * vstep);
    spin->set_range(vmin, vmax);
    spin->updateSize();
    spin->set_value(shapeVal);

    slider->set_digits(digits);
    slider->set_increments(vstep, 2.0 * vstep);
    slider->set_range(addMode ? vmin : value2slider(vmin), addMode ? vmax : value2slider(vmax));
    setSliderValue(addMode ? shapeVal : value2slider(shapeVal));

    sliderChange.block(false);
    spinChange.block(false);
}

void Adjuster::setAddMode(bool addM)
{
    if (addM != addMode) {
        // Switching the Adjuster to the new mode
        addMode = addM;

        if (addM) {
            // Switching to the relative mode
            double range = -vMin + vMax;

            if (range < 0.) {
                range = -range;
            }

            setLimits(-range, range, vStep, 0);

            // Add mode is always bipolar (centered at 0)
            if (!isBipolar_) {
                isBipolar_ = true;
                slider->get_style_context()->add_class("bipolar");
                slider->set_has_origin(false);
            }
        } else {
            // Switching to the absolute mode
            setLimits(vMin, vMax, vStep, defaultVal);

            // Re-evaluate bipolar status
            bool shouldBeBipolar = (vMin < 0 && vMax > 0);
            if (isBipolar_ != shouldBeBipolar) {
                isBipolar_ = shouldBeBipolar;
                if (isBipolar_) {
                    slider->get_style_context()->add_class("bipolar");
                    slider->set_has_origin(false);
                } else {
                    slider->get_style_context()->remove_class("bipolar");
                    slider->set_has_origin(true);
                }
            }
        }
    }
}

void Adjuster::spinChanged()
{
    if (adjusterListener && !blocked) {
        if (!buttonReleaseSlider.connected() || afterReset) {
            if (automatic) {
                setAutoValue(false);
            }
            adjusterListener->adjusterChanged(this, spin->get_value());
        }
    }

    if (editedState == UnEdited) {
        editedState = Edited;

        if (editedCheckBox) {
            editedChange.block(true);
            editedCheckBox->set_active(true);
            editedChange.block(false);
        }
    }

    afterReset = false;
}

void Adjuster::sliderChanged ()
{
    if (adjusterListener && !blocked) {
        if (!buttonReleaseSlider.connected() || afterReset) {
            if (automatic) {
                setAutoValue(false);
            }
            adjusterListener->adjusterChanged(this, spin->get_value());
        }
    }

    if (!afterReset && editedState == UnEdited) {
        editedState = Edited;

        if (editedCheckBox) {
            editedChange.block(true);
            editedCheckBox->set_active(true);
            editedChange.block(false);
        }
    }

    afterReset = false;
}

void Adjuster::setValue (double a)
{
    spinChange.cancel();
    sliderChange.cancel();
    spinChange.block();
    sliderChange.block(true);
    spin->set_value(shapeValue(a));
    setSliderValue(addMode ? shapeValue(a) : value2slider(shapeValue(a)));
    sliderChange.block(false);
    spinChange.unblock();
    afterReset = false;
}

void Adjuster::setAutoValue (bool a)
{
    if (automatic) {
        const bool oldVal = autoChange.block(true);
        automatic->set_active(a);
        autoChange.block(oldVal);
    }
}

bool Adjuster::notifyListener ()
{
    if (adjusterListener != nullptr && !blocked) {
        if (automatic) {
            setAutoValue(false);
        }
        adjusterListener->adjusterChanged(this, spin->get_value());
    }

    return false;
}

bool Adjuster::notifyListenerAutoToggled ()
{

    if (adjusterListener != nullptr && !blocked) {
        adjusterListener->adjusterAutoToggled(this, automatic->get_active());
    }

    return false;
}

void Adjuster::setEnabled (bool enabled)
{

    const bool autoVal = automatic && !editedCheckBox ? automatic->get_active() : true;
    spin->set_sensitive(enabled && autoVal);
    slider->set_sensitive(enabled && autoVal);

    if (automatic) {
        automatic->set_sensitive(enabled);
    }
}

void Adjuster::setEditedState (EditedState eState)
{

    if (editedState != eState) {
        if (editedCheckBox) {
            editedChange.block(true);
            editedCheckBox->set_active(eState == Edited);
            editedChange.block(false);
        }

        editedState = eState;
    }
}

EditedState Adjuster::getEditedState ()
{

    if (editedState != Irrelevant && editedCheckBox) {
        editedState = editedCheckBox->get_active() ? Edited : UnEdited;
    }

    return editedState;
}

void Adjuster::showEditedCB ()
{
    if (label) {
        removeIfThere(this, label, false);
    }

    if (!editedCheckBox) {
        editedCheckBox = Gtk::manage(new Gtk::CheckButton(adjustmentName));
        editedCheckBox->set_vexpand(false);
        editedCheckBox->set_hexpand(false);
        editedCheckBox->set_halign(Gtk::ALIGN_START);
        editedCheckBox->set_valign(Gtk::ALIGN_CENTER);

        if (!adjustmentName.empty()) {
            // Clear embedded label text — checkbox takes over
            slider->setLabelText("");
            attach_next_to(*editedCheckBox, *slider, Gtk::POS_LEFT, 1, 1);
        } else {
            // No-label layout: insert before first widget
            if (imageIcon1) {
                attach_next_to(*editedCheckBox, *imageIcon1, Gtk::POS_LEFT, 1, 1);
            } else {
                attach_next_to(*editedCheckBox, *slider, Gtk::POS_LEFT, 1, 1);
            }
        }

        editedChange = editedCheckBox->signal_toggled().connect( sigc::mem_fun(*this, &Adjuster::editedToggled) );
        editedCheckBox->show();
    }
}

void Adjuster::editedToggled ()
{
    if (adjusterListener && !blocked) {
        if (automatic) {
            setAutoValue(false);
        }
        adjusterListener->adjusterChanged(this, spin->get_value());
    }
}

void Adjuster::trimValue (double &val) const
{
    val = rtengine::LIM(val, vMin, vMax);
}

void Adjuster::trimValue (int &val) const
{
    val = rtengine::LIM<int>(val, vMin, vMax);
}

void Adjuster::trimValue (float &val) const
{
    val = rtengine::LIM<float>(val, vMin, vMax);
}

double Adjuster::getSliderValue() const
{
    double val = slider->get_value();
    if (logBase) {
        if (logAnchorMiddle) {
            double mid = (vMax - vMin) / 2;
            double mmid = vMin + mid;
            if (val >= mmid) {
                double range = vMax - mmid;
                double x = (val - mmid) / range;
                val = logPivot + (std::pow(logBase, x) - 1.0) / (logBase - 1.0) * (vMax - logPivot);
            } else {
                double range = mmid - vMin;
                double x = (mmid - val) / range;
                val = logPivot - (std::pow(logBase, x) - 1.0) / (logBase - 1.0) * (logPivot - vMin);
            }
        } else {
            if (val >= logPivot) {
                double range = vMax - logPivot;
                double x = (val - logPivot) / range;
                val = logPivot + (std::pow(logBase, x) - 1.0) / (logBase - 1.0) * range;
            } else {
                double range = logPivot - vMin;
                double x = (logPivot - val) / range;
                val = logPivot - (std::pow(logBase, x) - 1.0) / (logBase - 1.0) * range;
            }
        }
    }
    return val;
}

void Adjuster::setSliderValue(double val)
{
    if (logBase) {
        if (logAnchorMiddle) {
            double mid = (vMax - vMin) / 2;
            if (val >= logPivot) {
                double range = vMax - logPivot;
                double x = (val - logPivot) / range;
                val = (vMin + mid) + std::log1p(x * (logBase - 1.0)) / std::log(logBase) * mid;
            } else {
                double range = logPivot - vMin;
                double x = (logPivot - val) / range;
                val = (vMin + mid) - std::log1p(x * (logBase - 1.0)) / std::log(logBase) * mid;
            }
        } else {
            if (val >= logPivot) {
                double range = vMax - logPivot;
                double x = (val - logPivot) / range;
                val = logPivot + std::log1p(x * (logBase - 1.0)) / std::log(logBase) * range;
            } else {
                double range = logPivot - vMin;
                double x = (logPivot - val) / range;
                val = logPivot - std::log1p(x * (logBase - 1.0)) / std::log(logBase) * range;
            }
        }
    }
    slider->set_value(val);
}

void Adjuster::setLogScale(double base, double pivot, bool anchorMiddle)
{
    spinChange.block(true);
    sliderChange.block(true);

    const double cur = getSliderValue();
    logBase = base;
    logPivot = pivot;
    logAnchorMiddle = anchorMiddle;
    setSliderValue(cur);

    sliderChange.block(false);
    spinChange.block(false);
}

bool Adjuster::getAutoValue() const
{
    return automatic ? automatic->get_active() : false;
}

void Adjuster::setAutoInconsistent(bool i)
{
    if (automatic) {
        automatic->set_inconsistent(i);
    }
}

bool Adjuster::getAutoInconsistent() const
{
    return automatic ? automatic->get_inconsistent() : true /* we have to return something */;
}

void Adjuster::setAdjusterListener (AdjusterListener* alistener)
{
    adjusterListener = alistener;
}

double Adjuster::getValue() const
{
    return shapeValue(spin->get_value());
}

int Adjuster::getIntValue() const
{
    return spin->get_value_as_int();
}

Glib::ustring Adjuster::getTextValue() const
{
    if (addMode) {
        return Glib::ustring::compose("<i>%1</i>", spin->get_text());
    } else {
        return spin->get_text();
    }
}

void Adjuster::setLabel(const Glib::ustring &lbl)
{
    if (label) {
        label->set_label(lbl);
    } else {
        slider->setLabelText(lbl);
    }
}

bool Adjuster::block(bool isBlocked)
{
    bool oldValue = blocked;
    blocked = isBlocked;
    return oldValue;
}

bool Adjuster::getAddMode() const
{
    return addMode;
}

void Adjuster::setDelay(unsigned int min_delay_ms, unsigned int max_delay_ms)
{
    spinChange.setDelay(min_delay_ms, max_delay_ms);
    sliderChange.setDelay(min_delay_ms, max_delay_ms);
}

void Adjuster::showIcons(bool yes)
{
    if (imageIcon1) {
        imageIcon1->set_visible(yes);
        imageIcon1->set_no_show_all(!yes);
    }
    if (imageIcon2) {
        imageIcon2->set_visible(yes);
        imageIcon2->set_no_show_all(!yes);
    }
}

void Adjuster::setSliderGradient(const std::vector<GradientMilestone>& milestones)
{
    slider->setTrackGradient(milestones);
}

void Adjuster::clearSliderGradient()
{
    slider->clearTrackGradient();
}

void Adjuster::setLabelClickCallback(std::function<void()> callback)
{
    labelClickCallback_ = std::move(callback);
    slider->setLabelClickCallback(callback);
}

void Adjuster::setInteractionCallback(std::function<void(bool)> callback)
{
    interactionCallback_ = std::move(callback);
    // Connect slider press → callback(true), release → callback(false)
    slider->signal_button_press_event().connect(
        [this](GdkEventButton* event) -> bool {
            if (event->button == 1 && event->type == GDK_BUTTON_PRESS && interactionCallback_) {
                interactionCallback_(true);
            }
            return false;
        }, false);
    slider->signal_button_release_event().connect(
        [this](GdkEventButton* event) -> bool {
            if (event->button == 1 && interactionCallback_) {
                interactionCallback_(false);
            }
            return false;
        }, false);
}

void Adjuster::hideResetButton()
{
    reset->set_visible(false);
    reset->set_no_show_all(true);
}

void Adjuster::setSliderMinWidth(int px)
{
    slider->set_size_request(px, -1);
}

bool Adjuster::on_draw(const Cairo::RefPtr<Cairo::Context>& cr)
{
    if (!adjustmentName.empty() && slider->get_visible()) {
        // Pill covers only the slider area — right edge stops where spin begins
        const Gtk::Allocation sAlloc = slider->get_allocation();
        const int pillX = sAlloc.get_x();
        const int pillW = sAlloc.get_width();
        const int pillY = 0;
        const int pillH = get_allocated_height();
        const double pillR = 4.0;

        if (pillW < 10) {
            return Gtk::Grid::on_draw(cr);
        }

        // Slider position for fill calculations
        const int sliderX = sAlloc.get_x();
        const int sliderW = sAlloc.get_width();

        // --- Pill background ---
        auto pillPath = [&]() {
            cr->begin_new_sub_path();
            cr->arc(pillX + pillR, pillY + pillR, pillR, rtengine::RT_PI, rtengine::RT_PI * 1.5);
            cr->arc(pillX + pillW - pillR, pillY + pillR, pillR, rtengine::RT_PI * 1.5, 0);
            cr->arc(pillX + pillW - pillR, pillY + pillH - pillR, pillR, 0, rtengine::RT_PI * 0.5);
            cr->arc(pillX + pillR, pillY + pillH - pillR, pillR, rtengine::RT_PI * 0.5, rtengine::RT_PI);
            cr->close_path();
        };

        pillPath();
        cr->set_source_rgba(0.0, 0.0, 0.0, 0.3);
        cr->fill();

        // --- Value fill (within slider range only, clipped to pill) ---
        auto adj = slider->get_adjustment();
        const double rangeMin = adj->get_lower();
        const double rangeMax = adj->get_upper();
        const double range = rangeMax - rangeMin;
        const double val = adj->get_value();

        if (range > 0) {
            const double valueFrac = (val - rangeMin) / range;

            int fillLeft, fillRight;
            if (isBipolar_) {
                const double centerFrac = (0.0 - rangeMin) / range;
                const int centerX = sliderX + static_cast<int>(centerFrac * sliderW);
                const int valueX = sliderX + static_cast<int>(valueFrac * sliderW);
                fillLeft = std::min(centerX, valueX);
                fillRight = std::max(centerX, valueX);
            } else {
                fillLeft = sliderX;
                fillRight = sliderX + static_cast<int>(valueFrac * sliderW);
            }

            if (fillRight > fillLeft + 1) {
                cr->save();
                pillPath();
                cr->clip();
                // Vertical gradient fill: brighter at top, darker at bottom
                auto fillGrad = Cairo::LinearGradient::create(0, pillY, 0, pillY + pillH);
                fillGrad->add_color_stop_rgba(0.0, 1.0, 1.0, 1.0, 0.14);
                fillGrad->add_color_stop_rgba(1.0, 1.0, 1.0, 1.0, 0.03);
                cr->set_source(fillGrad);
                cr->rectangle(fillLeft, pillY, fillRight - fillLeft, pillH);
                cr->fill();
                cr->restore();
            }
        }
    }

    // Draw children, then label text on top
    cr->save();
    Gtk::Grid::on_draw(cr);
    cr->restore();

    // --- Label text: drawn AFTER children so it's on top of gradient tracks ---
    if (!adjustmentName.empty() && slider->get_visible()) {
        const int pillY = 0;
        const int pillH = get_allocated_height();
        const Gtk::Allocation sAlloc = slider->get_allocation();

        auto layout = create_pango_layout(adjustmentName);
        auto fontDesc = Pango::FontDescription("sans 9");
        layout->set_font_description(fontDesc);

        const int maxLabelWidth = sAlloc.get_width() / 2;
        layout->set_width(maxLabelWidth * Pango::SCALE);
        layout->set_ellipsize(Pango::ELLIPSIZE_END);

        Pango::Rectangle inkRect, logRect;
        layout->get_pixel_extents(inkRect, logRect);

        const int textX = sAlloc.get_x() + 10;
        const int textY = pillY + (pillH - logRect.get_height()) / 2;

        cr->set_source_rgba(1.0, 1.0, 1.0, 0.85);
        cr->move_to(textX, textY);
        layout->show_in_cairo_context(cr);
    }

    return true;
}
