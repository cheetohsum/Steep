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
    void updatePreview();
    void requestPreviewThumbs();
    const std::vector<ScanItem>& activeItems() const;

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
    std::set<Glib::ustring> pendingThumbs_;

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
    Gtk::Box* trayBox_;
};
