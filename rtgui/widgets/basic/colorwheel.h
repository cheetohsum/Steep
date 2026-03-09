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
#pragma once

#include <gtkmm.h>

class ColorWheel;

class ColorWheelListener
{
public:
    virtual ~ColorWheelListener() = default;
    virtual void colorWheelChanged(ColorWheel* source, double hue, double saturation) = 0;
};

class ColorWheelArea final : public Gtk::DrawingArea
{
public:
    ColorWheelArea();

    void setHueSat(double hue, double saturation, bool notify = true);
    double getHue() const { return hue_; }
    double getSaturation() const { return sat_; }
    void reset();
    void setOwner(ColorWheel* owner) { owner_ = owner; }
    void setListener(ColorWheelListener* l) { listener_ = l; }
    void setEdited(bool yes) { edited_ = yes; }
    bool getEdited() const { return edited_; }

    bool on_draw(const Cairo::RefPtr<Cairo::Context>& cr) override;
    bool on_button_press_event(GdkEventButton* event) override;
    bool on_button_release_event(GdkEventButton* event) override;
    bool on_motion_notify_event(GdkEventMotion* event) override;
    Gtk::SizeRequestMode get_request_mode_vfunc() const override;
    void get_preferred_width_vfunc(int& minimum_width, int& natural_width) const override;
    void get_preferred_height_for_width_vfunc(int width, int& minimum_height, int& natural_height) const override;
    void on_style_updated() override;

private:
    bool notifyListener();
    void updateFromMouse(double mx, double my);

    double hue_;       // 0..360 degrees
    double sat_;       // 0..1
    ColorWheel* owner_;
    ColorWheelListener* listener_;
    bool isDragged_;
    bool edited_;
    sigc::connection delayconn_;
    Cairo::RefPtr<Cairo::ImageSurface> cachedWheel_;
    int cachedSize_;
};

class ColorWheel : public Gtk::Box
{
public:
    ColorWheel(const Glib::ustring& label = "");

    void setHueSat(double hue, double saturation, bool notify = true);
    double getHue() const { return area_.getHue(); }
    double getSaturation() const { return area_.getSaturation(); }
    void reset();
    void setListener(ColorWheelListener* l) { area_.setListener(l); }
    void setEdited(bool yes) { area_.setEdited(yes); }
    bool getEdited() const { return area_.getEdited(); }
    void updateIndicatorFromArea() { updateIndicator(); }

private:
    void updateIndicator();
    void onEntryActivate();
    bool onEntryFocusOut(GdkEventFocus* event);
    void applyEntryValue();
    bool resetPressed(GdkEventButton* event);

    ColorWheelArea area_;
    Gtk::Label label_;

    // Indicator row: colored dot + editable entry
    Gtk::Box indicatorBox_;
    Gtk::Label dotLabel_;
    Gtk::Entry entry_;
    bool updatingIndicator_;
};
