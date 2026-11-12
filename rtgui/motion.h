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
#pragma once

#include <functional>
#include <memory>

namespace Gtk
{
class Widget;
class Revealer;
class Stack;
class ScrolledWindow;
}

// One place for every hand-rolled UI animation.
//
// Animations here are driven by the widget's frame clock and measure progress
// in elapsed time, so a late frame jumps ahead instead of running slow (the
// old "fixed step per 16 ms timeout" loops stretched whenever the main loop
// was busy). Durations and curves come from a small token set so the whole UI
// moves alike.
namespace steep
{
namespace motion
{

enum class Duration {
    Micro,     // 90 ms  - hover feedback, small inline reveals
    Standard,  // 160 ms - folds, reveals, value morphs
    Large      // 240 ms - panels and drawers crossing a lot of screen
};

enum class Ease {
    Enter,   // cubic ease-out: things arriving
    Exit,    // quad ease-in: things leaving
    Move,    // cubic ease-in-out: things travelling between two rests
    Linear
};

int ms(Duration token);
// Leaving is quicker than arriving: 0.7x the token.
int exitMs(Duration token);
double ease(Ease curve, double t);

// The user's "Reduce motion" preference.
bool reduceMotion();
// Push the preference into GtkSettings (gtk-enable-animations) so Revealers,
// Stacks and CSS transitions follow it. Call at startup and when it changes.
void applyReduceMotionSetting();

class Handle
{
public:
    Handle();
    ~Handle();  // cancels: no further onStep, onDone is not called
    Handle(Handle&& other) noexcept;
    Handle& operator=(Handle&& other) noexcept;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    bool running() const;
    // Last value handed to onStep (the end value once finished).
    double value() const;
    // finish = true jumps to the end value and fires onStep + onDone;
    // finish = false just freezes where it is.
    void stop(bool finish = false);
    // Head for a new end value from the CURRENT value. The duration is the
    // original one scaled by the distance left to travel, so reversing in
    // mid-flight neither snaps nor takes the full time again.
    void retarget(double to);
    void retarget(double to, int fullSpanMs, Ease curve);

    struct State;
    explicit Handle(std::shared_ptr<State> state);  // used by run(); not for callers

private:
    std::shared_ptr<State> state_;
};

// Animate a value from `from` to `to`. `onStep` receives the already eased
// value; `onDone` fires once after the final step. When `clockWidget` is not
// mapped, or reduce-motion is on, onStep(to) and onDone run immediately.
// If the widget is unmapped mid-flight the animation finishes at once, so
// onDone is a safe place to release whatever the animation was holding.
Handle run(Gtk::Widget& clockWidget, double from, double to, Duration token, Ease curve,
           std::function<void(double value)> onStep, std::function<void()> onDone = {});
Handle run(Gtk::Widget& clockWidget, double from, double to, int milliseconds, Ease curve,
           std::function<void(double value)> onStep, std::function<void()> onDone = {});

// Drive a 0..1 "shown" fraction to fully shown or fully hidden from wherever
// it currently is: Enter over `token` going in, Exit over 0.7x going out,
// both scaled by the distance left so a reversal in mid-flight stays smooth.
Handle toggle(Gtk::Widget& clockWidget, double current, bool show, Duration token,
              std::function<void(double fraction)> onStep, std::function<void()> onDone = {});

// A repeating 0..1 phase (spinners). Ticks only while the widget is on
// screen; with reduce-motion on, onPhase(0) is delivered once and it rests.
Handle loop(Gtk::Widget& clockWidget, int periodMs, std::function<void(double phase)> onPhase);

void configureRevealer(Gtk::Revealer& revealer, Duration token = Duration::Standard);
void configureStack(Gtk::Stack& stack, Duration token = Duration::Standard);

// Fold a scrolled pane's height from `fromH` to `toH` (the dir tree and the
// album list share this). With pinExact the height is pinned by setting
// min == max content height each frame under POLICY_EXTERNAL - a ScrolledWindow
// with a vertical POLICY_NEVER ignores both and demands the full child height.
// Without it only the maximum is moved (for panes whose minimum must stay
// small). `restore` runs at the end and must put policy/min/max back.
Handle animatePaneHeight(Gtk::ScrolledWindow& pane, int fromH, int toH, bool pinExact,
                         std::function<void()> restore);

}
}
