/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2018 Jean-Christophe FRISCH <natureh.510@gmail.com>
 *  Copyright (c) 2022 Pierre CABRERA <pierre.cab@gmail.com>
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

#include "rtscalable.h"

#include <iostream>

#include "rtengine/rtapp.h"
#include "rtengine/settings.h"
#include "rtengine/svgrender.h"
#include "guiutils.h"

#include <cairomm/context.h>

// Default static parameter values
double RTScalable::s_dpi = 96.;
int RTScalable::s_scale = 1;
sigc::signal<void(double, int)> RTScalable::s_signal_changed;

namespace
{

Cairo::RefPtr<Cairo::ImageSurface> makeSmallArrowSurface(const Glib::ustring& iconName, int size, int scale)
{
    if (iconName != "arrow-up-small"
        && iconName != "arrow-down-small"
        && iconName != "arrow-left-small"
        && iconName != "arrow-right-small"
        && iconName != "pan-up-symbolic"
        && iconName != "pan-down-symbolic"
        && iconName != "pan-start-symbolic"
        && iconName != "pan-end-symbolic") {
        return Cairo::RefPtr<Cairo::ImageSurface>();
    }

    const int scaledSize = size * scale;
    auto surface = Cairo::ImageSurface::create(Cairo::FORMAT_ARGB32, scaledSize, scaledSize);
    auto context = Cairo::Context::create(surface);

    context->scale(scale, scale);
    context->set_source_rgba(0.82, 0.84, 0.88, 0.95);

    const double pad = size * 0.25;
    const double mid = size * 0.5;
    const double end = size - pad;

    if (iconName == "arrow-up-small" || iconName == "pan-up-symbolic") {
        context->move_to(mid, pad);
        context->line_to(end, end);
        context->line_to(pad, end);
    } else if (iconName == "arrow-down-small" || iconName == "pan-down-symbolic") {
        context->move_to(pad, pad);
        context->line_to(end, pad);
        context->line_to(mid, end);
    } else if (iconName == "arrow-left-small" || iconName == "pan-start-symbolic") {
        context->move_to(pad, mid);
        context->line_to(end, pad);
        context->line_to(end, end);
    } else {
        context->move_to(pad, pad);
        context->line_to(end, mid);
        context->line_to(pad, end);
    }

    context->close_path();
    context->fill();

    cairo_surface_set_device_scale(surface->cobj(),
        static_cast<double>(scale),
        static_cast<double>(scale));

    return surface;
}

}

int RTScalable::getScaleForWindow(const Gtk::Window* window)
{
    int scale = window->get_scale_factor();
    // Default minimum value of 1 as scale is used to scale surface
    return scale > 0 ? scale : 1;
}

int RTScalable::getScaleForWidget(const Gtk::Widget* widget)
{
    int scale = widget->get_scale_factor();
    // Default minimum value of 1 as scale is used to scale surface
    return scale > 0 ? scale : 1;
}

void RTScalable::getDPInScale(const Gtk::Window* window, double &newDPI, int &newScale)
{
    if (window) {
        const auto screen = window->get_screen();
        newDPI = screen->get_resolution(); // Get DPI retrieved from the OS

        // Get scale factor associated to the window
        newScale = getScaleForWindow(window);
    }
}

Cairo::RefPtr<Cairo::ImageSurface> RTScalable::loadSurfaceFromIcon(const Glib::ustring &iconName, const Gtk::IconSize iconSize)
{
    GThreadLock lock; // All icon theme access or image access on separate thread HAVE to be protected

    Cairo::RefPtr<Cairo::ImageSurface> surf; // Create Cairo::RefPtr<Cairo::ImageSurface> nullptr

    // Get icon theme
    const auto theme = Gtk::IconTheme::get_default();

    // Get pixel size from Gtk::IconSize
    int wSize, hSize;

    if (!Gtk::IconSize::lookup(iconSize, wSize, hSize)) { // Size in invalid
        wSize = hSize = 16; // Set to a default size of 16px (i.e. Gtk::ICON_SIZE_SMALL_TOOLBAR one)
    }

    // Get scale based on DPI and scale
    // Note: hSize not used because icon are considered squared
    const int size = RTScalable::scalePixelSize(wSize);

    if (auto surface = makeSmallArrowSurface(iconName, size, RTScalable::getScale())) {
        return surface;
    }

    // Looking for corresponding icon (if existing)
    const auto iconInfo = theme->lookup_icon(iconName, size);

    Glib::ustring iconPath;

    if (iconInfo) {
        iconPath = iconInfo.get_filename();
    }

    // Fallback: direct file lookup when icon theme lookup fails (e.g. macOS missing icon cache)
    if (iconPath.empty()) {
        const auto fallback = Glib::build_filename(
            App::get().argv0(), "icons", "rawtherapee", "scalable", "apps",
            iconName + ".svg");
        if (Glib::file_test(fallback, Glib::FILE_TEST_EXISTS)) {
            iconPath = fallback;
        } else {
            std::cerr << "Failed to load icon \"" << iconName << "\" for size " << size << "px" << std::endl;
            return surf;
        }
    }

    {
        const auto pos = iconPath.find_last_of('.');

        if (pos < iconPath.length() && iconPath.substr(pos + 1, iconPath.length()).lowercase() == "svg") {
            const auto pngCachePath = Glib::build_filename(
                App::get().argv0(), "icons-png-cache", std::to_string(size),
                iconName + ".png");

            if (Glib::file_test(pngCachePath, Glib::FILE_TEST_EXISTS)) {
                return RTScalable::loadSurfaceFromPNG(pngCachePath, true);
            }
        }
    }

    // Create surface from corresponding icon
    const auto pos = iconPath.find_last_of('.');

    if (pos < iconPath.length()) {
        const auto fext = iconPath.substr(pos + 1, iconPath.length()).lowercase();

        // Case where iconPath is a PNG file
        if (fext == "png") {
            // Create surface from PNG file
            surf = RTScalable::loadSurfaceFromPNG(iconPath, true);
        }

        // Case where iconPath is a SVG file
        if (fext == "svg") {
            // Create surface from SVG file
            surf = RTScalable::loadSurfaceFromSVG(iconPath, size, size, true);
        }
    }

    return surf;
}

Cairo::RefPtr<Cairo::ImageSurface> RTScalable::loadSurfaceFromPNG(const Glib::ustring &fname, const bool is_path)
{
    GThreadLock lock; // All icon theme access or image access on separate thread HAVE to be protected

    Cairo::RefPtr<Cairo::ImageSurface> surf; // Create Cairo::RefPtr<Cairo::ImageSurface> nullptr

    Glib::ustring path;

    if (is_path) {
        // Directly use fname as a path
        path = fname;
    } else {
        // Look for PNG file in "images" folder
        Glib::ustring imagesFolder = Glib::build_filename(App::get().argv0(), "images");
        path = Glib::build_filename(imagesFolder, fname);
    }

    // Create surface from PNG file if file exist
    if (Glib::file_test(path.c_str(), Glib::FILE_TEST_EXISTS)) {
        surf = Cairo::ImageSurface::create_from_png(path);
    } else {
        std::cerr << "Failed to load PNG file \"" << fname << "\"" << std::endl;
    }

    return surf;
}

namespace
{

bool s_iconInkDark = false;

int hexNibble(const char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

// Flip the lightness of near-grey hex colors in an SVG source so line art
// tuned for dark themes stays readable on light ones. Saturated colors
// (flags, stars, color dots) pass through untouched. Only colors in
// attribute/style position are considered ('#' preceded by a quote, colon
// or whitespace) — url(#id) references are left alone.
std::string recolorSvgForLightTheme(const std::string& svg)
{
    std::string out = svg;

    for (size_t i = 1; i + 3 < out.size(); ++i) {
        if (out[i] != '#') {
            continue;
        }

        const char prev = out[i - 1];
        if (prev != '"' && prev != '\'' && prev != ':' && prev != ' ') {
            continue;
        }

        size_t n = 0;
        while (n < 8 && i + 1 + n < out.size() && hexNibble(out[i + 1 + n]) >= 0) {
            ++n;
        }
        if (n != 3 && n != 6) {
            continue;
        }

        int r, g, b;
        if (n == 6) {
            r = hexNibble(out[i + 1]) * 16 + hexNibble(out[i + 2]);
            g = hexNibble(out[i + 3]) * 16 + hexNibble(out[i + 4]);
            b = hexNibble(out[i + 5]) * 16 + hexNibble(out[i + 6]);
        } else {
            r = hexNibble(out[i + 1]) * 17;
            g = hexNibble(out[i + 2]) * 17;
            b = hexNibble(out[i + 3]) * 17;
        }

        const int mx = std::max(r, std::max(g, b));
        const int mn = std::min(r, std::min(g, b));
        if (mx - mn > 40) {
            i += n;
            continue; // saturated — semantic color, keep
        }

        r = 255 - r;
        g = 255 - g;
        b = 255 - b;

        static const char* hexDigits = "0123456789abcdef";
        char buf[7] = {
            hexDigits[(r >> 4) & 15], hexDigits[r & 15],
            hexDigits[(g >> 4) & 15], hexDigits[g & 15],
            hexDigits[(b >> 4) & 15], hexDigits[b & 15], 0
        };
        if (n == 6) {
            out.replace(i + 1, 6, buf);
        } else {
            const char buf3[4] = { hexDigits[(r >> 4) & 15], hexDigits[(g >> 4) & 15], hexDigits[(b >> 4) & 15], 0 };
            out.replace(i + 1, 3, buf3);
        }
        i += n;
    }

    return out;
}

} // namespace

void RTScalable::setIconInkDark(bool dark)
{
    s_iconInkDark = dark;
}

bool RTScalable::getIconInkDark()
{
    return s_iconInkDark;
}

Cairo::RefPtr<Cairo::ImageSurface> RTScalable::loadSurfaceFromSVG(const Glib::ustring &fname, const int width, const int height, const bool is_path)
{
    GThreadLock lock; // All icon theme access or image access on separate thread HAVE to be protected

    Cairo::RefPtr<Cairo::ImageSurface> surf; // Create Cairo::RefPtr<Cairo::ImageSurface> nullptr

    Glib::ustring path;

    if (is_path) {
        // Directly use fname as a path
        path = fname;
    } else {
        // Look for SVG file in "images" folder
        Glib::ustring imagesFolder = Glib::build_filename(App::get().argv0(), "images");
        path = Glib::build_filename(imagesFolder, fname);
    }

    // Create surface from SVG file if file exist
    if (Glib::file_test(path.c_str(), Glib::FILE_TEST_EXISTS)) {
        const auto pos = path.find_last_of('.');
        const auto pngPath = pos < path.length()
            ? path.substr(0, pos) + ".png"
            : Glib::ustring();

        if (!pngPath.empty() && Glib::file_test(pngPath.c_str(), Glib::FILE_TEST_EXISTS)) {
            return RTScalable::loadSurfaceFromPNG(pngPath, true);
        }
        // Read content of SVG file
        std::string svgFile;
        try {
            svgFile = Glib::file_get_contents(path);
        }
        catch (Glib::FileError &err) {
            std::cerr << "Failed to load SVG file \"" << fname << "\": "
                << err.what() << "\n";
            return surf;
        }

        if (s_iconInkDark) {
            svgFile = recolorSvgForLightTheme(svgFile);
        }

        try {
            surf = rtengine::renderSvg(svgFile, width, height, RTScalable::getScale());
        } catch(const rtengine::SvgRenderException& e) {
            std::cerr << "Failed to load SVG file \"" << fname << "\":\n"
                << e.what() << "\n";
        }
    } else {
        std::cerr << "Failed to load SVG file \"" << fname << "\"" << std::endl;
    }

    return surf;
}

void RTScalable::init(const Gtk::Window* window)
{
    // Retrieve DPI and Scale paremeters from OS
    double dpi = s_dpi;
    int scale = s_scale;
    getDPInScale(window, dpi, scale);
    setDPInScale(dpi, scale);
}

void RTScalable::setDPInScale (const Gtk::Window* window)
{
    double dpi = s_dpi;
    int scale = s_scale;
    getDPInScale(window, dpi, scale);
    setDPInScale(dpi, scale);
}

void RTScalable::setDPInScale (const double newDPI, const int newScale)
{
    if (s_dpi != newDPI || s_scale != newScale) {
        s_dpi = newDPI;
        s_scale = newScale;
        s_signal_changed.emit(newDPI, newScale);
    }
}

double RTScalable::getDPI ()
{
    return s_dpi;
}

int RTScalable::getScale ()
{
    return s_scale;
}

double RTScalable::getGlobalScale()
{
    return (RTScalable::getDPI() / RTScalable::baseDPI);
}

int RTScalable::scalePixelSize(const int pixel_size)
{
    const double s = getGlobalScale();
    return static_cast<int>(pixel_size * s + 0.5); // Rounded scaled size
}

double RTScalable::scalePixelSize(const double pixel_size)
{
    const double s = getGlobalScale();
    return (pixel_size * s);
}

RtScopedConnection RTScalable::connectToChanged(sigc::slot<void(double, int)>&& slot)
{
    return RtScopedConnection(s_signal_changed.connect(std::move(slot)));
}
