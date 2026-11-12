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
#include "motion.h"

#include <algorithm>
#include <cmath>

#include <gtkmm.h>

#include "options.h"
#include "rtengine/rtapp.h"

namespace steep
{
namespace motion
{

int ms(Duration token)
{
    switch (token) {
        case Duration::Micro:
            return 90;

        case Duration::Large:
            return 240;

        case Duration::Standard:
        default:
            return 160;
    }
}

int exitMs(Duration token)
{
    return static_cast<int>(std::lround(ms(token) * 0.7));
}

double ease(Ease curve, double t)
{
    t = std::max(0.0, std::min(1.0, t));

    switch (curve) {
        case Ease::Enter: {
            const double u = 1.0 - t;
            return 1.0 - u * u * u;
        }

        case Ease::Exit:
            return t * t;

        case Ease::Move:
            return t < 0.5 ? 4.0 * t * t * t : 1.0 - std::pow(-2.0 * t + 2.0, 3.0) / 2.0;

        case Ease::Linear:
        default:
            return t;
    }
}

bool reduceMotion()
{
    if (App::get().options().reduceMotion) {
        return true;
    }

    // Also honour a desktop-wide "no animations" setting.
    const auto settings = Gtk::Settings::get_default();
    return settings && !settings->property_gtk_enable_animations().get_value();
}

void applyReduceMotionSetting()
{
    const auto settings = Gtk::Settings::get_default();

    if (!settings) {
        return;
    }

    if (App::get().options().reduceMotion) {
        settings->property_gtk_enable_animations() = false;
    } else {
        // Back to whatever the desktop says, rather than forcing them on.
        gtk_settings_reset_property(settings->gobj(), "gtk-enable-animations");
    }
}

struct Handle::State {
    Gtk::Widget* widget = nullptr;
    guint tickId = 0;
    unsigned tickSerial = 0;   // identifies the live tick callback
    unsigned generation = 0;   // bumped by cancel/retarget; guards onDone
    bool inTick = false;
    bool running = false;
    bool looping = false;      // repeats forever; value is the phase

    double from = 0.0;
    double to = 0.0;
    double value = 0.0;
    double span = 1.0;         // distance a full-length run covers
    int fullMs = 0;            // duration of a full-length run
    int durMs = 0;             // duration of the current leg
    Ease curve = Ease::Enter;
    gint64 requestUs = 0;
    gint64 startUs = 0;        // 0 until the first frame

    std::function<void(double)> onStep;
    std::function<void()> onDone;
    sigc::connection unmapConn;

    ~State()
    {
        unmapConn.disconnect();

        if (widget) {
            if (tickId) {
                widget->remove_tick_callback(tickId);
            }

            widget->remove_destroy_notify_callback(this);
        }
    }

    static void* widgetDestroyed(void* data)
    {
        auto* self = static_cast<State*>(data);
        // The tick callback died with the widget.
        self->widget = nullptr;
        self->tickId = 0;
        self->running = false;
        return nullptr;
    }
};

namespace
{

using StatePtr = std::shared_ptr<Handle::State>;

void detach(const StatePtr& s)
{
    if (s->tickId) {
        // Never remove the callback from inside itself: that frees the slot
        // being executed. Zeroing the id makes the tick return false instead.
        if (!s->inTick && s->widget) {
            s->widget->remove_tick_callback(s->tickId);
        }

        s->tickId = 0;
    }

    s->unmapConn.disconnect();
}

void finishNow(const StatePtr& s)
{
    detach(s);
    s->running = false;
    s->value = s->to;
    const unsigned generation = ++s->generation;

    if (s->onStep) {
        s->onStep(s->to);
    }

    // onStep may have cancelled or retargeted this animation.
    if (s->generation == generation && s->onDone) {
        s->onDone();
    }
}

bool tick(const StatePtr& s, unsigned serial, const Glib::RefPtr<Gdk::FrameClock>& clock)
{
    if (s->tickSerial != serial || !s->tickId || !s->running) {
        return false;
    }

    const gint64 now = clock ? clock->get_frame_time() : g_get_monotonic_time();

    if (!s->startUs) {
        // Count from the first frame that can actually show the motion (a
        // heavy layout between the request and this frame must not eat the
        // animation), less one frame so this frame already moves.
        s->startUs = now - std::min<gint64>(std::max<gint64>(now - s->requestUs, 0), 16000);
    }

    double t = s->durMs > 0 ? (now - s->startUs) / (s->durMs * 1000.0) : 1.0;

    if (s->looping) {
        t = std::fmod(t, 1.0);
    }

    s->inTick = true;

    if (t >= 1.0) {
        finishNow(s);
    } else {
        s->value = s->from + (s->to - s->from) * ease(s->curve, t);

        if (s->onStep) {
            s->onStep(s->value);
        }
    }

    s->inTick = false;
    return s->tickSerial == serial && s->tickId != 0;
}

void start(const StatePtr& s)
{
    if (s->looping) {
        if (!s->widget || s->durMs <= 0 || reduceMotion()) {
            // A resting indicator instead of a moving one.
            if (s->onStep) {
                s->onStep(0.0);
            }

            return;
        }
    } else if (!s->widget || !s->widget->get_mapped() || s->durMs <= 0 || s->from == s->to || reduceMotion()) {
        finishNow(s);
        return;
    }

    s->running = true;
    s->startUs = 0;
    s->requestUs = g_get_monotonic_time();

    if (!s->tickId) {
        const unsigned serial = ++s->tickSerial;
        // The slot keeps the state alive for as long as GTK holds the tick.
        s->tickId = s->widget->add_tick_callback(
            [s, serial](const Glib::RefPtr<Gdk::FrameClock>& clock) -> bool {
                const StatePtr keep = s;
                return tick(keep, serial, clock);
            });
    }

    // A loop simply gets no frames while hidden and picks up again later.
    if (!s->looping && !s->unmapConn.connected()) {
        // An unmapped widget gets no frames; finish rather than strand the
        // caller half-way (onDone often releases a layout pause).
        const std::weak_ptr<Handle::State> weak = s;
        s->unmapConn = s->widget->signal_unmap().connect([weak]() {
            if (const StatePtr alive = weak.lock()) {
                if (alive->running) {
                    finishNow(alive);
                }
            }
        });
    }
}

Handle launch(Gtk::Widget& clockWidget, double from, double to, int fullMs, double span, Ease curve,
              std::function<void(double)> onStep, std::function<void()> onDone)
{
    auto s = std::make_shared<Handle::State>();
    s->widget = &clockWidget;
    clockWidget.add_destroy_notify_callback(s.get(), &Handle::State::widgetDestroyed);
    s->from = from;
    s->to = to;
    s->value = from;
    s->span = span > 0.0 ? span : std::abs(to - from);
    s->fullMs = fullMs;
    s->durMs = s->span > 0.0
        ? static_cast<int>(std::lround(fullMs * std::min(1.0, std::abs(to - from) / s->span)))
        : 0;
    s->curve = curve;
    s->onStep = std::move(onStep);
    s->onDone = std::move(onDone);

    Handle handle(s);
    start(s);
    return handle;
}

}

Handle::Handle() = default;

Handle::Handle(std::shared_ptr<State> state) : state_(std::move(state)) {}

Handle::~Handle()
{
    stop(false);
}

Handle::Handle(Handle&& other) noexcept : state_(std::move(other.state_)) {}

Handle& Handle::operator=(Handle&& other) noexcept
{
    if (this != &other) {
        stop(false);
        state_ = std::move(other.state_);
    }

    return *this;
}

bool Handle::running() const
{
    return state_ && state_->running;
}

double Handle::value() const
{
    return state_ ? state_->value : 0.0;
}

void Handle::stop(bool finish)
{
    if (!state_ || !state_->running) {
        return;
    }

    const StatePtr keep = state_;

    if (finish) {
        finishNow(keep);
    } else {
        ++keep->generation;
        keep->running = false;
        detach(keep);
    }
}

void Handle::retarget(double to)
{
    if (state_) {
        retarget(to, state_->fullMs, state_->curve);
    }
}

void Handle::retarget(double to, int fullSpanMs, Ease curve)
{
    if (!state_) {
        return;
    }

    const StatePtr keep = state_;
    ++keep->generation;
    keep->from = keep->value;
    keep->to = to;
    keep->fullMs = fullSpanMs;
    keep->curve = curve;
    keep->durMs = keep->span > 0.0
        ? static_cast<int>(std::lround(fullSpanMs * std::min(1.0, std::abs(to - keep->from) / keep->span)))
        : 0;
    start(keep);
}

Handle run(Gtk::Widget& clockWidget, double from, double to, Duration token, Ease curve,
           std::function<void(double)> onStep, std::function<void()> onDone)
{
    return launch(clockWidget, from, to, ms(token), 0.0, curve, std::move(onStep), std::move(onDone));
}

Handle run(Gtk::Widget& clockWidget, double from, double to, int milliseconds, Ease curve,
           std::function<void(double)> onStep, std::function<void()> onDone)
{
    return launch(clockWidget, from, to, milliseconds, 0.0, curve, std::move(onStep), std::move(onDone));
}

Handle toggle(Gtk::Widget& clockWidget, double current, bool show, Duration token,
              std::function<void(double)> onStep, std::function<void()> onDone)
{
    current = std::max(0.0, std::min(1.0, current));
    return launch(clockWidget, current, show ? 1.0 : 0.0, show ? ms(token) : exitMs(token), 1.0,
                  show ? Ease::Enter : Ease::Exit, std::move(onStep), std::move(onDone));
}

Handle loop(Gtk::Widget& clockWidget, int periodMs, std::function<void(double)> onPhase)
{
    auto s = std::make_shared<Handle::State>();
    s->widget = &clockWidget;
    clockWidget.add_destroy_notify_callback(s.get(), &Handle::State::widgetDestroyed);
    s->looping = true;
    s->from = 0.0;
    s->to = 1.0;
    s->fullMs = s->durMs = periodMs;
    s->curve = Ease::Linear;
    s->onStep = std::move(onPhase);

    Handle handle(s);
    start(s);
    return handle;
}

void configureRevealer(Gtk::Revealer& revealer, Duration token)
{
    revealer.set_transition_duration(ms(token));
}

void configureStack(Gtk::Stack& stack, Duration token)
{
    stack.set_transition_duration(ms(token));
}

Handle animatePaneHeight(Gtk::ScrolledWindow& pane, int fromH, int toH, bool pinExact,
                         std::function<void()> restore)
{
    Gtk::ScrolledWindow* const sw = &pane;

    // GTK rejects min > max, so the order of the two setters depends on the
    // direction of travel.
    const auto apply = [sw, pinExact](int h) {
        h = std::max(h, 1);

        if (!pinExact) {
            const int minH = sw->get_min_content_height();
            sw->set_max_content_height(minH > 0 ? std::max(h, minH) : h);
            return;
        }

        const int maxH = sw->get_max_content_height();

        if (maxH < 0 || h <= maxH) {
            sw->set_min_content_height(h);
            sw->set_max_content_height(h);
        } else {
            sw->set_max_content_height(h);
            sw->set_min_content_height(h);
        }
    };

    if (pinExact) {
        // Scrollbar hidden while the height is pinned; see the header for why
        // this is EXTERNAL and not NEVER.
        Gtk::PolicyType hPolicy = Gtk::POLICY_NEVER, vPolicy = Gtk::POLICY_AUTOMATIC;
        sw->get_policy(hPolicy, vPolicy);
        sw->set_policy(hPolicy, Gtk::POLICY_EXTERNAL);
    }

    apply(fromH);

    const bool grow = toH > fromH;
    return run(pane, fromH, toH, grow ? ms(Duration::Standard) : exitMs(Duration::Standard),
               grow ? Ease::Enter : Ease::Exit,
               [apply](double h) { apply(static_cast<int>(std::lround(h))); },
               std::move(restore));
}

}
}
