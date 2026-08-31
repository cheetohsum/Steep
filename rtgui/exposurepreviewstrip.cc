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

#include "exposurepreviewstrip.h"

#include <cmath>
#include <thread>
#include <algorithm>
#include <mutex>

#include "rtengine/rt_math.h"

#include "thumbnail.h"
#include "../rtengine/iimage.h"

#ifdef _OPENMP
#include <omp.h>
#endif

namespace
{

std::mutex& previewStripGenerationMutex()
{
    static std::mutex mutex;
    return mutex;
}

class PreviewStripOpenMPGuard
{
public:
    PreviewStripOpenMPGuard()
    {
#ifdef _OPENMP
        previousThreads_ = std::max(1, omp_get_max_threads());
        omp_set_num_threads(1);
#endif
    }

    ~PreviewStripOpenMPGuard()
    {
#ifdef _OPENMP
        omp_set_num_threads(previousThreads_);
#endif
    }

private:
#ifdef _OPENMP
    int previousThreads_ = 1;
#endif
};

int visibleWidgetWidth(Gtk::Widget* widget)
{
    if (!widget) {
        return 1;
    }

    int width = widget->get_allocated_width();
    Gtk::Widget* child = widget;

    for (Gtk::Widget* parent = child->get_parent(); parent; parent = parent->get_parent()) {
        const int parentWidth = parent->get_allocated_width();
        if (parentWidth > 0) {
            const int x = child->get_allocation().get_x();
            const int available = x >= 0 && x < parentWidth ? parentWidth - x : parentWidth;
            width = width > 0 ? std::min(width, available) : available;
        }

        child = parent;
    }

    return std::max(1, width);
}

}

PreviewStrip::PreviewStrip()
{
    set_can_focus(false);
    add_events(Gdk::BUTTON_PRESS_MASK | Gdk::BUTTON_RELEASE_MASK | Gdk::POINTER_MOTION_MASK);
    set_size_request(-1, STRIP_HEIGHT);
    set_hexpand(true);
    set_halign(Gtk::ALIGN_FILL);
    set_margin_start(4);
    set_margin_end(4);
    set_margin_top(2);
    set_margin_bottom(1);
}

PreviewStrip::~PreviewStrip()
{
    if (aliveToken_) {
        aliveToken_->store(false, std::memory_order_release);
    }
    cancelThumbnailGeneration();
    if (debounceConn_.connected()) {
        debounceConn_.disconnect();
    }
    if (dragThrottleConn_.connected()) {
        dragThrottleConn_.disconnect();
    }
}

void PreviewStrip::setThumbnail(Thumbnail* thm)
{
    if (thumbnail_ != thm) {
        cancelThumbnailGeneration();
        thumbnails_.clear();
        queue_draw();
    }

    thumbnail_ = thm;
    regenerateThumbnails();
}

void PreviewStrip::setCurrentParams(const rtengine::procparams::ProcParams& params)
{
    // A strip-generated update is ignored while its gesture is active. External
    // edits establish a new neutral center instead of compounding the old look.
    if (isDragging_) {
        // Guard against a lost button-release (a popup can steal the grab
        // mid-scrub): if no button is actually held, the drag ended and the
        // strip must not stay frozen ignoring every future update.
        bool buttonHeld = false;
        if (auto win = get_window()) {
            auto display = Gdk::Display::get_default();
            auto seat = display ? display->get_default_seat() : Glib::RefPtr<Gdk::Seat>();
            auto pointer = seat ? seat->get_pointer() : Glib::RefPtr<Gdk::Device>();
            if (pointer) {
                int px = 0, py = 0;
                Gdk::ModifierType mask = static_cast<Gdk::ModifierType>(0);
                win->get_device_position(pointer, px, py, mask);
                buttonHeld = (mask & (Gdk::BUTTON1_MASK | Gdk::BUTTON2_MASK | Gdk::BUTTON3_MASK)) != 0;
            }
        }
        if (buttonHeld) {
            return;
        }
        isDragging_ = false;
        dragPending_ = false;
    }

    currentParams_ = std::make_shared<rtengine::procparams::ProcParams>(params);
    scrubberPos_ = 0.0;
    queue_draw();
    regenerateThumbnails();
}

void PreviewStrip::setParamModifier(ParamModifier mod)
{
    paramModifier_ = mod;
}

void PreviewStrip::setDragCallback(DragCallback cb)
{
    dragCallback_ = cb;
}

void PreviewStrip::setReleaseCallback(DragCallback cb)
{
    releaseCallback_ = cb;
}

void PreviewStrip::resetScrubber()
{
    scrubberPos_ = 0.0;
    queue_draw();
}

void PreviewStrip::regenerateThumbnails()
{
    if (!thumbnail_ || !currentParams_ || !paramModifier_) {
        return;
    }

    cancelThumbnailGeneration();

    if (debounceConn_.connected()) {
        debounceConn_.disconnect();
    }

    debounceConn_ = Glib::signal_timeout().connect([this]() {
        generateThumbnailsAsync();
        return false;
    }, THUMB_START_DELAY_MS);
}

void PreviewStrip::generateThumbnailsAsync()
{
    if (!thumbnail_ || !currentParams_ || !paramModifier_) {
        return;
    }

    if (cancelToken_) {
        cancelToken_->store(true);
    }

    auto cancel = std::make_shared<std::atomic<bool>>(false);
    cancelToken_ = cancel;

    auto params = currentParams_; // shared_ptr copy
    Thumbnail* thm = thumbnail_;
    thm->increaseRef(); // prevent thumbnail deletion while thread runs
    ParamModifier modifier = paramModifier_;
    PreviewStrip* self = this;
    auto alive = aliveToken_;

    std::thread([self, params, thm, cancel, modifier, alive]() {
        std::unique_lock<std::mutex> generationLock(previewStripGenerationMutex());
        if (cancel->load(std::memory_order_acquire)
            || !alive->load(std::memory_order_acquire)) {
            thm->decreaseRef();
            return;
        }

        PreviewStripOpenMPGuard ompGuard;
        auto results = std::make_shared<std::vector<Glib::RefPtr<Gdk::Pixbuf>>>();
        results->reserve(NUM_THUMBS);

        for (int i = 0; i < NUM_THUMBS; ++i) {
            if (cancel->load(std::memory_order_acquire)
                || !alive->load(std::memory_order_acquire)) {
                thm->decreaseRef();
                return;
            }

            double t = -1.0 + (2.0 * i) / (NUM_THUMBS - 1);

            rtengine::procparams::ProcParams pp = *params;
            modifier(pp, t);

            double scale;
            rtengine::IImage8* img = thm->processThumbImage(pp, STRIP_HEIGHT, scale);
            if (!img) {
                results->push_back(Glib::RefPtr<Gdk::Pixbuf>());
                continue;
            }

            if (cancel->load(std::memory_order_acquire)
                || !alive->load(std::memory_order_acquire)) {
                delete img;
                thm->decreaseRef();
                return;
            }

            int tw = img->getWidth();
            int th = img->getHeight();
            auto pixbuf = Gdk::Pixbuf::create_from_data(
                img->getData(), Gdk::COLORSPACE_RGB, false, 8,
                tw, th, tw * 3);
            auto copied = pixbuf->copy();
            delete img;

            results->push_back(copied);
        }

        thm->decreaseRef();

        if (cancel->load(std::memory_order_acquire)
            || !alive->load(std::memory_order_acquire)) {
            return;
        }

        Glib::signal_idle().connect_once([self, results, cancel, alive]() {
            if (cancel->load(std::memory_order_acquire)
                || !alive->load(std::memory_order_acquire)) {
                return;
            }
            self->thumbnails_ = *results;
            self->queue_draw();
        });
    }).detach();
}

void PreviewStrip::cancelThumbnailGeneration()
{
    if (cancelToken_) {
        cancelToken_->store(true, std::memory_order_release);
    }
    if (debounceConn_.connected()) {
        debounceConn_.disconnect();
    }
}

void PreviewStrip::handleDrag(double x)
{
    int w = visibleWidgetWidth(this);
    if (w <= 0) return;
    scrubberPos_ = std::max(-1.0, std::min(1.0, (x / w) * 2.0 - 1.0));
    queue_draw();
}

Gtk::SizeRequestMode PreviewStrip::get_request_mode_vfunc() const
{
    return Gtk::SIZE_REQUEST_CONSTANT_SIZE;
}

void PreviewStrip::get_preferred_width_vfunc(int& min, int& natural) const
{
    min = 120;
    natural = STRIP_WIDTH;
}

void PreviewStrip::get_preferred_height_vfunc(int& min, int& natural) const
{
    min = STRIP_HEIGHT;
    natural = STRIP_HEIGHT;
}

bool PreviewStrip::on_draw(const Cairo::RefPtr<Cairo::Context>& cr)
{
    const int w = visibleWidgetWidth(this);
    const int h = get_allocated_height();

    if (w <= 0 || h <= 0) return true;

    cr->save();
    cr->rectangle(0, 0, w, h);
    cr->clip();

    // Pill-shaped clip
    double r = CORNER_RADIUS;
    cr->begin_new_path();
    cr->arc(r, r, r, M_PI, 1.5 * M_PI);
    cr->arc(w - r, r, r, 1.5 * M_PI, 2.0 * M_PI);
    cr->arc(w - r, h - r, r, 0, 0.5 * M_PI);
    cr->arc(r, h - r, r, 0.5 * M_PI, M_PI);
    cr->close_path();
    cr->clip();

    if (thumbnails_.empty() || thumbnails_.size() < static_cast<size_t>(NUM_THUMBS)) {
        // Quiet placeholder while previews generate — the finished set is
        // installed atomically, so the strip reveals all frames at once.
        auto grad = Cairo::LinearGradient::create(0, 0, w, 0);
        grad->add_color_stop_rgb(0, 0.15, 0.15, 0.18);
        grad->add_color_stop_rgb(0.5, 0.25, 0.25, 0.28);
        grad->add_color_stop_rgb(1, 0.35, 0.35, 0.38);
        cr->set_source(grad);
        cr->paint();
    } else {
        double slotWidth = static_cast<double>(w) / NUM_THUMBS;

        for (int i = 0; i < NUM_THUMBS; ++i) {
            if (!thumbnails_[i]) continue;

            double thumbW = thumbnails_[i]->get_width();
            double thumbH = thumbnails_[i]->get_height();
            double slotX = slotWidth * i;

            // Scale to cover the slot (fill/crop mode, no gaps)
            double scaleX = slotWidth / thumbW;
            double scaleY = static_cast<double>(h) / thumbH;
            double sc = std::max(scaleX, scaleY);

            // Center the thumbnail within the slot
            double offsetX = (slotWidth - thumbW * sc) / 2.0;
            double offsetY = (h - thumbH * sc) / 2.0;

            cr->save();
            // Clip to this slot, with a 1px breathing gap between neighbours
            // so the variants read as distinct frames instead of one smear.
            const double gapL = (i > 0) ? 1.0 : 0.0;
            const double gapR = (i < NUM_THUMBS - 1) ? 1.0 : 0.0;
            cr->rectangle(slotX + gapL, 0, slotWidth - gapL - gapR, h);
            cr->clip();
            cr->translate(slotX + offsetX, offsetY);
            cr->scale(sc, sc);
            Gdk::Cairo::set_source_pixbuf(cr, thumbnails_[i], 0, 0);
            cr->paint();
            cr->restore();
        }
    }

    // Scrubber line
    {
        double scrubX = (scrubberPos_ + 1.0) / 2.0 * w;

        cr->set_source_rgba(1, 1, 1, 0.85);
        cr->set_line_width(2.0);
        cr->move_to(scrubX, 0);
        cr->line_to(scrubX, h);
        cr->stroke();

        cr->set_source_rgba(1, 1, 1, 0.9);
        cr->move_to(scrubX - 4, 0);
        cr->line_to(scrubX + 4, 0);
        cr->line_to(scrubX, 5);
        cr->close_path();
        cr->fill();

        cr->move_to(scrubX - 4, h);
        cr->line_to(scrubX + 4, h);
        cr->line_to(scrubX, h - 5);
        cr->close_path();
        cr->fill();
    }

    cr->restore();

    // Pill border
    cr->begin_new_path();
    cr->arc(r, r, r, M_PI, 1.5 * M_PI);
    cr->arc(w - r, r, r, 1.5 * M_PI, 2.0 * M_PI);
    cr->arc(w - r, h - r, r, 0, 0.5 * M_PI);
    cr->arc(r, h - r, r, 0.5 * M_PI, M_PI);
    cr->close_path();
    cr->set_source_rgba(1, 1, 1, 0.15);
    cr->set_line_width(1.0);
    cr->stroke();

    return true;
}

bool PreviewStrip::on_button_press_event(GdkEventButton* event)
{
    if (event->button == 1) {
        // Snapshot current params as the baseline for this entire drag gesture.
        // All modifier calls during the drag use this snapshot, preventing compounding.
        if (currentParams_) {
            dragBaseParams_ = std::make_shared<rtengine::procparams::ProcParams>(*currentParams_);
        }
        isDragging_ = true;
        dragPending_ = false;
        handleDrag(event->x);
        return true;
    }
    return false;
}

bool PreviewStrip::on_button_release_event(GdkEventButton* event)
{
    if (event->button == 1 && isDragging_) {
        dragPending_ = false;
        if (dragThrottleConn_.connected()) {
            dragThrottleConn_.disconnect();
        }
        handleDrag(event->x);

        if (dragBaseParams_ && paramModifier_) {
            rtengine::procparams::ProcParams pp = *dragBaseParams_;
            paramModifier_(pp, scrubberPos_);
            if (releaseCallback_) {
                releaseCallback_(pp, scrubberPos_);
            } else if (dragCallback_) {
                dragCallback_(pp, scrubberPos_);
            }
        }
        isDragging_ = false;
        dragBaseParams_.reset();
        return true;
    }
    return false;
}

bool PreviewStrip::on_motion_notify_event(GdkEventMotion* event)
{
    if (isDragging_) {
        handleDrag(event->x);

        if (!dragPending_ && dragBaseParams_ && paramModifier_ && dragCallback_) {
            dragPending_ = true;
            dragThrottleConn_ = Glib::signal_timeout().connect([this]() {
                dragPending_ = false;
                if (!isDragging_ || !dragBaseParams_ || !paramModifier_ || !dragCallback_) {
                    return false;
                }

                rtengine::procparams::ProcParams pp = *dragBaseParams_;
                paramModifier_(pp, scrubberPos_);
                dragCallback_(pp, scrubberPos_);

                return false;
            }, 120);
        }

        return true;
    }
    return false;
}
