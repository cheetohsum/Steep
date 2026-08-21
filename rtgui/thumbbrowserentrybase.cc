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
#include "thumbbrowserentrybase.h"

#include "options.h"
#include "thumbbrowserbase.h"
#include "filethumbnailbuttonset.h"
#include "rtengine/rt_math.h"
#include "rtsurface.h"

namespace
{

Glib::ustring getPaddedName(const Glib::ustring& name)
{
    enum class State {
        OTHER,
        NUMBER
    };

    constexpr unsigned int pad_width = 16;

    Glib::ustring res;

    State state = State::OTHER;
    Glib::ustring number;

    for (auto c : name) {
        switch (state) {
            case State::OTHER: {
                switch (c) {
                    case '0':
                    case '1':
                    case '2':
                    case '3':
                    case '4':
                    case '5':
                    case '6':
                    case '7':
                    case '8':
                    case '9': {
                        number += c;

                        state = State::NUMBER;
                        break;
                    }

                    default: {
                        res += c;
                        break;
                    }
                }
                break;
            }

            case State::NUMBER: {
                switch (c) {
                    case '0':
                    case '1':
                    case '2':
                    case '3':
                    case '4':
                    case '5':
                    case '6':
                    case '7':
                    case '8':
                    case '9': {
                        number += c;
                        break;
                    }

                    default: {
                        if (number.size() < pad_width) {
                            res.append(pad_width - number.size(), '0');
                        }
                        res += number;
                        res += c;
                        number.clear();

                        state = State::OTHER;
                        break;
                    }
                }
                break;
            }
        }
    }

    switch (state) {
        case State::OTHER: {
            break;
        }

        case State::NUMBER: {
            if (number.size() < pad_width) {
                res.append(pad_width - number.size(), '0');
            }
            res += number;
            break;
        }
    }

    return res;
}

}

ThumbBrowserEntryBase::ThumbBrowserEntryBase (const Glib::ustring& fname, Thumbnail *thm) :
    fnlabw(0),
    fnlabh(0),
    dtlabw(0),
    dtlabh(0),
    exlabw(0),
    exlabh(0),
    textMetricsValid_(false),
    textMetricsInfoW_(0),
    textMetricsInfoH_(0),
    textMetricsScaleFactor_(1),
    textMetricsWithFilename_(WFNAME_NONE),
    textMetricsShowDateTime_(false),
    textMetricsShowBasicExif_(false),
    textMetricsItalicStyle_(false),
    previewSize(0, 0),
    prevPos(0, 0),
    activeDeviceScale(1),
    pendingDeviceScale(1),
    upperMargin(2),
    borderWidth(0),
    textGap(4),
    sideMargin(2),
    lowerMargin(2),
    previewSlotClock_(0),
    dispname(Glib::path_get_basename(fname)),
    buttonSet(nullptr),
    width(0),
    height(0),
    expected(0, 0),
    startx(0),
    starty(0),
    ofsX(0),
    ofsY(0),
    redrawRequests(0),
    parent(nullptr),
    original(nullptr),
    bbSelected(false),
    bbFramed(false),
    bbPreview(nullptr),
    bbImageAreaIconState(0),
    cursor_type(CSUndefined),
    collate_name(getPaddedName(dispname).casefold_collate_key()),
    collate_exif(),
    collate_ext(([&fname]() -> std::string {
        auto dot = fname.rfind('.');
        if (dot != Glib::ustring::npos) {
            return fname.substr(dot + 1).casefold_collate_key();
        }
        return std::string();
    })()),
    thumbnail(thm),
    filename(fname),
    selected(false),
    drawable(false),
    filtered(false),
    framed(false),
    processing(false),
    italicstyle(false),
    edited(false),
    recentlysaved(false),
    updatepriority(false),
    visibleGeneration(0),
    withFilename(WFNAME_NONE),
    animRatingAlpha_(0),
    animColorAlpha_(0),
    animPickAlpha_(0),
    animRatingActive_(false),
    animColorActive_(false),
    animPickActive_(false)
{
}

ThumbBrowserEntryBase::~ThumbBrowserEntryBase ()
{
    animTimerConn_.disconnect();
    delete buttonSet;
}

void ThumbBrowserEntryBase::ensureInfoLines () const
{
    if (infoLinesValid || !thumbnail) {
        return;
    }

    datetimeline = thumbnail->getDateTimeString();
    exifline = thumbnail->getExifString();
    infoLinesValid = true;
}

const std::string& ThumbBrowserEntryBase::getExifCollateKey () const
{
    if (!collate_exif_valid) {
        collate_exif = thumbnail
            ? getPaddedName(thumbnail->getExifString()).casefold_collate_key()
            : std::string();
        collate_exif_valid = true;
    }

    return collate_exif;
}

bool ThumbBrowserEntryBase::buttonSetVisible () const
{
    // Retired as a display: rank/label/pick render as image overlays in all
    // views now (see drawFilmstripOverlays). The legacy button-set badge only
    // reflected in-session rating actions — its cached state was never
    // initialized from the thumbnail on folder load — and drawing both would
    // duplicate the stars. Rating remains available via keyboard shortcuts,
    // the context menu, and the editor's star bar.
    return false;
}

void ThumbBrowserEntryBase::addButtonSet (LWButtonSet* bs)
{
    buttonSet = bs;
}

void ThumbBrowserEntryBase::updateBackBuffer ()
{
    if (!parent) {
        return;
    }

    Gtk::Widget* w = parent->getDrawingArea ();
    const hidpi::ScaledDeviceSize expectedDeviceSize = expected.scaleToDevice(activeDeviceScale);

    if (backBuffer && (backBuffer->getWidth() != expectedDeviceSize.width || backBuffer->getHeight() != expectedDeviceSize.height)) {
        // deleting the existing BackBuffer
        backBuffer.reset();
    }
    if (!backBuffer) {
        backBuffer = Glib::RefPtr<BackBuffer>(new BackBuffer(expectedDeviceSize.width, expectedDeviceSize.height));
    }

    // If thumbnail is hidden by a filter, drawing to it will crash
    // if either width or height is zero then return early
    if (!backBuffer->getWidth() || !backBuffer->getHeight()) {
        return;
    }

    Cairo::RefPtr<Cairo::ImageSurface> surface = backBuffer->getSurface();
    hidpi::setDeviceScale(surface, expectedDeviceSize.device_scale);

    bbSelected = selected;
    bbFramed = framed;
    bbPreview = preview.data();

    Cairo::RefPtr<Cairo::Context> cc = Cairo::Context::create(surface);

    Glib::RefPtr<Gtk::StyleContext> style = parent->getStyle();
    Gdk::RGBA textn = parent->getNormalTextColor();
    Gdk::RGBA texts = parent->getSelectedTextColor();
    Gdk::RGBA bgn = parent->getNormalBgColor();
    Gdk::RGBA bgs = parent->getSelectedBgColor();

    // Fill entire cell with theme background color.
    // The surface is RGB24 (no alpha) so we must paint the correct color
    // explicitly — uninitialized pixels default to black.
    cc->set_source_rgb(bgn.get_red(), bgn.get_green(), bgn.get_blue());
    cc->paint();

    cc->set_antialias(Cairo::ANTIALIAS_SUBPIXEL);

    drawFrame (cc, bgs, bgn);

    // calculate height of button set (hidden if nothing to show)
    // In filmstrip mode, don't reserve space — overlays are drawn on the image
    int bsHeight = 0;

    if (buttonSetVisible()) {
        int tmp;
        buttonSet->getAllocatedDimensions(tmp, bsHeight);
    }

    int infow, infoh;
    getTextSizes(infow, infoh);

    // draw thumbnail image
    if ((activeDeviceScale == pendingDeviceScale) && !preview.empty()) {
        prevPos.x = borderWidth + (expected.width - previewSize.width) / 2;
        const int hh = expected.height - (upperMargin + bsHeight + borderWidth + infoh + lowerMargin);
        prevPos.y = upperMargin + bsHeight + borderWidth + std::max((hh - previewSize.height) / 2, 0);

        hidpi::DeviceCoord deviceOffset = prevPos.scaleToDevice(activeDeviceScale);
        const hidpi::ScaledDeviceSize target = previewSize.scaleToDevice(activeDeviceScale);

        if (previewDataLayout.width == target.width && previewDataLayout.height == target.height) {
            // clang-format off
            backBuffer->copyRGBCharData(
                preview.data(),
                0, 0, previewDataLayout.width, previewDataLayout.height,
                previewDataLayout.width * 3,
                deviceOffset.x, deviceOffset.y);
            // clang-format on
        } else if (previewDataLayout.width > 0 && previewDataLayout.height > 0
                   && target.width > 0 && target.height > 0
                   && preview.size() == static_cast<std::size_t>(previewDataLayout.width) * previewDataLayout.height * 3) {
            // Pixels rendered for a different cell size (a view switch that has
            // no retained preview yet). Scale them so the cell keeps showing the
            // photo until the correctly sized render arrives, instead of
            // flashing an empty tile.
            Glib::RefPtr<Gdk::Pixbuf> stale = Gdk::Pixbuf::create_from_data(
                preview.data(),
                Gdk::COLORSPACE_RGB,
                false,
                8,
                previewDataLayout.width,
                previewDataLayout.height,
                previewDataLayout.width * 3);

            if (stale) {
                // `cc` carries the surface device scale, so draw in logical
                // coordinates and let cairo map the device-pixel pixbuf onto
                // the logical preview rectangle.
                cc->save();
                cc->rectangle(prevPos.x, prevPos.y, previewSize.width, previewSize.height);
                cc->clip();
                cc->translate(prevPos.x, prevPos.y);
                cc->scale(
                    static_cast<double>(previewSize.width) / previewDataLayout.width,
                    static_cast<double>(previewSize.height) / previewDataLayout.height);
                Gdk::Cairo::set_source_pixbuf(cc, stale, 0, 0);
                cc->paint();
                cc->restore();
            }
        }
    }

    customBackBufferUpdate (cc);

    // draw icons onto the thumbnail area
    bbIcons = getIconsOnImageArea ();
    bbSpecificityIcons = getSpecificityIconsOnImageArea ();
    bbImageAreaIconState = getImageAreaIconState ();

    int iofs_x = 4, iofs_y = 4;
    int istartx = prevPos.x;
    int istarty = prevPos.y;

    const auto& options = App::get().options();
    if ((parent->getLocation() != ThumbBrowserBase::THLOC_EDITOR && options.showFileNames && options.overlayedFileNames)
            || (parent->getLocation() == ThumbBrowserBase::THLOC_EDITOR && options.filmStripShowFileNames)) {
        cc->begin_new_path ();
        cc->rectangle (istartx, istarty, previewSize.width, fnlabh + dtlabh + exlabh + 2 * iofs_y);

        if ((texts.get_red() + texts.get_green() + texts.get_blue()) / 3 > 0.5) {
            cc->set_source_rgba (0, 0, 0, 0.5);
        } else {
            cc->set_source_rgba (1, 1, 1, 0.5);
        }

        cc->fill ();
    }

    istartx += iofs_x;
    istarty += iofs_y;

    if (!bbIcons.empty()) {
        int igap = 2;
        int iwidth = 0;
        int iheight = 0;

        for (size_t i = 0; i < bbIcons.size(); i++) {
            iwidth += bbIcons[i]->getWidth() + (i > 0 ? igap : 0);

            if (bbIcons[i]->getHeight() > iheight) {
                iheight = bbIcons[i]->getHeight();
            }
        }

        if ((parent->getLocation() != ThumbBrowserBase::THLOC_EDITOR && (!options.showFileNames || !options.overlayedFileNames))
                || (parent->getLocation() == ThumbBrowserBase::THLOC_EDITOR && !options.filmStripShowFileNames)) {
            // Draw the transparent black background around icons
            cc->begin_new_path ();
            cc->move_to(istartx - igap, istarty);
            cc->rel_line_to(igap, -igap);
            cc->rel_line_to(iwidth, 0);
            cc->rel_line_to(igap, igap);
            cc->rel_line_to(0, iheight);
            cc->rel_line_to(-igap, igap);
            cc->rel_line_to(-iwidth, 0);
            cc->rel_line_to(-igap, -igap);
            cc->rel_line_to(0, -iheight);
            cc->set_source_rgba (0, 0, 0, 0.6);
            cc->fill ();
        }

        for (size_t i = 0; i < bbIcons.size(); i++) {
            // Draw the image at 110, 90, except for the outermost 10 pixels.
            cc->set_source(bbIcons[i]->get(), istartx, istarty);
            cc->rectangle(istartx, istarty, bbIcons[i]->getWidth(), bbIcons[i]->getHeight());
            cc->fill();
            istartx += bbIcons[i]->getWidth() + igap;
        }
    }

    if (!bbSpecificityIcons.empty()) {
        int igap = 2;
        int istartx2 = prevPos.x + previewSize.width - 1 + igap;
        int istarty2 = prevPos.y + previewSize.height - igap - 1;

        for (size_t i = 0; i < bbSpecificityIcons.size(); ++i) {
            istartx2 -= bbSpecificityIcons[i]->getWidth() - igap;
            cc->set_source(bbSpecificityIcons[i]->get(), istartx2, istarty2 - bbSpecificityIcons[i]->getHeight());
            cc->rectangle(istartx2, istarty2 - bbSpecificityIcons[i]->getHeight(), bbSpecificityIcons[i]->getWidth(), bbSpecificityIcons[i]->getHeight());
            cc->fill();
        }
    }

    if ( ( (parent->getLocation() != ThumbBrowserBase::THLOC_EDITOR && options.showFileNames)
            || (parent->getLocation() == ThumbBrowserBase::THLOC_EDITOR && options.filmStripShowFileNames))
            && withFilename > WFNAME_NONE) {
        int textposx_fn, textposx_ex, textposx_dt, textposy, textw;

        if (! ((parent->getLocation() != ThumbBrowserBase::THLOC_EDITOR && options.overlayedFileNames)
                || parent->getLocation() == ThumbBrowserBase::THLOC_EDITOR) ) {
            textposx_fn = sideMargin;
            textposx_ex = sideMargin;
            textposx_dt = sideMargin;

            textposy = expected.height - lowerMargin - infoh;
            textw = expected.width - 2 * textGap;

            if (selected) {
                cc->set_source_rgb(texts.get_red(), texts.get_green(), texts.get_blue());
            } else {
                cc->set_source_rgb(textn.get_red(), textn.get_green(), textn.get_blue());
            }
        } else {
            textposx_fn = istartx;
            textposx_ex = istartx;
            textposx_dt = istartx;
            textposy = istarty;
            textw = previewSize.width - (istartx - prevPos.x);
            cc->set_source_rgb(texts.get_red(), texts.get_green(), texts.get_blue());
        }

        // draw file name
        Glib::RefPtr<Pango::Context> context = w->get_pango_context () ;
        Pango::FontDescription fontd = w->get_style_context()->get_font();
        // Reduce text size for thumbnail labels
        int baseSize = fontd.get_size();
        fontd.set_size(baseSize * 8 / 10); // 80% of normal size
        fontd.set_weight (Pango::WEIGHT_BOLD);

        if (italicstyle) {
            fontd.set_style (Pango::STYLE_ITALIC);
        } else {
            fontd.set_style (Pango::STYLE_NORMAL);
        }

        context->set_font_description (fontd);
        Glib::RefPtr<Pango::Layout> fn = w->create_pango_layout (dispname);
        fn->set_width (textw * Pango::SCALE);
        fn->set_ellipsize (Pango::ELLIPSIZE_MIDDLE);
        cc->move_to(textposx_fn, textposy);
        fn->add_to_cairo_context (cc);
        cc->fill();

        fontd.set_weight (Pango::WEIGHT_NORMAL);
        fontd.set_style (Pango::STYLE_NORMAL);
        context->set_font_description (fontd);

        if (withFilename == WFNAME_FULL) {
            // draw date/time label
            int tpos = fnlabh;

            if (options.fbShowDateTime && !datetimeline.empty()) {
                fn = w->create_pango_layout (datetimeline);
                fn->set_width (textw * Pango::SCALE);
                fn->set_ellipsize (Pango::ELLIPSIZE_MIDDLE);
                cc->move_to(textposx_dt, textposy + tpos);
                fn->add_to_cairo_context (cc);
                cc->fill();
                tpos += dtlabh;
            }

            // draw basic exif info
            if (options.fbShowBasicExif && !exifline.empty()) {
                fn = w->create_pango_layout (exifline);
                fn->set_width (textw * Pango::SCALE);
                fn->set_ellipsize (Pango::ELLIPSIZE_MIDDLE);
                cc->move_to(textposx_ex, textposy + tpos);
                fn->add_to_cairo_context (cc);
                cc->fill();
            }
        }
    }

    backBuffer->setDirty(false);
}

void ThumbBrowserEntryBase::getTextSizes (int& infow, int& infoh)
{
    infow = 0;
    infoh = 0;

    if (withFilename == WFNAME_NONE) {
        fnlabw = fnlabh = dtlabw = dtlabh = exlabw = exlabh = 0;
        textMetricsValid_ = false;
        return;
    }

    if (!parent) {
        return;
    }

    Gtk::Widget* w = parent->getDrawingArea ();

    // calculate dimensions of the text based fields

    Glib::RefPtr<Pango::Context> context = w->get_pango_context () ;
    Pango::FontDescription fontd = w->get_style_context()->get_font();
    const Glib::ustring fontKey = fontd.to_string();
    const auto& options = App::get().options();
    const bool showDateTime = withFilename == WFNAME_FULL && options.fbShowDateTime;
    const bool showBasicExif = withFilename == WFNAME_FULL && options.fbShowBasicExif;
    const int scaleFactor = w->get_scale_factor();

    if (showDateTime || showBasicExif) {
        ensureInfoLines();
    }

    if (textMetricsValid_
        && textMetricsFont_ == fontKey
        && textMetricsScaleFactor_ == scaleFactor
        && textMetricsWithFilename_ == withFilename
        && textMetricsShowDateTime_ == showDateTime
        && textMetricsShowBasicExif_ == showBasicExif
        && textMetricsItalicStyle_ == italicstyle) {
        infow = textMetricsInfoW_;
        infoh = textMetricsInfoH_;
        return;
    }

    // filename:
    // Reduce text size for thumbnail labels (must match drawing code)
    int baseSize = fontd.get_size();
    fontd.set_size(baseSize * 8 / 10); // 80% of normal size
    fontd.set_weight (Pango::WEIGHT_BOLD);
    fontd.set_style (italicstyle ? Pango::STYLE_ITALIC : Pango::STYLE_NORMAL);
    context->set_font_description (fontd);
    Glib::RefPtr<Pango::Layout> fn = w->create_pango_layout(dispname);
    fn->get_pixel_size (fnlabw, fnlabh);

    // calculate cumulated height of all info fields
    infoh = fnlabh;

    if (withFilename == WFNAME_FULL) {
        // datetime
        fontd.set_weight (Pango::WEIGHT_NORMAL);
        fontd.set_style (Pango::STYLE_NORMAL);
        context->set_font_description (fontd);

        // add date/tile size:
        if (showDateTime) {
            fn = w->create_pango_layout (datetimeline);
            fn->get_pixel_size (dtlabw, dtlabh);
            infoh += dtlabh;

            if (dtlabw + 2 * sideMargin > infow) {
                infow = dtlabw + 2 * sideMargin;
            }
        } else {
            dtlabw = dtlabh = 0;
        }

        // basic exif data
        if (showBasicExif) {
            fn = w->create_pango_layout (exifline);
            fn->get_pixel_size (exlabw, exlabh);
            infoh += exlabh;

            if (exlabw + 2 * sideMargin > infow) {
                infow = exlabw + 2 * sideMargin;
            }
        } else {
            exlabw = exlabh = 0;
        }
    } else {
        dtlabw = dtlabh = exlabw = exlabh = 0;
    }

    textMetricsValid_ = true;
    textMetricsInfoW_ = infow;
    textMetricsInfoH_ = infoh;
    textMetricsScaleFactor_ = scaleFactor;
    textMetricsWithFilename_ = withFilename;
    textMetricsShowDateTime_ = showDateTime;
    textMetricsShowBasicExif_ = showBasicExif;
    textMetricsItalicStyle_ = italicstyle;
    textMetricsFont_ = fontKey;
}

bool ThumbBrowserEntryBase::stashPreviewForSize (hidpi::LogicalSize size, int deviceScale)
{
    if (preview.empty() || previewDataLayout.width <= 0 || previewDataLayout.height <= 0) {
        return false;
    }

    if (preview.size() != static_cast<std::size_t>(previewDataLayout.width) * previewDataLayout.height * 3) {
        return false;
    }

    // Reuse the slot already describing this size, otherwise the oldest one.
    PreviewSlot* target = nullptr;

    for (auto& slot : previewSlots_) {
        if (slot.valid && slot.logicalSize == size && slot.deviceScale == deviceScale) {
            target = &slot;
            break;
        }
    }

    if (!target) {
        for (auto& slot : previewSlots_) {
            if (!slot.valid) {
                target = &slot;
                break;
            }
        }
    }

    if (!target) {
        target = &previewSlots_[0];

        for (auto& slot : previewSlots_) {
            if (slot.lastUse < target->lastUse) {
                target = &slot;
            }
        }
    }

    target->logicalSize = size;
    target->deviceScale = deviceScale;
    target->layout = previewDataLayout;
    target->data = preview;
    target->valid = true;
    target->lastUse = ++previewSlotClock_;
    savePreviewSlotExtras(*target);

    return true;
}

bool ThumbBrowserEntryBase::restorePreviewForSize (hidpi::LogicalSize size, int deviceScale)
{
    for (auto& slot : previewSlots_) {
        if (!slot.valid || slot.deviceScale != deviceScale || !(slot.logicalSize == size)) {
            continue;
        }

        const std::size_t expectedSize =
            static_cast<std::size_t>(slot.layout.width) * slot.layout.height * 3;

        if (slot.data.size() != expectedSize || expectedSize == 0) {
            slot.valid = false;
            continue;
        }

        // Hand the pixels over rather than copying them: the live `preview`
        // becomes the only copy of this size again, so an entry never holds
        // more than one retained size plus the one on screen.
        preview = std::move(slot.data);
        previewDataLayout = slot.layout;
        activeDeviceScale = deviceScale;
        loadPreviewSlotExtras(slot);
        slot.data.clear();
        slot.data.shrink_to_fit();
        slot.valid = false;

        if (backBuffer) {
            backBuffer->setDirty(true);
        }

        return true;
    }

    return false;
}

void ThumbBrowserEntryBase::releaseOffscreenBuffers ()
{
    // Never stall the draw path for a memory optimisation; the sweep will come
    // round again.
    MyTryWriterLock lock(lockRW);

    if (!lock.owns_lock()) {
        return;
    }

    // The Cairo cell surface is the larger of the two buffers (4 bytes per
    // device pixel over the whole cell) and is cheap to rebuild from `preview`
    // — a memcpy plus text and icons, with no worker job and no disk read. The
    // decoded preview itself is kept precisely so redrawing costs nothing.
    if (backBuffer) {
        backBuffer.reset();
        bbPreview = nullptr;
    }

    // Previews retained for the other view's size only pay off near the
    // viewport; far outside it they are just resident memory.
    invalidatePreviewSlots();
}

void ThumbBrowserEntryBase::invalidatePreviewSlots ()
{
    for (auto& slot : previewSlots_) {
        slot.valid = false;
        slot.data.clear();
        slot.data.shrink_to_fit();
    }
}

void ThumbBrowserEntryBase::resize (int h)
{
    MYWRITERLOCK(l, lockRW);

    height = h;

    // Retain the pixels rendered for the size we are leaving. The grid and the
    // filmstrip ask for different heights, so keeping both lets a view switch
    // restore finished thumbnails instead of re-rendering the folder.
    if (activeDeviceScale == pendingDeviceScale) {
        stashPreviewForSize(previewSize, activeDeviceScale);
    }

    // dimensions of the button set (hidden if nothing to show)
    // In filmstrip mode, don't reserve space — overlays are drawn on the image
    int bsw = 0, bsh = 0;

    if (buttonSetVisible()) {
        buttonSet->getMinimalDimensions (bsw, bsh);
    }

    const auto& options = App::get().options();
    if (parent->getLocation() == ThumbBrowserBase::THLOC_FILEBROWSER) {
        if (options.showFileNames) {
            withFilename = WFNAME_FULL;
        } else {
            withFilename = WFNAME_NONE;
        }
    } else if (parent->getLocation() == ThumbBrowserBase::THLOC_BATCHQUEUE) {
        withFilename = WFNAME_REDUCED;
    } else {
        if (options.filmStripShowFileNames) {
            withFilename = WFNAME_REDUCED;
        } else {
            withFilename = WFNAME_NONE;
        }
    }

    // calculate the height remaining for the thumbnail image
    previewSize.height = height - upperMargin - 2 * borderWidth - lowerMargin - bsh;
    int infow = 0;
    int infoh = 0;

    // Filmstrip (editor) never reserves a text row — file names there are
    // always drawn as overlays so the strip stays tight around the images.
    if (parent->getLocation() != ThumbBrowserBase::THLOC_EDITOR && options.showFileNames && !options.overlayedFileNames) {
        // dimensions of the info text
        getTextSizes (infow, infoh);
        infoh += textGap;
        //previewSize.height -= infoh;
        height += infoh;
    }

    // Minimum size for thumbs
    if (previewSize.height < 24) {
        previewSize.height = 24;
        height = previewSize.height + (upperMargin + 2 * borderWidth + lowerMargin) + bsh + infoh;
    }

    calcThumbnailSize ();  // recalculates previewSize

    width = previewSize.width + 2 * sideMargin + 2 * borderWidth;

    if (parent->getLocation() != ThumbBrowserBase::THLOC_EDITOR && options.showFileNames && !options.overlayedFileNames) {
        width = previewSize.width + 2 * sideMargin + 2 * borderWidth;

        if (width < infow + 2 * sideMargin + 2 * borderWidth) {
            width = infow + 2 * sideMargin + 2 * borderWidth;
        }
    }

    if (width < bsw + 2 * sideMargin + 2 * borderWidth) {
        width = bsw + 2 * sideMargin + 2 * borderWidth;
    }

    // calcThumbnailSize() above already reacted to the size change: it retains
    // the outgoing pixels in a per-size slot, restores a previously rendered
    // preview when one exists, and otherwise keeps the stale pixels as a
    // scaled placeholder. Dropping the preview a second time here would undo
    // that and force a full re-render of the whole folder on every view switch.
    if (backBuffer) {
        backBuffer->setDirty(true);    // This will force a backBuffer update on queue_draw
    }

    // Don't mark filtered entries as drawable — they should remain hidden
    // until arrangeFiles() explicitly sets drawable based on filter state.
    if (!filtered) {
        drawable = true;
    }
}

std::pair<hidpi::LogicalSize, int> ThumbBrowserEntryBase::getDesiredPreviewSize() const {
    return {previewSize, pendingDeviceScale};
}

void ThumbBrowserEntryBase::onDeviceScaleChanged(int newDeviceScale) {
    if (newDeviceScale != activeDeviceScale) {
        pendingDeviceScale = newDeviceScale;
        if (!filtered) {
            refreshThumbnailImage();
        }
    }
}

void ThumbBrowserEntryBase::appendQuickThumbnailJob(std::vector<ThumbImageUpdater::Request>& requests, bool cachePixbuf)
{
    (void)requests;
    (void)cachePixbuf;
    refreshQuickThumbnailImage();
}

void ThumbBrowserEntryBase::drawFrame (Cairo::RefPtr<Cairo::Context> cc, const Gdk::RGBA& bg, const Gdk::RGBA& fg)
{
    // Only draw a subtle highlight fill for selected thumbnails — no borders
    if (selected) {
        cc->rectangle(0, 0, expected.width, expected.height);
        cc->set_source_rgb(bg.get_red(), bg.get_green(), bg.get_blue());
        cc->fill();
    }
}

void ThumbBrowserEntryBase::draw (Cairo::RefPtr<Cairo::Context> cc)
{
    if (!drawable || !parent) {
        return;
    }

    MYREADERLOCK(l, lockRW);  // No resizes, position moves etc. inbetween

    bool shouldUpdateBackBuffer = [&]() -> bool {
        if (!backBuffer || backBuffer->isDirty()) return true;

        if (selected != bbSelected || framed != bbFramed
                || imageAreaIconsChanged()) {
            return true;
        }

        int bbWidth = backBuffer->getWidth();
        int bbHeight = backBuffer->getHeight();
        hidpi::ScaledDeviceSize device = expected.scaleToDevice(activeDeviceScale);
        if (device.width != bbWidth || device.height != bbHeight
                || preview.data() != bbPreview) {
            return true;
        }

        return false;
    }();

    if (shouldUpdateBackBuffer) {
        updateBackBuffer();
    }
    if (!backBuffer->surfaceCreated()) return;

    const int offsetX = getOffsetX();
    const int offsetY = getOffsetY();
    int x_offset = startx + offsetX;
    int y_offset = starty + offsetY;
    // Clip to entry bounds so no entry can paint outside its area
    cc->save();
    cc->rectangle(x_offset, y_offset, expected.width, expected.height);
    cc->clip();
    cc->set_source(backBuffer->getSurface(), x_offset, y_offset);
    cc->paint();

    // Selection highlight: accent frame + soft inner glow drawn just inside
    // the image bounds so nothing is clipped even in the tight filmstrip.
    if (selected && previewSize.width > 12 && previewSize.height > 12) {
        const double ar = 100.0 / 255.0, ag = 160.0 / 255.0, ab = 1.0;  // theme accent
        const double px = x_offset + prevPos.x;
        const double py = y_offset + prevPos.y;
        const double pw = previewSize.width;
        const double ph = previewSize.height;

        auto roundedInset = [&cc, px, py, pw, ph](double inset, double rad) {
            const double x0 = px + inset, y0 = py + inset;
            const double w = pw - 2.0 * inset, h = ph - 2.0 * inset;
            cc->begin_new_path();
            cc->arc(x0 + w - rad, y0 + rad, rad, -G_PI / 2.0, 0);
            cc->arc(x0 + w - rad, y0 + h - rad, rad, 0, G_PI / 2.0);
            cc->arc(x0 + rad, y0 + h - rad, rad, G_PI / 2.0, G_PI);
            cc->arc(x0 + rad, y0 + rad, rad, G_PI, 1.5 * G_PI);
            cc->close_path();
        };

        cc->set_line_join(Cairo::LINE_JOIN_ROUND);

        // Soft glow bleeding inward from the frame
        roundedInset(4.0, 3.0);
        cc->set_source_rgba(ar, ag, ab, 0.14);
        cc->set_line_width(5.0);
        cc->stroke();

        // Crisp accent frame hugging the image edge
        roundedInset(1.25, 3.5);
        cc->set_source_rgba(ar, ag, ab, 0.95);
        cc->set_line_width(2.5);
        cc->stroke();

        // White hairline inside the accent for extra pop
        roundedInset(3.25, 2.5);
        cc->set_source_rgba(1.0, 1.0, 1.0, 0.30);
        cc->set_line_width(1.0);
        cc->stroke();
    }

    cc->restore();

    // Rating/label/pick overlays draw directly on the image in every view
    // (browser and filmstrip). They read the thumbnail's live state, so
    // flags and stars are correct immediately after a folder loads.
    if (thumbnail) {
        drawFilmstripOverlays(cc, x_offset, y_offset);
    }

    // redraw button set above the thumbnail (hidden if no rank/label, or in filmstrip/tab mode)
    if (buttonSetVisible()) {
        buttonSet->setColors (selected ? parent->getSelectedBgColor() : parent->getNormalBgColor(), selected ? parent->getNormalBgColor() : parent->getSelectedBgColor());
        buttonSet->redraw (cc);
    }
}

void ThumbBrowserEntryBase::setPosition (int x, int y, int w, int h)
{
    MYWRITERLOCK(l, lockRW);

    expected.width = w;
    expected.height = h;
    startx = x;
    starty = y;

    if (buttonSetVisible()) {
        buttonSet->arrangeButtons (ofsX + x + sideMargin, ofsY + y + upperMargin, w - 2 * sideMargin, -1);
    }
}

void ThumbBrowserEntryBase::setOffset (int x, int y)
{
    // getOffsetX/Y read the parent's scroll offset whenever the entry has a
    // parent, which is always true inside a browser, and the button set is
    // permanently hidden (see buttonSetVisible). These fields therefore only
    // matter for a parentless entry, so this does not need the write lock —
    // taking it here stalled every reader of the entry (including its own
    // draw) once per visible entry per scroll frame and per pointer event.
    ofsX = -x;
    ofsY = -y;
}

int ThumbBrowserEntryBase::getOffsetX () const
{
    return parent ? parent->getScrollOffsetX() : ofsX;
}

int ThumbBrowserEntryBase::getOffsetY () const
{
    return parent ? parent->getScrollOffsetY() : ofsY;
}

int ThumbBrowserEntryBase::getX () const
{
    return getOffsetX() + startx;
}

int ThumbBrowserEntryBase::getY () const
{
    return getOffsetY() + starty;
}

bool ThumbBrowserEntryBase::shouldCacheRenderedThumbnailPixbuf () const
{
    return parent && parent->getLocation() == ThumbBrowserBase::THLOC_EDITOR;
}

bool ThumbBrowserEntryBase::inside (int x, int y) const
{

    const int offsetX = getOffsetX();
    const int offsetY = getOffsetY();
    return x > offsetX + startx && x < offsetX + startx + expected.width && y > offsetY + starty && y < offsetY + starty + expected.height;
}

rtengine::Coord2D ThumbBrowserEntryBase::getPosInImgSpace (int x, int y) const
{
    rtengine::Coord2D coord(-1., -1.);

    if (!preview.empty()) {
        x -= getOffsetX() + startx;
        y -= getOffsetY() + starty;

        if (x >= prevPos.x && x <= prevPos.x + previewSize.width
                && y >= prevPos.y && y <= prevPos.y + previewSize.height) {
            coord.x = double(x - prevPos.x) / double(previewSize.width);
            coord.y = double(y - prevPos.y) / double(previewSize.height);
        }
    }
    return coord;
}

bool ThumbBrowserEntryBase::insideWindow (int x, int y, int w, int h) const
{

    const int offsetX = getOffsetX();
    const int offsetY = getOffsetY();
    return !(offsetX + startx > x + w || offsetX + startx + expected.width < x || offsetY + starty > y + h || offsetY + starty + expected.height < y);
}

std::vector<std::shared_ptr<RTSurface>> ThumbBrowserEntryBase::getIconsOnImageArea()
{
    return std::vector<std::shared_ptr<RTSurface>>();
}

std::vector<std::shared_ptr<RTSurface>> ThumbBrowserEntryBase::getSpecificityIconsOnImageArea()
{
    return std::vector<std::shared_ptr<RTSurface>>();
}

std::size_t ThumbBrowserEntryBase::getImageAreaIconState()
{
    return 0;
}

bool ThumbBrowserEntryBase::imageAreaIconsChanged()
{
    return getIconsOnImageArea() != bbIcons
            || getSpecificityIconsOnImageArea() != bbSpecificityIcons;
}

bool ThumbBrowserEntryBase::motionNotify  (int x, int y)
{

    return buttonSetVisible() ? buttonSet->motionNotify (x, y) : false;
}

bool ThumbBrowserEntryBase::pressNotify   (int button, int type, int bstate, int x, int y)
{

    return buttonSetVisible() ? buttonSet->pressNotify (x, y) : false;
}

bool ThumbBrowserEntryBase::releaseNotify (int button, int type, int bstate, int x, int y)
{

    return buttonSetVisible() ? buttonSet->releaseNotify (x, y) : false;
}

std::tuple<Glib::ustring, bool> ThumbBrowserEntryBase::getToolTip (int x, int y) const
{
    Glib::ustring tooltip;

    if (buttonSetVisible()) {
        tooltip = buttonSet->getToolTip(x, y);
    }

    // Always show the filename in the tooltip since the filename in the thumbnail could be truncated.
    // If "Show Exif info" is disabled, also show Exif info in the tooltip.
    bool useMarkup = !tooltip.empty();
    if (inside(x, y) && tooltip.empty()) {
        tooltip = dispname;

        const auto& options = App::get().options();
        if (withFilename < WFNAME_FULL) {
            if (options.fbShowDateTime || options.fbShowBasicExif) {
                ensureInfoLines();
            }

            if (options.fbShowDateTime && !datetimeline.empty()) {
                tooltip += Glib::ustring("\n") + datetimeline;
            }

            if (options.fbShowBasicExif && !exifline.empty()) {
                tooltip += Glib::ustring("\n") + exifline;
            }
        }
    }

    return std::make_tuple(std::move(tooltip), useMarkup);
}

void ThumbBrowserEntryBase::drawFilmstripOverlays (Cairo::RefPtr<Cairo::Context> cc, int x, int y)
{
    if (!thumbnail || !parent) return;

    int rank = thumbnail->getRank();
    int clabel = thumbnail->getColorLabel();
    int pick = thumbnail->getPick();
    if (rank <= 0 && clabel <= 0 && pick == 0 && !animRatingActive_ && !animColorActive_ && !animPickActive_) return;

    // Icon area: bottom-left for stars, top-left for color label, top-right for pick flag
    int imgX = x + prevPos.x;
    int imgY = y + prevPos.y;
    int imgW = previewSize.width;
    int imgH = previewSize.height;

    // Draw rating animation glow
    if (animRatingActive_ && animRatingAlpha_ > 0) {
        cc->save();
        cc->rectangle(imgX, imgY, imgW, imgH);
        cc->clip();
        // Golden glow from bottom
        auto grad = Cairo::LinearGradient::create(imgX, imgY + imgH, imgX, imgY + imgH * 0.5);
        grad->add_color_stop_rgba(0, 1.0, 0.84, 0.0, 0.4 * animRatingAlpha_);
        grad->add_color_stop_rgba(1, 1.0, 0.84, 0.0, 0.0);
        cc->set_source(grad);
        cc->paint();
        cc->restore();
    }

    // Draw color label animation glow
    if (animColorActive_ && animColorAlpha_ > 0 && clabel > 0) {
        static const double colorR[] = {0, 1.0, 1.0, 0.2, 0.2, 0.6};
        static const double colorG[] = {0, 0.2, 0.85, 0.8, 0.4, 0.2};
        static const double colorB[] = {0, 0.2, 0.0, 0.2, 1.0, 0.8};
        int ci = std::min(clabel, 5);
        cc->save();
        // Colored border glow
        double glowW = 3.0 * animColorAlpha_;
        cc->set_source_rgba(colorR[ci], colorG[ci], colorB[ci], 0.7 * animColorAlpha_);
        cc->set_line_width(glowW);
        cc->rectangle(imgX + glowW/2, imgY + glowW/2, imgW - glowW, imgH - glowW);
        cc->stroke();
        cc->restore();
    }

    // Draw pick animation glow
    if (animPickActive_ && animPickAlpha_ > 0) {
        cc->save();
        cc->rectangle(imgX, imgY, imgW, imgH);
        cc->clip();
        if (pick == 1) {
            // Green glow from top for pick
            auto grad = Cairo::LinearGradient::create(imgX, imgY, imgX, imgY + imgH * 0.5);
            grad->add_color_stop_rgba(0, 0.2, 0.9, 0.2, 0.4 * animPickAlpha_);
            grad->add_color_stop_rgba(1, 0.2, 0.9, 0.2, 0.0);
            cc->set_source(grad);
        } else if (pick == -1) {
            // Red glow from top for reject
            auto grad = Cairo::LinearGradient::create(imgX, imgY, imgX, imgY + imgH * 0.5);
            grad->add_color_stop_rgba(0, 0.9, 0.2, 0.2, 0.4 * animPickAlpha_);
            grad->add_color_stop_rgba(1, 0.9, 0.2, 0.2, 0.0);
            cc->set_source(grad);
        }
        cc->paint();
        cc->restore();
    }

    // Draw star icons at bottom-left
    if (rank > 0 && FileThumbnailButtonSet::rankIcon) {
        auto starSurf = FileThumbnailButtonSet::rankIcon->get();
        if (starSurf) {
            int iconW = FileThumbnailButtonSet::rankIcon->getWidth();
            int iconH = FileThumbnailButtonSet::rankIcon->getHeight();

            int gap = 1;
            int totalW = rank * (iconW + gap) - gap;
            int availW = imgW - 6; // 3px margin each side

            // Scale down stars if they don't fit (e.g. narrow portrait thumbnails)
            double fitScale = 1.0;
            if (totalW > availW && totalW > 0) {
                fitScale = static_cast<double>(availW) / totalW;
            }

            int sx = imgX + 3;
            int sy = imgY + imgH - static_cast<int>(iconH * fitScale) - 3;

            // Semi-transparent backdrop pill
            int scaledTotalW = static_cast<int>(totalW * fitScale);
            int scaledIconH = static_cast<int>(iconH * fitScale);
            cc->set_source_rgba(0, 0, 0, 0.5);
            double radius = 3.0;
            cc->begin_new_path();
            cc->arc(sx - 2 + radius, sy - 2 + radius, radius, M_PI, 1.5 * M_PI);
            cc->arc(sx + scaledTotalW + 2 - radius, sy - 2 + radius, radius, 1.5 * M_PI, 2.0 * M_PI);
            cc->arc(sx + scaledTotalW + 2 - radius, sy + scaledIconH + 2 - radius, radius, 0, 0.5 * M_PI);
            cc->arc(sx - 2 + radius, sy + scaledIconH + 2 - radius, radius, 0.5 * M_PI, M_PI);
            cc->close_path();
            cc->fill();

            // Draw stars with scale-up animation on recent change
            double starScale = fitScale;
            if (animRatingActive_ && animRatingAlpha_ > 0.3) {
                starScale *= 1.0 + 0.3 * (animRatingAlpha_ - 0.3) / 0.7;
            }
            double drawX = sx;
            for (int i = 0; i < rank; i++) {
                cc->save();
                double scx = drawX + iconW * fitScale / 2.0;
                double scy = sy + scaledIconH / 2.0;
                cc->translate(scx, scy);
                cc->scale(starScale, starScale);
                cc->translate(-scx, -scy);
                cc->set_source(starSurf, drawX, sy);
                cc->rectangle(drawX, sy, iconW, iconH);
                cc->fill();
                cc->restore();
                drawX += (iconW + gap) * fitScale;
            }
        }
    }

    // Draw pick/reject flag at top-right
    if (pick != 0) {
        auto& flagIconPtr = (pick == 1) ? FileThumbnailButtonSet::pickIcon : FileThumbnailButtonSet::rejectIcon;
        if (flagIconPtr) {
            auto flagSurf = flagIconPtr->get();
            if (flagSurf) {
                int iconW = flagIconPtr->getWidth();
                int iconH = flagIconPtr->getHeight();
                int fx = imgX + imgW - iconW - 3;
                int fy = imgY + 3;

                // Semi-transparent backdrop pill
                cc->set_source_rgba(0, 0, 0, 0.5);
                double radius = 3.0;
                cc->begin_new_path();
                cc->arc(fx - 2 + radius, fy - 2 + radius, radius, M_PI, 1.5 * M_PI);
                cc->arc(fx + iconW + 2 - radius, fy - 2 + radius, radius, 1.5 * M_PI, 2.0 * M_PI);
                cc->arc(fx + iconW + 2 - radius, fy + iconH + 2 - radius, radius, 0, 0.5 * M_PI);
                cc->arc(fx - 2 + radius, fy + iconH + 2 - radius, radius, 0.5 * M_PI, M_PI);
                cc->close_path();
                cc->fill();

                cc->set_source(flagSurf, fx, fy);
                cc->rectangle(fx, fy, iconW, iconH);
                cc->fill();
            }
        }
    }

    // Draw color label circle at top-left (scaled down 80%)
    if (clabel > 0 && clabel <= 5 && FileThumbnailButtonSet::colorLabelIcon[clabel]) {
        auto circSurf = FileThumbnailButtonSet::colorLabelIcon[clabel]->get();
        if (circSurf) {
            int iconW = FileThumbnailButtonSet::colorLabelIcon[clabel]->getWidth();
            int iconH = FileThumbnailButtonSet::colorLabelIcon[clabel]->getHeight();
            double clScale = 0.8;
            int cx = imgX + 3;
            int cy = imgY + 3;

            // Semi-transparent backdrop circle
            cc->set_source_rgba(0, 0, 0, 0.5);
            cc->arc(cx + iconW * clScale / 2.0, cy + iconH * clScale / 2.0,
                    iconW * clScale / 2.0 + 1.5, 0, 2 * M_PI);
            cc->fill();

            // Draw scaled icon
            cc->save();
            cc->translate(cx, cy);
            cc->scale(clScale, clScale);
            cc->set_source(circSurf, 0, 0);
            cc->rectangle(0, 0, iconW, iconH);
            cc->fill();
            cc->restore();
        }
    }
}

void ThumbBrowserEntryBase::startRatingAnimation ()
{
    animRatingActive_ = true;
    animRatingAlpha_ = 1.0;
    animColorActive_ = animColorActive_; // preserve existing color anim
    animStartTime_ = std::chrono::steady_clock::now();

    if (!animTimerConn_.connected()) {
        animTimerConn_ = Glib::signal_timeout().connect(
            sigc::mem_fun(*this, &ThumbBrowserEntryBase::animTick), 30);
    }

    if (parent) {
        parent->getDrawingArea()->queue_draw();
    }
}

void ThumbBrowserEntryBase::startColorLabelAnimation ()
{
    animColorActive_ = true;
    animColorAlpha_ = 1.0;
    animRatingActive_ = animRatingActive_; // preserve existing rating anim
    animStartTime_ = std::chrono::steady_clock::now();

    if (!animTimerConn_.connected()) {
        animTimerConn_ = Glib::signal_timeout().connect(
            sigc::mem_fun(*this, &ThumbBrowserEntryBase::animTick), 30);
    }

    if (parent) {
        parent->getDrawingArea()->queue_draw();
    }
}

void ThumbBrowserEntryBase::startPickAnimation ()
{
    animPickActive_ = true;
    animPickAlpha_ = 1.0;
    animStartTime_ = std::chrono::steady_clock::now();

    if (!animTimerConn_.connected()) {
        animTimerConn_ = Glib::signal_timeout().connect(
            sigc::mem_fun(*this, &ThumbBrowserEntryBase::animTick), 30);
    }

    if (parent) {
        parent->getDrawingArea()->queue_draw();
    }
}

bool ThumbBrowserEntryBase::animTick ()
{
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double, std::milli>(now - animStartTime_).count();
    double t = std::min(1.0, elapsed / ANIM_DURATION_MS);

    // Ease-out cubic
    double ease = 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t);

    if (animRatingActive_) {
        animRatingAlpha_ = std::max(0.0, 1.0 - ease);
    }
    if (animColorActive_) {
        animColorAlpha_ = std::max(0.0, 1.0 - ease);
    }
    if (animPickActive_) {
        animPickAlpha_ = std::max(0.0, 1.0 - ease);
    }

    if (parent) {
        parent->getDrawingArea()->queue_draw();
    }

    if (t >= 1.0) {
        animRatingActive_ = false;
        animColorActive_ = false;
        animPickActive_ = false;
        animRatingAlpha_ = 0;
        animColorAlpha_ = 0;
        animPickAlpha_ = 0;
        return false; // disconnect timer
    }
    return true; // continue
}
