/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2004-2010 Gabor Horvath <hgabor@rawtherapee.com>, Oliver Duis <www.oliverduis.de>
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

#include <cmath>
#include <unordered_map>

#include "rtengine/rt_math.h"

#include <gtkmm.h>
#include "rtwindow.h"
#include "cachemanager.h"
#include "preferences.h"
#include "iccprofilecreator.h"
#include "cursormanager.h"
#include "editwindow.h"
#include "rtimage.h"
#include "thumbnail.h"
#include "tools/whitebalance.h"
#include "rtengine/settings.h"
#include "batchqueuepanel.h"
#include "batchqueue.h"
#include "batchqueueentry.h"
#include "editorpanel.h"
#include "filepanel.h"
#include "filecatalog.h"
#include "filebrowser.h"
#include "thumbbrowserentrybase.h"
#include "indclippedpanel.h"
#include "previewmodepanel.h"
#include "presetlistpanel.h"
#include "profilepanel.h"
#include "tools/filmsimulation.h"
#include "mcp/mcpserver.h"
#include "mcp/mcpdialog.h"

Glib::RefPtr<Gtk::CssProvider> cssForced;
Glib::RefPtr<Gtk::CssProvider> cssRT;

#if defined(__APPLE__)
static gboolean
osx_should_quit_cb (GtkosxApplication *app, gpointer data)
{
    RTWindow * const rtWin = static_cast<RTWindow*>(data);
    return rtWin->on_delete_event (0);
}

static void
osx_will_quit_cb (GtkosxApplication *app, gpointer data)
{
    RTWindow *rtWin = static_cast<RTWindow*>(data);
    rtWin->on_delete_event (0);
    gtk_main_quit ();
}

bool RTWindow::osxFileOpenEvent (Glib::ustring path)
{

    CacheManager* cm = CacheManager::getInstance();
    Thumbnail* thm = cm->getEntry ( path );

    if (thm && fpanel) {
        std::vector<Thumbnail*> entries;
        entries.push_back (thm);
        fpanel->fileCatalog->openRequested (entries);
        return true;
    }

    return false;
}

static gboolean
osx_open_file_cb (GtkosxApplication *app, gchar *path_, gpointer data)
{
    RTWindow *rtWin = static_cast<RTWindow*>(data);

    if (!App::get().argv1().empty()) {
        // skip handling if we have a file argument or else we get double open of same file
        return false;
    }

    Glib::ustring path = Glib::ustring (path_);
    Glib::ustring suffix = path.length() > 4 ? path.substr (path.length() - 3) : "";
    suffix = suffix.lowercase();

    if (suffix == "pp3")  {
        path = path.substr (0, path.length() - 4);
    }

    return rtWin->osxFileOpenEvent (path);
}
#endif // __APPLE__

RTWindow::RTWindow ()
    : mainNB (nullptr)
    , bpanel (nullptr)
    , splash (nullptr)
    , btn_fullscreen (nullptr)
    , btn_minimize (nullptr)
    , btn_close (nullptr)
    , iFullscreen (nullptr)
    , iFullscreen_exit (nullptr)
    , headerBar (nullptr)
    , optionsBtn (nullptr)
    , navFileBrowser (nullptr)
    , navQueue (nullptr)
    , navEditor (nullptr)
    , navSwitching (false)
    , suppressEditorSwitchAutoOpen_ (false)
    , mainOverlay (nullptr)
    , queueOverlayBox (nullptr)
    , queueBackdrop (nullptr)
    , queueOverlayVisible (false)
    , queueAnimFraction (0.0)
    , heroOverlay_ (nullptr)
    , heroAnimFraction_ (0.0)
    , heroPageSwitched_ (false)
    , heroAnimIn_ (true)
    , heroFilmX_ (0), heroFilmY_ (0), heroFilmW_ (0), heroFilmH_ (0)
    , startupOverlay_ (nullptr)
    , startupAnimTime_ (0.0)
    , startupAnimActive_ (false)
    , epanel (nullptr)
    , fpanel (nullptr)
    , mcpServer_ (new mcp::McpServer())
{
    cacheMgr->init ();
    ProfilePanel::init (this);
    PresetListPanel::init (this);

    // ------- loading theme files

    Glib::RefPtr<Gdk::Screen> screen = Gdk::Screen::get_default();
    auto& options = App::get().mut_options();

    if (screen) {
        // Setting default theme and icon theme (bases for custom themes)
        Gtk::Settings::get_for_screen (screen)->property_gtk_theme_name() = "Adwaita";
        Gtk::Settings::get_for_screen (screen)->property_gtk_application_prefer_dark_theme() = true;
        Gtk::Settings::get_for_screen (screen)->property_gtk_icon_theme_name() = "rawtherapee";

        // Initialize RTScalable for Hi-DPI support
        RTScalable::init(this);

        // Look for theme and set it
        // Check if the current theme name in options exists, otherwise set it to default one (i.e. "RawTherapee.css")
        auto filename = Glib::build_filename(App::get().argv0(), "themes", options.theme + ".css");
        if (!Glib::file_test(filename, Glib::FILE_TEST_EXISTS)) {
            options.theme = "RawTherapee";
            filename = Glib::build_filename(App::get().argv0(), "themes", options.theme + ".css");
        }

        cssRT = Gtk::CssProvider::create();

        try {
            cssRT->load_from_path (filename);
            Gtk::StyleContext::add_provider_for_screen (screen, cssRT, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        } catch (Glib::Error &err) {
            printf ("Error: Can't load css file \"%s\"\nMessage: %s\n", filename.c_str(), err.what().c_str());
        } catch (...) {
            printf ("Error: Can't load css file \"%s\"\n", filename.c_str());
        }

        // Set the font face and size
        Glib::ustring css;

        if (options.fontFamily != "default") { // Set font and size according to user choice
            // Set font and size in css from options
            css = Glib::ustring::compose (
                "* { font-family: %1; font-size: %2pt }",
                options.fontFamily,
                options.fontSize); // Font size is in "pt" in options
        } else { // Set font and size according to default values
            // Retrieve default style values from Gtk::Settings
            const auto defaultSettings = Gtk::Settings::get_default();
            Glib::ustring defaultFont;
            defaultSettings->get_property("gtk-font-name", defaultFont);
            const Pango::FontDescription defaultFontDesc = Pango::FontDescription(defaultFont);

            // Set font and size in css
            auto defaultFontFamily = defaultFontDesc.get_family();
            const int defaultFontSize = defaultFontDesc.get_size() / Pango::SCALE; // Font size is managed in ()"pt" * Pango::SCALE) by Pango (also refer to notes in rtscalable.h)
#if defined(__APPLE__)
            // Default MacOS font (i.e. "") is not correctly handled
            // in Gtk css. Replacing it by "-apple-system" to avoid this
            if (defaultFontFamily == ".AppleSystemUIFont") {
                defaultFontFamily = "-apple-system";
            }
#endif
            css = Glib::ustring::compose (
                "* { font-family: %1; font-size: %2pt }",
                defaultFontFamily,
                defaultFontSize);
        }

        // Load custom CSS for font
        if (!css.empty()) {
            if (rtengine::settings->verbose) {
                printf("CSS:\n%s\n\n", css.c_str());
            }

            try {
                cssForced = Gtk::CssProvider::create();
                cssForced->load_from_data (css);

                Gtk::StyleContext::add_provider_for_screen (screen, cssForced, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
            } catch (Glib::Error &err) {
                printf ("Error: \"%s\"\n", err.what().c_str());
            } catch (...) {
                printf ("Error: Can't load the desired font correctly\n");
            }
        }

    }

    // ------- end loading theme files

    // Initialize FileBrowserEntry icons
    FileBrowserEntry::init();

    // For UNIX system, set app icon
#ifndef _WIN32
    try {
        set_default_icon_name("rawtherapee");
    } catch (Glib::Exception& ex) {
        printf ("%s\n", ex.what().c_str());
    }
#endif

#if defined(__APPLE__)
    {
        osxApp  = (GtkosxApplication *)g_object_new (GTKOSX_TYPE_APPLICATION, NULL);
        RTWindow *rtWin = this;
        g_signal_connect (osxApp, "NSApplicationBlockTermination", G_CALLBACK (osx_should_quit_cb), rtWin);
        g_signal_connect (osxApp, "NSApplicationWillTerminate",  G_CALLBACK (osx_will_quit_cb), rtWin);
        g_signal_connect (osxApp, "NSApplicationOpenFile", G_CALLBACK (osx_open_file_cb), rtWin);
        // RT don't have a menu, but we must create a dummy one to get the default OS X app menu working
        GtkWidget *menubar;
        menubar = gtk_menu_bar_new ();
        gtkosx_application_set_menu_bar (osxApp, GTK_MENU_SHELL (menubar));
        gtkosx_application_set_use_quartz_accelerators (osxApp, FALSE);
        gtkosx_application_ready (osxApp);
    }
#endif
    versionStr = "Steep " + App::VERSION;

    set_title_decorated ("");
    set_resizable (true);
    set_decorated (true);
    set_default_size (options.windowWidth, options.windowHeight);
    set_modal (false);

    on_delete_has_run = false;
    is_fullscreen = false;
    is_minimized = false;
    property_destroy_with_parent().set_value (false);
    signal_window_state_event().connect ( sigc::mem_fun (*this, &RTWindow::on_window_state_event) );
    onConfEventConn = signal_configure_event().connect ( sigc::mem_fun (*this, &RTWindow::on_configure_event) );
    signal_key_press_event().connect ( sigc::mem_fun (*this, &RTWindow::keyPressed) );
    signal_key_release_event().connect(sigc::mem_fun(*this, &RTWindow::keyReleased));

    if (App::get().isSimpleEditor()) {
        epanel = Gtk::manage ( new EditorPanel (nullptr) );
        epanel->setParent (this);
        epanel->setParentWindow (this);
        add (*epanel);
        show_all ();

        pldBridge = nullptr; // No progress listener

        CacheManager* cm = CacheManager::getInstance();
        Thumbnail* thm = cm->getEntry ( App::get().argv1() );

        if (thm) {
            int error;
            rtengine::InitialImage *ii = rtengine::InitialImage::load (App::get().argv1(), thm->getType() == FT_Raw, &error, nullptr);
            epanel->open ( thm, ii );
        }
    } else {
        mainNB = Gtk::manage (new Gtk::Notebook ());
        mainNB->set_name ("MainNotebook");
        mainNB->set_scrollable (true);
        mainNB->set_show_tabs (false); // Tabs controlled by header bar buttons
        mainNB->signal_switch_page().connect_notify ( sigc::mem_fun (*this, &RTWindow::on_mainNB_switch_page) );

        // File Browser panel (tab 0)
        fpanel = new FilePanel ();
        fpanel->setParent (this);
        mainNB->append_page (*fpanel);

        // Batch Queue panel (overlay drawer, not a notebook tab)
        bpanel = Gtk::manage ( new BatchQueuePanel (fpanel->fileCatalog) );

        if (isSingleTabMode()) {
            createSetmEditor();
        }
        mainNB->set_current_page (mainNB->page_num (*fpanel));

        // ===== Queue Overlay Drawer =====
        // Two overlay children: backdrop (full-size dark) + drawer (top 70%, opaque).
        // Both sized exactly via signal_get_child_position — no natural-height issues.
        mainOverlay = Gtk::manage (new Gtk::Overlay ());
        mainOverlay->add (*mainNB); // notebook is the base child

        // 1) Backdrop: full-size EventBox, click to dismiss.
        // No visible window — we paint the dark overlay via signal_draw to avoid
        // GdkWindow z-order issues that would darken the drawer too.
        queueBackdrop = Gtk::manage (new Gtk::EventBox ());
        queueBackdrop->set_name ("QueueOverlayBackdrop");
        queueBackdrop->set_visible_window (false);
        queueBackdrop->signal_button_press_event().connect ([this](GdkEventButton*) -> bool {
            hideQueueOverlay();
            return true;
        });
        queueBackdrop->signal_draw ().connect (
            [this](const Cairo::RefPtr<Cairo::Context>& cr) -> bool {
                int w = queueBackdrop->get_allocated_width ();
                int h = queueBackdrop->get_allocated_height ();
                // Instant darken — full opacity whenever backdrop is visible
                cr->set_source_rgba (0, 0, 0, 0.55);
                cr->rectangle (0, 0, w, h);
                cr->fill ();
                return false;
            }, false);

        // 2) Drawer: Box with slide-down animation via Cairo translate.
        // Content always keeps its full layout. During animation, the entire
        // content is translated upward and clipped, creating a smooth slide-in.
        queueOverlayBox = Gtk::manage (new Gtk::Box (Gtk::ORIENTATION_VERTICAL));
        queueOverlayBox->set_name ("QueueDrawerBox");
        queueOverlayBox->signal_draw ().connect (
            [this](const Cairo::RefPtr<Cairo::Context>& cr) -> bool {
                int w = queueOverlayBox->get_allocated_width ();
                int h = queueOverlayBox->get_allocated_height ();
                // Ease-out quart — fast start, smooth deceleration
                double t = queueAnimFraction;
                double eased = 1.0 - std::pow (1.0 - t, 4);
                int visibleH = std::max (1, static_cast<int>(h * eased));
                // Clip to the visible portion (top visibleH pixels)
                cr->rectangle (0, 0, w, visibleH);
                cr->clip ();
                // Translate content upward so it slides into view from above
                cr->translate (0, -(h - visibleH));
                // Paint opaque background using the drawer's CSS background-color
                queueOverlayBox->get_style_context ()->render_background (
                    cr, 0, 0, w, h);
                return false; // default handler draws children (also translated)
            }, false);
        // Hide the bottom zoom slider bar and horizontal scrollbar — not needed in overlay mode
        for (auto* child : bpanel->get_children ()) {
            if (child->get_name () == "BatchQueueBottomBox") {
                child->set_no_show_all (true);
                child->hide ();
                break;
            }
        }
        bpanel->setOverlayMode (true);
        queueOverlayBox->pack_start (*bpanel, true, true);

        // Add both as overlay children (backdrop first, drawer on top)
        mainOverlay->add_overlay (*queueBackdrop);
        mainOverlay->add_overlay (*queueOverlayBox);

        // Size both overlay children via signal_get_child_position
        mainOverlay->signal_get_child_position().connect (
            [this](Gtk::Widget* child, Gdk::Rectangle& alloc) -> bool {
                int pw = mainOverlay->get_allocated_width();
                int ph = mainOverlay->get_allocated_height();
                if (child == queueBackdrop) {
                    // Backdrop fills the entire area
                    alloc.set_x (0);
                    alloc.set_y (0);
                    alloc.set_width (pw);
                    alloc.set_height (ph);
                    return true;
                }
                if (child == queueOverlayBox) {
                    // Drawer: top 50% — always full height; animation is via clip in signal_draw
                    int targetH = static_cast<int>(ph * 0.50);
                    alloc.set_x (0);
                    alloc.set_y (0);
                    alloc.set_width (pw);
                    alloc.set_height (targetH);
                    return true;
                }
                if (child == heroOverlay_) {
                    alloc.set_x (0);
                    alloc.set_y (0);
                    alloc.set_width (pw);
                    alloc.set_height (ph);
                    return true;
                }
                if (child == startupOverlay_) {
                    alloc.set_x (0);
                    alloc.set_y (0);
                    alloc.set_width (pw);
                    alloc.set_height (ph);
                    return true;
                }
                return false;
            }, false);

        // ===== Hero Transition Overlay (thumbnail animation browser→filmstrip) =====
        heroOverlay_ = Gtk::manage (new Gtk::EventBox ());
        heroOverlay_->set_name ("HeroTransitionOverlay");
        heroOverlay_->set_visible_window (false);
        heroOverlay_->signal_draw ().connect (
            [this](const Cairo::RefPtr<Cairo::Context>& cr) -> bool {
                if (capturedThumbs_.empty()) return false;

                double t = heroAnimFraction_;

                // Motion starts after page switch; remap remaining time to 0→1
                double switchPt = heroAnimIn_ ? 0.35 : 0.25;
                double motionT = heroPageSwitched_
                    ? std::min(1.0, (t - switchPt) / (0.90 - switchPt))
                    : 0.0;
                // Ease-out-cubic for smooth movement
                double motionEased = 1.0 - std::pow(1.0 - motionT, 3);

                // Filmstrip clip boundaries (set by computeFilmstripTargets)
                double filmR = heroFilmX_ + heroFilmW_;

                for (auto& ct : capturedThumbs_) {
                    if (!ct.surface) continue;

                    double curX, curY, curW, curH, alpha;

                    if (ct.isHero && heroPageSwitched_) {
                        // Interpolate from src to dst position
                        curX = ct.srcX + (ct.dstX - ct.srcX) * motionEased;
                        curY = ct.srcY + (ct.dstY - ct.srcY) * motionEased;
                        curW = ct.srcW + (ct.dstW - ct.srcW) * motionEased;
                        curH = ct.srcH + (ct.dstH - ct.srcH) * motionEased;

                        // Fade out near end (crossfade with real filmstrip underneath)
                        alpha = motionT < 0.65 ? 1.0 : std::max(0.0, 1.0 - (motionT - 0.65) / 0.35);

                        // Fade out heroes whose destination is past the filmstrip right edge
                        // (would be hidden behind the sidebar)
                        if (ct.dstX + ct.dstW > filmR) {
                            double fadeT = std::min(1.0, t / 0.35);
                            alpha = std::max(0.0, 1.0 - fadeT * fadeT);
                        }
                    } else {
                        // Non-hero: stay in place, fade out quickly
                        curX = ct.srcX;
                        curY = ct.srcY;
                        curW = ct.srcW;
                        curH = ct.srcH;
                        double fadeT = std::min(1.0, t / 0.35);
                        alpha = 1.0 - fadeT * fadeT;
                    }

                    if (alpha <= 0.001) continue;

                    cr->save();
                    cr->translate(curX, curY);
                    double scaleX = curW / ct.surface->get_width();
                    double scaleY = curH / ct.surface->get_height();
                    cr->scale(scaleX, scaleY);
                    cr->set_source(ct.surface, 0, 0);
                    cr->paint_with_alpha(alpha);
                    cr->restore();
                }
                return true;
            }, false);
        mainOverlay->add_overlay (*heroOverlay_);

        // ===== Startup Animation Overlay =====
        startupOverlay_ = Gtk::manage (new Gtk::EventBox ());
        startupOverlay_->set_name ("StartupOverlay");
        startupOverlay_->set_visible_window (false);

        // Pre-render the steep logo at large size using the icon theme
        {
            auto iconTheme = Gtk::IconTheme::get_default();
            try {
                auto pixbuf = iconTheme->load_icon("steep-logo", 96, Gtk::ICON_LOOKUP_FORCE_SVG);
                if (pixbuf) {
                    logoSurface_ = Cairo::ImageSurface::create(
                        Cairo::FORMAT_ARGB32, pixbuf->get_width(), pixbuf->get_height());
                    auto cr = Cairo::Context::create(logoSurface_);
                    Gdk::Cairo::set_source_pixbuf(cr, pixbuf, 0, 0);
                    cr->paint();
                }
            } catch (...) {
                // Logo not found — animation will still work without it
            }
        }

        startupAnimActive_ = true; // active immediately so first draw shows the overlay

        startupOverlay_->signal_draw ().connect (
            [this](const Cairo::RefPtr<Cairo::Context>& cr) -> bool {
                if (!startupAnimActive_) return false;

                int w = startupOverlay_->get_allocated_width ();
                int h = startupOverlay_->get_allocated_height ();
                double t = startupAnimTime_;

                // Phase timings (seconds)
                const double fadeInEnd = 0.3;     // logo fade-in
                const double holdEnd = 1.8;       // hold with animated topo
                const double fadeOutEnd = 3.0;    // everything fades out

                // Overall opacity: fade in quickly, hold, then fade out
                double opacity = 1.0;
                if (t < fadeInEnd) {
                    // Ease-out for smooth fade-in
                    double p = t / fadeInEnd;
                    opacity = 1.0 - (1.0 - p) * (1.0 - p);
                } else if (t > holdEnd) {
                    double p = std::min(1.0, (t - holdEnd) / (fadeOutEnd - holdEnd));
                    // Ease-in for fade-out
                    opacity = 1.0 - p * p;
                }
                opacity = std::max(0.0, std::min(1.0, opacity));

                if (opacity <= 0.001) return false;

                // Draw fully opaque dark background
                cr->set_source_rgba (0.08, 0.09, 0.14, opacity);
                cr->rectangle (0, 0, w, h);
                cr->fill ();

                double cx = w * 0.5;
                double cy = h * 0.5;
                double animPhase = t * 0.6;  // slow drift

                // === Digital Topography Contour Lines ===
                // Flowing contour lines across the entire surface
                const int numLines = 22;
                double lineSpacing = h / (double)(numLines + 1);

                for (int i = 0; i < numLines; i++) {
                    double baseY = lineSpacing * (i + 1);

                    // Color varies per line — blues/teals/cyans
                    double seed = i * 0.47 + 1.2;
                    double r = 0.18 + 0.08 * std::sin(seed);
                    double g = 0.32 + 0.12 * std::cos(seed + 0.8);
                    double b = 0.52 + 0.10 * std::sin(seed + 2.0);

                    // Lines brightest near center, dimmer at edges
                    double distFromCenter = std::abs(baseY - cy) / cy;
                    double lineAlpha = opacity * (0.25 + 0.35 * (1.0 - distFromCenter));

                    cr->set_source_rgba (r, g, b, lineAlpha);
                    cr->set_line_width (1.2 + 0.4 * std::sin(i * 1.3));

                    cr->move_to (0, baseY);
                    for (int x = 0; x <= w; x += 3) {
                        double fx = x / (double)w;
                        // Multi-frequency sine waves for organic topographic contours
                        double dy = 0.0;
                        dy += 18.0 * std::sin(fx * 3.5 * M_PI + animPhase * 1.0 + i * 0.6);
                        dy += 10.0 * std::sin(fx * 6.0 * M_PI + animPhase * 1.4 + i * 0.35);
                        dy +=  6.0 * std::sin(fx * 11.0 * M_PI + animPhase * 0.8 + i * 0.9);
                        dy +=  3.0 * std::cos(fx * 17.0 * M_PI + animPhase * 1.7 + i * 1.2);
                        // Pinch amplitude toward horizontal center for a "peak" effect
                        double pinch = 1.0 + 0.5 * std::exp(-8.0 * (fx - 0.5) * (fx - 0.5));
                        cr->line_to (x, baseY + dy * pinch);
                    }
                    cr->stroke ();
                }

                // === Elevation contour rings — irregular topographic rings ===
                for (int ring = 0; ring < 8; ring++) {
                    double baseR = 50.0 + ring * 35.0 + 8.0 * std::sin(animPhase * 0.9 + ring * 0.7);
                    double ringAlpha = opacity * (0.12 + 0.08 * (1.0 - ring / 8.0));
                    double rb = 0.30 + 0.05 * ring;
                    double gb = 0.50 + 0.03 * ring;
                    double bb = 0.65 + 0.02 * ring;
                    cr->set_source_rgba (rb, gb, bb, ringAlpha);
                    cr->set_line_width (0.9 + 0.2 * (ring % 3 == 0 ? 1.0 : 0.0));

                    bool first = true;
                    for (int deg = 0; deg <= 360; deg += 2) {
                        double rad = deg * M_PI / 180.0;
                        double wobble = 1.0
                            + 0.08 * std::sin(rad * 3 + animPhase + ring * 0.9)
                            + 0.05 * std::cos(rad * 5 + animPhase * 1.3 + ring * 0.6)
                            + 0.03 * std::sin(rad * 8 + animPhase * 0.7 + ring * 1.4);
                        double px = cx + baseR * wobble * std::cos(rad);
                        double py = cy + baseR * wobble * std::sin(rad);
                        if (first) { cr->move_to(px, py); first = false; }
                        else cr->line_to(px, py);
                    }
                    cr->close_path ();
                    cr->stroke ();
                }

                // === Subtle grid-dot pattern (elevation markers) ===
                {
                    double dotAlpha = opacity * 0.06;
                    cr->set_source_rgba(0.4, 0.6, 0.8, dotAlpha);
                    double spacing = 30.0;
                    for (double gx = spacing; gx < w; gx += spacing) {
                        for (double gy = spacing; gy < h; gy += spacing) {
                            cr->arc(gx, gy, 0.6, 0, 2 * M_PI);
                            cr->fill();
                        }
                    }
                }

                // === Central glow behind logo ===
                {
                    double glowR = std::min(w, h) * 0.22;
                    auto gradient = Cairo::RadialGradient::create(
                        cx, cy - 10, 0, cx, cy - 10, glowR);
                    gradient->add_color_stop_rgba(0.0, 0.25, 0.45, 0.65, 0.25 * opacity);
                    gradient->add_color_stop_rgba(0.5, 0.15, 0.30, 0.50, 0.10 * opacity);
                    gradient->add_color_stop_rgba(1.0, 0.08, 0.09, 0.14, 0.0);
                    cr->set_source(gradient);
                    cr->rectangle(0, 0, w, h);
                    cr->fill();
                }

                // === Steep Logo (centered, large) ===
                if (logoSurface_) {
                    double logoW = logoSurface_->get_width();
                    double logoH = logoSurface_->get_height();
                    double targetSize = std::min(w, h) * 0.16;
                    targetSize = std::max(120.0, std::min(220.0, targetSize));
                    double scale = targetSize / std::max(logoW, logoH);
                    double logoX = (w - logoW * scale) * 0.5;
                    double logoY = (h - logoH * scale) * 0.5 - 24;

                    cr->save();
                    cr->translate(logoX, logoY);
                    cr->scale(scale, scale);
                    cr->set_source(logoSurface_, 0, 0);
                    cr->paint_with_alpha(opacity);
                    cr->restore();
                }

                // === "STEEP" text below logo ===
                {
                    auto layout = Pango::Layout::create(cr);
                    auto fontDesc = Pango::FontDescription();
                    fontDesc.set_family("sans-serif");
                    fontDesc.set_weight(Pango::WEIGHT_LIGHT);
                    fontDesc.set_size(20 * PANGO_SCALE);
                    layout->set_font_description(fontDesc);
                    layout->set_text("S T E E P");
                    layout->set_alignment(Pango::ALIGN_CENTER);

                    int textW, textH;
                    layout->get_pixel_size(textW, textH);
                    double textX = (w - textW) * 0.5;
                    double textY = cy + 60;

                    cr->move_to(textX, textY);
                    cr->set_source_rgba(0.80, 0.83, 0.88, opacity * 0.9);
                    layout->show_in_cairo_context(cr);
                }

                return true;
            }, false);

        mainOverlay->add_overlay (*startupOverlay_);

        // Prevent window's show_all() from showing these; we manage visibility manually
        queueBackdrop->set_no_show_all (true);
        queueOverlayBox->set_no_show_all (true);
        heroOverlay_->set_no_show_all (true);

        // ===== Header Bar (replaces left sidebar + action grid) =====
        headerBar = Gtk::manage (new Gtk::HeaderBar ());
        headerBar->set_name ("RTHeaderBar");
        headerBar->set_show_close_button (false);
        headerBar->set_has_subtitle (true);

        // -- Options dropdown menu (left side, before nav buttons) --
        optionsBtn = Gtk::manage (new Gtk::MenuButton ());
        optionsBtn->set_name ("OptionsMenuButton");
        optionsBtn->set_relief (Gtk::RELIEF_NONE);
        optionsBtn->set_image (*Gtk::manage (new RTImage ("steep-logo", Gtk::ICON_SIZE_LARGE_TOOLBAR)));
        optionsBtn->set_tooltip_markup (M ("MAIN_BUTTON_PREFERENCES"));

        Gtk::Popover* optionsPopover = Gtk::manage (new Gtk::Popover ());
        optionsPopover->set_name ("OptionsPopover");
        Gtk::Box* optionsBox = Gtk::manage (new Gtk::Box (Gtk::ORIENTATION_VERTICAL, 2));
        optionsBox->set_margin_top (8);
        optionsBox->set_margin_bottom (8);
        optionsBox->set_margin_start (10);
        optionsBox->set_margin_end (10);

        Gtk::Button* menuHelpBtn = Gtk::manage (new Gtk::Button ());
        {
            Gtk::Box* hbox = Gtk::manage (new Gtk::Box (Gtk::ORIENTATION_HORIZONTAL, 8));
            hbox->pack_start (*Gtk::manage (new RTImage ("questionmark", Gtk::ICON_SIZE_LARGE_TOOLBAR)), false, false);
            hbox->pack_start (*Gtk::manage (new Gtk::Label (M ("GENERAL_HELP"))), false, false);
            menuHelpBtn->add (*hbox);
        }
        menuHelpBtn->set_relief (Gtk::RELIEF_NONE);
        menuHelpBtn->signal_clicked().connect ([this, optionsPopover]() {
            optionsPopover->popdown();
            showRawPedia();
        });

        Gtk::Button* menuIccBtn = Gtk::manage (new Gtk::Button ());
        {
            Gtk::Box* hbox = Gtk::manage (new Gtk::Box (Gtk::ORIENTATION_HORIZONTAL, 8));
            hbox->pack_start (*Gtk::manage (new RTImage ("gamut-plus", Gtk::ICON_SIZE_LARGE_TOOLBAR)), false, false);
            hbox->pack_start (*Gtk::manage (new Gtk::Label (M ("MAIN_BUTTON_ICCPROFCREATOR"))), false, false);
            menuIccBtn->add (*hbox);
        }
        menuIccBtn->set_relief (Gtk::RELIEF_NONE);
        menuIccBtn->signal_clicked().connect ([this, optionsPopover]() {
            optionsPopover->popdown();
            showICCProfileCreator();
        });

        Gtk::Button* menuPrefsBtn = Gtk::manage (new Gtk::Button ());
        {
            Gtk::Box* hbox = Gtk::manage (new Gtk::Box (Gtk::ORIENTATION_HORIZONTAL, 8));
            hbox->pack_start (*Gtk::manage (new RTImage ("preferences", Gtk::ICON_SIZE_LARGE_TOOLBAR)), false, false);
            hbox->pack_start (*Gtk::manage (new Gtk::Label (M ("MAIN_BUTTON_PREFERENCES"))), false, false);
            menuPrefsBtn->add (*hbox);
        }
        menuPrefsBtn->set_relief (Gtk::RELIEF_NONE);
        menuPrefsBtn->signal_clicked().connect ([this, optionsPopover]() {
            optionsPopover->popdown();
            showPreferences();
        });

        Gtk::Separator* menuSep1 = Gtk::manage (new Gtk::Separator (Gtk::ORIENTATION_HORIZONTAL));

        Gtk::Button* menuNavBtn = Gtk::manage (new Gtk::Button ());
        {
            Gtk::Box* hbox = Gtk::manage (new Gtk::Box (Gtk::ORIENTATION_HORIZONTAL, 8));
            hbox->pack_start (*Gtk::manage (new RTImage ("color-picker", Gtk::ICON_SIZE_LARGE_TOOLBAR)), false, false);
            hbox->pack_start (*Gtk::manage (new Gtk::Label (M ("MAIN_MENU_NAVIGATOR"))), false, false);
            menuNavBtn->add (*hbox);
        }
        menuNavBtn->set_relief (Gtk::RELIEF_NONE);
        menuNavBtn->signal_clicked().connect ([this, optionsPopover]() {
            optionsPopover->popdown();
            EditorPanel* ep = getActiveEditorPanel();
            if (ep) {
                ep->showNavigatorDialog();
            }
        });

        Gtk::Button* menuHistBtn = Gtk::manage (new Gtk::Button ());
        {
            Gtk::Box* hbox = Gtk::manage (new Gtk::Box (Gtk::ORIENTATION_HORIZONTAL, 8));
            hbox->pack_start (*Gtk::manage (new RTImage ("undo", Gtk::ICON_SIZE_LARGE_TOOLBAR)), false, false);
            hbox->pack_start (*Gtk::manage (new Gtk::Label (M ("MAIN_MENU_HISTORY"))), false, false);
            menuHistBtn->add (*hbox);
        }
        menuHistBtn->set_relief (Gtk::RELIEF_NONE);
        menuHistBtn->signal_clicked().connect ([this, optionsPopover]() {
            optionsPopover->popdown();
            EditorPanel* ep = getActiveEditorPanel();
            if (ep) {
                ep->showHistoryDialog();
            }
        });

        Gtk::Separator* menuSep2 = Gtk::manage (new Gtk::Separator (Gtk::ORIENTATION_HORIZONTAL));

        Gtk::Box* bgColorRow = Gtk::manage (new Gtk::Box (Gtk::ORIENTATION_HORIZONTAL, 8));
        bgColorRow->set_margin_start (4);
        bgColorRow->set_margin_end (4);
        Gtk::Label* bgColorLabel = Gtk::manage (new Gtk::Label (M ("MAIN_MENU_PREVIEW_BG")));
        bgColorCombo = Gtk::manage (new Gtk::ComboBoxText ());
        bgColorCombo->append (M ("MAIN_MENU_PREVIEW_BG_THEME"));
        bgColorCombo->append (M ("MAIN_MENU_PREVIEW_BG_BLACK"));
        bgColorCombo->append (M ("MAIN_MENU_PREVIEW_BG_GREY"));
        bgColorCombo->append (M ("MAIN_MENU_PREVIEW_BG_WHITE"));
        bgColorCombo->set_active (0);
        bgColorRow->pack_start (*bgColorLabel, false, false);
        bgColorRow->pack_end (*bgColorCombo, false, false);

        bgColorCombo->signal_changed().connect ([this]() {
            EditorPanel* ep = getActiveEditorPanel();
            if (!ep) return;
            PreviewModePanel* pmp = ep->getPreviewModePanel();
            if (!pmp) return;
            // Combo order: Theme(0), Black(1), Grey(2), White(3)
            // Internal order: Theme=0, Black=1, White=2, Grey=3
            const int comboToInternal[] = {0, 1, 3, 2};
            int idx = bgColorCombo->get_active_row_number();
            if (idx >= 0 && idx < 4) {
                pmp->setBackColor(comboToInternal[idx]);
            }
        });

        Gtk::Separator* menuSep3 = Gtk::manage (new Gtk::Separator (Gtk::ORIENTATION_HORIZONTAL));

        chkFocusMask = Gtk::manage (new Gtk::CheckButton (M ("MAIN_MENU_FOCUSMASK")));
        chkFocusMask->signal_toggled().connect ([this]() {
            EditorPanel* ep = getActiveEditorPanel();
            if (!ep) return;
            IndicateClippedPanel* icp = ep->getIndicateClippedPanel();
            if (icp) icp->setFocusMask(chkFocusMask->get_active());
        });

        chkSharpMask = Gtk::manage (new Gtk::CheckButton (M ("MAIN_MENU_SHARPMASK")));
        chkSharpMask->signal_toggled().connect ([this]() {
            EditorPanel* ep = getActiveEditorPanel();
            if (!ep) return;
            IndicateClippedPanel* icp = ep->getIndicateClippedPanel();
            if (icp) icp->setSharpMask(chkSharpMask->get_active());
        });

        chkClippedShadows = Gtk::manage (new Gtk::CheckButton (M ("MAIN_MENU_CLIPPED_SHADOWS")));
        chkClippedShadows->signal_toggled().connect ([this]() {
            EditorPanel* ep = getActiveEditorPanel();
            if (!ep) return;
            IndicateClippedPanel* icp = ep->getIndicateClippedPanel();
            if (icp) icp->setClippedShadows(chkClippedShadows->get_active());
        });

        chkClippedHighlights = Gtk::manage (new Gtk::CheckButton (M ("MAIN_MENU_CLIPPED_HIGHLIGHTS")));
        chkClippedHighlights->signal_toggled().connect ([this]() {
            EditorPanel* ep = getActiveEditorPanel();
            if (!ep) return;
            IndicateClippedPanel* icp = ep->getIndicateClippedPanel();
            if (icp) icp->setClippedHighlights(chkClippedHighlights->get_active());
        });

        chkHistogramProfile = Gtk::manage (new Gtk::CheckButton (M ("MAIN_MENU_HISTOGRAM_PROFILE")));
        chkHistogramProfile->signal_toggled().connect ([this]() {
            EditorPanel* ep = getActiveEditorPanel();
            if (!ep) return;
            Gtk::ToggleButton* tb = ep->getToggleHistogramProfile();
            if (tb && tb->get_active() != chkHistogramProfile->get_active()) {
                tb->set_active(chkHistogramProfile->get_active());
            }
        });

        optionsPopover->signal_show().connect ([this]() {
            EditorPanel* ep = getActiveEditorPanel();
            if (!ep) {
                return;
            }
            PreviewModePanel* pmp = ep->getPreviewModePanel();
            if (pmp) {
                // Internal order: Theme=0, Black=1, White=2, Grey=3
                // Combo order: Theme(0), Black(1), Grey(2), White(3)
                const int internalToCombo[] = {0, 1, 3, 2};
                int bc = pmp->GetbackColor();
                if (bc >= 0 && bc < 4) {
                    bgColorCombo->set_active(internalToCombo[bc]);
                }
            }
            IndicateClippedPanel* icp = ep->getIndicateClippedPanel();
            if (icp) {
                chkFocusMask->set_active(icp->showFocusMask());
                chkSharpMask->set_active(icp->showSharpMask());
                chkClippedShadows->set_active(icp->showClippedShadows());
                chkClippedHighlights->set_active(icp->showClippedHighlights());
            }
            Gtk::ToggleButton* tb = ep->getToggleHistogramProfile();
            if (tb) {
                chkHistogramProfile->set_active(tb->get_active());
            }

            // Sync preview channel checkboxes
            if (pmp) {
                chkPreviewR->set_active(pmp->showR());
                chkPreviewG->set_active(pmp->showG());
                chkPreviewB->set_active(pmp->showB());
                chkPreviewL->set_active(pmp->showL());
            }

            // Sync color management controls
            intentCombo->set_active(ep->getRenderingIntent());

#if !defined(__APPLE__)
            // Repopulate profile combo
            profileCombo->remove_all();
            int profileCount = ep->getMonitorProfileCount();
            for (int i = 0; i < profileCount; i++) {
                profileCombo->append(ep->getMonitorProfileName(i));
            }
            profileCombo->set_active(ep->getMonitorProfileIndex());
#endif

            chkSoftProof->set_active(ep->getSoftProofing());
            chkGamutCheck->set_active(ep->getGamutCheck());
        });

        // -- Preview Channel section --
        Gtk::Separator* menuSep4 = Gtk::manage (new Gtk::Separator (Gtk::ORIENTATION_HORIZONTAL));

        Gtk::Label* previewChannelLabel = Gtk::manage (new Gtk::Label ());
        previewChannelLabel->set_markup ("<b>" + M ("MAIN_MENU_PREVIEW_CHANNEL") + "</b>");
        previewChannelLabel->set_halign (Gtk::ALIGN_START);
        previewChannelLabel->set_margin_start (4);

        chkPreviewR = Gtk::manage (new Gtk::CheckButton (M ("MAIN_MENU_PREVIEW_RED")));
        chkPreviewG = Gtk::manage (new Gtk::CheckButton (M ("MAIN_MENU_PREVIEW_GREEN")));
        chkPreviewB = Gtk::manage (new Gtk::CheckButton (M ("MAIN_MENU_PREVIEW_BLUE")));
        chkPreviewL = Gtk::manage (new Gtk::CheckButton (M ("MAIN_MENU_PREVIEW_LUMINOSITY")));

        chkPreviewR->signal_toggled().connect ([this]() {
            EditorPanel* ep = getActiveEditorPanel();
            if (!ep) return;
            PreviewModePanel* pmp = ep->getPreviewModePanel();
            if (!pmp) return;
            if (pmp->showR() != chkPreviewR->get_active()) pmp->toggleR();
        });
        chkPreviewG->signal_toggled().connect ([this]() {
            EditorPanel* ep = getActiveEditorPanel();
            if (!ep) return;
            PreviewModePanel* pmp = ep->getPreviewModePanel();
            if (!pmp) return;
            if (pmp->showG() != chkPreviewG->get_active()) pmp->toggleG();
        });
        chkPreviewB->signal_toggled().connect ([this]() {
            EditorPanel* ep = getActiveEditorPanel();
            if (!ep) return;
            PreviewModePanel* pmp = ep->getPreviewModePanel();
            if (!pmp) return;
            if (pmp->showB() != chkPreviewB->get_active()) pmp->toggleB();
        });
        chkPreviewL->signal_toggled().connect ([this]() {
            EditorPanel* ep = getActiveEditorPanel();
            if (!ep) return;
            PreviewModePanel* pmp = ep->getPreviewModePanel();
            if (!pmp) return;
            if (pmp->showL() != chkPreviewL->get_active()) pmp->toggleL();
        });

        // -- Color Management section --
        Gtk::Separator* menuSep5 = Gtk::manage (new Gtk::Separator (Gtk::ORIENTATION_HORIZONTAL));

        Gtk::Box* intentRow = Gtk::manage (new Gtk::Box (Gtk::ORIENTATION_HORIZONTAL, 8));
        intentRow->set_margin_start (4);
        intentRow->set_margin_end (4);
        Gtk::Label* intentLabel = Gtk::manage (new Gtk::Label (M ("MAIN_MENU_RENDERING_INTENT")));
        intentCombo = Gtk::manage (new Gtk::ComboBoxText ());
        intentCombo->append (M ("PREFERENCES_INTENT_PERCEPTUAL"));
        intentCombo->append (M ("PREFERENCES_INTENT_RELATIVE"));
        intentCombo->append (M ("PREFERENCES_INTENT_ABSOLUTE"));
        intentCombo->set_active (1);
        intentRow->pack_start (*intentLabel, false, false);
        intentRow->pack_end (*intentCombo, false, false);

        intentCombo->signal_changed().connect ([this]() {
            EditorPanel* ep = getActiveEditorPanel();
            if (!ep) return;
            int idx = intentCombo->get_active_row_number();
            if (idx >= 0) ep->setRenderingIntent(idx);
        });

        Gtk::Box* profileRow = Gtk::manage (new Gtk::Box (Gtk::ORIENTATION_HORIZONTAL, 8));
        profileRow->set_margin_start (4);
        profileRow->set_margin_end (4);
        Gtk::Label* profileLabel = Gtk::manage (new Gtk::Label (M ("MAIN_MENU_MONITOR_PROFILE")));
        profileCombo = Gtk::manage (new Gtk::ComboBoxText ());
        profileRow->pack_start (*profileLabel, false, false);
        profileRow->pack_end (*profileCombo, false, false);

        profileCombo->signal_changed().connect ([this]() {
            EditorPanel* ep = getActiveEditorPanel();
            if (!ep) return;
            int idx = profileCombo->get_active_row_number();
            if (idx >= 0) ep->setMonitorProfileIndex(idx);
        });

        chkSoftProof = Gtk::manage (new Gtk::CheckButton (M ("MAIN_MENU_SOFT_PROOFING")));
        chkSoftProof->signal_toggled().connect ([this]() {
            EditorPanel* ep = getActiveEditorPanel();
            if (!ep) return;
            if (ep->getSoftProofing() != chkSoftProof->get_active())
                ep->setSoftProofing(chkSoftProof->get_active());
        });

        chkGamutCheck = Gtk::manage (new Gtk::CheckButton (M ("MAIN_MENU_GAMUT_WARNING")));
        chkGamutCheck->signal_toggled().connect ([this]() {
            EditorPanel* ep = getActiveEditorPanel();
            if (!ep) return;
            if (ep->getGamutCheck() != chkGamutCheck->get_active())
                ep->setGamutCheck(chkGamutCheck->get_active());
        });

        optionsBox->pack_start (*menuHelpBtn, false, false);
        optionsBox->pack_start (*menuIccBtn, false, false);
        optionsBox->pack_start (*menuPrefsBtn, false, false);
        optionsBox->pack_start (*menuSep1, false, false, 4);
        optionsBox->pack_start (*menuNavBtn, false, false);
        optionsBox->pack_start (*menuHistBtn, false, false);
        optionsBox->pack_start (*menuSep2, false, false, 4);
        optionsBox->pack_start (*bgColorRow, false, false);
        optionsBox->pack_start (*chkFocusMask, false, false);
        optionsBox->pack_start (*chkSharpMask, false, false);
        optionsBox->pack_start (*chkClippedShadows, false, false);
        optionsBox->pack_start (*chkClippedHighlights, false, false);
        optionsBox->pack_start (*menuSep3, false, false, 4);
        optionsBox->pack_start (*chkHistogramProfile, false, false);
        optionsBox->pack_start (*menuSep4, false, false, 4);
        optionsBox->pack_start (*previewChannelLabel, false, false);
        optionsBox->pack_start (*chkPreviewR, false, false);
        optionsBox->pack_start (*chkPreviewG, false, false);
        optionsBox->pack_start (*chkPreviewB, false, false);
        optionsBox->pack_start (*chkPreviewL, false, false);
        optionsBox->pack_start (*menuSep5, false, false, 4);
        optionsBox->pack_start (*intentRow, false, false);
#if !defined(__APPLE__)
        optionsBox->pack_start (*profileRow, false, false);
#endif
        optionsBox->pack_start (*chkSoftProof, false, false);
        optionsBox->pack_start (*chkGamutCheck, false, false);

        // MCP Server button
        auto* menuMcpSep = Gtk::manage(new Gtk::Separator());
        auto* menuMcpBtn = Gtk::manage(new Gtk::Button(M("MCP_MENU_BUTTON")));
        menuMcpBtn->set_relief(Gtk::RELIEF_NONE);
        menuMcpBtn->set_halign(Gtk::ALIGN_START);
        menuMcpBtn->signal_clicked().connect([this]() {
            optionsBtn->get_popover()->hide();
            showMcpDialog();
        });
        optionsBox->pack_start(*menuMcpSep, false, false, 4);
        optionsBox->pack_start(*menuMcpBtn, false, false);

        optionsBox->show_all ();
        optionsPopover->add (*optionsBox);
        optionsBtn->set_popover (*optionsPopover);

        // -- Navigation buttons (icon-only, no labels) --
        navFileBrowser = Gtk::manage (new Gtk::ToggleButton ());
        navFileBrowser->set_name ("NavButton");
        navFileBrowser->set_image (*Gtk::manage (new RTImage ("nav-filebrowser", Gtk::ICON_SIZE_LARGE_TOOLBAR)));
        navFileBrowser->set_tooltip_markup (M ("MAIN_FRAME_FILEBROWSER_TOOLTIP"));
        navFileBrowser->set_relief (Gtk::RELIEF_NONE);
        navFileBrowser->set_active (true);
        navFileBrowser->signal_toggled().connect (
            sigc::bind (sigc::mem_fun (*this, &RTWindow::on_nav_switched), navFileBrowser));

        navQueue = Gtk::manage (new Gtk::ToggleButton ());
        navQueue->set_name ("NavButton");
        navQueue->set_image (*Gtk::manage (new RTImage ("nav-queue", Gtk::ICON_SIZE_LARGE_TOOLBAR)));
        navQueue->set_tooltip_markup (M ("MAIN_FRAME_QUEUE_TOOLTIP"));
        navQueue->set_relief (Gtk::RELIEF_NONE);
        navQueue->signal_toggled().connect (
            sigc::bind (sigc::mem_fun (*this, &RTWindow::on_nav_switched), navQueue));

        navEditor = Gtk::manage (new Gtk::ToggleButton ());
        navEditor->set_name ("NavButton");
        navEditor->set_image (*Gtk::manage (new RTImage ("nav-editor", Gtk::ICON_SIZE_LARGE_TOOLBAR)));
        navEditor->set_tooltip_markup (M ("MAIN_FRAME_EDITOR_TOOLTIP"));
        navEditor->set_relief (Gtk::RELIEF_NONE);
        navEditor->signal_toggled().connect (
            sigc::bind (sigc::mem_fun (*this, &RTWindow::on_nav_switched), navEditor));

        // -- Progress bar (hidden until processing starts) --
        setExpandAlignProperties (&prProgBar, false, false, Gtk::ALIGN_CENTER, Gtk::ALIGN_CENTER);
        prProgBar.set_show_text (true);
        prProgBar.set_orientation (Gtk::ORIENTATION_HORIZONTAL);
        prProgBar.set_no_show_all(true);
        prProgBar.hide();

        // -- Window control buttons (right side) --
        iFullscreen = new RTImage ("fullscreen-enter", Gtk::ICON_SIZE_LARGE_TOOLBAR);
        iFullscreen_exit = new RTImage ("fullscreen-leave", Gtk::ICON_SIZE_LARGE_TOOLBAR);

        btn_minimize = Gtk::manage (new Gtk::Button ());
        btn_minimize->set_name ("WindowControlButton");
        btn_minimize->set_relief (Gtk::RELIEF_NONE);
        btn_minimize->set_image (*Gtk::manage (new RTImage ("window-minimize", Gtk::ICON_SIZE_LARGE_TOOLBAR)));
        btn_minimize->set_tooltip_markup (M ("MAIN_BUTTON_MINIMIZE"));
        btn_minimize->signal_clicked().connect (sigc::mem_fun (*this, &RTWindow::minimize_window));

        btn_fullscreen = Gtk::manage (new Gtk::Button ());
        btn_fullscreen->set_name ("WindowControlButton");
        btn_fullscreen->set_relief (Gtk::RELIEF_NONE);
        btn_fullscreen->set_tooltip_markup (M ("MAIN_BUTTON_FULLSCREEN"));
        btn_fullscreen->set_image (*iFullscreen);
        btn_fullscreen->signal_clicked().connect (sigc::mem_fun (*this, &RTWindow::toggle_fullscreen));

        btn_close = Gtk::manage (new Gtk::Button ());
        btn_close->set_name ("WindowCloseButton");
        btn_close->set_relief (Gtk::RELIEF_NONE);
        btn_close->set_image (*Gtk::manage (new RTImage ("window-close", Gtk::ICON_SIZE_LARGE_TOOLBAR)));
        btn_close->set_tooltip_markup (M ("MAIN_BUTTON_CLOSE"));
        btn_close->signal_clicked().connect (sigc::mem_fun (*this, &RTWindow::close_window));

        // -- Assemble header bar --
        headerBar->pack_start (*optionsBtn);
        Gtk::Separator* sep1 = Gtk::manage (new Gtk::Separator (Gtk::ORIENTATION_VERTICAL));
        sep1->set_margin_start (4);
        sep1->set_margin_end (4);
        headerBar->pack_start (*sep1);
        headerBar->pack_start (*navFileBrowser);
        headerBar->pack_start (*navQueue);
        headerBar->pack_start (*navEditor);
        Gtk::Separator* sep2 = Gtk::manage (new Gtk::Separator (Gtk::ORIENTATION_VERTICAL));
        sep2->set_margin_start (4);
        sep2->set_margin_end (4);
        headerBar->pack_start (*sep2);
        headerBar->pack_start (prProgBar);

        headerBar->pack_end (*btn_close);
        headerBar->pack_end (*btn_fullscreen);
        headerBar->pack_end (*btn_minimize);

        // Use an invisible widget as the CSD titlebar so the compositor doesn't add
        // server-side decorations. The real headerBar is packed as a regular widget
        // so it stays visible in fullscreen.
#ifndef _WIN32
        Gtk::EventBox* fakeTitlebar = Gtk::manage(new Gtk::EventBox());
        fakeTitlebar->set_size_request(-1, 0);
        fakeTitlebar->set_no_show_all(true);
        set_titlebar(*fakeTitlebar);
#endif

        // Wrap headerBar in an EventBox so we can catch clicks on empty areas for dragging
        Gtk::EventBox* headerDragBox = Gtk::manage(new Gtk::EventBox());
        headerDragBox->set_above_child(false); // let child widgets get events first
        headerDragBox->add(*headerBar);
        headerDragBox->signal_button_press_event().connect([this](GdkEventButton* event) -> bool {
            if (event->type == GDK_2BUTTON_PRESS && event->button == 1) {
                if (is_maximized()) {
                    unmaximize();
                } else {
                    maximize();
                }
                return true;
            }
            if (event->type == GDK_BUTTON_PRESS && event->button == 1) {
                get_window()->begin_move_drag(event->button,
                    event->x_root, event->y_root, event->time);
                return true;
            }
            return false;
        }, false);

        Gtk::Box* mainVBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
        mainVBox->set_name("MainVBox");
        mainVBox->pack_start(*headerDragBox, false, false);
        mainVBox->pack_start(*mainOverlay, true, true);

        pldBridge = new PLDBridge (static_cast<rtengine::ProgressListener*> (this));

        add (*mainVBox);
        show_all ();

        // CSD edge resize: detect cursor near window edges and start resize drag
        add_events(Gdk::POINTER_MOTION_MASK | Gdk::BUTTON_PRESS_MASK);
        signal_motion_notify_event().connect(
            sigc::mem_fun(*this, &RTWindow::onWindowMotion), false);
        signal_button_press_event().connect(
            sigc::mem_fun(*this, &RTWindow::onWindowButtonPress), false);

        bpanel->init (this);

        if (!App::get().argv1().empty() && !App::get().isRemote()) {
            Thumbnail* thm = cacheMgr->getEntry (App::get().argv1());

            if (thm) {
                fpanel->fileCatalog->openRequested ({thm});
            }
        }
    }
}

RTWindow::~RTWindow()
{

    // Stop MCP server before other members are destroyed
    if (mcpServer_) {
        mcpServer_->stop();
    }

    if (!App::get().isSimpleEditor()) {
        delete pldBridge;
    }

    pldBridge = nullptr;
#if defined(__APPLE__)
    g_object_unref (osxApp);
#endif

    delete fpanel;
    delete iFullscreen;
    delete iFullscreen_exit;
}

void RTWindow::on_realize ()
{
    Gtk::Window::on_realize ();

    if ( fpanel ) {
        fpanel->setAspect();
    }

    if (App::get().isSimpleEditor()) {
        epanel->setAspect();
    }

    mainWindowCursorManager.init (get_window());

    // Start startup animation timer
    if (startupOverlay_ && startupAnimActive_) {
        auto startTime = std::make_shared<Glib::Timer>();
        startTime->start();

        startupAnimConn_ = Glib::signal_timeout().connect (
            [this, startTime]() -> bool {
                startupAnimTime_ = startTime->elapsed();

                if (startupAnimTime_ > 3.1) {
                    // Animation complete
                    startupAnimActive_ = false;
                    startupOverlay_->hide ();
                    startupOverlay_->set_no_show_all (true);
                    return false; // disconnect timer
                }

                startupOverlay_->queue_draw ();
                return true; // continue
            }, 16); // ~60fps
    }

    // Display release notes only if new major version.
    bool waitForSplash = false;
    auto& options = App::get().mut_options();
    if (options.is_new_version()) {
        // Update the version parameter with the right value
        options.version = App::VERSION;

        splash = new Splash (*this, false);
        splash->set_transient_for (*this);
        splash->signal_delete_event().connect ( sigc::mem_fun (*this, &RTWindow::splashClosed) );

        if (splash->hasReleaseNotes()) {
            waitForSplash = true;
            splash->showReleaseNotes();
            splash->show ();
        } else {
            delete splash;
            splash = nullptr;
        }
    }

    if (!waitForSplash) {
        showErrors();
    }

    // Auto-start MCP server if configured
    if (options.mcpAutoStart && mcpServer_ && !mcpServer_->isRunning()) {
        mcpServer_->start();
    }
}

void RTWindow::showErrors()
{
    auto& options = App::get().mut_options();
    // alerting users if the default raw and image profiles are missing
    if (options.is_defProfRawMissing()) {
        options.defProfRaw = DEFPROFILE_RAW;
        Gtk::MessageDialog msgd (*this, Glib::ustring::compose (M ("OPTIONS_DEFRAW_MISSING"), escapeHtmlChars(options.defProfRaw)), true, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
        msgd.run ();
    }
    if (options.is_bundledDefProfRawMissing()) {
        Gtk::MessageDialog msgd (*this, Glib::ustring::compose (M ("OPTIONS_BUNDLED_MISSING"), escapeHtmlChars(options.defProfRaw)), true, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
        msgd.run ();
        options.defProfRaw = DEFPROFILE_INTERNAL;
    }

    if (options.is_defProfImgMissing()) {
        options.defProfImg = DEFPROFILE_IMG;
        Gtk::MessageDialog msgd (*this, Glib::ustring::compose (M ("OPTIONS_DEFIMG_MISSING"), escapeHtmlChars(options.defProfImg)), true, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
        msgd.run ();
    }
    if (options.is_bundledDefProfImgMissing()) {
        Gtk::MessageDialog msgd (*this, Glib::ustring::compose (M ("OPTIONS_BUNDLED_MISSING"), escapeHtmlChars(options.defProfImg)), true, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
        msgd.run ();
        options.defProfImg = DEFPROFILE_INTERNAL;
    }
}

bool RTWindow::on_configure_event (GdkEventConfigure* event)
{
    auto& options = App::get().mut_options();
    if (!options.windowMaximized && !is_fullscreen && !is_minimized) {
        get_size (options.windowWidth, options.windowHeight);
        get_position (options.windowX, options.windowY);
    }

    // With update the RTScalable on scale or resolution change
    RTScalable::setDPInScale(this);

    return Gtk::Widget::on_configure_event (event);
}

bool RTWindow::on_window_state_event (GdkEventWindowState* event)
{
    // Retrieve RT window states
    App::get().mut_options().windowMaximized = event->new_window_state & GDK_WINDOW_STATE_MAXIMIZED;
    is_minimized = event->new_window_state & GDK_WINDOW_STATE_ICONIFIED;
    is_fullscreen = event->new_window_state & GDK_WINDOW_STATE_FULLSCREEN;

    return Gtk::Widget::on_window_state_event (event);
}

void RTWindow::on_mainNB_switch_page (Gtk::Widget* widget, guint page_num)
{
    if (!on_delete_has_run) {
        if (isEditorPanel (page_num)) {
            if (isSingleTabMode() && epanel) {
                MoveFileBrowserToEditor();
            }

            EditorPanel *ep = static_cast<EditorPanel*> (mainNB->get_nth_page (page_num));
            ep->setAspect();

            if (!isSingleTabMode()) {
                if (filesEdited.size() > 0) {
                    set_title_decorated (ep->getFileName());
                }
            }

            // Auto-open the browser's selected photo in the editor.
            // Skip during hero transition — heavy EditorPanel::open() would block animation.
            // The hero completion callback handles it instead.
            if (isSingleTabMode()
                && fpanel
                && !heroAnimConn_.connected()
                && !suppressEditorSwitchAutoOpen_) {
                fpanel->openSelectedInEditor();
            }

            // Sync left panel state: browser → editor
            if (isSingleTabMode() && epanel && fpanel) {
                epanel->setLeftPanelVisible(fpanel->isLeftPanelVisible());
            }

            // Collapse filter bar to prevent overlap with filmstrip
            ep->collapseFilterBar();

            // Animate editor panels in (sidebar slides from right, filmstrip from top)
            // Skip if hero transition is active — it manages its own animation
            if (isSingleTabMode() && epanel && !heroAnimConn_.connected()) {
                epanel->animateEditorIn();
            }
        } else {
            // in single tab mode with command line filename epanel does not exist yet
            if (isSingleTabMode() && epanel) {
                // Save profile on leaving the editor panel
                epanel->saveProfile();

                // Sync left panel state: editor → browser
                if (fpanel) {
                    fpanel->setLeftPanelVisible(epanel->isLeftPanelVisible());
                }

                // Moving the FileBrowser only if the user has switched to the FileBrowser tab
                if (mainNB->get_nth_page (page_num) == fpanel) {
                    MoveFileBrowserToMain();
                }
            }
        }

        // Close album view in whichever panel we're leaving
        // so the album doesn't stay open when switching between browser/editor.
        if (fpanel) {
            fpanel->closeAlbumView();
        }
        if (isSingleTabMode() && epanel) {
            epanel->closeAlbumView();
        }

        // Keep header bar nav buttons in sync with notebook page
        syncNavButtons (page_num);
    }
}

void RTWindow::addEditorPanel (EditorPanel* ep, const std::string &name)
{
    if (App::get().options().multiDisplayMode > 0) {
        EditWindow * wndEdit = EditWindow::getInstance (this);
        wndEdit->addEditorPanel (ep, name);
        wndEdit->show_all();
        wndEdit->restoreWindow(); // Need to be called after RTWindow creation to work with all OS Windows Manager
        ep->setAspect();
        wndEdit->toFront();
    } else {
        ep->setParent (this);
        ep->setParentWindow (this);
        ep->setExternalEditorChangedSignal(&externalEditorChangedSignal);

        // Tabs are hidden; just append the page
        mainNB->append_page (*ep);
        mainNB->set_current_page (mainNB->page_num (*ep));

        set_title_decorated (name);
        epanels[ name ] = ep;
        filesEdited.insert ( name );
        fpanel->refreshEditedState (filesEdited);
        ep->tbTopPanel_1_visible (false); //hide the toggle Top Panel button
    }
}

void RTWindow::remEditorPanel (EditorPanel* ep)
{
    if (ep->getIsProcessing()) {
        return;    // Will crash if destroyed while loading
    }

    if (App::get().options().multiDisplayMode > 0) {
        EditWindow * wndEdit = EditWindow::getInstance (this);
        wndEdit->remEditorPanel (ep);
    } else {
        ep->setExternalEditorChangedSignal(nullptr);
        epanels.erase (ep->getFileName());
        filesEdited.erase (ep->getFileName ());
        fpanel->refreshEditedState (filesEdited);

        mainNB->remove_page (*ep);

        if (!isEditorPanel (mainNB->get_current_page())) {
            mainNB->set_current_page (mainNB->page_num (*fpanel));

            set_title_decorated ("");
        } else {
            const EditorPanel* lep = static_cast<EditorPanel*> (mainNB->get_nth_page (mainNB->get_current_page()));
            set_title_decorated (lep->getFileName());
        }

        // TODO: ask what to do: close & apply, close & apply selection, close & revert, cancel
    }
}

bool RTWindow::selectEditorPanel (const std::string &name)
{
    if (App::get().options().multiDisplayMode > 0) {
        EditWindow * wndEdit = EditWindow::getInstance (this);

        if (wndEdit->selectEditorPanel (name)) {
            set_title_decorated (name);
            wndEdit->toFront();
            return true;
        }
    } else {
        std::map<Glib::ustring, EditorPanel*>::iterator iep = epanels.find (name);

        if (iep != epanels.end()) {
            mainNB->set_current_page (mainNB->page_num (*iep->second));
            set_title_decorated (name);
            return true;
        } else {
            //set_title_decorated(name);
            //printf("RTWindow::selectEditorPanel - plain set\n");
        }
    }

    return false;
}

bool RTWindow::keyPressed (GdkEventKey* event)
{

    bool ctrl = event->state & GDK_CONTROL_MASK;
    //bool shift = event->state & GDK_SHIFT_MASK;

    bool try_quit = false;
#if defined(__APPLE__)
    bool apple_cmd = event->state & GDK_MOD2_MASK;

    if (event->keyval == GDK_KEY_q && apple_cmd) {
        try_quit = true;
    }

#else

    if (event->keyval == GDK_KEY_q && ctrl) {
        try_quit = true;
    }

#endif

    if (try_quit) {
        if (!on_delete_event (nullptr)) {
            gtk_main_quit();
        }
    }

    if (event->keyval == GDK_KEY_F11) {
        toggle_fullscreen();
    }

    if (App::get().isSimpleEditor())
        // in simpleEditor mode, there's no other tab that can handle pressed keys, so we can send the event to editor panel then return
    {
        return epanel->handleShortcutKey (event);
    };

    // Escape dismisses queue overlay
    if (event->keyval == GDK_KEY_Escape && queueOverlayVisible) {
        hideQueueOverlay();
        return true;
    }

    if (ctrl) {
        switch (event->keyval) {
            case GDK_KEY_F2: // file browser panel
                mainNB->set_current_page (mainNB->page_num (*fpanel));
                return true;

            case GDK_KEY_F3: // toggle queue overlay
                toggleQueueOverlay();
                return true;

            case GDK_KEY_F4: //single tab mode, editor panel
                if (isSingleTabMode() && epanel) {
                    mainNB->set_current_page (mainNB->page_num (*epanel));
                }

                return true;

            case GDK_KEY_w: //multi-tab mode, close editor panel
                if (!isSingleTabMode() &&
                        mainNB->get_current_page() != mainNB->page_num (*fpanel)) {

                    EditorPanel* ep = static_cast<EditorPanel*> (mainNB->get_nth_page (mainNB->get_current_page()));
                    remEditorPanel (ep);
                    return true;
                }
        }
    }

    // Route shortcuts to bpanel when queue overlay is visible
    if (queueOverlayVisible) {
        return bpanel->handleShortcutKey (event);
    }

    if (mainNB->get_current_page() == mainNB->page_num (*fpanel)) {
        return fpanel->handleShortcutKey (event);
    } else {
        EditorPanel* ep = static_cast<EditorPanel*> (mainNB->get_nth_page (mainNB->get_current_page()));
        return ep->handleShortcutKey (event);
    }

    return false;
}

bool RTWindow::keyReleased(GdkEventKey *event)
{
    if (fpanel && mainNB->get_current_page() == mainNB->page_num(*fpanel)) {
        return fpanel->handleShortcutKeyRelease(event);
    }
    return false;
}

void RTWindow::addBatchQueueJob (BatchQueueEntry* bqe, bool head)
{

    std::vector<BatchQueueEntry*> entries;
    entries.push_back (bqe);
    bpanel->addBatchQueueJobs (entries, head);
    fpanel->queue_draw ();
}

void RTWindow::addBatchQueueJobs(const std::vector<BatchQueueEntry*>& entries)
{
    bpanel->addBatchQueueJobs (entries, false);
    fpanel->queue_draw ();
}

bool RTWindow::on_delete_event (GdkEventAny* event)
{

    if (on_delete_has_run) {
        // on Mac OSX we can get multiple events
        return false;
    }

    // Check if any editor is still processing, and do NOT quit if so. Otherwise crashes and inconsistent caches
    bool isProcessing = false;
    EditWindow* editWindow = nullptr;
    auto& options = App::get().mut_options();

    if (isSingleTabMode() || App::get().isSimpleEditor()) {
        isProcessing = epanel->getIsProcessing();
    } else if (options.multiDisplayMode > 0) {
        editWindow = EditWindow::getInstance (this);
        isProcessing = editWindow->isProcessing();
    } else {
        int pageCount = mainNB->get_n_pages();

        for (int i = 0; i < pageCount && !isProcessing; i++) {
            if (isEditorPanel (i)) {
                isProcessing |= (static_cast<EditorPanel*> (mainNB->get_nth_page (i)))->getIsProcessing();
            }
        }
    }

    if (isProcessing) {
        return true;
    }

    if ( fpanel ) {
        fpanel->saveOptions ();
    }

    if ( bpanel ) {
        bpanel->saveOptions ();
    }

    if ((isSingleTabMode() || App::get().isSimpleEditor()) && epanel->isRealized()) {
        epanel->saveProfile();
        epanel->writeOptions ();
    } else {
        if (options.multiDisplayMode > 0 && editWindow) {
            editWindow->closeOpenEditors();
            editWindow->writeOptions();
        } else if (epanels.size()) {
            // Storing the options of the last EditorPanel before Gtk destroys everything
            // Look at the active panel first, if any, otherwise look at the first one (sorted on the filename)

            int page = mainNB->get_current_page();
            Gtk::Widget *w = mainNB->get_nth_page (page);
            bool optionsWritten = false;

            for (std::map<Glib::ustring, EditorPanel*>::iterator i = epanels.begin(); i != epanels.end(); ++i) {
                if (i->second == w) {
                    i->second->writeOptions();
                    optionsWritten = true;
                }
            }

            if (!optionsWritten) {
                // fallback solution: save the options of the first editor panel
                std::map<Glib::ustring, EditorPanel*>::iterator i = epanels.begin();
                i->second->writeOptions();
            }
        }
    }

    cacheMgr->closeCache ();  // also makes cleanup if too large
    ProfilePanel::cleanup();
    PresetListPanel::cleanup();
    ClutComboBox::cleanup();
    BatchQueueEntry::savedAsIcon.reset();
    FileBrowserEntry::editedIcon.reset();
    FileBrowserEntry::recentlySavedIcon.reset();
    FileBrowserEntry::enqueuedIcon.reset();
    FileBrowserEntry::hdr.reset();
    FileBrowserEntry::ps.reset();

    if (!options.windowMaximized && !is_fullscreen && !is_minimized) {
        get_size (options.windowWidth, options.windowHeight);
        get_position (options.windowX, options.windowY);
    }

    // Retrieve window monitor ID
    options.windowMonitor = 0;
    const auto display = get_screen()->get_display();
    const int monitor_nb = display->get_n_monitors();

    for (int id = 0; id < monitor_nb; id++) {
        if (display->get_monitor_at_window(get_window()) == display->get_monitor(id)) {
            options.windowMonitor = id;
            break;
        }
    }

    try {
        Options::save ();
    } catch (Options::Error &e) {
        Gtk::MessageDialog msgd (getToplevelWindow (this), e.get_msg(), true, Gtk::MESSAGE_WARNING, Gtk::BUTTONS_CLOSE, true);
        msgd.run();
    }

    hide();

    on_delete_has_run = true;
    return false;
}


void RTWindow::writeToolExpandedStatus (std::vector<int> &tpOpen)
{
    if ((isSingleTabMode() || App::get().isGimpPlugin()) && epanel->isRealized()) {
        epanel->writeToolExpandedStatus (tpOpen);
    } else {
        // Storing the options of the last EditorPanel before Gtk destroys everything
        // Look at the active panel first, if any, otherwise look at the first one (sorted on the filename)
        if (epanels.size()) {
            int page = mainNB->get_current_page();
            Gtk::Widget *w = mainNB->get_nth_page (page);
            bool optionsWritten = false;

            for (std::map<Glib::ustring, EditorPanel*>::iterator i = epanels.begin(); i != epanels.end(); ++i) {
                if (i->second == w) {
                    i->second->writeToolExpandedStatus (tpOpen);
                    optionsWritten = true;
                }
            }

            if (!optionsWritten) {
                // fallback solution: save the options of the first editor panel
                std::map<Glib::ustring, EditorPanel*>::iterator i = epanels.begin();
                i->second->writeToolExpandedStatus (tpOpen);
            }
        }
    }
}


EditorPanel* RTWindow::getActiveEditorPanel()
{
    if (isSingleTabMode() || App::get().isGimpPlugin()) {
        return epanel;
    }

    // Multi-tab mode: find the active editor panel
    int page = mainNB->get_current_page();
    Gtk::Widget *w = mainNB->get_nth_page(page);

    for (auto& kv : epanels) {
        if (kv.second == w) {
            return kv.second;
        }
    }

    // Fallback: return first editor panel if any
    if (!epanels.empty()) {
        return epanels.begin()->second;
    }

    return nullptr;
}

void RTWindow::showRawPedia()
{
    GError* gerror = nullptr;
    gtk_show_uri(nullptr, "https://rawpedia.rawtherapee.com/", GDK_CURRENT_TIME, &gerror);
}

void RTWindow::showICCProfileCreator ()
{
    ICCProfileCreator *iccpc = new ICCProfileCreator (this);
    iccpc->run ();
    delete iccpc;

    fpanel->optionsChanged ();

    const auto& options = App::get().options();
    if (epanel) {
        epanel->defaultMonitorProfileChanged (options.rtSettings.monitorProfile, options.rtSettings.autoMonitorProfile);
    }

    for (const auto &p : epanels) {
        p.second->defaultMonitorProfileChanged (options.rtSettings.monitorProfile, options.rtSettings.autoMonitorProfile);
    }
}

void RTWindow::showPreferences ()
{
    Preferences *pref = new Preferences (this);
    pref->run ();
    delete pref;

    fpanel->optionsChanged ();

    const auto& options = App::get().options();
    if (epanel) {
        epanel->defaultMonitorProfileChanged (options.rtSettings.monitorProfile, options.rtSettings.autoMonitorProfile);
    }

    for (const auto &p : epanels) {
        p.second->defaultMonitorProfileChanged (options.rtSettings.monitorProfile, options.rtSettings.autoMonitorProfile);
    }
}

void RTWindow::setProgress(double p)
{
    if (prProgBar.get_fraction() != p) {
        prProgBar.set_fraction(p);
    }

    if (p > 0.0 && p < 1.0) {
        if (!prProgBar.get_visible()) {
            prProgBar.show();
        }
    } else {
        if (prProgBar.get_visible()) {
            prProgBar.hide();
        }
    }
}

void RTWindow::setProgressStr(const Glib::ustring& str)
{
    if (!App::get().options().mainNBVertical && prProgBar.get_text() != str) {
        prProgBar.set_text(str);
    }
}

void RTWindow::setProgressState(bool inProcessing)
{
    if (inProcessing) {
        if (!prProgBar.get_visible()) {
            prProgBar.show();
        }
    } else {
        if (prProgBar.get_visible()) {
            prProgBar.hide();
        }
    }
}

void RTWindow::error(const Glib::ustring& descr)
{
    prProgBar.set_text(descr);
}

void RTWindow::toggle_fullscreen ()
{
    onConfEventConn.block(true); // Avoid getting size and position while window is getting fullscreen

    if (is_fullscreen) {
        unfullscreen();

        if (btn_fullscreen) {
            btn_fullscreen->set_tooltip_markup (M ("MAIN_BUTTON_FULLSCREEN"));
            btn_fullscreen->set_image (*iFullscreen);
        }
    } else {
        fullscreen();

        if (btn_fullscreen) {
            btn_fullscreen->set_tooltip_markup (M ("MAIN_BUTTON_UNFULLSCREEN"));
            btn_fullscreen->set_image (*iFullscreen_exit);
        }
    }

    onConfEventConn.block(false);
}

void RTWindow::minimize_window ()
{
    iconify();
}

void RTWindow::close_window ()
{
    // Trigger the same close path as the window manager close button
    GdkEventAny ev;
    ev.type = GDK_DELETE;
    ev.window = nullptr;
    ev.send_event = TRUE;
    on_delete_event (&ev);
}

void RTWindow::on_nav_switched (Gtk::ToggleButton* active)
{
    if (navSwitching) {
        return;
    }

    navSwitching = true;

    if (active == navQueue) {
        // Queue is an independent overlay toggle, not mutually exclusive
        if (active->get_active()) {
            showQueueOverlay();
        } else {
            hideQueueOverlay();
        }
        navSwitching = false;
        return;
    }

    // Enforce mutual exclusion between file browser and editor
    if (active->get_active()) {
        if (active != navFileBrowser) { navFileBrowser->set_active (false); }
        if (active != navEditor)      { navEditor->set_active (false); }

        // Close queue overlay when switching to browser or editor
        if (queueOverlayVisible) {
            hideQueueOverlay();
        }

        // Switch to the corresponding notebook page
        if (active == navFileBrowser) {
            // Cancel any running forward hero transition
            if (heroAnimConn_.connected()) {
                heroAnimConn_.disconnect();
                heroOverlay_->hide();
                capturedThumbs_.clear();
                fpanel->setContentOpacity(1.0);
                if (epanel && epanel->catalogPane) {
                    epanel->catalogPane->set_opacity(1.0);
                }
            }
            mainNB->set_current_page (mainNB->page_num (*fpanel));
        } else if (active == navEditor) {
            // In single-tab mode, switch to epanel; in multi-tab, switch to last editor
            if (isSingleTabMode() && epanel) {
                // Instant switch — no hero animation
                mainNB->set_current_page (mainNB->page_num (*epanel));
            } else if (!epanels.empty()) {
                // Switch to the last opened editor
                mainNB->set_current_page (mainNB->page_num (*epanels.rbegin()->second));
            }
        }
    } else {
        // Don't allow deactivating the current button by clicking it again
        active->set_active (true);

        // But still close queue overlay if it's open
        if (queueOverlayVisible) {
            hideQueueOverlay();
        }
    }

    navSwitching = false;
}

void RTWindow::syncNavButtons (guint page_num)
{
    if (navSwitching || !navFileBrowser) {
        return;
    }

    navSwitching = true;

    Gtk::Widget* page = mainNB->get_nth_page (page_num);

    navFileBrowser->set_active (page == fpanel);
    navEditor->set_active (page != fpanel);
    // navQueue state is managed by overlay toggle, not notebook page

    navSwitching = false;
}

void RTWindow::SetEditorCurrent()
{
    if (!mainNB || !epanel) {
        return;
    }

    const int targetPage = mainNB->page_num (*epanel);
    if (targetPage >= 0 && mainNB->get_current_page() != targetPage) {
        suppressEditorSwitchAutoOpen_ = true;
        mainNB->set_current_page (targetPage);
        suppressEditorSwitchAutoOpen_ = false;
    }
}

void RTWindow::SetMainCurrent()
{
    if (!mainNB || !fpanel) {
        return;
    }

    const int targetPage = mainNB->page_num (*fpanel);
    if (targetPage >= 0 && mainNB->get_current_page() != targetPage) {
        mainNB->set_current_page (targetPage);
    }
}

void RTWindow::MoveFileBrowserToMain()
{
    if ( fpanel->ribbonPane->get_children().empty()) {
        FileCatalog *fCatalog = fpanel->fileCatalog;
        epanel->catalogPane->remove (*fCatalog);
        fpanel->ribbonPane->add (*fCatalog);
        fCatalog->enableTabMode (false);
        fCatalog->tbLeftPanel_1_visible (false);  // Left toggle now in FilePanel footer
        fCatalog->tbRightPanel_1_visible (true);
    }
}

void RTWindow::MoveFileBrowserToEditor()
{
    if (epanel->catalogPane->get_children().empty() ) {
        FileCatalog *fCatalog = fpanel->fileCatalog;
        fpanel->ribbonPane->remove (*fCatalog);
        fCatalog->disableInspector();
        epanel->catalogPane->add (*fCatalog);
        epanel->showTopPanel (App::get().options().editorFilmStripOpened);
        fCatalog->enableTabMode (true);
        fCatalog->refreshHeight();
        fCatalog->tbLeftPanel_1_visible (false);
        fCatalog->tbRightPanel_1_visible (false);
    }
}

void RTWindow::updateExternalEditorWidget(int selectedIndex, const std::vector<ExternalEditor> & editors)
{
    if (epanel) {
        epanel->updateExternalEditorWidget(selectedIndex, editors);
    }

    for (auto panel : epanels) {
        panel.second->updateExternalEditorWidget(selectedIndex, editors);
    }

    if (App::get().options().multiDisplayMode > 0) {
        EditWindow::getInstance(this)
            ->updateExternalEditorWidget(selectedIndex, editors);
    }
}

void RTWindow::updateProfiles (const Glib::ustring &printerProfile, rtengine::RenderingIntent printerIntent, bool printerBPC)
{
    if (epanel) {
        epanel->updateProfiles (printerProfile, printerIntent, printerBPC);
    }

    for (auto panel : epanels) {
        panel.second->updateProfiles (printerProfile, printerIntent, printerBPC);
    }
}

void RTWindow::updateTPVScrollbar (bool hide)
{
    fpanel->updateTPVScrollbar (hide);

    if (epanel) {
        epanel->updateTPVScrollbar (hide);
    }

    for (auto panel : epanels) {
        panel.second->updateTPVScrollbar (hide);
    }
}

void RTWindow::updateFBQueryTB (bool singleRow)
{
    fpanel->fileCatalog->updateFBQueryTB (singleRow);
}

void RTWindow::updateFBToolBarVisibility (bool showFilmStripToolBar)
{
    fpanel->fileCatalog->updateFBToolBarVisibility (showFilmStripToolBar);
}

void RTWindow::updateShowtooltipVisibility (bool showtooltip)
{
    if (epanel) {
        epanel->updateShowtooltipVisibility (showtooltip);
    }

    for (auto panel : epanels) {
        panel.second->updateShowtooltipVisibility (showtooltip);
    }
}

void RTWindow::updateHistogramPosition (int oldPosition, int newPosition)
{
    if (epanel) {
        epanel->updateHistogramPosition (oldPosition, newPosition);
    }

    for (auto panel : epanels) {
        panel.second->updateHistogramPosition (oldPosition, newPosition);
    }
}

void RTWindow::updateToolPanelToolLocations(
    const std::vector<Glib::ustring> &favorites, bool cloneFavoriteTools)
{
    if (fpanel) {
        fpanel->updateToolPanelToolLocations(favorites, cloneFavoriteTools);
    }

    if (epanel) {
        epanel->updateToolPanelToolLocations(favorites, cloneFavoriteTools);
    }

    for (const auto &panel : epanels) {
        panel.second->updateToolPanelToolLocations(favorites, cloneFavoriteTools);
    }

    if (App::get().options().multiDisplayMode > 0) {
        EditWindow::getInstance(this)
            ->updateToolPanelToolLocations(favorites, cloneFavoriteTools);
    }
}

bool RTWindow::splashClosed (GdkEventAny* event)
{
    delete splash;
    splash = nullptr;
    showErrors();
    return true;
}

void RTWindow::setWindowSize ()
{
    onConfEventConn.block(true); // Avoid getting size and position while window is being moved, maximized, ...

    const auto& options = App::get().options();
    Gdk::Rectangle lMonitorRect;
    const auto display = get_screen()->get_display();
    display->get_monitor (std::min (options.windowMonitor, display->get_n_monitors() - 1))->get_geometry(lMonitorRect);

#ifdef __APPLE__
    // Get macOS menu bar height
    Gdk::Rectangle lWorkAreaRect;
    display->get_monitor (std::min (options.windowMonitor, display->get_n_monitors() - 1))->get_workarea(lWorkAreaRect);
    const int macMenuBarHeight = lWorkAreaRect.get_y();

    // Place RT window to saved one in options file
    if (options.windowX <= lMonitorRect.get_x() + lMonitorRect.get_width()
            && options.windowX >= 0
            && options.windowY <= lMonitorRect.get_y() + lMonitorRect.get_height() - macMenuBarHeight
            && options.windowY >= 0) {
        move (options.windowX, options.windowY + macMenuBarHeight);
    } else {
        move (lMonitorRect.get_x(), lMonitorRect.get_y() + macMenuBarHeight);
    }
#else
    // Place RT window to saved one in options file
    if (options.windowX <= lMonitorRect.get_x() + lMonitorRect.get_width()
            && options.windowX >= 0
            && options.windowY <= lMonitorRect.get_y() + lMonitorRect.get_height()
            && options.windowY >= 0) {
        move (options.windowX, options.windowY);
    } else {
        move (lMonitorRect.get_x(), lMonitorRect.get_y());
    }
#endif

    // Maximize RT window according to options file
    if (options.windowMaximized) {
        maximize();
    } else {
        unmaximize();
        resize (options.windowWidth, options.windowHeight);
    }

    onConfEventConn.block(false);
}

void RTWindow::get_position(int& x, int& y) const
{
    // Call native function
    Gtk::Window::get_position (x, y);

    // Retrieve display (concatenation of all monitors) size
    int width = 0, height = 0;
    const auto display = get_screen()->get_display();
    const int nbMonitors = display->get_n_monitors();

    for (int i = 0; i < nbMonitors; i++) {
        Gdk::Rectangle lMonitorRect;
        display->get_monitor(i)->get_geometry(lMonitorRect);
        width = std::max(width, lMonitorRect.get_x() + lMonitorRect.get_width());
        height = std::max(height, lMonitorRect.get_y() + lMonitorRect.get_height());
    }

    // Saturate position at monitor limits to avoid unexpected behavior (fixes #6233)
    x = std::min(width, std::max(0, x));
    y = std::min(height, std::max(0, y));
}

void RTWindow::set_title_decorated (Glib::ustring fname)
{
    if (!get_title().empty()) {
        set_title ("");
    }

    if (headerBar) {
        if (!headerBar->get_title().empty()) {
            headerBar->set_title ("");
        }
        if (!headerBar->get_subtitle().empty()) {
            headerBar->set_subtitle ("");
        }
    }
}

void RTWindow::closeOpenEditors()
{
    std::map<Glib::ustring, EditorPanel*>::const_iterator itr;
    itr = epanels.begin();

    while (itr != epanels.end()) {
        remEditorPanel ((*itr).second);
        itr = epanels.begin();
    }
}

bool RTWindow::isEditorPanel (Widget* panel)
{
    return (panel != fpanel);
}

bool RTWindow::isEditorPanel (guint pageNum)
{
    return isEditorPanel (mainNB->get_nth_page (pageNum));
}

void RTWindow::setEditorMode (bool tabbedUI)
{
    MoveFileBrowserToMain();
    closeOpenEditors();
    SetMainCurrent();

    if (tabbedUI) {
        mainNB->remove_page (*epanel);
        epanel = nullptr;
        set_title_decorated ("");
    } else {
        createSetmEditor();
        epanel->show_all();
        set_title_decorated ("");
    }
}

void RTWindow::createSetmEditor()
{
    // Editor panel, single-tab mode only
    epanel = Gtk::manage ( new EditorPanel (fpanel) );
    epanel->setParent (this);
    epanel->setParentWindow (this);
    epanel->tbTopPanel_1_visible (true);
    mainNB->append_page (*epanel);
}

bool RTWindow::isSingleTabMode() const
{
    const auto& options = App::get().options();
    return !options.tabbedUI && ! (options.multiDisplayMode > 0);
}

void RTWindow::toggleQueueOverlay()
{
    if (queueOverlayVisible) {
        hideQueueOverlay();
    } else {
        showQueueOverlay();
    }
}

void RTWindow::showQueueOverlay()
{
    if (queueOverlayVisible) {
        return;
    }

    queueOverlayVisible = true;

    // Show widgets
    queueBackdrop->set_no_show_all (false);
    queueBackdrop->show_all ();
    queueBackdrop->set_no_show_all (true);
    queueOverlayBox->set_no_show_all (false);
    queueOverlayBox->show_all ();
    queueOverlayBox->set_no_show_all (true);

    // Backdrop darkens instantly
    queueAnimFraction = std::max (queueAnimFraction, 0.01);

    // Force batch queue to recalculate layout and scrollbars for new size
    bpanel->queue_resize ();

    // Animate drawer slide-down — 100ms with ease-out quart
    queueAnimConn.disconnect ();
    queueAnimConn = Glib::signal_timeout ().connect ([this]() -> bool {
        queueAnimFraction += 16.0 / 100.0;
        if (queueAnimFraction >= 1.0) {
            queueAnimFraction = 1.0;
            queueBackdrop->queue_draw ();
            queueOverlayBox->queue_draw ();
            return false;
        }
        queueBackdrop->queue_draw ();
        queueOverlayBox->queue_draw ();
        return true;
    }, 16);

    // Sync nav button state (save/restore navSwitching for re-entrant safety)
    bool wasSwitching = navSwitching;
    navSwitching = true;
    navQueue->set_active (true);
    navSwitching = wasSwitching;
}

void RTWindow::hideQueueOverlay()
{
    if (!queueOverlayVisible) {
        return;
    }

    queueOverlayVisible = false;

    // Fade out drawer + backdrop — 70ms snap shut.
    // Uses set_opacity() which works at compositor level, properly hiding
    // child widgets that have their own GdkWindows (thumbnails, scrollbars).
    queueAnimConn.disconnect ();
    queueAnimConn = Glib::signal_timeout ().connect ([this]() -> bool {
        queueAnimFraction -= 16.0 / 70.0;
        if (queueAnimFraction <= 0.0) {
            queueAnimFraction = 0.0;
            queueOverlayBox->set_opacity (1.0); // restore for next show
            queueBackdrop->hide ();
            queueOverlayBox->hide ();
            return false;
        }
        queueOverlayBox->set_opacity (queueAnimFraction * queueAnimFraction);
        queueBackdrop->queue_draw ();
        return true;
    }, 16);

    // Sync nav button state (save/restore navSwitching for re-entrant safety)
    bool wasSwitching = navSwitching;
    navSwitching = true;
    navQueue->set_active (false);
    navSwitching = wasSwitching;
}

void RTWindow::captureVisibleThumbnails()
{
    capturedThumbs_.clear();
    if (!fpanel || !fpanel->fileCatalog || !fpanel->fileCatalog->fileBrowser) return;

    auto* fb = fpanel->fileCatalog->fileBrowser;
    const auto& entries = fb->getEntries();
    int vpW = fb->get_allocated_width();
    int vpH = fb->get_allocated_height();

    for (auto* entry : entries) {
        if (!entry->insideWindow(0, 0, vpW, vpH)) continue;

        auto surface = entry->snapshotSurface();
        if (!surface) continue;

        // Translate entry position to mainOverlay coordinates
        int absX = 0, absY = 0;
        fb->translate_coordinates(*mainOverlay, entry->getX(), entry->getY(), absX, absY);

        CapturedThumb ct;
        ct.surface = surface;
        ct.srcX = absX;
        ct.srcY = absY;
        ct.srcW = entry->getEffectiveWidth();
        ct.srcH = entry->getEffectiveHeight();
        ct.dstX = ct.srcX;  // default: same position (will be updated for heroes)
        ct.dstY = ct.srcY;
        ct.dstW = ct.srcW;
        ct.dstH = ct.srcH;
        ct.isHero = false;
        ct.entry = entry;
        capturedThumbs_.push_back(std::move(ct));
    }
}

void RTWindow::computeFilmstripTargets()
{
    if (!epanel || !epanel->catalogPane || !fpanel || !fpanel->fileCatalog) return;

    auto* fb = fpanel->fileCatalog->fileBrowser;
    const auto& entries = fb->getEntries();

    int filmVpW = fb->get_allocated_width();
    int filmVpH = fb->get_allocated_height();
    if (filmVpW < 10 || filmVpH < 10) return; // Allocation not ready

    // Get filmstrip's internal widget position in overlay coordinates
    int filmAbsX = 0, filmAbsY = 0;
    fb->translate_coordinates(*mainOverlay, 0, 0, filmAbsX, filmAbsY);

    // Store filmstrip clip rect so the draw lambda can fade/clip heroes at edges
    heroFilmX_ = filmAbsX;
    heroFilmY_ = filmAbsY;
    heroFilmW_ = filmVpW;
    heroFilmH_ = filmVpH;

    // Build entry→captured_thumb map for O(1) lookup
    std::unordered_map<ThumbBrowserEntryBase*, CapturedThumb*> capturedMap;
    for (auto& ct : capturedThumbs_) {
        capturedMap[ct.entry] = &ct;
    }

    // Compute horizontal filmstrip positions manually.
    // arrangeFiles() may not have run yet (layout paused + widget not allocated),
    // so we mirror the TB_Horizontal branch: entries placed left-to-right at y=0.
    // Scroll is at 0 for a freshly-switched filmstrip.
    int currx = 0;
    for (auto* entry : entries) {
        if (entry->filtered) continue;

        int entryW = entry->getMinimalWidth();
        int entryH = entry->getEffectiveHeight();

        // Check if this entry would be visible in the viewport (scroll=0)
        if (currx + entryW > 0 && currx < filmVpW) {
            auto it = capturedMap.find(entry);
            if (it != capturedMap.end()) {
                CapturedThumb* ct = it->second;
                ct->isHero = true;
                ct->dstX = filmAbsX + currx;
                ct->dstY = filmAbsY;
                ct->dstW = entryW;
                ct->dstH = entryH;
            }
        }

        currx += entryW;
    }
}

void RTWindow::startHeroTransition()
{
    heroAnimConn_.disconnect();
    heroAnimFraction_ = 0.0;
    heroPageSwitched_ = false;
    heroAnimIn_ = true;

    // Show the hero overlay (thumbnails drawn on transparent layer above browser)
    heroOverlay_->set_no_show_all(false);
    heroOverlay_->show_all();
    heroOverlay_->set_no_show_all(true);

    // Don't switch pages yet — browser stays visible underneath,
    // fading out smoothly (including toolbar/header)
    heroAnimConn_ = Glib::signal_timeout().connect([this]() -> bool {
        heroAnimFraction_ += 16.0 / 400.0;  // 400ms total hero animation

        if (heroAnimFraction_ >= 1.0) {
            heroAnimFraction_ = 1.0;
            // Final state: overlay done, real filmstrip visible
            fpanel->setContentOpacity(1.0);
            if (epanel->catalogPane) {
                epanel->catalogPane->set_opacity(1.0);
            }
            heroOverlay_->hide();
            capturedThumbs_.clear();

            // Now open the selected image — use idle dispatch so it runs
            // after this timer callback fully unwinds.
            Glib::signal_idle().connect_once([this]() {
                if (fpanel) {
                    fpanel->openSelectedInEditor();
                }
            });

            return false;
        }

        // Phase 1 (0–35%): Browser fades out smoothly (including header/toolbar)
        // Hero thumbnails start floating, non-heroes fade
        if (heroAnimFraction_ < 0.35) {
            double fade = heroAnimFraction_ / 0.35; // 0→1
            double easedFade = fade * fade; // ease-in-quad
            fpanel->setContentOpacity(1.0 - easedFade);
        }

        // Phase 2 (35%): Switch pages — browser fully faded, switch under overlay
        if (!heroPageSwitched_ && heroAnimFraction_ >= 0.35) {
            heroPageSwitched_ = true;
            fpanel->setContentOpacity(1.0); // restore for next time
            mainNB->set_current_page(mainNB->page_num(*epanel));
            // MoveFileBrowserToEditor + setAspect happen in on_mainNB_switch_page

            // Hide filmstrip during hero — we'll crossfade it in
            if (epanel->catalogPane) {
                epanel->catalogPane->set_opacity(0.0);
            }
            // Start sidebar slide (skip filmstrip animation)
            epanel->animateEditorIn(true);

            // Compute filmstrip targets immediately — manual position computation
            // doesn't depend on GTK allocation, only on entry order and sizes.
            computeFilmstripTargets();
        }

        // Phase 3 (70–100%): Crossfade real filmstrip in
        if (heroPageSwitched_ && heroAnimFraction_ > 0.70 && epanel->catalogPane) {
            double reveal = (heroAnimFraction_ - 0.70) / 0.30; // 0→1
            double easedReveal = 1.0 - std::pow(1.0 - reveal, 3); // ease-out-cubic
            epanel->catalogPane->set_opacity(easedReveal);
        }

        heroOverlay_->queue_draw();
        return true;
    }, 16);
}

void RTWindow::captureFilmstripThumbnails()
{
    capturedThumbs_.clear();
    if (!epanel || !epanel->catalogPane || !fpanel || !fpanel->fileCatalog) return;

    auto* fb = fpanel->fileCatalog->fileBrowser;
    const auto& entries = fb->getEntries();
    int vpW = fb->get_allocated_width();
    int vpH = fb->get_allocated_height();

    for (auto* entry : entries) {
        if (!entry->insideWindow(0, 0, vpW, vpH)) continue;

        auto surface = entry->snapshotSurface();
        if (!surface) continue;

        int absX = 0, absY = 0;
        fb->translate_coordinates(*mainOverlay, entry->getX(), entry->getY(), absX, absY);

        CapturedThumb ct;
        ct.surface = surface;
        ct.srcX = absX;
        ct.srcY = absY;
        ct.srcW = entry->getEffectiveWidth();
        ct.srcH = entry->getEffectiveHeight();
        ct.dstX = ct.srcX;
        ct.dstY = ct.srcY;
        ct.dstW = ct.srcW;
        ct.dstH = ct.srcH;
        ct.isHero = false;
        ct.entry = entry;
        capturedThumbs_.push_back(std::move(ct));
    }
}

void RTWindow::computeBrowserTargets()
{
    if (!fpanel || !fpanel->fileCatalog || !fpanel->fileCatalog->fileBrowser) return;

    auto* fb = fpanel->fileCatalog->fileBrowser;
    int vpW = fb->get_allocated_width();
    int vpH = fb->get_allocated_height();

    for (auto& ct : capturedThumbs_) {
        if (!ct.entry->insideWindow(0, 0, vpW, vpH)) continue;

        int absX = 0, absY = 0;
        fb->translate_coordinates(*mainOverlay,
            ct.entry->getX(), ct.entry->getY(),
            absX, absY);

        ct.isHero = true;
        ct.dstX = absX;
        ct.dstY = absY;
        ct.dstW = ct.entry->getEffectiveWidth();
        ct.dstH = ct.entry->getEffectiveHeight();
    }
}

void RTWindow::startReverseHeroTransition(std::function<void()> afterPageSwitch)
{
    heroAnimConn_.disconnect();
    viewAnimConn_.disconnect();
    heroAnimFraction_ = 0.0;
    heroPageSwitched_ = false;
    heroAnimIn_ = false;

    // Show overlay with captured filmstrip thumbnails
    heroOverlay_->set_no_show_all(false);
    heroOverlay_->show_all();
    heroOverlay_->set_no_show_all(true);

    heroAnimConn_ = Glib::signal_timeout().connect([this, afterPageSwitch]() -> bool {
        heroAnimFraction_ += 16.0 / 400.0;  // 400ms total

        if (heroAnimFraction_ >= 1.0) {
            heroAnimFraction_ = 1.0;
            fpanel->setContentOpacity(1.0);
            heroOverlay_->hide();
            capturedThumbs_.clear();

            // Resume layout — flush any accumulated inserts now that animation is done
            if (fpanel && fpanel->fileCatalog && fpanel->fileCatalog->fileBrowser) {
                fpanel->fileCatalog->fileBrowser->resumeLayout();
            }

            return false;
        }

        // Phase 1 (0–25%): Filmstrip thumbnails visible at their filmstrip positions,
        // editor content fading underneath
        // Phase 2 (25%): Switch to browser page
        if (!heroPageSwitched_ && heroAnimFraction_ >= 0.25) {
            heroPageSwitched_ = true;
            if (afterPageSwitch) afterPageSwitch();
            // Browser appears but hidden behind overlay
            fpanel->setContentOpacity(0.0);

            // Compute browser grid target positions immediately — enableTabMode(false)
            // already ran arrangeFiles() via filterChanged(), so entries have valid positions.
            computeBrowserTargets();

            // Pause layout so animateEditorOut's resumeLayout() (at 180ms) doesn't
            // trigger a heavy arrangeFiles() mid-animation from accumulated inserts.
            if (fpanel && fpanel->fileCatalog && fpanel->fileCatalog->fileBrowser) {
                fpanel->fileCatalog->fileBrowser->pauseLayout();
            }
        }

        // Phase 3 (25–90%): Hero thumbnails grow from filmstrip to browser grid positions
        // Phase 4 (70–100%): Real browser crossfades in
        if (heroPageSwitched_ && heroAnimFraction_ > 0.70) {
            double reveal = (heroAnimFraction_ - 0.70) / 0.30;
            double easedReveal = 1.0 - std::pow(1.0 - reveal, 3);
            fpanel->setContentOpacity(easedReveal);
        }

        heroOverlay_->queue_draw();
        return true;
    }, 16);
}

void RTWindow::showMcpDialog()
{
    mcp::McpDialog dlg(*this, mcpServer_.get());
    dlg.run();
}

int RTWindow::detectEdge(double x, double y, int w, int h) const
{
    const int g = RESIZE_GRIP;
    bool left   = x < g;
    bool right  = x >= w - g;
    bool top    = y < g;
    bool bottom = y >= h - g;

    if (top && left)     return Gdk::WINDOW_EDGE_NORTH_WEST;
    if (top && right)    return Gdk::WINDOW_EDGE_NORTH_EAST;
    if (bottom && left)  return Gdk::WINDOW_EDGE_SOUTH_WEST;
    if (bottom && right) return Gdk::WINDOW_EDGE_SOUTH_EAST;
    if (top)             return Gdk::WINDOW_EDGE_NORTH;
    if (bottom)          return Gdk::WINDOW_EDGE_SOUTH;
    if (left)            return Gdk::WINDOW_EDGE_WEST;
    if (right)           return Gdk::WINDOW_EDGE_EAST;
    return -1;
}

bool RTWindow::onWindowMotion(GdkEventMotion* event)
{
    if (is_fullscreen || is_maximized()) return false;

    int w = get_allocated_width();
    int h = get_allocated_height();
    int edge = detectEdge(event->x, event->y, w, h);

    Glib::RefPtr<Gdk::Window> win = get_window();
    if (!win) return false;

    if (edge < 0) {
        win->set_cursor();
        return false;
    }

    Glib::RefPtr<Gdk::Cursor> cursor;
    Glib::RefPtr<Gdk::Display> disp = get_display();
    switch (static_cast<Gdk::WindowEdge>(edge)) {
        case Gdk::WINDOW_EDGE_NORTH:       cursor = Gdk::Cursor::create(disp, "n-resize"); break;
        case Gdk::WINDOW_EDGE_SOUTH:       cursor = Gdk::Cursor::create(disp, "s-resize"); break;
        case Gdk::WINDOW_EDGE_WEST:        cursor = Gdk::Cursor::create(disp, "w-resize"); break;
        case Gdk::WINDOW_EDGE_EAST:        cursor = Gdk::Cursor::create(disp, "e-resize"); break;
        case Gdk::WINDOW_EDGE_NORTH_WEST:  cursor = Gdk::Cursor::create(disp, "nw-resize"); break;
        case Gdk::WINDOW_EDGE_NORTH_EAST:  cursor = Gdk::Cursor::create(disp, "ne-resize"); break;
        case Gdk::WINDOW_EDGE_SOUTH_WEST:  cursor = Gdk::Cursor::create(disp, "sw-resize"); break;
        case Gdk::WINDOW_EDGE_SOUTH_EAST:  cursor = Gdk::Cursor::create(disp, "se-resize"); break;
    }
    win->set_cursor(cursor);
    return false;
}

bool RTWindow::onWindowButtonPress(GdkEventButton* event)
{
    if (is_fullscreen || is_maximized()) return false;
    if (event->button != 1 || event->type != GDK_BUTTON_PRESS) return false;

    int w = get_allocated_width();
    int h = get_allocated_height();
    int edge = detectEdge(event->x, event->y, w, h);

    if (edge < 0) return false;

    get_window()->begin_resize_drag(static_cast<Gdk::WindowEdge>(edge),
        event->button,
        static_cast<int>(event->x_root), static_cast<int>(event->y_root),
        event->time);
    return true;
}
namespace
{

}
