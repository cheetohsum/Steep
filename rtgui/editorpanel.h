/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2004-2010 Gabor Horvath <hgabor@rawtherapee.com>
 *  Copyright (c) 2010 Oliver Duis <www.oliverduis.de>
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

#include "albumbrowser.h"
#include "browserfilter.h"
#include "extprog.h"
#include "histogrampanel.h"
#include "history.h"
#include "imageareapanel.h"
#include "presetlistpanel.h"
#include "progressconnector.h"
#include "thumbnaillistener.h"
#include "windows/saveasdlg.h"
#include "windows/navigatordialog.h"
#include "windows/historydialog.h"

#include "rtengine/noncopyable.h"
#include "rtengine/rtengine.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>

#include <gtkmm.h>

namespace rtengine
{
template<typename T>
class array2D;
}

using ExternalEditorChangedSignal = sigc::signal<void>;

class BatchQueueEntry;
class DirBrowser;
class EditorPanel;
class FilePanel;
class IndicateClippedPanel;
class MyProgressBar;
class Navigator;
class PlacesBrowser;
class PopUpButton;
class PreviewModePanel;
class RecentBrowser;
class RTAppChooserDialog;
class Thumbnail;
class ToolPanelCoordinator;

struct EditorPanelIdleHelper {
    EditorPanel* epanel;
    bool destroyed;
    int pending;
};

class RTWindow;

class EditorPanel final :
    public Gtk::Box,
    public PParamsChangeListener,
    public rtengine::ProgressListener,
    public ThumbnailListener,
    public HistoryBeforeLineListener,
    public rtengine::HistogramListener,
    public HistogramPanelListener,
    public rtengine::NonCopyable
{
public:

    explicit EditorPanel (FilePanel* filePanel = nullptr);
    ~EditorPanel () override;

    void open (Thumbnail* tmb, rtengine::InitialImage* isrc);
    void setAspect ();
    void on_realize () override;
    void leftPaneButtonReleased (GdkEventButton *event);
    void rightPaneButtonReleased (GdkEventButton *event);

    void setParent (RTWindow* p)
    {
        parent = p;
    }

    void setParentWindow (Gtk::Window* p)
    {
        parentWindow = p;

        if (p && navigator && !navigatorDialog_) {
            navigatorDialog_ = new NavigatorDialog(*p, navigator);
        }
        if (p && history && !historyDialog_) {
            historyDialog_ = new HistoryDialog(*p, history);
        }
    }

    void writeOptions();
    void writeToolExpandedStatus (std::vector<int> &tpOpen);
    void updateShowtooltipVisibility (bool showtooltip);

    void showTopPanel (bool show);
    bool isRealized()
    {
        return realized;
    }
    // ProgressListener interface
    void setProgress(double p) override;
    void setProgressStr(const Glib::ustring& str) override;
    void setProgressState(bool inProcessing) override;
    void error(const Glib::ustring& descr) override;

    void error(const Glib::ustring& title, const Glib::ustring& descr);
    void displayError(const Glib::ustring& title, const Glib::ustring& descr);  // this is called by error in the gtk thread
    void refreshProcessingState (bool inProcessing); // this is called by setProcessingState in the gtk thread

    // PParamsChangeListener interface
    void procParamsChanged(
        const rtengine::procparams::ProcParams* params,
        const rtengine::ProcEvent& ev,
        const Glib::ustring& descr,
        const ParamsEdited* paramsEdited = nullptr
    ) override;
    void clearParamChanges() override;

    // thumbnaillistener interface
    void procParamsChanged (Thumbnail* thm, int whoChangedIt, bool upgradeHint) override;

    // HistoryBeforeLineListener
    void historyBeforeLineChanged (const rtengine::procparams::ProcParams& params) override;

    // HistogramListener
    void histogramChanged(
        const LUTu& histRed,
        const LUTu& histGreen,
        const LUTu& histBlue,
        const LUTu& histLuma,
        const LUTu& histToneCurve,
        const LUTu& histLCurve,
        const LUTu& histCCurve,
        const LUTu& histLCAM,
        const LUTu& histCCAM,
        const LUTu& histRedRaw,
        const LUTu& histGreenRaw,
        const LUTu& histBlueRaw,
        const LUTu& histChroma,
        const LUTu& histLRETI,
        int vectorscopeScale,
        const array2D<int>& vectorscopeHC,
        const array2D<int>& vectorscopeHS,
        int waveformScale,
        const array2D<int>& waveformRed,
        const array2D<int>& waveformGreen,
        const array2D<int>& waveformBlue,
        const array2D<int>& waveformLuma
    ) override;
    void setObservable(rtengine::HistogramObservable* observable) override;
    bool updateHistogram(void) const override;
    bool updateHistogramRaw(void) const override;
    bool updateVectorscopeHC(void) const override;
    bool updateVectorscopeHS(void) const override;
    bool updateWaveform(void) const override;

    // HistogramPanelListener
    void scopeTypeChanged(Options::ScopeType new_type) override;

    // event handlers
    void info_toggled ();
    void hideHistoryActivated ();
    void tbRightPanel_1_toggled ();
    void tbTopPanel_1_toggled ();
    void beforeAfterToggled ();
    void tbBeforeLock_toggled();
    void saveAsPressed ();
    void queueImgPressed ();
    void sendToExternal();
    void sendToExternalChanged(int);
    void sendToExternalPressed();
    void openNextEditorImage ();
    void openPreviousEditorImage ();
    void syncFileBrowser ();

    // Signals.
    ExternalEditorChangedSignal * getExternalEditorChangedSignal();
    void setExternalEditorChangedSignal(ExternalEditorChangedSignal *signal);

    void tbTopPanel_1_visible (bool visible);
    bool CheckSidePanelsVisibility();
    void tbShowHideSidePanels_managestate();
    void toggleSidePanels();
    void toggleSidePanelsZoomFit();

    void saveProfile ();
    void collapseFilterBar();  // Collapse filter bar to prevent filmstrip overlap
    void closeAlbumView ();  // public: close album view + deselect sidebar
    Glib::ustring getShortName ();
    Glib::ustring getFileName () const;
    bool handleShortcutKey (GdkEventKey* event);

    bool getIsProcessing() const
    {
        return isProcessing;
    }
    void signalStopProcessing()
    {
        if (ipc) {
            ipc->signalStop();
        }
    }
    PreviewModePanel* getPreviewModePanel();
    IndicateClippedPanel* getIndicateClippedPanel();
    Gtk::ToggleButton* getToggleHistogramProfile() { return toggleHistogramProfile; }

    // Color management accessors (delegated to ColorManagementToolbar)
    int getRenderingIntent () const;
    void setRenderingIntent (int i);
    bool getSoftProofing () const;
    void setSoftProofing (bool a);
    bool getGamutCheck () const;
    void setGamutCheck (bool a);
    int getMonitorProfileIndex () const;
    void setMonitorProfileIndex (int i);
    int getMonitorProfileCount () const;
    Glib::ustring getMonitorProfileName (int i) const;
    void updateExternalEditorWidget(int selectedIndex, const std::vector<ExternalEditor> &editors);
    void updateProfiles (const Glib::ustring &printerProfile, rtengine::RenderingIntent printerIntent, bool printerBPC);
    void updateTPVScrollbar (bool hide);
    void updateHistogramPosition (int oldPosition, int newPosition);
    void updateToolPanelToolLocations(
        const std::vector<Glib::ustring> &favorites, bool cloneFavoriteTools);

    // Returns sidebar/filmstrip insets for queue overlay positioning
    void getQueueOverlayInsets (int& left, int& top, int& right) const;

    // Animated view transition (browser ↔ editor)
    void animateEditorIn(bool skipFilmstrip = false);
    void animateEditorOut(std::function<void()> onComplete);

    // Left panel (history) visibility for sync with browser sidebar
    bool isLeftPanelVisible() const { return hidehp && hidehp->get_active(); }
    void setLeftPanelVisible(bool visible) { if (hidehp && hidehp->get_active() != visible) hidehp->set_active(visible); }

    void defaultMonitorProfileChanged (const Glib::ustring &profile_name, bool auto_monitor_profile);

    bool saveImmediately (const Glib::ustring &filename, const SaveFormat &sf);

    void showNavigatorDialog();
    void showHistoryDialog();

    Gtk::Box* catalogPane;

    // MCP server access
    rtengine::StagedImageProcessor* getIpc() const { return ipc; }
    ToolPanelCoordinator* getTpc() const { return tpc; }

private:
    Gtk::Box* filmstripActionBar;
    Gtk::Button* filmstripRankBtns[5];
    int filmstripCurrentRating;
    std::map<std::string, Gtk::CheckMenuItem*> editorCopyFilters_;
    Gtk::Menu* editorCopyFilterMenu_;
    void updateFilmstripStars(int highlightUpTo);
    Gtk::Revealer* colorLabelRevealer_;

    // Filmstrip flag/reject
    Gtk::Button* filmstripFlagBtn_;
    Gtk::Button* filmstripRejectBtn_;
    int filmstripCurrentPick_;
    void updateFilmstripFlagBtn();

    // Filmstrip sort
    Gtk::MenuButton* filmstripSortBtn_;
    Gtk::Menu* filmstripSortMenu_;
    Gtk::RadioMenuItem* filmstripSortMethod_[Options::SORT_METHOD_COUNT];
    Gtk::RadioMenuItem* filmstripSortOrder_[2];
    void filmstripSortChanged ();

    // Album view sort
    Gtk::MenuButton* albumSortBtn_;
    Gtk::Menu* albumSortMenu_;
    Gtk::RadioMenuItem* albumSortMethod_[Options::SORT_METHOD_COUNT];
    Gtk::RadioMenuItem* albumSortOrder_[2];
    void albumSortChanged ();

    // Filter bar
    Gtk::ToggleButton* tbFilterBar;
    Gtk::Revealer* filterBarRevealer;
    Gtk::ToggleButton* fbUnRanked;
    Gtk::ToggleButton* fbRank[5];
    Gtk::ToggleButton* fbUnCLabeled;
    Gtk::ToggleButton* fbCLabel[5];
    Gtk::ToggleButton* fbEdited[2];
    Gtk::ToggleButton* fbRecentlySaved[2];
    Gtk::Button* fbClearAll;
    Gtk::Entry* fbSearchEntry;
    bool filterBarBlockSignals;

    // Filetype filter in editor filter bar
    Gtk::MenuButton* fbFiletypeButton_;
    Gtk::Popover* fbFiletypePopover_;
    Gtk::Box* fbFiletypeBox_;
    Gtk::CheckButton* fbFiletypeAllCheck_;
    std::map<std::string, Gtk::CheckButton*> fbFiletypeChecks_;
    bool fbFiletypeBlockSignals_ = false;
    void rebuildEditorFiletypePopover();
    void onEditorFiletypeCheckToggled(const std::string& ft);
    void onEditorFiletypeAllToggled();

    void filterBarToggled();
    void filterBarChanged();
    void filterBarClearAll();
    void applyEditorFilter();
    BrowserFilter buildEditorFilter();

    void close ();

    BatchQueueEntry*    createBatchQueueEntry ();
    bool                idle_imageSaved (ProgressConnector<int> *pc, rtengine::IImagefloat* img, Glib::ustring fname, SaveFormat sf, rtengine::procparams::ProcParams &pparams);
    bool                idle_saveImage (ProgressConnector<rtengine::IImagefloat*> *pc, Glib::ustring fname, SaveFormat sf, rtengine::procparams::ProcParams &pparams);
    bool                idle_sendToGimp ( ProgressConnector<rtengine::IImagefloat*> *pc, Glib::ustring fname);
    bool                idle_sentToGimp (ProgressConnector<int> *pc, rtengine::IImagefloat* img, Glib::ustring filename);
    void                histogramProfile_toggled ();
    RTAppChooserDialog *getAppChooserDialog();
    void onAppChooserDialogResponse(int resposneId);
    void updateExternalEditorSelection();


    Glib::ustring lastSaveAsFileName;
    bool realized;

    MyProgressBar  *progressLabel;
    Gtk::ToggleButton* hidehp;
    Gtk::ToggleButton* tbShowHideSidePanels;
    Gtk::ToggleButton* tbTopPanel_1;
    Gtk::ToggleButton* tbRightPanel_1;
    Gtk::ToggleButton* tbBeforeLock;
    //bool bAllSidePanelsVisible;
    Gtk::ToggleButton* beforeAfter;
    Gtk::Overlay* hpanedr;
    Gtk::Widget* editorToolbarTop_;
    Gtk::Widget* editorToolbarBottom_;
    Gtk::Image *iHistoryShow, *iHistoryHide;
    Gtk::Image *iTopPanel_1_Show, *iTopPanel_1_Hide;
    Gtk::Image *iRightPanel_1_Show, *iRightPanel_1_Hide;
    Gtk::Image *iShowHideSidePanels;
    Gtk::Image *iShowHideSidePanels_exit;
    Gtk::Image *iBeforeLockON, *iBeforeLockOFF;
    Gtk::Paned *leftbox;
    Gtk::Overlay* editOverlay_;
    NavigatorDialog* navigatorDialog_;
    HistoryDialog* historyDialog_;
    PlacesBrowser* editorPlacesBrowser_;
    RecentBrowser* editorRecentBrowser_;
    DirBrowser* editorDirBrowser_;
    AlbumBrowser* albumBrowser_;
    Gtk::Paned* editorPlacesPaned_;

    std::set<std::string> currentAlbumWhitelist_;
    void onAlbumSelected (const std::set<std::string>& whitelist);
    void onAlbumViewRequested (const Glib::ustring& albumName, const std::vector<Glib::ustring>& files);
    void addCurrentImageToTargetAlbum ();

    // Album grid view
    enum class AlbumViewMode { GRID, FIT, COLLAGE };

    Gtk::Stack* albumViewStack_;
    Gtk::Box* albumViewBox_;
    Gtk::Box* albumViewHeader_;
    Gtk::Label* albumNameLabel_;
    Gtk::Label* albumCountLabel_;
    Gtk::ScrolledWindow* albumViewScrolled_;
    Gtk::FlowBox* albumViewGrid_;
    Glib::ustring currentAlbumViewName_;
    std::vector<Glib::ustring> currentAlbumFiles_;
    int albumViewSession_;
    bool albumViewBuilt_;
    Gtk::ToggleButton* tbAlbumView_;
    sigc::connection albumViewToggleConn_;
    std::map<std::string, Glib::RefPtr<Gdk::Pixbuf>> albumThumbCache_;

    // Album view mode, zoom, info
    AlbumViewMode albumViewMode_;
    int albumThumbHeight_;
    bool albumShowInfo_;
    Gtk::Scale* albumZoomSlider_;
    sigc::connection albumZoomConn_;
    Gtk::ToggleButton* albumInfoToggle_;
    Gtk::RadioButton* albumModeGrid_;
    Gtk::RadioButton* albumModeFit_;
    Gtk::RadioButton* albumModeCollage_;
    Gtk::RadioButton::Group albumModeGroup_;
    Gtk::Stack* albumGridStack_;
    Gtk::DrawingArea* albumCollageArea_;
    struct CollageItem { int x, y, w, h; std::string filepath; };
    std::vector<CollageItem> collageLayout_;
    int collageContentHeight_;
    bool collageRelayoutPending_ = false;
    int fitLastW_ = 0;
    int fitLastH_ = 0;
    std::map<std::string, double> albumAspectCache_;
    std::map<std::string, Glib::RefPtr<Gdk::Pixbuf>> collageScaledCache_;
    sigc::connection albumFitResizeConn_;

    void showAlbumView (const Glib::ustring& albumName, const std::vector<Glib::ustring>& files);
    void hideAlbumView ();
    void toggleAlbumView ();
    void loadAlbumThumbnails (int session, const std::vector<Glib::ustring>& files);
    void rebuildAlbumGrid ();
    void albumZoomChanged ();
    void albumInfoToggled ();
    void albumViewModeChanged ();
    void applyAlbumViewMode ();
    void recalculateFitSize ();
    void recalculateCollageLayout ();
    bool onCollageAreaDraw (const Cairo::RefPtr<Cairo::Context>& cr);
    bool onCollageAreaClick (GdkEventButton* ev);
    double getAspectRatio (const std::string& filepath);

    Gtk::Box *vboxright;
    Gtk::Box *vsubboxright;
    Gtk::Box *histogramRow_;
    Gtk::Label *exifInfo;

    Gtk::Button* queueimg;
    Gtk::Button* saveimgas;
    PopUpButton* send_to_external;
    Gtk::RadioButtonGroup send_to_external_radio_group;
    Gtk::Button* navSync;
    Gtk::Button* navNext;
    Gtk::Button* navPrev;
    EditorInfo external_editor_info;
    std::unique_ptr<RTAppChooserDialog> app_chooser_dialog;
    ExternalEditorChangedSignal *externalEditorChangedSignal;
    sigc::connection externalEditorChangedSignalConnection;

    rtengine::InitialImage *cached_exported_image;
    rtengine::procparams::ProcParams cached_exported_pparams;
    Glib::ustring cached_exported_filename;

    class ColorManagementToolbar;
    std::unique_ptr<ColorManagementToolbar> colorMgmtToolBar;

    ImageAreaPanel* iareapanel;
    PreviewHandler* previewHandler;
    PreviewHandler* beforePreviewHandler;   // for the before-after view
    Navigator* navigator;
    ImageAreaPanel* beforeIarea;    // for the before-after view
    Gtk::Box* beforeBox;
    Gtk::Box* afterBox;
    Gtk::Label* beforeLabel;
    Gtk::Label* afterLabel;
    Gtk::Box* beforeAfterBox;
    Gtk::Box* beforeHeaderBox;
    Gtk::Box* afterHeaderBox;
    Gtk::ToggleButton* toggleHistogramProfile;

    PresetListPanel* presetListPanel;
    History* history;
    HistogramPanel* histogramPanel;
    ToolPanelCoordinator* tpc;
    RTWindow* parent;
    Gtk::Window* parentWindow;
    //SaveAsDialog* saveAsDialog;
    FilePanel* fPanel;

    bool firstProcessingDone;

    Thumbnail* openThm;  // may get invalid on external delete event
    Glib::ustring fname;  // must be saved separately

    int selectedFrame;

    rtengine::InitialImage* isrc;
    rtengine::StagedImageProcessor* ipc;
    rtengine::StagedImageProcessor* beforeIpc;    // for the before-after view

    EditorPanelIdleHelper* epih;

    int err;

    time_t processingStartedTime;

    sigc::connection ShowHideSidePanelsconn;

    bool isProcessing;

    IdleRegister idle_register;

    // Cancellation token for async placeholder thumbnail generation.
    // Set to true in close()/destructor to prevent stale callbacks.
    std::shared_ptr<std::atomic<bool>> placeholderCancel_;

    // Cancellation token for async before/after image loading.
    std::shared_ptr<std::atomic<bool>> beforeAfterCancel_;

    rtengine::HistogramObservable* histogram_observable;
    Options::ScopeType histogram_scope_type;
    Glib::ustring lastSyncedEditorDir_;  // prevent redundant dir browser scroll resets

    // View transition animation state
    double editorAnimFraction_ = 1.0;  // 0=hidden, 1=fully shown
    bool editorAnimIn_ = true;         // true=animating in, false=animating out
    sigc::connection editorAnimConn_;

    // Sidebar/filmstrip toggle animation state
    double leftAnimFraction_ = 1.0;    // 0=hidden, 1=fully shown
    sigc::connection leftAnimConn_;
    double rightAnimFraction_ = 1.0;
    sigc::connection rightAnimConn_;
    double topAnimFraction_ = 1.0;
    sigc::connection topAnimConn_;
    int filmstripFullHeight_ = 0;      // cached filmstrip height for animation
};
