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
#include "batchqueueentry.h"
#include "editorpanel.h"
#include "filepanel.h"
#include "indclippedpanel.h"
#include "previewmodepanel.h"
#include "presetlistpanel.h"
#include "profilepanel.h"
#include "tools/filmsimulation.h"

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
    , epanel (nullptr)
    , fpanel (nullptr)
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
                "* { font-family: %1; font-size: %2pt }"
                " #MyExpander * { font-size: 10px; }"
                " .MyExpanderSummary * { font-size: 10px; }",
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
                "* { font-family: %1; font-size: %2pt }"
                " #MyExpander * { font-size: 10px; }"
                " .MyExpanderSummary * { font-size: 10px; }",
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

        // Compact UI overrides at high priority to ensure they win over theme CSS
        {
            auto cssCompact = Gtk::CssProvider::create();
            Glib::ustring compactCSS =
                "#MyExpander * { font-size: 10px; min-height: 0; min-width: 0; }\n"
                "#MyExpander button,"
                " .MyExpanderSummary button {"
                "   min-height: 0; min-width: 0; padding: 0 4px; margin: 0; }\n"
                "#MyExpander .text-button,"
                " #MyExpander .image-button,"
                " #MyExpander .independent,"
                " .MyExpanderSummary .text-button,"
                " .MyExpanderSummary .image-button,"
                " .MyExpanderSummary .independent {"
                "   min-height: 0; min-width: 0; padding: 0 4px; margin: 0; }\n"
                "#MyExpander button label,"
                " .MyExpanderSummary button label {"
                "   min-height: 0; margin: 0; padding: 0; }\n"
                "#MyExpander button.combo,"
                " .MyExpanderSummary button.combo {"
                "   min-height: 0; min-width: 0;"
                "   padding: 0 2px; margin: 0; }\n"
                "#MyExpander combobox,"
                " .MyExpanderSummary combobox {"
                "   min-height: 0; margin: 0; padding: 0; }\n"
                "#MyExpander combobox cellview,"
                " .MyExpanderSummary combobox cellview {"
                "   min-height: 0; padding: 0; margin: 0; }\n"
                "#MyExpander entry,"
                " .MyExpanderSummary entry {"
                "   min-height: 0; padding: 0; margin: 0; }\n"
                ".MyExpanderSummary * { font-size: 10px; min-height: 0; min-width: 0; }\n";
            try {
                cssCompact->load_from_data(compactCSS);
                Gtk::StyleContext::add_provider_for_screen(
                    screen, cssCompact, GTK_STYLE_PROVIDER_PRIORITY_USER + 200);
            } catch (Glib::Error &err) {
                printf("Compact CSS error: %s\n", err.what().c_str());
            } catch (...) {
                printf("Compact CSS unknown error\n");
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
    versionStr = "RawTherapee " + App::VERSION;

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

        // Batch Queue panel (tab 1)
        bpanel = Gtk::manage ( new BatchQueuePanel (fpanel->fileCatalog) );
        mainNB->append_page (*bpanel);

        if (isSingleTabMode()) {
            createSetmEditor();
        }

        mainNB->set_current_page (mainNB->page_num (*fpanel));

        // ===== Header Bar (replaces left sidebar + action grid) =====
        headerBar = Gtk::manage (new Gtk::HeaderBar ());
        headerBar->set_name ("RTHeaderBar");
        headerBar->set_show_close_button (false);
        headerBar->set_has_subtitle (true);

        // -- Options dropdown menu (left side, before nav buttons) --
        optionsBtn = Gtk::manage (new Gtk::MenuButton ());
        optionsBtn->set_name ("OptionsMenuButton");
        optionsBtn->set_relief (Gtk::RELIEF_NONE);
        optionsBtn->set_image (*Gtk::manage (new RTImage ("options-menu", Gtk::ICON_SIZE_LARGE_TOOLBAR)));
        optionsBtn->set_tooltip_markup (M ("MAIN_BUTTON_PREFERENCES"));

        Gtk::Popover* optionsPopover = Gtk::manage (new Gtk::Popover ());
        optionsPopover->set_name ("OptionsPopover");
        Gtk::Box* optionsBox = Gtk::manage (new Gtk::Box (Gtk::ORIENTATION_VERTICAL, 2));
        optionsBox->set_margin_top (6);
        optionsBox->set_margin_bottom (6);
        optionsBox->set_margin_start (2);
        optionsBox->set_margin_end (2);

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

        set_titlebar (*headerBar);

        pldBridge = new PLDBridge (static_cast<rtengine::ProgressListener*> (this));

        add (*mainNB);
        show_all ();

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
        } else {
            // in single tab mode with command line filename epanel does not exist yet
            if (isSingleTabMode() && epanel) {
                // Save profile on leaving the editor panel
                epanel->saveProfile();

                // Moving the FileBrowser only if the user has switched to the FileBrowser tab
                if (mainNB->get_nth_page (page_num) == fpanel) {
                    MoveFileBrowserToMain();
                }
            }
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
        bool queueHadFocus = (mainNB->get_current_page() == mainNB->page_num (*bpanel));
        ep->setExternalEditorChangedSignal(nullptr);
        epanels.erase (ep->getFileName());
        filesEdited.erase (ep->getFileName ());
        fpanel->refreshEditedState (filesEdited);

        mainNB->remove_page (*ep);

        if (!isEditorPanel (mainNB->get_current_page())) {
            if (!queueHadFocus) {
                mainNB->set_current_page (mainNB->page_num (*fpanel));
            }

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

    if (ctrl) {
        switch (event->keyval) {
            case GDK_KEY_F2: // file browser panel
                mainNB->set_current_page (mainNB->page_num (*fpanel));
                return true;

            case GDK_KEY_F3: // batch queue panel
                mainNB->set_current_page (mainNB->page_num (*bpanel));
                return true;

            case GDK_KEY_F4: //single tab mode, editor panel
                if (isSingleTabMode() && epanel) {
                    mainNB->set_current_page (mainNB->page_num (*epanel));
                }

                return true;

            case GDK_KEY_w: //multi-tab mode, close editor panel
                if (!isSingleTabMode() &&
                        mainNB->get_current_page() != mainNB->page_num (*fpanel) &&
                        mainNB->get_current_page() != mainNB->page_num (*bpanel)) {

                    EditorPanel* ep = static_cast<EditorPanel*> (mainNB->get_nth_page (mainNB->get_current_page()));
                    remEditorPanel (ep);
                    return true;
                }
        }
    }

    if (mainNB->get_current_page() == mainNB->page_num (*fpanel)) {
        return fpanel->handleShortcutKey (event);
    } else if (mainNB->get_current_page() == mainNB->page_num (*bpanel)) {
        return bpanel->handleShortcutKey (event);
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
    prProgBar.set_fraction(p);

    if (p > 0.0 && p < 1.0) {
        prProgBar.show();
    } else {
        prProgBar.hide();
    }
}

void RTWindow::setProgressStr(const Glib::ustring& str)
{
    if (!App::get().options().mainNBVertical) {
        prProgBar.set_text(str);
    }
}

void RTWindow::setProgressState(bool inProcessing)
{
    if (inProcessing) {
        prProgBar.show();
    } else {
        prProgBar.hide();
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

    // Enforce mutual exclusion: only one nav button active at a time
    if (active->get_active()) {
        if (active != navFileBrowser) { navFileBrowser->set_active (false); }
        if (active != navQueue)       { navQueue->set_active (false); }
        if (active != navEditor)      { navEditor->set_active (false); }

        // Switch to the corresponding notebook page
        if (active == navFileBrowser) {
            mainNB->set_current_page (mainNB->page_num (*fpanel));
        } else if (active == navQueue) {
            mainNB->set_current_page (mainNB->page_num (*bpanel));
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
    navQueue->set_active (page == bpanel);
    navEditor->set_active (page != fpanel && page != bpanel);

    navSwitching = false;
}

void RTWindow::SetEditorCurrent()
{
    mainNB->set_current_page (mainNB->page_num (*epanel));
}

void RTWindow::SetMainCurrent()
{
    mainNB->set_current_page (mainNB->page_num (*fpanel));
}

void RTWindow::MoveFileBrowserToMain()
{
    if ( fpanel->ribbonPane->get_children().empty()) {
        FileCatalog *fCatalog = fpanel->fileCatalog;
        epanel->catalogPane->remove (*fCatalog);
        fpanel->ribbonPane->add (*fCatalog);
        fCatalog->enableTabMode (false);
        fCatalog->tbLeftPanel_1_visible (true);
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
    Glib::ustring subtitle;

    if (!fname.empty()) {
        subtitle = " - " + fname;
    }

    set_title (versionStr + subtitle);

    if (headerBar) {
        headerBar->set_title (versionStr);
        headerBar->set_subtitle (fname);
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
    return (panel != bpanel) && (panel != fpanel);
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
