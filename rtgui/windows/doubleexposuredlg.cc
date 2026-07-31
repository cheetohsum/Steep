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
#include "doubleexposuredlg.h"

#include <algorithm>
#include <cmath>
#include <thread>

#include <glibmm/miscutils.h>

#include "cacheimagedata.h"
#include "cachemanager.h"
#include "imagescanhelpers.h"
#include "multilangmgr.h"
#include "options.h"
#include "pathutils.h"
#include "rtimage.h"
#include "selectsindex.h"
#include "thumbnail.h"

#include "rtengine/iimage.h"

using rtengine::procparams::DoubleExposureParams;

namespace
{

constexpr int GRID_THUMB_H = 110;
constexpr int PREVIEW_THUMB_H = 380;
constexpr int PREVIEW_THUMB_HI = 1000;
constexpr size_t MAX_LAYERS = 8;

// sRGB <-> linear helpers for the approximate dialog preview. The real
// composite happens scene-linearly in the engine; this only needs to be
// perceptually faithful at picking time.
float srgbToLin(float v)
{
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

float linToSrgb(float v)
{
    v = std::max(0.f, std::min(1.f, v));
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f;
}

float shadowWeight(float y)
{
    constexpr float lo = 0.10f;
    constexpr float hi = 0.45f;

    if (y <= lo) {
        return 1.f;
    }

    if (y >= hi) {
        return 0.f;
    }

    const float t = (hi - y) / (hi - lo);
    return t * t * (3.f - 2.f * t);
}

Glib::RefPtr<Gdk::Pixbuf> pixbufFromThumb(const Glib::ustring& path, int height)
{
    Thumbnail* thm = CacheManager::getInstance()->getEntry(path);

    if (!thm) {
        return {};
    }

    Glib::RefPtr<Gdk::Pixbuf> result;
    double scale = 1.0;
    rtengine::IImage8* img = thm->processThumbImage(height, scale);

    if (img) {
        if (img->getWidth() > 0 && img->getHeight() > 0) {
            const auto pb = Gdk::Pixbuf::create_from_data(
                img->getData(), Gdk::COLORSPACE_RGB, false, 8,
                img->getWidth(), img->getHeight(), 3 * img->getWidth());
            result = pb->copy(); // detach from the IImage8 buffer before deleting it
        }

        delete img;
    }

    thm->decreaseRef();
    return result;
}

} // namespace

// ---------------------------------------------------------------------------
// DEThumbGrid — a lightweight thumbnail grid with click-order selection
// badges, drawn directly (PreviewStrip pattern).
// ---------------------------------------------------------------------------

class DEThumbGrid final : public Gtk::DrawingArea
{
public:
    struct Item {
        Glib::ustring path;
        Glib::RefPtr<Gdk::Pixbuf> pixbuf;
        int rank = 0;
        int pick = 0;
        int badge = -1; // 0-based stack index when selected
    };

    sigc::signal<void, const Glib::ustring&>& signalToggled()
    {
        return signalToggled_;
    }

    DEThumbGrid()
    {
        add_events(Gdk::BUTTON_PRESS_MASK);
        set_hexpand(true);
        set_vexpand(true);
    }

    void setItems(std::vector<Item> items)
    {
        items_ = std::move(items);
        relayout();
        queue_draw();
    }

    void setItemPixbuf(const Glib::ustring& path, const Glib::RefPtr<Gdk::Pixbuf>& pixbuf)
    {
        for (auto& item : items_) {
            if (item.path == path) {
                item.pixbuf = pixbuf;
                queue_draw();
                return;
            }
        }
    }

    void setBadges(const std::vector<rtengine::procparams::DoubleExposureParams::Layer>& layers)
    {
        for (auto& item : items_) {
            item.badge = -1;

            for (size_t i = 0; i < layers.size(); ++i) {
                if (layers[i].path == item.path) {
                    item.badge = static_cast<int>(i);
                    break;
                }
            }
        }

        queue_draw();
    }

    // 0-based stack index of the layer currently selected for adjustment.
    void setSelectedBadge(int badgeIndex)
    {
        selectedBadge_ = badgeIndex;
        queue_draw();
    }

private:
    static constexpr int CELL_W = 152;
    static constexpr int THUMB_H = 102;
    static constexpr int TEXT_H = 30;
    static constexpr int PAD = 6;

    std::vector<Item> items_;
    sigc::signal<void, const Glib::ustring&> signalToggled_;
    int cols_ = 1;
    int selectedBadge_ = -1;

    int cellH() const
    {
        return THUMB_H + TEXT_H + PAD;
    }

    void relayout()
    {
        const int width = std::max(get_allocated_width(), CELL_W);
        cols_ = std::max(1, width / CELL_W);
        const int rows = items_.empty() ? 1 : (static_cast<int>(items_.size()) + cols_ - 1) / cols_;
        set_size_request(-1, rows * cellH() + PAD);
    }

    void on_size_allocate(Gtk::Allocation& allocation) override
    {
        Gtk::DrawingArea::on_size_allocate(allocation);
        const int newCols = std::max(1, allocation.get_width() / CELL_W);

        if (newCols != cols_) {
            relayout();
        }
    }

    bool on_button_press_event(GdkEventButton* event) override
    {
        if (event->type == GDK_BUTTON_PRESS && event->button == 1) {
            const int col = static_cast<int>(event->x) / CELL_W;
            const int row = (static_cast<int>(event->y) - PAD / 2) / cellH();

            if (col >= 0 && col < cols_ && row >= 0) {
                const size_t index = static_cast<size_t>(row) * cols_ + col;

                if (index < items_.size()) {
                    signalToggled_.emit(items_[index].path);
                }
            }

            return true;
        }

        return false;
    }

    bool on_draw(const Cairo::RefPtr<Cairo::Context>& cr) override
    {
        const auto style = get_style_context();
        style->render_background(cr, 0, 0, get_allocated_width(), get_allocated_height());

        const Gdk::RGBA fg = style->get_color(Gtk::STATE_FLAG_NORMAL);

        for (size_t i = 0; i < items_.size(); ++i) {
            const Item& item = items_[i];
            const int col = static_cast<int>(i) % cols_;
            const int row = static_cast<int>(i) / cols_;
            const double cx = col * CELL_W + PAD / 2.0;
            const double cy = row * cellH() + PAD / 2.0;
            const double tw = CELL_W - PAD;
            const double th = THUMB_H;

            // thumb slot background
            cr->set_source_rgba(0.0, 0.0, 0.0, 0.25);
            cr->rectangle(cx, cy, tw, th);
            cr->fill();

            if (item.pixbuf) {
                const double sc = std::min(tw / item.pixbuf->get_width(), th / item.pixbuf->get_height());
                const double dw = item.pixbuf->get_width() * sc;
                const double dh = item.pixbuf->get_height() * sc;
                const double dx = cx + (tw - dw) / 2.0;
                const double dy = cy + (th - dh) / 2.0;

                cr->save();
                cr->translate(dx, dy);
                cr->scale(sc, sc);
                Gdk::Cairo::set_source_pixbuf(cr, item.pixbuf, 0, 0);
                cr->paint();
                cr->restore();
            }

            if (item.badge >= 0) {
                // amber selection outline + numbered badge (stack order);
                // the layer being adjusted gets a brighter, thicker outline
                const bool adjusting = item.badge == selectedBadge_;

                if (adjusting) {
                    cr->set_source_rgb(1.0, 0.84, 0.42);
                    cr->set_line_width(3.5);
                } else {
                    cr->set_source_rgb(0.886, 0.663, 0.243);
                    cr->set_line_width(2.0);
                }

                cr->rectangle(cx + 1, cy + 1, tw - 2, th - 2);
                cr->stroke();
                cr->set_source_rgb(0.886, 0.663, 0.243);

                const double bx = cx + 13.0;
                const double by = cy + 13.0;
                cr->arc(bx, by, 10.0, 0, 2 * M_PI);
                cr->fill();

                cr->set_source_rgb(0.09, 0.08, 0.06);
                cr->select_font_face("sans-serif", Cairo::FONT_SLANT_NORMAL, Cairo::FONT_WEIGHT_BOLD);
                cr->set_font_size(12.0);
                const std::string num = std::to_string(item.badge + 1);
                Cairo::TextExtents ext;
                cr->get_text_extents(num, ext);
                cr->move_to(bx - ext.width / 2.0 - ext.x_bearing, by - ext.height / 2.0 - ext.y_bearing);
                cr->show_text(num);
            }

            // caption: basename + rank stars + pick flag
            cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), 0.85);
            cr->select_font_face("sans-serif", Cairo::FONT_SLANT_NORMAL, Cairo::FONT_WEIGHT_NORMAL);
            cr->set_font_size(10.0);

            std::string caption = Glib::path_get_basename(item.path);

            if (caption.size() > 22) {
                caption = caption.substr(0, 19) + "...";
            }

            cr->move_to(cx + 2, cy + th + 12);
            cr->show_text(caption);

            double markerX = cx + 2;
            const double markerY = cy + th + 24;

            if (item.pick == 1) {
                cr->set_source_rgb(0.50, 0.70, 0.82);
                cr->arc(markerX + 4, markerY - 3.5, 3.5, 0, 2 * M_PI);
                cr->fill();
                markerX += 13;
            }

            if (item.rank > 0) {
                cr->set_source_rgb(0.85, 0.71, 0.31);
                cr->set_font_size(9.0);
                std::string stars;

                for (int s = 0; s < std::min(item.rank, 5); ++s) {
                    stars += "\xE2\x98\x85"; // ★
                }

                cr->move_to(markerX, markerY);
                cr->show_text(stars);
            }
        }

        return true;
    }
};

// ---------------------------------------------------------------------------
// DEBlendPreview — aspect-fit display of the composited preview pixbuf.
// ---------------------------------------------------------------------------

class DEBlendPreview final : public Gtk::DrawingArea
{
public:
    DEBlendPreview()
    {
        set_size_request(420, 300);
        set_hexpand(true);
        set_vexpand(true);
    }

    void setComposite(const Glib::RefPtr<Gdk::Pixbuf>& pixbuf)
    {
        composite_ = pixbuf;
        queue_draw();
    }

private:
    Glib::RefPtr<Gdk::Pixbuf> composite_;

    bool on_draw(const Cairo::RefPtr<Cairo::Context>& cr) override
    {
        const int w = get_allocated_width();
        const int h = get_allocated_height();

        cr->set_source_rgb(0.06, 0.06, 0.07);
        cr->rectangle(0, 0, w, h);
        cr->fill();

        if (!composite_) {
            const auto style = get_style_context();
            const Gdk::RGBA fg = style->get_color(Gtk::STATE_FLAG_NORMAL);
            cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), 0.6);
            cr->select_font_face("sans-serif", Cairo::FONT_SLANT_NORMAL, Cairo::FONT_WEIGHT_NORMAL);
            cr->set_font_size(12.0);
            const std::string msg = M("DOUBLEEXPOSURE_PREVIEW_EMPTY");
            Cairo::TextExtents ext;
            cr->get_text_extents(msg, ext);
            cr->move_to((w - ext.width) / 2.0, (h - ext.height) / 2.0);
            cr->show_text(msg);
            return true;
        }

        const double sc = std::min(static_cast<double>(w) / composite_->get_width(),
                                   static_cast<double>(h) / composite_->get_height());
        const double dw = composite_->get_width() * sc;
        const double dh = composite_->get_height() * sc;

        cr->save();
        cr->translate((w - dw) / 2.0, (h - dh) / 2.0);
        cr->scale(sc, sc);
        Gdk::Cairo::set_source_pixbuf(cr, composite_, 0, 0);
        cr->paint();
        cr->restore();

        return true;
    }
};

// ---------------------------------------------------------------------------
// DoubleExposureDlg
// ---------------------------------------------------------------------------

DoubleExposureDlg::DoubleExposureDlg(Gtk::Window* parent, const Glib::ustring& baseImagePath,
                                     const DoubleExposureParams& initial,
                                     const BrowserFilter* browserFilter,
                                     const Glib::ustring& browserCurrentDir) :
    Gtk::Dialog(M("DOUBLEEXPOSURE_DIALOG_TITLE"), true),
    baseImagePath_(baseImagePath),
    folderDir_(baseImagePath.empty() ? Glib::ustring() : Glib::ustring(Glib::path_get_dirname(baseImagePath))),
    browserCurrentDir_(browserCurrentDir),
    params_(initial),
    haveBrowserFilter_(browserFilter != nullptr),
    selectedLayer_(0),
    syncingControls_(false),
    globalScope_(false),
    folderScanned_(false),
    globalScanned_(false),
    folderScanRunning_(false),
    globalScanRunning_(false),
    folderScanGen_(0),
    globalScanGen_(0),
    alive_(std::make_shared<std::atomic<bool>>(true))
{
    if (browserFilter) {
        browserFilter_ = *browserFilter;
    }

    if (parent) {
        set_transient_for(*parent);
    }

    set_default_size(1060, 680);

    Gtk::Box* content = get_content_area();
    content->set_spacing(8);
    content->set_border_width(10);

    // --- toolbar: scope + filters + count ---
    Gtk::Box* toolbar = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 8));

    folderBtn_ = Gtk::manage(new Gtk::ToggleButton(M("DOUBLEEXPOSURE_SCOPE_FOLDER")));
    folderBtn_->set_tooltip_text(M("DOUBLEEXPOSURE_SCOPE_FOLDER_TOOLTIP"));
    globalBtn_ = Gtk::manage(new Gtk::ToggleButton());
    globalBtn_->set_image(*Gtk::manage(new RTImage("globe-small", Gtk::ICON_SIZE_BUTTON)));
    globalBtn_->set_tooltip_text(M("DOUBLEEXPOSURE_SCOPE_GLOBAL_TOOLTIP"));
    folderBtn_->set_active(true);
    folderBtn_->signal_toggled().connect([this]() {
        if (folderBtn_->get_active()) {
            scopeChanged(false);
        } else if (!globalBtn_->get_active()) {
            // clicking the already-active folder button: keep it active and
            // offer a different source folder instead
            folderBtn_->set_active(true);
            openFolderChooser();
        }
    });
    globalBtn_->signal_toggled().connect([this]() {
        if (globalBtn_->get_active()) {
            scopeChanged(true);
        } else if (!folderBtn_->get_active()) {
            globalBtn_->set_active(true);
        }
    });

    Gtk::Box* scopeBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 0));
    scopeBox->get_style_context()->add_class("linked");
    scopeBox->pack_start(*folderBtn_, Gtk::PACK_SHRINK);
    scopeBox->pack_start(*globalBtn_, Gtk::PACK_SHRINK);
    toolbar->pack_start(*scopeBox, Gtk::PACK_SHRINK);

    // Seed pick/star filters from the browser tab's current filter state.
    bool seedPicked = false;
    int seedMinStars = 0;

    if (haveBrowserFilter_) {
        seedPicked = browserFilter_.showPicked && !browserFilter_.showUnflagged;

        if (!browserFilter_.showRanked[0]) {
            for (int r = 1; r <= 5; ++r) {
                if (browserFilter_.showRanked[r]) {
                    seedMinStars = r;
                    break;
                }
            }
        }
    }

    pickedFilter_ = Gtk::manage(new Gtk::ToggleButton(M("DOUBLEEXPOSURE_FILTER_PICKED")));
    pickedFilter_->set_tooltip_text(M("FILEBROWSER_SHOWPICKEDHINT"));
    pickedFilter_->set_active(seedPicked);
    pickedFilter_->signal_toggled().connect(sigc::mem_fun(*this, &DoubleExposureDlg::applyFilter));
    toolbar->pack_start(*pickedFilter_, Gtk::PACK_SHRINK);

    starsFilter_ = Gtk::manage(new MyComboBoxText());
    starsFilter_->append(M("DOUBLEEXPOSURE_FILTER_ANYSTARS"));

    for (int i = 1; i <= 5; ++i) {
        starsFilter_->append(Glib::ustring::compose("\xE2\x98\x85 %1+", i));
    }

    starsFilter_->set_active(seedMinStars);
    starsFilter_->setPreferredWidth(100, 130);
    starsFilter_->connect(starsFilter_->signal_changed().connect(sigc::mem_fun(*this, &DoubleExposureDlg::applyFilter)));
    toolbar->pack_start(*starsFilter_, Gtk::PACK_SHRINK);

    spinner_ = Gtk::manage(new Gtk::Spinner());
    toolbar->pack_start(*spinner_, Gtk::PACK_SHRINK);

    countLabel_ = Gtk::manage(new Gtk::Label(""));
    countLabel_->set_halign(Gtk::ALIGN_END);
    toolbar->pack_end(*countLabel_, Gtk::PACK_SHRINK);

    content->pack_start(*toolbar, Gtk::PACK_SHRINK);

    // --- main split: grid | preview + controls ---
    Gtk::Paned* split = Gtk::manage(new Gtk::Paned(Gtk::ORIENTATION_HORIZONTAL));
    split->set_position(480);

    grid_ = Gtk::manage(new DEThumbGrid());
    grid_->signalToggled().connect(sigc::mem_fun(*this, &DoubleExposureDlg::itemToggled));

    Gtk::ScrolledWindow* gridScroll = Gtk::manage(new Gtk::ScrolledWindow());
    gridScroll->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    gridScroll->add(*grid_);
    split->pack1(*gridScroll, true, false);

    Gtk::Box* right = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 6));

    preview_ = Gtk::manage(new DEBlendPreview());
    right->pack_start(*preview_, Gtk::PACK_EXPAND_WIDGET);

    highRes_ = Gtk::manage(new Gtk::CheckButton(M("DOUBLEEXPOSURE_HIGHRES")));
    highRes_->set_halign(Gtk::ALIGN_END);
    highRes_->signal_toggled().connect(sigc::mem_fun(*this, &DoubleExposureDlg::highResToggled));
    right->pack_start(*highRes_, Gtk::PACK_SHRINK);

    // blend controls
    auto makeScaleRow = [](const Glib::ustring& label, Gtk::Scale*& outScale, double lo, double hi, double step, double value) {
        Gtk::Box* row = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
        Gtk::Label* lab = Gtk::manage(new Gtk::Label(label, Gtk::ALIGN_START));
        lab->set_size_request(150, -1);
        lab->set_xalign(0.f);
        outScale = Gtk::manage(new Gtk::Scale(Gtk::ORIENTATION_HORIZONTAL));
        outScale->set_range(lo, hi);
        outScale->set_increments(step, step * 5);
        outScale->set_value(value);
        outScale->set_draw_value(true);
        outScale->set_value_pos(Gtk::POS_RIGHT);
        outScale->set_digits(step < 1.0 ? 2 : 0);
        outScale->set_hexpand(true);
        row->pack_start(*lab, Gtk::PACK_SHRINK);
        row->pack_start(*outScale, Gtk::PACK_EXPAND_WIDGET);
        return row;
    };

    Gtk::Box* blendRow = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    Gtk::Label* blendLab = Gtk::manage(new Gtk::Label(M("TP_DOUBLEEXPOSURE_BLEND"), Gtk::ALIGN_START));
    blendLab->set_size_request(150, -1);
    blendLab->set_xalign(0.f);
    blendMethod_ = Gtk::manage(new MyComboBoxText());
    blendMethod_->append(M("TP_DOUBLEEXPOSURE_BLEND_ADD"));
    blendMethod_->append(M("TP_DOUBLEEXPOSURE_BLEND_SCREEN"));
    blendMethod_->append(M("TP_DOUBLEEXPOSURE_BLEND_MULTIPLY"));
    blendMethod_->append(M("TP_DOUBLEEXPOSURE_BLEND_LIGHTEN"));
    blendMethod_->set_active(static_cast<int>(params_.blendMode));
    blendMethod_->setPreferredWidth(180, 230);
    blendMethod_->connect(blendMethod_->signal_changed().connect(sigc::mem_fun(*this, &DoubleExposureDlg::blendControlChanged)));
    blendRow->pack_start(*blendLab, Gtk::PACK_SHRINK);
    blendRow->pack_start(*blendMethod_, Gtk::PACK_EXPAND_WIDGET);
    right->pack_start(*blendRow, Gtk::PACK_SHRINK);

    autoGain_ = Gtk::manage(new Gtk::CheckButton(M("TP_DOUBLEEXPOSURE_AUTOGAIN")));
    autoGain_->set_active(params_.autoGain);
    autoGain_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_AUTOGAIN_TOOLTIP"));
    autoGain_->signal_toggled().connect(sigc::mem_fun(*this, &DoubleExposureDlg::blendControlChanged));
    right->pack_start(*autoGain_, Gtk::PACK_SHRINK);

    right->pack_start(*makeScaleRow(M("TP_DOUBLEEXPOSURE_BASEEV"), baseEvScale_, -4.0, 4.0, 0.05, params_.baseEv), Gtk::PACK_SHRINK);
    baseEvScale_->signal_value_changed().connect(sigc::mem_fun(*this, &DoubleExposureDlg::blendControlChanged));

    right->pack_start(*makeScaleRow(M("TP_DOUBLEEXPOSURE_FILLSHADOWS"), fillShadowsScale_, 0.0, 100.0, 1.0, params_.fillShadows), Gtk::PACK_SHRINK);
    fillShadowsScale_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_FILLSHADOWS_TOOLTIP"));
    fillShadowsScale_->signal_value_changed().connect(sigc::mem_fun(*this, &DoubleExposureDlg::blendControlChanged));

    Gtk::Separator* sep = Gtk::manage(new Gtk::Separator(Gtk::ORIENTATION_HORIZONTAL));
    right->pack_start(*sep, Gtk::PACK_SHRINK);

    layerLabel_ = Gtk::manage(new Gtk::Label("", Gtk::ALIGN_START));
    layerLabel_->set_xalign(0.f);
    right->pack_start(*layerLabel_, Gtk::PACK_SHRINK);

    right->pack_start(*makeScaleRow(M("TP_DOUBLEEXPOSURE_EV"), layerEvScale_, -4.0, 4.0, 0.05, 0.0), Gtk::PACK_SHRINK);
    layerEvScale_->signal_value_changed().connect(sigc::mem_fun(*this, &DoubleExposureDlg::layerControlChanged));

    right->pack_start(*makeScaleRow(M("TP_DOUBLEEXPOSURE_OPACITY"), layerOpacityScale_, 0.0, 100.0, 1.0, 100.0), Gtk::PACK_SHRINK);
    layerOpacityScale_->signal_value_changed().connect(sigc::mem_fun(*this, &DoubleExposureDlg::layerControlChanged));

    split->pack2(*right, true, false);
    content->pack_start(*split, Gtk::PACK_EXPAND_WIDGET);

    // --- tray: base plate + ordered overlay chips ---
    trayBox_ = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    trayBox_->set_tooltip_text(M("DOUBLEEXPOSURE_ORDER_TOOLTIP"));
    Gtk::ScrolledWindow* trayScroll = Gtk::manage(new Gtk::ScrolledWindow());
    trayScroll->set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_NEVER);
    trayScroll->set_min_content_height(44);
    trayScroll->add(*trayBox_);
    content->pack_start(*trayScroll, Gtk::PACK_SHRINK);

    add_button(M("GENERAL_CANCEL"), Gtk::RESPONSE_CANCEL);
    add_button(M("GENERAL_OK"), Gtk::RESPONSE_OK);
    set_default_response(Gtk::RESPONSE_OK);

    show_all_children();
    spinner_->hide();

    rebuildTray();
    syncLayerControls();
    requestPreviewThumbs();
    startScan(false);
}

// Request preview-pane renders of the base plate and every layer at the
// resolution matching the high-res toggle.
void DoubleExposureDlg::requestPreviewThumbs()
{
    std::vector<Glib::ustring> paths = {baseImagePath_};

    for (const auto& layer : params_.layers) {
        paths.push_back(layer.path);
    }

    requestThumbs(paths, highRes_ && highRes_->get_active() ? PREVIEW_THUMB_HI : PREVIEW_THUMB_H);
}

void DoubleExposureDlg::highResToggled()
{
    requestPreviewThumbs();
    updatePreview();
}

void DoubleExposureDlg::openFolderChooser()
{
    Gtk::FileChooserDialog chooser(*this, M("DOUBLEEXPOSURE_PICK_FOLDER"), Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER);
    chooser.add_button(M("GENERAL_CANCEL"), Gtk::RESPONSE_CANCEL);
    chooser.add_button(M("GENERAL_OK"), Gtk::RESPONSE_OK);

    if (!folderDir_.empty()) {
        chooser.set_current_folder(folderDir_);
    }

    if (chooser.run() == Gtk::RESPONSE_OK && !chooser.get_filename().empty()) {
        folderDir_ = chooser.get_filename();
        folderBtn_->set_label(Glib::path_get_basename(folderDir_));
        folderScanned_ = false;
        // A still-running scan of the previous folder is obsolete: the
        // generation bump in startScan discards its late posts.
        folderScanRunning_ = false;
        folderItems_.clear();
        startScan(false);
    }
}

DoubleExposureDlg::~DoubleExposureDlg()
{
    *alive_ = false;
}

DoubleExposureParams DoubleExposureDlg::getResult() const
{
    return params_;
}

const std::vector<DoubleExposureDlg::ScanItem>& DoubleExposureDlg::activeItems() const
{
    return globalScope_ ? globalItems_ : folderItems_;
}

// --- scanning ---

void DoubleExposureDlg::startScan(bool global)
{
    if ((global && (globalScanned_ || globalScanRunning_)) || (!global && (folderScanned_ || folderScanRunning_))) {
        applyFilter();
        return;
    }

    int generation;

    if (global) {
        globalScanRunning_ = true;
        globalItems_.clear();
        generation = ++globalScanGen_;
    } else {
        folderScanRunning_ = true;
        folderItems_.clear();
        generation = ++folderScanGen_;
    }

    spinner_->show();
    spinner_->start();
    applyFilter();

    const Glib::ustring basePath = baseImagePath_;
    auto alive = alive_;

    std::vector<Glib::ustring> dirs;

    std::vector<std::pair<Glib::ustring, SelectsIndex::Entry>> indexEntries;

    if (global) {
        // "Global selects" = the persistent selects index (instant, complete
        // across every folder ever flagged) plus a reconciling walk over
        // favorites, recents, the browser's folder, and this dialog's folder.
        indexEntries = SelectsIndex::getInstance().snapshot();

        std::set<Glib::ustring> dirSet;

        for (const auto& d : App::get().options().favoriteDirs) {
            dirSet.insert(d);
        }

        for (const auto& d : App::get().options().recentFolders) {
            dirSet.insert(d);
        }

        if (!basePath.empty()) {
            dirSet.insert(Glib::path_get_dirname(basePath));
        }

        if (!browserCurrentDir_.empty()) {
            dirSet.insert(browserCurrentDir_);
        }

        if (!folderDir_.empty()) {
            dirSet.insert(folderDir_);
        }

        for (const auto& item : indexEntries) {
            dirSet.insert(Glib::path_get_dirname(item.first));
        }

        dirs.assign(dirSet.begin(), dirSet.end());
    } else if (!folderDir_.empty()) {
        dirs.push_back(folderDir_);
    }

    const bool recurse = global && App::get().options().globalScanSubfolders;

    // Inherit the browser tab's filetype filter (empty = all types).
    std::set<std::string> filetypes;

    if (haveBrowserFilter_) {
        filetypes = browserFilter_.filetypeFilter;
    }

    std::thread([this, alive, global, generation, basePath, dirs, filetypes, indexEntries, recurse]() {
        std::vector<ScanItem> batch;
        std::set<std::string> seen;

        const auto flushBatch = [&]() {
            if (batch.empty()) {
                return;
            }

            std::vector<ScanItem> ready;
            ready.swap(batch);
            // Stream results in small batches so the grid fills as we go,
            // even inside one large directory.
            Glib::signal_idle().connect_once([this, alive, global, generation, ready]() {
                if (!*alive) {
                    return;
                }

                onScanPartial(global, generation, ready);
            });
        };

        // Index pass first: instant results from every folder ever flagged.
        for (const auto& item : indexEntries) {
            if (!*alive) {
                return;
            }

            const Glib::ustring& fpath = item.first;

            if (fpath == basePath || !seen.insert(fpath.raw()).second) {
                continue;
            }

            if (!filetypes.empty()) {
                const std::string ext = getExtension(fpath).uppercase();

                if (filetypes.find(ext) == filetypes.end()) {
                    continue;
                }
            }

            if (!Glib::file_test(fpath, Glib::FILE_TEST_EXISTS)) {
                SelectsIndex::getInstance().forget(fpath);
                continue;
            }

            ScanItem scanItem;
            scanItem.path = fpath;
            scanItem.rank = std::max(0, std::min(item.second.rating, 5));
            scanItem.pick = item.second.pick;
            batch.push_back(std::move(scanItem));

            if (batch.size() >= 48) {
                flushBatch();
            }
        }

        flushBatch();

        for (const auto& dir : dirs) {
            const auto files = recurse
                ? imagescan::listImageFilesRecursive(dir, 3)
                : imagescan::listImageFiles(dir);

            for (const auto& fpath : files) {
                if (fpath == basePath || !seen.insert(fpath.raw()).second) {
                    continue; // the base plate cannot double-expose with itself
                }

                if (!filetypes.empty()) {
                    const std::string ext = getExtension(fpath).uppercase();

                    if (filetypes.find(ext) == filetypes.end()) {
                        continue;
                    }
                }

                CacheImageData cid;
                const bool haveCache = imagescan::loadCacheDataFast(fpath, cid);

                if (!haveCache) {
                    if (global) {
                        // Unknown to the cache: only a sidecar can make this
                        // file a select.
                        if (!Glib::file_test(fpath + ".pp3", Glib::FILE_TEST_EXISTS)) {
                            continue;
                        }

                        imagescan::loadPP3OverlaysFast(fpath, cid);
                    } else {
                        if (!imagescan::loadExifForFile(fpath, cid)) {
                            continue;
                        }

                        imagescan::loadPP3OverlaysFast(fpath, cid);
                    }
                } else if (!global) {
                    // Folder scope honors sidecar tweaks with one merged parse.
                    imagescan::loadPP3OverlaysFast(fpath, cid);
                } else if (cid.pickLabel == 0 && cid.rating == 0
                           && Glib::file_test(fpath + ".pp3", Glib::FILE_TEST_EXISTS)) {
                    // The cache can lag the sidecar for flags/ranks: verify
                    // apparent non-selects against the sidecar when one exists.
                    imagescan::loadPP3OverlaysFast(fpath, cid);
                }

                if (global && (haveCache || cid.rating > 0 || cid.pickLabel != 0 || cid.colorLabel > 0)) {
                    // Converge the selects index as we go (no-op when unchanged).
                    SelectsIndex::getInstance().note(fpath, cid.rating, cid.pickLabel, cid.colorLabel);
                }

                ScanItem item;
                item.path = fpath;
                item.rank = std::max(0, std::min(cid.rating, 5));
                item.pick = cid.pickLabel;
                batch.push_back(std::move(item));

                if (batch.size() >= 48) {
                    flushBatch();
                }
            }

            if (!*alive) {
                return;
            }

            flushBatch();
        }

        Glib::signal_idle().connect_once([this, alive, global, generation]() {
            if (!*alive) {
                return;
            }

            onScanDone(global, generation);
        });
    }).detach();
}

void DoubleExposureDlg::onScanPartial(bool global, int generation, const std::vector<ScanItem>& items)
{
    if (generation != (global ? globalScanGen_ : folderScanGen_)) {
        return; // stale scan (scope re-scanned meanwhile)
    }

    auto& target = global ? globalItems_ : folderItems_;
    target.insert(target.end(), items.begin(), items.end());

    if (global == globalScope_) {
        applyFilter();
    }
}

void DoubleExposureDlg::onScanDone(bool global, int generation)
{
    if (generation != (global ? globalScanGen_ : folderScanGen_)) {
        return;
    }

    if (global) {
        globalScanned_ = true;
        globalScanRunning_ = false;
    } else {
        folderScanned_ = true;
        folderScanRunning_ = false;
    }

    if (!folderScanRunning_ && !globalScanRunning_) {
        spinner_->stop();
        spinner_->hide();
    }

    if (global == globalScope_) {
        applyFilter();
    }
}

void DoubleExposureDlg::requestThumbs(const std::vector<Glib::ustring>& paths, int height)
{
    std::vector<Glib::ustring> needed;

    for (const auto& path : paths) {
        const Glib::ustring key = path + Glib::ustring::compose("|%1", height);

        if (pendingThumbs_.count(key)) {
            continue;
        }

        const auto& map = height == GRID_THUMB_H ? gridPix_ : (height == PREVIEW_THUMB_HI ? hiPix_ : bigPix_);

        if (map.count(path)) {
            continue;
        }

        pendingThumbs_.insert(key);
        needed.push_back(path);
    }

    if (needed.empty()) {
        return;
    }

    auto alive = alive_;

    std::thread([this, alive, needed, height]() {
        for (const auto& path : needed) {
            if (!*alive) {
                return;
            }

            Glib::RefPtr<Gdk::Pixbuf> pixbuf = pixbufFromThumb(path, height);

            Glib::signal_idle().connect_once([this, alive, path, height, pixbuf]() {
                if (!*alive) {
                    return;
                }

                onThumbLoaded(path, height, pixbuf);
            });
        }
    }).detach();
}

void DoubleExposureDlg::onThumbLoaded(const Glib::ustring& path, int height, Glib::RefPtr<Gdk::Pixbuf> pixbuf)
{
    pendingThumbs_.erase(path + Glib::ustring::compose("|%1", height));

    if (!pixbuf) {
        return;
    }

    if (height == GRID_THUMB_H) {
        gridPix_[path] = pixbuf;
        grid_->setItemPixbuf(path, pixbuf);
    } else if (height == PREVIEW_THUMB_HI) {
        hiPix_[path] = pixbuf;
        updatePreview();
    } else {
        bigPix_[path] = pixbuf;
        updatePreview();
    }
}

// --- UI logic ---

void DoubleExposureDlg::scopeChanged(bool global)
{
    globalScope_ = global;

    if (global) {
        folderBtn_->set_active(false);
        startScan(true);
    } else {
        globalBtn_->set_active(false);
        startScan(false);
    }

    applyFilter();
}

void DoubleExposureDlg::applyFilter()
{
    const bool pickedOnly = pickedFilter_->get_active();
    const int minStars = std::max(0, starsFilter_->get_active_row_number());

    std::vector<DEThumbGrid::Item> visible;
    const auto& source = activeItems();

    for (const auto& item : source) {
        if (pickedOnly && item.pick != 1) {
            continue;
        }

        if (minStars > 0 && item.rank < minStars) {
            continue;
        }

        DEThumbGrid::Item gi;
        gi.path = item.path;
        gi.rank = item.rank;
        gi.pick = item.pick;

        const auto pix = gridPix_.find(item.path);

        if (pix != gridPix_.end()) {
            gi.pixbuf = pix->second;
        }

        visible.push_back(std::move(gi));
    }

    countLabel_->set_text(Glib::ustring::compose(M("DOUBLEEXPOSURE_COUNT"), visible.size(), source.size()));

    std::vector<Glib::ustring> thumbRequests;

    for (const auto& gi : visible) {
        if (!gi.pixbuf) {
            thumbRequests.push_back(gi.path);
        }
    }

    grid_->setItems(std::move(visible));
    grid_->setBadges(params_.layers);
    grid_->setSelectedBadge(params_.layers.empty() ? -1 : static_cast<int>(selectedLayer_));
    requestThumbs(thumbRequests, GRID_THUMB_H);
}

void DoubleExposureDlg::itemToggled(const Glib::ustring& path)
{
    // Clicking a layer that is already in the stack selects it for
    // adjustment; removal happens on the tray chips.
    for (size_t i = 0; i < params_.layers.size(); ++i) {
        if (params_.layers[i].path == path) {
            selectLayer(i);
            return;
        }
    }

    if (params_.layers.size() >= MAX_LAYERS) {
        return;
    }

    DoubleExposureParams::Layer layer;
    layer.path = path;
    params_.layers.push_back(layer);
    selectedLayer_ = params_.layers.size() - 1;

    grid_->setBadges(params_.layers);
    grid_->setSelectedBadge(static_cast<int>(selectedLayer_));
    rebuildTray();
    syncLayerControls();
    requestThumbs({path}, highRes_->get_active() ? PREVIEW_THUMB_HI : PREVIEW_THUMB_H);
    updatePreview();
}

void DoubleExposureDlg::moveLayer(size_t index, int direction)
{
    const int target = static_cast<int>(index) + direction;

    if (index >= params_.layers.size() || target < 0 || target >= static_cast<int>(params_.layers.size())) {
        return;
    }

    std::swap(params_.layers[index], params_.layers[target]);
    selectedLayer_ = static_cast<size_t>(target);

    grid_->setBadges(params_.layers);
    grid_->setSelectedBadge(static_cast<int>(selectedLayer_));
    rebuildTray();
    syncLayerControls();
    updatePreview();
}

void DoubleExposureDlg::removeLayer(size_t index)
{
    if (index >= params_.layers.size()) {
        return;
    }

    params_.layers.erase(params_.layers.begin() + index);

    if (selectedLayer_ >= params_.layers.size() && selectedLayer_ > 0) {
        selectedLayer_ = params_.layers.size() - 1;
    }

    grid_->setBadges(params_.layers);
    grid_->setSelectedBadge(params_.layers.empty() ? -1 : static_cast<int>(selectedLayer_));
    rebuildTray();
    syncLayerControls();
    updatePreview();
}

void DoubleExposureDlg::selectLayer(size_t index)
{
    if (index < params_.layers.size()) {
        selectedLayer_ = index;
        grid_->setSelectedBadge(static_cast<int>(selectedLayer_));
        rebuildTray();
        syncLayerControls();
    }
}

void DoubleExposureDlg::rebuildTray()
{
    for (Gtk::Widget* child : trayBox_->get_children()) {
        trayBox_->remove(*child);
    }

    // base plate chip
    Gtk::Box* baseChip = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 4));
    Gtk::Label* baseLabel = Gtk::manage(new Gtk::Label(
        Glib::ustring::compose("%1 \xC2\xB7 %2", M("DOUBLEEXPOSURE_BASEPLATE"), Glib::path_get_basename(baseImagePath_))));
    baseChip->pack_start(*baseLabel, Gtk::PACK_SHRINK);
    baseChip->get_style_context()->add_class("dim-label");
    trayBox_->pack_start(*baseChip, Gtk::PACK_SHRINK);

    for (size_t i = 0; i < params_.layers.size(); ++i) {
        Gtk::Label* arrow = Gtk::manage(new Gtk::Label("\xE2\x86\x92")); // →
        arrow->get_style_context()->add_class("dim-label");
        trayBox_->pack_start(*arrow, Gtk::PACK_SHRINK);

        Gtk::Box* chip = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 2));
        chip->get_style_context()->add_class("linked");

        Gtk::Button* label = Gtk::manage(new Gtk::Button(
            Glib::ustring::compose("%1 \xC2\xB7 %2", i + 1, Glib::path_get_basename(params_.layers[i].path))));
        label->set_relief(Gtk::RELIEF_NONE);

        if (i == selectedLayer_) {
            // highlight the layer currently bound to the adjustment sliders
            label->get_style_context()->add_class("suggested-action");
        }

        const size_t idx = i;
        label->signal_clicked().connect([this, idx]() { selectLayer(idx); });
        chip->pack_start(*label, Gtk::PACK_SHRINK);

        Gtk::Button* left = Gtk::manage(new Gtk::Button("\xE2\x97\x82")); // ◂
        left->set_relief(Gtk::RELIEF_NONE);
        left->set_sensitive(i > 0);
        left->signal_clicked().connect([this, idx]() { moveLayer(idx, -1); });
        chip->pack_start(*left, Gtk::PACK_SHRINK);

        Gtk::Button* rightBtn = Gtk::manage(new Gtk::Button("\xE2\x96\xB8")); // ▸
        rightBtn->set_relief(Gtk::RELIEF_NONE);
        rightBtn->set_sensitive(i + 1 < params_.layers.size());
        rightBtn->signal_clicked().connect([this, idx]() { moveLayer(idx, 1); });
        chip->pack_start(*rightBtn, Gtk::PACK_SHRINK);

        Gtk::Button* close = Gtk::manage(new Gtk::Button("\xE2\x9C\x95")); // ✕
        close->set_relief(Gtk::RELIEF_NONE);
        close->signal_clicked().connect([this, idx]() { removeLayer(idx); });
        chip->pack_start(*close, Gtk::PACK_SHRINK);

        trayBox_->pack_start(*chip, Gtk::PACK_SHRINK);
    }

    trayBox_->show_all();
}

void DoubleExposureDlg::syncLayerControls()
{
    syncingControls_ = true;

    if (params_.layers.empty() || selectedLayer_ >= params_.layers.size()) {
        layerLabel_->set_text(M("DOUBLEEXPOSURE_NOLAYER"));
        layerEvScale_->set_sensitive(false);
        layerOpacityScale_->set_sensitive(false);
    } else {
        const auto& layer = params_.layers[selectedLayer_];
        layerLabel_->set_text(Glib::ustring::compose("%1 %2 \xE2\x80\x94 %3",
                              M("TP_DOUBLEEXPOSURE_LAYER"), selectedLayer_ + 1,
                              Glib::path_get_basename(layer.path)));
        layerEvScale_->set_sensitive(true);
        layerOpacityScale_->set_sensitive(true);
        layerEvScale_->set_value(layer.ev);
        layerOpacityScale_->set_value(layer.opacity);
    }

    autoGain_->set_sensitive(blendMethod_->get_active_row_number() == 0);

    syncingControls_ = false;
}

void DoubleExposureDlg::layerControlChanged()
{
    if (syncingControls_ || selectedLayer_ >= params_.layers.size()) {
        return;
    }

    params_.layers[selectedLayer_].ev = layerEvScale_->get_value();
    params_.layers[selectedLayer_].opacity = layerOpacityScale_->get_value();
    updatePreview();
}

void DoubleExposureDlg::blendControlChanged()
{
    if (syncingControls_) {
        return;
    }

    const int blendRow = blendMethod_->get_active_row_number();
    params_.blendMode = static_cast<DoubleExposureParams::BlendMode>(blendRow < 0 ? 0 : blendRow);
    params_.autoGain = autoGain_->get_active();
    params_.baseEv = baseEvScale_->get_value();
    params_.fillShadows = fillShadowsScale_->get_value();

    autoGain_->set_sensitive(blendRow == 0);

    updatePreview();
}

void DoubleExposureDlg::updatePreview()
{
    // Prefer the high-res render when the toggle is on and it has arrived;
    // fall back to the standard tier so the preview never goes blank.
    const bool wantHi = highRes_ && highRes_->get_active();

    const auto findPix = [this, wantHi](const Glib::ustring& path) -> Glib::RefPtr<Gdk::Pixbuf> {
        if (wantHi) {
            const auto hi = hiPix_.find(path);

            if (hi != hiPix_.end() && hi->second) {
                return hi->second;
            }
        }

        const auto it = bigPix_.find(path);
        return it != bigPix_.end() ? it->second : Glib::RefPtr<Gdk::Pixbuf>();
    };

    const Glib::RefPtr<Gdk::Pixbuf> base = findPix(baseImagePath_);

    if (!base) {
        preview_->setComposite({});
        return;
    }
    const int w = base->get_width();
    const int h = base->get_height();

    if (w <= 0 || h <= 0) {
        preview_->setComposite({});
        return;
    }

    // linearize the base
    std::vector<float> lin(static_cast<size_t>(w) * h * 3);

    float srgbLut[256];

    for (int i = 0; i < 256; ++i) {
        srgbLut[i] = srgbToLin(i / 255.f);
    }

    const guint8* baseData = base->get_pixels();
    const int baseStride = base->get_rowstride();
    const int baseChannels = base->get_n_channels();

    for (int y = 0; y < h; ++y) {
        const guint8* row = baseData + y * baseStride;

        for (int x = 0; x < w; ++x) {
            const size_t o = (static_cast<size_t>(y) * w + x) * 3;
            lin[o] = srgbLut[row[x * baseChannels]];
            lin[o + 1] = srgbLut[row[x * baseChannels + 1]];
            lin[o + 2] = srgbLut[row[x * baseChannels + 2]];
        }
    }

    struct LayerPix {
        const Glib::RefPtr<Gdk::Pixbuf> pix;
        float gain;
        float opacity;
    };

    std::vector<LayerPix> layerPix;

    const bool additive = params_.blendMode == DoubleExposureParams::BlendMode::ADD;
    const float autoGainFactor = (additive && params_.autoGain && !params_.layers.empty())
                                 ? 1.f / static_cast<float>(params_.layers.size() + 1)
                                 : 1.f;

    for (const auto& layer : params_.layers) {
        const Glib::RefPtr<Gdk::Pixbuf> pix = findPix(layer.path);

        if (!pix) {
            continue;
        }

        float gain = static_cast<float>(std::pow(2.0, layer.ev));

        if (additive) {
            gain *= autoGainFactor;
        }

        layerPix.push_back({pix, gain, static_cast<float>(layer.opacity) / 100.f});
    }

    const float baseGain = static_cast<float>(std::pow(2.0, params_.baseEv)) * autoGainFactor;
    const float fillAmount = std::max(0.f, std::min(1.f, static_cast<float>(params_.fillShadows) / 100.f));

    auto result = Gdk::Pixbuf::create(Gdk::COLORSPACE_RGB, false, 8, w, h);
    guint8* outData = result->get_pixels();
    const int outStride = result->get_rowstride();

    for (int y = 0; y < h; ++y) {
        guint8* outRow = outData + y * outStride;

        for (int x = 0; x < w; ++x) {
            const size_t o = (static_cast<size_t>(y) * w + x) * 3;
            float r = lin[o] * baseGain;
            float g = lin[o + 1] * baseGain;
            float b = lin[o + 2] * baseGain;

            for (const auto& lp : layerPix) {
                // cover-fit nearest sample from the layer pixbuf
                const int lw = lp.pix->get_width();
                const int lh = lp.pix->get_height();
                const float cover = std::max(static_cast<float>(lw) / w, static_cast<float>(lh) / h);
                int lx = static_cast<int>((x - w * 0.5f) * cover + lw * 0.5f);
                int ly = static_cast<int>((y - h * 0.5f) * cover + lh * 0.5f);
                lx = std::max(0, std::min(lx, lw - 1));
                ly = std::max(0, std::min(ly, lh - 1));

                const guint8* lrow = lp.pix->get_pixels() + ly * lp.pix->get_rowstride();
                const int lch = lp.pix->get_n_channels();
                float pr = srgbLut[lrow[lx * lch]] * lp.gain;
                float pg = srgbLut[lrow[lx * lch + 1]] * lp.gain;
                float pb = srgbLut[lrow[lx * lch + 2]] * lp.gain;

                float cr, cg, cb;

                switch (params_.blendMode) {
                    case DoubleExposureParams::BlendMode::SCREEN:
                        cr = 1.f - (1.f - std::min(1.f, r)) * (1.f - std::min(1.f, pr));
                        cg = 1.f - (1.f - std::min(1.f, g)) * (1.f - std::min(1.f, pg));
                        cb = 1.f - (1.f - std::min(1.f, b)) * (1.f - std::min(1.f, pb));
                        break;

                    case DoubleExposureParams::BlendMode::MULTIPLY:
                        cr = r * pr;
                        cg = g * pg;
                        cb = b * pb;
                        break;

                    case DoubleExposureParams::BlendMode::LIGHTEN:
                        cr = std::max(r, pr);
                        cg = std::max(g, pg);
                        cb = std::max(b, pb);
                        break;

                    case DoubleExposureParams::BlendMode::ADD:
                    default:
                        cr = r + pr;
                        cg = g + pg;
                        cb = b + pb;
                        break;
                }

                float wgt = lp.opacity;

                if (fillAmount > 0.f) {
                    const float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                    wgt *= (1.f - fillAmount) + fillAmount * shadowWeight(lum);
                }

                r += wgt * (cr - r);
                g += wgt * (cg - g);
                b += wgt * (cb - b);
            }

            outRow[x * 3] = static_cast<guint8>(linToSrgb(r) * 255.f + 0.5f);
            outRow[x * 3 + 1] = static_cast<guint8>(linToSrgb(g) * 255.f + 0.5f);
            outRow[x * 3 + 2] = static_cast<guint8>(linToSrgb(b) * 255.f + 0.5f);
        }
    }

    preview_->setComposite(result);
}
