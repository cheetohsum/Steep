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
    previewSize(0, 0),
    prevPos(0, 0),
    activeDeviceScale(1),
    pendingDeviceScale(1),
    upperMargin(2),
    borderWidth(0),
    textGap(4),
    sideMargin(2),
    lowerMargin(2),
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
    cursor_type(CSUndefined),
    collate_name(getPaddedName(dispname).casefold_collate_key()),
    collate_exif(getPaddedName(thm->getExifString()).casefold_collate_key()),
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
    bool inFilmstrip = parent && parent->getLocation() == ThumbBrowserBase::THLOC_EDITOR;

    if (buttonSet && buttonSet->shouldShow() && !inFilmstrip) {
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

        // clang-format off
        backBuffer->copyRGBCharData(
            preview.data(),
            0, 0, previewDataLayout.width, previewDataLayout.height,
            previewDataLayout.width * 3,
            deviceOffset.x, deviceOffset.y);
        // clang-format on
    }

    customBackBufferUpdate (cc);

    // draw icons onto the thumbnail area
    bbIcons = getIconsOnImageArea ();
    bbSpecificityIcons = getSpecificityIconsOnImageArea ();

    int iofs_x = 4, iofs_y = 4;
    int istartx = prevPos.x;
    int istarty = prevPos.y;

    const auto& options = App::get().options();
    if ((parent->getLocation() != ThumbBrowserBase::THLOC_EDITOR && options.showFileNames && options.overlayedFileNames)
            || (parent->getLocation() == ThumbBrowserBase::THLOC_EDITOR && options.filmStripShowFileNames && options.filmStripOverlayedFileNames)) {
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
                || (parent->getLocation() == ThumbBrowserBase::THLOC_EDITOR && (!options.filmStripShowFileNames || !options.filmStripOverlayedFileNames))) {
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
                || (parent->getLocation() == ThumbBrowserBase::THLOC_EDITOR && options.filmStripOverlayedFileNames)) ) {
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

    if (!parent) {
        return;
    }

    Gtk::Widget* w = parent->getDrawingArea ();

    // calculate dimensions of the text based fields

    Glib::RefPtr<Pango::Context> context = w->get_pango_context () ;
    context->set_font_description (w->get_style_context()->get_font());


    // filename:
    Pango::FontDescription fontd = w->get_style_context()->get_font();
    // Reduce text size for thumbnail labels (must match drawing code)
    int baseSize = fontd.get_size();
    fontd.set_size(baseSize * 8 / 10); // 80% of normal size
    fontd.set_weight (Pango::WEIGHT_BOLD);
    context->set_font_description (fontd);
    Glib::RefPtr<Pango::Layout> fn = w->create_pango_layout(dispname);
    fn->get_pixel_size (fnlabw, fnlabh);

    // calculate cumulated height of all info fields
    infoh = fnlabh;
    infow = 0;

    if (withFilename == WFNAME_FULL) {
        const auto& options = App::get().options();

        // datetime
        fontd.set_weight (Pango::WEIGHT_NORMAL);
        context->set_font_description (fontd);
        fn = w->create_pango_layout (datetimeline);
        fn->get_pixel_size (dtlabw, dtlabh);

        // basic exif data
        fn = w->create_pango_layout (exifline);
        fn->get_pixel_size (exlabw, exlabh);

        // add date/tile size:
        if (options.fbShowDateTime) {
            infoh += dtlabh;

            if (dtlabw + 2 * sideMargin > infow) {
                infow = dtlabw + 2 * sideMargin;
            }
        } else {
            dtlabw = dtlabh = 0;
        }

        if (options.fbShowBasicExif) {
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
}

void ThumbBrowserEntryBase::resize (int h)
{
    MYWRITERLOCK(l, lockRW);

    height = h;
    int old_preh = previewSize.height;
    int old_prew = previewSize.width;

    // dimensions of the button set (hidden if nothing to show)
    // In filmstrip mode, don't reserve space — overlays are drawn on the image
    int bsw = 0, bsh = 0;
    bool inFilmstrip = parent && parent->getLocation() == ThumbBrowserBase::THLOC_EDITOR;

    if (buttonSet && buttonSet->shouldShow() && !inFilmstrip) {
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

    if (    (parent->getLocation() != ThumbBrowserBase::THLOC_EDITOR && options.showFileNames && !options.overlayedFileNames)
            || (parent->getLocation() == ThumbBrowserBase::THLOC_EDITOR && options.filmStripShowFileNames && !options.filmStripOverlayedFileNames)) {
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

    if (    (parent->getLocation() != ThumbBrowserBase::THLOC_EDITOR && options.showFileNames && !options.overlayedFileNames)
            || (parent->getLocation() == ThumbBrowserBase::THLOC_EDITOR && options.filmStripShowFileNames && !options.filmStripOverlayedFileNames)) {
        width = previewSize.width + 2 * sideMargin + 2 * borderWidth;

        if (width < infow + 2 * sideMargin + 2 * borderWidth) {
            width = infow + 2 * sideMargin + 2 * borderWidth;
        }
    }

    if (width < bsw + 2 * sideMargin + 2 * borderWidth) {
        width = bsw + 2 * sideMargin + 2 * borderWidth;
    }

    if (previewSize.height != old_preh || previewSize.width != old_prew) { // if new thumbnail height or new orientation
        preview.clear();
        refreshThumbnailImage ();
    } else if (backBuffer) {
        backBuffer->setDirty(true);    // This will force a backBuffer update on queue_draw
    }

    drawable = true;
}

std::pair<hidpi::LogicalSize, int> ThumbBrowserEntryBase::getDesiredPreviewSize() const {
    return {previewSize, pendingDeviceScale};
}

void ThumbBrowserEntryBase::onDeviceScaleChanged(int newDeviceScale) {
    if (newDeviceScale != activeDeviceScale) {
        pendingDeviceScale = newDeviceScale;
        refreshThumbnailImage();
    }
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
                || getIconsOnImageArea() != bbIcons
                || getSpecificityIconsOnImageArea() != bbSpecificityIcons) {
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

    int x_offset = startx + ofsX;
    int y_offset = starty + ofsY;
    cc->set_source(backBuffer->getSurface(), x_offset, y_offset);
    cc->paint();

    // In filmstrip mode: draw rating/label overlays directly on the image
    bool inFilmstrip = parent && parent->getLocation() == ThumbBrowserBase::THLOC_EDITOR;
    if (inFilmstrip && thumbnail) {
        drawFilmstripOverlays(cc, x_offset, y_offset);
    }

    // redraw button set above the thumbnail (hidden if no rank/label, or in filmstrip/tab mode)
    if (buttonSet && buttonSet->shouldShow() && !(parent && parent->isInTabMode())) {
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

    if (buttonSet) {
        buttonSet->arrangeButtons (ofsX + x + sideMargin, ofsY + y + upperMargin, w - 2 * sideMargin, -1);
    }
}

void ThumbBrowserEntryBase::setOffset (int x, int y)
{
    MYWRITERLOCK(l, lockRW);

    ofsX = -x;
    ofsY = -y;

    if (buttonSet) {
        buttonSet->move (ofsX + startx + sideMargin, ofsY + starty + upperMargin);
    }
}

bool ThumbBrowserEntryBase::inside (int x, int y) const
{

    return x > ofsX + startx && x < ofsX + startx + expected.width && y > ofsY + starty && y < ofsY + starty + expected.height;
}

rtengine::Coord2D ThumbBrowserEntryBase::getPosInImgSpace (int x, int y) const
{
    rtengine::Coord2D coord(-1., -1.);

    if (!preview.empty()) {
        x -= ofsX + startx;
        y -= ofsY + starty;

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

    return !(ofsX + startx > x + w || ofsX + startx + expected.width < x || ofsY + starty > y + h || ofsY + starty + expected.height < y);
}

std::vector<std::shared_ptr<RTSurface>> ThumbBrowserEntryBase::getIconsOnImageArea()
{
    return std::vector<std::shared_ptr<RTSurface>>();
}

std::vector<std::shared_ptr<RTSurface>> ThumbBrowserEntryBase::getSpecificityIconsOnImageArea()
{
    return std::vector<std::shared_ptr<RTSurface>>();
}

bool ThumbBrowserEntryBase::motionNotify  (int x, int y)
{

    return (buttonSet && !(parent && parent->isInTabMode())) ? buttonSet->motionNotify (x, y) : false;
}

bool ThumbBrowserEntryBase::pressNotify   (int button, int type, int bstate, int x, int y)
{

    return (buttonSet && !(parent && parent->isInTabMode())) ? buttonSet->pressNotify (x, y) : false;
}

bool ThumbBrowserEntryBase::releaseNotify (int button, int type, int bstate, int x, int y)
{

    return (buttonSet && !(parent && parent->isInTabMode())) ? buttonSet->releaseNotify (x, y) : false;
}

std::tuple<Glib::ustring, bool> ThumbBrowserEntryBase::getToolTip (int x, int y) const
{
    Glib::ustring tooltip;

    if (buttonSet) {
        tooltip = buttonSet->getToolTip(x, y);
    }

    // Always show the filename in the tooltip since the filename in the thumbnail could be truncated.
    // If "Show Exif info" is disabled, also show Exif info in the tooltip.
    bool useMarkup = !tooltip.empty();
    if (inside(x, y) && tooltip.empty()) {
        tooltip = dispname;

        const auto& options = App::get().options();
        if (withFilename < WFNAME_FULL) {
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

Cairo::RefPtr<Cairo::ImageSurface> ThumbBrowserEntryBase::snapshotSurface () const
{
    if (!backBuffer || !backBuffer->surfaceCreated()) {
        return {};
    }
    auto src = backBuffer->getSurface();
    int w = src->get_width();
    int h = src->get_height();
    auto copy = Cairo::ImageSurface::create(Cairo::FORMAT_ARGB32, w, h);
    auto cr = Cairo::Context::create(copy);
    cr->set_source(src, 0, 0);
    cr->paint();
    return copy;
}
