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

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>

#include "rtengine/rt_math.h"

#include <gtkmm.h>
#include "rtwindow.h"
#include "guiutils.h"
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
#include "indclippedpanel.h"
#include "previewmodepanel.h"
#include "presetlistpanel.h"
#include "profilepanel.h"
#include "tools/filmsimulation.h"
#include "mcp/mcpserver.h"
#include "mcp/mcpdialog.h"
#include "steepperflog.h"
#include "widgets/basic/adjuster.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

#ifdef _WIN32
#include "rtengine/leanwindows.h"
#endif

Glib::RefPtr<Gtk::CssProvider> cssForced;
Glib::RefPtr<Gtk::CssProvider> cssRT;
Glib::RefPtr<Gtk::CssProvider> cssSteepPalette;
Glib::RefPtr<Gtk::CssProvider> cssSteepWidgets;

static std::string editorFileKey(const Glib::ustring& path)
{
    std::string key = path.casefold().raw();
    std::replace(key.begin(), key.end(), '\\', '/');
    return key;
}

namespace
{

bool viewSwitchTraceOn()
{
    static const bool enabled = std::getenv("STEEP_FILESEL_LOG") != nullptr;
    return enabled;
}

// GUI freezes worth reporting, in ms (STEEP_LOOPSTALL_MS overrides).
int loopStallThresholdMs()
{
    static const int value = []() {
        const char* const env = std::getenv("STEEP_LOOPSTALL_MS");
        const int parsed = env ? std::atoi(env) : 0;
        return parsed > 0 ? parsed : 100;
    }();
    return value;
}

#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
// While the GUI thread is frozen, find out where. A watchdog thread notices
// the heartbeat has stopped, suspends the GUI thread just long enough to copy
// its instruction pointer and the top of its stack (plain memcpy: nothing that
// could take a lock the frozen thread might hold), resumes it, and only then
// scans the copy for return addresses inside steep.exe. Offsets are relative
// to the image base: nm address = ImageBase (0x140000000) + offset.
class LoopStallSampler
{
public:
    static void start(std::shared_ptr<std::atomic<gint64>> lastTick)
    {
        HANDLE gui = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &gui,
                             THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0)) {
            return;
        }

        // Windows 8+ API, looked up at run time (the headers may target 7).
        ULONG_PTR stackLow = 0;
        ULONG_PTR stackHigh = 0;
        using StackLimitsFn = VOID (WINAPI*)(PULONG_PTR, PULONG_PTR);
        const auto stackLimits = reinterpret_cast<StackLimitsFn>(reinterpret_cast<void*>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetCurrentThreadStackLimits")));

        if (stackLimits) {
            stackLimits(&stackLow, &stackHigh);
        }

        const auto base = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        const DWORD64 imageLow = reinterpret_cast<DWORD64>(base);
        const DWORD64 imageHigh = imageLow + nt->OptionalHeader.SizeOfImage;

        std::thread([gui, stackLow, stackHigh, imageLow, imageHigh, lastTick]() {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
            const gint64 thresholdUs = static_cast<gint64>(loopStallThresholdMs()) * 1000;
            gint64 sampledForTick = 0;
            int samplesThisStall = 0;

            for (;;) {
                Sleep(25);
                const gint64 tick = lastTick->load(std::memory_order_acquire);
                const gint64 stalledUs = g_get_monotonic_time() - tick;

                if (stalledUs < thresholdUs) {
                    continue;
                }

                // A few samples per stall are enough to see where it sits.
                if (tick != sampledForTick) {
                    sampledForTick = tick;
                    samplesThisStall = 0;
                }

                if (samplesThisStall >= 4) {
                    continue;
                }

                ++samplesThisStall;

                constexpr size_t kStackCopy = 16 * 1024;
                static unsigned char stackCopy[kStackCopy];
                size_t copied = 0;
                DWORD64 rip = 0;

                if (SuspendThread(gui) == static_cast<DWORD>(-1)) {
                    continue;
                }

                CONTEXT ctx;
                memset(&ctx, 0, sizeof(ctx));
                ctx.ContextFlags = CONTEXT_CONTROL;

                if (GetThreadContext(gui, &ctx)) {
                    rip = ctx.Rip;
                    const ULONG_PTR rsp = static_cast<ULONG_PTR>(ctx.Rsp);

                    if (rsp >= stackLow && rsp < stackHigh) {
                        copied = std::min<size_t>(kStackCopy, stackHigh - rsp);
                        memcpy(stackCopy, reinterpret_cast<const void*>(rsp), copied);
                    }
                }

                ResumeThread(gui);

                char line[512];
                int len = snprintf(line, sizeof(line), "[loopStall] frozen %.0fms rip=%s+0x%llx callers:",
                                   stalledUs / 1000.0,
                                   (rip >= imageLow && rip < imageHigh) ? "steep" : "dll",
                                   static_cast<unsigned long long>(rip >= imageLow && rip < imageHigh ? rip - imageLow : rip));
                int found = 0;

                for (size_t off = 0; off + sizeof(DWORD64) <= copied && found < 12 && len > 0 && len < static_cast<int>(sizeof(line)) - 24; off += sizeof(DWORD64)) {
                    DWORD64 value;
                    memcpy(&value, stackCopy + off, sizeof(value));

                    if (value > imageLow + 0x1000 && value < imageHigh) {
                        len += snprintf(line + len, sizeof(line) - len, " +0x%llx",
                                        static_cast<unsigned long long>(value - imageLow));
                        ++found;
                    }
                }

                fileBrowserPerfLog("%s\n", line);
            }
        }).detach();
    }
};
#endif

// Times the stages of one browser⇄editor view switch. Lines land in the same
// steep-fileSel.log as the editorOpen/fileSel traces so a switch and the work
// it triggers can be read together.
class ViewSwitchTrace
{
public:
    explicit ViewSwitchTrace(const char* direction)
        : on_(viewSwitchTraceOn()),
          direction_(direction),
          start_(on_ ? g_get_monotonic_time() : 0),
          last_(start_)
    {
    }

    void step(const char* name)
    {
        if (!on_) {
            return;
        }

        const gint64 now = g_get_monotonic_time();
        fileBrowserPerfLog("[viewSwitch] %s %s %.1fms (t+%.1fms)\n",
                           direction_, name,
                           (now - last_) / 1000.0,
                           (now - start_) / 1000.0);
        last_ = now;
    }

    // Logs the handler total, then two async markers: "painted" on the target
    // page's first draw (what the user actually sees) and "settled" once the
    // low-priority idle queue drains (how congested the main loop is).
    void finish(Gtk::Widget* paintTarget)
    {
        if (!on_) {
            return;
        }

        const gint64 now = g_get_monotonic_time();
        fileBrowserPerfLog("[viewSwitch] %s handler done (t+%.1fms)\n",
                           direction_, (now - start_) / 1000.0);

        const gint64 start = start_;
        const char* const direction = direction_;

        if (paintTarget) {
            auto conn = std::make_shared<sigc::connection>();
            *conn = paintTarget->signal_draw().connect_notify(
                [start, direction, conn](const Cairo::RefPtr<Cairo::Context>&) {
                    const gint64 painted = g_get_monotonic_time();
                    fileBrowserPerfLog("[viewSwitch] %s painted (t+%.1fms)\n",
                                       direction, (painted - start) / 1000.0);
                    conn->disconnect();
                });
        }

        Glib::signal_idle().connect_once([start, direction]() {
            const gint64 settled = g_get_monotonic_time();
            fileBrowserPerfLog("[viewSwitch] %s settled (t+%.1fms)\n",
                               direction, (settled - start) / 1000.0);
        }, Glib::PRIORITY_LOW);
    }

private:
    const bool on_;
    const char* const direction_;
    const gint64 start_;
    gint64 last_;
};

} // namespace

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
            options.theme = "Walnut";
            filename = Glib::build_filename(App::get().argv0(), "themes", options.theme + ".css");
        }

        // steep_* palette defaults (see themes/common/palette-defaults.css): loaded
        // BEFORE the theme at the same priority so every token resolves under any
        // theme, while the theme's own @define-color steep_* entries win (GTK
        // resolves named colors from the most recently added provider first).
        // The shared structure layer will join this chain ahead of both.
        const auto paletteDefaultsPath = Glib::build_filename(App::get().argv0(), "themes", "common", "palette-defaults.css");
        cssSteepPalette = Gtk::CssProvider::create();

        try {
            cssSteepPalette->load_from_path (paletteDefaultsPath);
            Gtk::StyleContext::add_provider_for_screen (screen, cssSteepPalette, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        } catch (Glib::Error &err) {
            printf ("Error: Can't load css file \"%s\"\nMessage: %s\n", paletteDefaultsPath.c_str(), err.what().c_str());
        } catch (...) {
            printf ("Error: Can't load css file \"%s\"\n", paletteDefaultsPath.c_str());
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

        // steep widget layer (see themes/common/widgets.css): structural rules for
        // steep's custom widgets, consolidated from what used to be inline CSS
        // providers all over rtgui. Sits ABOVE the theme (+200) exactly like the
        // inline providers it replaced, so themes cannot break widget geometry;
        // themes recolor these widgets through the steep_* palette tokens instead.
        const auto widgetsCssPath = Glib::build_filename(App::get().argv0(), "themes", "common", "widgets.css");
        cssSteepWidgets = Gtk::CssProvider::create();

        try {
            cssSteepWidgets->load_from_path (widgetsCssPath);
            Gtk::StyleContext::add_provider_for_screen (screen, cssSteepWidgets, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 200);
        } catch (Glib::Error &err) {
            printf ("Error: Can't load css file \"%s\"\nMessage: %s\n", widgetsCssPath.c_str(), err.what().c_str());
        } catch (...) {
            printf ("Error: Can't load css file \"%s\"\n", widgetsCssPath.c_str());
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
            // Steep's own face (bundled, registered in main.cc) leads; the
            // system face stays behind it in case the bundle is missing. Jost
            // sets about a point smaller than the system UI faces it replaces.
            css = Glib::ustring::compose (
                "* { font-family: Jost, %1; font-size: %2pt }",
                defaultFontFamily,
                defaultFontSize + 1);
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

        // Light themes (steep_wash = black) need dark icon line-art. Must be
        // decided before the first icon loads (FileBrowserEntry::init below).
        {
            Gdk::RGBA wash;
            if (get_style_context()->lookup_color("steep_wash", wash)) {
                RTScalable::setIconInkDark(wash.get_red() + wash.get_green() + wash.get_blue() < 1.5);
            }

            RTScalable::setIconPaletteFrom(get_style_context());
        }

        if (rtengine::settings->verbose) {
            // Gate check for the token contract: these must differ between themes
            // (and fall back to palette-defaults.css values for themes without a
            // steep palette block).
            const auto ctx = get_style_context();
            Gdk::RGBA accent, surface;
            const bool haveAccent = ctx->lookup_color("steep_accent", accent);
            const bool haveSurface = ctx->lookup_color("steep_surface_1", surface);
            printf("Theme \"%s\": steep_accent=%s steep_surface_1=%s\n",
                   options.theme.c_str(),
                   haveAccent ? accent.to_string().c_str() : "<unresolved>",
                   haveSurface ? surface.to_string().c_str() : "<unresolved>");
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

        // Fast-export settings (former right-side tab) live in the export
        // drawer now, collapsed under their own expander
        if (Gtk::Widget* fastExport = fpanel->takeFastExportSettingsWidget()) {
            bpanel->embedFastExportSettings(fastExport);
        }

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
                // Instant darken — full opacity whenever backdrop is visible.
                // This is the authoritative value: the EventBox has no
                // visible window, so its #QueueOverlayBackdrop CSS rule is
                // never applied.
                cr->set_source_rgba (0, 0, 0, 0.38);
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
                // Background from the drawer's CSS background-color, but in
                // two bands: solid behind the controls, translucent behind
                // the photo tray so the photos underneath show through.
                auto styleCtx = queueOverlayBox->get_style_context ();
                int trayY = h;

                if (BatchQueue* bq = bpanel->getBatchQueue ()) {
                    int tx = 0, ty = 0;
                    if (bq->get_realized ()
                            && bq->translate_coordinates (*queueOverlayBox, 0, 0, tx, ty)) {
                        trayY = std::min (h, std::max (0, ty));
                    }
                }

                styleCtx->render_background (cr, 0, 0, w, trayY);

                if (trayY < h) {
                    const Gdk::RGBA bg = styleCtx->get_background_color ();
                    cr->save ();
                    cr->set_source_rgba (bg.get_red (), bg.get_green (), bg.get_blue (), 0.55);
                    cr->rectangle (0, trayY, w, h - trayY);
                    cr->fill ();
                    cr->restore ();
                }

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

        // The drawer is sized from the queue's contents, so every change in
        // queue length has to re-run the overlay's child positioning.
        bpanel->setQueueSizeCallback ([this](int) {
            if (mainOverlay) {
                mainOverlay->queue_resize ();
            }
        });

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
                    // Drawer height follows the queue instead of always
                    // claiming half the window: an empty queue shows just
                    // its controls (no dark slab of blank photo area), and
                    // each row of thumbnails grows it until it reaches the
                    // 50% ceiling. Animation is still via clip in signal_draw.
                    int minH = 0, natH = 0;
                    queueOverlayBox->get_preferred_height (minH, natH);
                    int targetH = std::max (minH, 44);

                    if (BatchQueue* bq = bpanel->getBatchQueue ()) {
                        if (!bq->getEntries ().empty ()) {
                            // getEffectiveHeight already accounts for the row
                            // count at the current tile size.
                            targetH = std::max (targetH, minH + bq->getEffectiveHeight ());
                        }
                    }

                    targetH = std::min (targetH, static_cast<int> (ph * 0.50));
                    targetH = std::max (targetH, 1);

                    alloc.set_x (0);
                    alloc.set_y (0);
                    alloc.set_width (pw);
                    alloc.set_height (targetH);
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

                const int w = startupOverlay_->get_allocated_width ();
                const int h = startupOverlay_->get_allocated_height ();
                const double t = startupAnimTime_;

                // ~2.9 s on the frame clock. A starburst opens behind the
                // mark, three arches draw themselves in, a satellite rides an
                // orbit round the mark while a band of light sweeps across,
                // the wordmark rises letter by letter over a rule, and then
                // the whole composition drifts toward the viewer and fades.
                const auto clamp01 = [](double v) { return std::max(0.0, std::min(1.0, v)); };
                const auto easeOut = [](double p) { return 1.0 - std::pow(1.0 - p, 3.0); };
                const auto easeInOut = [](double p) { return p < 0.5 ? 4 * p * p * p : 1 - std::pow(-2 * p + 2, 3) / 2; };
                const auto phase = [&](double from, double to) { return clamp01((t - from) / (to - from)); };

                const double exitP = easeInOut(phase(2.35, 2.9));
                const double opacity = 1.0 - exitP;

                if (opacity <= 0.001) {
                    return false;
                }

                const Gdk::RGBA bg = themeColor(*this, "steep_surface_1", Gdk::RGBA("#14171f"));
                const Gdk::RGBA accent = themeColor(*this, "steep_accent", Gdk::RGBA("#d6a650"));
                const Gdk::RGBA ink = themeColor(*this, "steep_text_hi", Gdk::RGBA("#ccd4e0"));
                const Gdk::RGBA dim = themeColor(*this, "steep_text_dim", Gdk::RGBA("#8a92a1"));
                const auto setRgba = [&](const Gdk::RGBA& c, double a) {
                    cr->set_source_rgba(c.get_red(), c.get_green(), c.get_blue(), a);
                };

                cr->set_source_rgba(bg.get_red(), bg.get_green(), bg.get_blue(), opacity);
                cr->paint();

                const double cx = w * 0.5;
                const double cy = h * 0.5;
                const double unit = std::max(150.0, std::min(280.0, std::min(w, h) * 0.22));
                const double baseY = cy + unit * 0.55;
                const double markY = baseY - unit * 0.95;

                // A soft pool of light behind everything, brightening as the
                // mark arrives.
                {
                    const double bloom = easeOut(phase(0.3, 1.4));
                    auto pool = Cairo::RadialGradient::create(cx, markY, 0, cx, markY, unit * 1.6);
                    pool->add_color_stop_rgba(0.0, accent.get_red(), accent.get_green(), accent.get_blue(), 0.10 * bloom * opacity);
                    pool->add_color_stop_rgba(1.0, accent.get_red(), accent.get_green(), accent.get_blue(), 0.0);
                    cr->set_source(pool);
                    cr->paint();
                }

                // The exit: a slow drift toward the viewer, and a lift.
                cr->save();
                const double zoom = 1.0 + 0.05 * exitP;
                cr->translate(cx, cy - 10.0 * exitP);
                cr->scale(zoom, zoom);
                cr->translate(-cx, -cy);

                // Starburst: fine rays opening out from behind the mark,
                // turning slowly the whole time.
                {
                    const double open = easeOut(phase(0.0, 1.3));
                    const int rays = 56;
                    const double inner = unit * 0.42;
                    const double outer = inner + unit * 0.9 * open;
                    cr->set_line_width(1.0);
                    cr->set_line_cap(Cairo::LINE_CAP_ROUND);

                    for (int i = 0; i < rays; ++i) {
                        const double a = 2.0 * M_PI * i / rays + 0.12 * t;
                        const double len = (i % 2 == 0) ? 1.0 : 0.62;
                        setRgba(accent, opacity * open * (i % 2 == 0 ? 0.13 : 0.07));
                        cr->move_to(cx + std::cos(a) * inner, markY + std::sin(a) * inner);
                        cr->line_to(cx + std::cos(a) * (inner + (outer - inner) * len),
                                    markY + std::sin(a) * (inner + (outer - inner) * len));
                        cr->stroke();
                    }
                }

                // Arches: the Atelier container shape, nested.
                const auto archPath = [&](double aw, double ah) {
                    const double r = aw / 2.0;
                    cr->begin_new_path();
                    cr->move_to(cx - r, baseY);
                    cr->line_to(cx - r, baseY - (ah - r));
                    cr->arc(cx, baseY - (ah - r), r, M_PI, 2 * M_PI);
                    cr->line_to(cx + r, baseY);
                };

                for (int i = 0; i < 3; ++i) {
                    const double aw = unit * (1.25 - 0.22 * i);
                    const double ah = unit * (1.55 - 0.22 * i);
                    const double r = aw / 2.0;
                    const double length = 2.0 * (ah - r) + M_PI * r;
                    const double drawn = easeInOut(phase(0.15 + 0.18 * i, 1.15 + 0.18 * i));

                    if (drawn <= 0.0) {
                        continue;
                    }

                    archPath(aw, ah);
                    const std::vector<double> dash = {length * drawn, length};
                    cr->set_dash(dash, 0.0);
                    cr->set_line_width(i == 0 ? 1.8 : 1.1);
                    cr->set_line_cap(Cairo::LINE_CAP_ROUND);
                    setRgba(accent, opacity * (0.9 - 0.25 * i));
                    cr->stroke();
                    cr->unset_dash();

                    // A bright bead running at the tip of the line as it draws.
                    if (drawn < 1.0) {
                        archPath(aw, ah);
                        const std::vector<double> bead = {0.0, length * drawn, 0.001, length * 2};
                        cr->set_dash(bead, 0.0);
                        cr->set_line_width(i == 0 ? 5.0 : 3.5);
                        setRgba(accent, opacity * 0.9 * std::sin(drawn * M_PI));
                        cr->stroke();
                        cr->unset_dash();
                    }
                }

                // Band of light sweeping left to right inside the outer arch.
                {
                    const double sweep = easeInOut(phase(0.8, 2.0));

                    if (sweep > 0.0 && sweep < 1.0) {
                        const double sx = cx - unit * 1.1 + sweep * unit * 2.2;
                        auto light = Cairo::LinearGradient::create(sx - unit * 0.4, 0, sx + unit * 0.4, 0);
                        light->add_color_stop_rgba(0.0, accent.get_red(), accent.get_green(), accent.get_blue(), 0.0);
                        light->add_color_stop_rgba(0.5, accent.get_red(), accent.get_green(), accent.get_blue(),
                                                   0.12 * opacity * std::sin(sweep * M_PI));
                        light->add_color_stop_rgba(1.0, accent.get_red(), accent.get_green(), accent.get_blue(), 0.0);
                        archPath(unit * 1.25, unit * 1.55);
                        cr->close_path();
                        cr->set_source(light);
                        cr->fill();
                    }
                }

                // Orbit: a thin ellipse round the mark with a satellite on it.
                {
                    const double ring = easeOut(phase(0.55, 1.35));

                    if (ring > 0.0) {
                        const double rx = unit * 0.46, ry = unit * 0.16;
                        const double tilt = -0.32;
                        cr->save();
                        cr->translate(cx, markY);
                        cr->rotate(tilt);
                        cr->scale(rx, ry);
                        cr->begin_new_path();
                        cr->arc(0, 0, 1.0, 0, 2 * M_PI * ring);
                        cr->restore();
                        cr->set_line_width(0.9);
                        setRgba(dim, opacity * 0.55);
                        cr->stroke();

                        const double a = -M_PI * 0.5 + 2.0 * M_PI * easeInOut(phase(0.7, 2.3)) * 1.25;
                        const double ex = std::cos(a) * rx, ey = std::sin(a) * ry;
                        const double px = cx + ex * std::cos(tilt) - ey * std::sin(tilt);
                        const double py = markY + ex * std::sin(tilt) + ey * std::cos(tilt);
                        auto glow = Cairo::RadialGradient::create(px, py, 0, px, py, 9);
                        glow->add_color_stop_rgba(0.0, accent.get_red(), accent.get_green(), accent.get_blue(), opacity * ring * 0.9);
                        glow->add_color_stop_rgba(1.0, accent.get_red(), accent.get_green(), accent.get_blue(), 0.0);
                        cr->set_source(glow);
                        cr->arc(px, py, 9, 0, 2 * M_PI);
                        cr->fill();
                        setRgba(ink, opacity * ring);
                        cr->arc(px, py, 2.2, 0, 2 * M_PI);
                        cr->fill();
                    }
                }

                // The mark: fades in and settles from slightly large.
                const double markP = easeOut(phase(0.5, 1.4));

                if (logoSurface_ && markP > 0.0) {
                    const double logoW = logoSurface_->get_width();
                    const double logoH = logoSurface_->get_height();
                    const double target = unit * 0.5 * (1.1 - 0.1 * markP);
                    const double s = target / std::max(logoW, logoH);
                    cr->save();
                    cr->translate(cx - logoW * s * 0.5, markY - logoH * s * 0.5 + 8.0 * (1.0 - markP));
                    cr->scale(s, s);
                    cr->set_source(logoSurface_, 0, 0);
                    cr->paint_with_alpha(opacity * markP);
                    cr->restore();
                }

                // Wordmark: each letter rises into place in turn while the
                // tracking closes in.
                const double wordP = easeOut(phase(0.95, 1.85));

                if (wordP > 0.0) {
                    static const char* const letters[] = {"S", "T", "E", "E", "P"};
                    Pango::FontDescription font;
                    font.set_family("Michroma, Jost, sans-serif");
                    font.set_size(static_cast<int>(unit * 0.11) * PANGO_SCALE);
                    const double tracking = unit * (0.09 + 0.10 * (1.0 - wordP));
                    std::vector<Glib::RefPtr<Pango::Layout>> layouts;
                    double total = 0.0;
                    int th = 0;

                    for (const char* letter : letters) {
                        auto layout = Pango::Layout::create(cr);
                        layout->set_font_description(font);
                        layout->set_text(letter);
                        int lw, lh;
                        layout->get_pixel_size(lw, lh);
                        th = std::max(th, lh);
                        total += lw;
                        layouts.push_back(layout);
                    }

                    total += tracking * (layouts.size() - 1);
                    double x = cx - total * 0.5;
                    const double textY = baseY + unit * 0.12;

                    for (size_t i = 0; i < layouts.size(); ++i) {
                        const double lp = easeOut(phase(0.95 + 0.09 * i, 1.55 + 0.09 * i));
                        int lw, lh;
                        layouts[i]->get_pixel_size(lw, lh);
                        cr->move_to(x, textY + 10.0 * (1.0 - lp));
                        setRgba(ink, opacity * lp);
                        layouts[i]->show_in_cairo_context(cr);
                        x += lw + tracking;
                    }

                    // A fine rule under the wordmark, drawn out from the
                    // centre, with a small diamond at each end.
                    const double ruleP = easeInOut(phase(1.35, 2.0));

                    if (ruleP > 0.0) {
                        const double rule = unit * 0.55 * ruleP;
                        const double ry = std::round(textY + th + 12) + 0.5;
                        setRgba(dim, opacity * 0.7);
                        cr->set_line_width(1.0);
                        cr->move_to(cx - rule, ry);
                        cr->line_to(cx + rule, ry);
                        cr->stroke();

                        for (const double ex : {cx - rule, cx + rule}) {
                            const double d = 3.0 * ruleP;
                            setRgba(accent, opacity * ruleP);
                            cr->move_to(ex, ry - d);
                            cr->line_to(ex + d, ry);
                            cr->line_to(ex, ry + d);
                            cr->line_to(ex - d, ry);
                            cr->close_path();
                            cr->fill();
                        }
                    }
                }

                cr->restore();

                return true;
            }, false);

        mainOverlay->add_overlay (*startupOverlay_);

        // Prevent window's show_all() from showing these; we manage visibility manually
        queueBackdrop->set_no_show_all (true);
        queueOverlayBox->set_no_show_all (true);

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

        // Two columns under a header naming the app: what Steep itself
        // offers on the left, how the open photo is shown on the right.
        optionsBox->set_spacing (10);
        optionsBox->set_margin_top (12);
        optionsBox->set_margin_bottom (12);
        optionsBox->set_margin_start (14);
        optionsBox->set_margin_end (14);

        const auto heading = [] (const Glib::ustring& text) {
            Gtk::Label* l = Gtk::manage (new Gtk::Label (text));
            l->set_halign (Gtk::ALIGN_START);
            l->get_style_context()->add_class ("options-heading");
            return l;
        };

        Gtk::Box* menuHeader = Gtk::manage (new Gtk::Box (Gtk::ORIENTATION_HORIZONTAL, 10));
        menuHeader->set_name ("OptionsHeader");
        menuHeader->pack_start (*Gtk::manage (new RTImage ("steep-logo", Gtk::ICON_SIZE_LARGE_TOOLBAR)), false, false);
        Gtk::Label* wordmark = Gtk::manage (new Gtk::Label ("STEEP"));
        wordmark->get_style_context()->add_class ("options-wordmark");
        menuHeader->pack_start (*wordmark, false, false);
        Gtk::Label* versionLabel = Gtk::manage (new Gtk::Label (App::VERSION));
        versionLabel->get_style_context()->add_class ("dim-label");
        versionLabel->get_style_context()->add_class ("options-version");
        versionLabel->set_valign (Gtk::ALIGN_BASELINE);
        menuHeader->pack_end (*versionLabel, false, false);

        Gtk::Box* menuCols = Gtk::manage (new Gtk::Box (Gtk::ORIENTATION_HORIZONTAL, 16));
        Gtk::Box* appCol = Gtk::manage (new Gtk::Box (Gtk::ORIENTATION_VERTICAL, 1));
        Gtk::Box* viewCol = Gtk::manage (new Gtk::Box (Gtk::ORIENTATION_VERTICAL, 6));
        viewCol->set_size_request (300, -1);

        // The MCP entry, drawn like its neighbours.
        Gtk::Box* mcpRow = Gtk::manage (new Gtk::Box (Gtk::ORIENTATION_HORIZONTAL, 8));
        mcpRow->pack_start (*Gtk::manage (new RTImage ("atom", Gtk::ICON_SIZE_LARGE_TOOLBAR)), false, false);
        mcpRow->pack_start (*Gtk::manage (new Gtk::Label (M ("MCP_MENU_BUTTON"))), false, false);
        Gtk::Button* menuMcpBtn = Gtk::manage (new Gtk::Button ());
        menuMcpBtn->add (*mcpRow);
        menuMcpBtn->set_relief (Gtk::RELIEF_NONE);
        menuMcpBtn->signal_clicked().connect ([this]() {
            optionsBtn->get_popover()->hide();
            showMcpDialog();
        });

        for (Gtk::Button* b : {menuPrefsBtn, menuHelpBtn, menuIccBtn, menuMcpBtn, menuNavBtn, menuHistBtn}) {
            b->get_style_context()->add_class ("options-item");
        }

        appCol->pack_start (*menuPrefsBtn, false, false);
        appCol->pack_start (*menuHelpBtn, false, false);
        appCol->pack_start (*menuIccBtn, false, false);
        appCol->pack_start (*menuMcpBtn, false, false);
        Gtk::Label* windowsHeading = heading (M ("MAIN_MENU_SEC_WINDOWS"));
        windowsHeading->set_margin_top (10);
        appCol->pack_start (*windowsHeading, false, false);
        appCol->pack_start (*menuNavBtn, false, false);
        appCol->pack_start (*menuHistBtn, false, false);

        // Background: a segmented choice; the combo stays as its model, so
        // the sync on show and the handler above are untouched.
        Gtk::Box* bgSeg = Gtk::manage (new Gtk::Box (Gtk::ORIENTATION_HORIZONTAL, 0));
        makeSegmented (*bgSeg);
        {
            Gtk::RadioButton::Group bgGroup;
            const char* bgKeys[] = {"MAIN_MENU_PREVIEW_BG_THEME", "MAIN_MENU_PREVIEW_BG_BLACK",
                                    "MAIN_MENU_PREVIEW_BG_GREY", "MAIN_MENU_PREVIEW_BG_WHITE"};
            auto radios = std::make_shared<std::vector<Gtk::RadioButton*>> ();

            for (int i = 0; i < 4; ++i) {
                Gtk::RadioButton* r = Gtk::manage (new Gtk::RadioButton (bgGroup, M (bgKeys[i])));
                r->set_mode (false);
                r->signal_toggled().connect ([this, r, i]() {
                    if (r->get_active() && bgColorCombo->get_active_row_number() != i) {
                        bgColorCombo->set_active (i);
                    }
                });
                bgSeg->pack_start (*r, true, true);
                radios->push_back (r);
            }

            bgColorCombo->signal_changed().connect ([this, radios]() {
                const int i = bgColorCombo->get_active_row_number();

                if (i >= 0 && i < static_cast<int>(radios->size()) && !(*radios)[i]->get_active()) {
                    (*radios)[i]->set_active (true);
                }
            });
        }

        viewCol->pack_start (*heading (M ("MAIN_MENU_PREVIEW_BG")), false, false);
        viewCol->pack_start (*bgSeg, false, false);

        // Overlays as toggles, two by two.
        Gtk::Grid* overlayGrid = Gtk::manage (new Gtk::Grid ());
        overlayGrid->set_column_homogeneous (true);
        overlayGrid->set_row_spacing (4);
        overlayGrid->set_column_spacing (4);
        {
            Gtk::CheckButton* toggles[] = {chkClippedHighlights, chkClippedShadows, chkFocusMask, chkSharpMask};

            for (int i = 0; i < 4; ++i) {
                toggles[i]->set_mode (false);
                toggles[i]->get_style_context()->add_class ("options-toggle");
                overlayGrid->attach (*toggles[i], i % 2, i / 2, 1, 1);
            }
        }
        viewCol->pack_start (*heading (M ("MAIN_MENU_SEC_OVERLAYS")), false, false);
        viewCol->pack_start (*overlayGrid, false, false);

        Gtk::Box* channelSeg = Gtk::manage (new Gtk::Box (Gtk::ORIENTATION_HORIZONTAL, 0));
        makeSegmented (*channelSeg);

        for (Gtk::CheckButton* c : {chkPreviewR, chkPreviewG, chkPreviewB, chkPreviewL}) {
            c->set_mode (false);
            channelSeg->pack_start (*c, true, true);
        }

        viewCol->pack_start (*heading (M ("MAIN_MENU_PREVIEW_CHANNEL")), false, false);
        viewCol->pack_start (*channelSeg, false, false);
        viewCol->pack_start (*chkHistogramProfile, false, false);

        viewCol->pack_start (*heading (M ("MAIN_MENU_SEC_COLOUR")), false, false);
        intentRow->set_margin_start (0);
        intentRow->set_margin_end (0);
        profileRow->set_margin_start (0);
        profileRow->set_margin_end (0);
        intentCombo->set_hexpand (true);
        profileCombo->set_hexpand (true);
        intentRow->child_property_pack_type (*intentCombo) = Gtk::PACK_START;
        intentRow->child_property_expand (*intentCombo) = true;
        intentRow->child_property_fill (*intentCombo) = true;
        profileRow->child_property_pack_type (*profileCombo) = Gtk::PACK_START;
        profileRow->child_property_expand (*profileCombo) = true;
        profileRow->child_property_fill (*profileCombo) = true;
        intentLabel->set_size_request (110, -1);
        intentLabel->set_xalign (0.f);
        profileLabel->set_size_request (110, -1);
        profileLabel->set_xalign (0.f);
        viewCol->pack_start (*intentRow, false, false);
#if !defined(__APPLE__)
        viewCol->pack_start (*profileRow, false, false);
#endif
        Gtk::Box* proofRow = Gtk::manage (new Gtk::Box (Gtk::ORIENTATION_HORIZONTAL, 12));
        proofRow->pack_start (*chkSoftProof, false, false);
        proofRow->pack_start (*chkGamutCheck, false, false);
        viewCol->pack_start (*proofRow, false, false);

        menuCols->pack_start (*appCol, false, false);
        menuCols->pack_start (*Gtk::manage (new Gtk::Separator (Gtk::ORIENTATION_VERTICAL)), false, false);
        menuCols->pack_start (*viewCol, true, true);

        optionsBox->pack_start (*menuHeader, false, false);
        optionsBox->pack_start (*Gtk::manage (new Gtk::Separator (Gtk::ORIENTATION_HORIZONTAL)), false, false);
        optionsBox->pack_start (*menuCols, false, false);

        // The separators the single column used are not part of this layout.
        for (Gtk::Separator* s : {menuSep1, menuSep2, menuSep3, menuSep4, menuSep5}) {
            delete s;
        }

        delete previewChannelLabel;

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

#ifndef _WIN32
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
#endif

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

#ifndef _WIN32
        headerBar->pack_end (*btn_close);
        headerBar->pack_end (*btn_fullscreen);
        headerBar->pack_end (*btn_minimize);
#endif

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

    // Managed editor panels can outlive this destructor body. Detach them
    // before mcpServer_ is destroyed so EditorPanel::close() cannot call back
    // through a partially destroyed RTWindow.
    if (mcpServer_) {
        mcpServer_->setEditorPanel(nullptr);
        mcpServer_->stop();
    }
    if (epanel) {
        epanel->setParent(nullptr);
    }
    for (const auto& entry : epanels) {
        if (entry.second) {
            entry.second->setParent(nullptr);
        }
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

        // Driven by the frame clock, not a 16 ms timer: one draw per
        // display refresh, so the motion never stutters against vsync.
        startupOverlay_->add_tick_callback(
            [this, startTime](const Glib::RefPtr<Gdk::FrameClock>&) -> bool {
                startupAnimTime_ = startTime->elapsed();

                if (startupAnimTime_ > 2.95) {
                    startupAnimActive_ = false;
                    startupOverlay_->hide ();
                    startupOverlay_->set_no_show_all (true);
                    return false;
                }

                startupOverlay_->queue_draw ();
                return true;
            });
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

    // Auto-start MCP server if configured. STEEP_MCP_AUTOSTART=1 starts it for this session only
    // (scripts), and STEEP_MCP_PORT pins the port so they can find it.
    const char* const mcpEnvStart = g_getenv("STEEP_MCP_AUTOSTART");
    const bool mcpStartFromEnv = mcpEnvStart && std::string(mcpEnvStart) == "1";
    if ((options.mcpAutoStart || mcpStartFromEnv) && mcpServer_ && !mcpServer_->isRunning()) {
        const char* const mcpEnvPort = g_getenv("STEEP_MCP_PORT");
        mcpServer_->start(mcpEnvPort ? std::atoi(mcpEnvPort) : 0);
    }

    // Main-loop stall probe: a 25ms default-priority heartbeat. A gap beyond
    // the threshold (100ms, STEEP_LOOPSTALL_MS) means the GUI thread could not
    // even run timeouts — input and painting were frozen for that long. On
    // Windows a watchdog also samples where the GUI thread was stuck. Inert
    // unless the perf log is on.
    if (viewSwitchTraceOn()) {
        auto lastTick = std::make_shared<std::atomic<gint64>>(g_get_monotonic_time());
        Glib::signal_timeout().connect([lastTick]() -> bool {
            const gint64 now = g_get_monotonic_time();
            const double gapMs = (now - lastTick->load(std::memory_order_relaxed)) / 1000.0;
            if (gapMs > loopStallThresholdMs()) {
                fileBrowserPerfLog("[loopStall] gap=%.0fms\n", gapMs);
            }
            lastTick->store(now, std::memory_order_release);
            return true;
        }, 25);
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
        LoopStallSampler::start(lastTick);
#endif
    }

    // Debug rig, inert unless STEEP_SWITCH_SELFTEST is set: after startup has
    // settled, flip browser⇄editor that many times so [viewSwitch] latency can
    // be measured hands-free. Nothing gets selected, matching the empty-editor
    // switch the trace is after. STEEP_SWITCH_SELFTEST_DELAY_MS/_PERIOD_MS
    // move the flips relative to startup (e.g. into a cold folder load).
    if (const char* const cyclesEnv = std::getenv("STEEP_SWITCH_SELFTEST")) {
        if (isSingleTabMode() && epanel && fpanel && mainNB) {
            const int cycles = std::max(1, std::atoi(cyclesEnv));
            const char* const delayEnv = std::getenv("STEEP_SWITCH_SELFTEST_DELAY_MS");
            const char* const periodEnv = std::getenv("STEEP_SWITCH_SELFTEST_PERIOD_MS");
            const unsigned int delayMs = delayEnv ? std::max(1, std::atoi(delayEnv)) : 15000;
            const unsigned int periodMs = periodEnv ? std::max(100, std::atoi(periodEnv)) : 2500;
            Glib::signal_timeout().connect_once([this, cycles, periodMs]() {
                fileBrowserPerfLog("[viewSwitch] selftest start cycles=%d period=%ums\n",
                                   cycles, periodMs);
                auto flip = std::make_shared<int>(0);
                const bool advance = std::getenv("STEEP_SWITCH_SELFTEST_ADVANCE") != nullptr;
                Glib::signal_timeout().connect([this, flip, cycles, advance]() -> bool {
                    // Phase A: empty editor, nothing selected. Phase B: same
                    // flips with the first image selected — entering the
                    // editor auto-opens it, matching a real editing session.
                    // With ADVANCE set, phase B steps to the next image before
                    // every editor flip so each cycle opens a fresh file.
                    const int phaseFlips = cycles * 2;
                    if (*flip == phaseFlips
                        && fpanel->fileCatalog && fpanel->fileCatalog->fileBrowser) {
                        fpanel->fileCatalog->fileBrowser->selectFirst(false);
                        fileBrowserPerfLog("[viewSwitch] selftest selectFirst\n");
                    } else if (advance && *flip > phaseFlips && (*flip % 2) == 0
                               && fpanel->fileCatalog && fpanel->fileCatalog->fileBrowser) {
                        fpanel->fileCatalog->fileBrowser->selectNext(1, false);
                        fileBrowserPerfLog("[viewSwitch] selftest selectNext\n");
                    }
                    const bool toEditor = (*flip % 2) == 0;
                    fileBrowserPerfLog("[viewSwitch] selftest flip %d -> %s\n",
                                       *flip, toEditor ? "editor" : "browser");
                    Gtk::Widget& page = toEditor ? static_cast<Gtk::Widget&>(*epanel)
                                                 : static_cast<Gtk::Widget&>(*fpanel);
                    mainNB->set_current_page(mainNB->page_num(page));

                    // With EDIT set, dirty the exposure a beat after each
                    // phase-B editor flip so the switch back exercises the
                    // background sidecar save with a real change.
                    if (toEditor && *flip >= phaseFlips
                        && std::getenv("STEEP_SWITCH_SELFTEST_EDIT")) {
                        Glib::signal_timeout().connect_once([this]() {
                            if (epanel) {
                                fileBrowserPerfLog("[viewSwitch] selftest nudge exposure\n");
                                epanel->debugNudgeExposure();
                            }
                        }, 4000);
                    }
                    ++*flip;
                    if (*flip >= phaseFlips * 2) {
                        fileBrowserPerfLog("[viewSwitch] selftest done\n");
                        return false;
                    }
                    return true;
                }, periodMs);
            }, delayMs);
        }
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
        const bool toEditor = isEditorPanel (page_num);
        ViewSwitchTrace trace (toEditor ? "->editor" : "->browser");

        if (toEditor) {
            if (isSingleTabMode() && epanel) {
                MoveFileBrowserToEditor();
                trace.step ("moveFileBrowserToEditor");
            }

            EditorPanel *ep = static_cast<EditorPanel*> (mainNB->get_nth_page (page_num));
            ep->setAspect();
            trace.step ("setAspect");

            if (!isSingleTabMode()) {
                if (filesEdited.size() > 0) {
                    set_title_decorated (ep->getFileName());
                }
            }

            // Auto-open the browser's selected photo in the editor.
            if (isSingleTabMode()
                && fpanel
                && !suppressEditorSwitchAutoOpen_) {
                fpanel->openSelectedInEditor();
                trace.step ("openSelectedInEditor");
            }

            // Edit view opens with the left sidebar collapsed by default,
            // but if the user manually expanded it last time they were in
            // the editor, that choice sticks across view switches.
            if (isSingleTabMode() && epanel) {
                if (App::get().options().editorShowLeftSidebar) {
                    epanel->setLeftPanelVisible(true);
                } else {
                    epanel->collapseLeftSidebarForEdit();
                }
                trace.step ("leftSidebarState");
            }

            // Collapse filter bar to prevent overlap with filmstrip
            ep->collapseFilterBar();
            trace.step ("collapseFilterBar");

            // Animate editor panels in (sidebar slides from right, filmstrip from top)
            if (isSingleTabMode() && epanel) {
                epanel->animateEditorIn();
                trace.step ("animateEditorIn");
            }
        } else {
            // in single tab mode with command line filename epanel does not exist yet
            if (isSingleTabMode() && epanel) {
                // Save profile on leaving the editor panel. The disk writes
                // run on the cleanup executor: a spun-down HDD used to hold
                // the whole view switch hostage for its spin-up here.
                epanel->saveProfileAsync();
                trace.step ("saveProfile");

                // Sync left panel state: editor → browser
                if (fpanel) {
                    fpanel->setLeftPanelVisible(epanel->isLeftPanelVisible());
                    trace.step ("leftSidebarState");
                }

                // Moving the FileBrowser only if the user has switched to the FileBrowser tab
                if (mainNB->get_nth_page (page_num) == fpanel) {
                    MoveFileBrowserToMain();
                    trace.step ("moveFileBrowserToMain");
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
        trace.step ("closeAlbumView");

        // Both views own a directory tree. Mirror the catalog's final active
        // directory after all transition state (including album exit) settles.
        if (isSingleTabMode() && fpanel && fpanel->fileCatalog) {
            const Glib::ustring directory = fpanel->fileCatalog->lastSelectedDir();
            if (isEditorPanel(page_num) && epanel) {
                epanel->syncDirectoryHighlight(directory);
            } else if (mainNB->get_nth_page(page_num) == fpanel) {
                fpanel->syncDirectoryHighlight(directory);
            }
            trace.step ("syncDirectoryHighlight");
        }

        // Keep header bar nav buttons in sync with notebook page
        syncNavButtons (page_num);
        trace.step ("syncNavButtons");
        trace.finish(mainNB ? mainNB->get_nth_page (page_num) : nullptr);
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
        // A view switch may have left its sidecar write on the cleanup
        // executor; finish it before teardown so the file cannot be torn.
        EditorPanel::drainBackgroundSaves();
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

EditorPanel* RTWindow::getEditorPanelForFile(const Glib::ustring& filename)
{
    const std::string key = editorFileKey(filename);
    auto matches = [&key](EditorPanel* panel) {
        return panel && editorFileKey(panel->getFileName()) == key;
    };

    if (matches(epanel)) {
        return epanel;
    }

    for (const auto& entry : epanels) {
        if (matches(entry.second)) {
            return entry.second;
        }
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

    // Setting-row text size can also be changed from Preferences, so push it
    // into the live Adjusters the same way the right-click slider does.
    Adjuster::setPillScale(options.adjusterPillScale);

    // Same for the folder name above the filmstrip, and its top/bottom
    // placement, in every open editor.
    if (epanel) {
        epanel->refreshEditorTitleVisibility();
        epanel->applyFilmstripPlacement();
    }

    for (const auto& p : epanels) {
        p.second->refreshEditorTitleVisibility();
        p.second->applyFilmstripPlacement();
    }
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
        // Nothing resets the text between jobs, so without this a later
        // re-show (e.g. a thumbnail progress tick) surfaces the previous
        // job's label — users saw a stale "Decoding..." during unrelated
        // background work. Every shower sets its own text.
        prProgBar.set_text("");
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
            mainNB->set_current_page (mainNB->page_num (*fpanel));
        } else if (active == navEditor) {
            // In single-tab mode, switch to epanel; in multi-tab, switch to last editor
            if (isSingleTabMode() && epanel) {
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
        ViewSwitchTrace trace ("->browser.move");
        FileCatalog *fCatalog = fpanel->fileCatalog;
        epanel->catalogPane->remove (*fCatalog);
        fpanel->ribbonPane->add (*fCatalog);
        trace.step ("reparent");
        fCatalog->enableTabMode (false);
        trace.step ("enableTabMode(false)");
        // ...and the browser picks up whatever the filmstrip was filtered by.
        fCatalog->setFilterToggles (epanel->getFilterToggles ());
        fCatalog->tbLeftPanel_1_visible (false);  // Left toggle now in FilePanel footer
        fCatalog->tbRightPanel_1_visible (false); // Right sidebar retired in browser view

        // The filmstrip's filter must not linger in browser view — restore
        // the catalog toolbar's filter and resume any paused preview loading.
        fCatalog->reapplyBrowserFilter ();
        trace.step ("reapplyBrowserFilter");

        // Browser view keeps its left sidebar visible by default
        fpanel->setLeftPanelVisible (true);
        trace.step ("setLeftPanelVisible");

        // Center the browser on the image that was selected in the filmstrip.
        // Deferred to a low-priority idle so the re-parented browser has been
        // allocated and re-arranged first — entry positions are only valid
        // after that.
        if (epanel) {
            const Glib::ustring current = epanel->getFileName();
            if (!current.empty()) {
                Glib::signal_idle().connect_once([fCatalog, current]() {
                    if (fCatalog->fileBrowser) {
                        fCatalog->fileBrowser->selectImage(current, true);
                    }
                }, Glib::PRIORITY_LOW);
            }
        }
    }
}

void RTWindow::MoveFileBrowserToEditor()
{
    if (epanel->catalogPane->get_children().empty() ) {
        ViewSwitchTrace trace ("->editor.move");
        FileCatalog *fCatalog = fpanel->fileCatalog;
        fpanel->ribbonPane->remove (*fCatalog);
        fCatalog->disableInspector();
        epanel->catalogPane->add (*fCatalog);
        trace.step ("reparent");
        epanel->showTopPanel (App::get().options().editorFilmStripOpened);
        trace.step ("showTopPanel");
        fCatalog->enableTabMode (true);
        trace.step ("enableTabMode(true)");
        // The filmstrip starts from whatever the browser was filtering by.
        epanel->setFilterToggles (fCatalog->getFilterToggles ());
        epanel->restoreEditorFilter();
        trace.step ("restoreEditorFilter");
        fCatalog->refreshHeight();
        trace.step ("refreshHeight");
        fCatalog->tbLeftPanel_1_visible (false);
        fCatalog->tbRightPanel_1_visible (false);

        // The left sidebar state is applied once by on_mainNB_switch_page after
        // this returns. Collapsing it here as well ran the 200 ms slide (and
        // the browser re-allocation it drags along) twice per switch.
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
    // A real window title: shown by the native Windows titlebar (which
    // otherwise falls back to "steep.exe"), the taskbar, and alt-tab.
    // The in-window header bar shows navigation, not text — its title
    // stays empty.
    Glib::ustring title = "Steep";

    if (!fname.empty()) {
        title = Glib::path_get_basename(fname) + " — Steep";
    }

    if (get_title() != title) {
        set_title (title);
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
