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

#include "rtengine/rt_math.h"

#include "thumbnail.h"
#include "../rtengine/iimage.h"

PreviewStrip::PreviewStrip()
{
    set_can_focus(false);
    add_events(Gdk::BUTTON_PRESS_MASK | Gdk::BUTTON_RELEASE_MASK | Gdk::POINTER_MOTION_MASK);
    set_size_request(-1, STRIP_HEIGHT);
    set_margin_start(4);
    set_margin_end(4);
    set_margin_top(2);
    set_margin_bottom(1);
}

PreviewStrip::~PreviewStrip()
{
    if (cancelToken_) {
        cancelToken_->store(true);
    }
    if (debounceConn_.connected()) {
        debounceConn_.disconnect();
    }
    if (dragThrottleConn_.connected()) {
        dragThrottleConn_.disconnect();
    }
}

void PreviewStrip::setThumbnail(Thumbnail* thm)
{
    thumbnail_ = thm;
    regenerateThumbnails();
}

void PreviewStrip::setCurrentParams(const rtengine::procparams::ProcParams& params)
{
    // During dragging, ignore external param updates (they contain our own drag offsets).
    // The drag always works relative to dragBaseParams_ which was captured at drag start.
    if (isDragging_) return;

    currentParams_ = std::make_shared<rtengine::procparams::ProcParams>(params);
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

    if (debounceConn_.connected()) {
        debounceConn_.disconnect();
    }

    debounceConn_ = Glib::signal_timeout().connect([this]() {
        generateThumbnailsAsync();
        return false;
    }, 500);
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
    ParamModifier modifier = paramModifier_;
    PreviewStrip* self = this;

    std::thread([self, params, thm, cancel, modifier]() {
        auto results = std::make_shared<std::vector<Glib::RefPtr<Gdk::Pixbuf>>>();
        results->reserve(NUM_THUMBS);

        for (int i = 0; i < NUM_THUMBS; ++i) {
            if (cancel->load()) return;

            double t = -1.0 + (2.0 * i) / (NUM_THUMBS - 1);

            rtengine::procparams::ProcParams pp = *params;
            modifier(pp, t);

            double scale;
            rtengine::IImage8* img = thm->processThumbImage(pp, STRIP_HEIGHT, scale);
            if (!img) {
                results->push_back(Glib::RefPtr<Gdk::Pixbuf>());
                continue;
            }

            if (cancel->load()) {
                delete img;
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

        if (cancel->load()) return;

        Glib::signal_idle().connect_once([self, results, cancel]() {
            if (cancel->load()) return;
            self->thumbnails_ = *results;
            self->queue_draw();
        });
    }).detach();
}

void PreviewStrip::handleDrag(double x)
{
    int w = get_allocated_width();
    if (w <= 0) return;
    scrubberPos_ = std::max(-1.0, std::min(1.0, (x / w) * 2.0 - 1.0));
    queue_draw();
}

Gtk::SizeRequestMode PreviewStrip::get_request_mode_vfunc() const
{
    return Gtk::SIZE_REQUEST_CONSTANT_SIZE;
}

void PreviewStrip::get_preferred_height_vfunc(int& min, int& natural) const
{
    min = STRIP_HEIGHT;
    natural = STRIP_HEIGHT;
}

bool PreviewStrip::on_draw(const Cairo::RefPtr<Cairo::Context>& cr)
{
    const int w = get_allocated_width();
    const int h = get_allocated_height();

    if (w <= 0 || h <= 0) return true;

    cr->save();

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
        auto grad = Cairo::LinearGradient::create(0, 0, w, 0);
        grad->add_color_stop_rgb(0, 0.15, 0.15, 0.18);
        grad->add_color_stop_rgb(0.5, 0.25, 0.25, 0.28);
        grad->add_color_stop_rgb(1, 0.35, 0.35, 0.38);
        cr->set_source(grad);
        cr->paint();

        cr->set_source_rgba(1, 1, 1, 0.4);
        cr->select_font_face("sans-serif", Cairo::FONT_SLANT_NORMAL, Cairo::FONT_WEIGHT_NORMAL);
        cr->set_font_size(10);
        Cairo::TextExtents te;
        cr->get_text_extents("Loading previews...", te);
        cr->move_to((w - te.width) / 2, (h + te.height) / 2);
        cr->show_text("Loading previews...");
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
            // Clip to this slot
            cr->rectangle(slotX, 0, slotWidth, h);
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
        handleDrag(event->x);

        if (dragCallback_ && dragBaseParams_ && paramModifier_) {
            rtengine::procparams::ProcParams pp = *dragBaseParams_;
            paramModifier_(pp, scrubberPos_);
            dragCallback_(pp, scrubberPos_);
        }
        return true;
    }
    return false;
}

bool PreviewStrip::on_button_release_event(GdkEventButton* event)
{
    if (event->button == 1 && isDragging_) {
        isDragging_ = false;
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
            // Accept the final modified params as the new baseline.
            currentParams_ = std::make_shared<rtengine::procparams::ProcParams>(pp);
        }
        dragBaseParams_.reset();
        return true;
    }
    return false;
}

bool PreviewStrip::on_motion_notify_event(GdkEventMotion* event)
{
    if (isDragging_) {
        handleDrag(event->x);

        // Throttle: mark drag as pending, fire at most every 150ms
        dragPending_ = true;
        if (!dragThrottleConn_.connected()) {
            dragThrottleConn_ = Glib::signal_timeout().connect([this]() {
                if (dragPending_ && dragCallback_ && dragBaseParams_ && paramModifier_) {
                    dragPending_ = false;
                    rtengine::procparams::ProcParams pp = *dragBaseParams_;
                    paramModifier_(pp, scrubberPos_);
                    dragCallback_(pp, scrubberPos_);
                }
                // Keep timer alive while dragging
                return isDragging_;
            }, 150);
        }
        return true;
    }
    return false;
}
