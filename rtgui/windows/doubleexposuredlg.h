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
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include <gtkmm.h>

#include "browserfilter.h"
#include "guiutils.h"
#include "rtengine/procparams.h"

class DEThumbGrid;
class DEBlendPreview;
struct DEThumbQueue;
struct DEScenePlate;

namespace rtengine
{
class ColorTemp;
}

// Modal picker for the double exposure tool: a mini file-browser (current
// folder or global picked/starred selects) on the left, a live approximate
// blend preview and the layer stack on the right. On OK the chosen layer
// stack and blend settings are read back via getResult().
class DoubleExposureDlg final : public Gtk::Dialog
{
public:
    DoubleExposureDlg(Gtk::Window* parent, const Glib::ustring& baseImagePath,
                      const rtengine::procparams::DoubleExposureParams& initial,
                      const BrowserFilter* browserFilter,
                      const Glib::ustring& browserCurrentDir);
    ~DoubleExposureDlg() override;

    rtengine::procparams::DoubleExposureParams getResult() const;

    // Non-empty when the user pressed a chip's develop button: the dialog
    // answered OK and this layer should be opened for editing.
    Glib::ustring getEditRequestPath() const
    {
        return editRequestPath_;
    }

    // Session-wide template: the last layer settings the user removed or
    // applied, inherited by newly added exposures (also on other photos).
    static bool haveStickyLayer();
    static const rtengine::procparams::DoubleExposureParams::Layer& stickyLayer();
    static void rememberStickyLayer(const rtengine::procparams::DoubleExposureParams::Layer& layer);

private:
    struct ScanItem {
        Glib::ustring path;
        int rank = 0;
        int pick = 0;
    };

    // -- scanning / thumb loading (worker threads post back via idle) --
    void startScan(bool global);
    void onScanPartial(bool global, int generation, const std::vector<ScanItem>& items);
    void onScanDone(bool global, int generation);
    void requestThumbs(const std::vector<Glib::ustring>& paths, int height, bool neutral = false);
    void onThumbLoaded(const Glib::ustring& path, int height, bool neutral, Glib::RefPtr<Gdk::Pixbuf> pixbuf);
    void requestScenePlates(const std::vector<Glib::ustring>& paths, int height);
    void onSceneLoaded(const Glib::ustring& path, int height, std::shared_ptr<const DEScenePlate> plate);
    void initSceneContext();
    void pumpThumbQueue();
    // Grid thumbs are decoded for the scrolled viewport only (plus a
    // screenful of look-ahead): a global scan can stream in thousands of
    // files and decoding them all would take minutes of CPU and I/O.
    void requestVisibleThumbs();
    void scheduleVisibleThumbs();
    // Coalesces the full grid rebuild while a scan streams results in.
    void scheduleFilterRefresh();

    // -- UI logic --
    void scopeChanged(bool global);
    void openFolderChooser();
    void applyFilter();
    void itemToggled(const Glib::ustring& path);
    void moveLayer(size_t index, int direction);
    void removeLayer(size_t index);
    void selectLayer(size_t index);
    void rebuildTray();
    void syncLayerControls();
    void layerControlChanged();
    void blendControlChanged();
    void highResToggled();
    // quick = interactive pass (half resolution, replicated); a full pass
    // follows once the controls settle.
    void updatePreview(bool quick = false);
    // Interactive placement of the selected exposure from the preview.
    void onPreviewMove(double dxCompositePx, double dyCompositePx);
    void onPreviewScale(double factor);
    void onPreviewReset();
    void syncPlacementControls();
    void schedulePreviewUpdate();
    void requestPreviewThumbs();
    const std::vector<ScanItem>& activeItems() const;
    // Puts the grid back where it was when the picker last closed, once the
    // (streamed) results are tall enough; gives up as soon as the user
    // scrolls. `final` applies whatever is reachable and stops trying.
    void restoreScroll(bool final);
    void savePickerState();

    Glib::ustring baseImagePath_;
    Glib::ustring editRequestPath_;
    Glib::ustring folderDir_;
    Glib::ustring browserCurrentDir_;
    rtengine::procparams::DoubleExposureParams params_;
    BrowserFilter browserFilter_;
    bool haveBrowserFilter_;
    size_t selectedLayer_;
    bool syncingControls_;

    bool globalScope_;
    bool folderScanned_;
    bool globalScanned_;
    bool folderScanRunning_;
    bool globalScanRunning_;
    int folderScanGen_;
    int globalScanGen_;
    std::vector<ScanItem> folderItems_;
    std::vector<ScanItem> globalItems_;

    std::map<Glib::ustring, Glib::RefPtr<Gdk::Pixbuf>> gridPix_;
    std::map<Glib::ustring, Glib::RefPtr<Gdk::Pixbuf>> bigPix_;
    std::map<Glib::ustring, Glib::RefPtr<Gdk::Pixbuf>> hiPix_;
    // Neutral (default-params) renders: layer previews plus the base plate's
    // scene reference for engine-faithful compositing.
    std::map<Glib::ustring, Glib::RefPtr<Gdk::Pixbuf>> neutralPix_;
    std::map<Glib::ustring, Glib::RefPtr<Gdk::Pixbuf>> neutralHiPix_;
    // Scene plates: the engine's own preview-tier decodes (scene-linear,
    // working profile, full upright frame), area-averaged to the preview
    // height. The composite runs on these so crossovers, gates and detail
    // match the canvas; the neutral pixbufs above remain for the tray chips.
    std::map<Glib::ustring, std::shared_ptr<const DEScenePlate>> scenePix_;
    std::map<Glib::ustring, std::shared_ptr<const DEScenePlate>> sceneHiPix_;
    std::set<Glib::ustring> pendingThumbs_;

    // Base image context the scene plates must honour: the edit's working
    // profile (shared cache entries with the engine), a fixed white balance
    // when the edit uses one, and the coarse rotation the canvas is in.
    Glib::ustring workingProfile_;
    std::shared_ptr<const rtengine::ColorTemp> baseWb_;
    int baseCoarseRotate_ = 0;
    bool baseHflip_ = false;
    bool baseVflip_ = false;
    double workingToSrgb_[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

    std::shared_ptr<std::atomic<bool>> alive_;
    std::shared_ptr<DEThumbQueue> thumbQueue_;
    bool filterRefreshPending_;
    bool visibleThumbsPending_;

    // -- widgets --
    Gtk::ToggleButton* folderBtn_;
    Gtk::ToggleButton* globalBtn_;
    Gtk::ToggleButton* pickedFilter_;
    MyComboBoxText* starsFilter_;
    Gtk::Label* countLabel_;
    Gtk::Spinner* spinner_;
    Gtk::ScrolledWindow* gridScroll_;
    DEThumbGrid* grid_;
    DEBlendPreview* preview_;
    Gtk::CheckButton* highRes_;
    MyComboBoxText* blendMethod_;
    Gtk::CheckButton* autoGain_;
    Gtk::Scale* baseEvScale_;
    Gtk::Label* layerLabel_;
    Gtk::Scale* layerEvScale_;
    Gtk::Scale* layerOpacityScale_;
    Gtk::Scale* gateStrengthScale_;
    Gtk::Scale* softnessScale_;
    Gtk::Scale* latitudeScale_;
    Gtk::Scale* offsetXScale_;
    Gtk::Scale* offsetYScale_;
    Gtk::Scale* scaleScale_;
    Gtk::Button* resetPlacement_;
    Gtk::Box* trayBox_;

    // preview pixel -> base full-frame fraction, from the last updatePreview
    double previewNx0_ = 0.0;
    double previewNxs_ = 0.0;
    double previewNy0_ = 0.0;
    double previewNys_ = 0.0;
    bool previewUpdatePending_ = false;
    sigc::connection previewSettle_;
    double pendingScrollRestore_ = -1.0; // saved grid scroll still to apply, <0 = none
    double lastRestoredScroll_ = -1.0;   // what we last set, to detect the user scrolling

    // Everything updatePreview needs that depends only on the base (not on
    // the layers): the linearized styled plate, the crop geometry and the
    // look-transfer curve. Rebuilt only when the base inputs change, so a
    // slider drag pays for the composite alone.
    struct PreviewCache {
        Glib::RefPtr<Gdk::Pixbuf> styledRef;
        Glib::RefPtr<Gdk::Pixbuf> neutralRef;
        std::shared_ptr<const DEScenePlate> plateRef;
        bool wantHi = false;
        int w = 0;
        int h = 0;
        std::vector<float> lin;
        float nx0 = 0.f, nxs = 0.f, ny0 = 0.f, nys = 0.f, baseAspect = 1.f;
        bool haveTransfer = false;
        float toneCurve[256] = {};
        float satFactor = 1.f;
    };
    PreviewCache previewCache_;
};
