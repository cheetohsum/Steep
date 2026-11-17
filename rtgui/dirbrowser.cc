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
#include "dirbrowser.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "rtengine/rt_math.h"

#include "backgroundtasks.h"
#include "cachemanager.h"
#include "guiutils.h"
#include "rtimage.h"
#include "rtsurface.h"
#include "multilangmgr.h"
#include "options.h"
#include "thumbnail.h"
#include "cacheimagedata.h"
#include "imagescanhelpers.h"

#include <algorithm>
#include <mutex>
#include <thread>

#include <glib/gstdio.h>
#include <glibmm/checksum.h>

// Subclass TreeView to intercept motion/leave events BEFORE the base class
// handler (which returns TRUE when hover_selection is enabled, consuming events).
// The Glib::ObjectBase("DirTreeView") call registers a new GType so gtkmm
// installs class closures that dispatch to our virtual function overrides.
class DirTreeView : public Gtk::TreeView {
public:
    DirTreeView() : Glib::ObjectBase("DirTreeView") {}

    std::function<bool(GdkEventMotion*)> onMotion;
    std::function<bool(GdkEventCrossing*)> onLeave;
    // Returns true to consume the press (blocks row activation) — used for
    // the inline favorite star on the current folder row.
    std::function<bool(GdkEventButton*)> onButtonPress;

    // Clip-reveal state: set by DirBrowser when expanding
    std::string revealParentPath;
    double revealFraction = 1.0;

protected:
    bool on_button_press_event(GdkEventButton* event) override {
        if (onButtonPress && onButtonPress(event)) {
            return true;
        }
        return Gtk::TreeView::on_button_press_event(event);
    }
    bool on_motion_notify_event(GdkEventMotion* event) override {
        if (onMotion) onMotion(event);
        return Gtk::TreeView::on_motion_notify_event(event);
    }
    bool on_leave_notify_event(GdkEventCrossing* event) override {
        if (onLeave) onLeave(event);
        return Gtk::TreeView::on_leave_notify_event(event);
    }
    bool on_draw(const Cairo::RefPtr<Cairo::Context>& cr) override {
        if (!revealParentPath.empty() && revealFraction < 1.0) {
            Gtk::TreePath parentPath(revealParentPath);
            // Get the parent row's background area to find its bottom edge
            Gdk::Rectangle parentRect;
            get_background_area(parentPath, *get_column(0), parentRect);
            int revealTop = parentRect.get_y() + parentRect.get_height();

            // Find total children height by checking the last child
            auto parentIter = get_model()->get_iter(parentPath);
            int totalChildH = 0;
            if (parentIter && !parentIter->children().empty()) {
                auto lastChild = parentIter->children().end();
                --lastChild;
                Gdk::Rectangle lastRect;
                get_background_area(get_model()->get_path(lastChild), *get_column(0), lastRect);
                totalChildH = (lastRect.get_y() + lastRect.get_height()) - revealTop;
            }

            int revealH = static_cast<int>(totalChildH * revealFraction);
            int w = get_allocated_width();
            int h = get_allocated_height();

            cr->save();
            // Clip: everything above children area + revealed portion of children
            cr->rectangle(0, 0, w, revealTop + revealH);
            // Plus everything below the children area (other rows)
            if (revealTop + totalChildH < h) {
                cr->rectangle(0, revealTop + totalChildH, w, h - (revealTop + totalChildH));
            }
            cr->clip();
            bool result = Gtk::TreeView::on_draw(cr);
            cr->restore();
            return result;
        }
        return Gtk::TreeView::on_draw(cr);
    }
};

// The folder's name, and under it (folder previews) a strip of a few of its
// photos. Before the strip has loaded the row already reserves its height and
// shows faint tiles, so rows do not jump when the pictures land.
class FolderNameRenderer : public Gtk::CellRendererText {
public:
    // Tile edge in logical px for Options::folderPreviewSize 0..3.
    static int tileFor(int size) { return size <= 0 ? 18 : size == 1 ? 26 : size == 2 ? 36 : 52; }
    static constexpr int kGap = 3;
    static constexpr int kAbove = 2;    // between the name and the strip
    static constexpr int kBelow = 5;

    FolderNameRenderer() : Glib::ObjectBase("FolderNameRenderer") {}

    // Set by the cell data function before every measure and render.
    int slots = 0;
    Glib::RefPtr<Gdk::Pixbuf> strip;    // device pixels, `scale` per logical px
    int scale = 1;
    int tile = 24;                      // logical px, square
    Gdk::RGBA placeholderInk;

    int visibleRight = 0;               // right edge of the visible tree, tree px
    // Height of the name band (the shared sidebar row height). Never set a
    // fixed size on this renderer: that would override the strip's height.
    int nameRowH = 0;

    // The strip starts under the row's chevron, not under the name.
    static constexpr int kLead = 16;

protected:
    // One line at the chosen size; what does not fit is cut by the sidebar's
    // edge (a narrower sidebar shows fewer photos, never smaller ones).
    int stripHeight() const
    {
        return slots > 0 ? kAbove + tile + kBelow : 0;
    }

    void get_preferred_height_vfunc(Gtk::Widget& widget, int& minimum, int& natural) const override
    {
        Gtk::CellRendererText::get_preferred_height_vfunc(widget, minimum, natural);
        minimum = std::max(minimum, nameRowH) + stripHeight();
        natural = std::max(natural, nameRowH) + stripHeight();
    }

    void get_preferred_height_for_width_vfunc(Gtk::Widget& widget, int width, int& minimum, int& natural) const override
    {
        Gtk::CellRendererText::get_preferred_height_for_width_vfunc(widget, width, minimum, natural);
        minimum = std::max(minimum, nameRowH) + stripHeight();
        natural = std::max(natural, nameRowH) + stripHeight();
    }

    void render_vfunc(const Cairo::RefPtr<Cairo::Context>& cr, Gtk::Widget& widget,
                      const Gdk::Rectangle& background_area, const Gdk::Rectangle& cell_area,
                      Gtk::CellRendererState flags) override
    {
        if (slots <= 0) {
            Gtk::CellRendererText::render_vfunc(cr, widget, background_area, cell_area, flags);
            return;
        }

        int textMin = 0, textH = 0;
        Gtk::CellRendererText::get_preferred_height_vfunc(widget, textMin, textH);
        const int bandH = std::max(textH, nameRowH);
        const int top = cell_area.get_y() + std::max(0, (cell_area.get_height() - bandH - stripHeight()) / 2);
        const Gdk::Rectangle textArea(cell_area.get_x(), top, cell_area.get_width(), bandH);
        Gtk::CellRendererText::render_vfunc(cr, widget, background_area, textArea, flags);

        const double x0 = std::max<double>(background_area.get_x(), cell_area.get_x() - kLead);
        const double y0 = top + bandH + kAbove;

        cr->save();
        cr->rectangle(background_area.get_x(), background_area.get_y(),
                      std::max(0, visibleRight - background_area.get_x()), background_area.get_height());
        cr->clip();

        if (strip) {
            cr->translate(x0, y0);
            cr->scale(1.0 / scale, 1.0 / scale);
            Gdk::Cairo::set_source_pixbuf(cr, strip, 0, 0);
            cr->paint();
        } else {
            cr->set_source_rgba(placeholderInk.get_red(), placeholderInk.get_green(), placeholderInk.get_blue(), 0.08);
            for (int i = 0; i < slots; ++i) {
                roundedRect(cr, x0 + i * (tile + kGap), y0, tile, tile, 3.0);
                cr->fill();
            }
        }

        cr->restore();
    }

public:
    static void roundedRect(const Cairo::RefPtr<Cairo::Context>& cr, double x, double y, double w, double h, double r)
    {
        steepui::roundedRect(cr, x, y, w, h, r);
    }
};

namespace
{

// ---------------------------------------------------------------------------
// Per-folder rating / flag / colour-label summary
// ---------------------------------------------------------------------------

struct FolderStats {
    int rank[6] = {0, 0, 0, 0, 0, 0};   // index 1..5 = stars
    int pick = 0;
    int reject = 0;
    int color[6] = {0, 0, 0, 0, 0, 0};  // index 1..5 = colour labels

    int rated() const { return rank[1] + rank[2] + rank[3] + rank[4] + rank[5]; }
    int labelled() const { return color[1] + color[2] + color[3] + color[4] + color[5]; }
    bool empty() const { return rated() == 0 && pick == 0 && reject == 0 && labelled() == 0; }

    Glib::ustring encode() const
    {
        if (empty()) {
            return Glib::ustring();
        }
        std::string s;
        for (int i = 1; i <= 5; ++i) { s += std::to_string(rank[i]); s += ' '; }
        s += std::to_string(pick); s += ' ';
        s += std::to_string(reject);
        for (int i = 1; i <= 5; ++i) { s += ' '; s += std::to_string(color[i]); }
        return s;
    }

    static FolderStats decode(const Glib::ustring& str)
    {
        FolderStats st;
        if (str.empty()) {
            return st;
        }
        int v[12] = {};
        std::sscanf(str.c_str(), "%d %d %d %d %d %d %d %d %d %d %d %d",
                    &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10], &v[11]);
        for (int i = 0; i < 5; ++i) { st.rank[i + 1] = v[i]; st.color[i + 1] = v[7 + i]; }
        st.pick = v[5];
        st.reject = v[6];
        return st;
    }
};

// Same rules the folder preview strip uses to read a photo's rating, flag and
// label without building a Thumbnail: the cache entry, then the sidecar.
void accumulateFileFlags(const Glib::ustring& fname, FolderStats& st)
{
    CacheImageData cid;
    cid.rating = 0;
    cid.pickLabel = 0;
    cid.colorLabel = 0;
    imagescan::loadCacheDataFast(fname, cid);
    imagescan::loadPP3OverlaysFast(fname, cid);

    if (cid.rating >= 1 && cid.rating <= 5) {
        ++st.rank[cid.rating];
    }
    if (cid.pickLabel > 0) {
        ++st.pick;
    } else if (cid.pickLabel < 0) {
        ++st.reject;
    }
    if (cid.colorLabel >= 1 && cid.colorLabel <= 5) {
        ++st.color[cid.colorLabel];
    }
}

// Same palette as the thumbnail colour-label glow (thumbbrowserentrybase.cc).
const double kLabelR[] = {0, 1.0, 1.0, 0.2, 0.2, 0.6};
const double kLabelG[] = {0, 0.2, 0.85, 0.8, 0.4, 0.2};
const double kLabelB[] = {0, 0.2, 0.0, 0.2, 1.0, 0.8};

enum ChipKind { CHIP_RATED, CHIP_PICK, CHIP_REJECT, CHIP_LABEL };

struct Chip {
    ChipKind kind;
    int count;
    int x;      // offset from the left of the chip strip
    int w;
    Glib::RefPtr<Pango::Layout> layout;
};

constexpr int kChipIconGap = 3;
constexpr int kChipGap = 9;

int chipIconSize(int textH)
{
    return std::max(6, textH * 7 / 10);
}

std::vector<Chip> layoutChips(Gtk::Widget& widget, const FolderStats& st, int& totalW, int& textH)
{
    std::vector<Chip> chips;
    const std::pair<ChipKind, int> items[] = {
        {CHIP_RATED, st.rated()},
        {CHIP_PICK, st.pick},
        {CHIP_REJECT, st.reject},
        {CHIP_LABEL, st.labelled()},
    };
    totalW = 0;
    textH = 0;
    Pango::AttrList attrs;
    auto scale = Pango::Attribute::create_attr_scale(0.8);
    attrs.insert(scale);
    for (const auto& it : items) {
        if (it.second <= 0) {
            continue;
        }
        Chip c;
        c.kind = it.first;
        c.count = it.second;
        c.x = 0;
        c.layout = widget.create_pango_layout(std::to_string(it.second));
        c.layout->set_attributes(attrs);
        int lw = 0, lh = 0;
        c.layout->get_pixel_size(lw, lh);
        c.w = lw;
        textH = std::max(textH, lh);
        chips.push_back(c);
    }
    const int icon = chipIconSize(textH);
    for (auto& c : chips) {
        if (totalW > 0) {
            totalW += kChipGap;
        }
        c.x = totalW;
        c.w += icon + kChipIconGap;
        totalW += c.w;
    }
    return chips;
}

void drawChipIcon(const Cairo::RefPtr<Cairo::Context>& cr, ChipKind kind, double x, double cy, double s,
                  const Gdk::RGBA& fg, const FolderStats& st)
{
    cr->save();
    cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), fg.get_alpha());
    switch (kind) {
        case CHIP_RATED: {
            const double cx = x + s / 2.0, r = s / 2.0 + 0.5, ri = r * 0.45;
            for (int i = 0; i < 10; ++i) {
                const double a = -M_PI / 2.0 + i * M_PI / 5.0;
                const double rr = (i % 2) ? ri : r;
                const double px = cx + rr * std::cos(a), py = cy + 0.08 * s + rr * std::sin(a);
                if (i == 0) {
                    cr->move_to(px, py);
                } else {
                    cr->line_to(px, py);
                }
            }
            cr->close_path();
            cr->fill();
            break;
        }
        case CHIP_PICK: {
            // Flag: pole + pennant
            const double top = cy - s / 2.0, bot = cy + s / 2.0;
            cr->set_line_width(std::max(1.0, s / 9.0));
            cr->move_to(x + s * 0.2, bot);
            cr->line_to(x + s * 0.2, top);
            cr->stroke();
            cr->move_to(x + s * 0.2, top);
            cr->line_to(x + s * 0.95, top + s * 0.28);
            cr->line_to(x + s * 0.2, top + s * 0.56);
            cr->close_path();
            cr->fill();
            break;
        }
        case CHIP_REJECT: {
            const double h = s * 0.38, cx = x + s / 2.0;
            cr->set_line_width(std::max(1.2, s / 6.5));
            cr->set_line_cap(Cairo::LINE_CAP_ROUND);
            cr->move_to(cx - h, cy - h);
            cr->line_to(cx + h, cy + h);
            cr->move_to(cx + h, cy - h);
            cr->line_to(cx - h, cy + h);
            cr->stroke();
            break;
        }
        case CHIP_LABEL: {
            int best = 1;
            for (int i = 2; i <= 5; ++i) {
                if (st.color[i] > st.color[best]) {
                    best = i;
                }
            }
            cr->set_source_rgba(kLabelR[best], kLabelG[best], kLabelB[best], 0.9);
            cr->arc(x + s / 2.0, cy, s * 0.4, 0, 2 * M_PI);
            cr->fill();
            break;
        }
    }
    cr->restore();
}

class FolderStatsRenderer : public Gtk::CellRenderer
{
public:
    FolderStatsRenderer() : Glib::ObjectBase("SteepFolderStatsRenderer"), Gtk::CellRenderer() {}

    FolderStats stats;
    Gdk::RGBA ink;          // icon + number colour (steep_text_dim)
    int nameRowH = 0;       // chips sit level with the name, not the preview strip

protected:
    void get_preferred_width_vfunc(Gtk::Widget& widget, int& minimum, int& natural) const override
    {
        int w = 0, h = 0;
        layoutChips(widget, stats, w, h);
        const int xpad = property_xpad();
        minimum = natural = w > 0 ? w + 2 * xpad : 0;
    }

    void get_preferred_height_vfunc(Gtk::Widget&, int& minimum, int& natural) const override
    {
        minimum = natural = 0;
    }

    void render_vfunc(const Cairo::RefPtr<Cairo::Context>& cr, Gtk::Widget& widget,
                      const Gdk::Rectangle&, const Gdk::Rectangle& cell_area,
                      Gtk::CellRendererState) override
    {
        int totalW = 0, textH = 0;
        auto chips = layoutChips(widget, stats, totalW, textH);
        if (chips.empty()) {
            return;
        }
        const int xpad = property_xpad();
        const int x0 = cell_area.get_x() + cell_area.get_width() - xpad - totalW;
        const double cy = cell_area.get_y() + (nameRowH > 0 ? nameRowH : cell_area.get_height()) / 2.0;
        const int icon = chipIconSize(textH);
        const Gdk::RGBA& fg = ink;
        for (const auto& c : chips) {
            drawChipIcon(cr, c.kind, x0 + c.x, cy, icon, fg, stats);
            cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), fg.get_alpha());
            cr->move_to(x0 + c.x + icon + kChipIconGap, cy - textH / 2.0);
            c.layout->show_in_cairo_context(cr);
        }
    }
};

Glib::ustring chipTooltipMarkup(ChipKind kind, const FolderStats& st)
{
    Glib::ustring out;
    switch (kind) {
        case CHIP_RATED:
            for (int r = 5; r >= 1; --r) {
                if (st.rank[r] == 0) {
                    continue;
                }
                if (!out.empty()) {
                    out += "\n";
                }
                Glib::ustring stars;
                for (int i = 0; i < 5; ++i) {
                    stars += i < r ? "★" : "☆";
                }
                out += Glib::ustring::compose("%1  %2", stars, st.rank[r]);
            }
            break;
        case CHIP_PICK:
            out = Glib::ustring::compose("%1  %2", Glib::Markup::escape_text(M("FILEBROWSER_POPUPPICK")), st.pick);
            break;
        case CHIP_REJECT:
            out = Glib::ustring::compose("%1  %2", Glib::Markup::escape_text(M("FILEBROWSER_POPUPREJECT")), st.reject);
            break;
        case CHIP_LABEL:
            for (int i = 1; i <= 5; ++i) {
                if (st.color[i] == 0) {
                    continue;
                }
                if (!out.empty()) {
                    out += "\n";
                }
                char hex[8];
                std::snprintf(hex, sizeof(hex), "#%02x%02x%02x",
                              int(kLabelR[i] * 255), int(kLabelG[i] * 255), int(kLabelB[i] * 255));
                out += Glib::ustring::compose("<span foreground=\"%1\">●</span>  %2", hex, st.color[i]);
            }
            break;
    }
    return out;
}

} // namespace

#ifdef _WIN32
#include "rtengine/leanwindows.h"
#include <shellapi.h>
#endif // _WIN32

namespace
{

std::vector<Glib::ustring> listSubDirs (const Glib::RefPtr<Gio::File>& dir, bool addHidden)
{
    std::vector<Glib::ustring> subDirs;

    try {

        const Glib::ustring dirPath = dir->get_path ();

        // CD-ROM with no disc inserted are reported, but do not exist.
        if (!Glib::file_test (dirPath, Glib::FILE_TEST_EXISTS)) {
            return subDirs;
        }

        auto enumerator = dir->enumerate_children ("standard::name,standard::type,standard::is-hidden");

        while (true) {
            try {
                auto file = enumerator->next_file ();
                if (!file) {
                    break;
                }

                const Glib::ustring fileName = file->get_name ();
                // The Windows GIO backend can occasionally omit standard::type even
                // when it was requested, and get_file_type() warns in that case.
                const bool isDir = file->has_attribute (G_FILE_ATTRIBUTE_STANDARD_TYPE)
                    ? file->get_file_type () == Gio::FILE_TYPE_DIRECTORY
                    : Glib::file_test (Glib::build_filename (dirPath, fileName), Glib::FILE_TEST_IS_DIR);

                if (!isDir) {
                    continue;
                }

                if (!addHidden
                        && file->has_attribute (G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN)
                        && file->is_hidden ()) {
                    continue;
                }

                subDirs.push_back (fileName);
            } catch (const Glib::Exception& exception) {

                if (rtengine::settings->verbose) {
                    std::cerr << exception.what().c_str() << std::endl;
                }

            }
        }

    } catch (const Glib::Exception& exception) {

        if (rtengine::settings->verbose) {
            std::cerr << "Failed to list subdirectories of \"" << dir->get_parse_name() << "\": " << exception.what () << std::endl;
        }

    }

    return subDirs;
}

}

DirBrowser::DirBrowser () : dirTreeModel(),
    dtColumns(),
    tvc(M("DIRBROWSER_FOLDERS")),

    openfolder("folder-open-small"),
    closedfolder("folder-closed-small"),
    icdrom("device-optical"),
    ifloppy("device-floppy"),
    ihdd("device-hdd"),
    inetwork("device-network"),
    iremovable("device-usb"),

    expandSuccess(false),
    reuseLoadedDirs_(false)
#ifdef _WIN32
    , volumes(0)
#endif
{
    set_orientation(Gtk::ORIENTATION_VERTICAL);
    dirtree = Gtk::manage ( new DirTreeView() );
    scrolledwindow4 = Gtk::manage ( new Gtk::ScrolledWindow() );
    crt = Gtk::manage(new FolderNameRenderer());
    crt->property_ellipsize() = Pango::ELLIPSIZE_END;
    crt->property_max_width_chars() = 22;

//   dirtree->set_flags(Gtk::CAN_FOCUS);
    dirtree->set_headers_visible(false);
    dirtree->set_rules_hint(false);
    dirtree->set_reorderable(false);
    dirtree->set_enable_search(false);
    dirtree->set_show_expanders(false);
    dirtree->set_level_indentation(10);
    dirtree->set_hover_selection(true);
    dirtree->set_activate_on_single_click(true);
    scrolledwindow4->set_can_focus(true);
    scrolledwindow4->set_shadow_type(Gtk::SHADOW_NONE);
    scrolledwindow4->set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    scrolledwindow4->set_min_content_height(150);
    scrolledwindow4->property_window_placement().set_value(Gtk::CORNER_TOP_LEFT);
    scrolledwindow4->add(*dirtree);

    dirtree->set_name("DirBrowserTree");

    pack_start (*scrolledwindow4);
    dirtree->show ();
    scrolledwindow4->show ();

    // Hover thumbnail popup
    popupVisible_ = false;
    hoverSession_ = 0;
    hoverPopup_ = new Gtk::Window(Gtk::WINDOW_POPUP);
    hoverPopup_->set_type_hint(Gdk::WINDOW_TYPE_HINT_TOOLTIP);
    hoverPopup_->set_name("DirHoverPopup");

    hoverBox_ = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 3));
    for (int i = 0; i < 5; i++) {
        hoverImages_[i] = Gtk::manage(new Gtk::Image());
        hoverImages_[i]->set_size_request(48, 48);
        hoverImages_[i]->set_no_show_all(true);
        hoverBox_->pack_start(*hoverImages_[i], Gtk::PACK_SHRINK);
    }
    hoverPopup_->add(*hoverBox_);
    hoverBox_->show();

    // Hook into DirTreeView's virtual method overrides for motion/leave
    dirtree->add_events(Gdk::POINTER_MOTION_MASK | Gdk::LEAVE_NOTIFY_MASK);
    dirtree->onMotion = [this](GdkEventMotion* event) { return onMotionNotify(event); };
    dirtree->onLeave = [this](GdkEventCrossing* event) { return onLeaveNotify(event); };

    dirtree->set_has_tooltip(true);
    dirtree->signal_query_tooltip().connect(sigc::mem_fun(*this, &DirBrowser::onQueryTooltip));
    flagsChangedConn_ = folderFlagsChanged().connect(sigc::mem_fun(*this, &DirBrowser::queueFolderStatsRefresh));

    // Hide popup on scroll
    scrolledwindow4->get_vadjustment()->signal_value_changed().connect([this]() {
        hideHoverPopup();
    });

    previewSettingsChanged().connect(sigc::mem_fun(*this, &DirBrowser::refreshFolderPreviews));

}

DirBrowser::~DirBrowser()
{
    dirUpdateConn_.disconnect();
    previewApplyConn_.disconnect();
    statRefreshConn_.disconnect();
    flagsChangedConn_.disconnect();
    hideHoverPopup();
    delete hoverPopup_;
    idle_register.destroy();
}

// Generate rotated chevron pixbufs: frame 0 = right (0°), frame count = down (90°)
static std::vector<Glib::RefPtr<Gdk::Pixbuf>> generateChevronFrames(int count, const Gdk::RGBA& ink)
{
    const int sz = 16;
    const double cx = sz / 2.0, cy = sz / 2.0;
    std::vector<Glib::RefPtr<Gdk::Pixbuf>> frames;
    frames.reserve(count + 1);
    for (int i = 0; i <= count; ++i) {
        auto surface = Cairo::ImageSurface::create(Cairo::FORMAT_ARGB32, sz, sz);
        auto cr = Cairo::Context::create(surface);
        double angle = (M_PI / 2.0) * i / count; // 0 to 90°
        cr->translate(cx, cy);
        cr->rotate(angle);
        cr->translate(-cx, -cy);
        cr->set_source_rgba(ink.get_red(), ink.get_green(), ink.get_blue(), 1.0);
        cr->set_line_width(1.5);
        cr->set_line_cap(Cairo::LINE_CAP_ROUND);
        cr->set_line_join(Cairo::LINE_JOIN_ROUND);
        cr->move_to(6, 3.5);
        cr->line_to(10.5, 8);
        cr->line_to(6, 12.5);
        cr->stroke();
        frames.push_back(Gdk::Pixbuf::create(surface, 0, 0, sz, sz));
    }
    return frames;
}

// Draw a small 5-point star, filled (favorited) or outlined (not yet).
// Ink comes from the theme so the star matches whatever palette is active.
static Glib::RefPtr<Gdk::Pixbuf> generateStarPixbuf(bool filled, const Gdk::RGBA& ink)
{
    const int sz = 16;
    auto surface = Cairo::ImageSurface::create(Cairo::FORMAT_ARGB32, sz, sz);
    auto cr = Cairo::Context::create(surface);
    const double cx = sz / 2.0, cy = sz / 2.0 + 0.5;
    const double rOuter = 6.0, rInner = 2.6;
    for (int i = 0; i < 10; ++i) {
        double r = (i % 2 == 0) ? rOuter : rInner;
        double a = -M_PI / 2.0 + i * M_PI / 5.0;
        double x = cx + r * std::cos(a);
        double y = cy + r * std::sin(a);
        if (i == 0) cr->move_to(x, y); else cr->line_to(x, y);
    }
    cr->close_path();
    if (filled) {
        cr->set_source_rgba(ink.get_red(), ink.get_green(), ink.get_blue(), 1.0);
        cr->fill_preserve();
        cr->set_source_rgba(ink.get_red() * 0.8, ink.get_green() * 0.8, ink.get_blue() * 0.8, 1.0);
        cr->set_line_width(1.0);
        cr->stroke();
    } else {
        cr->set_source_rgba(ink.get_red(), ink.get_green(), ink.get_blue(), 0.9);
        cr->set_line_width(1.2);
        cr->set_line_join(Cairo::LINE_JOIN_ROUND);
        cr->stroke();
    }
    return Gdk::Pixbuf::create(surface, 0, 0, sz, sz);
}

sigc::signal<void>& DirBrowser::favoritesChanged ()
{
    static sigc::signal<void> sig;
    return sig;
}

bool DirBrowser::isDirFavorite (const Glib::ustring& dir) const
{
    if (dir.empty()) {
        return false;
    }
    for (const auto& fav : App::get().options().favoriteDirs) {
        if (isSamePath(fav, dir)) {
            return true;
        }
    }
    return false;
}

bool DirBrowser::isCurrentDirFavorite () const
{
    return isDirFavorite(currentDir_);
}

void DirBrowser::toggleCurrentDirFavorite ()
{
    toggleDirFavorite(currentDir_);
}

void DirBrowser::toggleDirFavorite (const Glib::ustring& dir)
{
    if (dir.empty()) {
        return;
    }

    auto& favs = App::get().mut_options().favoriteDirs;
    bool removed = false;
    for (auto it = favs.begin(); it != favs.end(); ++it) {
        if (isSamePath(*it, dir)) {
            favs.erase(it);
            removed = true;
            break;
        }
    }
    if (!removed) {
        favs.push_back(dir);
    }

    try {
        Options::saveAsync();
    } catch (Options::Error& error) {
        std::cerr << "Failed to save options: " << error.get_msg() << std::endl;
    }

    dirtree->queue_draw();
    favoritesChanged().emit();
}

void DirBrowser::fillDirTree ()
{
    //Create the Tree model:
    ++previewGeneration_;
    dirTreeModel = Gtk::TreeStore::create(dtColumns);
    // A freshly built tree has nothing marked yet; callers re-sync afterwards.
    currentDir_.clear();
    currentRowRef_ = Gtk::TreeRowReference();
    dirTreeModel->set_sort_func(
        dtColumns.filename,
        [this](const Gtk::TreeModel::iterator& first, const Gtk::TreeModel::iterator& second) -> int
        {
            const Glib::ustring firstName = first ? first->get_value(dtColumns.filename) : Glib::ustring();
            const Glib::ustring secondName = second ? second->get_value(dtColumns.filename) : Glib::ustring();

            if (firstName == secondName) {
                return 0;
            }

            if (firstName.empty()) {
                return 1;
            }

            if (secondName.empty()) {
                return -1;
            }

            return firstName.raw().compare(secondName.raw()) < 0 ? -1 : 1;
        });
    dirtree->set_model (dirTreeModel);

    fillRoot ();

    // Generate chevron rotation frames (Cairo-drawn)
    // Theme ink rather than a fixed grey, which vanished on light themes.
    // GTK has no "theme changed" signal of its own, so re-read the token
    // whenever the style is invalidated and rebuild only if it moved.
    const auto rebuildChevrons = [this](const Gdk::RGBA& ink) {
        chevronFrames_ = generateChevronFrames(6, ink);
        chevronRightPixbuf_ = chevronFrames_.front();
        chevronDownPixbuf_ = chevronFrames_.back();
    };
    const Gdk::RGBA chevronInk = themeColor(*dirtree, "steep_text_dim", Gdk::RGBA("#8a92a1"));
    rebuildChevrons(chevronInk);
    const auto lastChevronInk = std::make_shared<Gdk::RGBA>(chevronInk);
    dirtree->signal_style_updated().connect([this, rebuildChevrons, lastChevronInk]() {
        const Gdk::RGBA ink = themeColor(*dirtree, "steep_text_dim", Gdk::RGBA("#8a92a1"));

        if (ink != *lastChevronInk) {
            *lastChevronInk = ink;
            rebuildChevrons(ink);
            starFilledPixbuf_ = generateStarPixbuf(true, themeColor(*this, "steep_accent", Gdk::RGBA("#f2c240")));
            starHollowPixbuf_ = generateStarPixbuf(false, ink);
            crt->placeholderInk = themeColor(*dirtree, "steep_text", Gdk::RGBA("#d0d4dc"));
            dirtree->queue_draw();
        }
    });

    // Chevron expand/collapse indicator
    Gtk::CellRendererPixbuf* chevronCR = Gtk::manage(new Gtk::CellRendererPixbuf());
    chevronCR->property_ypad() = 0;
    chevronCR->property_xpad() = 0;
    tvc.pack_start(*chevronCR, false);
    tvc.set_cell_data_func(*chevronCR, [this](Gtk::CellRenderer* cr, const Gtk::TreeModel::iterator& iter) {
        auto* pbCR = static_cast<Gtk::CellRendererPixbuf*>(cr);
        paintCurrentDirBackground(cr, iter);
        auto path = dirTreeModel->get_path(iter);
        if (path.empty()) {
            pbCR->property_pixbuf() = chevronRightPixbuf_;
            return;
        }
        auto pathStr = path.to_string();
        auto it = chevronAnimFrame_.find(pathStr);
        if (it != chevronAnimFrame_.end()) {
            int frame = std::max(0, std::min(it->second, static_cast<int>(chevronFrames_.size()) - 1));
            pbCR->property_pixbuf() = chevronFrames_[frame];
        } else {
            bool expanded = dirtree->row_expanded(path);
            pbCR->property_pixbuf() = expanded ? chevronDownPixbuf_ : chevronRightPixbuf_;
        }
    });

    crt->placeholderInk = themeColor(*dirtree, "steep_text", Gdk::RGBA("#d0d4dc"));
    tvc.pack_start (*crt, false);
    tvc.set_cell_data_func(*crt, [this](Gtk::CellRenderer* cr, const Gtk::TreeModel::iterator& iter) {
        auto* textCR = static_cast<FolderNameRenderer*>(cr);
        paintCurrentDirBackground(cr, iter);
        textCR->property_text() = iter ? iter->get_value(dtColumns.filename) : Glib::ustring();
        textCR->property_weight() = isCurrentDirRow(iter) ? Pango::WEIGHT_BOLD : Pango::WEIGHT_NORMAL;
        const bool previews = iter && App::get().options().folderPreviews;
        textCR->slots = previews ? iter->get_value(dtColumns.previewSlots) : 0;
        textCR->strip = previews ? iter->get_value(dtColumns.previewStrip) : Glib::RefPtr<Gdk::Pixbuf>();
        textCR->scale = std::max(1, dirtree->get_scale_factor());
        textCR->tile = FolderNameRenderer::tileFor(App::get().options().folderPreviewSize);
        const auto hadj = scrolledwindow4->get_hadjustment();
        textCR->visibleRight = hadj && hadj->get_page_size() > 0
                               ? static_cast<int>(hadj->get_value() + hadj->get_page_size())
                               : dirtree->get_allocated_width();
    });

    // Favorite star, right of the name on the row under the pointer only:
    // filled when the folder is a starred place, hollow to offer starring
    // it. Clicking it toggles (see the onButtonPress hook below).
    starFilledPixbuf_ = generateStarPixbuf(true, themeColor(*this, "steep_accent", Gdk::RGBA("#f2c240")));
    starHollowPixbuf_ = generateStarPixbuf(false, themeColor(*this, "steep_text_dim", Gdk::RGBA("#9ea4b2")));
    starCR_ = Gtk::manage(new Gtk::CellRendererPixbuf());
    starCR_->property_ypad() = 0;
    starCR_->property_xpad() = 4;
    tvc.pack_start(*starCR_, false);
    tvc.set_cell_data_func(*starCR_, [this](Gtk::CellRenderer* cr, const Gtk::TreeModel::iterator& iter) {
        auto* pbCR = static_cast<Gtk::CellRendererPixbuf*>(cr);
        paintCurrentDirBackground(cr, iter);
        const Glib::ustring rowDir = iter ? iter->get_value(dtColumns.dirname) : Glib::ustring();
        const bool hovered = iter && !rowDir.empty() && !hoverRow_.empty()
                             && dirTreeModel->get_path(iter) == hoverRow_;
        // Always measured, so the row does not reflow as the pointer moves.
        pbCR->property_pixbuf() = hovered ? (isDirFavorite(rowDir) ? starFilledPixbuf_ : starHollowPixbuf_)
                                          : Glib::RefPtr<Gdk::Pixbuf>();
        pbCR->property_width() = 16 + 2 * 4;
    });

    Gtk::CellRendererText* countCR = Gtk::manage(new Gtk::CellRendererText());
    countCR->property_foreground_rgba() = themeColor(*dirtree, "steep_text_dim", Gdk::RGBA("#888888"));
    countCR->property_xalign() = 1.0;
    countCR->property_ypad() = 0;
    countCR->property_scale() = 0.8;
    countCR->property_xpad() = 8;
    tvc.pack_end(*countCR, false);
    tvc.set_cell_data_func(*countCR, [this](Gtk::CellRenderer* cr, const Gtk::TreeModel::iterator& iter) {
        auto* textCR = static_cast<Gtk::CellRendererText*>(cr);
        paintCurrentDirBackground(cr, iter);
        textCR->property_text() = iter ? iter->get_value(dtColumns.photoCount) : Glib::ustring();
    });

    // Rating / flag / reject / label chips, left of the photo count.
    // Hovering a chip shows its breakdown (onQueryTooltip).
    auto* statsCR = Gtk::manage(new FolderStatsRenderer());
    statsCR->property_xpad() = 2;
    statsCR->property_ypad() = 0;
    statsCR->ink = themeColor(*dirtree, "steep_text_dim", Gdk::RGBA("#888888"));
    statsCR_ = statsCR;
    tvc.pack_end(*statsCR, false);
    tvc.set_cell_data_func(*statsCR, [this](Gtk::CellRenderer* cr, const Gtk::TreeModel::iterator& iter) {
        paintCurrentDirBackground(cr, iter);
        static_cast<FolderStatsRenderer*>(cr)->stats =
            FolderStats::decode(iter ? iter->get_value(dtColumns.flagStats) : Glib::ustring());
    });

    dirtree->append_column(tvc);

    const auto& options = App::get().options();
    tvc.set_sort_order(options.dirBrowserSortType);
    tvc.set_sort_column(dtColumns.filename);
    tvc.set_sort_indicator(true);
    tvc.set_clickable();

    dirTreeModel->set_sort_column(dtColumns.filename, options.dirBrowserSortType);

    crt->property_ypad() = 0;

    // One name-row height for every sidebar list. A row with a preview strip
    // is taller than its name; the chevron, star and count stay level with
    // the name (centred in that first band) instead of centring on the row.
    int lineH = 0;
    const int rowH = steepui::listRowHeight(*dirtree, &lineH);
    crt->nameRowH = rowH;
    chevronCR->property_yalign() = 0.f;
    chevronCR->property_ypad() = std::max(0, (rowH - 16) / 2);
    starCR_->property_yalign() = 0.f;
    starCR_->property_ypad() = std::max(0, (rowH - 16) / 2);
    countCR->property_yalign() = 0.f;
    // The count is set at 0.8 scale (above).
    countCR->property_ypad() = std::max(0, (rowH - static_cast<int>(lineH * 0.8)) / 2);
    static_cast<FolderStatsRenderer*>(statsCR_)->nameRowH = rowH;

    // Clicking the star on the current folder row toggles its favorite
    // status instead of re-activating the row.
    dirtree->onButtonPress = [this](GdkEventButton* ev) -> bool {
        if (ev->button == 3 && ev->type == GDK_BUTTON_PRESS) {
            showPreviewMenu(ev);
            return true;
        }
        if (ev->button != 1 || ev->type != GDK_BUTTON_PRESS || !starCR_ || !dirTreeModel) {
            return false;
        }
        Gtk::TreeModel::Path path;
        Gtk::TreeViewColumn* col = nullptr;
        int cx = 0, cy = 0;
        if (!dirtree->get_path_at_pos(static_cast<int>(ev->x), static_cast<int>(ev->y), path, col, cx, cy)) {
            return false;
        }
        auto iter = dirTreeModel->get_iter(path);
        if (!iter || iter->get_value(dtColumns.dirname).empty()) {
            return false;
        }
        // Resolve the star cell's x range within the row
        tvc.cell_set_cell_data(dirTreeModel, iter, false, dirtree->row_expanded(path));
        Gdk::Rectangle cellArea;
        dirtree->get_cell_area(path, tvc, cellArea);
        int startPos = 0, width = 0;
        if (!tvc.get_cell_position(*starCR_, startPos, width) || width <= 0) {
            return false;
        }
        const int x0 = cellArea.get_x() + startPos;
        if (ev->x >= x0 && ev->x < x0 + width) {
            toggleDirFavorite(iter->get_value(dtColumns.dirname));
            return true;
        }
        return false;
    };

    dirtree->signal_row_expanded().connect(sigc::mem_fun(*this, &DirBrowser::row_expanded));
    dirtree->signal_row_collapsed().connect(sigc::mem_fun(*this, &DirBrowser::row_collapsed));
    dirtree->signal_row_activated().connect(sigc::mem_fun(*this, &DirBrowser::row_activated));
    dirTreeModel->signal_sort_column_changed().connect(sigc::mem_fun(*this, &DirBrowser::on_sort_column_changed));
}

#ifdef _WIN32
void DirBrowser::addRoot (char letter)
{

    char volume[4];
    volume[0] = letter;
    strcpy (volume + 1, ":\\");

    Gtk::TreeModel::iterator root = dirTreeModel->append();
    root->set_value (dtColumns.filename, Glib::ustring(volume));
    root->set_value (dtColumns.dirname, Glib::ustring(volume));
    root->set_value (dtColumns.childrenLoaded, false);

    int type = GetDriveType (volume);

    if (type == DRIVE_CDROM) {
        root->set_value (dtColumns.icon_name, icdrom);
    } else if (type == DRIVE_REMOVABLE) {
        if (letter - 'A' < 2) {
            root->set_value (dtColumns.icon_name, ifloppy);
        } else {
            root->set_value (dtColumns.icon_name, iremovable);
        }
    } else if (type == DRIVE_REMOTE) {
        root->set_value (dtColumns.icon_name, inetwork);
    } else if (type == DRIVE_FIXED) {
        root->set_value (dtColumns.icon_name, ihdd);
    }

    Gtk::TreeModel::iterator child = dirTreeModel->append (root->children());
    child->set_value (dtColumns.filename, Glib::ustring("foo"));
    child->set_value (dtColumns.childrenLoaded, false);
}

void DirBrowser::updateDirTreeRoot ()
{

    for (Gtk::TreeModel::iterator i = dirTreeModel->children().begin(); i != dirTreeModel->children().end(); ++i) {
        updateDirTree (i);
    }
}

void DirBrowser::updateDirTree (const Gtk::TreeModel::iterator& iter)
{

    if (dirtree->row_expanded (dirTreeModel->get_path (iter))) {
        updateDir (iter);

        for (Gtk::TreeModel::iterator i = iter->children().begin(); i != iter->children().end(); ++i) {
            updateDirTree (i);
        }
    }
}

void DirBrowser::updateVolumes ()
{

    unsigned int nvolumes = GetLogicalDrives ();

    if (nvolumes != volumes) {
        GThreadLock lock;

        for (int i = 0; i < 32; i++)
            if (((volumes >> i) & 1) && !((nvolumes >> i) & 1)) { // volume i has been deleted
                for (Gtk::TreeModel::iterator iter = dirTreeModel->children().begin(); iter != dirTreeModel->children().end(); ++iter)
                    if (iter->get_value (dtColumns.filename).c_str()[0] - 'A' == i) {
                        dirTreeModel->erase (iter);
                        break;
                    }
            } else if (!((volumes >> i) & 1) && ((nvolumes >> i) & 1)) {
                addRoot ('A' + i);    // volume i has been added
            }

        volumes = nvolumes;
    }
}

int updateVolumesUI (void* br)
{
    (static_cast<DirBrowser*>(br))->updateVolumes ();
    return 1;
}

#endif

void DirBrowser::fillRoot ()
{

#ifdef _WIN32
    volumes = GetLogicalDrives ();

    for (int i = 0; i < 32; i++)
        if ((volumes >> i) & 1) {
            addRoot ('A' + i);
        }

    // since sigc++ is not thread safe, we have to use the glib function
    g_timeout_add (5000, updateVolumesUI, this);
#else
    Gtk::TreeModel::Row rootRow = *(dirTreeModel->append());
    rootRow[dtColumns.filename] = "/";
    rootRow[dtColumns.dirname] = "/";
    rootRow[dtColumns.childrenLoaded] = false;
    Gtk::TreeModel::Row childRow = *(dirTreeModel->append(rootRow.children()));
    childRow[dtColumns.filename] = "foo";
    childRow[dtColumns.childrenLoaded] = false;
#endif
}

void DirBrowser::on_sort_column_changed() const
{
    auto& options = App::get().mut_options();
    options.dirBrowserSortType = tvc.get_sort_order();
}

void DirBrowser::browseForFolder ()
{
    Gtk::Window* toplevel = dynamic_cast<Gtk::Window*>(get_toplevel());
    if (!toplevel) return;

    Gtk::FileChooserDialog fc(*toplevel, M("DIRBROWSER_BROWSE"), Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER);
    fc.set_name("RTFileChooser");
    fc.set_default_size(780, 520);

    // Styling lives in themes/common/widgets.css (#RTFileChooser)

    fc.add_button(M("GENERAL_CANCEL"), Gtk::RESPONSE_CANCEL);
    fc.add_button(M("GENERAL_OK"), Gtk::RESPONSE_OK);
    fc.set_default_response(Gtk::RESPONSE_OK);

    int result = fc.run();

    if (result == Gtk::RESPONSE_OK) {
        Glib::ustring dir = fc.get_filename();
        if (!dir.empty()) {
            open(dir);
        }
    }
}

void DirBrowser::row_expanded (const Gtk::TreeModel::iterator& iter, const Gtk::TreeModel::Path& path)
{

    expandSuccess = false;

    if (reuseLoadedDirs_ && iter->get_value(dtColumns.childrenLoaded)) {
        expandSuccess = true;

        if (iter->get_value(dtColumns.icon_name) == closedfolder || iter->get_value(dtColumns.icon_name) == "") {
            iter->set_value(dtColumns.icon_name, openfolder);
        }

        return;
    }

    // We will disable model's sorting because it decreases speed of inserting new items
    // in list tree dramatically. Therefore will do:
    // 1) Disable sorting in model
    // 2) Manually sort data in the order determined by the options
    // 3) Enable sorting in model again for UI (sorting by click on header)
    int prevSortColumn;
    Gtk::SortType prevSortType;
    dirTreeModel->get_sort_column_id(prevSortColumn, prevSortType);
    dirTreeModel->set_sort_column(Gtk::TreeSortable::DEFAULT_UNSORTED_COLUMN_ID, Gtk::SORT_ASCENDING);

    const auto& options = App::get().options();
    auto dir = Gio::File::create_for_path (iter->get_value (dtColumns.dirname));
    auto subDirs = listSubDirs (dir, options.fbShowHidden);

    Gtk::TreeNodeChildren children = iter->children();
    std::list<Gtk::TreeIter> forErase(children.begin(), children.end());

    std::sort (subDirs.begin (), subDirs.end (), [&](const Glib::ustring& firstDir, const Glib::ustring& secondDir)
    {
        switch (options.dirBrowserSortType) {
        default:
        case Gtk::SORT_ASCENDING:
            return firstDir.raw().compare(secondDir.raw()) < 0;
        case Gtk::SORT_DESCENDING:
            return firstDir.raw().compare(secondDir.raw()) > 0;
        }
    });

    for (auto it = subDirs.begin(), end = subDirs.end(); it != end; ++it) {
        addDir(iter, *it);
    }

    for (auto it = forErase.begin(), end = forErase.end(); it != end; ++it) {
        dirTreeModel->erase(*it);
    }

    dirTreeModel->set_sort_column(prevSortColumn, prevSortType);

    expandSuccess = true;

    // Update row icon (only if row icon is not a volume one or is empty)
    if (iter->get_value(dtColumns.icon_name) == closedfolder || iter->get_value(dtColumns.icon_name) == "") {
        iter->set_value(dtColumns.icon_name, openfolder);
    }

    Glib::RefPtr<Gio::FileMonitor> monitor = dir->monitor_directory(Gio::FileMonitorFlags::FILE_MONITOR_WATCH_MOVES);
    iter->set_value (dtColumns.monitor, monitor);
    monitor->signal_changed().connect (sigc::bind(sigc::mem_fun(*this, &DirBrowser::file_changed), iter, dir->get_parse_name()));
    iter->set_value(dtColumns.childrenLoaded, true);

    countPhotosInChildren(iter);
}

void DirBrowser::row_collapsed (const Gtk::TreeModel::iterator& iter, const Gtk::TreeModel::Path& path)
{
    // Update row icon (only if row icon is not a volume one)
    if (iter->get_value(dtColumns.icon_name) == openfolder) {
        iter->set_value(dtColumns.icon_name, closedfolder);
    }
}

void DirBrowser::updateDir (const Gtk::TreeModel::iterator& iter)
{

    // first test if some files are deleted
    bool change = true;

    while (change) {
        change = false;

        for (Gtk::TreeModel::iterator it = iter->children().begin(); it != iter->children().end(); ++it)
            if (!Glib::file_test (it->get_value (dtColumns.dirname), Glib::FILE_TEST_EXISTS)
                    || !Glib::file_test (it->get_value (dtColumns.dirname), Glib::FILE_TEST_IS_DIR)) {
                GThreadLock lock;
                dirTreeModel->erase (it);
                change = true;
                break;
            }
    }

    // test if new files are created
    auto dir = Gio::File::create_for_path (iter->get_value (dtColumns.dirname));
    auto subDirs = listSubDirs (dir, App::get().options().fbShowHidden);

    for (size_t i = 0; i < subDirs.size(); i++) {
        bool found = false;

        for (Gtk::TreeModel::iterator it = iter->children().begin(); it != iter->children().end() && !found ; ++it) {
            found = (it->get_value (dtColumns.filename) == subDirs[i]);
        }

        if (!found) {
            GThreadLock lock;
            addDir (iter, subDirs[i]);
        }
    }

    iter->set_value(dtColumns.childrenLoaded, true);
}

void DirBrowser::addDir (const Gtk::TreeModel::iterator& iter, const Glib::ustring& dirname)
{

    Gtk::TreeModel::iterator child = dirTreeModel->append(iter->children());
    child->set_value (dtColumns.filename, dirname);
    child->set_value (dtColumns.icon_name, closedfolder);
    Glib::ustring fullname = Glib::build_filename (iter->get_value (dtColumns.dirname), dirname);
    child->set_value (dtColumns.dirname, fullname);
    child->set_value (dtColumns.childrenLoaded, false);
    Gtk::TreeModel::iterator fooRow = dirTreeModel->append(child->children());
    fooRow->set_value (dtColumns.filename, Glib::ustring("foo"));
    fooRow->set_value (dtColumns.childrenLoaded, false);
}

void DirBrowser::row_activated (const Gtk::TreeModel::Path& path, Gtk::TreeViewColumn* column)
{
    if (path.empty()) {
        return;
    }

    Gtk::TreeModel::iterator iter = dirTreeModel->get_iter(path);
    if (!iter) {
        return;
    }

    Glib::ustring dname = iter->get_value (dtColumns.dirname);

    if (Glib::file_test (dname, Glib::FILE_TEST_IS_DIR)) {
        // Clicking a folder opens it, so it becomes the row to keep marked.
        setCurrentDir (dname, path);
        dirSelectionSignal (dname, Glib::ustring());

        // A fold cancelled in mid-flight leaves the height capped; lift the
        // cap before measuring. Nothing is laid out in between, so the new
        // animation still starts from the height on screen.
        if (expandAnim_.running()) {
            expandAnim_.stop();
            scrolledwindow4->set_max_content_height(-1);
        }

        const auto liftHeightCap = [this]() {
            scrolledwindow4->set_max_content_height(-1);
        };
        bool wasExpanded = dirtree->row_expanded(path);

        if (wasExpanded) {
            // Start chevron collapse animation
            auto pathStr = path.to_string();
            clearReveal();
            animateChevron(pathStr, false);

            // Collapse with animation
            const int startH = scrolledwindow4->get_allocated_height();
            dirtree->collapse_row(path);
            int minH = 0, natH = 0;
            dirtree->get_preferred_height(minH, natH);
            const int targetH = std::max(natH + 4, scrolledwindow4->get_min_content_height());
            // Clamp to current if natural is larger (scrolled content)
            if (targetH >= startH) {
                // Content is still larger than viewport, no height animation needed
                return;
            }
            scrolledwindow4->set_propagate_natural_height(true);
            // Only the maximum moves here: the tree can be far taller than
            // the window, so its height must never become a minimum.
            expandAnim_ = steep::motion::animatePaneHeight(*scrolledwindow4, startH, targetH, false, liftHeightCap);
        } else {
            // Start chevron expand animation + child reveal
            auto pathStr = path.to_string();
            animateChevron(pathStr, true);
            animateReveal(pathStr);

            // Expand with animation
            const int startH = scrolledwindow4->get_allocated_height();
            dirtree->expand_row(path, false);
            int minH = 0, natH = 0;
            dirtree->get_preferred_height(minH, natH);
            const int targetH = natH + 4;
            if (targetH <= startH) {
                // Content fits, no animation needed
                return;
            }
            scrolledwindow4->set_propagate_natural_height(true);
            expandAnim_ = steep::motion::animatePaneHeight(*scrolledwindow4, startH, targetH, false, liftHeightCap);
        }
    }
}

void DirBrowser::animateChevron(const std::string& pathStr, bool expanding)
{
    const int last = static_cast<int>(chevronFrames_.size()) - 1;
    // A chevron still turning carries on from the frame it is showing.
    const auto it = chevronAnimFrame_.find(pathStr);
    const int startFrame = it != chevronAnimFrame_.end() ? it->second : (expanding ? 0 : last);
    const int endFrame = expanding ? last : 0;
    chevronAnimFrame_[pathStr] = startFrame;

    const int duration = last > 0
        ? steep::motion::ms(steep::motion::Duration::Micro) * std::abs(endFrame - startFrame) / last
        : 0;
    chevronAnims_[pathStr] = steep::motion::run(*dirtree, startFrame, endFrame, duration, steep::motion::Ease::Move,
        [this, pathStr](double frame) {
            chevronAnimFrame_[pathStr] = static_cast<int>(std::lround(frame));
            dirtree->queue_draw();
        },
        [this, pathStr]() {
            // Back to the static open/closed pixbuf.
            chevronAnimFrame_.erase(pathStr);
            dirtree->queue_draw();
        });
}

void DirBrowser::animateReveal(const std::string& parentPathStr)
{
    revealParentPath_ = parentPathStr;
    dirtree->revealParentPath = parentPathStr;
    dirtree->revealFraction = 0.0;
    revealAnim_ = steep::motion::run(*dirtree, 0.0, 1.0, steep::motion::Duration::Standard, steep::motion::Ease::Enter,
        [this](double fraction) {
            dirtree->revealFraction = fraction;
            dirtree->queue_draw();
        },
        [this]() {
            revealParentPath_.clear();
            dirtree->revealParentPath.clear();
            dirtree->revealFraction = 1.0;
            dirtree->queue_draw();
        });
}

void DirBrowser::clearReveal()
{
    revealAnim_.stop();
    revealParentPath_.clear();
    dirtree->revealParentPath.clear();
    dirtree->revealFraction = 1.0;
}

Gtk::TreePath DirBrowser::expandToDir (const Glib::ustring& absDirPath)
{

    Gtk::TreeModel::Path path;
    path.push_back(0);

    char* dcpy = strdup (absDirPath.c_str());
    char* dir = strtok (dcpy, "/\\");
#ifdef _WIN32
    int count = 0;
#endif
    expandSuccess = true;

#ifndef _WIN32
    Gtk::TreeModel::iterator j = dirTreeModel->get_iter (path);
    path.up ();
    path.push_back (0);
    row_expanded(j, path);
    path.push_back (0);
#endif

    while (dir) {
        Glib::ustring dirstr = dir;
#ifdef _WIN32

        if (count == 0) {
            dirstr = dirstr + "\\";
        }

#endif
        Gtk::TreeModel::iterator i = dirTreeModel->get_iter (path);
        int ix = 0;

        while (i && expandSuccess) {
            Gtk::TreeModel::Row crow = *i;
            Glib::ustring str = crow[dtColumns.filename];
#ifdef _WIN32

            if (str.casefold() == dirstr.casefold()) {
#else

            if (str == dirstr) {
#endif
                path.up ();
                path.push_back (ix);
                row_expanded(i, path);
                path.push_back (0);
                break;
            }

            ++ix;
            ++i;
        }
#ifdef _WIN32
        count++;
#endif
        dir = strtok(nullptr, "/\\");
    }

    free(dcpy);

    path.up ();
    if (!path.empty()) {
        dirtree->expand_to_path (path);
    }

    return path;
}

bool DirBrowser::isSamePath (const Glib::ustring& a, const Glib::ustring& b)
{
    if (a.empty() || b.empty()) {
        return false;
    }

#ifdef _WIN32
    return a.casefold() == b.casefold();
#else
    return a == b;
#endif
}

bool DirBrowser::isCurrentDirRow (const Gtk::TreeModel::iterator& iter) const
{
    return iter && isSamePath(iter->get_value(dtColumns.dirname), currentDir_);
}

// Paints the open folder's row so it stays marked even though hover selection
// keeps moving (and clearing) the GTK selection. The tint is dropped while the
// row is itself selected, so the two highlights never stack.
void DirBrowser::paintCurrentDirBackground (Gtk::CellRenderer* renderer, const Gtk::TreeModel::iterator& iter) const
{
    bool tint = isCurrentDirRow(iter);

    if (tint && dirTreeModel) {
        const auto selection = dirtree->get_selection();

        if (selection && selection->is_selected(dirTreeModel->get_path(iter))) {
            tint = false;
        }
    }

    renderer->property_cell_background_set() = tint;

    if (tint) {
        Gdk::RGBA hover = themeColor(*this, "steep_accent", Gdk::RGBA("#64a0ff"));
        hover.set_alpha(0.25);
        renderer->property_cell_background_rgba() = hover;
    }
}

void DirBrowser::setCurrentDir (const Glib::ustring& absDirPath, const Gtk::TreeModel::Path& path)
{
    currentDir_ = absDirPath;
    currentRowRef_ = (dirTreeModel && !path.empty())
        ? Gtk::TreeRowReference(dirTreeModel, path)
        : Gtk::TreeRowReference();

    dirtree->queue_draw();
}

Glib::ustring DirBrowser::highlightDirInternal (const Glib::ustring& dirname, bool collapseTree)
{
    if (!dirTreeModel) {
        return Glib::ustring();
    }

    // WARNING & TODO: One should test here if the directory/file has R/W access permission to avoid crash

    Glib::RefPtr<Gio::File> dir = Gio::File::create_for_path(dirname);

    if( !dir->query_exists()) {
        return Glib::ustring();
    }

    Glib::ustring absDirPath = dir->get_parse_name ();

    // Only skip the walk when the row we last expanded to is still in the tree;
    // a rebuilt or pruned model has to be expanded again.
    const bool alreadyHighlighted = isSamePath(currentDir_, absDirPath) && currentRowRef_.is_valid();

    if (alreadyHighlighted && !collapseTree) {
        return absDirPath;
    }

    if (collapseTree) {
        dirtree->collapse_all ();
    }

    reuseLoadedDirs_ = !collapseTree;
    Gtk::TreePath path = expandToDir (absDirPath);
    reuseLoadedDirs_ = false;

    setCurrentDir (absDirPath, path);

    if (!path.empty()) {
        dirtree->scroll_to_row (path);
        dirtree->get_selection()->select (path);
    }

    return absDirPath;
}

void DirBrowser::highlightDir (const Glib::ustring& dirname, bool collapseTree)
{
    highlightDirInternal(dirname, collapseTree);
}

Glib::ustring DirBrowser::getHighlightedDir () const
{
    return currentDir_;
}

void DirBrowser::open (const Glib::ustring& dirname, const Glib::ustring& fileName, bool collapseTree)
{
    const Glib::ustring absDirPath = highlightDirInternal(dirname, collapseTree);

    if (absDirPath.empty()) {
        return;
    }

    Glib::ustring absFilePath;

    if (!fileName.empty()) {
        absFilePath = Glib::build_filename (absDirPath, fileName);
    }

    dirSelectionSignal (absDirPath, absFilePath);
}

void DirBrowser::file_changed (const Glib::RefPtr<Gio::File>& file, const Glib::RefPtr<Gio::File>& other_file, Gio::FileMonitorEvent event_type, const Gtk::TreeModel::iterator& iter, const Glib::ustring& dirName)
{
    // file is the file that is/was in the monitored directory. other_file is
    // null if only one file is involved (create/delete events), the file that
    // is/was in another directory, or the new name for a renamed file. We want
    // to inspect the file type of the changed file, so we decide which file to
    // use based on the event type.
    const Glib::RefPtr<Gio::File> current_file =
        (event_type == Gio::FILE_MONITOR_EVENT_MOVED ||
            event_type == Gio::FILE_MONITOR_EVENT_RENAMED ||
            event_type == Gio::FILE_MONITOR_EVENT_MOVED_OUT)
            ? other_file
            : file;

    // No need to update the directory if the even type is not rename, move,
    // create, or delete, or if the file is not a directory.
    if (!current_file ||
        event_type == Gio::FILE_MONITOR_EVENT_CHANGED ||
        event_type == Gio::FILE_MONITOR_EVENT_CHANGES_DONE_HINT ||
        event_type == Gio::FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED ||
        event_type == Gio::FILE_MONITOR_EVENT_PRE_UNMOUNT ||
        (event_type != Gio::FILE_MONITOR_EVENT_DELETED &&
            event_type != Gio::FILE_MONITOR_EVENT_UNMOUNTED &&
            !Glib::file_test(current_file->get_path(), Glib::FILE_TEST_IS_DIR))) {
        return;
    }

    scheduleUpdateDir (iter);
}

void DirBrowser::scheduleUpdateDir (const Gtk::TreeModel::iterator& iter)
{
    // Monitor events come in bursts (imports, tools churning temp files,
    // sidecar/cache writes). Rescanning synchronously for each one pinned the
    // GUI thread inside updateDir; coalesce per directory row instead.
    const Glib::ustring dirName = iter->get_value (dtColumns.dirname);
    pendingDirUpdates_[dirName.raw()] = Gtk::TreeRowReference (dirTreeModel, dirTreeModel->get_path (iter));

    if (!dirUpdateConn_.connected()) {
        dirUpdateConn_ = Glib::signal_timeout().connect (
            [this]() {
                flushPendingDirUpdates();
                return false;
            },
            250);
    }
}

void DirBrowser::flushPendingDirUpdates ()
{
    std::map<std::string, Gtk::TreeRowReference> pending;
    pending.swap (pendingDirUpdates_);

    for (const auto& entry : pending) {
        if (!entry.second.is_valid()) {
            continue;
        }

        Gtk::TreeModel::iterator iter = dirTreeModel->get_iter (entry.second.get_path());

        if (iter) {
            updateDir (iter);
        }
    }
}

void DirBrowser::selectDir (Glib::ustring dir)
{

    open (dir, "");
}

bool DirBrowser::onMotionNotify(GdkEventMotion* event)
{
    {
        Gtk::TreeModel::Path row;
        Gtk::TreeViewColumn* col = nullptr;
        int cx = 0, cy = 0;
        if (!dirtree->get_path_at_pos(static_cast<int>(event->x), static_cast<int>(event->y), row, col, cx, cy)) {
            row = Gtk::TreeModel::Path();
        }
        if (row != hoverRow_) {
            hoverRow_ = row;
            dirtree->queue_draw();
        }
    }

    // Over a chip its tooltip takes the place of the thumbnail popup.
    if (statsChipAt(static_cast<int>(event->x), static_cast<int>(event->y), nullptr, nullptr, nullptr)) {
        hideHoverPopup();
        return false;
    }

    // The rows already show their photos; the popup would repeat them.
    if (App::get().options().folderPreviews) {
        hideHoverPopup();
        return false;
    }

    Gtk::TreeModel::Path path;
    Gtk::TreeViewColumn* column = nullptr;
    int cell_x, cell_y;

    if (dirtree->get_path_at_pos(static_cast<int>(event->x), static_cast<int>(event->y),
                                  path, column, cell_x, cell_y)) {
        if (hoveredPath_.empty() || path != hoveredPath_) {
            hoveredPath_ = path;

            if (popupVisible_) {
                // Popup already showing — update immediately for new row
                showHoverPopup(path);
            } else {
                // Start/restart delay timer — fires for whichever row is
                // current when it expires (not the row at connect time)
                hoverTimer_.disconnect();
                hoverTimer_ = Glib::signal_timeout().connect([this]() -> bool {
                    if (!hoveredPath_.empty()) {
                        showHoverPopup(hoveredPath_);
                    }
                    return false;
                }, 300);
            }
        }
    } else {
        hideHoverPopup();
    }
    return false;  // don't consume — let hover_selection work
}

bool DirBrowser::onLeaveNotify(GdkEventCrossing*)
{
    if (!hoverRow_.empty()) {
        hoverRow_ = Gtk::TreeModel::Path();
        dirtree->queue_draw();
    }
    hideHoverPopup();
    return false;
}

void DirBrowser::showHoverPopup(const Gtk::TreeModel::Path& path)
{
    if (path.empty() || !dirTreeModel) return;

    auto iter = dirTreeModel->get_iter(path);
    if (!iter) return;

    Glib::ustring dirname = (*iter)[dtColumns.dirname];
    if (dirname.empty()) return;

    // Position popup to the right of the sidebar, aligned with the row
    Gdk::Rectangle cellArea;
    dirtree->get_cell_area(path, *dirtree->get_column(0), cellArea);

    int root_x = 0, root_y = 0;
    auto binWin = dirtree->get_bin_window();
    if (binWin) {
        int bwx, bwy;
        binWin->get_origin(bwx, bwy);
        root_x = bwx;
        root_y = bwy + cellArea.get_y();
    }

    // Place to the right of the entire DirBrowser widget
    int sidebarW = get_allocated_width();
    int popupX = root_x + sidebarW + 4;
    int popupY = root_y;

    // Clear all images
    for (int i = 0; i < 5; i++) {
        hoverImages_[i]->clear();
        hoverImages_[i]->hide();
    }

    hoverPopup_->move(popupX, popupY);
    hoverPopup_->show();
    popupVisible_ = true;

    int session = ++hoverSession_;
    steep::runBackground([this, dirname, session]() { loadHoverThumbnails(dirname, session); });
}

void DirBrowser::hideHoverPopup()
{
    hoverTimer_.disconnect();
    hoveredPath_ = Gtk::TreeModel::Path();
    if (popupVisible_) {
        hoverPopup_->hide();
        popupVisible_ = false;
    }
    ++hoverSession_;
}

void DirBrowser::loadHoverThumbnails(const Glib::ustring& dirname, int session)
{
    std::vector<Glib::ustring> imageFiles;
    try {
        Glib::Dir dir(dirname);
        const auto& exts = App::get().options().parsedExtensionsSet;
        for (auto it = dir.begin(); it != dir.end() && imageFiles.size() < 5; ++it) {
            const Glib::ustring& fname = *it;
            auto dotpos = fname.find_last_of('.');
            if (dotpos != Glib::ustring::npos) {
                Glib::ustring ext = fname.substr(dotpos + 1).lowercase();
                if (exts.count(ext.raw())) {
                    imageFiles.push_back(Glib::build_filename(dirname, fname));
                }
            }
        }
    } catch (...) {
        return;
    }

    if (imageFiles.empty()) {
        Glib::signal_idle().connect_once([this, session]() {
            if (session != hoverSession_) return;
            hideHoverPopup();
        });
        return;
    }

    for (size_t i = 0; i < imageFiles.size(); i++) {
        if (session != hoverSession_) return;

        const Glib::ustring& fpath = imageFiles[i];
        Glib::RefPtr<Gdk::Pixbuf> pixbuf;

        try {
            Thumbnail* thm = CacheManager::getInstance()->getEntry(fpath);
            if (thm) {
                double scale = 1.0;
                rtengine::IImage8* img = thm->processThumbImage(48, scale);
                if (img) {
                    auto pb = Gdk::Pixbuf::create_from_data(
                        img->getData(), Gdk::COLORSPACE_RGB, false, 8,
                        img->getWidth(), img->getHeight(), img->getWidth() * 3);
                    pixbuf = pb->copy();
                    delete img;
                }
                thm->decreaseRef();
            }
        } catch (...) {}

        if (!pixbuf) continue;

        int idx = static_cast<int>(i);
        Glib::signal_idle().connect_once([this, session, idx, pixbuf]() {
            if (session != hoverSession_ || !popupVisible_) return;
            hoverImages_[idx]->set(pixbuf);
            hoverImages_[idx]->show();
        });
    }
}

void DirBrowser::countPhotosInChildren (const Gtk::TreeModel::iterator& parent)
{
    struct Entry {
        Glib::ustring dirname;
        Glib::ustring pathStr;
    };
    std::vector<Entry> entries;

    for (auto child = parent->children().begin(); child != parent->children().end(); ++child) {
        Glib::ustring dirname = (*child)[dtColumns.dirname];
        if (!dirname.empty()) {
            entries.push_back({dirname, dirTreeModel->get_path(child).to_string()});
        }
    }

    {
        std::vector<ScanEntry> statEntries;
        for (const auto& e : entries) {
            statEntries.push_back({e.dirname, e.pathStr});
        }
        scanFolderStats(std::move(statEntries));
    }

    // DirBrowser is created once and lives for the lifetime of the app,
    // so capturing `this` in the idle callback is safe. Avoid capturing a
    // Glib::RefPtr<Gtk::TreeStore> into the background thread — a crash
    // observed there (refptr copy ctor reading 0xffffffffffffffff) appeared
    // to come from the captured RefPtr's internal pointer being clobbered,
    // so we just use the stable `this` on the main thread instead.
    DirBrowser* self = this;
    const int generation = previewGeneration_;
    const bool previews = App::get().options().folderPreviews;
    const int previewCount = std::max(1, std::min(5, App::get().options().folderPreviewCount));
    const int scale = std::max(1, dirtree->get_scale_factor());
    const int tileSize = FolderNameRenderer::tileFor(App::get().options().folderPreviewSize);
    const bool preferRated = App::get().options().folderPreviewsPreferRated;
    const Glib::ustring cacheDir = Glib::build_filename(App::get().options().cacheBaseDir, "folderpreviews");

    steep::runBackground([entries, self, generation, previews, previewCount, scale, tileSize, preferRated, cacheDir]() {
        struct Result {
            Glib::ustring pathStr;
            Glib::ustring dirname;
            Glib::ustring countStr;
            int slots;
        };
        auto results = std::make_shared<std::vector<Result>>();
        results->reserve(entries.size());
        std::vector<std::vector<std::string>> photos(entries.size());

        for (size_t e = 0; e < entries.size(); ++e) {
            const auto& entry = entries[e];
            try {
                Glib::Dir dir(entry.dirname);
                const auto& exts = App::get().options().parsedExtensionsSet;
                for (auto it = dir.begin(); it != dir.end(); ++it) {
                    const Glib::ustring& fname = *it;
                    auto dotpos = fname.find_last_of('.');
                    if (dotpos != Glib::ustring::npos) {
                        Glib::ustring ext = fname.substr(dotpos + 1).lowercase();
                        if (exts.count(ext.raw())) {
                            photos[e].push_back(fname.raw());
                        }
                    }
                }
            } catch (...) {}

            const int count = static_cast<int>(photos[e].size());
            results->push_back({entry.pathStr, entry.dirname,
                                count > 0 ? Glib::ustring(std::to_string(count)) : Glib::ustring(),
                                previews ? std::min(count, previewCount) : 0});
        }

        // Counts first: they are cheap, and the slots they set reserve each
        // row's strip before any picture is decoded.
        Glib::signal_idle().connect_once([self, results, generation]() {
            const bool current = generation == self->previewGeneration_;
            for (const auto& r : *results) {
                try {
                    Gtk::TreeModel::Path path(r.pathStr);
                    auto iter = self->dirTreeModel->get_iter(path);
                    if (iter && iter->get_value(self->dtColumns.dirname) == r.dirname) {
                        (*iter)[self->dtColumns.photoCount] = r.countStr;
                        if (current) {
                            (*iter)[self->dtColumns.previewSlots] = r.slots;
                        }
                    }
                } catch (...) {}
            }
        });

        if (!previews) {
            return;
        }

        // One strip at a time across every expanded folder: the thumbnails
        // compete with the file browser for the disk and the cache. The
        // serial queue also keeps the second I/O thread free for counts and
        // hover thumbnails while a long run of strips is being built.
        static steep::SerialQueue* const stripQueue = new steep::SerialQueue(steep::Lane::IO);
        constexpr int kRatedScanLimit = 2000;
        const auto photoLists = std::make_shared<std::vector<std::vector<std::string>>>(std::move(photos));

        stripQueue->enqueue([entries, self, generation, previewCount, scale, tileSize, preferRated, cacheDir, photoLists]() {
            std::vector<std::vector<std::string>>& photos = *photoLists;

            for (size_t e = 0; e < entries.size(); ++e) {
                std::vector<std::string>& names = photos[e];
                const int n = static_cast<int>(names.size());
                if (n == 0) {
                    continue;
                }
                if (generation != self->previewGeneration_) {
                    return;
                }

                // Spread through the folder in name order: the middle of each of
                // k equal runs, so 300 photos give 30, 90, 150, 210 and 270.
                std::sort(names.begin(), names.end());
                const int k = std::min(n, previewCount);
                const auto spread = [](int count, int want) {
                    std::vector<int> picks;
                    for (int j = 0; j < want; ++j) {
                        picks.push_back((2 * j + 1) * count / (2 * want));
                    }
                    return picks;
                };

                std::vector<int> indices;
                // Past a couple of thousand photos the per-photo rating scan
                // costs more than the strip is worth: space them evenly.
                if (preferRated && n <= kRatedScanLimit) {
                    // Starred or picked photos first, spread the same way through
                    // the folder; rejects never show. Evenly spaced others fill
                    // whatever slots the rated ones leave.
                    std::vector<int> rated, others;
                    for (int i = 0; i < n; ++i) {
                        // Two small reads per photo: on a big folder that is
                        // a long scan, so let a superseded one stop early.
                        if (i % 200 == 199 && generation != self->previewGeneration_) {
                            return;
                        }
                        const Glib::ustring path = Glib::build_filename(entries[e].dirname.raw(), names[i]);
                        CacheImageData cid;
                        cid.rating = 0;
                        cid.pickLabel = 0;
                        imagescan::loadCacheDataFast(path, cid);
                        imagescan::loadPP3OverlaysFast(path, cid);
                        if (cid.pickLabel < 0) {
                            continue;
                        }
                        (cid.rating > 0 || cid.pickLabel > 0 ? rated : others).push_back(i);
                    }
                    const int ratedCount = static_cast<int>(rated.size());
                    for (int r : spread(ratedCount, std::min(k, ratedCount))) {
                        indices.push_back(rated[r]);
                    }
                    const int othersCount = static_cast<int>(others.size());
                    const int fill = std::min(k - static_cast<int>(indices.size()), othersCount);
                    for (int o : spread(othersCount, fill)) {
                        indices.push_back(others[o]);
                    }
                    std::sort(indices.begin(), indices.end());
                } else {
                    indices = spread(n, k);
                }

                if (indices.empty()) {
                    continue;
                }
                const int shown = static_cast<int>(indices.size());
                std::vector<std::string> chosen;
                for (int i : indices) {
                    chosen.push_back(Glib::build_filename(entries[e].dirname.raw(), names[i]));
                }

                // The key covers what the strip shows: the files, their edits
                // (sidecar times) and the tile size, so a stale strip is never
                // read back.
                std::string identity = std::to_string(scale) + "|" + std::to_string(tileSize);
                for (const auto& f : chosen) {
                    identity += "|" + f;
                    for (const std::string& p : {f, f + ".pp3"}) {
                        GStatBuf st;
                        if (g_stat(p.c_str(), &st) == 0) {
                            identity += ":" + std::to_string(static_cast<long long>(st.st_mtime)) + ":" + std::to_string(static_cast<long long>(st.st_size));
                        }
                    }
                }
                const std::string cacheFile = Glib::build_filename(cacheDir.raw(),
                    Glib::Checksum::compute_checksum(Glib::Checksum::CHECKSUM_MD5, identity) + ".png");

                Glib::RefPtr<Gdk::Pixbuf> strip;
                if (Glib::file_test(cacheFile, Glib::FILE_TEST_EXISTS)) {
                    try {
                        strip = Gdk::Pixbuf::create_from_file(cacheFile);
                    } catch (...) {}
                }

                if (!strip) {
                        if (generation != self->previewGeneration_) {
                        return;
                    }

                    const int tile = tileSize * scale;
                    const int gap = FolderNameRenderer::kGap * scale;
                    const int width = shown * tile + (shown - 1) * gap;
                    auto surface = Cairo::ImageSurface::create(Cairo::FORMAT_ARGB32, width, tile);
                    auto cr = Cairo::Context::create(surface);
                    int drawn = 0;

                    for (int j = 0; j < shown; ++j) {
                        Glib::RefPtr<Gdk::Pixbuf> thumb;
                        try {
                            Thumbnail* thm = CacheManager::getInstance()->getEntry(chosen[j]);
                            if (thm) {
                                double thumbScale = 1.0;
                                // Twice the tile: a portrait frame still covers
                                // the square after the crop.
                                rtengine::IImage8* img = thm->processThumbImage(tile * 2, thumbScale);
                                if (img) {
                                    thumb = Gdk::Pixbuf::create_from_data(
                                        img->getData(), Gdk::COLORSPACE_RGB, false, 8,
                                        img->getWidth(), img->getHeight(), img->getWidth() * 3)->copy();
                                    delete img;
                                }
                                thm->decreaseRef();
                            }
                        } catch (...) {}

                        if (!thumb || thumb->get_width() < 1 || thumb->get_height() < 1) {
                            continue;
                        }

                        // Cover-crop into the square, corners rounded.
                        const double x = j * (tile + gap);
                        const double cover = std::max(double(tile) / thumb->get_width(), double(tile) / thumb->get_height());
                        cr->save();
                        FolderNameRenderer::roundedRect(cr, x, 0, tile, tile, 3.0 * scale);
                        cr->clip();
                        cr->translate(x + (tile - thumb->get_width() * cover) / 2.0, (tile - thumb->get_height() * cover) / 2.0);
                        cr->scale(cover, cover);
                        Gdk::Cairo::set_source_pixbuf(cr, thumb, 0, 0);
                        cr->paint();
                        cr->restore();
                        ++drawn;
                    }

                    if (drawn == 0) {
                        continue;
                    }

                    surface->flush();
                    strip = Gdk::Pixbuf::create(surface, 0, 0, width, tile);
                    try {
                        g_mkdir_with_parents(cacheDir.c_str(), 0755);
                        strip->save(cacheFile, "png");
                    } catch (...) {}
                }

                const Glib::ustring pathStr = entries[e].pathStr;
                const Glib::ustring dirname = entries[e].dirname;
                Glib::signal_idle().connect_once([self, strip, pathStr, dirname, generation]() {
                    if (generation != self->previewGeneration_) {
                        return;
                    }
                    try {
                        auto iter = self->dirTreeModel->get_iter(Gtk::TreeModel::Path(pathStr));
                        if (iter && iter->get_value(self->dtColumns.dirname) == dirname) {
                            (*iter)[self->dtColumns.previewStrip] = strip;
                        }
                    } catch (...) {}
                });
            }
        });
    });
}

void DirBrowser::refreshFolderPreviews ()
{
    ++previewGeneration_;
    if (!dirTreeModel) {
        return;
    }

    // Drop every strip, then recount the folders that are on show; the
    // count pass sets the new slots and loads the new strips.
    std::vector<Gtk::TreeModel::iterator> expanded;
    dirTreeModel->foreach_iter([this, &expanded](const Gtk::TreeModel::iterator& iter) -> bool {
        (*iter)[dtColumns.previewSlots] = 0;
        (*iter)[dtColumns.previewStrip] = Glib::RefPtr<Gdk::Pixbuf>();
        if (iter->get_value(dtColumns.childrenLoaded) && dirtree->row_expanded(dirTreeModel->get_path(iter))) {
            expanded.push_back(iter);
        }
        return false;
    });

    for (const auto& iter : expanded) {
        countPhotosInChildren(iter);
    }
    dirtree->columns_autosize();
    hideHoverPopup();
}

sigc::signal<void>& DirBrowser::previewSettingsChanged ()
{
    static sigc::signal<void> sig;
    return sig;
}

void DirBrowser::setPreviewOptions (bool show, int count, int size, bool preferRated)
{
    auto& options = App::get().mut_options();
    options.folderPreviewsPreferRated = preferRated;
    options.folderPreviews = show;
    options.folderPreviewCount = std::max(1, std::min(5, count));
    options.folderPreviewSize = std::max(0, std::min(3, size));

    try {
        Options::saveAsync();
    } catch (Options::Error& error) {
        std::cerr << "Failed to save options: " << error.get_msg() << std::endl;
    }

    previewSettingsChanged().emit();
}

void DirBrowser::openInFileManager (const Glib::ustring& dir)
{
    if (dir.empty()) {
        return;
    }

#ifdef _WIN32
    // Windows has no GIO handler for a file:// folder URI; ask the shell.
    std::string path = dir.raw();
    for (auto& c : path) {
        if (c == '/') {
            c = '\\';
        }
    }
    const std::string argument = "\"" + path + "\"";
    if (wchar_t* wide = reinterpret_cast<wchar_t*>(g_utf8_to_utf16(argument.c_str(), -1, nullptr, nullptr, nullptr))) {
        const HINSTANCE result = ShellExecuteW(nullptr, L"open", L"explorer.exe", wide, nullptr, SW_SHOWNORMAL);
        g_free(wide);
        if (reinterpret_cast<INT_PTR>(result) > 32) {
            return;
        }
    }
#endif

    try {
        Gio::AppInfo::launch_default_for_uri_async(Gio::File::create_for_path(dir)->get_uri());
    } catch (const Glib::Error& error) {
        std::cerr << "Could not open " << dir << ": " << error.what() << std::endl;
    }
}

void DirBrowser::schedulePreviewApply ()
{
    if (previewPopoverSyncing_) {
        return;
    }
    // A drag crosses several stops; apply the one it settles on.
    previewApplyConn_.disconnect();
    previewApplyConn_ = Glib::signal_timeout().connect([this]() {
        setPreviewOptions(previewSwitch_->get_active(),
                          static_cast<int>(std::lround(previewCountScale_->get_value())),
                          static_cast<int>(std::lround(previewSizeScale_->get_value())),
                          previewRatedSwitch_->get_active());
        return false;
    }, 200);
}

void DirBrowser::buildPreviewPopover ()
{
    previewPopover_.reset(new Gtk::Popover(*dirtree));
    previewPopover_->set_position(Gtk::POS_RIGHT);

    Gtk::Grid* grid = Gtk::manage(new Gtk::Grid());
    grid->set_row_spacing(6);
    grid->set_column_spacing(10);
    grid->set_border_width(10);

    Gtk::Label* showLabel = Gtk::manage(new Gtk::Label(M("DIRBROWSER_PREVIEWS_SHOW"), Gtk::ALIGN_START));
    previewSwitch_ = Gtk::manage(new Gtk::Switch());
    previewSwitch_->set_halign(Gtk::ALIGN_END);
    previewSwitch_->set_valign(Gtk::ALIGN_CENTER);
    previewSwitch_->get_style_context()->add_class("compact-switch");
    grid->attach(*showLabel, 0, 0, 1, 1);
    grid->attach(*previewSwitch_, 1, 0, 1, 1);

    // Discrete sliders: they snap to whole stops and show where they sit.
    const auto makeScale = [](double lo, double hi) {
        Gtk::Scale* scale = Gtk::manage(new Gtk::Scale(Gtk::ORIENTATION_HORIZONTAL));
        scale->set_range(lo, hi);
        scale->set_increments(1, 1);
        scale->set_round_digits(0);
        scale->set_digits(0);
        scale->set_value_pos(Gtk::POS_RIGHT);
        scale->set_hexpand(true);
        scale->set_size_request(150, -1);
        for (int v = static_cast<int>(lo); v <= static_cast<int>(hi); ++v) {
            scale->add_mark(v, Gtk::POS_BOTTOM, "");
        }
        return scale;
    };

    Gtk::Label* sizeLabel = Gtk::manage(new Gtk::Label(M("DIRBROWSER_PREVIEWS_SIZE"), Gtk::ALIGN_START));
    previewSizeScale_ = makeScale(0, 3);
    previewSizeScale_->signal_format_value().connect([](double v) -> Glib::ustring {
        static const char* names[] = {"S", "M", "L", "XL"};
        return names[std::max(0, std::min(3, static_cast<int>(std::lround(v))))];
    });
    grid->attach(*sizeLabel, 0, 1, 1, 1);
    grid->attach(*previewSizeScale_, 1, 1, 1, 1);

    Gtk::Label* countLabel = Gtk::manage(new Gtk::Label(M("DIRBROWSER_PREVIEWS_COUNT"), Gtk::ALIGN_START));
    previewCountScale_ = makeScale(1, 5);
    grid->attach(*countLabel, 0, 2, 1, 1);
    grid->attach(*previewCountScale_, 1, 2, 1, 1);

    Gtk::Label* ratedLabel = Gtk::manage(new Gtk::Label(M("DIRBROWSER_PREVIEWS_PREFERRATED"), Gtk::ALIGN_START));
    previewRatedSwitch_ = Gtk::manage(new Gtk::Switch());
    previewRatedSwitch_->set_halign(Gtk::ALIGN_END);
    previewRatedSwitch_->set_valign(Gtk::ALIGN_CENTER);
    previewRatedSwitch_->get_style_context()->add_class("compact-switch");
    grid->attach(*ratedLabel, 0, 3, 1, 1);
    grid->attach(*previewRatedSwitch_, 1, 3, 1, 1);

    grid->attach(*Gtk::manage(new Gtk::Separator(Gtk::ORIENTATION_HORIZONTAL)), 0, 4, 2, 1);

    // The browser's own Open source folder action: same icon, same words.
    Gtk::Button* openButton = Gtk::manage(new Gtk::Button(M("FILEBROWSER_POPUPOPENSOURCEFOLDER")));
    openButton->set_image(*Gtk::manage(new RTImage("folder-open-small", Gtk::ICON_SIZE_BUTTON)));
    openButton->set_always_show_image(true);
    openButton->set_relief(Gtk::RELIEF_NONE);
    openButton->set_halign(Gtk::ALIGN_FILL);
    if (auto* label = dynamic_cast<Gtk::Label*>(openButton->get_child())) {
        label->set_xalign(0.f);
    }
    grid->attach(*openButton, 0, 5, 2, 1);

    previewSwitch_->property_active().signal_changed().connect([this, sizeLabel, countLabel, ratedLabel]() {
        const bool on = previewSwitch_->get_active();
        sizeLabel->set_sensitive(on);
        countLabel->set_sensitive(on);
        ratedLabel->set_sensitive(on);
        previewSizeScale_->set_sensitive(on);
        previewCountScale_->set_sensitive(on);
        previewRatedSwitch_->set_sensitive(on);
        schedulePreviewApply();
    });
    previewRatedSwitch_->property_active().signal_changed().connect(sigc::mem_fun(*this, &DirBrowser::schedulePreviewApply));
    previewSizeScale_->signal_value_changed().connect(sigc::mem_fun(*this, &DirBrowser::schedulePreviewApply));
    previewCountScale_->signal_value_changed().connect(sigc::mem_fun(*this, &DirBrowser::schedulePreviewApply));
    openButton->signal_clicked().connect([this]() {
        previewPopover_->popdown();
        openInFileManager(previewMenuDir_);
    });

    previewPopover_->add(*grid);
    grid->show_all();
}

void DirBrowser::showPreviewMenu (GdkEventButton* event)
{
    if (!previewPopover_) {
        buildPreviewPopover();
    }

    // The folder under the pointer when the button went down: hover
    // selection moves on while the popover is open.
    previewMenuDir_.clear();
    Gtk::TreeModel::Path path;
    Gtk::TreeViewColumn* column = nullptr;
    int cx = 0, cy = 0;
    Gdk::Rectangle rect(static_cast<int>(event->x), static_cast<int>(event->y), 1, 1);
    if (dirTreeModel && dirtree->get_path_at_pos(static_cast<int>(event->x), static_cast<int>(event->y), path, column, cx, cy)) {
        if (auto iter = dirTreeModel->get_iter(path)) {
            previewMenuDir_ = iter->get_value(dtColumns.dirname);
        }
    }

    const auto& options = App::get().options();
    previewPopoverSyncing_ = true;
    previewSwitch_->set_active(options.folderPreviews);
    previewSizeScale_->set_value(options.folderPreviewSize);
    previewCountScale_->set_value(options.folderPreviewCount);
    previewRatedSwitch_->set_active(options.folderPreviewsPreferRated);
    previewPopoverSyncing_ = false;

    previewPopover_->set_pointing_to(rect);
    previewPopover_->popup();
}

// ---------------------------------------------------------------------------
// Folder rating / flag / label chips
// ---------------------------------------------------------------------------

void DirBrowser::scanFolderStats (std::vector<ScanEntry> entries)
{
    if (entries.empty()) {
        return;
    }

    // DirBrowser lives for the lifetime of the app (see countPhotosInChildren).
    DirBrowser* self = this;

    steep::runBackground([entries, self]() {
        struct Result {
            Glib::ustring dirname;
            Glib::ustring pathStr;
            Glib::ustring statsStr;
        };
        auto results = std::make_shared<std::vector<Result>>();
        results->reserve(entries.size());

        for (const auto& entry : entries) {
            FolderStats stats;
            try {
                Glib::Dir dir(entry.dirname);
                const auto& exts = App::get().options().parsedExtensionsSet;
                for (auto it = dir.begin(); it != dir.end(); ++it) {
                    const Glib::ustring fname = *it;
                    const auto dotpos = fname.find_last_of('.');
                    if (dotpos == Glib::ustring::npos) {
                        continue;
                    }
                    const Glib::ustring ext = fname.substr(dotpos + 1).lowercase();
                    if (exts.count(ext.raw())) {
                        accumulateFileFlags(Glib::build_filename(entry.dirname, fname), stats);
                    }
                }
            } catch (...) {}

            results->push_back({entry.dirname, entry.pathStr, stats.encode()});
        }

        Glib::signal_idle().connect_once([self, results]() {
            if (!self->dirTreeModel) {
                return;
            }
            for (const auto& r : *results) {
                try {
                    Gtk::TreeModel::iterator iter;
                    if (!r.pathStr.empty()) {
                        iter = self->dirTreeModel->get_iter(Gtk::TreeModel::Path(r.pathStr));
                    }
                    if (!iter || iter->get_value(self->dtColumns.dirname) != r.dirname) {
                        // Rows moved since the scan started: find the folder by name.
                        iter = Gtk::TreeModel::iterator();
                        self->dirTreeModel->foreach_iter([&](const Gtk::TreeModel::iterator& it) -> bool {
                            if (it->get_value(self->dtColumns.dirname) == r.dirname) {
                                iter = it;
                                return true;
                            }
                            return false;
                        });
                    }
                    if (iter && iter->get_value(self->dtColumns.flagStats) != r.statsStr) {
                        (*iter)[self->dtColumns.flagStats] = r.statsStr;
                    }
                } catch (...) {}
            }
        });
    });
}

sigc::signal<void, const Glib::ustring&>& DirBrowser::folderFlagsChanged ()
{
    static sigc::signal<void, const Glib::ustring&> sig;
    return sig;
}

void DirBrowser::queueFolderStatsRefresh (const Glib::ustring& dirname)
{
    pendingStatDirs_.insert(dirname.raw());
    statRefreshConn_.disconnect();
    statRefreshConn_ = Glib::signal_timeout().connect([this]() -> bool {
        std::set<std::string> wanted;
        wanted.swap(pendingStatDirs_);
        std::vector<ScanEntry> entries;
        if (dirTreeModel) {
            dirTreeModel->foreach_iter([&](const Gtk::TreeModel::iterator& it) -> bool {
                const Glib::ustring d = it->get_value(dtColumns.dirname);
                if (!d.empty()) {
                    for (const auto& w : wanted) {
                        if (isSamePath(d, Glib::ustring(w))) {
                            entries.push_back({d, dirTreeModel->get_path(it).to_string()});
                            break;
                        }
                    }
                }
                return false;
            });
        }
        scanFolderStats(std::move(entries));
        return false;
    }, 400);
}

// (bx, by) in the tree's bin-window coordinates.
bool DirBrowser::statsChipAt (int bx, int by, Gtk::TreeModel::Path* pathOut, int* chipOut, Gdk::Rectangle* rectOut)
{
    if (!statsCR_ || !dirTreeModel) {
        return false;
    }
    Gtk::TreeModel::Path path;
    Gtk::TreeViewColumn* column = nullptr;
    int cx = 0, cy = 0;
    if (!dirtree->get_path_at_pos(bx, by, path, column, cx, cy) || column != &tvc) {
        return false;
    }
    auto iter = dirTreeModel->get_iter(path);
    if (!iter) {
        return false;
    }
    const FolderStats st = FolderStats::decode(iter->get_value(dtColumns.flagStats));
    if (st.empty()) {
        return false;
    }

    // Lay the row out the way it was drawn.
    tvc.cell_set_cell_data(dirTreeModel, iter, false, dirtree->row_expanded(path));
    int start = 0, width = 0;
    if (!tvc.get_cell_position(*statsCR_, start, width) || width <= 0) {
        return false;
    }
    Gdk::Rectangle cellArea;
    dirtree->get_cell_area(path, tvc, cellArea);

    int totalW = 0, textH = 0;
    const auto chips = layoutChips(*dirtree, st, totalW, textH);
    const int xpad = statsCR_->property_xpad();
    const int x0 = cellArea.get_x() + start + width - xpad - totalW;
    for (const auto& c : chips) {
        const int left = x0 + c.x - kChipGap / 2;
        const int right = x0 + c.x + c.w + kChipGap / 2;
        if (bx >= left && bx < right) {
            if (pathOut) {
                *pathOut = path;
            }
            if (chipOut) {
                *chipOut = c.kind;
            }
            if (rectOut) {
                int tx = 0, ty = 0;
                dirtree->convert_bin_window_to_widget_coords(left, cellArea.get_y(), tx, ty);
                *rectOut = Gdk::Rectangle(tx, ty, right - left, cellArea.get_height());
            }
            return true;
        }
    }
    return false;
}

bool DirBrowser::onQueryTooltip (int x, int y, bool keyboard, const Glib::RefPtr<Gtk::Tooltip>& tooltip)
{
    if (keyboard) {
        return false;
    }
    int bx = 0, by = 0;
    dirtree->convert_widget_to_bin_window_coords(x, y, bx, by);
    Gtk::TreeModel::Path path;
    int chip = 0;
    Gdk::Rectangle rect;
    if (!statsChipAt(bx, by, &path, &chip, &rect)) {
        return false;
    }
    auto iter = dirTreeModel->get_iter(path);
    if (!iter) {
        return false;
    }
    const Glib::ustring markup =
        chipTooltipMarkup(static_cast<ChipKind>(chip), FolderStats::decode(iter->get_value(dtColumns.flagStats)));
    if (markup.empty()) {
        return false;
    }
    tooltip->set_markup(markup);
    tooltip->set_tip_area(rect);
    return true;
}
