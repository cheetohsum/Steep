/*
 *  This file is part of RawTherapee.
 *
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
#include <cairomm/cairomm.h>
#include "rtengine/rt_math.h"

#include "guiutils.h"

#include "options.h"
#include "rtengine/rtapp.h"
#include "rtengine/utils.h"
#include "rtengine/procparams.h"
#include "rtimage.h"
#include "rtscalable.h"
#include "multilangmgr.h"
#include "toolpanel.h"
#include "widgets/basic/adjuster.h"

#include <assert.h>

using namespace std;

namespace
{

void drawCropGuides(const Cairo::RefPtr<Cairo::Context>& cr,
                    double rectx1, double recty1, double rectx2, double recty2,
                    const rtengine::procparams::CropParams& cparams)
{
    if (cparams.guide == rtengine::procparams::CropParams::Guide::NONE) return;

    cr->set_line_width (1.0);
    cr->set_source_rgba (1.0, 1.0, 1.0, 0.618);
    cr->move_to (rectx1, recty1);
    cr->line_to (rectx2, recty1);
    cr->line_to (rectx2, recty2);
    cr->line_to (rectx1, recty2);
    cr->line_to (rectx1, recty1);
    cr->stroke ();
    cr->set_source_rgba (0.0, 0.0, 0.0, 0.618);
    cr->set_dash (std::valarray<double>({4}), 0);
    cr->move_to (rectx1, recty1);
    cr->line_to (rectx2, recty1);
    cr->line_to (rectx2, recty2);
    cr->line_to (rectx1, recty2);
    cr->line_to (rectx1, recty1);
    cr->stroke ();
    cr->set_dash (std::valarray<double>(), 0);

    if (
        cparams.guide != rtengine::procparams::CropParams::Guide::RULE_OF_DIAGONALS
        && cparams.guide != rtengine::procparams::CropParams::Guide::GOLDEN_TRIANGLE_1
        && cparams.guide != rtengine::procparams::CropParams::Guide::GOLDEN_TRIANGLE_2
    ) {
        // draw guide lines
        std::vector<double> horiz_ratios;
        std::vector<double> vert_ratios;

        switch (cparams.guide) {
            case rtengine::procparams::CropParams::Guide::NONE:
            case rtengine::procparams::CropParams::Guide::FRAME:
            case rtengine::procparams::CropParams::Guide::RULE_OF_DIAGONALS:
            case rtengine::procparams::CropParams::Guide::GOLDEN_TRIANGLE_1:
            case rtengine::procparams::CropParams::Guide::GOLDEN_TRIANGLE_2: {
                break;
            }

            case rtengine::procparams::CropParams::Guide::RULE_OF_THIRDS: {
                horiz_ratios.push_back (1.0 / 3.0);
                horiz_ratios.push_back (2.0 / 3.0);
                vert_ratios.push_back (1.0 / 3.0);
                vert_ratios.push_back (2.0 / 3.0);
                break;
            }

            case rtengine::procparams::CropParams::Guide::HARMONIC_MEANS: {
                horiz_ratios.push_back (1.0 - 0.618);
                horiz_ratios.push_back (0.618);
                vert_ratios.push_back (0.618);
                vert_ratios.push_back (1.0 - 0.618);
                break;
            }

            case rtengine::procparams::CropParams::Guide::GRID: {
                // To have even distribution, normalize it a bit
                const int longSideNumLines = 10;

                int w = rectx2 - rectx1, h = recty2 - recty1;

                if (w > longSideNumLines && h > longSideNumLines) {
                    if (w > h) {
                        for (int i = 1; i < longSideNumLines; i++) {
                            vert_ratios.push_back ((double)i / longSideNumLines);
                        }

                        int shortSideNumLines = (int)round(h * (double)longSideNumLines / w);

                        for (int i = 1; i < shortSideNumLines; i++) {
                            horiz_ratios.push_back ((double)i / shortSideNumLines);
                        }
                    } else {
                        for (int i = 1; i < longSideNumLines; i++) {
                            horiz_ratios.push_back ((double)i / longSideNumLines);
                        }

                        int shortSideNumLines = (int)round(w * (double)longSideNumLines / h);

                        for (int i = 1; i < shortSideNumLines; i++) {
                            vert_ratios.push_back ((double)i / shortSideNumLines);
                        }
                    }
                }
                break;
            }

            case rtengine::procparams::CropParams::Guide::EPASSPORT: {
                /* Official measurements do not specify exact ratios, just min/max measurements within which the eyes and chin-crown distance must lie. I averaged those measurements to produce these guides.
                 * The first horizontal guide is for the crown, the second is roughly for the nostrils, the third is for the chin.
                 * http://www.homeoffice.gov.uk/agencies-public-bodies/ips/passports/information-photographers/
                 * "(...) the measurement of the face from the bottom of the chin to the crown (ie the top of the head, not the top of the hair) is between 29mm and 34mm."
                 */
                horiz_ratios.push_back (7.0 / 45.0);
                horiz_ratios.push_back (26.0 / 45.0);
                horiz_ratios.push_back (37.0 / 45.0);
                vert_ratios.push_back (0.5);
                break;
            }

            case rtengine::procparams::CropParams::Guide::CENTERED_SQUARE: {
                const double w = rectx2 - rectx1, h = recty2 - recty1;
                double ratio = w / h;
                if (ratio < 1.0) {
                    ratio = 1.0 / ratio;
                    horiz_ratios.push_back((ratio - 1.0) / (2.0 * ratio));
                    horiz_ratios.push_back(1.0 - (ratio - 1.0) / (2.0 * ratio));
                } else {
                    vert_ratios.push_back((ratio - 1.0) / (2.0 * ratio));
                    vert_ratios.push_back(1.0 - (ratio - 1.0) / (2.0 * ratio));
                }
                break;
            }
        }

        // Horizontals
        for (size_t i = 0; i < horiz_ratios.size(); i++) {
            cr->set_source_rgba (1.0, 1.0, 1.0, 0.618);
            cr->move_to (rectx1, recty1 + round((recty2 - recty1) * horiz_ratios[i]));
            cr->line_to (rectx2, recty1 + round((recty2 - recty1) * horiz_ratios[i]));
            cr->stroke ();
            cr->set_source_rgba (0.0, 0.0, 0.0, 0.618);
            std::valarray<double> ds (1);
            ds[0] = 4;
            cr->set_dash (ds, 0);
            cr->move_to (rectx1, recty1 + round((recty2 - recty1) * horiz_ratios[i]));
            cr->line_to (rectx2, recty1 + round((recty2 - recty1) * horiz_ratios[i]));
            cr->stroke ();
            ds.resize (0);
            cr->set_dash (ds, 0);
        }

        // Verticals
        for (size_t i = 0; i < vert_ratios.size(); i++) {
            cr->set_source_rgba (1.0, 1.0, 1.0, 0.618);
            cr->move_to (rectx1 + round((rectx2 - rectx1) * vert_ratios[i]), recty1);
            cr->line_to (rectx1 + round((rectx2 - rectx1) * vert_ratios[i]), recty2);
            cr->stroke ();
            cr->set_source_rgba (0.0, 0.0, 0.0, 0.618);
            std::valarray<double> ds (1);
            ds[0] = 4;
            cr->set_dash (ds, 0);
            cr->move_to (rectx1 + round((rectx2 - rectx1) * vert_ratios[i]), recty1);
            cr->line_to (rectx1 + round((rectx2 - rectx1) * vert_ratios[i]), recty2);
            cr->stroke ();
            ds.resize (0);
            cr->set_dash (ds, 0);
        }
    } else if (cparams.guide == rtengine::procparams::CropParams::Guide::RULE_OF_DIAGONALS) {
        double corners_from[4][2];
        double corners_to[4][2];
        int mindim = min(rectx2 - rectx1, recty2 - recty1);
        corners_from[0][0] = rectx1;
        corners_from[0][1] = recty1;
        corners_to[0][0]   = rectx1 + mindim;
        corners_to[0][1]   = recty1 + mindim;
        corners_from[1][0] = rectx1;
        corners_from[1][1] = recty2;
        corners_to[1][0]   = rectx1 + mindim;
        corners_to[1][1]   = recty2 - mindim;
        corners_from[2][0] = rectx2;
        corners_from[2][1] = recty1;
        corners_to[2][0]   = rectx2 - mindim;
        corners_to[2][1]   = recty1 + mindim;
        corners_from[3][0] = rectx2;
        corners_from[3][1] = recty2;
        corners_to[3][0]   = rectx2 - mindim;
        corners_to[3][1]   = recty2 - mindim;

        for (int i = 0; i < 4; i++) {
            cr->set_source_rgba (1.0, 1.0, 1.0, 0.618);
            cr->move_to (corners_from[i][0], corners_from[i][1]);
            cr->line_to (corners_to[i][0], corners_to[i][1]);
            cr->stroke ();
            cr->set_source_rgba (0.0, 0.0, 0.0, 0.618);
            std::valarray<double> ds (1);
            ds[0] = 4;
            cr->set_dash (ds, 0);
            cr->move_to (corners_from[i][0], corners_from[i][1]);
            cr->line_to (corners_to[i][0], corners_to[i][1]);
            cr->stroke ();
            ds.resize (0);
            cr->set_dash (ds, 0);
        }
    } else if (
        cparams.guide == rtengine::procparams::CropParams::Guide::GOLDEN_TRIANGLE_1
        || cparams.guide == rtengine::procparams::CropParams::Guide::GOLDEN_TRIANGLE_2
    ) {
        // main diagonal
        if(cparams.guide == rtengine::procparams::CropParams::Guide::GOLDEN_TRIANGLE_2) {
            std::swap(rectx1, rectx2);
        }

        cr->set_source_rgba (1.0, 1.0, 1.0, 0.618);
        cr->move_to (rectx1, recty1);
        cr->line_to (rectx2, recty2);
        cr->stroke ();
        cr->set_source_rgba (0.0, 0.0, 0.0, 0.618);
        cr->set_dash (std::valarray<double>({4}), 0);
        cr->move_to (rectx1, recty1);
        cr->line_to (rectx2, recty2);
        cr->stroke ();
        cr->set_dash (std::valarray<double>(), 0);

        double height = recty2 - recty1;
        double width = rectx2 - rectx1;
        double d = sqrt(height * height + width * width);
        double alpha = asin(width / d);
        double beta = asin(height / d);
        double a = sin(beta) * height;
        double b = sin(alpha) * height;

        double x = (a * b) / height;
        double y = height - (b * (d - a)) / width;
        cr->set_source_rgba (1.0, 1.0, 1.0, 0.618);
        cr->move_to (rectx1, recty2);
        cr->line_to (rectx1 + x, recty1 + y);
        cr->stroke ();
        cr->set_source_rgba (0.0, 0.0, 0.0, 0.618);
        cr->set_dash (std::valarray<double>({4}), 0);
        cr->move_to (rectx1, recty2);
        cr->line_to (rectx1 + x, recty1 + y);
        cr->stroke ();
        cr->set_dash (std::valarray<double>(), 0);

        x = width - (a * b) / height;
        y = (b * (d - a)) / width;
        cr->set_source_rgba (1.0, 1.0, 1.0, 0.618);
        cr->move_to (rectx2, recty1);
        cr->line_to (rectx1 + x, recty1 + y);
        cr->stroke ();
        cr->set_source_rgba (0.0, 0.0, 0.0, 0.618);
        cr->set_dash (std::valarray<double>({4}), 0);
        cr->move_to (rectx2, recty1);
        cr->line_to (rectx1 + x, recty1 + y);
        cr->stroke ();
        cr->set_dash (std::valarray<double>(), 0);
    }
}

}  // namespace

IdleRegister::~IdleRegister()
{
    destroy();
}

void IdleRegister::add(std::function<bool ()> function, gint priority)
{
    const auto dispatch =
        [](gpointer data) -> gboolean
        {
            DataWrapper* const data_wrapper = static_cast<DataWrapper*>(data);

            if (!data_wrapper->function()) {
                data_wrapper->self->mutex.lock();
                data_wrapper->self->ids.erase(data_wrapper);
                data_wrapper->self->mutex.unlock();

                delete data_wrapper;
                return FALSE;
            }

            return TRUE;
        };

    DataWrapper* const data_wrapper = new DataWrapper{
        this,
        std::move(function)
    };

    mutex.lock();
    ids[data_wrapper] = gdk_threads_add_idle_full(priority, dispatch, data_wrapper, nullptr);
    mutex.unlock();
}

void IdleRegister::destroy()
{
    mutex.lock();
    for (const auto& id : ids) {
        g_source_remove(id.second);
        delete id.first;
    }
    ids.clear();
    mutex.unlock();
}

BlockAdjusterEvents::BlockAdjusterEvents(Adjuster* adjuster) : adj(adjuster)
{
    if (adj) {
        adj->block(true);
    }
}

BlockAdjusterEvents::~BlockAdjusterEvents()
{
    if (adj) {
        adj->block(false);
    }
}

DisableListener::DisableListener(ToolPanel* panelToDisable) : panel(panelToDisable)
{
    if (panel) {
        panel->disableListener();
    }
}

DisableListener::~DisableListener()
{
    if (panel) {
        panel->enableListener();
    }
}

Glib::ustring escapeHtmlChars(const Glib::ustring &src)
{

    // Sources chars to be escaped
    static const Glib::ustring srcChar("&<>");

    // Destination strings, in the same order than the source
    static std::vector<Glib::ustring> dstChar(3);
    dstChar.at(0) = "&amp;";
    dstChar.at(1) = "&lt;";
    dstChar.at(2) = "&gt;";

    // Copying the original string, that will be modified
    Glib::ustring dst(src);

    // Iterating all chars of the copy of the source string
    for (size_t i = 0; i < dst.length();) {

        // Looking out if it's part of the characters to be escaped
        size_t pos = srcChar.find_first_of(dst.at(i), 0);

        if (pos != Glib::ustring::npos) {
            // If yes, replacing the char in the destination string
            dst.replace(i, 1, dstChar.at(pos));
            // ... and going forward  by the length of the new string
            i += dstChar.at(pos).length();
        } else {
            ++i;
        }
    }

    return dst;
}

Gtk::EventBox* createEdgeGrip (bool vertical,
                               const Glib::ustring& tooltipMarkup,
                               const std::function<void ()>& onClick)
{
    // Styling (transparent at rest, tapered highlight on hover) lives in
    // themes/common/widgets.css (#EdgeGripV / #EdgeGripH).
    auto* grip = Gtk::manage (new Gtk::EventBox ());
    grip->set_name (vertical ? "EdgeGripV" : "EdgeGripH");
    grip->set_visible_window (true);
    grip->add_events (Gdk::BUTTON_PRESS_MASK | Gdk::ENTER_NOTIFY_MASK | Gdk::LEAVE_NOTIFY_MASK);
    grip->set_tooltip_markup (tooltipMarkup);

    // An EventBox does not prelight by itself, and the highlight waits for
    // the pointer to settle: brushing past on the way to the canvas should
    // not flash a bar.
    auto hoverConn = std::make_shared<sigc::connection>();

    grip->signal_enter_notify_event().connect (
        [grip, hoverConn](GdkEventCrossing*) -> bool {
            hoverConn->disconnect();
            *hoverConn = Glib::signal_timeout().connect (
                sigc::track_obj ([grip]() -> bool {
                    grip->set_state_flags (Gtk::STATE_FLAG_PRELIGHT, false);
                    return false;
                }, *grip),
                300);
            return false;
        });

    const auto clearHover = [grip, hoverConn]() {
        hoverConn->disconnect();
        grip->unset_state_flags (Gtk::STATE_FLAG_PRELIGHT);
    };

    grip->signal_leave_notify_event().connect (
        [clearHover](GdkEventCrossing*) -> bool {
            clearHover();
            return false;
        });
    grip->signal_unmap().connect (clearHover);

    grip->signal_button_press_event().connect (
        [onClick](GdkEventButton* event) -> bool {
            if (event && event->button == 1 && event->type == GDK_BUTTON_PRESS) {
                onClick();
                return true;
            }

            return false;
        });

    return grip;
}

void setExpandAlignProperties(Gtk::Widget *widget, bool hExpand, bool vExpand, enum Gtk::Align hAlign, enum Gtk::Align vAlign)
{
    widget->set_hexpand(hExpand);
    widget->set_vexpand(vExpand);
    widget->set_halign(hAlign);
    widget->set_valign(vAlign);
}

Gtk::Border getPadding(const Glib::RefPtr<Gtk::StyleContext> style)
{
    Gtk::Border padding;
    if (!style) {
        return padding;
    }

    padding = style->get_padding();

    if (RTScalable::getGlobalScale() > 1.0) {
        // Scale pixel border size based on DPI and Scale
        padding.set_left(RTScalable::scalePixelSize(padding.get_left()));
        padding.set_right(RTScalable::scalePixelSize(padding.get_right()));
        padding.set_top(RTScalable::scalePixelSize(padding.get_top()));
        padding.set_bottom(RTScalable::scalePixelSize(padding.get_bottom()));
    }

    return padding;
}

namespace
{
// GUI thread only — themeColor/themeColorCacheInvalidate are never called from
// worker threads, so no locking.
std::map<std::string, Gdk::RGBA> themeColorCache;
}

Gdk::RGBA themeColor(const Glib::RefPtr<Gtk::StyleContext>& ctx, const char* name, const Gdk::RGBA& fallback)
{
    const auto cached = themeColorCache.find(name);
    if (cached != themeColorCache.end()) {
        return cached->second;
    }

    Gdk::RGBA color = fallback;
    if (ctx) {
        Gdk::RGBA looked;
        if (ctx->lookup_color(name, looked)) {
            color = looked;
        }
    }

    themeColorCache.emplace(name, color);
    return color;
}

Gdk::RGBA themeColor(const Gtk::Widget& widget, const char* name, const Gdk::RGBA& fallback)
{
    // Read-only lookup; get_style_context() has no const overload returning
    // a mutable ref, so cast rather than force every caller to be non-const.
    return themeColor(const_cast<Gtk::Widget&>(widget).get_style_context(), name, fallback);
}

void themeColorCacheInvalidate()
{
    themeColorCache.clear();
}

bool removeIfThere (Gtk::Container* cont, Gtk::Widget* w, bool increference)
{

    Glib::ListHandle<Gtk::Widget*> list = cont->get_children ();
    Glib::ListHandle<Gtk::Widget*>::iterator i = list.begin ();

    for (; i != list.end() && *i != w; ++i);

    if (i != list.end()) {
        if (increference) {
            w->reference ();
        }

        cont->remove (*w);
        return true;
    } else {
        return false;
    }
}

bool confirmOverwrite (Gtk::Window& parent, const std::string& filename)
{
    bool safe = true;

    if (Glib::file_test (filename, Glib::FILE_TEST_EXISTS)) {
        Glib::ustring msg_ = Glib::ustring ("<b>\"") + escapeHtmlChars(Glib::path_get_basename (filename)) + "\": "
                             + M("MAIN_MSG_ALREADYEXISTS") + "</b>\n" + M("MAIN_MSG_QOVERWRITE");
        Gtk::MessageDialog msgd (parent, msg_, true, Gtk::MESSAGE_WARNING, Gtk::BUTTONS_YES_NO, true);
        safe = (msgd.run () == Gtk::RESPONSE_YES);
    }

    return safe;
}

void writeFailed (Gtk::Window& parent, const std::string& filename)
{
    Glib::ustring msg_ = Glib::ustring::compose(M("MAIN_MSG_WRITEFAILED"), escapeHtmlChars(filename));
    Gtk::MessageDialog msgd (parent, msg_, true, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
    msgd.run ();
}

void drawCrop (const Cairo::RefPtr<Cairo::Context>& cr,
               double imx, double imy, double imw, double imh,
               double clipWidth, double clipHeight,
               double startx, double starty, double scale,
               const rtengine::procparams::CropParams& cparams,
               bool drawGuide, bool useBgColor, bool fullImageVisible,
               bool solidOverlay)
{
    cr->save();

    cr->set_line_width(0.0);
    cr->rectangle(imx, imy, clipWidth, clipHeight);
    cr->clip();

    double c1x = (cparams.x - startx) * scale;
    double c1y = (cparams.y - starty) * scale;
    double c2x = (cparams.x + cparams.w - startx) * scale - (fullImageVisible ? 0.0 : 1.0);
    double c2y = (cparams.y + cparams.h - starty) * scale - (fullImageVisible ? 0.0 : 1.0);

    const auto& options = App::get().options();
    // crop overlay color, linked with crop windows background
    if (solidOverlay) {
        // Fully opaque overlay when not in crop editing mode
        if (options.bgcolor == 0) {
            cr->set_source_rgba (0, 0, 0, 1.0);
        } else if (options.bgcolor == 1) {
            cr->set_source_rgb (0, 0, 0);
        } else if (options.bgcolor == 2) {
            cr->set_source_rgb (1, 1, 1);
        } else if (options.bgcolor == 3) {
            cr->set_source_rgb (0.467, 0.467, 0.467);
        }
    } else if (options.bgcolor == 0 || !useBgColor) {
        cr->set_source_rgba (options.cutOverlayBrush[0], options.cutOverlayBrush[1], options.cutOverlayBrush[2], options.cutOverlayBrush[3]);
    } else if (options.bgcolor == 1) {
        cr->set_source_rgb (0, 0, 0);
    } else if (options.bgcolor == 2) {
        cr->set_source_rgb (1, 1, 1);
    } else if (options.bgcolor == 3) {
        cr->set_source_rgb (0.467, 0.467, 0.467);
    }

    cr->rectangle (imx, imy, imw + 0.5, round(c1y) + 0.5);
    cr->rectangle (imx, round(imy + c2y) + 0.5, imw + 0.5, round(imh - c2y) + 0.5);
    cr->rectangle (imx, round(imy + c1y) + 0.5, round(c1x) + 0.5, round(c2y - c1y + 1) + 0.5);
    cr->rectangle (round(imx + c2x) + 0.5, round(imy + c1y) + 0.5, round(imw - c2x) + 0.5, round(c2y - c1y + 1) + 0.5);
    cr->fill ();

    cr->restore();

    // rectangle around the cropped area and guides
    if (cparams.guide != rtengine::procparams::CropParams::Guide::NONE && drawGuide) {
        cr->save();
        cr->rectangle(imx, imy, clipWidth, clipHeight);
        cr->clip();

        double rectx1 = round(c1x) + imx + 0.5;
        double recty1 = round(c1y) + imy + 0.5;
        double rectx2 = round(c2x) + imx + 0.5;
        double recty2 = round(c2y) + imy + 0.5;

        // Clamp guide rect to visible image area on all four sides
        rectx1 = max(rectx1, imx + 0.5);
        recty1 = max(recty1, imy + 0.5);
        rectx2 = min(rectx2, imx + clipWidth - 0.5);
        recty2 = min(recty2, imy + clipHeight - 0.5);

        if (rectx2 > rectx1 && recty2 > recty1) {
            drawCropGuides(cr, rectx1, recty1, rectx2, recty2, cparams);
        }

        cr->restore();
    }
}

/*
bool ExpanderBox::on_draw(const ::Cairo::RefPtr< Cairo::Context> &cr) {

    if (!options.useSystemTheme) {
        Glib::RefPtr<Gdk::Window> window = get_window();
        Glib::RefPtr<Gtk::StyleContext> style = get_style_context ();

        int x_, y_, w_, h_;
        window->get_geometry(x_, y_, w_, h_);
        double x = 0.;
        double y = 0.;
        double w = double(w_);
        double h = double(h_);

        cr->set_antialias (Cairo::ANTIALIAS_NONE);

        // draw a frame
        style->render_background(cr, x, y, w, h);
        / *
        cr->set_line_width (1.0);
        Gdk::RGBA c = style->get_color (Gtk::STATE_FLAG_NORMAL);
        cr->set_source_rgb (c.get_red(), c.get_green(), c.get_blue());
        cr->move_to(x+0.5, y+0.5);
        cr->line_to(x+w, y+0.5);
        cr->line_to(x+w, y+h);
        cr->line_to(x+0.5, y+h);
        cr->line_to(x+0.5, y+0.5);
        cr->stroke ();
        * /
    }
    return Gtk::EventBox::on_draw(cr);
}
*/

ExpanderBox::ExpanderBox( Gtk::Container *p): pC(p)
{
    set_name ("ExpanderBox");
    // No own GDK window — background comes from child CSS, not EventBox.
    set_visible_window(false);
//GTK318
#if GTK_MAJOR_VERSION == 3 && GTK_MINOR_VERSION < 20
    set_border_width(2);
#endif
//GTK318
}

void ExpanderBox::setLevel(int level)
{
    if (level <= 1) {
        set_name("ExpanderBox");
    } else if (level == 2) {
        set_name("ExpanderBox2");
    } else if (level >= 3) {
        set_name("ExpanderBox3");
    }
}

void ExpanderBox::show_all()
{
    // ask childs to show themselves, but not us (remain unchanged)
    Gtk::Container::show_all_children(true);
}

void ExpanderBox::showBox()
{
    Gtk::EventBox::show();
}

void ExpanderBox::hideBox()
{
    Gtk::EventBox::hide();
}

MyExpander::MyExpander(bool useEnabled, Gtk::Widget* titleWidget) :
    inconsistentImage("power-inconsistent-small"),
    enabledImage("power-on-small"),
    disabledImage("power-off-small"),
    openedImage("expander-open-small"),
    closedImage("expander-closed-small"),
    enabled(false), inconsistent(false), flushEvent(false), expBox(nullptr),
    child(nullptr), headerWidget(nullptr), statusImage(nullptr),
    label(nullptr), useEnabled(useEnabled),
    summaryBox(nullptr), expandArrow(nullptr), expandable_(true), flatMode_(false)
{
    set_orientation(Gtk::ORIENTATION_VERTICAL);
    set_spacing(0);
    set_name("MyExpander");
    set_can_focus(false);
    setExpandAlignProperties(this, true, false, Gtk::ALIGN_FILL, Gtk::ALIGN_FILL);

    headerHBox = Gtk::manage( new Gtk::Box());
    headerHBox->set_can_focus(false);
    setExpandAlignProperties(headerHBox, true, false, Gtk::ALIGN_FILL, Gtk::ALIGN_FILL);

    if (useEnabled) {
        get_style_context()->add_class("OnOff");
        statusImage = Gtk::manage(new RTImage(disabledImage));
        imageEvBox = Gtk::manage(new Gtk::EventBox());
        imageEvBox->set_name("MyExpanderStatus");
        imageEvBox->add(*statusImage);
        imageEvBox->set_above_child(true);
        imageEvBox->signal_button_release_event().connect( sigc::mem_fun(this, & MyExpander::on_enabled_change) );
        imageEvBox->signal_enter_notify_event().connect( sigc::mem_fun(this, & MyExpander::on_enter_leave_enable), false );
        imageEvBox->signal_leave_notify_event().connect( sigc::mem_fun(this, & MyExpander::on_enter_leave_enable), false );
        headerHBox->pack_start(*imageEvBox, Gtk::PACK_SHRINK, 0);
    } else {
        get_style_context()->add_class("Fold");
        statusImage = Gtk::manage(new RTImage(openedImage));
        headerHBox->pack_start(*statusImage, Gtk::PACK_SHRINK, 0);
    }

    statusImage->set_can_focus(false);

    if (titleWidget) {
        setExpandAlignProperties(titleWidget, true, false, Gtk::ALIGN_FILL, Gtk::ALIGN_FILL);
        headerHBox->pack_start(*titleWidget, Gtk::PACK_EXPAND_WIDGET, 0);
        headerWidget = titleWidget;
    }

    if (useEnabled) {
        expandArrow = Gtk::manage(new RTImage(closedImage));
        expandArrow->set_can_focus(false);
        headerHBox->pack_end(*expandArrow, Gtk::PACK_SHRINK, 0);
    }

    titleEvBox = Gtk::manage(new Gtk::EventBox());
    titleEvBox->set_name("MyExpanderTitle");
    titleEvBox->set_border_width(0);
    titleEvBox->add(*headerHBox);
    titleEvBox->set_above_child(false);  // this is the key! By making it below the child, they will get the events first.
    titleEvBox->set_can_focus(false);

    pack_start(*titleEvBox, Gtk::PACK_EXPAND_WIDGET, 0);

    summaryBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
    summaryBox->set_spacing(0);
    summaryBox->set_border_width(0);
    summaryBox->set_hexpand(true);
    summaryBox->set_halign(Gtk::ALIGN_FILL);
    summaryBox->get_style_context()->add_class("MyExpanderSummary");
    summaryBox->show();
    pack_start(*summaryBox, Gtk::PACK_SHRINK, 0);

    updateStyle();

    titleEvBox->signal_button_release_event().connect( sigc::mem_fun(this, & MyExpander::on_toggle) );
    titleEvBox->signal_enter_notify_event().connect( sigc::mem_fun(this, & MyExpander::on_enter_leave_title), false);
    titleEvBox->signal_leave_notify_event().connect( sigc::mem_fun(this, & MyExpander::on_enter_leave_title), false);
}

MyExpander::MyExpander(bool useEnabled, Glib::ustring titleLabel) :
    inconsistentImage("power-inconsistent-small"),
    enabledImage("power-on-small"),
    disabledImage("power-off-small"),
    openedImage("expander-open-small"),
    closedImage("expander-closed-small"),
    enabled(false), inconsistent(false), flushEvent(false), expBox(nullptr),
    child(nullptr), headerWidget(nullptr),
    label(nullptr), useEnabled(useEnabled),
    summaryBox(nullptr), expandArrow(nullptr), expandable_(true), flatMode_(false)
{
    set_orientation(Gtk::ORIENTATION_VERTICAL);
    set_spacing(0);
    set_name("MyExpander");
    set_can_focus(false);
    setExpandAlignProperties(this, true, false, Gtk::ALIGN_FILL, Gtk::ALIGN_FILL);

    headerHBox = Gtk::manage( new Gtk::Box());
    headerHBox->set_can_focus(false);
    setExpandAlignProperties(headerHBox, true, false, Gtk::ALIGN_FILL, Gtk::ALIGN_FILL);

    if (useEnabled) {
        get_style_context()->add_class("OnOff");
        statusImage = Gtk::manage(new RTImage(disabledImage));
        imageEvBox = Gtk::manage(new Gtk::EventBox());
        imageEvBox->set_name("MyExpanderStatus");
        imageEvBox->add(*statusImage);
        imageEvBox->set_above_child(true);
        imageEvBox->signal_button_release_event().connect( sigc::mem_fun(this, & MyExpander::on_enabled_change) );
        imageEvBox->signal_enter_notify_event().connect( sigc::mem_fun(this, & MyExpander::on_enter_leave_enable), false );
        imageEvBox->signal_leave_notify_event().connect( sigc::mem_fun(this, & MyExpander::on_enter_leave_enable), false );
        headerHBox->pack_start(*imageEvBox, Gtk::PACK_SHRINK, 0);
    } else {
        get_style_context()->add_class("Fold");
        statusImage = Gtk::manage(new RTImage(openedImage));
        headerHBox->pack_start(*statusImage, Gtk::PACK_SHRINK, 0);
    }

    statusImage->set_can_focus(false);

    label = Gtk::manage(new Gtk::Label());
    setExpandAlignProperties(label, true, false, Gtk::ALIGN_START, Gtk::ALIGN_CENTER);
    label->set_markup(escapeHtmlChars(titleLabel));
    headerHBox->pack_start(*label, Gtk::PACK_EXPAND_WIDGET, 0);

    if (useEnabled) {
        expandArrow = Gtk::manage(new RTImage(closedImage));
        expandArrow->set_can_focus(false);
        headerHBox->pack_end(*expandArrow, Gtk::PACK_SHRINK, 0);
    }

    titleEvBox = Gtk::manage(new Gtk::EventBox());
    titleEvBox->set_name("MyExpanderTitle");
    titleEvBox->set_border_width(0);
    titleEvBox->add(*headerHBox);
    titleEvBox->set_above_child(false);  // this is the key! By make it below the child, they will get the events first.
    titleEvBox->set_can_focus(false);

    pack_start(*titleEvBox, Gtk::PACK_EXPAND_WIDGET, 0);

    summaryBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
    summaryBox->set_spacing(0);
    summaryBox->set_border_width(0);
    summaryBox->set_hexpand(true);
    summaryBox->set_halign(Gtk::ALIGN_FILL);
    summaryBox->get_style_context()->add_class("MyExpanderSummary");
    summaryBox->show();
    pack_start(*summaryBox, Gtk::PACK_SHRINK, 0);

    updateStyle();

    titleEvBox->signal_button_release_event().connect( sigc::mem_fun(this, & MyExpander::on_toggle));
    titleEvBox->signal_enter_notify_event().connect( sigc::mem_fun(this, & MyExpander::on_enter_leave_title), false);
    titleEvBox->signal_leave_notify_event().connect( sigc::mem_fun(this, & MyExpander::on_enter_leave_title), false);
}

bool MyExpander::on_enter_leave_title (GdkEventCrossing* event)
{
    if (is_sensitive()) {
        if (event->type == GDK_ENTER_NOTIFY) {
            titleEvBox->set_state(Gtk::STATE_PRELIGHT);
            queue_draw();
        } else if (event->type == GDK_LEAVE_NOTIFY) {
            titleEvBox->set_state(Gtk::STATE_NORMAL);
            queue_draw();
        }
    }

    return true;
}

bool MyExpander::on_enter_leave_enable (GdkEventCrossing* event)
{
    if (is_sensitive()) {
        if (event->type == GDK_ENTER_NOTIFY) {
            imageEvBox->set_state(Gtk::STATE_PRELIGHT);
            queue_draw();
        } else if (event->type == GDK_LEAVE_NOTIFY) {
            imageEvBox->set_state(Gtk::STATE_NORMAL);
            queue_draw();
        }
    }

    return true;
}

void MyExpander::updateStyle()
{
    updateVScrollbars(App::get().options().hideTPVScrollbar);

//GTK318
#if GTK_MAJOR_VERSION == 3 && GTK_MINOR_VERSION < 20
    headerHBox->set_spacing(2);
    headerHBox->set_border_width(1);
    set_spacing(0);
    set_border_width(0);
#endif
//GTK318
}

void MyExpander::updateVScrollbars(bool hide)
{
    if (hide) {
        get_style_context()->remove_class("withScrollbar");
    } else {
        get_style_context()->add_class("withScrollbar");
    }
}

void MyExpander::setLevel (int level)
{
    if (expBox) {
        expBox->setLevel(level);
    }
}

void MyExpander::setLabel (Glib::ustring newLabel)
{
    if (label) {
        label->set_markup(escapeHtmlChars(newLabel));
    }
}

void MyExpander::setLabel (Gtk::Widget *newWidget)
{
    if (headerWidget) {
        removeIfThere(headerHBox, headerWidget, false);
        headerHBox->pack_start(*newWidget, Gtk::PACK_EXPAND_WIDGET, 0);
    }
}

bool MyExpander::get_inconsistent()
{
    return inconsistent;
}

void MyExpander::set_inconsistent(bool isInconsistent)
{
    if (inconsistent != isInconsistent) {
        inconsistent = isInconsistent;

        if (useEnabled) {
            if (isInconsistent) {
                statusImage->set_from_icon_name(inconsistentImage);
            } else {
                if (enabled) {
                    statusImage->set_from_icon_name(enabledImage);
                    get_style_context()->add_class("enabledTool");
                } else {
                    statusImage->set_from_icon_name(disabledImage);
                    get_style_context()->remove_class("enabledTool");
                }
            }
        }

    }
}

bool MyExpander::getUseEnabled()
{
    return useEnabled;
}

bool MyExpander::getEnabled()
{
    return enabled;
}

void MyExpander::setEnabled(bool isEnabled)
{
    if (isEnabled != enabled) {
        if (useEnabled) {
            if (enabled) {
                enabled = false;

                if (!inconsistent) {
                    statusImage->set_from_icon_name(disabledImage);
                    get_style_context()->remove_class("enabledTool");
                    message.emit();
                }
            } else {
                enabled = true;

                if (!inconsistent) {
                    statusImage->set_from_icon_name(enabledImage);
                    get_style_context()->add_class("enabledTool");
                    message.emit();
                }
            }
        }
    }
}

void MyExpander::setEnabledTooltipMarkup(Glib::ustring tooltipMarkup)
{
    if (useEnabled) {
        statusImage->set_tooltip_markup(tooltipMarkup);
    }
}

void MyExpander::setEnabledTooltipText(Glib::ustring tooltipText)
{
    if (useEnabled) {
        statusImage->set_tooltip_text(tooltipText);
    }
}

void MyExpander::setExpandable(bool canExpand)
{
    expandable_ = canExpand;
    if (!canExpand) {
        if (expandArrow) {
            expandArrow->hide();
        }
        if (!useEnabled) {
            statusImage->hide();
        }
        if (expBox) {
            expBox->hideBox();
        }
    } else {
        if (expandArrow) {
            expandArrow->show();
        }
        if (!useEnabled) {
            statusImage->show();
        }
    }
}

void MyExpander::hideHeader()
{
    // Hide the title row but keep the body visible.
    titleEvBox->set_no_show_all(true);
    titleEvBox->hide();

    // Force the enabled state directly (without emitting signal_enabled_toggled)
    if (useEnabled && !enabled) {
        enabled = true;
        statusImage->set_from_icon_name(enabledImage);
        get_style_context()->add_class("enabledTool");
    }

    // Show the body. ExpanderBox overrides show() to a no-op and
    // show_all() to only show children. We need:
    // 1. Clear set_no_show_all so future show_all from parents will work
    // 2. showBox() to actually show the EventBox via Gtk::EventBox::show()
    // 3. show_all() to recursively show all children inside
    if (expBox) {
        expBox->set_no_show_all(false);
        expBox->showBox();   // shows the EventBox container itself
        expBox->show_all();  // shows children (content widgets)
    }
}

void MyExpander::setFlatMode(bool flat)
{
    flatMode_ = flat;
    if (!flat) return;

    get_style_context()->add_class("flat-mode");

    // Hide the entire header row (tool name label + all icons)
    titleEvBox->set_no_show_all(true);
    titleEvBox->hide();

    // Force expanded (content always visible)
    set_expanded(true);

    // Allow parent show_all() to propagate into the body.
    // Force the ExpanderBox EventBox and its child to be fully
    // transparent via a high-priority inline CSS provider.
    if (expBox) {
        expBox->set_no_show_all(false);
        expBox->set_visible_window(false);
    }

    // Force enabled
    if (useEnabled) {
        setEnabled(true);
    }
}

void MyExpander::collapseDetail()
{
    if (expBox) {
        expBox->hideBox();
        expBox->set_no_show_all(true);
    }
}

void MyExpander::set_expanded( bool expanded )
{
    if (!expBox) {
        return;
    }

    // Flat mode panels are always expanded — ignore collapse requests
    if (flatMode_ && !expanded) {
        return;
    }

    if (!useEnabled) {
        if (expanded ) {
            statusImage->set_from_icon_name(openedImage);
        } else {
            statusImage->set_from_icon_name(closedImage);
        }
    }

    if (expandArrow) {
        if (expanded) {
            expandArrow->set_from_icon_name(openedImage);
        } else {
            expandArrow->set_from_icon_name(closedImage);
        }
    }

    if (expanded) {
        expBox->showBox();
    } else {
        expBox->hideBox();
    }
}

bool MyExpander::get_expanded()
{
    return expBox ? expBox->get_visible() : false;
}

void MyExpander::add  (Gtk::Container& widget, bool setChild)
{
    if(setChild) {
        child = &widget;
    }
    expBox = Gtk::manage (new ExpanderBox (child));
    expBox->add (widget);
    pack_start(*expBox, Gtk::PACK_SHRINK, 0);
    widget.show();
    expBox->hideBox();
    expBox->set_no_show_all(true);  // Prevent parent show_all() from expanding
}

bool MyExpander::on_toggle(GdkEventButton* event)
{
    if (flatMode_) return false;

    if (flushEvent) {
        flushEvent = false;
        return false;
    }

    if (!expandable_) {
        return false;
    }

    if (!expBox || event->button != 1) {
        return false;
    }

    bool isVisible = expBox->is_visible();

    if (!useEnabled) {
        if (isVisible) {
            statusImage->set_from_icon_name(closedImage);
        } else {
            statusImage->set_from_icon_name(openedImage);
        }
    }

    if (expandArrow) {
        if (isVisible) {
            expandArrow->set_from_icon_name(closedImage);
        } else {
            expandArrow->set_from_icon_name(openedImage);
        }
    }

    if (isVisible) {
        expBox->hideBox();
    } else {
        expBox->showBox();
    }

    return false;
}

// used to connect a function to the enabled_toggled signal
MyExpander::type_signal_enabled_toggled MyExpander::signal_enabled_toggled()
{
    return message;
}

// internal use ; when the user clicks on the toggle button, it calls this method that will emit an enabled_change event
bool MyExpander::on_enabled_change(GdkEventButton* event)
{
    if (flatMode_) return false;

    if (event->button == 1) {
        if (enabled) {
            enabled = false;
            statusImage->set_from_icon_name(disabledImage);
            get_style_context()->remove_class("enabledTool");
        } else {
            enabled = true;
            statusImage->set_from_icon_name(enabledImage);
            get_style_context()->add_class("enabledTool");
        }

        message.emit();
        flushEvent = true;
    }

    return false;
}

/*
 *
 * Derived class of some widgets to properly handle the scroll wheel ;
 * the user has to use the Shift key to be able to change the widget's value,
 * otherwise the mouse wheel will scroll the editor's tabs content.
 *
 */
MyScrolledWindow::MyScrolledWindow ()
{
    set_hexpand(true);
    set_halign(Gtk::ALIGN_FILL);
    set_overlay_scrolling(false);
}

bool MyScrolledWindow::on_scroll_event (GdkEventScroll* event)
{
    if (!App::get().options().hideTPVScrollbar) {
        Gtk::ScrolledWindow::on_scroll_event (event);
        return true;
    }

    Glib::RefPtr<Gtk::Adjustment> adjust = get_vadjustment();
    Gtk::Scrollbar *scroll = get_vscrollbar();

    if (adjust && scroll) {
        const double upperBound = adjust->get_upper();
        const double lowerBound = adjust->get_lower();
        double value = adjust->get_value();
        double step  = adjust->get_step_increment();

        if (event->direction == GDK_SCROLL_DOWN) {
            const double value2 = rtengine::min<double>(value + step, upperBound);

            if (value2 != value) {
                scroll->set_value(value2);
            }
        } else if (event->direction == GDK_SCROLL_UP) {
            const double value2 = rtengine::max<double>(value - step, lowerBound);

            if (value2 != value) {
                scroll->set_value(value2);
            }
        } else if (event->direction == GDK_SCROLL_SMOOTH) {
            const double value2 = rtengine::LIM<double>(value + event->delta_y * step, lowerBound, upperBound);

            if (value2 != value) {
                scroll->set_value(value2);
            }
        }
    }

    return true;
}

void MyScrolledWindow::get_preferred_width_vfunc (int &minimum_width, int &natural_width) const
{
    natural_width = minimum_width = RTScalable::scalePixelSize(100);
}

void MyScrolledWindow::get_preferred_height_vfunc (int &minimum_height, int &natural_height) const
{
    if (get_propagate_natural_height()) {
        Gtk::ScrolledWindow::get_preferred_height_vfunc(minimum_height, natural_height);
    } else {
        natural_height = minimum_height = RTScalable::scalePixelSize(50);
    }
}

void MyScrolledWindow::get_preferred_height_for_width_vfunc (int width, int &minimum_height, int &natural_height) const
{
    if (get_propagate_natural_height()) {
        Gtk::ScrolledWindow::get_preferred_height_for_width_vfunc(width, minimum_height, natural_height);
    } else {
        natural_height = minimum_height = RTScalable::scalePixelSize(50);
    }
}

/*
 *
 * Derived class of some widgets to properly handle the scroll wheel ;
 * the user has to use the Shift key to be able to change the widget's value,
 * otherwise the mouse wheel will scroll the toolbar.
 *
 */
MyScrolledToolbar::MyScrolledToolbar ()
{
    set_policy (Gtk::POLICY_EXTERNAL, Gtk::POLICY_NEVER);
    get_style_context()->add_class("scrollableToolbar");

    // Works fine with Gtk 3.22, but a custom made get_preferred_height had to be created as a workaround
    // taken from the official Gtk3.22 source code
    //set_propagate_natural_height(true);
}

bool MyScrolledToolbar::on_scroll_event (GdkEventScroll* event)
{
    Glib::RefPtr<Gtk::Adjustment> adjust = get_hadjustment();
    Gtk::Scrollbar *scroll = get_hscrollbar();

    if (adjust && scroll) {
        const double upperBound = adjust->get_upper();
        const double lowerBound = adjust->get_lower();
        double value = adjust->get_value();
        double step  = adjust->get_step_increment() * 2;
        double value2 = 0.;

//        printf("MyScrolledToolbar::on_scroll_event / delta_x=%.5f, delta_y=%.5f, direction=%d, type=%d, send_event=%d\n",
//                event->delta_x, event->delta_y, (int)event->direction, (int)event->type, event->send_event);

        if (event->direction == GDK_SCROLL_DOWN) {
            value2 = rtengine::min<double>(value + step, upperBound);
            if (value2 != value) {
                scroll->set_value(value2);
            }
        } else if (event->direction == GDK_SCROLL_UP) {
            value2 = rtengine::max<double>(value - step, lowerBound);
            if (value2 != value) {
                scroll->set_value(value2);
            }
        } else if (event->direction == GDK_SCROLL_SMOOTH) {
            if (event->delta_x) {  // if the user use a pad, it can scroll horizontally
                value2 = rtengine::LIM<double>(value + (event->delta_x > 0 ? 30 : -30), lowerBound, upperBound);
            } else if (event->delta_y) {
                value2 = rtengine::LIM<double>(value + (event->delta_y > 0 ? 30 : -30), lowerBound, upperBound);
            }
            if (value2 != value) {
                scroll->set_value(value2);
            }
        }
    }

    return true;
}

void MyScrolledToolbar::get_preferred_height_vfunc (int &minimumHeight, int &naturalHeight) const
{
    int currMinHeight = 0;
    int currNatHeight = 0;
    std::vector<const Widget*> childs = get_children();
    minimumHeight = naturalHeight = 0;

    for (auto child : childs)
    {
        if(child->is_visible()) {
            child->get_preferred_height(currMinHeight, currNatHeight);
            minimumHeight = rtengine::max(currMinHeight, minimumHeight);
            naturalHeight = rtengine::max(currNatHeight, naturalHeight);
        }
    }
}

MyComboBoxText::MyComboBoxText (bool has_entry) : Gtk::ComboBoxText(has_entry)
{
    minimumWidth = naturalWidth = RTScalable::scalePixelSize(70);
    Gtk::CellRendererText* cellRenderer = dynamic_cast<Gtk::CellRendererText*>(get_first_cell());
    cellRenderer->property_ellipsize() = Pango::ELLIPSIZE_MIDDLE;
    add_events(Gdk::SCROLL_MASK|Gdk::SMOOTH_SCROLL_MASK);
}

bool MyComboBoxText::on_scroll_event (GdkEventScroll* event)
{

//    printf("MyComboboxText::on_scroll_event / delta_x=%.5f, delta_y=%.5f, direction=%d, type=%d, send_event=%d\n",
//            event->delta_x, event->delta_y, (int)event->direction, (int)event->type, event->send_event);
    // If Shift is pressed, the widget is modified
    if (event->state & GDK_SHIFT_MASK) {
        Gtk::ComboBoxText::on_scroll_event(event);
        return true;
    }

    // ... otherwise the scroll event is sent back to an upper level
    return false;
}

void MyComboBoxText::setPreferredWidth (int minimum_width, int natural_width)
{
    if (natural_width == -1 && minimum_width == -1) {
        naturalWidth = minimumWidth = RTScalable::scalePixelSize(70);
    } else if (natural_width == -1) {
        naturalWidth =  minimumWidth = minimum_width;
    } else if (minimum_width == -1) {
        naturalWidth = natural_width;
        minimumWidth = rtengine::max(naturalWidth / 2, RTScalable::scalePixelSize(20));
        minimumWidth = rtengine::min(naturalWidth, minimumWidth);
    } else {
        naturalWidth = natural_width;
        minimumWidth = minimum_width;
    }
}

void MyComboBoxText::get_preferred_width_vfunc (int &minimum_width, int &natural_width) const
{
    natural_width = rtengine::max(naturalWidth, RTScalable::scalePixelSize(10));
    minimum_width = rtengine::max(minimumWidth, RTScalable::scalePixelSize(10));
}

void MyComboBoxText::get_preferred_width_for_height_vfunc (int height, int &minimum_width, int &natural_width) const
{
    natural_width = rtengine::max(naturalWidth, RTScalable::scalePixelSize(10));
    minimum_width = rtengine::max(minimumWidth, RTScalable::scalePixelSize(10));
}


MyComboBox::MyComboBox ()
{
    minimumWidth = naturalWidth = RTScalable::scalePixelSize(70);
}

bool MyComboBox::on_scroll_event (GdkEventScroll* event)
{

    // If Shift is pressed, the widget is modified
    if (event->state & GDK_SHIFT_MASK) {
        Gtk::ComboBox::on_scroll_event(event);
        return true;
    }

    // ... otherwise the scroll event is sent back to an upper level
    return false;
}

void MyComboBox::setPreferredWidth (int minimum_width, int natural_width)
{
    if (natural_width == -1 && minimum_width == -1) {
        naturalWidth = minimumWidth = RTScalable::scalePixelSize(70);
    } else if (natural_width == -1) {
        naturalWidth =  minimumWidth = minimum_width;
    } else if (minimum_width == -1) {
        naturalWidth = natural_width;
        minimumWidth = rtengine::max(naturalWidth / 2, RTScalable::scalePixelSize(20));
        minimumWidth = rtengine::min(naturalWidth, minimumWidth);
    } else {
        naturalWidth = natural_width;
        minimumWidth = minimum_width;
    }
}

void MyComboBox::get_preferred_width_vfunc (int &minimum_width, int &natural_width) const
{
    natural_width = rtengine::max(naturalWidth, RTScalable::scalePixelSize(10));
    minimum_width = rtengine::max(minimumWidth, RTScalable::scalePixelSize(10));
}

void MyComboBox::get_preferred_width_for_height_vfunc (int height, int &minimum_width, int &natural_width) const
{
    natural_width = rtengine::max(naturalWidth, RTScalable::scalePixelSize(10));
    minimum_width = rtengine::max(minimumWidth, RTScalable::scalePixelSize(10));
}

MySpinButton::MySpinButton ()
{
    Gtk::Border border;
    border.set_bottom(0);
    border.set_top(0);
    border.set_left(3);
    border.set_right(3);
    set_inner_border(border);
    set_numeric(true);
    set_wrap(false);
    set_alignment(Gtk::ALIGN_END);
    set_update_policy(Gtk::SpinButtonUpdatePolicy::UPDATE_IF_VALID); // Avoid updating text if input is not a numeric
}

void MySpinButton::updateSize()
{
    double vMin, vMax;
    int maxAbs;
    unsigned int digits, digits2;
    unsigned int maxLen;

    get_range(vMin, vMax);

    digits = get_digits();
    maxAbs = (int)(fmax(fabs(vMin), fabs(vMax)) + 0.000001);

    if (maxAbs == 0) {
        digits2 = 1;
    } else {
        digits2 = (int)(log10(double(maxAbs)) + 0.000001);
        digits2++;
    }

    maxLen = digits + digits2 + (vMin < 0 ? 1 : 0) + (digits > 0 ? 1 : 0);
    set_max_length(maxLen);
    set_width_chars(maxLen);
    set_max_width_chars(maxLen);
}

bool MySpinButton::on_key_press_event (GdkEventKey* event)
{
    double vMin, vMax;
    get_range(vMin, vMax);

    if ((event->keyval >= GDK_KEY_a && event->keyval <= GDK_KEY_z)
            || (event->keyval >= GDK_KEY_A && event->keyval <= GDK_KEY_Z)
            || event->keyval == GDK_KEY_equal || event->keyval == GDK_KEY_underscore
            || event->keyval == GDK_KEY_plus || (event->keyval == GDK_KEY_minus && vMin >= 0)) {
        return false; // Event is propagated further
    } else {
        if (event->keyval == GDK_KEY_comma || event->keyval == GDK_KEY_KP_Decimal) {
            set_text(get_text() + ".");
            set_position(get_text().length()); // When setting text, cursor position is reset at text start. Avoiding this with this code
            return true; // Event is not propagated further
        }

        return Gtk::SpinButton::on_key_press_event(event); // Event is propagated normally
    }
}

bool MySpinButton::on_scroll_event (GdkEventScroll* event)
{
    // If Shift is pressed, the widget is modified
    if (event->state & GDK_SHIFT_MASK) {
        Gtk::SpinButton::on_scroll_event(event);
        return true;
    }

    // ... otherwise the scroll event is sent back to an upper level
    return false;
}

bool MyHScale::on_scroll_event (GdkEventScroll* event)
{

//    printf("MyHScale::on_scroll_event / delta_x=%.5f, delta_y=%.5f, direction=%d, type=%d, send_event=%d\n",
//            event->delta_x, event->delta_y, (int)event->direction, (int)event->type, event->send_event);
    // If Shift is pressed, the widget is modified
    if (event->state & GDK_SHIFT_MASK) {
        Gtk::Scale::on_scroll_event(event);
        return true;
    }

    // ... otherwise the scroll event is sent back to an upper level
    return false;
}

bool MyHScale::on_key_press_event (GdkEventKey* event)
{

    if ( event->string[0] == '+' || event->string[0] == '-' ) {
        return false;
    } else {
        return Gtk::Widget::on_key_press_event(event);
    }
}

void MyHScale::setTrackGradient(const std::vector<GradientMilestone>& milestones)
{
    trackGradient_ = milestones;
    queue_draw();
}

void MyHScale::clearTrackGradient()
{
    trackGradient_.clear();
    queue_draw();
}

void MyHScale::setLabelText(const Glib::ustring& text)
{
    labelText_ = text;
    queue_draw();
}

void MyHScale::setLabelClickCallback(std::function<void()> callback)
{
    labelClickCallback_ = std::move(callback);
}

bool MyHScale::hasLabel() const
{
    return !labelText_.empty();
}

double MyHScale::getLabelAreaWidth() const
{
    return labelAreaWidth_;
}

bool MyHScale::on_button_press_event(GdkEventButton* event)
{
    if (event->button == 1 && labelClickCallback_ && labelAreaWidth_ > 0) {
        if (event->x < labelAreaWidth_) {
            labelClickCallback_();
            return true; // consume click, don't move thumb
        }
    }
    return Gtk::Scale::on_button_press_event(event);
}

bool MyHScale::on_draw (const Cairo::RefPtr<Cairo::Context>& cr)
{
    const Gtk::Allocation alloc = get_allocation();
    const int w = alloc.get_width();
    const int h = alloc.get_height();
    const int troughY = h / 2;
    const bool hasLbl = hasLabel();

    // For labeled sliders: skip GTK default draw entirely (we paint everything).
    // Pill background is drawn by Adjuster::on_draw — don't erase it here.
    // Zoom-style sliders also paint everything themselves.
    // For other unlabeled: let GTK draw first, then overlay custom elements.
    bool ret = true;
    if (!hasLbl && !zoomStyle_) {
        ret = Gtk::Scale::on_draw(cr);
    }
    // (labeled: no erase — Adjuster's pill shows through)

    int sliderStart = 0, sliderEnd = 0;
    get_slider_range(sliderStart, sliderEnd);
    const int padding = 2;
    const int troughWidth = w - 2 * padding;
    const bool isBipolar = get_style_context()->has_class("bipolar");

    const double rangeMin = get_adjustment()->get_lower();
    const double rangeMax = get_adjustment()->get_upper();
    const double value = get_value();
    const double range = rangeMax - rangeMin;

    if (!hasLbl && zoomStyle_) {
        // Modern look: pill trough, accent fill up to the knob, round knob.
        labelAreaWidth_ = 0.0;
        const double troughH = 5.0;
        const double radius = troughH / 2.0;
        const double ty0 = troughY - radius;
        const double x0 = padding + radius;
        const double x1 = padding + troughWidth - radius;

        cr->begin_new_sub_path();
        cr->arc(x0, ty0 + radius, radius, rtengine::RT_PI * 0.5, rtengine::RT_PI * 1.5);
        cr->arc(x1, ty0 + radius, radius, rtengine::RT_PI * 1.5, rtengine::RT_PI * 0.5);
        cr->close_path();
        {
            const Gdk::RGBA troughWash = themeColor(*this, "steep_wash", Gdk::RGBA("#ffffff"));
            cr->set_source_rgba(troughWash.get_red(), troughWash.get_green(), troughWash.get_blue(), 0.13);
        }
        cr->fill();

        if (range > 0) {
            const double valueFrac = (value - rangeMin) / range;
            const double knobX = sliderStart + valueFrac * (sliderEnd - sliderStart);

            if (knobX > x0) {
                cr->begin_new_sub_path();
                cr->arc(x0, ty0 + radius, radius, rtengine::RT_PI * 0.5, rtengine::RT_PI * 1.5);
                cr->arc(std::max(knobX, x0), ty0 + radius, radius, rtengine::RT_PI * 1.5, rtengine::RT_PI * 0.5);
                cr->close_path();
                const Gdk::RGBA accent = themeColor(*this, "steep_accent", Gdk::RGBA("#64a0ff"));
                cr->set_source_rgba(accent.get_red(), accent.get_green(), accent.get_blue(), 0.75);
                cr->fill();
            }

            const bool isHover = get_state_flags() & Gtk::STATE_FLAG_PRELIGHT;
            const double kr = isHover ? 7.0 : 6.0;

            cr->arc(knobX, troughY + 1.0, kr, 0, 2 * rtengine::RT_PI);
            cr->set_source_rgba(0.0, 0.0, 0.0, 0.35);
            cr->fill();

            cr->arc(knobX, troughY, kr, 0, 2 * rtengine::RT_PI);
            {
                const Gdk::RGBA knobInk = themeColor(*this, "steep_text_hi", Gdk::RGBA("#eeeef4"));
                cr->set_source_rgba(knobInk.get_red(), knobInk.get_green(), knobInk.get_blue(), isHover ? 1.0 : 0.92);
            }
            cr->fill();

            cr->arc(knobX, troughY, kr - 0.5, 0, 2 * rtengine::RT_PI);
            const Gdk::RGBA ringAccent = themeColor(*this, "steep_accent", Gdk::RGBA("#64a0ff"));
            cr->set_source_rgba(ringAccent.get_red(), ringAccent.get_green(), ringAccent.get_blue(), 0.9);
            cr->set_line_width(1.6);
            cr->stroke();
        }

        return true;
    }

    if (hasLbl) {
        // Pill background, value fill, and label text are all drawn by Adjuster::on_draw
        // at the grid level. Here we only draw the gradient track if present.

        const double pillR = 4.0;

        // --- Gradient track inside pill (if present, e.g. Temperature/Tint) ---
        if (!trackGradient_.empty()) {
            const int gradH = h - 6;
            const int gradY2 = troughY - gradH / 2;
            const int gradStartX = padding + 2;
            const int gradEndX = w - padding - 2;

            auto gradient = Cairo::LinearGradient::create(gradStartX, 0, gradEndX, 0);
            for (const auto& ms : trackGradient_) {
                gradient->add_color_stop_rgba(ms.position, ms.r, ms.g, ms.b, (ms.a > 0 ? ms.a : 1.0) * 0.4);
            }
            cr->begin_new_sub_path();
            cr->arc(gradStartX + pillR, gradY2 + pillR, pillR, rtengine::RT_PI, rtengine::RT_PI * 1.5);
            cr->arc(gradEndX - pillR, gradY2 + pillR, pillR, rtengine::RT_PI * 1.5, 0);
            cr->arc(gradEndX - pillR, gradY2 + gradH - pillR, pillR, 0, rtengine::RT_PI * 0.5);
            cr->arc(gradStartX + pillR, gradY2 + gradH - pillR, pillR, rtengine::RT_PI * 0.5, rtengine::RT_PI);
            cr->close_path();
            cr->set_source(gradient);
            cr->fill();
        }

        // Compute label area width for click detection (used by on_button_press_event)
        auto layout = create_pango_layout(labelText_);
        auto fontDesc = Pango::FontDescription("sans 9");
        layout->set_font_description(fontDesc);
        Pango::Rectangle logRect;
        layout->get_pixel_extents(logRect, logRect);
        labelAreaWidth_ = padding + 12 + logRect.get_width() + 6;
    } else {
        // --- Unlabeled slider: keep existing custom overlay ---
        labelAreaWidth_ = 0.0;

        if (!trackGradient_.empty()) {
            const int troughHeight = std::max(6, static_cast<int>(h * 0.28));
            const int fillY = troughY - troughHeight / 2;
            const double radius = troughHeight / 2.0;

            auto gradient = Cairo::LinearGradient::create(padding, 0, padding + troughWidth, 0);
            for (const auto& ms : trackGradient_) {
                gradient->add_color_stop_rgba(ms.position, ms.r, ms.g, ms.b, ms.a > 0 ? ms.a : 1.0);
            }
            cr->begin_new_sub_path();
            cr->arc(padding + radius, fillY + radius, radius, rtengine::RT_PI * 0.5, rtengine::RT_PI * 1.5);
            cr->arc(padding + troughWidth - radius, fillY + radius, radius, rtengine::RT_PI * 1.5, rtengine::RT_PI * 0.5);
            cr->close_path();
            cr->set_source(gradient);
            cr->fill();
        }

        // Bipolar center-fill
        if (isBipolar && range > 0) {
            const int bipolarHeight = 2;
            const int bipolarY = troughY - bipolarHeight / 2;
            const double centerFrac = (0.0 - rangeMin) / range;
            const double valueFrac = (value - rangeMin) / range;
            const int centerX = padding + static_cast<int>(centerFrac * troughWidth);
            const int valueX = padding + static_cast<int>(valueFrac * troughWidth);
            const int fillLeft = std::min(centerX, valueX);
            const int fillRight = std::max(centerX, valueX);

            if (fillRight > fillLeft) {
                const Gdk::RGBA fillAccent = themeColor(*this, "steep_accent", Gdk::RGBA("#64a0ff"));
                cr->set_source_rgba(fillAccent.get_red(), fillAccent.get_green(), fillAccent.get_blue(), 0.85);
                cr->rectangle(fillLeft, bipolarY, fillRight - fillLeft, bipolarHeight);
                cr->fill();
            }
        }

        // Tick marks
        if (showTickMarks_) {
            const int troughHeight = std::max(6, static_cast<int>(h * 0.28));
            const int fillY = troughY - troughHeight / 2;
            const int tickHeight = 3;
            const int tickTop = fillY - tickHeight - 1;

            for (int i = 0; i <= 10; ++i) {
                const double frac = i / 10.0;
                const int x = padding + static_cast<int>(frac * troughWidth);
                const bool isCenter = (i == 5);

                const Gdk::RGBA tickWash = themeColor(*this, "steep_wash", Gdk::RGBA("#ffffff"));
                cr->set_source_rgba(tickWash.get_red(), tickWash.get_green(), tickWash.get_blue(), isCenter && isBipolar ? 0.25 : 0.12);
                cr->set_line_width(1.0);
                cr->move_to(x + 0.5, tickTop);
                cr->line_to(x + 0.5, tickTop + tickHeight + (isCenter && isBipolar ? 1 : 0));
                cr->stroke();
            }
        }

        // Diamond thumb for unlabeled
        if (range > 0) {
            const double valueFrac = (value - rangeMin) / range;
            const int knobX = sliderStart + static_cast<int>(valueFrac * (sliderEnd - sliderStart));
            const double dw = 4.0;
            const double dh = 5.0;
            bool isHover = get_state_flags() & Gtk::STATE_FLAG_PRELIGHT;

            cr->move_to(knobX, troughY - dh + 1);
            cr->line_to(knobX + dw, troughY + 1);
            cr->line_to(knobX, troughY + dh + 1);
            cr->line_to(knobX - dw, troughY + 1);
            cr->close_path();
            cr->set_source_rgba(0, 0, 0, 0.2);
            cr->fill();

            cr->move_to(knobX, troughY - dh);
            cr->line_to(knobX + dw, troughY);
            cr->line_to(knobX, troughY + dh);
            cr->line_to(knobX - dw, troughY);
            cr->close_path();
            {
                const Gdk::RGBA thumbInk = themeColor(*this, "steep_text_hi", Gdk::RGBA("#d0d0d4"));
                cr->set_source_rgba(thumbInk.get_red(), thumbInk.get_green(), thumbInk.get_blue(), isHover ? 1.0 : 0.85);
            }
            cr->fill_preserve();

            {
                const Gdk::RGBA thumbEdge = themeColor(*this, "steep_text_dim", Gdk::RGBA("#666a70"));
                cr->set_source_rgba(thumbEdge.get_red(), thumbEdge.get_green(), thumbEdge.get_blue(), 1.0);
            }
            cr->set_line_width(1.0);
            cr->stroke();
        }
    }

    return ret;
}

class MyFileChooserWidget::Impl
{
public:
    Impl(const Glib::ustring &title, Gtk::FileChooserAction action) :
        title_(title),
        action_(action)
    {
    }

    Glib::ustring title_;
    Gtk::FileChooserAction action_;
    std::string filename_;
    std::string current_folder_;
    std::vector<Glib::RefPtr<Gtk::FileFilter>> file_filters_;
    Glib::RefPtr<Gtk::FileFilter> cur_filter_;
    std::vector<std::string> shortcut_folders_;
    bool show_hidden_{false};
    sigc::signal<void> selection_changed_;
};


MyFileChooserWidget::MyFileChooserWidget(const Glib::ustring &title, Gtk::FileChooserAction action) :
    pimpl(new Impl(title, action))
{
}


std::unique_ptr<Gtk::Image> MyFileChooserWidget::make_folder_image()
{
    return std::unique_ptr<Gtk::Image>(new RTImage("folder-open-small", Gtk::ICON_SIZE_BUTTON));
}

void MyFileChooserWidget::show_chooser(Gtk::Widget *parent)
{
    Gtk::FileChooserDialog dlg(getToplevelWindow(parent), pimpl->title_, pimpl->action_);
    dlg.add_button(M("GENERAL_CANCEL"), Gtk::RESPONSE_CANCEL);
    dlg.add_button(M(pimpl->action_ == Gtk::FILE_CHOOSER_ACTION_SAVE ? "GENERAL_SAVE" : "GENERAL_OPEN"), Gtk::RESPONSE_OK);
    dlg.set_filename(pimpl->filename_);
    for (auto &f : pimpl->file_filters_) {
        dlg.add_filter(f);
    }
    if (pimpl->cur_filter_) {
        dlg.set_filter(pimpl->cur_filter_);
    }
    for (auto &f : pimpl->shortcut_folders_) {
        dlg.add_shortcut_folder(f);
    }
    if (!pimpl->current_folder_.empty()) {
        dlg.set_current_folder(pimpl->current_folder_);
    }
    dlg.set_show_hidden(pimpl->show_hidden_);
    int res = dlg.run();
    if (res == Gtk::RESPONSE_OK) {
        pimpl->filename_ = dlg.get_filename();
        pimpl->current_folder_ = dlg.get_current_folder();
        on_filename_set();
        pimpl->selection_changed_.emit();
    }
}


void MyFileChooserWidget::on_filename_set()
{
    // Sub-classes decide if anything needs to be done.
}


sigc::signal<void> &MyFileChooserWidget::signal_selection_changed()
{
    return pimpl->selection_changed_;
}


sigc::signal<void> &MyFileChooserWidget::signal_file_set()
{
    return pimpl->selection_changed_;
}


std::string MyFileChooserWidget::get_filename() const
{
    return pimpl->filename_;
}


bool MyFileChooserWidget::set_filename(const std::string &filename)
{
    pimpl->filename_ = filename;
    on_filename_set();
    return true;
}


void MyFileChooserWidget::add_filter(const Glib::RefPtr<Gtk::FileFilter> &filter)
{
    pimpl->file_filters_.push_back(filter);
}


void MyFileChooserWidget::remove_filter(const Glib::RefPtr<Gtk::FileFilter> &filter)
{
    auto it = std::find(pimpl->file_filters_.begin(), pimpl->file_filters_.end(), filter);
    if (it != pimpl->file_filters_.end()) {
        pimpl->file_filters_.erase(it);
    }
}


void MyFileChooserWidget::set_filter(const Glib::RefPtr<Gtk::FileFilter> &filter)
{
    pimpl->cur_filter_ = filter;
}


std::vector<Glib::RefPtr<Gtk::FileFilter>> MyFileChooserWidget::list_filters() const
{
    return pimpl->file_filters_;
}


bool MyFileChooserWidget::set_current_folder(const std::string &filename)
{
    pimpl->current_folder_ = filename;
    if (pimpl->action_ == Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER) {
        set_filename(filename);
    }
    return true;
}

std::string MyFileChooserWidget::get_current_folder() const
{
    return pimpl->current_folder_;
}


bool MyFileChooserWidget::add_shortcut_folder(const std::string &folder)
{
    pimpl->shortcut_folders_.push_back(folder);
    return true;
}


bool MyFileChooserWidget::remove_shortcut_folder(const std::string &folder)
{
    auto it = std::find(pimpl->shortcut_folders_.begin(), pimpl->shortcut_folders_.end(), folder);
    if (it != pimpl->shortcut_folders_.end()) {
        pimpl->shortcut_folders_.erase(it);
    }
    return true;
}


void MyFileChooserWidget::unselect_all()
{
    pimpl->filename_ = "";
    on_filename_set();
}


void MyFileChooserWidget::unselect_filename(const std::string &filename)
{
    if (pimpl->filename_ == filename) {
        unselect_all();
    }
}


void MyFileChooserWidget::set_show_hidden(bool yes)
{
    pimpl->show_hidden_ = yes;
}


class MyFileChooserButton::Impl
{
public:
    Gtk::Box box_;
    Gtk::Label lbl_{"", Gtk::ALIGN_START};
};

MyFileChooserButton::MyFileChooserButton(const Glib::ustring &title, Gtk::FileChooserAction action):
    MyFileChooserWidget(title, action),
    pimpl(new Impl())
{
    pimpl->lbl_.set_ellipsize(Pango::ELLIPSIZE_MIDDLE);
    pimpl->lbl_.set_justify(Gtk::JUSTIFY_LEFT);
    on_filename_set();
    pimpl->box_.pack_start(pimpl->lbl_, true, true);
    pimpl->box_.pack_start(*Gtk::manage(new Gtk::Separator(Gtk::ORIENTATION_VERTICAL)), false, false, 5);
    pimpl->box_.pack_start(*Gtk::manage(make_folder_image().release()), false, false);
    pimpl->box_.show_all_children();
    add(pimpl->box_);
    signal_clicked().connect([this]() {
        show_chooser(this);
    });

    if (GTK_MINOR_VERSION < 20) {
        set_border_width(2); // margin doesn't work on GTK < 3.20
    }

    set_name("MyFileChooserButton");
}

void MyFileChooserButton::on_filename_set()
{
    if (Glib::file_test(get_filename(), Glib::FILE_TEST_EXISTS)) {
        pimpl->lbl_.set_label(Glib::path_get_basename(get_filename()));
    } else {
        pimpl->lbl_.set_label(Glib::ustring("(") + M("GENERAL_NONE") + ")");
    }
}


// For an unknown reason (a bug ?), it doesn't work when action = FILE_CHOOSER_ACTION_SELECT_FOLDER !
bool MyFileChooserButton::on_scroll_event (GdkEventScroll* event)
{

    // If Shift is pressed, the widget is modified
    if (event->state & GDK_SHIFT_MASK) {
        Gtk::Button::on_scroll_event(event);
        return true;
    }

    // ... otherwise the scroll event is sent back to an upper level
    return false;
}

void MyFileChooserButton::get_preferred_width_vfunc (int &minimum_width, int &natural_width) const
{
    minimum_width = natural_width = RTScalable::scalePixelSize(35);
}

void MyFileChooserButton::get_preferred_width_for_height_vfunc (int height, int &minimum_width, int &natural_width) const
{
    minimum_width = natural_width = RTScalable::scalePixelSize(35);
}


class MyFileChooserEntry::Impl
{
public:
    Gtk::Entry entry;
    Gtk::Button file_chooser_button;
};


MyFileChooserEntry::MyFileChooserEntry(const Glib::ustring &title, Gtk::FileChooserAction action) :
    MyFileChooserWidget(title, action),
    pimpl(new Impl())
{
    const auto on_text_changed = [this]() {
        set_filename(pimpl->entry.get_text());
    };
    pimpl->entry.get_buffer()->signal_deleted_text().connect([on_text_changed](guint, guint) { on_text_changed(); });
    pimpl->entry.get_buffer()->signal_inserted_text().connect([on_text_changed](guint, const gchar *, guint) { on_text_changed(); });

    pimpl->file_chooser_button.set_image(*Gtk::manage(make_folder_image().release()));
    pimpl->file_chooser_button.signal_clicked().connect([this]() {
        const auto &filename = get_filename();
        if (Glib::file_test(filename, Glib::FILE_TEST_IS_DIR)) {
            set_current_folder(filename);
        }
        show_chooser(this);
    });

    pack_start(pimpl->entry, true, true);
    pack_start(pimpl->file_chooser_button, false, false);
}


Glib::ustring MyFileChooserEntry::get_placeholder_text() const
{
    return pimpl->entry.get_placeholder_text();
}


void MyFileChooserEntry::set_placeholder_text(const Glib::ustring &text)
{
    pimpl->entry.set_placeholder_text(text);
}


void MyFileChooserEntry::on_filename_set()
{
    if (pimpl->entry.get_text() != get_filename()) {
        pimpl->entry.set_text(get_filename());
    }
}


TextOrIcon::TextOrIcon (const Glib::ustring &icon_name, const Glib::ustring &labelTx, const Glib::ustring &tooltipTx)
{

    RTImage *img = Gtk::manage(new RTImage(icon_name, Gtk::ICON_SIZE_LARGE_TOOLBAR));
    pack_start(*img, Gtk::PACK_SHRINK, 0);
    set_tooltip_markup("<span font_size=\"large\" font_weight=\"bold\">" + labelTx  + "</span>\n" + tooltipTx);

    set_name("TextOrIcon");
    show_all();

}

class ImageAndLabel::Impl
{
public:
    RTImage* image;
    Gtk::Label* label;

    Impl(RTImage* image, Gtk::Label* label) : image(image), label(label) {}
    static std::unique_ptr<RTImage> createImage(const Glib::ustring& iconName);
};

std::unique_ptr<RTImage> ImageAndLabel::Impl::createImage(const Glib::ustring& iconName)
{
    if (iconName.empty()) {
        return nullptr;
    }
    return std::unique_ptr<RTImage>(new RTImage(iconName, Gtk::ICON_SIZE_LARGE_TOOLBAR));
}

ImageAndLabel::ImageAndLabel(const Glib::ustring& label, const Glib::ustring& iconName) :
    ImageAndLabel(label, Gtk::manage(Impl::createImage(iconName).release()))
{
}

ImageAndLabel::ImageAndLabel(const Glib::ustring& label, RTImage *image) :
    pimpl(new Impl(image, Gtk::manage(new Gtk::Label(label))))
{
    Gtk::Grid* grid = Gtk::manage(new Gtk::Grid());
    grid->set_orientation(Gtk::ORIENTATION_HORIZONTAL);

    if (image) {
        grid->attach_next_to(*image, Gtk::POS_LEFT, 1, 1);
    }

    grid->attach_next_to(*(pimpl->label), Gtk::POS_RIGHT, 1, 1);
    grid->set_column_spacing(4);
    grid->set_row_spacing(0);
    pack_start(*grid, Gtk::PACK_SHRINK, 0);
}

const RTImage* ImageAndLabel::getImage() const
{
    return pimpl->image;
}

const Gtk::Label* ImageAndLabel::getLabel() const
{
    return pimpl->label;
}

class MyImageMenuItem::Impl
{
private:
    std::unique_ptr<ImageAndLabel> widget;

public:
    Impl(const Glib::ustring &label, const Glib::ustring &iconName) :
        widget(new ImageAndLabel(label, iconName)) {}
    Impl(const Glib::ustring &label, RTImage *itemImage) :
        widget(new ImageAndLabel(label, itemImage)) {}
    ImageAndLabel* getWidget() const { return widget.get(); }
};

MyImageMenuItem::MyImageMenuItem(const Glib::ustring& label, const Glib::ustring& iconName) :
    pimpl(new Impl(label, iconName))
{
    add(*(pimpl->getWidget()));
}

MyImageMenuItem::MyImageMenuItem(const Glib::ustring& label, RTImage* itemImage) :
    pimpl(new Impl(label, itemImage))
{
    add(*(pimpl->getWidget()));
}

const RTImage *MyImageMenuItem::getImage () const
{
    return pimpl->getWidget()->getImage();
}

const Gtk::Label* MyImageMenuItem::getLabel () const
{
    return pimpl->getWidget()->getLabel();
}

class MyRadioImageMenuItem::Impl
{
    std::unique_ptr<ImageAndLabel> widget;

public:
    Impl(const Glib::ustring &label, RTImage *image) :
        widget(new ImageAndLabel(label, image)) {}
    ImageAndLabel* getWidget() const { return widget.get(); }
};

MyRadioImageMenuItem::MyRadioImageMenuItem(const Glib::ustring& label, RTImage *image, Gtk::RadioButton::Group& group) :
    Gtk::RadioMenuItem(group),
    pimpl(new Impl(label, image))
{
    add(*(pimpl->getWidget()));
}

const Gtk::Label* MyRadioImageMenuItem::getLabel() const
{
    return pimpl->getWidget()->getLabel();
}

MyProgressBar::MyProgressBar(int width) : w(rtengine::max(width, RTScalable::scalePixelSize(10))) {}
MyProgressBar::MyProgressBar() : w(RTScalable::scalePixelSize(200)) {}

void MyProgressBar::setPreferredWidth(int width)
{
    w = rtengine::max(width, RTScalable::scalePixelSize(10));
}

void MyProgressBar::get_preferred_width_vfunc (int &minimum_width, int &natural_width) const
{
    minimum_width = rtengine::max(w / 2, RTScalable::scalePixelSize(50));
    natural_width = rtengine::max(w, RTScalable::scalePixelSize(50));
}

void MyProgressBar::get_preferred_width_for_height_vfunc (int height, int &minimum_width, int &natural_width) const
{
    get_preferred_width_vfunc (minimum_width, natural_width);
}

BackBuffer::BackBuffer() : x(0), y(0), w(0), h(0), offset(0, 0), dirty(true) {}
BackBuffer::BackBuffer(int width, int height, Cairo::Format format) : x(0), y(0), w(width), h(height), offset(0, 0), dirty(true)
{
    if (w > 0 && h > 0) {
        surface = Cairo::ImageSurface::create(format, w, h);
    } else {
        w = h = 0;
    }
}

void BackBuffer::setDestPosition(int x, int y)
{
    // values will be clamped when used...
    this->x = x;
    this->y = y;
}

void BackBuffer::setSrcOffset(int x, int y)
{
    // values will be clamped when used...
    offset.set(x, y);
}

void BackBuffer::setSrcOffset(const rtengine::Coord &newOffset)
{
    // values will be clamped when used...
    offset = newOffset;
}

void BackBuffer::getSrcOffset(int &x, int &y)
{
    // values will be clamped when used...
    offset.get(x, y);
}

void BackBuffer::getSrcOffset(rtengine::Coord &offset)
{
    // values will be clamped when used...
    offset = this->offset;
}

// Note: newW & newH must be > 0
bool BackBuffer::setDrawRectangle(Glib::RefPtr<Gdk::Window> window, Gdk::Rectangle &rectangle, bool updateBackBufferSize)
{
    return setDrawRectangle(window, rectangle.get_x(), rectangle.get_y(), rectangle.get_width(), rectangle.get_height(), updateBackBufferSize);
}

// Note: newW & newH must be > 0
bool BackBuffer::setDrawRectangle(Glib::RefPtr<Gdk::Window> window, int newX, int newY, int newW, int newH, bool updateBackBufferSize)
{
    assert(newW && newH);

    bool newSize = (newW > 0 && w != newW) || (newH > 0 && h != newH);

    x = newX;
    y = newY;
    if (newW > 0) {
        w = newW;
    }
    if (newH > 0) {
        h = newH;
    }

    // WARNING: we're assuming that the surface type won't change during all the execution time of RT. I guess it may be wrong when the user change the gfx card display settings!?
    if (((updateBackBufferSize && newSize) || !surface) && window) {
        // allocate a new Surface
        surface.clear();  // ... don't know if this is necessary?
        surface = Cairo::ImageSurface::create(Cairo::FORMAT_RGB24, w, h);
        dirty = true;
    }

    return dirty;
}

// Note: newW & newH must be > 0
bool BackBuffer::setDrawRectangle(Cairo::Format format, Gdk::Rectangle &rectangle, bool updateBackBufferSize)
{
    return setDrawRectangle(format, rectangle.get_x(), rectangle.get_y(), rectangle.get_width(), rectangle.get_height(), updateBackBufferSize);
}

// Note: newW & newH must be > 0
bool BackBuffer::setDrawRectangle(Cairo::Format format, int newX, int newY, int newW, int newH, bool updateBackBufferSize)
{
    assert(newW && newH);

    bool newSize = (newW > 0 && w != newW) || (newH > 0 && h != newH);

    x = newX;
    y = newY;
    if (newW > 0) {
        w = newW;
    }
    if (newH > 0) {
        h = newH;
    }

    // WARNING: we're assuming that the surface type won't change during all the execution time of RT. I guess it may be wrong when the user change the gfx card display settings!?
    if ((updateBackBufferSize && newSize) || !surface) {
        // allocate a new Surface
        surface.clear();  // ... don't know if this is necessary?
        surface = Cairo::ImageSurface::create(format, w, h);
        dirty = true;
    }

    return dirty;
}

/*
 * Copy uint8 RGB raw data to an ImageSurface. We're assuming that the source contains enough data for the given srcX, srcY, srcW, srcH -> no error checking!
 */
void BackBuffer::copyRGBCharData(const unsigned char *srcData, int srcX, int srcY, int srcW, int srcH, int srcRowStride, int dstX, int dstY)
{
    unsigned char r, g, b;

    if (!surface) {
        return;
    }

    //printf("copyRGBCharData:    src: (X:%d Y:%d, W:%d H:%d)  /  dst: (X: %d Y:%d)\n", srcX, srcY, srcW, srcH, dstX, dstY);

    unsigned char *dstData = surface->get_data();
    int surfW = surface->get_width();
    int surfH = surface->get_height();

    if (!srcData || dstX >= surfW || dstY >= surfH || srcW <= 0 || srcH <= 0 || srcX < 0 || srcY < 0) {
        return;
    }

    for (int i = 0; i < srcH; ++i) {
        if (dstY + i >= surfH) {
            break;
        }

        const unsigned char *src = srcData + i * srcRowStride;
        unsigned char *dst = dstData + ((dstY + i) * surfW + dstX) * 4;

        for (int j = 0; j < srcW; ++j) {
            if (dstX + j >= surfW) {
                break;
            }

            r = *(src++);
            g = *(src++);
            b = *(src++);

            rtengine::poke255_uc(dst, r, g, b);
        }
    }

    surface->mark_dirty();

}

/*
 * Copy the backbuffer to a Gdk::Window
 */
void BackBuffer::copySurface(Glib::RefPtr<Gdk::Window> window, Gdk::Rectangle *destRectangle)
{
    if (surface && window) {
        // TODO: look out if window can be different on each call, and if not, store a reference to the window
        Cairo::RefPtr<Cairo::Context> crSrc = window->create_cairo_context();
        Cairo::RefPtr<Cairo::Surface> destSurface = crSrc->get_target();

        // compute the source offset
        int offsetX = rtengine::LIM<int>(offset.x, 0, surface->get_width());
        int offsetY = rtengine::LIM<int>(offset.y, 0, surface->get_height());

        // now copy the off-screen Surface to the destination Surface
        Cairo::RefPtr<Cairo::Context> crDest = Cairo::Context::create(destSurface);
        crDest->set_line_width(0.);

        if (destRectangle) {
            crDest->set_source(surface, -offsetX + destRectangle->get_x(), -offsetY + destRectangle->get_y());
            int w_ = destRectangle->get_width() > 0 ? destRectangle->get_width() : w;
            int h_ = destRectangle->get_height() > 0 ? destRectangle->get_height() : h;
            //printf("BackBuffer::copySurface / rectangle1(%d, %d, %d, %d)\n", destRectangle->get_x(), destRectangle->get_y(), w_, h_);
            crDest->rectangle(destRectangle->get_x(), destRectangle->get_y(), w_, h_);
            //printf("BackBuffer::copySurface / rectangle1\n");
        } else {
            crDest->set_source(surface, -offsetX + x, -offsetY + y);
            //printf("BackBuffer::copySurface / rectangle2(%d, %d, %d, %d)\n", x, y, w, h);
            crDest->rectangle(x, y, w, h);
            //printf("BackBuffer::copySurface / rectangle2\n");
        }

        crDest->fill();
    }
}

/*
 * Copy the BackBuffer to another BackBuffer
 */
void BackBuffer::copySurface(BackBuffer *destBackBuffer, Gdk::Rectangle *destRectangle)
{
    if (surface && destBackBuffer) {
        Cairo::RefPtr<Cairo::ImageSurface> destSurface = destBackBuffer->getSurface();

        if (!destSurface) {
            return;
        }

        // compute the source offset
        int offsetX = rtengine::LIM<int>(offset.x, 0, surface->get_width());
        int offsetY = rtengine::LIM<int>(offset.y, 0, surface->get_height());

        // now copy the off-screen Surface to the destination Surface
        Cairo::RefPtr<Cairo::Context> crDest = Cairo::Context::create(destSurface);
        crDest->set_line_width(0.);

        if (destRectangle) {
            crDest->set_source(surface, -offsetX + destRectangle->get_x(), -offsetY + destRectangle->get_y());
            int w_ = destRectangle->get_width() > 0 ? destRectangle->get_width() : w;
            int h_ = destRectangle->get_height() > 0 ? destRectangle->get_height() : h;
            //printf("BackBuffer::copySurface / rectangle3(%d, %d, %d, %d)\n", destRectangle->get_x(), destRectangle->get_y(), w_, h_);
            crDest->rectangle(destRectangle->get_x(), destRectangle->get_y(), w_, h_);
            //printf("BackBuffer::copySurface / rectangle3\n");
        } else {
            crDest->set_source(surface, -offsetX + x, -offsetY + y);
            //printf("BackBuffer::copySurface / rectangle4(%d, %d, %d, %d)\n", x, y, w, h);
            crDest->rectangle(x, y, w, h);
            //printf("BackBuffer::copySurface / rectangle4\n");
        }

        crDest->fill();
    }
}

/*
 * Copy the BackBuffer to another Cairo::Surface
 */
void BackBuffer::copySurface(const Cairo::RefPtr<Cairo::ImageSurface>& destSurface,
                             Gdk::Rectangle *destRectangle)
{
    if (surface && destSurface) {
        // compute the source offset
        int offsetX = rtengine::LIM<int>(offset.x, 0, surface->get_width());
        int offsetY = rtengine::LIM<int>(offset.y, 0, surface->get_height());

        // now copy the off-screen Surface to the destination Surface
        Cairo::RefPtr<Cairo::Context> crDest = Cairo::Context::create(destSurface);
        crDest->set_line_width(0.);

        if (destRectangle) {
            crDest->set_source(surface, -offsetX + destRectangle->get_x(), -offsetY + destRectangle->get_y());
            int w_ = destRectangle->get_width() > 0 ? destRectangle->get_width() : w;
            int h_ = destRectangle->get_height() > 0 ? destRectangle->get_height() : h;
            //printf("BackBuffer::copySurface / rectangle5(%d, %d, %d, %d)\n", destRectangle->get_x(), destRectangle->get_y(), w_, h_);
            crDest->rectangle(destRectangle->get_x(), destRectangle->get_y(), w_, h_);
            //printf("BackBuffer::copySurface / rectangle5\n");
        } else {
            crDest->set_source(surface, -offsetX + x, -offsetY + y);
            //printf("BackBuffer::copySurface / rectangle6(%d, %d, %d, %d)\n", x, y, w, h);
            crDest->rectangle(x, y, w, h);
            //printf("BackBuffer::copySurface / rectangle6\n");
        }

        crDest->fill();
    }
}

/*
 * Copy the BackBuffer to another Cairo::Surface
 */
void BackBuffer::copySurface(const Cairo::RefPtr<Cairo::Context>& crDest,
                             Gdk::Rectangle *destRectangle)
{
    if (surface && crDest) {
        // compute the source offset
        int offsetX = rtengine::LIM<int>(offset.x, 0, surface->get_width());
        int offsetY = rtengine::LIM<int>(offset.y, 0, surface->get_height());

        // now copy the off-screen Surface to the destination Surface
        // int srcSurfW = surface->get_width();
        // int srcSurfH = surface->get_height();
        //printf("srcSurf:  w: %d, h: %d\n", srcSurfW, srcSurfH);
        crDest->set_line_width(0.);

        if (destRectangle) {
            crDest->set_source(surface, -offsetX + destRectangle->get_x(), -offsetY + destRectangle->get_y());
            int w_ = destRectangle->get_width() > 0 ? destRectangle->get_width() : w;
            int h_ = destRectangle->get_height() > 0 ? destRectangle->get_height() : h;
            //printf("BackBuffer::copySurface / rectangle7(%d, %d, %d, %d)\n", destRectangle->get_x(), destRectangle->get_y(), w_, h_);
            crDest->rectangle(destRectangle->get_x(), destRectangle->get_y(), w_, h_);
            //printf("BackBuffer::copySurface / rectangle7\n");
        } else {
            crDest->set_source(surface, -offsetX + x, -offsetY + y);
            //printf("BackBuffer::copySurface / rectangle8(%d, %d, %d, %d)\n", x, y, w, h);
            crDest->rectangle(x, y, w, h);
            //printf("BackBuffer::copySurface / rectangle8\n");
        }

        crDest->fill();
    }
}

SpotPicker::SpotPicker(int const defaultValue, Glib::ustring const &buttonKey, Glib::ustring const &buttonTooltip, Glib::ustring const &labelKey) :
    Gtk::Grid(),
    _spotHalfWidth(defaultValue),
    _spotLabel(labelSetup(labelKey)),
    _spotSizeSetter(MyComboBoxText(selecterSetup())),
    _spotButton(spotButtonTemplate(buttonKey, buttonTooltip))

{
    this->get_style_context()->add_class("grid-spacing");
    setExpandAlignProperties(this, true, false, Gtk::ALIGN_FILL, Gtk::ALIGN_CENTER);

    this->attach (_spotButton, 0, 0, 1, 1);
    this->attach (_spotLabel, 1, 0, 1, 1);
    this->attach (_spotSizeSetter, 2, 0, 1, 1);
    _spotSizeSetter.signal_changed().connect( sigc::mem_fun(*this, &SpotPicker::spotSizeChanged));
}

Gtk::Label SpotPicker::labelSetup(Glib::ustring const &key) const
{
    Gtk::Label label(key);
    setExpandAlignProperties(&label, false, false, Gtk::ALIGN_START, Gtk::ALIGN_CENTER);
    return label;
}

MyComboBoxText SpotPicker::selecterSetup() const
{
    MyComboBoxText spotSize = MyComboBoxText();
    setExpandAlignProperties(&spotSize, false, false, Gtk::ALIGN_START, Gtk::ALIGN_CENTER);

    spotSize.append ("2");
    if (_spotHalfWidth == 2) {
        spotSize.set_active(0);
    }

    spotSize.append ("4");

    if (_spotHalfWidth == 4) {
        spotSize.set_active(1);
    }

    spotSize.append ("8");

    if (_spotHalfWidth == 8) {
        spotSize.set_active(2);
    }

    spotSize.append ("16");

    if (_spotHalfWidth == 16) {
        spotSize.set_active(3);
    }

    spotSize.append ("32");

    if (_spotHalfWidth == 32) {
        spotSize.set_active(4);
    }
    return spotSize;
}

Gtk::ToggleButton SpotPicker::spotButtonTemplate(Glib::ustring const &key, const Glib::ustring &tooltip) const
{
    Gtk::ToggleButton spotButton = Gtk::ToggleButton(key);
    setExpandAlignProperties(&spotButton, true, false, Gtk::ALIGN_FILL, Gtk::ALIGN_CENTER);
    spotButton.get_style_context()->add_class("independent");
    spotButton.set_tooltip_text(tooltip);
    return spotButton;
}

void SpotPicker::spotSizeChanged()
{
    _spotHalfWidth = atoi(_spotSizeSetter.get_active_text().c_str());
}

// OptionalRadioButtonGroup class

void OptionalRadioButtonGroup::onButtonToggled(Gtk::ToggleButton *button)
{
    if (!button) {
        return;
    }

    if (button->get_active()) {
        if (active_button == button) {
            // Same button, noting to do.
        } else if (active_button) {
            // Deactivate the other button.
            active_button->set_active(false);
        }
        active_button = button;
    } else {
        if (active_button == button) {
            // Active button got deactivated.
            active_button = nullptr;
        } else {
            // No effect on other buttons.
        }
    }
}

Gtk::ToggleButton *OptionalRadioButtonGroup::getActiveButton() const
{
    return active_button;
}

void OptionalRadioButtonGroup::register_button(Gtk::ToggleButton &button)
{
    button.signal_toggled().connect(sigc::bind(
        sigc::mem_fun(this, &OptionalRadioButtonGroup::onButtonToggled),
        &button));
    onButtonToggled(&button);
}


// AdvancedSection

AdvancedSection::AdvancedSection() :
    AdvancedSection(M("TP_GENERAL_ADVANCED"))
{
}

AdvancedSection::AdvancedSection(const Glib::ustring& customLabel) :
    Gtk::Box(Gtk::ORIENTATION_VERTICAL),
    expanded(false)
{
    set_name("AdvancedSection");
    get_style_context()->add_class("AdvancedSection");

    // Separator line above header
    auto* sep = Gtk::manage(new Gtk::Separator(Gtk::ORIENTATION_HORIZONTAL));
    pack_start(*sep, Gtk::PACK_SHRINK, 0);

    // Clickable header with arrow + label
    auto* headerBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL));
    headerBox->set_spacing(4);

    arrowImage = Gtk::manage(new RTImage("expander-closed-small"));
    arrowImage->set_can_focus(false);
    headerBox->pack_start(*arrowImage, Gtk::PACK_SHRINK, 0);

    auto* label = Gtk::manage(new Gtk::Label(customLabel));
    setExpandAlignProperties(label, false, false, Gtk::ALIGN_START, Gtk::ALIGN_CENTER);
    headerBox->pack_start(*label, Gtk::PACK_SHRINK, 0);

    auto* headerEvBox = Gtk::manage(new Gtk::EventBox());
    headerEvBox->set_name("AdvancedSectionHeader");
    setExpandAlignProperties(headerEvBox, true, false, Gtk::ALIGN_FILL, Gtk::ALIGN_FILL);
    headerEvBox->add(*headerBox);
    headerEvBox->set_can_focus(false);
    headerEvBox->signal_button_release_event().connect(
        sigc::mem_fun(*this, &AdvancedSection::onHeaderClick));

    pack_start(*headerEvBox, Gtk::PACK_SHRINK, 0);

    // Content box
    contentBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
    pack_start(*contentBox, Gtk::PACK_SHRINK, 0);

    // Default state based on UI complexity
    bool defaultExpanded = (App::get().options().uiComplexity >= Options::UI_EXPERT);
    setExpanded(defaultExpanded);

    show_all_children();
    if (!expanded) {
        contentBox->hide();
    }
}

bool AdvancedSection::onHeaderClick(GdkEventButton* event)
{
    if (event->button != 1) {
        return false;
    }
    setExpanded(!expanded);
    return false;
}

void AdvancedSection::setExpanded(bool expand)
{
    expanded = expand;
    updateArrow();
    if (expanded) {
        contentBox->set_no_show_all(false);
        contentBox->show_all();
    } else {
        contentBox->hide();
        contentBox->set_no_show_all(true);
    }

    if (onToggled) {
        onToggled(expanded);
    }
}

bool AdvancedSection::getExpanded() const
{
    return expanded;
}

void AdvancedSection::setBatchMode(bool batchMode)
{
    if (batchMode) {
        setExpanded(true);
    }
}

void AdvancedSection::updateArrow()
{
    if (expanded) {
        arrowImage->set_from_icon_name("expander-open-small");
    } else {
        arrowImage->set_from_icon_name("expander-closed-small");
    }
}

// ToolGroup

ToolGroup::ToolGroup(const Glib::ustring& label) :
    Gtk::Box(Gtk::ORIENTATION_VERTICAL),
    expanded(false)
{
    set_name("ToolGroup");
    get_style_context()->add_class("ToolGroup");
    set_hexpand(true);
    set_halign(Gtk::ALIGN_FILL);

    // Clickable header: single label with arrow + text via markup
    groupLabel_ = Glib::Markup::escape_text(label);
    arrowLabel = Gtk::manage(new Gtk::Label());
    arrowLabel->set_use_markup(true);
    arrowLabel->set_markup("<small>\xe2\x96\xb8</small>  " + groupLabel_);
    arrowLabel->set_can_focus(false);
    setExpandAlignProperties(arrowLabel, false, false, Gtk::ALIGN_START, Gtk::ALIGN_CENTER);

    auto* headerBtn = Gtk::manage(new Gtk::Button());
    headerBtn->set_name("ToolGroupHeader");
    headerBtn->set_relief(Gtk::RELIEF_NONE);
    headerBtn->set_can_focus(false);
    headerBtn->set_halign(Gtk::ALIGN_START);
    // Flattening lives in themes/common/widgets.css (#ToolGroupHeader)
    headerBtn->add(*arrowLabel);

    // Reset button (X) — hidden by default, shown when group has non-default values
    resetBtn = Gtk::manage(new Gtk::Button());
    resetBtn->set_name("ToolGroupReset");
    resetBtn->set_relief(Gtk::RELIEF_NONE);
    resetBtn->set_can_focus(false);
    resetBtn->set_tooltip_text("Reset to defaults");
    auto* resetLabel = Gtk::manage(new Gtk::Label());
    resetLabel->set_use_markup(true);
    resetLabel->set_markup("<small>\xc3\x97</small>"); // × (multiplication sign, used as X)
    resetBtn->add(*resetLabel);
    resetBtn->set_no_show_all(true);
    resetBtn->signal_clicked().connect([this]() {
        if (resetCallback_) resetCallback_();
    });

    headerBtn->signal_clicked().connect(
        sigc::mem_fun(*this, &ToolGroup::onHeaderClicked));

    // Header row: header button + reset button
    headerRow = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL));
    headerRow->set_hexpand(true);
    headerRow->set_halign(Gtk::ALIGN_FILL);
    headerRow->pack_start(*headerBtn, Gtk::PACK_SHRINK, 0);
    headerRow->pack_start(*resetBtn, Gtk::PACK_SHRINK, 0);
    pack_start(*headerRow, Gtk::PACK_SHRINK, 0);

    // Persistent box: always visible even when collapsed (for preview strips)
    persistentBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
    persistentBox->set_hexpand(true);
    persistentBox->set_halign(Gtk::ALIGN_FILL);
    persistentBox->set_margin_end(12);
    pack_start(*persistentBox, Gtk::PACK_SHRINK, 0);

    // Content box inside a Revealer for smooth expand/collapse animation
    contentBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
    contentBox->set_hexpand(true);
    contentBox->set_halign(Gtk::ALIGN_FILL);
    contentBox->set_margin_end(12);
    revealer = Gtk::manage(new Gtk::Revealer());
    revealer->set_transition_type(Gtk::REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    revealer->set_transition_duration(200);
    revealer->add(*contentBox);
    pack_start(*revealer, Gtk::PACK_SHRINK, 0);

    // Default: collapsed
    setExpanded(false);
    show_all_children();
}

void ToolGroup::onHeaderClicked()
{
    setExpanded(!expanded);
}

void ToolGroup::setExpanded(bool expand)
{
    expanded = expand;
    updateArrow();
    revealer->set_reveal_child(expanded);
}

bool ToolGroup::getExpanded() const
{
    return expanded;
}

void ToolGroup::updateArrow()
{
    if (expanded) {
        arrowLabel->set_markup("<small>\xe2\x96\xbe</small>  " + groupLabel_); // ▾
    } else {
        arrowLabel->set_markup("<small>\xe2\x96\xb8</small>  " + groupLabel_); // ▸
    }
}

void ToolGroup::setResetVisible(bool visible)
{
    if (visible) {
        resetBtn->set_no_show_all(false);
        resetBtn->set_visible(true);
        resetBtn->show_all();
    } else {
        resetBtn->set_visible(false);
        resetBtn->set_no_show_all(true);
    }
}

void ToolGroup::setResetCallback(std::function<void()> cb)
{
    resetCallback_ = cb;
}

void ToolGroup::addHeaderWidget(Gtk::Widget& widget)
{
    headerRow->pack_end(widget, Gtk::PACK_SHRINK, 0);
    widget.show_all();
}
