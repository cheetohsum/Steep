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

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

#include <gtkmm.h>

#include "../rtengine/procparams.h"

class Thumbnail;

class PreviewStrip : public Gtk::DrawingArea {
public:
    // Callback to modify a copy of ProcParams for interpolation value t in [-1, 1].
    // Called on background thread (for thumbnails) and main thread (for drag).
    // pp starts as a copy of currentParams; modify it in-place.
    using ParamModifier = std::function<void(rtengine::procparams::ProcParams& pp, double t)>;

    // Called on main thread when user drags or releases.
    // Receives the modified ProcParams (with ParamModifier applied) and the raw t value.
    using DragCallback = std::function<void(const rtengine::procparams::ProcParams& modified, double t)>;

    PreviewStrip();
    ~PreviewStrip() override;

    void setThumbnail(Thumbnail* thm);
    void setCurrentParams(const rtengine::procparams::ProcParams& params);
    void setParamModifier(ParamModifier mod);
    void setDragCallback(DragCallback cb);
    void setReleaseCallback(DragCallback cb);
    void regenerateThumbnails();
    void resetScrubber();

private:
    bool on_draw(const Cairo::RefPtr<Cairo::Context>& cr) override;
    bool on_button_press_event(GdkEventButton* event) override;
    bool on_button_release_event(GdkEventButton* event) override;
    bool on_motion_notify_event(GdkEventMotion* event) override;
    Gtk::SizeRequestMode get_request_mode_vfunc() const override;
    void get_preferred_height_vfunc(int& min, int& natural) const override;

    void generateThumbnailsAsync();
    void handleDrag(double x);

    static constexpr int STRIP_HEIGHT = 36;
    static constexpr int NUM_THUMBS = 6;
    static constexpr double CORNER_RADIUS = 8.0;

    Thumbnail* thumbnail_ = nullptr;
    std::shared_ptr<rtengine::procparams::ProcParams> currentParams_;
    std::shared_ptr<rtengine::procparams::ProcParams> dragBaseParams_; // snapshot at drag start
    std::vector<Glib::RefPtr<Gdk::Pixbuf>> thumbnails_;
    double scrubberPos_ = 0.0; // -1.0 to 1.0
    bool isDragging_ = false;
    bool dragPending_ = false; // throttled drag waiting to fire
    ParamModifier paramModifier_;
    DragCallback dragCallback_;
    DragCallback releaseCallback_;
    sigc::connection debounceConn_;
    sigc::connection dragThrottleConn_;
    std::shared_ptr<std::atomic<bool>> cancelToken_;
};
