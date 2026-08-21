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
#include "albumbrowser.h"

#include <cmath>
#include <cairomm/cairomm.h>

#include "rtengine/rt_math.h"

#include "cacheimagedata.h"
#include "cachemanager.h"
#include "guiutils.h"
#include "imagescanhelpers.h"
#include "multilangmgr.h"
#include "options.h"
#include "pathutils.h"
#include "rtimage.h"
#include "thumbnail.h"
#include "../rtengine/procparams.h"

#include <fstream>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <thread>

// Album tree area sizing: the minimum height reserved for the album list
// (its default footprint in the sidebar) and the cap it may grow to before
// scrolling kicks in.
constexpr int kAlbumTreeMinHeight = 260;
constexpr int kAlbumTreeMaxHeight = 340;

// Subclass TreeView: set_hover_selection(true) makes GTK process motion
// events internally (prelight_or_select). The virtual override intercepts
// these to track the hovered row for programmatic cell-background highlighting.
class AlbumTreeView : public Gtk::TreeView {
public:
    AlbumTreeView() : Glib::ObjectBase("AlbumTreeView") {
        add_events(Gdk::POINTER_MOTION_MASK | Gdk::LEAVE_NOTIFY_MASK);
    }

    std::function<void(const Gtk::TreeModel::Path&)> onHoverChanged;
    // Custom DnD: AlbumBrowser sets these to detect drag threshold
    std::function<void(double, double)> onDragMotion;  // called with x,y on motion while button pressed
    bool buttonDown = false;

    // Clip-reveal state: set by AlbumBrowser when expanding
    std::string revealParentPath;
    double revealFraction = 1.0;

protected:
    bool on_button_press_event(GdkEventButton* event) override {
        if (event->button == 1) buttonDown = true;
        return Gtk::TreeView::on_button_press_event(event);
    }
    bool on_button_release_event(GdkEventButton* event) override {
        if (event->button == 1) buttonDown = false;
        return Gtk::TreeView::on_button_release_event(event);
    }
    bool on_motion_notify_event(GdkEventMotion* event) override {
        Gtk::TreeModel::Path path;
        Gtk::TreeViewColumn* col;
        int cx, cy;
        if (get_path_at_pos(static_cast<int>(event->x), static_cast<int>(event->y), path, col, cx, cy)) {
            if (hoveredPath_.empty() || path != hoveredPath_) {
                hoveredPath_ = path;
                if (onHoverChanged) onHoverChanged(path);
                queue_draw();
            }
        } else if (!hoveredPath_.empty()) {
            hoveredPath_ = Gtk::TreeModel::Path();
            if (onHoverChanged) onHoverChanged(hoveredPath_);
            queue_draw();
        }
        if (buttonDown && onDragMotion) {
            onDragMotion(event->x, event->y);
        }
        return Gtk::TreeView::on_motion_notify_event(event);
    }
    bool on_leave_notify_event(GdkEventCrossing* event) override {
        if (!hoveredPath_.empty()) {
            hoveredPath_ = Gtk::TreeModel::Path();
            if (onHoverChanged) onHoverChanged(hoveredPath_);
            queue_draw();
        }
        return Gtk::TreeView::on_leave_notify_event(event);
    }
    bool on_draw(const Cairo::RefPtr<Cairo::Context>& cr) override {
        if (!revealParentPath.empty() && revealFraction < 1.0) {
            Gtk::TreePath parentPath(revealParentPath);
            Gdk::Rectangle parentRect;
            get_background_area(parentPath, *get_column(0), parentRect);
            int revealTop = parentRect.get_y() + parentRect.get_height();

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
            cr->rectangle(0, 0, w, revealTop + revealH);
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
private:
    Gtk::TreeModel::Path hoveredPath_;
};

// Generate rotated chevron pixbufs: frame 0 = right (0°), frame count = down (90°)
static std::vector<Glib::RefPtr<Gdk::Pixbuf>> generateChevronFrames(int count)
{
    const int sz = 16;
    const double cx = sz / 2.0, cy = sz / 2.0;
    std::vector<Glib::RefPtr<Gdk::Pixbuf>> frames;
    frames.reserve(count + 1);
    for (int i = 0; i <= count; ++i) {
        auto surface = Cairo::ImageSurface::create(Cairo::FORMAT_ARGB32, sz, sz);
        auto cr = Cairo::Context::create(surface);
        double angle = (M_PI / 2.0) * i / count;
        cr->translate(cx, cy);
        cr->rotate(angle);
        cr->translate(-cx, -cy);
        cr->set_source_rgba(0.6, 0.6, 0.6, 1.0);
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

// Static signal: all AlbumBrowser instances connect to this.
// When one instance saves, it emits this signal so others reload.
sigc::signal<void, AlbumBrowser*> AlbumBrowser::albumsChangedOnDisk_;

AlbumBrowser::AlbumBrowser ()
    : selectionChanging_(false), nextNodeId_(0),
      selectedNodeId_(-1), contextMenuNodeId_(-1), coverLoadSession_(0),
      firstTreeLoad_(true)
{
    set_orientation(Gtk::ORIENTATION_VERTICAL);

    // Connect to global change signal so we reload when another instance saves
    globalChangeConn_ = albumsChangedOnDisk_.connect(
        sigc::mem_fun(*this, &AlbumBrowser::onGlobalAlbumsChanged));

    // Header bar: the album icon doubles as the creation dropdown. The bar
    // sits in an event box so its empty area can be dragged to resize the
    // album list and clicked to collapse/expand it.
    Gtk::Box* headerBar = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 0));
    headerBar->set_name("AlbumHeader");

    Gtk::MenuButton* addBtn = Gtk::manage(new Gtk::MenuButton());
    addBtn->set_name("AlbumAddBtn");
    addBtn->set_relief(Gtk::RELIEF_NONE);
    addBtn->set_tooltip_text(M("ALBUM_CREATE_TOOLTIP"));
    auto* headerIcon = Gtk::manage(new RTImage("album-view-grid", Gtk::ICON_SIZE_SMALL_TOOLBAR));
    addBtn->set_image(*headerIcon);
    addBtn->set_always_show_image(true);
    addBtn->set_margin_start(2);

    addMenu_ = Gtk::manage(new Gtk::Menu());
    auto* miCreateAlbum = Gtk::manage(new Gtk::MenuItem(M("ALBUM_CREATE_ALBUM")));
    miCreateAlbum->signal_activate().connect([this]() { createAlbum(); });
    addMenu_->append(*miCreateAlbum);

    auto* miCreateSmart = Gtk::manage(new Gtk::MenuItem(M("ALBUM_CREATE_SMART")));
    miCreateSmart->signal_activate().connect([this]() { createSmartAlbum(); });
    addMenu_->append(*miCreateSmart);

    auto* miCreateFolder = Gtk::manage(new Gtk::MenuItem(M("ALBUM_CREATE_FOLDER")));
    miCreateFolder->signal_activate().connect([this]() { createFolder(); });
    addMenu_->append(*miCreateFolder);

    addMenu_->show_all();
    addBtn->set_popup(*addMenu_);

    // Close album button (hidden until an album is active)
    closeAlbumBtn_ = Gtk::manage(new Gtk::Button());
    closeAlbumBtn_->set_name("AlbumCloseBtn");
    closeAlbumBtn_->set_label("\xC3\x97"); // × character
    closeAlbumBtn_->set_relief(Gtk::RELIEF_NONE);
    closeAlbumBtn_->set_tooltip_text("Close album view");
    closeAlbumBtn_->set_no_show_all(true);
    closeAlbumBtn_->signal_clicked().connect(sigc::mem_fun(*this, &AlbumBrowser::deselectAlbum));

    headerBar->pack_start(*addBtn, Gtk::PACK_SHRINK);
    headerBar->pack_end(*closeAlbumBtn_, Gtk::PACK_SHRINK);

    headerEvtBox_ = Gtk::manage(new Gtk::EventBox());
    headerEvtBox_->set_tooltip_text(M("ALBUM_HEADER_RESIZE_TIP"));
    headerEvtBox_->add(*headerBar);
    headerEvtBox_->add_events(Gdk::BUTTON_PRESS_MASK | Gdk::BUTTON_RELEASE_MASK | Gdk::POINTER_MOTION_MASK);

    headerEvtBox_->signal_button_press_event().connect([this](GdkEventButton* ev) -> bool {
        if (ev->button != 1) {
            return false;
        }
        headerPressed_ = true;
        headerDragging_ = false;
        headerPressRootY_ = ev->y_root;
        headerPressHeight_ = App::get().options().albumPanelCollapsed
            ? 0 : std::max(scrollw_->get_allocated_height(), 0);
        headerDragHeight_ = headerPressHeight_;
        return true;
    });
    headerEvtBox_->signal_motion_notify_event().connect([this](GdkEventMotion* ev) -> bool {
        if (!headerPressed_) {
            return false;
        }
        const double dy = headerPressRootY_ - ev->y_root; // dragging up grows the list
        if (!headerDragging_ && std::abs(dy) > 4.0) {
            headerDragging_ = true;
            expandAnimConn_.disconnect();
            hideAlbumHoverPopup();
            App::get().mut_options().albumPanelCollapsed = false;
            treeView_->show();
            scrollw_->show();
        }
        if (headerDragging_) {
            const int h = std::max(std::min(headerPressHeight_ + static_cast<int>(dy), 600), 80);
            if (h != headerDragHeight_) {
                headerDragHeight_ = h;
                // Coalesce to one resize per frame: motion events outrun the
                // frame clock, and relayouting the sidebar for each of them
                // is what made the drag stutter.
                if (!headerDragApplyConn_.connected()) {
                    headerDragApplyConn_ = Glib::signal_timeout().connect([this]() -> bool {
                        scrollw_->set_min_content_height(headerDragHeight_);
                        scrollw_->set_max_content_height(headerDragHeight_);
                        return false;
                    }, 16);
                }
            }
        }
        return true;
    });
    headerEvtBox_->signal_button_release_event().connect([this](GdkEventButton* ev) -> bool {
        if (ev->button != 1 || !headerPressed_) {
            return false;
        }
        headerPressed_ = false;
        headerDragApplyConn_.disconnect();
        auto& o = App::get().mut_options();
        if (headerDragging_) {
            o.albumPanelHeight = headerDragHeight_;
            o.albumPanelCollapsed = false;
        } else {
            o.albumPanelCollapsed = !o.albumPanelCollapsed;
        }
        applyPanelSizing();
        try {
            Options::save();
        } catch (Options::Error&) {}
        return true;
    });
    // Resize affordance on the header's own (non-button) area
    headerEvtBox_->signal_realize().connect([this]() {
        headerEvtBox_->get_window()->set_cursor(
            Gdk::Cursor::create(headerEvtBox_->get_display(), "ns-resize"));
    });

    pack_start(*headerEvtBox_, Gtk::PACK_SHRINK, 0);

    // Scrolled tree view
    scrollw_ = Gtk::manage(new Gtk::ScrolledWindow());
    scrollw_->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    scrollw_->set_propagate_natural_height(true);
    scrollw_->set_min_content_height(kAlbumTreeMinHeight);
    scrollw_->set_max_content_height(kAlbumTreeMaxHeight);
    scrollw_->set_overlay_scrolling(false);
    // Visibility and height are managed by applyPanelSizing() (collapse state
    // and manual header-drag height live in options).
    scrollw_->set_no_show_all(true);

    treeView_ = Gtk::manage(new AlbumTreeView());
    treeView_->set_name("AlbumBrowserTree");
    treeView_->set_headers_visible(false);

    // Hover highlighting: set_hover_selection(true) is required for motion
    // events to reach the bin_window (and thus the virtual override).
    treeView_->set_hover_selection(true);
    treeView_->onHoverChanged = [this](const Gtk::TreeModel::Path& path) {
        hoveredPath_ = path;
        onHoverRowChanged(path);
    };

    // Hover thumbnail popup (shares DirHoverPopup styling with the folder tree)
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

    // Custom DnD: detect drag by checking motion distance while button held
    dragSourceNodeId_ = -1;
    dragActive_ = false;
    dragStartX_ = dragStartY_ = 0;
    treeView_->onDragMotion = [this](double x, double y) {
        if (dragSourceNodeId_ < 0) return;
        if (!dragActive_) {
            double dx = x - dragStartX_;
            double dy = y - dragStartY_;
            if (dx * dx + dy * dy > 25) {  // 5px threshold
                dragActive_ = true;
            }
        }
        if (dragActive_) {
            Gtk::TreeModel::Path path;
            int destNodeId = -1;
            DropAction action = computeDropAction(static_cast<int>(x), static_cast<int>(y), path, destNodeId);
            dropTargetPath_ = path;
            switch (action) {
                case DropAction::INTO_FOLDER:
                    treeView_->set_drag_dest_row(path, Gtk::TREE_VIEW_DROP_INTO_OR_AFTER);
                    break;
                case DropAction::BEFORE:
                    treeView_->set_drag_dest_row(path, Gtk::TREE_VIEW_DROP_BEFORE);
                    break;
                case DropAction::AFTER:
                    treeView_->set_drag_dest_row(path, Gtk::TREE_VIEW_DROP_AFTER);
                    break;
                case DropAction::TO_ROOT: {
                    treeView_->unset_rows_drag_dest();
                    break;
                }
            }
        }
    };

    model_ = Gtk::TreeStore::create(columns_);
    treeView_->set_model(model_);
    treeView_->set_show_expanders(false);
    treeView_->set_level_indentation(12);

    // Generate chevron rotation frames.
    chevronFrames_ = generateChevronFrames(6);
    chevronRightPixbuf_ = chevronFrames_.front();
    chevronDownPixbuf_ = chevronFrames_.back();

    // Chevron expand/collapse indicator column (only shown for FOLDER nodes)
    Gtk::CellRendererPixbuf* chevronCR = Gtk::manage(new Gtk::CellRendererPixbuf());
    chevronCR->property_ypad() = 0;
    chevronCR->property_xpad() = 0;
    Gtk::TreeView::Column* chevronCol = Gtk::manage(new Gtk::TreeView::Column(""));
    chevronCol->pack_start(*chevronCR, false);
    chevronCol->set_expand(false);
    chevronCol->set_cell_data_func(*chevronCR, [this](Gtk::CellRenderer* cr, const Gtk::TreeModel::iterator& iter) {
        auto* pbCR = static_cast<Gtk::CellRendererPixbuf*>(cr);
        int nodeType = (*iter)[columns_.nodeType];
        if (nodeType == static_cast<int>(AlbumNodeType::FOLDER)) {
            auto path = model_->get_path(iter);
            auto pathStr = path.to_string();
            auto animIt = chevronAnimFrame_.find(pathStr);
            if (animIt != chevronAnimFrame_.end()) {
                int frame = std::max(0, std::min(animIt->second, static_cast<int>(chevronFrames_.size()) - 1));
                pbCR->property_pixbuf() = chevronFrames_[frame];
            } else {
                bool expanded = treeView_->row_expanded(path);
                pbCR->property_pixbuf() = expanded ? chevronDownPixbuf_ : chevronRightPixbuf_;
            }
            pbCR->property_visible() = true;
        } else {
            pbCR->property_pixbuf().reset_value();
            pbCR->property_visible() = false;
        }
        auto rowPath = model_->get_path(iter);
        bool hovered = !hoveredPath_.empty() && rowPath == hoveredPath_;
        bool dropTarget = dragActive_ && !dropTargetPath_.empty() && rowPath == dropTargetPath_;
        pbCR->property_cell_background_set() = hovered || dropTarget;
        if (dropTarget) pbCR->property_cell_background() = Glib::ustring("#2a4a6b");
        else if (hovered) pbCR->property_cell_background() = Glib::ustring("#3a3f4b");
    });
    treeView_->append_column(*chevronCol);

    // Cover thumbnail / folder icon column (before name)
    Gtk::CellRendererPixbuf* coverCR = Gtk::manage(new Gtk::CellRendererPixbuf());
    coverCR->property_ypad() = 0;
    coverCR->property_xpad() = 2;
    Gtk::TreeView::Column* coverCol = Gtk::manage(new Gtk::TreeView::Column(""));
    coverCol->pack_start(*coverCR, false);
    coverCol->set_expand(false);
    // Use cell_data_func to show folder icon for folders, cover pixbuf for albums
    coverCol->set_cell_data_func(*coverCR, [this](Gtk::CellRenderer* cr, const Gtk::TreeModel::iterator& iter) {
        auto* pbCR = static_cast<Gtk::CellRendererPixbuf*>(cr);
        int nodeType = (*iter)[columns_.nodeType];
        if (nodeType == static_cast<int>(AlbumNodeType::FOLDER)) {
            pbCR->property_pixbuf().reset_value();
        } else {
            pbCR->property_pixbuf() = (*iter)[columns_.coverPixbuf];
        }
        auto rowPath = model_->get_path(iter);
        bool hovered = !hoveredPath_.empty() && rowPath == hoveredPath_;
        bool dropTarget = dragActive_ && !dropTargetPath_.empty() && rowPath == dropTargetPath_;
        pbCR->property_cell_background_set() = hovered || dropTarget;
        if (dropTarget) pbCR->property_cell_background() = Glib::ustring("#2a4a6b");
        else if (hovered) pbCR->property_cell_background() = Glib::ustring("#3a3f4b");
    });
    treeView_->append_column(*coverCol);

    // Name column — set to expand so album names aren't truncated
    Gtk::CellRendererText* nameCR = Gtk::manage(new Gtk::CellRendererText());
    nameCR->property_ellipsize() = Pango::ELLIPSIZE_END;
    Gtk::TreeView::Column* nameCol = Gtk::manage(new Gtk::TreeView::Column(""));
    nameCol->pack_start(*nameCR, true);
    nameCol->add_attribute(*nameCR, "text", columns_.name);
    nameCol->set_expand(true);  // <-- FIX: let name column take all available space
    nameCol->set_cell_data_func(*nameCR, [this](Gtk::CellRenderer* cr, const Gtk::TreeModel::iterator& iter) {
        auto* textCR = static_cast<Gtk::CellRendererText*>(cr);
        bool isTarget = (*iter)[columns_.isTarget];
        int nodeType = (*iter)[columns_.nodeType];
        textCR->property_weight() = isTarget ? Pango::WEIGHT_BOLD : Pango::WEIGHT_NORMAL;
        textCR->property_style() = (nodeType == static_cast<int>(AlbumNodeType::SMART_ALBUM))
            ? Pango::STYLE_ITALIC : Pango::STYLE_NORMAL;
        auto rowPath = model_->get_path(iter);
        bool hovered = !hoveredPath_.empty() && rowPath == hoveredPath_;
        bool dropTarget = dragActive_ && !dropTargetPath_.empty() && rowPath == dropTargetPath_;
        textCR->property_cell_background_set() = hovered || dropTarget;
        if (dropTarget) textCR->property_cell_background() = Glib::ustring("#2a4a6b");
        else if (hovered) textCR->property_cell_background() = Glib::ustring("#3a3f4b");
    });
    treeView_->append_column(*nameCol);

    // Create search icon pixbuf (small magnifier drawn with Cairo)
    {
        auto surface = Cairo::ImageSurface::create(Cairo::FORMAT_ARGB32, 14, 14);
        auto cr = Cairo::Context::create(surface);
        cr->set_source_rgba(0.58, 0.62, 0.68, 0.85);
        cr->set_line_width(1.3);
        cr->arc(5.5, 5.5, 3.5, 0, 2 * M_PI);
        cr->stroke();
        cr->set_line_width(1.6);
        cr->set_line_cap(Cairo::LINE_CAP_ROUND);
        cr->move_to(8.2, 8.2);
        cr->line_to(11.8, 11.8);
        cr->stroke();
        searchPixbuf_ = Gdk::Pixbuf::create(surface, 0, 0, 14, 14);
    }

    // Search icon column (clickable, only for smart albums)
    Gtk::CellRendererPixbuf* searchCR = Gtk::manage(new Gtk::CellRendererPixbuf());
    searchCol_ = Gtk::manage(new Gtk::TreeView::Column(""));
    searchCol_->pack_start(*searchCR, false);
    searchCol_->set_expand(false);
    searchCol_->add_attribute(*searchCR, "pixbuf", columns_.searchIcon);
    searchCol_->set_cell_data_func(*searchCR, [this](Gtk::CellRenderer* cr, const Gtk::TreeModel::iterator& iter) {
        auto* pbCR = static_cast<Gtk::CellRendererPixbuf*>(cr);
        // Manually set the pixbuf since set_cell_data_func overrides add_attribute
        pbCR->property_pixbuf() = (*iter)[columns_.searchIcon];
        auto rowPath = model_->get_path(iter);
        bool hovered = !hoveredPath_.empty() && rowPath == hoveredPath_;
        bool dropTarget = dragActive_ && !dropTargetPath_.empty() && rowPath == dropTargetPath_;
        pbCR->property_cell_background_set() = hovered || dropTarget;
        if (dropTarget) pbCR->property_cell_background() = Glib::ustring("#2a4a6b");
        else if (hovered) pbCR->property_cell_background() = Glib::ustring("#3a3f4b");
    });
    treeView_->append_column(*searchCol_);

    // Count column — fixed width, right-aligned
    Gtk::CellRendererText* countCR = Gtk::manage(new Gtk::CellRendererText());
    countCR->property_foreground() = "#888888";
    countCR->property_xalign() = 1.0;
    Gtk::TreeView::Column* countCol = Gtk::manage(new Gtk::TreeView::Column(""));
    countCol->pack_start(*countCR, false);
    countCol->set_expand(false);
    countCol->set_cell_data_func(*countCR, [this](Gtk::CellRenderer* cr, const Gtk::TreeModel::iterator& iter) {
        auto* textCR = static_cast<Gtk::CellRendererText*>(cr);
        int nodeType = (*iter)[columns_.nodeType];
        int count = (*iter)[columns_.count];
        if (nodeType == static_cast<int>(AlbumNodeType::FOLDER)) {
            textCR->property_text() = "";
        } else if (count == -1) {
            textCR->property_text() = Glib::ustring::compose("%1", "\xe2\x80\xa6"); // "…" ellipsis
        } else {
            textCR->property_text() = Glib::ustring::compose("%1", count);
        }
        auto rowPath = model_->get_path(iter);
        bool hovered = !hoveredPath_.empty() && rowPath == hoveredPath_;
        bool dropTarget = dragActive_ && !dropTargetPath_.empty() && rowPath == dropTargetPath_;
        textCR->property_cell_background_set() = hovered || dropTarget;
        if (dropTarget) textCR->property_cell_background() = Glib::ustring("#2a4a6b");
        else if (hovered) textCR->property_cell_background() = Glib::ustring("#3a3f4b");
    });
    treeView_->append_column(*countCol);

    scrollw_->add(*treeView_);
    pack_start(*scrollw_, Gtk::PACK_SHRINK);
    set_margin_bottom(8);

    treeView_->signal_button_press_event().connect(sigc::mem_fun(*this, &AlbumBrowser::onButtonPress), false);
    // Handle left-click selection on button RELEASE so DnD can initiate on press.
    // Don't connect selection signal_changed — hover_selection triggers it on
    // every mouse move. Instead, onSelectionChanged is called from release.
    // Custom DnD: GTK's enable_model_drag_source is incompatible with
    // set_hover_selection(true). Instead we track press→motion→release.
    treeView_->signal_button_release_event().connect([this](GdkEventButton* ev) -> bool {
        if (ev->button == 1) {
            if (dragActive_) {
                // Complete the drop
                Gtk::TreeModel::Path path;
                int destNodeId = -1;
                DropAction action = computeDropAction(static_cast<int>(ev->x),
                                                       static_cast<int>(ev->y), path, destNodeId);
                dragActive_ = false;
                dropTargetPath_ = Gtk::TreeModel::Path();
                treeView_->unset_rows_drag_dest();
                treeView_->queue_draw();

                if (destNodeId != dragSourceNodeId_) {
                    switch (action) {
                        case DropAction::INTO_FOLDER:
                            moveNode(dragSourceNodeId_, destNodeId, -1, true);
                            break;
                        case DropAction::BEFORE: {
                            const AlbumNode* dn = findNodeConst(destNodeId);
                            if (dn) moveNode(dragSourceNodeId_, dn->parentId, destNodeId, false);
                            break;
                        }
                        case DropAction::AFTER: {
                            const AlbumNode* dn = findNodeConst(destNodeId);
                            if (dn) moveNode(dragSourceNodeId_, dn->parentId, destNodeId, true);
                            break;
                        }
                        case DropAction::TO_ROOT:
                            moveNode(dragSourceNodeId_, -1, -1, true);
                            break;
                    }
                }
            } else {
                // Simple click — handle selection
                Gtk::TreeModel::Path path;
                Gtk::TreeViewColumn* col;
                int cx, cy;
                if (treeView_->get_path_at_pos(static_cast<int>(ev->x),
                                                static_cast<int>(ev->y),
                                                path, col, cx, cy)) {
                    if (col == searchCol_) {
                        auto iter = model_->get_iter(path);
                        if (iter) {
                            int nodeType = (*iter)[columns_.nodeType];
                            if (nodeType == static_cast<int>(AlbumNodeType::SMART_ALBUM)) {
                                int nodeId = (*iter)[columns_.nodeId];
                                runSmartAlbumSearch(nodeId, false);
                                return true;
                            }
                        }
                    }
                    treeView_->get_selection()->select(path);
                    onSelectionChanged();
                }
            }
            dragSourceNodeId_ = -1;
        }
        return false;
    }, false);

    // Hide the hover popup once the list scrolls under the pointer
    scrollw_->get_vadjustment()->signal_value_changed().connect([this]() {
        hideAlbumHoverPopup();
    });

    loadAlbums();
    refreshTree();

    show_all();
    applyPanelSizing();
}

AlbumBrowser::~AlbumBrowser ()
{
    chevronAnimConn_.disconnect();
    expandAnimConn_.disconnect();
    headerDragApplyConn_.disconnect();
    hoverPopupTimer_.disconnect();
    cycleConn_.disconnect();
    ++hoverPopupSession_;
    globalChangeConn_.disconnect();
    delete hoverPopup_;
}

void AlbumBrowser::startChevronAnim()
{
    if (chevronAnimConn_.connected()) return;
    chevronAnimConn_ = Glib::signal_timeout().connect([this]() -> bool {
        // Advance chevron frames
        for (auto it = chevronAnimFrame_.begin(); it != chevronAnimFrame_.end(); ) {
            bool expanding = chevronAnimExpanding_[it->first];
            if (expanding) {
                it->second++;
                if (it->second >= static_cast<int>(chevronFrames_.size()) - 1) {
                    it->second = static_cast<int>(chevronFrames_.size()) - 1;
                    chevronAnimExpanding_.erase(it->first);
                    it = chevronAnimFrame_.erase(it);
                    continue;
                }
            } else {
                it->second--;
                if (it->second <= 0) {
                    it->second = 0;
                    chevronAnimExpanding_.erase(it->first);
                    it = chevronAnimFrame_.erase(it);
                    continue;
                }
            }
            ++it;
        }

        // Advance reveal fraction (clip-based reveal)
        if (revealAlpha_ < 1.0) {
            revealAlpha_ += 16.0 / 150.0;
            if (revealAlpha_ >= 1.0) {
                revealAlpha_ = 1.0;
                revealParentPath_.clear();
                treeView_->revealParentPath.clear();
                treeView_->revealFraction = 1.0;
            } else {
                double t = 1.0 - std::pow(1.0 - revealAlpha_, 3);
                treeView_->revealFraction = t;
            }
        }

        treeView_->queue_draw();

        bool done = chevronAnimFrame_.empty() && revealAlpha_ >= 1.0;
        return !done;
    }, 16);
}

void AlbumBrowser::onGlobalAlbumsChanged (AlbumBrowser* sender)
{
    if (sender == this) return;  // Don't reload ourselves
    loadAlbums();
    refreshTree();
}

Glib::ustring AlbumBrowser::getAlbumsFilePath () const
{
    return Glib::build_filename(Options::rtdir, "albums");
}

void AlbumBrowser::loadAlbums ()
{
    nodes_.clear();
    targetAlbumName_.clear();
    nextNodeId_ = 0;

    Glib::ustring fpath = getAlbumsFilePath();

    try {
        Glib::KeyFile kf;
        if (!kf.load_from_file(fpath)) return;

        if (kf.has_key("Albums", "Target")) {
            targetAlbumName_ = kf.get_string("Albums", "Target");
        }

        // Try new format first (with Node_ sections)
        bool newFormat = false;
        for (const auto& group : kf.get_groups()) {
            if (group.substr(0, 5) == "Node_") {
                newFormat = true;
                break;
            }
        }

        if (newFormat) {
            for (const auto& group : kf.get_groups()) {
                if (group.substr(0, 5) != "Node_") continue;

                AlbumNode node;
                node.id = std::stoi(group.substr(5));
                if (node.id >= nextNodeId_) nextNodeId_ = node.id + 1;

                node.type = AlbumNodeType::ALBUM;
                if (kf.has_key(group, "Type")) {
                    Glib::ustring t = kf.get_string(group, "Type");
                    if (t == "folder") node.type = AlbumNodeType::FOLDER;
                    else if (t == "smart") node.type = AlbumNodeType::SMART_ALBUM;
                }

                node.name = kf.has_key(group, "Name") ? kf.get_string(group, "Name") : "";
                node.parentId = kf.has_key(group, "Parent") ? kf.get_integer(group, "Parent") : -1;

                if (node.type == AlbumNodeType::ALBUM && kf.has_key(group, "Files")) {
                    node.filePaths = kf.get_string_list(group, "Files");
                }

                if (node.type == AlbumNodeType::SMART_ALBUM) {
                    if (kf.has_key(group, "Rules")) {
                        node.rules = deserializeRules(kf.get_string(group, "Rules"));
                    }
                    node.matchAll = true;
                    if (kf.has_key(group, "MatchAll")) {
                        node.matchAll = kf.get_boolean(group, "MatchAll");
                    }
                }

                if (kf.has_key(group, "Cover")) {
                    node.coverPath = kf.get_string(group, "Cover");
                }

                if (!node.name.empty()) {
                    nodes_.push_back(std::move(node));
                }
            }
        } else {
            // Legacy format: Album_0, Album_1, ...
            int count = 0;
            if (kf.has_key("Albums", "Count")) {
                count = kf.get_integer("Albums", "Count");
            }

            for (int i = 0; i < count; i++) {
                Glib::ustring section = Glib::ustring::compose("Album_%1", i);
                if (!kf.has_group(section)) continue;

                AlbumNode node;
                node.id = nextNodeId_++;
                node.type = AlbumNodeType::ALBUM;
                node.parentId = -1;
                node.matchAll = true;

                if (kf.has_key(section, "Name")) {
                    node.name = kf.get_string(section, "Name");
                }
                if (kf.has_key(section, "Files")) {
                    node.filePaths = kf.get_string_list(section, "Files");
                }
                if (!node.name.empty()) {
                    nodes_.push_back(std::move(node));
                }
            }
        }
    } catch (...) {
        // File doesn't exist yet or is malformed
    }
}

void AlbumBrowser::saveAlbums ()
{
    Glib::KeyFile kf;

    kf.set_string("Albums", "Target", targetAlbumName_);

    for (const auto& node : nodes_) {
        Glib::ustring section = Glib::ustring::compose("Node_%1", node.id);

        Glib::ustring typeStr = "album";
        if (node.type == AlbumNodeType::FOLDER) typeStr = "folder";
        else if (node.type == AlbumNodeType::SMART_ALBUM) typeStr = "smart";

        kf.set_string(section, "Type", typeStr);
        kf.set_string(section, "Name", node.name);
        kf.set_integer(section, "Parent", node.parentId);

        if (node.type == AlbumNodeType::ALBUM && !node.filePaths.empty()) {
            Glib::ArrayHandle<Glib::ustring> arr(node.filePaths);
            kf.set_string_list(section, "Files", arr);
        }

        if (!node.coverPath.empty()) {
            kf.set_string(section, "Cover", node.coverPath);
        }

        if (node.type == AlbumNodeType::SMART_ALBUM) {
            kf.set_string(section, "Rules", serializeRules(node.rules));
            kf.set_boolean(section, "MatchAll", node.matchAll);
        }
    }

    try {
        Glib::ustring data = kf.to_data();
        Glib::ustring fpath = getAlbumsFilePath();
        std::ofstream f(fpath.c_str());
        f << data;
    } catch (...) {}

    // Notify all other AlbumBrowser instances to reload
    albumsChangedOnDisk_.emit(this);
}

void AlbumBrowser::refreshTree ()
{
    saveExpansionState();
    model_->clear();
    addChildrenToTree(-1, nullptr);
    restoreExpansionState();

    // Load cover thumbnails in background
    int session = ++coverLoadSession_;
    std::thread(&AlbumBrowser::loadCoverThumbnails, this, session).detach();
}

void AlbumBrowser::saveExpansionState ()
{
    if (firstTreeLoad_) return;  // Nothing to save yet

    expandedFolders_.clear();
    model_->foreach_iter([this](const Gtk::TreeModel::iterator& iter) -> bool {
        int nodeType = (*iter)[columns_.nodeType];
        if (nodeType == static_cast<int>(AlbumNodeType::FOLDER)) {
            int nodeId = (*iter)[columns_.nodeId];
            if (treeView_->row_expanded(model_->get_path(iter))) {
                expandedFolders_.insert(nodeId);
            }
        }
        return false;
    });
}

void AlbumBrowser::restoreExpansionState ()
{
    if (firstTreeLoad_) {
        // First load: folders start collapsed
        firstTreeLoad_ = false;
    } else {
        // Restore previously expanded folders
        model_->foreach_iter([this](const Gtk::TreeModel::iterator& iter) -> bool {
            int nodeType = (*iter)[columns_.nodeType];
            if (nodeType == static_cast<int>(AlbumNodeType::FOLDER)) {
                int nodeId = (*iter)[columns_.nodeId];
                if (expandedFolders_.count(nodeId)) {
                    treeView_->expand_row(model_->get_path(iter), false);
                }
            }
            return false;
        });
    }
}

void AlbumBrowser::addChildrenToTree (int parentId, const Gtk::TreeModel::Row* parentRow)
{
    for (const auto& node : nodes_) {
        if (node.parentId != parentId) continue;

        Gtk::TreeModel::Row row;
        if (parentRow) {
            row = *(model_->append(parentRow->children()));
        } else {
            row = *(model_->append());
        }

        row[columns_.name] = node.name;
        row[columns_.count] = getAlbumFileCount(node);
        row[columns_.nodeType] = static_cast<int>(node.type);
        row[columns_.isTarget] = (node.name == targetAlbumName_ && node.type == AlbumNodeType::ALBUM);
        row[columns_.nodeId] = node.id;
        if (node.type == AlbumNodeType::SMART_ALBUM) {
            row[columns_.searchIcon] = searchPixbuf_;
        }

        // Set cover pixbuf for albums (folder icons handled by cell_data_func)
        if (node.type == AlbumNodeType::ALBUM) {
            auto cit = coverCache_.find(node.id);
            if (cit != coverCache_.end()) {
                row[columns_.coverPixbuf] = cit->second;
            }
        }

        // Recurse for folders
        if (node.type == AlbumNodeType::FOLDER) {
            addChildrenToTree(node.id, &row);
        }
    }
}

int AlbumBrowser::getAlbumFileCount (const AlbumNode& node) const
{
    if (node.type == AlbumNodeType::ALBUM) {
        return static_cast<int>(node.filePaths.size());
    }
    return 0;
}

AlbumNode* AlbumBrowser::findNode (int id)
{
    for (auto& n : nodes_) {
        if (n.id == id) return &n;
    }
    return nullptr;
}

AlbumNode* AlbumBrowser::findNodeByName (const Glib::ustring& name)
{
    for (auto& n : nodes_) {
        if (n.name == name) return &n;
    }
    return nullptr;
}

const AlbumNode* AlbumBrowser::findNodeConst (int id) const
{
    for (const auto& n : nodes_) {
        if (n.id == id) return &n;
    }
    return nullptr;
}

bool AlbumBrowser::isDescendantOf (int nodeId, int potentialAncestorId) const
{
    int current = nodeId;
    while (current >= 0) {
        if (current == potentialAncestorId) return true;
        const AlbumNode* node = findNodeConst(current);
        if (!node) break;
        current = node->parentId;
    }
    return false;
}

void AlbumBrowser::onDragDataGet (const Glib::RefPtr<Gdk::DragContext>&,
                                   Gtk::SelectionData& data, guint, guint)
{
    if (dragSourceNodeId_ >= 0) {
        data.set("ALBUM_NODE", 8,
                 reinterpret_cast<const guint8*>(&dragSourceNodeId_), sizeof(int));
    }
}

void AlbumBrowser::onDragDataReceived (const Glib::RefPtr<Gdk::DragContext>& context,
                                        int x, int y,
                                        const Gtk::SelectionData& data,
                                        guint, guint time)
{
    if (data.get_length() < static_cast<int>(sizeof(int))) {
        context->drag_finish(false, false, time);
        return;
    }

    int sourceNodeId = *reinterpret_cast<const int*>(data.get_data());

    Gtk::TreeModel::Path destPath;
    Gtk::TreeViewDropPosition dropPos;

    if (!treeView_->get_dest_row_at_pos(x, y, destPath, dropPos)) {
        moveNode(sourceNodeId, -1, -1, true);
    } else {
        auto destIter = model_->get_iter(destPath);
        if (!destIter) {
            context->drag_finish(false, false, time);
            return;
        }
        int destNodeId = (*destIter)[columns_.nodeId];
        int destNodeType = (*destIter)[columns_.nodeType];

        if ((dropPos == Gtk::TREE_VIEW_DROP_INTO_OR_BEFORE ||
             dropPos == Gtk::TREE_VIEW_DROP_INTO_OR_AFTER) &&
            destNodeType == static_cast<int>(AlbumNodeType::FOLDER)) {
            moveNode(sourceNodeId, destNodeId, -1, true);
        } else {
            const AlbumNode* destNode = findNodeConst(destNodeId);
            if (destNode) {
                bool after = (dropPos == Gtk::TREE_VIEW_DROP_AFTER ||
                              dropPos == Gtk::TREE_VIEW_DROP_INTO_OR_AFTER);
                moveNode(sourceNodeId, destNode->parentId, destNodeId, after);
            }
        }
    }

    context->drag_finish(true, false, time);
}

void AlbumBrowser::moveNode (int sourceId, int newParentId, int siblingId, bool after)
{
    // Don't allow dropping a node into itself or its own descendants
    if (newParentId >= 0 && isDescendantOf(newParentId, sourceId)) return;

    // Find and remove source node from vector
    auto srcIt = std::find_if(nodes_.begin(), nodes_.end(),
        [sourceId](const AlbumNode& n) { return n.id == sourceId; });
    if (srcIt == nodes_.end()) return;

    AlbumNode srcNode = *srcIt;
    srcNode.parentId = newParentId;
    nodes_.erase(srcIt);

    if (siblingId >= 0) {
        // Insert before/after the sibling
        auto sibIt = std::find_if(nodes_.begin(), nodes_.end(),
            [siblingId](const AlbumNode& n) { return n.id == siblingId; });
        if (sibIt != nodes_.end()) {
            if (after) ++sibIt;
            nodes_.insert(sibIt, srcNode);
        } else {
            nodes_.push_back(srcNode);
        }
    } else {
        // No sibling target — append at end
        nodes_.push_back(srcNode);
    }

    saveAlbums();
    refreshTree();
}

AlbumBrowser::DropAction AlbumBrowser::computeDropAction(int x, int y,
    Gtk::TreeModel::Path& outPath, int& outNodeId) const
{
    outNodeId = -1;
    outPath = Gtk::TreeModel::Path();

    Gtk::TreeModel::Path path;
    if (!treeView_->get_path_at_pos(x, y, path)) {
        return DropAction::TO_ROOT;
    }

    auto iter = model_->get_iter(path);
    if (!iter) return DropAction::TO_ROOT;

    outPath = path;
    outNodeId = (*iter)[columns_.nodeId];
    int nodeType = (*iter)[columns_.nodeType];

    // Get the cell area to determine where within the row the cursor is
    Gdk::Rectangle cellArea;
    treeView_->get_cell_area(path, *treeView_->get_column(0), cellArea);
    int rowHeight = cellArea.get_height();
    int relY = y - cellArea.get_y();
    double fraction = (rowHeight > 0) ? static_cast<double>(relY) / rowHeight : 0.5;

    if (nodeType == static_cast<int>(AlbumNodeType::FOLDER)) {
        // Top 25% = before, bottom 25% = after, middle 50% = into
        if (fraction < 0.25) return DropAction::BEFORE;
        if (fraction > 0.75) return DropAction::AFTER;
        return DropAction::INTO_FOLDER;
    } else {
        // Non-folder: top half = before, bottom half = after
        if (fraction < 0.5) return DropAction::BEFORE;
        return DropAction::AFTER;
    }
}

void AlbumBrowser::sortNodes(bool byName)
{
    // Sort children within each parent group, recursively
    // Collect unique parent IDs
    std::set<int> parentIds;
    for (const auto& n : nodes_) parentIds.insert(n.parentId);

    // For each parent, sort its direct children
    for (int pid : parentIds) {
        // Find the range of children for this parent — they may not be contiguous
        // so we gather them, sort, and put back
        std::vector<std::pair<size_t, AlbumNode>> children;
        for (size_t i = 0; i < nodes_.size(); ++i) {
            if (nodes_[i].parentId == pid) {
                children.push_back({i, nodes_[i]});
            }
        }
        if (children.size() <= 1) continue;

        if (byName) {
            std::sort(children.begin(), children.end(),
                [](const std::pair<size_t, AlbumNode>& a, const std::pair<size_t, AlbumNode>& b) {
                    // Folders first, then alphabetical
                    if (a.second.type == AlbumNodeType::FOLDER && b.second.type != AlbumNodeType::FOLDER) return true;
                    if (a.second.type != AlbumNodeType::FOLDER && b.second.type == AlbumNodeType::FOLDER) return false;
                    return a.second.name.lowercase() < b.second.name.lowercase();
                });
        } else {
            // Sort by creation order (ID)
            std::sort(children.begin(), children.end(),
                [](const std::pair<size_t, AlbumNode>& a, const std::pair<size_t, AlbumNode>& b) {
                    if (a.second.type == AlbumNodeType::FOLDER && b.second.type != AlbumNodeType::FOLDER) return true;
                    if (a.second.type != AlbumNodeType::FOLDER && b.second.type == AlbumNodeType::FOLDER) return false;
                    return a.second.id < b.second.id;
                });
        }

        // Write sorted children back into their original positions
        std::vector<size_t> positions;
        for (const auto& c : children) positions.push_back(c.first);
        std::sort(positions.begin(), positions.end());
        for (size_t i = 0; i < positions.size(); ++i) {
            nodes_[positions[i]] = children[i].second;
        }
    }

    saveAlbums();
    refreshTree();
}

void AlbumBrowser::createAlbum (int parentId)
{
    Gtk::Window* toplevel = dynamic_cast<Gtk::Window*>(get_toplevel());
    if (!toplevel) return;

    Gtk::Dialog dlg(M("ALBUM_CREATE_ALBUM"), *toplevel, true);
    dlg.add_button(M("GENERAL_CANCEL"), Gtk::RESPONSE_CANCEL);
    dlg.add_button(M("GENERAL_OK"), Gtk::RESPONSE_OK);

    Gtk::Entry entry;
    entry.set_activates_default(true);
    dlg.get_content_area()->pack_start(entry, Gtk::PACK_SHRINK, 8);
    dlg.set_default_response(Gtk::RESPONSE_OK);
    dlg.show_all_children();

    if (dlg.run() == Gtk::RESPONSE_OK) {
        Glib::ustring name = entry.get_text().c_str();
        name = name.c_str();
        if (!name.empty()) {
            AlbumNode node;
            node.id = nextNodeId_++;
            node.type = AlbumNodeType::ALBUM;
            node.name = name;
            node.parentId = parentId;
            node.matchAll = true;
            nodes_.push_back(std::move(node));
            if (targetAlbumName_.empty()) {
                targetAlbumName_ = name;
            }
            saveAlbums();
            refreshTree();
        }
    }
}

void AlbumBrowser::createSmartAlbum (int parentId)
{
    Glib::ustring name;
    std::vector<SmartAlbumRule> rules;
    bool matchAll = true;

    if (showSmartAlbumDialog(name, rules, matchAll, false)) {
        AlbumNode node;
        node.id = nextNodeId_++;
        node.type = AlbumNodeType::SMART_ALBUM;
        node.name = name;
        node.parentId = parentId;
        node.rules = std::move(rules);
        node.matchAll = matchAll;
        nodes_.push_back(std::move(node));
        saveAlbums();
        refreshTree();
    }
}

void AlbumBrowser::createFolder (int parentId)
{
    Gtk::Window* toplevel = dynamic_cast<Gtk::Window*>(get_toplevel());
    if (!toplevel) return;

    Gtk::Dialog dlg(M("ALBUM_CREATE_FOLDER"), *toplevel, true);
    dlg.add_button(M("GENERAL_CANCEL"), Gtk::RESPONSE_CANCEL);
    dlg.add_button(M("GENERAL_OK"), Gtk::RESPONSE_OK);

    Gtk::Entry entry;
    entry.set_activates_default(true);
    dlg.get_content_area()->pack_start(entry, Gtk::PACK_SHRINK, 8);
    dlg.set_default_response(Gtk::RESPONSE_OK);
    dlg.show_all_children();

    if (dlg.run() == Gtk::RESPONSE_OK) {
        Glib::ustring name = entry.get_text().c_str();
        name = name.c_str();
        if (!name.empty()) {
            AlbumNode node;
            node.id = nextNodeId_++;
            node.type = AlbumNodeType::FOLDER;
            node.name = name;
            node.parentId = parentId;
            node.matchAll = true;
            nodes_.push_back(std::move(node));
            saveAlbums();
            refreshTree();
        }
    }
}

void AlbumBrowser::renameNode ()
{
    if (contextMenuNodeId_ < 0) return;

    AlbumNode* node = findNode(contextMenuNodeId_);
    if (!node) return;

    Gtk::Window* toplevel = dynamic_cast<Gtk::Window*>(get_toplevel());
    if (!toplevel) return;

    Gtk::Dialog dlg(M("ALBUM_RENAME"), *toplevel, true);
    dlg.add_button(M("GENERAL_CANCEL"), Gtk::RESPONSE_CANCEL);
    dlg.add_button(M("GENERAL_OK"), Gtk::RESPONSE_OK);

    Gtk::Entry entry;
    entry.set_text(node->name);
    entry.set_activates_default(true);
    dlg.get_content_area()->pack_start(entry, Gtk::PACK_SHRINK, 8);
    dlg.set_default_response(Gtk::RESPONSE_OK);
    dlg.show_all_children();

    if (dlg.run() == Gtk::RESPONSE_OK) {
        Glib::ustring newName = entry.get_text().c_str();
        if (!newName.empty() && newName != node->name) {
            if (targetAlbumName_ == node->name) {
                targetAlbumName_ = newName;
            }
            if (selectedAlbumName_ == node->name) {
                selectedAlbumName_ = newName;
            }
            node->name = newName;
            saveAlbums();
            refreshTree();
        }
    }
}

void AlbumBrowser::deleteNode ()
{
    if (contextMenuNodeId_ < 0) return;

    // Collect all descendant IDs (for folders)
    std::vector<int> toDelete;
    std::function<void(int)> collectChildren = [&](int id) {
        toDelete.push_back(id);
        for (const auto& n : nodes_) {
            if (n.parentId == id) {
                collectChildren(n.id);
            }
        }
    };
    collectChildren(contextMenuNodeId_);

    // Check if target album is being deleted
    for (int id : toDelete) {
        AlbumNode* n = findNode(id);
        if (n && n->name == targetAlbumName_) {
            targetAlbumName_.clear();
        }
    }

    // Remove all nodes
    nodes_.erase(
        std::remove_if(nodes_.begin(), nodes_.end(),
            [&toDelete](const AlbumNode& n) {
                return std::find(toDelete.begin(), toDelete.end(), n.id) != toDelete.end();
            }),
        nodes_.end()
    );

    selectedAlbumName_.clear();
    selectedNodeId_ = -1;
    saveAlbums();
    refreshTree();

    // Clear filter
    std::set<std::string> empty;
    albumSelectedSignal_.emit(empty);
}

void AlbumBrowser::setTargetAlbum ()
{
    if (contextMenuAlbumName_.empty()) return;
    targetAlbumName_ = contextMenuAlbumName_;
    saveAlbums();
    refreshTree();
}

void AlbumBrowser::addCurrentImage ()
{
    if (contextMenuNodeId_ < 0 || !getCurrentFilePath_) return;

    AlbumNode* node = findNode(contextMenuNodeId_);
    if (!node || node->type != AlbumNodeType::ALBUM) return;

    Glib::ustring filePath = getCurrentFilePath_();
    if (!filePath.empty()) {
        addFileToAlbum(node->name, filePath);
    }
}

void AlbumBrowser::editSmartAlbumRules ()
{
    if (contextMenuNodeId_ < 0) return;

    AlbumNode* node = findNode(contextMenuNodeId_);
    if (!node || node->type != AlbumNodeType::SMART_ALBUM) return;

    Glib::ustring name = node->name;
    std::vector<SmartAlbumRule> rules = node->rules;
    bool matchAll = node->matchAll;

    if (showSmartAlbumDialog(name, rules, matchAll, true)) {
        node->name = name;
        node->rules = std::move(rules);
        node->matchAll = matchAll;
        saveAlbums();
        refreshTree();
    }
}

void AlbumBrowser::addFileToTargetAlbum (const Glib::ustring& filePath)
{
    if (filePath.empty()) return;

    // Use target album if set, otherwise fall back to currently selected album
    Glib::ustring albumName = targetAlbumName_;
    if (albumName.empty()) {
        albumName = selectedAlbumName_;
    }
    if (albumName.empty()) return;

    addFileToAlbum(albumName, filePath);
}

void AlbumBrowser::addFileToAlbum (const Glib::ustring& albumName, const Glib::ustring& filePath)
{
    AlbumNode* node = findNodeByName(albumName);
    if (!node || node->type != AlbumNodeType::ALBUM) return;

    // Don't add duplicates
    for (const auto& f : node->filePaths) {
        if (f == filePath) return;
    }

    node->filePaths.push_back(filePath);
    saveAlbums();
    refreshTree();
}

void AlbumBrowser::removeFileFromAlbum (const Glib::ustring& albumName, const Glib::ustring& filePath)
{
    AlbumNode* node = findNodeByName(albumName);
    if (!node || node->type != AlbumNodeType::ALBUM) return;

    auto it = std::find(node->filePaths.begin(), node->filePaths.end(), filePath);
    if (it != node->filePaths.end()) {
        node->filePaths.erase(it);
        saveAlbums();
        refreshTree();
    }
}

void AlbumBrowser::onSelectionChanged ()
{
    if (selectionChanging_) return;
    selectionChanging_ = true;

    auto iter = treeView_->get_selection()->get_selected();
    if (iter) {
        int nodeId = (*iter)[columns_.nodeId];
        Glib::ustring name = (*iter)[columns_.name];
        if (nodeId == selectedNodeId_) {
            // Clicking same node again -> deselect
            treeView_->get_selection()->unselect_all();
            selectedAlbumName_.clear();
            selectedNodeId_ = -1;
            closeAlbumBtn_->hide();
            std::set<std::string> empty;
            albumSelectedSignal_.emit(empty);
            albumViewSignal_.emit("", std::vector<Glib::ustring>());
            selectionChanging_ = false;
            return;
        }

        selectedNodeId_ = nodeId;
        selectedAlbumName_ = name;

        AlbumNode* node = findNode(nodeId);
        if (!node) {
            selectionChanging_ = false;
            return;
        }

        if (node->type == AlbumNodeType::ALBUM) {
            // Build whitelist from album's files
            std::set<std::string> whitelist;
            for (const auto& f : node->filePaths) {
                whitelist.insert(std::string(f.c_str()));
            }
            closeAlbumBtn_->show();
            albumSelectedSignal_.emit(whitelist);
            albumViewSignal_.emit(node->name, node->filePaths);
        } else if (node->type == AlbumNodeType::SMART_ALBUM) {
            closeAlbumBtn_->show();
            runSmartAlbumSearch(nodeId);
        } else {
            // Folders - toggle expand/collapse with animation
            auto path = model_->get_path(iter);
            expandAnimConn_.disconnect();
            bool wasExpanded = treeView_->row_expanded(path);

            // Hide the scrollbar during the animation. POLICY_EXTERNAL, not
            // POLICY_NEVER: with NEVER, GTK propagates the child's full
            // minimum height and ignores max_content_height, so the panel
            // would jump to the full tree height and snap back afterwards.
            // With EXTERNAL the scrollbar stays hidden while min==max
            // content height (set each tick below) pins the height exactly.
            scrollw_->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_EXTERNAL);

            if (wasExpanded) {
                // Start chevron collapse animation
                auto pathStr = path.to_string();
                auto cit = chevronAnimFrame_.find(pathStr);
                chevronAnimFrame_[pathStr] = cit != chevronAnimFrame_.end()
                    ? cit->second : static_cast<int>(chevronFrames_.size()) - 1;
                chevronAnimExpanding_[pathStr] = false;
                revealParentPath_.clear();
                treeView_->revealParentPath.clear();
                treeView_->revealFraction = 1.0;
                startChevronAnim();

                // Collapse animation: capture height, collapse, animate
                expandAnimStartH_ = scrollw_->get_allocated_height();
                treeView_->collapse_row(path);
                // Measure new natural height after collapse; the target must
                // match the settled height once min/max are restored, or the
                // panel snaps at the end of the animation.
                int minH = 0, natH = 0;
                treeView_->get_preferred_height(minH, natH);
                expandAnimTargetH_ = std::max(std::min(natH + 4, effMaxHeight()), effMinHeight());
                expandAnimExpanding_ = false;
                expandAnimFraction_ = 0.0;
                scrollw_->set_min_content_height(expandAnimStartH_);
                scrollw_->set_max_content_height(expandAnimStartH_);
                expandAnimConn_ = Glib::signal_timeout().connect([this]() -> bool {
                    expandAnimFraction_ += 16.0 / 150.0; // 150ms collapse
                    if (expandAnimFraction_ >= 1.0) {
                        scrollw_->set_min_content_height(effMinHeight());
                        scrollw_->set_max_content_height(effMaxHeight());
                        scrollw_->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
                        return false;
                    }
                    double t = expandAnimFraction_ * expandAnimFraction_; // ease-in-quad
                    int h = expandAnimStartH_ + static_cast<int>((expandAnimTargetH_ - expandAnimStartH_) * t);
                    scrollw_->set_min_content_height(h);
                    scrollw_->set_max_content_height(h);
                    return true;
                }, 16);
            } else {
                // Start chevron expand animation + child reveal
                auto pathStr = path.to_string();
                auto cit = chevronAnimFrame_.find(pathStr);
                chevronAnimFrame_[pathStr] = cit != chevronAnimFrame_.end()
                    ? cit->second : 0;
                chevronAnimExpanding_[pathStr] = true;
                revealParentPath_ = pathStr;
                revealAlpha_ = 0.0;
                treeView_->revealParentPath = pathStr;
                treeView_->revealFraction = 0.0;
                startChevronAnim();

                // Expand animation: capture height, expand, animate
                expandAnimStartH_ = scrollw_->get_allocated_height();
                treeView_->expand_row(path, false);
                // Measure new natural height after expand; target must match
                // the settled height once min/max are restored (see collapse).
                int minH = 0, natH = 0;
                treeView_->get_preferred_height(minH, natH);
                expandAnimTargetH_ = std::max(std::min(natH + 4, effMaxHeight()), effMinHeight());
                expandAnimExpanding_ = true;
                expandAnimFraction_ = 0.0;
                scrollw_->set_min_content_height(expandAnimStartH_);
                scrollw_->set_max_content_height(expandAnimStartH_);
                expandAnimConn_ = Glib::signal_timeout().connect([this]() -> bool {
                    expandAnimFraction_ += 16.0 / 250.0; // 250ms expand
                    if (expandAnimFraction_ >= 1.0) {
                        scrollw_->set_min_content_height(effMinHeight());
                        scrollw_->set_max_content_height(effMaxHeight());
                        scrollw_->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
                        return false;
                    }
                    double t = 1.0 - std::pow(1.0 - expandAnimFraction_, 4); // ease-out-quart
                    int h = expandAnimStartH_ + static_cast<int>((expandAnimTargetH_ - expandAnimStartH_) * t);
                    scrollw_->set_min_content_height(h);
                    scrollw_->set_max_content_height(h);
                    return true;
                }, 16);
            }

            selectedAlbumName_.clear();
            selectedNodeId_ = -1;
            closeAlbumBtn_->hide();
            treeView_->get_selection()->unselect_all();
        }
    } else {
        selectedAlbumName_.clear();
        selectedNodeId_ = -1;
        closeAlbumBtn_->hide();
        std::set<std::string> empty;
        albumSelectedSignal_.emit(empty);
        albumViewSignal_.emit("", std::vector<Glib::ustring>());
    }
    selectionChanging_ = false;
}

void AlbumBrowser::runSmartAlbumSearch (int nodeId, bool openAlbum)
{
    AlbumNode* node = findNode(nodeId);
    if (!node || node->type != AlbumNodeType::SMART_ALBUM) return;

    AlbumNode nodeCopy = *node;
    Glib::ustring albumName = node->name;

    // Show searching indicator: replace the search icon with "..." text in count
    model_->foreach_iter([this, nodeId](const Gtk::TreeModel::iterator& iter) -> bool {
        if ((*iter)[columns_.nodeId] == nodeId) {
            (*iter)[columns_.count] = -1; // sentinel for "searching"
            return true;
        }
        return false;
    });

    std::thread([this, nodeCopy, albumName, nodeId, openAlbum]() {
        auto matchingFiles = evaluateSmartAlbum(nodeCopy);
        Glib::signal_idle().connect_once([this, matchingFiles, albumName, nodeId, openAlbum]() {
            // Update the count in the tree model
            model_->foreach_iter([this, nodeId, &matchingFiles](const Gtk::TreeModel::iterator& iter) -> bool {
                if ((*iter)[columns_.nodeId] == nodeId) {
                    (*iter)[columns_.count] = static_cast<int>(matchingFiles.size());
                    return true; // stop
                }
                return false; // continue
            });

            if (openAlbum) {
                // Emit signals so the file browser shows the results
                std::set<std::string> whitelist;
                for (const auto& f : matchingFiles) {
                    whitelist.insert(std::string(f.c_str()));
                }
                selectedNodeId_ = nodeId;
                selectedAlbumName_ = albumName;
                albumSelectedSignal_.emit(whitelist);
                albumViewSignal_.emit(albumName, matchingFiles);
            }
        });
    }).detach();
}

bool AlbumBrowser::onButtonPress (GdkEventButton* event)
{
    hideAlbumHoverPopup();

    // Left-click: capture source row and start coords for custom DnD.
    // Selection is handled on button RELEASE (see signal_button_release above).
    if (event->type == GDK_BUTTON_PRESS && event->button == 1) {
        dragActive_ = false;
        dragSourceNodeId_ = -1;
        dragStartX_ = event->x;
        dragStartY_ = event->y;
        Gtk::TreeModel::Path path;
        if (treeView_->get_path_at_pos(static_cast<int>(event->x),
                                        static_cast<int>(event->y), path)) {
            auto iter = model_->get_iter(path);
            if (iter) {
                dragSourceNodeId_ = (*iter)[columns_.nodeId];
            }
        }
        return false;
    }

    if (event->type == GDK_BUTTON_PRESS && event->button == 3) {
        Gtk::TreeModel::Path path;
        if (treeView_->get_path_at_pos(static_cast<int>(event->x), static_cast<int>(event->y), path)) {
            auto iter = model_->get_iter(path);
            if (iter) {
                int nodeId = (*iter)[columns_.nodeId];
                // Store for context menu callbacks without triggering onSelectionChanged
                contextMenuNodeId_ = nodeId;
                contextMenuAlbumName_ = Glib::ustring((*iter)[columns_.name]);
                showContextMenu(event, nodeId);
                return true;  // prevent default selection
            }
        } else {
            contextMenuNodeId_ = -1;
            contextMenuAlbumName_.clear();
            showContextMenu(event, -1);
            return true;
        }
    }
    return false;
}

void AlbumBrowser::showContextMenu (GdkEventButton* event, int nodeId)
{
    auto* menu = Gtk::manage(new Gtk::Menu());

    if (nodeId < 0) {
        // Empty area
        auto* mi1 = Gtk::manage(new Gtk::MenuItem(M("ALBUM_CREATE_ALBUM")));
        mi1->signal_activate().connect([this]() { createAlbum(); });
        menu->append(*mi1);

        auto* mi2 = Gtk::manage(new Gtk::MenuItem(M("ALBUM_CREATE_SMART")));
        mi2->signal_activate().connect([this]() { createSmartAlbum(); });
        menu->append(*mi2);

        auto* mi3 = Gtk::manage(new Gtk::MenuItem(M("ALBUM_CREATE_FOLDER")));
        mi3->signal_activate().connect([this]() { createFolder(); });
        menu->append(*mi3);
    } else {
        AlbumNode* node = findNode(nodeId);
        if (!node) return;

        if (node->type == AlbumNodeType::FOLDER) {
            auto* mi1 = Gtk::manage(new Gtk::MenuItem(M("ALBUM_CREATE_ALBUM")));
            mi1->signal_activate().connect([this, nodeId]() { createAlbum(nodeId); });
            menu->append(*mi1);

            auto* mi2 = Gtk::manage(new Gtk::MenuItem(M("ALBUM_CREATE_SMART")));
            mi2->signal_activate().connect([this, nodeId]() { createSmartAlbum(nodeId); });
            menu->append(*mi2);

            menu->append(*Gtk::manage(new Gtk::SeparatorMenuItem()));

            auto* miRename = Gtk::manage(new Gtk::MenuItem(M("ALBUM_RENAME")));
            miRename->signal_activate().connect(sigc::mem_fun(*this, &AlbumBrowser::renameNode));
            menu->append(*miRename);

            auto* miDelete = Gtk::manage(new Gtk::MenuItem(M("ALBUM_DELETE")));
            miDelete->signal_activate().connect(sigc::mem_fun(*this, &AlbumBrowser::deleteNode));
            menu->append(*miDelete);
        } else if (node->type == AlbumNodeType::ALBUM) {
            auto* miAdd = Gtk::manage(new Gtk::MenuItem(M("ALBUM_ADD_CURRENT")));
            miAdd->signal_activate().connect(sigc::mem_fun(*this, &AlbumBrowser::addCurrentImage));
            menu->append(*miAdd);

            auto* miTarget = Gtk::manage(new Gtk::MenuItem(M("ALBUM_SET_TARGET")));
            miTarget->signal_activate().connect(sigc::mem_fun(*this, &AlbumBrowser::setTargetAlbum));
            menu->append(*miTarget);

            // "Choose cover..." — pick from album images
            if (!node->filePaths.empty()) {
                auto* miCover = Gtk::manage(new Gtk::MenuItem(M("ALBUM_SET_COVER")));
                miCover->signal_activate().connect([this, nodeId]() {
                    AlbumNode* n = findNode(nodeId);
                    if (!n || n->filePaths.empty()) return;

                    Gtk::Window* toplevel = dynamic_cast<Gtk::Window*>(get_toplevel());
                    if (!toplevel) return;

                    Gtk::Dialog dlg(M("ALBUM_SET_COVER"), *toplevel, true);
                    dlg.add_button(M("GENERAL_CANCEL"), Gtk::RESPONSE_CANCEL);
                    dlg.set_default_size(360, 280);

                    Gtk::ScrolledWindow* sw = Gtk::manage(new Gtk::ScrolledWindow());
                    sw->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
                    Gtk::FlowBox* flowBox = Gtk::manage(new Gtk::FlowBox());
                    flowBox->set_selection_mode(Gtk::SELECTION_SINGLE);
                    flowBox->set_max_children_per_line(5);
                    flowBox->set_min_children_per_line(3);

                    Glib::ustring chosenPath;
                    for (const auto& fp : n->filePaths) {
                        Gtk::Image* img = Gtk::manage(new Gtk::Image());
                        img->set_size_request(64, 64);
                        img->set_tooltip_text(Glib::path_get_basename(fp));
                        flowBox->add(*img);

                        // Load thumbnail asynchronously
                        Glib::ustring path = fp;
                        Glib::signal_idle().connect_once([img, path]() {
                            try {
                                Thumbnail* thm = CacheManager::getInstance()->getEntry(path);
                                if (thm) {
                                    double scale = 1.0;
                                    rtengine::IImage8* im = thm->processThumbImage(64, scale);
                                    if (im) {
                                        auto pb = Gdk::Pixbuf::create_from_data(
                                            im->getData(), Gdk::COLORSPACE_RGB, false, 8,
                                            im->getWidth(), im->getHeight(), im->getWidth() * 3);
                                        img->set(pb->copy());
                                        delete im;
                                    }
                                    thm->decreaseRef();
                                }
                            } catch (...) {}
                        });
                    }

                    sw->add(*flowBox);
                    dlg.get_content_area()->pack_start(*sw, Gtk::PACK_EXPAND_WIDGET);
                    dlg.show_all_children();

                    flowBox->signal_child_activated().connect([&](Gtk::FlowBoxChild* child) {
                        int idx = child->get_index();
                        if (idx >= 0 && idx < static_cast<int>(n->filePaths.size())) {
                            chosenPath = n->filePaths[idx];
                            dlg.response(Gtk::RESPONSE_OK);
                        }
                    });

                    if (dlg.run() == Gtk::RESPONSE_OK && !chosenPath.empty()) {
                        setCoverForAlbum(nodeId, chosenPath);
                    }
                });
                menu->append(*miCover);
            }

            menu->append(*Gtk::manage(new Gtk::SeparatorMenuItem()));

            auto* miRename = Gtk::manage(new Gtk::MenuItem(M("ALBUM_RENAME")));
            miRename->signal_activate().connect(sigc::mem_fun(*this, &AlbumBrowser::renameNode));
            menu->append(*miRename);

            auto* miDelete = Gtk::manage(new Gtk::MenuItem(M("ALBUM_DELETE")));
            miDelete->signal_activate().connect(sigc::mem_fun(*this, &AlbumBrowser::deleteNode));
            menu->append(*miDelete);
        } else if (node->type == AlbumNodeType::SMART_ALBUM) {
            auto* miSearch = Gtk::manage(new Gtk::MenuItem(M("ALBUM_SEARCH")));
            miSearch->signal_activate().connect([this]() {
                runSmartAlbumSearch(contextMenuNodeId_, false);
            });
            menu->append(*miSearch);

            auto* miEdit = Gtk::manage(new Gtk::MenuItem(M("ALBUM_EDIT_RULES")));
            miEdit->signal_activate().connect(sigc::mem_fun(*this, &AlbumBrowser::editSmartAlbumRules));
            menu->append(*miEdit);

            menu->append(*Gtk::manage(new Gtk::SeparatorMenuItem()));

            auto* miRename = Gtk::manage(new Gtk::MenuItem(M("ALBUM_RENAME")));
            miRename->signal_activate().connect(sigc::mem_fun(*this, &AlbumBrowser::renameNode));
            menu->append(*miRename);

            auto* miDelete = Gtk::manage(new Gtk::MenuItem(M("ALBUM_DELETE")));
            miDelete->signal_activate().connect(sigc::mem_fun(*this, &AlbumBrowser::deleteNode));
            menu->append(*miDelete);
        }
    }

    // Sort options — always available
    menu->append(*Gtk::manage(new Gtk::SeparatorMenuItem()));
    auto* miSortName = Gtk::manage(new Gtk::MenuItem(M("ALBUM_SORT_NAME")));
    miSortName->signal_activate().connect([this]() { sortNodes(true); });
    menu->append(*miSortName);
    auto* miSortDate = Gtk::manage(new Gtk::MenuItem(M("ALBUM_SORT_DATE")));
    miSortDate->signal_activate().connect([this]() { sortNodes(false); });
    menu->append(*miSortDate);

    menu->show_all();
    menu->popup(event->button, event->time);
}

// ---- Smart Album Dialog ----

bool AlbumBrowser::showSmartAlbumDialog (Glib::ustring& name, std::vector<SmartAlbumRule>& rules,
                                          bool& matchAll, bool isEdit)
{
    Gtk::Window* toplevel = dynamic_cast<Gtk::Window*>(get_toplevel());
    if (!toplevel) return false;

    Gtk::Dialog dlg(M("ALBUM_SMART_DIALOG_TITLE"), *toplevel, true);
    dlg.add_button(M("GENERAL_CANCEL"), Gtk::RESPONSE_CANCEL);
    dlg.add_button(M("GENERAL_OK"), Gtk::RESPONSE_OK);
    dlg.set_default_size(420, -1);

    auto* content = dlg.get_content_area();

    // Name entry
    Gtk::Box* nameRow = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 4));
    nameRow->pack_start(*Gtk::manage(new Gtk::Label(M("ALBUM_SMART_NAME"))), Gtk::PACK_SHRINK);
    Gtk::Entry* nameEntry = Gtk::manage(new Gtk::Entry());
    nameEntry->set_text(name);
    nameRow->pack_start(*nameEntry, Gtk::PACK_EXPAND_WIDGET);
    content->pack_start(*nameRow, Gtk::PACK_SHRINK, 4);

    // Match mode
    Gtk::Box* matchRow = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 4));
    Gtk::ComboBoxText* matchCombo = Gtk::manage(new Gtk::ComboBoxText());
    matchCombo->append(M("ALBUM_SMART_MATCH_ALL"));
    matchCombo->append(M("ALBUM_SMART_MATCH_ANY"));
    matchCombo->set_active(matchAll ? 0 : 1);
    matchRow->pack_start(*Gtk::manage(new Gtk::Label("Match:")), Gtk::PACK_SHRINK);
    matchRow->pack_start(*matchCombo, Gtk::PACK_SHRINK);
    content->pack_start(*matchRow, Gtk::PACK_SHRINK, 4);

    // Rule list
    Gtk::Box* rulesBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 2));

    std::vector<Glib::ustring> fieldNames = {
        M("ALBUM_SMART_FIELD_RATING"),
        M("ALBUM_SMART_FIELD_COLOR"),
        M("ALBUM_SMART_FIELD_FILETYPE"),
        M("ALBUM_SMART_FIELD_CAMERA"),
        M("ALBUM_SMART_FIELD_LENS"),
        M("ALBUM_SMART_FIELD_ISO"),
        M("ALBUM_SMART_FIELD_FOCAL"),
        M("ALBUM_SMART_FIELD_APERTURE"),
        M("ALBUM_SMART_FIELD_EDITED")
    };

    std::vector<Glib::ustring> opNames = {">=", "<=", "is", "is not", "contains"};

    struct RuleRow {
        Gtk::Box* box;
        Gtk::ComboBoxText* fieldCombo;
        Gtk::ComboBoxText* opCombo;
        Gtk::Entry* valueEntry;
    };

    auto ruleRows = std::make_shared<std::vector<RuleRow>>();

    auto addRuleRow = [&](const SmartAlbumRule* existingRule) {
        RuleRow rr;
        rr.box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 2));

        rr.fieldCombo = Gtk::manage(new Gtk::ComboBoxText());
        for (const auto& fn : fieldNames) {
            rr.fieldCombo->append(fn);
        }
        rr.fieldCombo->set_active(existingRule ? static_cast<int>(existingRule->field) : 0);
        rr.box->pack_start(*rr.fieldCombo, Gtk::PACK_SHRINK);

        rr.opCombo = Gtk::manage(new Gtk::ComboBoxText());
        for (const auto& on : opNames) {
            rr.opCombo->append(on);
        }
        rr.opCombo->set_active(existingRule ? static_cast<int>(existingRule->op) : 0);
        rr.box->pack_start(*rr.opCombo, Gtk::PACK_SHRINK);

        rr.valueEntry = Gtk::manage(new Gtk::Entry());
        rr.valueEntry->set_width_chars(10);
        if (existingRule) rr.valueEntry->set_text(existingRule->value);
        rr.box->pack_start(*rr.valueEntry, Gtk::PACK_EXPAND_WIDGET);

        Gtk::Button* removeBtn = Gtk::manage(new Gtk::Button("x"));
        removeBtn->set_relief(Gtk::RELIEF_NONE);
        auto rowPtr = rr.box;
        removeBtn->signal_clicked().connect([rulesBox, rowPtr, ruleRows]() {
            rulesBox->remove(*rowPtr);
            ruleRows->erase(
                std::remove_if(ruleRows->begin(), ruleRows->end(),
                    [rowPtr](const RuleRow& r) { return r.box == rowPtr; }),
                ruleRows->end()
            );
        });
        rr.box->pack_end(*removeBtn, Gtk::PACK_SHRINK);

        ruleRows->push_back(rr);
        rulesBox->pack_start(*rr.box, Gtk::PACK_SHRINK);
        rr.box->show_all();
    };

    for (const auto& rule : rules) {
        addRuleRow(&rule);
    }
    if (rules.empty()) {
        addRuleRow(nullptr);
    }

    content->pack_start(*rulesBox, Gtk::PACK_SHRINK, 4);

    Gtk::Button* addRuleBtn = Gtk::manage(new Gtk::Button(M("ALBUM_SMART_ADD_RULE")));
    addRuleBtn->signal_clicked().connect([&addRuleRow]() { addRuleRow(nullptr); });
    content->pack_start(*addRuleBtn, Gtk::PACK_SHRINK, 4);

    dlg.show_all_children();

    if (dlg.run() == Gtk::RESPONSE_OK) {
        name = nameEntry->get_text().c_str();
        if (name.empty()) return false;

        matchAll = (matchCombo->get_active_row_number() == 0);

        rules.clear();
        for (const auto& rr : *ruleRows) {
            SmartAlbumRule rule;
            rule.field = static_cast<SmartAlbumRule::Field>(rr.fieldCombo->get_active_row_number());
            rule.op = static_cast<SmartAlbumRule::Op>(rr.opCombo->get_active_row_number());
            rule.value = rr.valueEntry->get_text();
            if (!rule.value.empty()) {
                rules.push_back(rule);
            }
        }
        return true;
    }

    return false;
}

// ---- Serialization ----

Glib::ustring AlbumBrowser::serializeRules (const std::vector<SmartAlbumRule>& rules)
{
    Glib::ustring result;
    for (size_t i = 0; i < rules.size(); i++) {
        if (i > 0) result += ";";
        result += Glib::ustring::compose("%1:%2:%3",
            static_cast<int>(rules[i].field),
            static_cast<int>(rules[i].op),
            rules[i].value);
    }
    return result;
}

std::vector<SmartAlbumRule> AlbumBrowser::deserializeRules (const Glib::ustring& str)
{
    std::vector<SmartAlbumRule> rules;
    if (str.empty()) return rules;

    std::string s = str.c_str();
    std::istringstream stream(s);
    std::string token;
    while (std::getline(stream, token, ';')) {
        size_t pos1 = token.find(':');
        if (pos1 == std::string::npos) continue;
        size_t pos2 = token.find(':', pos1 + 1);
        if (pos2 == std::string::npos) continue;

        SmartAlbumRule rule;
        rule.field = static_cast<SmartAlbumRule::Field>(std::stoi(token.substr(0, pos1)));
        rule.op = static_cast<SmartAlbumRule::Op>(std::stoi(token.substr(pos1 + 1, pos2 - pos1 - 1)));
        rule.value = token.substr(pos2 + 1);
        rules.push_back(rule);
    }
    return rules;
}

// ---- Smart album evaluation ----

namespace {

bool matchesRule (const SmartAlbumRule& rule, const CacheImageData& cfs)
{
    double numVal = 0;
    Glib::ustring strVal;

    switch (rule.field) {
        case SmartAlbumRule::RATING:    numVal = cfs.rating; break;
        case SmartAlbumRule::COLOR_LABEL: numVal = cfs.colorLabel; break;
        case SmartAlbumRule::ISO:       numVal = cfs.iso; break;
        case SmartAlbumRule::FOCAL_LEN: numVal = cfs.focalLen; break;
        case SmartAlbumRule::APERTURE:  numVal = cfs.fnumber; break;
        case SmartAlbumRule::CAMERA:    strVal = cfs.camMake + " " + cfs.camModel; break;
        case SmartAlbumRule::LENS:      strVal = cfs.lens; break;
        case SmartAlbumRule::FILE_TYPE: strVal = cfs.filetype; break;
        case SmartAlbumRule::EDITED:    numVal = cfs.recentlySaved ? 1 : 0; break;
    }

    double ruleNum = 0;
    try { ruleNum = std::stod(std::string(rule.value.c_str())); } catch (...) {}

    bool isStringField = (rule.field == SmartAlbumRule::CAMERA ||
                          rule.field == SmartAlbumRule::LENS ||
                          rule.field == SmartAlbumRule::FILE_TYPE);

    if (isStringField) {
        Glib::ustring lhs = strVal.lowercase();
        Glib::ustring rhs = Glib::ustring(rule.value).lowercase();
        switch (rule.op) {
            case SmartAlbumRule::EQ:       return lhs == rhs;
            case SmartAlbumRule::NEQ:      return lhs != rhs;
            case SmartAlbumRule::CONTAINS: return lhs.find(rhs) != Glib::ustring::npos;
            default: return false;
        }
    } else {
        switch (rule.op) {
            case SmartAlbumRule::GTE: return numVal >= ruleNum;
            case SmartAlbumRule::LTE: return numVal <= ruleNum;
            case SmartAlbumRule::EQ:  return numVal == ruleNum;
            case SmartAlbumRule::NEQ: return numVal != ruleNum;
            default: return false;
        }
    }
}

} // anonymous namespace

// Folder/metadata scan helpers shared with the double-exposure picker.
using imagescan::listImageFiles;
using imagescan::loadCacheDataForFile;
using imagescan::loadExifForFile;
using imagescan::loadRankFromPP3;

std::vector<Glib::ustring> AlbumBrowser::evaluateSmartAlbum (const AlbumNode& node)
{
    std::vector<Glib::ustring> matches;
    if (node.type != AlbumNodeType::SMART_ALBUM || node.rules.empty()) return matches;

    // Build list of directories to scan: favorites + current directory
    std::set<Glib::ustring> dirsToScan;
    for (const auto& d : App::get().options().favoriteDirs) {
        dirsToScan.insert(d);
    }
    if (!currentDirectory_.empty()) {
        dirsToScan.insert(currentDirectory_);
    }

    for (const auto& dir : dirsToScan) {
        auto files = listImageFiles(dir);
        for (const auto& fpath : files) {
            CacheImageData cid;

            // Fast path: try loading from cache file (no thumbnail generation)
            if (!loadCacheDataForFile(fpath, cid)) {
                // Slow path: read EXIF directly (still much cheaper than thumbnail)
                if (!loadExifForFile(fpath, cid)) continue;
            }

            // Override rating/colorLabel with PP3 sidecar values (user-set rank)
            loadRankFromPP3(fpath, cid);

            bool match = node.matchAll;

            for (const auto& rule : node.rules) {
                bool ruleResult = matchesRule(rule, cid);
                if (node.matchAll) {
                    match = match && ruleResult;
                    if (!match) break;
                } else {
                    match = match || ruleResult;
                    if (match) break;
                }
            }

            if (match) {
                matches.push_back(fpath);
            }
        }
    }
    return matches;
}

// ---- Cover thumbnails ----

void AlbumBrowser::loadCoverThumbnails(int session)
{
    for (const auto& node : nodes_) {
        if (session != coverLoadSession_) return;
        if (node.type != AlbumNodeType::ALBUM) continue;

        // Skip if already cached
        if (coverCache_.find(node.id) != coverCache_.end()) continue;

        // Determine cover path
        Glib::ustring coverFile = node.coverPath;
        if (coverFile.empty() && !node.filePaths.empty()) {
            coverFile = node.filePaths.front();
        }
        if (coverFile.empty()) continue;

        Glib::RefPtr<Gdk::Pixbuf> pixbuf;
        try {
            Thumbnail* thm = CacheManager::getInstance()->getEntry(coverFile);
            if (thm) {
                double scale = 1.0;
                rtengine::IImage8* img = thm->processThumbImage(32, scale);
                if (img) {
                    auto pb = Gdk::Pixbuf::create_from_data(
                        img->getData(), Gdk::COLORSPACE_RGB, false, 8,
                        img->getWidth(), img->getHeight(), img->getWidth() * 3);
                    // Scale to 32px height, preserve aspect ratio
                    int w = img->getWidth(), h = img->getHeight();
                    if (w > 0 && h > 0) {
                        double ratio = 32.0 / h;
                        int nw = std::max(1, static_cast<int>(w * ratio));
                        int nh = 32;
                        pixbuf = pb->scale_simple(nw, nh, Gdk::INTERP_BILINEAR);
                    } else {
                        pixbuf = pb->copy();
                    }
                    delete img;
                }
                thm->decreaseRef();
            }
        } catch (...) {}

        if (!pixbuf) continue;

        int nodeId = node.id;
        Glib::signal_idle().connect_once([this, session, nodeId, pixbuf]() {
            if (session != coverLoadSession_) return;
            coverCache_[nodeId] = pixbuf;

            // Update the tree model row
            model_->foreach_iter([this, nodeId, pixbuf](const Gtk::TreeModel::iterator& iter) -> bool {
                if ((*iter)[columns_.nodeId] == nodeId) {
                    (*iter)[columns_.coverPixbuf] = pixbuf;
                    return true;
                }
                return false;
            });
        });
    }
}

// ---- Panel sizing (header drag / collapse) ----

int AlbumBrowser::effMinHeight() const
{
    const int h = App::get().options().albumPanelHeight;
    return h > 0 ? h : kAlbumTreeMinHeight;
}

int AlbumBrowser::effMaxHeight() const
{
    const int h = App::get().options().albumPanelHeight;
    return h > 0 ? h : kAlbumTreeMaxHeight;
}

void AlbumBrowser::applyPanelSizing()
{
    if (App::get().options().albumPanelCollapsed) {
        hideAlbumHoverPopup();
        expandAnimConn_.disconnect();
        scrollw_->hide();
        return;
    }

    scrollw_->set_min_content_height(effMinHeight());
    scrollw_->set_max_content_height(effMaxHeight());
    // show_all() skips the scrolled window (no_show_all), so show its
    // subtree explicitly here.
    treeView_->show();
    scrollw_->show();
}

// ---- Hover preview popup ----

std::vector<Glib::ustring> AlbumBrowser::filterByFiletype(const std::vector<Glib::ustring>& paths) const
{
    std::set<std::string> filter;
    if (filetypeFilterGetter_) {
        filter = filetypeFilterGetter_();
    }
    if (filter.empty()) {
        return paths;
    }

    std::vector<Glib::ustring> out;
    out.reserve(paths.size());
    for (const auto& p : paths) {
        const auto dotpos = p.find_last_of('.');
        if (dotpos == Glib::ustring::npos) {
            continue;
        }
        const std::string ext = p.substr(dotpos + 1).uppercase().raw();
        if (filter.count(ext)) {
            out.push_back(p);
        }
    }
    return out;
}

void AlbumBrowser::onHoverRowChanged(const Gtk::TreeModel::Path& path)
{
    if (path.empty() || dragActive_) {
        hideAlbumHoverPopup();
        return;
    }

    auto iter = model_->get_iter(path);
    const bool isAlbum = iter
        && (*iter)[columns_.nodeType] == static_cast<int>(AlbumNodeType::ALBUM);

    if (!isAlbum) {
        hideAlbumHoverPopup();
        return;
    }

    if (popupVisible_) {
        // Already showing — retarget immediately for the new row
        showAlbumHoverPopup(path);
    } else {
        // Delay before showing; fires for whichever row is hovered at expiry
        hoverPopupTimer_.disconnect();
        hoverPopupTimer_ = Glib::signal_timeout().connect([this]() -> bool {
            if (!hoveredPath_.empty()) {
                showAlbumHoverPopup(hoveredPath_);
            }
            return false;
        }, 500);
    }
}

void AlbumBrowser::showAlbumHoverPopup(const Gtk::TreeModel::Path& path)
{
    auto iter = path.empty() ? Gtk::TreeModel::iterator() : model_->get_iter(path);
    if (!iter || (*iter)[columns_.nodeType] != static_cast<int>(AlbumNodeType::ALBUM)) {
        hideAlbumHoverPopup();
        return;
    }

    const int nodeId = (*iter)[columns_.nodeId];
    AlbumNode* node = findNode(nodeId);
    if (!node) {
        hideAlbumHoverPopup();
        return;
    }

    std::vector<Glib::ustring> files = filterByFiletype(node->filePaths);
    if (files.empty()) {
        hideAlbumHoverPopup();
        return;
    }

    // Position to the right of the sidebar, aligned with the hovered row
    Gdk::Rectangle cellArea;
    treeView_->get_cell_area(path, *treeView_->get_column(0), cellArea);
    int root_x = 0, root_y = 0;
    auto binWin = treeView_->get_bin_window();
    if (binWin) {
        int bwx, bwy;
        binWin->get_origin(bwx, bwy);
        root_x = bwx;
        root_y = bwy + cellArea.get_y();
    }
    const int popupX = root_x + get_allocated_width() + 4;
    const int popupY = root_y;

    // Restart the preview for this album
    cycleConn_.disconnect();
    const int session = ++hoverPopupSession_;
    cycleFiles_ = std::move(files);
    cyclePixbufs_.clear();
    cycleLoading_.clear();
    cycleStart_ = 0;

    for (int i = 0; i < 5; i++) {
        hoverImages_[i]->clear();
        hoverImages_[i]->hide();
    }

    hoverPopup_->move(popupX, popupY);
    hoverPopup_->show();
    popupVisible_ = true;

    // Load the visible thumbnails; slow scrolling starts once they are in.
    const int initial = std::min<int>(5, static_cast<int>(cycleFiles_.size()));
    std::vector<Glib::ustring> initialPaths(cycleFiles_.begin(), cycleFiles_.begin() + initial);
    std::vector<int> indices;
    for (int i = 0; i < initial; i++) {
        indices.push_back(i);
        cycleLoading_.insert(i);
    }
    std::thread(&AlbumBrowser::loadCycleThumbsWorker, this,
                std::move(initialPaths), std::move(indices), session, true).detach();
}

void AlbumBrowser::hideAlbumHoverPopup()
{
    hoverPopupTimer_.disconnect();
    cycleConn_.disconnect();
    ++hoverPopupSession_;
    cycleFiles_.clear();
    cyclePixbufs_.clear();
    cycleLoading_.clear();
    cycleStart_ = 0;
    if (popupVisible_) {
        hoverPopup_->hide();
        popupVisible_ = false;
    }
}

void AlbumBrowser::loadCycleThumbsWorker(std::vector<Glib::ustring> files, std::vector<int> indices, int session, bool startCycleWhenDone)
{
    for (size_t i = 0; i < files.size(); ++i) {
        if (session != hoverPopupSession_) {
            return;
        }

        Glib::RefPtr<Gdk::Pixbuf> pixbuf;
        try {
            Thumbnail* thm = CacheManager::getInstance()->getEntry(files[i]);
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

        const int idx = indices[i];
        Glib::signal_idle().connect_once([this, session, idx, pixbuf]() {
            if (session != hoverPopupSession_) {
                return;
            }
            cycleLoading_.erase(idx);
            // A null entry marks an unloadable file so the cycle can skip past it
            cyclePixbufs_[idx] = pixbuf;
            if (popupVisible_) {
                applyCycleImages();
            }
        });
    }

    if (startCycleWhenDone) {
        // Queued after the per-thumb idles above, so the visible thumbnails
        // have all been delivered by the time this starts the scroll.
        Glib::signal_idle().connect_once([this, session]() {
            if (session != hoverPopupSession_ || !popupVisible_) {
                return;
            }
            startCycleTimer();
        });
    }
}

void AlbumBrowser::applyCycleImages()
{
    const int n = static_cast<int>(cycleFiles_.size());
    for (int s = 0; s < 5; s++) {
        int idx;
        if (n > 5) {
            idx = (cycleStart_ + s) % n;
        } else {
            idx = s;
            if (idx >= n) {
                hoverImages_[s]->clear();
                hoverImages_[s]->hide();
                continue;
            }
        }
        auto it = cyclePixbufs_.find(idx);
        if (it == cyclePixbufs_.end()) {
            continue; // still loading — leave the slot as-is
        }
        if (it->second) {
            hoverImages_[s]->set(it->second);
            hoverImages_[s]->show();
        } else {
            hoverImages_[s]->clear();
            hoverImages_[s]->hide();
        }
    }
}

void AlbumBrowser::startCycleTimer()
{
    if (static_cast<int>(cycleFiles_.size()) <= 5) {
        return; // the whole album is visible — nothing to scroll through
    }

    cycleConn_.disconnect();
    cycleConn_ = Glib::signal_timeout().connect([this]() -> bool {
        if (!popupVisible_ || static_cast<int>(cycleFiles_.size()) <= 5) {
            return false;
        }
        const int n = static_cast<int>(cycleFiles_.size());
        const int next = (cycleStart_ + 5) % n;
        if (!cyclePixbufs_.count(next)) {
            // Preload the incoming thumbnail; advance on a later tick once
            // it has finished loading.
            if (!cycleLoading_.count(next)) {
                cycleLoading_.insert(next);
                std::thread(&AlbumBrowser::loadCycleThumbsWorker, this,
                            std::vector<Glib::ustring>{cycleFiles_[next]},
                            std::vector<int>{next}, hoverPopupSession_, false).detach();
            }
            return true;
        }
        cycleStart_ = (cycleStart_ + 1) % n;
        applyCycleImages();
        return true;
    }, 1100);
}

void AlbumBrowser::deselectAlbum()
{
    selectionChanging_ = true;
    treeView_->get_selection()->unselect_all();
    selectedAlbumName_.clear();
    selectedNodeId_ = -1;
    closeAlbumBtn_->hide();
    selectionChanging_ = false;

    std::set<std::string> empty;
    albumSelectedSignal_.emit(empty);
    albumViewSignal_.emit("", std::vector<Glib::ustring>());
}

void AlbumBrowser::setCoverForAlbum(int nodeId, const Glib::ustring& filePath)
{
    AlbumNode* node = findNode(nodeId);
    if (!node) return;

    node->coverPath = filePath;
    coverCache_.erase(nodeId);
    saveAlbums();
    refreshTree();
}

// ---- Backward compatibility ----

std::vector<AlbumEntry> AlbumBrowser::getAlbums () const
{
    std::vector<AlbumEntry> result;
    for (const auto& node : nodes_) {
        if (node.type == AlbumNodeType::ALBUM) {
            AlbumEntry e;
            e.name = node.name;
            e.filePaths = node.filePaths;
            result.push_back(std::move(e));
        }
    }
    return result;
}
