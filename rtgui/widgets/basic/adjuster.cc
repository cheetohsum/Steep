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

#include "rtengine/edittrace.h"

#include <sigc++/slot.h>
#include <algorithm>
#include <cmath>
#include <string>

#include <gdk/gdkkeysyms.h>

#include "guiutils.h"
#include "multilangmgr.h"
#include "options.h"
#include "rtimage.h"
#include "rtscalable.h"
#include "rtengine/rt_math.h"

namespace {

constexpr int MIN_RESET_BUTTON_HEIGHT = 12;

constexpr int LABEL_FIXED_WIDTH = 38;
constexpr int SPIN_FIXED_WIDTH = 18;

// Type inside the setting pills. Both the name and the value use it, so they
// stay matched; kept in one place because the pill is drawn by hand.
constexpr int ADJUSTER_PILL_FONT_PT = 10;
// Minimum pill height at scale 1. set_size_request is a floor, so the theme
// can still ask for more; scaling multiplies this so the text stays enclosed.
constexpr int ADJUSTER_PILL_BASE_HEIGHT = 20;

double one2one(double val)
{
    return val;
}
}

double Adjuster::pillScale_ = 1.0;
std::vector<Adjuster*> Adjuster::instances_;

namespace {
Glib::ustring pillFontName()
{
    const int pt = std::max(6, static_cast<int>(std::lround(ADJUSTER_PILL_FONT_PT * Adjuster::getPillScale())));
    return Glib::ustring::compose("sans %1", pt);
}
}

double Adjuster::getPillScale()
{
    static bool loaded = false;

    if (!loaded) {
        // Options are read before any Adjuster exists, so pick the saved
        // scale up on first use rather than needing an init call.
        loaded = true;
        pillScale_ = rtengine::LIM(App::get().options().adjusterPillScale, 0.7, 2.2);
    }

    return pillScale_;
}

void Adjuster::setPillScale(double scale)
{
    const double clamped = rtengine::LIM(scale, 0.7, 2.2);

    if (std::fabs(clamped - pillScale_) < 0.001) {
        return;
    }

    pillScale_ = clamped;
    App::get().mut_options().adjusterPillScale = clamped;

    for (Adjuster* a : instances_) {
        a->applyPillScale();
    }
}

void Adjuster::applyPillScale()
{
    if (!adjustmentName.empty() && slider) {
        // The pill is the slider, so its height carries the type.
        slider->set_size_request(-1, static_cast<int>(std::lround(ADJUSTER_PILL_BASE_HEIGHT * getPillScale())));
    }

    queue_resize();
    queue_draw();
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

    // Programmatically disable +/- buttons (some platforms ignore CSS hiding)
    spin->set_numeric(true);
    spin->set_update_policy(Gtk::UPDATE_IF_VALID);

    // Note: GtkSpinButton is NOT a GtkContainer on Windows, so
    // gtk_container_forall cannot be used to hide +/- buttons.
    // For named adjusters, spin is hidden entirely and value drawn via Cairo.
    // For unnamed adjusters, the platform default spin button is kept visible.

    reset->set_size_request(-1, RTScalable::scalePixelSize(spin->get_height() > MIN_RESET_BUTTON_HEIGHT ? spin->get_height() : MIN_RESET_BUTTON_HEIGHT));
    slider = Gtk::manage(new MyHScale());
    setExpandAlignProperties(slider, true, true, Gtk::ALIGN_FILL, Gtk::ALIGN_FILL);
    slider->set_draw_value(false);
    slider->set_has_origin(false); // Disable GTK's built-in trough highlight

    // Grab focus on first click so default handler processes the click immediately
    slider->signal_button_press_event().connect(
        [this](GdkEventButton* event) -> bool {
            // Right-click is a reset, the same one double-click performs.
            // Taken before the default handler so GtkScale never sees the
            // press and cannot seek the slider on the way past.
            if (event->button == 3 && event->type == GDK_BUTTON_PRESS) {
                resetValue(false);
                return true;
            }

            // A click on the number itself opens a type-in editor rather than
            // seeking the slider, so an exact value can be entered.
            if (event->type == GDK_BUTTON_PRESS && event->button == 1
                    && valueTextRect_.get_width() > 0) {
                const double adjusterX = event->x + slider->get_allocation().get_x();

                if (adjusterX >= valueTextRect_.get_x()
                        && adjusterX <= valueTextRect_.get_x() + valueTextRect_.get_width()) {
                    beginValueEdit();
                    return true;   // consume: no seek, no drag
                }
            }

            slider->grab_focus();
            return false; // propagate to default handler
        }, false); // before default handler

    // Label-inside-pill drawing is handled by MyHScale::on_draw. Avoid
    // installing per-adjuster CSS providers here; startup creates many
    // Adjusters and repeated Gtk CSS parsing was crashing during construction
    // on Windows.
    if (!adjustmentName.empty()) {
        slider->setLabelText(adjustmentName);
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
        // Label-inside-slider: value drawn by Cairo in on_draw, spin hidden
        // Slider fills the full width — pill covers the entire row, perfectly centered
        set_size_request(-1, -1);
        // Breathing room between stacked pills so they read as separate rows.
        set_margin_top(3);
        set_margin_bottom(3);
        slider->set_size_request(-1, static_cast<int>(std::lround(ADJUSTER_PILL_BASE_HEIGHT * getPillScale())));
        attach(*slider, 0, 0, 1, 1);   // slider fills full width (hexpand=true)
        spin->set_no_show_all(true);
        spin->set_visible(false);       // hidden — value rendered by on_draw

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

    // Moving an adjuster reprocesses the image, so its debounce is paced to
    // what the pipeline can currently deliver (see delayed.h).
    spinChange.setEnginePaced(true);
    sliderChange.setEnginePaced(true);

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
            if (rtengine::edittrace::verbose()) {
                rtengine::edittrace::logf("sliderRawEvent");
            }
            spinChange.block();
            const double v = shapeValue(getSliderValue());
            spin->set_value(addMode ? v : this->slider2value(v));
            spinChange.unblock();
            queue_draw(); // redraw Cairo value text
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
                if ((event->type == GDK_2BUTTON_PRESS && event->button == 1)
                        || (event->type == GDK_BUTTON_PRESS && event->button == 3)) {
                    resetValue(false);
                    return true;
                }
                return false;
            }, false);
    }

    instances_.push_back(this);

    show_all();
}

Adjuster::~Adjuster ()
{
    instances_.erase(std::remove(instances_.begin(), instances_.end(), this), instances_.end());


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

void Adjuster::setResetValue(double value)
{
    ctorDefaultVal = shapeValue(value);
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

void Adjuster::benchDragTo (double a)
{
    // Deliberately does NOT block spinChange/sliderChange: the point is to
    // exercise the same debounce a mouse drag goes through.
    setSliderValue(addMode ? shapeValue(a) : value2slider(shapeValue(a)));
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

void Adjuster::hideSpinButton()
{
    spin->set_visible(false);
    spin->set_no_show_all(true);
}

void Adjuster::setSliderMinWidth(int px)
{
    slider->set_size_request(px, -1);
}

void Adjuster::get_preferred_width_vfunc(int& minimum_width, int& natural_width) const
{
    Gtk::Grid::get_preferred_width_vfunc(minimum_width, natural_width);

    if (!adjustmentName.empty()) {
        minimum_width = std::min(minimum_width, RTScalable::scalePixelSize(80));
        natural_width = std::min(natural_width, RTScalable::scalePixelSize(160));
    }
}

void Adjuster::get_preferred_width_for_height_vfunc(int height, int& minimum_width, int& natural_width) const
{
    Gtk::Grid::get_preferred_width_for_height_vfunc(height, minimum_width, natural_width);

    if (!adjustmentName.empty()) {
        minimum_width = std::min(minimum_width, RTScalable::scalePixelSize(80));
        natural_width = std::min(natural_width, RTScalable::scalePixelSize(160));
    }
}

namespace
{

// GTK4-seam draw function (docs/gtk4-readiness.md): pure function of
// (context, state) — the widget's on_draw only gathers the spec.
struct AdjusterPillSpec {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    double radius = 4.0;
    // Fill extent in widget coordinates; fillRight <= fillLeft draws no fill.
    int fillLeft = 0;
    int fillRight = 0;
    Gdk::RGBA well;
    Gdk::RGBA wash;
};

void drawAdjusterPill(const Cairo::RefPtr<Cairo::Context>& cr, const AdjusterPillSpec& s)
{
    const auto pillPath = [&]() {
        cr->begin_new_sub_path();
        cr->arc(s.x + s.radius, s.y + s.radius, s.radius, rtengine::RT_PI, rtengine::RT_PI * 1.5);
        cr->arc(s.x + s.w - s.radius, s.y + s.radius, s.radius, rtengine::RT_PI * 1.5, 0);
        cr->arc(s.x + s.w - s.radius, s.y + s.h - s.radius, s.radius, 0, rtengine::RT_PI * 0.5);
        cr->arc(s.x + s.radius, s.y + s.h - s.radius, s.radius, rtengine::RT_PI * 0.5, rtengine::RT_PI);
        cr->close_path();
    };

    // Well background: recesses on dark themes, reads as a white field on
    // light ones.
    pillPath();
    cr->set_source_rgba(s.well.get_red(), s.well.get_green(), s.well.get_blue(), 0.85);
    cr->fill();

    // Value fill, clipped to the pill, in the theme's wash direction —
    // lightens dark themes, darkens light ones.
    if (s.fillRight > s.fillLeft + 1) {
        cr->save();
        pillPath();
        cr->clip();
        auto fillGrad = Cairo::LinearGradient::create(0, s.y, 0, s.y + s.h);
        fillGrad->add_color_stop_rgba(0.0, s.wash.get_red(), s.wash.get_green(), s.wash.get_blue(), 0.14);
        fillGrad->add_color_stop_rgba(1.0, s.wash.get_red(), s.wash.get_green(), s.wash.get_blue(), 0.03);
        cr->set_source(fillGrad);
        cr->rectangle(s.fillLeft, s.y, s.fillRight - s.fillLeft, s.h);
        cr->fill();
        cr->restore();
    }
}

} // namespace

void Adjuster::beginValueEdit()
{
    if (!valuePopover_) {
        valuePopover_.reset(new Gtk::Popover());
        valuePopover_->set_relative_to(*this);
        valuePopover_->set_position(Gtk::POS_TOP);

        valueEntry_ = Gtk::manage(new Gtk::Entry());
        valueEntry_->set_width_chars(7);
        valueEntry_->set_max_width_chars(9);
        valueEntry_->set_alignment(1.f);
        valueEntry_->set_input_purpose(Gtk::INPUT_PURPOSE_DIGITS);
        valueEntry_->signal_activate().connect(sigc::mem_fun(*this, &Adjuster::commitValueEdit));
        valueEntry_->signal_key_press_event().connect(
            [this](GdkEventKey* event) -> bool {
                if (event->keyval == GDK_KEY_Escape) {
                    valuePopover_->popdown();   // leave the value untouched
                    return true;
                }
                return false;
            }, false);

        auto* box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 0));
        box->set_border_width(4);
        box->pack_start(*valueEntry_, Gtk::PACK_SHRINK);
        valuePopover_->add(*box);
        box->show_all();
    }

    valuePopover_->set_pointing_to(valueTextRect_);
    valueEntry_->set_text(spin->get_text());
    valuePopover_->popup();
    valueEntry_->grab_focus();
    valueEntry_->select_region(0, -1);
}

void Adjuster::commitValueEdit()
{
    if (!valueEntry_) {
        return;
    }

    // Accept either decimal separator whatever the locale, and parse with the
    // locale-independent reader so "0.33" always means a third.
    std::string text = valueEntry_->get_text().raw();
    std::replace(text.begin(), text.end(), ',', '.');

    const char* start = text.c_str();
    char* end = nullptr;
    const double parsed = g_ascii_strtod(start, &end);

    if (end != start && std::isfinite(parsed)) {
        // Same entry point the slider uses, so listeners and the pill update
        // exactly as they would from a drag.
        spin->set_value(rtengine::LIM(parsed, vMin, vMax));
        queue_draw();
    }

    valuePopover_->popdown();
}

bool Adjuster::on_draw(const Cairo::RefPtr<Cairo::Context>& cr)
{
    if (!adjustmentName.empty() && slider->get_visible()) {
        // Pill covers only the slider area — right edge stops where spin begins
        const Gtk::Allocation sAlloc = slider->get_allocation();

        if (sAlloc.get_width() < 10) {
            return Gtk::Grid::on_draw(cr);
        }

        AdjusterPillSpec spec;
        spec.x = sAlloc.get_x();
        spec.y = 0;
        spec.w = sAlloc.get_width();
        spec.h = get_allocated_height();
        spec.well = themeColor(*this, "steep_surface_0", Gdk::RGBA("#14171f"));
        spec.wash = themeColor(*this, "steep_wash", Gdk::RGBA("#ffffff"));

        auto adj = slider->get_adjustment();
        const double rangeMin = adj->get_lower();
        const double range = adj->get_upper() - rangeMin;

        if (range > 0) {
            const double valueFrac = (adj->get_value() - rangeMin) / range;

            if (isBipolar_) {
                const double centerFrac = (0.0 - rangeMin) / range;
                const int centerX = spec.x + static_cast<int>(centerFrac * spec.w);
                const int valueX = spec.x + static_cast<int>(valueFrac * spec.w);
                spec.fillLeft = std::min(centerX, valueX);
                spec.fillRight = std::max(centerX, valueX);
            } else {
                spec.fillLeft = spec.x;
                spec.fillRight = spec.x + static_cast<int>(valueFrac * spec.w);
            }
        }

        drawAdjusterPill(cr, spec);
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
        auto fontDesc = Pango::FontDescription(pillFontName());
        layout->set_font_description(fontDesc);

        const int maxLabelWidth = sAlloc.get_width() / 2;
        layout->set_width(maxLabelWidth * Pango::SCALE);
        layout->set_ellipsize(Pango::ELLIPSIZE_END);

        Pango::Rectangle inkRect, logRect;
        layout->get_pixel_extents(inkRect, logRect);

        const int textX = sAlloc.get_x() + 10;
        const int textY = pillY + (pillH - logRect.get_height()) / 2;

        const Gdk::RGBA labelInk = themeColor(*this, "steep_text_hi", Gdk::RGBA("#e2e6ed"));
        cr->set_source_rgba(labelInk.get_red(), labelInk.get_green(), labelInk.get_blue(), 0.95);
        cr->move_to(textX, textY);
        layout->show_in_cairo_context(cr);

        // Draw value text on the right side of the pill
        Glib::ustring valStr = spin->get_text();
        auto valLayout = create_pango_layout(valStr);
        valLayout->set_font_description(Pango::FontDescription(pillFontName()));
        Pango::Rectangle vInk, vLog;
        valLayout->get_pixel_extents(vInk, vLog);
        const int valX = sAlloc.get_x() + sAlloc.get_width() - vLog.get_width() - 10;
        const int valY = pillY + (pillH - vLog.get_height()) / 2;
        const Gdk::RGBA valueInk = themeColor(*this, "steep_text", Gdk::RGBA("#cdd2da"));
        cr->set_source_rgba(valueInk.get_red(), valueInk.get_green(), valueInk.get_blue(), 0.85);
        cr->move_to(valX, valY);
        valLayout->show_in_cairo_context(cr);

        // Remember where the number landed so a click on it can open the
        // type-in editor (see beginValueEdit). Generously padded: the digits
        // are a small target.
        valueTextRect_.set_x(valX - 6);
        valueTextRect_.set_y(pillY);
        valueTextRect_.set_width(vLog.get_width() + 14);
        valueTextRect_.set_height(pillH);
    } else {
        valueTextRect_.set_width(0);
    }

    return true;
}
