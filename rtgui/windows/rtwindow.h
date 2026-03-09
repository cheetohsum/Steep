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
#pragma once

#include <functional>
#include <set>
#include <vector>

#include <gtkmm.h>
#include <sigc++/signal.h>

#if defined(__APPLE__)
#include <gtkosxapplication.h>
#endif

#include "progressconnector.h"
#include "splash.h"

#include "rtengine/noncopyable.h"

#include <memory>

class BatchQueueEntry;
class BatchQueuePanel;
class EditorPanel;
struct ExternalEditor;
class FilePanel;
class PLDBridge;
class ThumbBrowserEntryBase;

namespace mcp { class McpServer; }
class RTWindow final :
    public Gtk::Window,
    public rtengine::ProgressListener,
    public rtengine::NonCopyable
{

private:
    Gtk::Notebook* mainNB;
    BatchQueuePanel* bpanel;
    std::set<Glib::ustring> filesEdited;
    std::map<Glib::ustring, EditorPanel*> epanels;

    sigc::signal<void> externalEditorChangedSignal;

    Splash* splash;
    Gtk::ProgressBar prProgBar;
    PLDBridge* pldBridge;
    bool is_fullscreen;
    bool is_minimized;
    sigc::connection onConfEventConn;
    bool on_delete_has_run;
    Gtk::Button * btn_fullscreen;
    Gtk::Button * btn_minimize;
    Gtk::Button * btn_close;

    Gtk::Image *iFullscreen, *iFullscreen_exit;

    // Header bar and navigation
    Gtk::HeaderBar* headerBar;
    Gtk::MenuButton* optionsBtn;
    Gtk::ComboBoxText* bgColorCombo;
    Gtk::CheckButton* chkFocusMask;
    Gtk::CheckButton* chkSharpMask;
    Gtk::CheckButton* chkClippedShadows;
    Gtk::CheckButton* chkClippedHighlights;
    Gtk::CheckButton* chkHistogramProfile;
    Gtk::CheckButton* chkPreviewR;
    Gtk::CheckButton* chkPreviewG;
    Gtk::CheckButton* chkPreviewB;
    Gtk::CheckButton* chkPreviewL;
    Gtk::ComboBoxText* intentCombo;
    Gtk::ComboBoxText* profileCombo;
    Gtk::CheckButton* chkSoftProof;
    Gtk::CheckButton* chkGamutCheck;
    Gtk::ToggleButton* navFileBrowser;
    Gtk::ToggleButton* navQueue;
    Gtk::ToggleButton* navEditor;
    bool navSwitching; // guard against recursive toggle

    // Queue overlay drawer
    Gtk::Overlay* mainOverlay;
    Gtk::Box* queueOverlayBox;
    Gtk::EventBox* queueBackdrop;
    bool queueOverlayVisible;
    double queueAnimFraction;        // 0.0 = hidden, 1.0 = fully shown
    sigc::connection queueAnimConn;  // animation timer

    // Browser/editor view transition animation
    sigc::connection viewAnimConn_;  // browser fade animation timer

    // Hero thumbnail transition (browser grid → filmstrip)
    struct CapturedThumb {
        Cairo::RefPtr<Cairo::ImageSurface> surface;
        double srcX, srcY, srcW, srcH;  // browser position (overlay coords)
        double dstX, dstY, dstW, dstH;  // filmstrip target (overlay coords)
        bool isHero;                      // true = animates to filmstrip
        ThumbBrowserEntryBase* entry;     // for matching after reparent
    };
    Gtk::EventBox* heroOverlay_;
    std::vector<CapturedThumb> capturedThumbs_;
    double heroAnimFraction_;
    bool heroPageSwitched_;           // true once notebook page has been switched
    bool heroAnimIn_;                 // true=browser→editor, false=editor→browser
    sigc::connection heroAnimConn_;

    // Startup animation overlay
    Gtk::EventBox* startupOverlay_;
    double startupAnimTime_;       // elapsed seconds
    bool startupAnimActive_;
    sigc::connection startupAnimConn_;
    Cairo::RefPtr<Cairo::ImageSurface> logoSurface_;

    void captureVisibleThumbnails();
    void startHeroTransition();
    void computeFilmstripTargets();
    void captureFilmstripThumbnails();
    void startReverseHeroTransition(std::function<void()> afterPageSwitch);
    void computeBrowserTargets();

    bool isSingleTabMode() const;

    bool on_expose_event_epanel (GdkEventExpose* event);
    bool on_expose_event_fpanel (GdkEventExpose* event);
    bool splashClosed (GdkEventAny* event);
    bool isEditorPanel (Widget* panel);
    bool isEditorPanel (guint pageNum);
    void showErrors ();

    Glib::ustring versionStr;
#if defined(__APPLE__)
    GtkosxApplication *osxApp;
#endif

public:
    RTWindow ();
    ~RTWindow() override;

#if defined(__APPLE__)
    bool osxFileOpenEvent (Glib::ustring path);
#endif
    void addEditorPanel (EditorPanel* ep, const std::string &name);
    void remEditorPanel (EditorPanel* ep);
    bool selectEditorPanel (const std::string &name);

    void addBatchQueueJob       (BatchQueueEntry* bqe, bool head = false);
    void addBatchQueueJobs      (const std::vector<BatchQueueEntry*>& entries);

    bool keyPressed (GdkEventKey* event);
    bool keyReleased(GdkEventKey *event);
    bool on_configure_event (GdkEventConfigure* event) override;
    bool on_delete_event (GdkEventAny* event) override;
    bool on_window_state_event (GdkEventWindowState* event) override;
    void on_mainNB_switch_page (Gtk::Widget* widget, guint page_num);

    void showRawPedia();
    void showICCProfileCreator ();
    void showPreferences ();
    void on_realize () override;
    void toggle_fullscreen ();
    void minimize_window ();
    void close_window ();
    void on_nav_switched (Gtk::ToggleButton* active);
    void syncNavButtons (guint page_num);
    void toggleQueueOverlay();
    void showQueueOverlay();
    void hideQueueOverlay();
    void get_position(int& x, int& y) const;

    void setProgress(double p) override;
    void setProgressStr(const Glib::ustring& str) override;
    void setProgressState(bool inProcessing) override;
    void error(const Glib::ustring& descr) override;

    rtengine::ProgressListener* getProgressListener ()
    {
        return pldBridge;
    }

    EditorPanel*  epanel;
    FilePanel* fpanel;
    std::unique_ptr<mcp::McpServer> mcpServer_;

    void SetEditorCurrent();
    void SetMainCurrent();
    void MoveFileBrowserToEditor();
    void MoveFileBrowserToMain();

    void updateExternalEditorWidget(int selectedIndex, const std::vector<ExternalEditor> &editors);
    void updateProfiles (const Glib::ustring &printerProfile, rtengine::RenderingIntent printerIntent, bool printerBPC);
    void updateTPVScrollbar (bool hide);
    void updateHistogramPosition (int oldPosition, int newPosition);
    void updateFBQueryTB (bool singleRow);
    void updateFBToolBarVisibility (bool showFilmStripToolBar);
    void updateShowtooltipVisibility (bool showtooltip);
    void updateToolPanelToolLocations(
        const std::vector<Glib::ustring> &favorites, bool cloneFavoriteTools);
    bool getIsFullscreen()
    {
        return is_fullscreen;
    }
    void setWindowSize ();
    void set_title_decorated (Glib::ustring fname);
    void closeOpenEditors();
    void setEditorMode (bool tabbedUI);
    void createSetmEditor();

    void writeToolExpandedStatus (std::vector<int> &tpOpen);

    EditorPanel* getActiveEditorPanel();

    void showMcpDialog();
    mcp::McpServer* getMcpServer() { return mcpServer_.get(); }
};
