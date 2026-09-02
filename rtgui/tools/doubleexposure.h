/** -*- C++ -*-
 *
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
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "browserfilter.h"
#include "guiutils.h"
#include "toolpanel.h"
#include "widgets/basic/adjuster.h"

#include <gtkmm.h>

// Double exposure: composite partner photos onto the edited image the way
// light stacks on one frame of film. The panel keeps the layer stack and
// blend settings; partners are picked in a modal browser dialog.
class DoubleExposure final : public ToolParamBlock, public AdjusterListener, public FoldableToolPanel
{
private:
    Gtk::Button *chooseButton;
    Gtk::Button *clearButton;
    Gtk::Box *layersBox;
    MyComboBoxText *layerSel;
    Adjuster *layerEv;
    Adjuster *layerOpacity;
    MyComboBoxText *blendMethod;
    Gtk::Box *compareRow;        // comparative modes only
    MyComboBoxText *compareMode;
    Adjuster *softness;
    Adjuster *highlightLatitude;
    MyComboBoxText *gateSource;
    Adjuster *gateLow;
    Adjuster *gateHigh;
    Adjuster *gateFeather;
    Adjuster *gateStrength;
    Gtk::CheckButton *autoGain;
    Adjuster *baseEv;

    std::vector<rtengine::procparams::DoubleExposureParams::Layer> layers;
    Glib::ustring editedFilePath_;
    std::function<BrowserFilter()> browserFilterProvider_;
    std::function<Glib::ustring()> browserDirProvider_;
    std::function<void(const Glib::ustring&, const Glib::ustring&)> openPartnerHandler_;
    std::map<Glib::ustring, Glib::RefPtr<Gdk::Pixbuf>> rowThumbs_;
    std::set<Glib::ustring> pendingRowThumbs_;
    std::shared_ptr<std::atomic<bool>> aliveToken_;
    bool layersEdited_;
    bool autoGainEdited_;

    rtengine::ProcEvent EvDEEnabled;
    rtengine::ProcEvent EvDELayers;
    rtengine::ProcEvent EvDELayerSettings;
    rtengine::ProcEvent EvDEBlend;
    rtengine::ProcEvent EvDEAutoGain;
    rtengine::ProcEvent EvDEBaseEv;
    rtengine::ProcEvent EvDEGate;
    rtengine::ProcEvent EvDECompare;
    rtengine::ProcEvent EvDESoftness;
    rtengine::ProcEvent EvDELatitude;

    void openChooser();
    void clearAll();
    void rebuildLayerRows();
    void requestRowThumbs(const std::vector<Glib::ustring>& paths);
    void refreshLayerSelector();
    void loadSelectedLayer();
    void removeLayer(size_t index);
    void updateSensitivity();
    int selectedLayerIndex() const;

public:
    static const Glib::ustring TOOL_NAME;

    DoubleExposure();
    ~DoubleExposure() override;

    void read(const rtengine::procparams::ProcParams *pp, const ParamsEdited *pedited = nullptr) override;
    void write(rtengine::procparams::ProcParams *pp, ParamsEdited *pedited = nullptr) override;
    void setDefaults(const rtengine::procparams::ProcParams *defParams, const ParamsEdited *pedited = nullptr) override;
    void setBatchMode(bool batchMode) override;

    void adjusterChanged(Adjuster *a, double newval) override;
    void enabledChanged() override;
    void blendChanged();
    void compareChanged();
    void gateSourceChanged();
    void autoGainToggled();
    void layerSelChanged();

    // Absolute path of the image being edited; drives the picker's folder
    // scope and excludes the base plate from the candidate list.
    void setEditedFilePath(const Glib::ustring &path);

    // Supplies the browser tab's current filter so the picker inherits the
    // user's filetype/pick/star filtering.
    void setBrowserFilterProvider(std::function<BrowserFilter()> provider);

    // Supplies the browser tab's current directory so global selects can
    // include the folder the user is browsing.
    void setBrowserDirProvider(std::function<Glib::ustring()> provider);

    // Called with (partnerPath, editedImagePath) when the user asks to
    // develop one of the partner exposures; surfaces it in the filmstrip
    // and opens it in the editor.
    void setOpenPartnerHandler(std::function<void(const Glib::ustring&, const Glib::ustring&)> handler);
};
