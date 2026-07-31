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
#include <cmath>

#include "doubleexposure.h"

#include "eventmapper.h"
#include "windows/doubleexposuredlg.h"

#include "rtengine/procparams.h"

#include <glibmm/miscutils.h>

using namespace rtengine;
using namespace rtengine::procparams;

const Glib::ustring DoubleExposure::TOOL_NAME = "doubleexposure";

DoubleExposure::DoubleExposure() :
    FoldableToolPanel(this, TOOL_NAME, M("TP_DOUBLEEXPOSURE_LABEL"), false, true),
    layersEdited_(false),
    blendEdited_(false),
    autoGainEdited_(false)
{
    auto m = ProcEventMapper::getInstance();
    EvDEEnabled = m->newEvent(HDR, "HISTORY_MSG_DOUBLEEXPOSURE_ENABLED");
    EvDELayers = m->newEvent(HDR, "HISTORY_MSG_DOUBLEEXPOSURE_LAYERS");
    EvDELayerSettings = m->newEvent(HDR, "HISTORY_MSG_DOUBLEEXPOSURE_LAYER_SETTINGS");
    EvDEBlend = m->newEvent(HDR, "HISTORY_MSG_DOUBLEEXPOSURE_BLEND");
    EvDEAutoGain = m->newEvent(HDR, "HISTORY_MSG_DOUBLEEXPOSURE_AUTOGAIN");
    EvDEBaseEv = m->newEvent(HDR, "HISTORY_MSG_DOUBLEEXPOSURE_BASEEV");
    EvDEFillShadows = m->newEvent(HDR, "HISTORY_MSG_DOUBLEEXPOSURE_FILLSHADOWS");

    chooseButton = Gtk::manage(new Gtk::Button(M("TP_DOUBLEEXPOSURE_CHOOSE")));
    chooseButton->signal_clicked().connect(sigc::mem_fun(*this, &DoubleExposure::openChooser));
    chooseButton->show();

    clearButton = Gtk::manage(new Gtk::Button("\xE2\x9C\x95")); // ✕
    clearButton->set_relief(Gtk::RELIEF_NONE);
    clearButton->set_tooltip_text(M("TP_DOUBLEEXPOSURE_CLEAR"));
    clearButton->set_no_show_all(true);
    clearButton->signal_clicked().connect(sigc::mem_fun(*this, &DoubleExposure::clearAll));

    layersBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 2));
    layersBox->show();

    layerSel = Gtk::manage(new MyComboBoxText());
    layerSel->connect(layerSel->signal_changed().connect(sigc::mem_fun(*this, &DoubleExposure::layerSelChanged)));
    layerSel->show();

    Gtk::Box* layerSelRow = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 4));
    Gtk::Label* layerSelLabel = Gtk::manage(new Gtk::Label(M("TP_DOUBLEEXPOSURE_LAYER") + ":", Gtk::ALIGN_START));
    layerSelRow->pack_start(*layerSelLabel, Gtk::PACK_SHRINK);
    layerSelRow->pack_start(*layerSel, Gtk::PACK_EXPAND_WIDGET);
    layerSelRow->show_all();

    layerEv = Gtk::manage(new Adjuster(M("TP_DOUBLEEXPOSURE_EV"), -4.0, 4.0, 0.05, 0.0));
    layerEv->setAdjusterListener(this);
    layerEv->show();

    layerOpacity = Gtk::manage(new Adjuster(M("TP_DOUBLEEXPOSURE_OPACITY"), 0.0, 100.0, 1.0, 100.0));
    layerOpacity->setAdjusterListener(this);
    layerOpacity->show();

    blendMethod = Gtk::manage(new MyComboBoxText());
    blendMethod->append(M("TP_DOUBLEEXPOSURE_BLEND_ADD"));
    blendMethod->append(M("TP_DOUBLEEXPOSURE_BLEND_SCREEN"));
    blendMethod->append(M("TP_DOUBLEEXPOSURE_BLEND_MULTIPLY"));
    blendMethod->append(M("TP_DOUBLEEXPOSURE_BLEND_LIGHTEN"));
    blendMethod->set_active(0);
    blendMethod->setPreferredWidth(150, 200);
    blendMethod->connect(blendMethod->signal_changed().connect(sigc::mem_fun(*this, &DoubleExposure::blendChanged)));
    blendMethod->show();

    Gtk::Box* blendRow = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 4));
    Gtk::Label* blendLabel = Gtk::manage(new Gtk::Label(M("TP_DOUBLEEXPOSURE_BLEND") + ":", Gtk::ALIGN_START));
    blendRow->pack_start(*blendLabel, Gtk::PACK_SHRINK);
    blendRow->pack_start(*blendMethod, Gtk::PACK_EXPAND_WIDGET);
    blendRow->show_all();

    autoGain = Gtk::manage(new Gtk::CheckButton(M("TP_DOUBLEEXPOSURE_AUTOGAIN")));
    autoGain->set_active(true);
    autoGain->set_tooltip_text(M("TP_DOUBLEEXPOSURE_AUTOGAIN_TOOLTIP"));
    autoGain->signal_toggled().connect(sigc::mem_fun(*this, &DoubleExposure::autoGainToggled));
    autoGain->show();

    baseEv = Gtk::manage(new Adjuster(M("TP_DOUBLEEXPOSURE_BASEEV"), -4.0, 4.0, 0.05, 0.0));
    baseEv->setAdjusterListener(this);
    baseEv->show();

    fillShadows = Gtk::manage(new Adjuster(M("TP_DOUBLEEXPOSURE_FILLSHADOWS"), 0.0, 100.0, 1.0, 25.0));
    fillShadows->setAdjusterListener(this);
    fillShadows->set_tooltip_text(M("TP_DOUBLEEXPOSURE_FILLSHADOWS_TOOLTIP"));
    fillShadows->show();

    Gtk::Box* chooseRow = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 2));
    chooseRow->pack_start(*chooseButton, Gtk::PACK_EXPAND_WIDGET);
    chooseRow->pack_start(*clearButton, Gtk::PACK_SHRINK);
    chooseRow->show();

    getSummaryBox()->pack_start(*chooseRow);
    getSummaryBox()->show_all();

    pack_start(*layersBox);
    pack_start(*layerSelRow);
    pack_start(*layerEv);
    pack_start(*layerOpacity);
    pack_start(*blendRow);
    pack_start(*autoGain);
    pack_start(*baseEv);
    pack_start(*fillShadows);

    updateSensitivity();
}

void DoubleExposure::setEditedFilePath(const Glib::ustring& path)
{
    editedFilePath_ = path;
}

void DoubleExposure::setBrowserFilterProvider(std::function<BrowserFilter()> provider)
{
    browserFilterProvider_ = std::move(provider);
}

void DoubleExposure::setBrowserDirProvider(std::function<Glib::ustring()> provider)
{
    browserDirProvider_ = std::move(provider);
}

int DoubleExposure::selectedLayerIndex() const
{
    const int row = layerSel->get_active_row_number();

    if (row < 0 || row >= static_cast<int>(layers.size())) {
        return -1;
    }

    return row;
}

void DoubleExposure::rebuildLayerRows()
{
    for (Gtk::Widget* child : layersBox->get_children()) {
        layersBox->remove(*child);
    }

    for (size_t i = 0; i < layers.size(); ++i) {
        Gtk::Box* row = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));

        Gtk::Label* num = Gtk::manage(new Gtk::Label(Glib::ustring::compose("%1", i + 1)));
        num->get_style_context()->add_class("dim-label");
        row->pack_start(*num, Gtk::PACK_SHRINK);

        Gtk::Label* name = Gtk::manage(new Gtk::Label(Glib::path_get_basename(layers[i].path), Gtk::ALIGN_START));
        name->set_ellipsize(Pango::ELLIPSIZE_MIDDLE);
        name->set_tooltip_text(layers[i].path);

        if (!Glib::file_test(layers[i].path, Glib::FILE_TEST_EXISTS)) {
            name->set_tooltip_text(layers[i].path + "\n" + M("TP_DOUBLEEXPOSURE_MISSING"));
            name->get_style_context()->add_class("error-label");
        }

        row->pack_start(*name, Gtk::PACK_EXPAND_WIDGET);

        Gtk::Button* remove = Gtk::manage(new Gtk::Button());
        remove->set_image_from_icon_name("window-close-symbolic", Gtk::ICON_SIZE_MENU);
        remove->set_relief(Gtk::RELIEF_NONE);
        remove->set_tooltip_text(M("TP_DOUBLEEXPOSURE_REMOVE"));
        const size_t idx = i;
        remove->signal_clicked().connect([this, idx]() { removeLayer(idx); });
        row->pack_start(*remove, Gtk::PACK_SHRINK);

        row->show_all();
        layersBox->pack_start(*row, Gtk::PACK_SHRINK);
    }
}

void DoubleExposure::refreshLayerSelector()
{
    layerSel->block(true);
    layerSel->remove_all();

    for (size_t i = 0; i < layers.size(); ++i) {
        layerSel->append(Glib::ustring::compose("%1 %2 — %3", M("TP_DOUBLEEXPOSURE_LAYER"), i + 1, Glib::path_get_basename(layers[i].path)));
    }

    if (!layers.empty()) {
        layerSel->set_active(0);
    }

    layerSel->block(false);
    loadSelectedLayer();
}

void DoubleExposure::loadSelectedLayer()
{
    const int idx = selectedLayerIndex();

    if (idx < 0) {
        return;
    }

    layerEv->setValue(layers[idx].ev);
    layerOpacity->setValue(layers[idx].opacity);
}

void DoubleExposure::removeLayer(size_t index)
{
    if (index >= layers.size()) {
        return;
    }

    layers.erase(layers.begin() + index);
    layersEdited_ = true;
    rebuildLayerRows();
    refreshLayerSelector();
    updateSensitivity();
    autoEnable();

    if (listener && getEnabled()) {
        listener->panelChanged(EvDELayers, Glib::ustring::compose("%1", layers.size()));
    }
}

void DoubleExposure::updateSensitivity()
{
    const bool haveLayers = !layers.empty();
    layerSel->set_sensitive(haveLayers);
    layerEv->set_sensitive(haveLayers);
    layerOpacity->set_sensitive(haveLayers);
    clearButton->set_visible(haveLayers);

    const bool additive = blendMethod->get_active_row_number() == 0;
    autoGain->set_sensitive(additive);
}

void DoubleExposure::clearAll()
{
    if (layers.empty()) {
        return;
    }

    disableListener();
    layers.clear();
    layersEdited_ = true;
    rebuildLayerRows();
    refreshLayerSelector();
    updateSensitivity();
    setEnabled(false);
    enableListener();

    if (listener) {
        listener->panelChanged(EvDELayers, M("GENERAL_DISABLED"));
    }
}

void DoubleExposure::openChooser()
{
    Gtk::Window* toplevel = dynamic_cast<Gtk::Window*>(get_toplevel());

    DoubleExposureParams current;
    current.enabled = getEnabled();
    current.layers = layers;
    const int blendRow = blendMethod->get_active_row_number();
    current.blendMode = static_cast<DoubleExposureParams::BlendMode>(blendRow < 0 ? 0 : blendRow);
    current.autoGain = autoGain->get_active();
    current.baseEv = baseEv->getValue();
    current.fillShadows = fillShadows->getValue();

    BrowserFilter browserFilter;
    bool haveBrowserFilter = false;

    if (browserFilterProvider_) {
        browserFilter = browserFilterProvider_();
        haveBrowserFilter = true;
    }

    const Glib::ustring browserDir = browserDirProvider_ ? browserDirProvider_() : Glib::ustring();

    DoubleExposureDlg dialog(toplevel, editedFilePath_, current, haveBrowserFilter ? &browserFilter : nullptr, browserDir);

    if (dialog.run() == Gtk::RESPONSE_OK) {
        const DoubleExposureParams result = dialog.getResult();

        disableListener();
        layers = result.layers;
        blendMethod->block(true);
        blendMethod->set_active(static_cast<int>(result.blendMode));
        blendMethod->block(false);
        autoGain->set_active(result.autoGain);
        baseEv->setValue(result.baseEv);
        fillShadows->setValue(result.fillShadows);
        rebuildLayerRows();
        refreshLayerSelector();
        updateSensitivity();
        enableListener();

        layersEdited_ = true;
        blendEdited_ = true;
        autoGainEdited_ = true;

        if (!layers.empty()) {
            setEnabled(true);
        }

        if (listener) {
            listener->panelChanged(EvDELayers, Glib::ustring::compose("%1", layers.size()));
        }
    }

    dialog.hide();
}

void DoubleExposure::read(const ProcParams* pp, const ParamsEdited* pedited)
{
    disableListener();

    if (pedited) {
        baseEv->setEditedState(pedited->doubleExposure.baseEv ? Edited : UnEdited);
        fillShadows->setEditedState(pedited->doubleExposure.fillShadows ? Edited : UnEdited);
        set_inconsistent(multiImage && !pedited->doubleExposure.enabled);
        autoGain->set_inconsistent(!pedited->doubleExposure.autoGain);
    }

    setEnabled(pp->doubleExposure.enabled);
    layers = pp->doubleExposure.layers;
    layersEdited_ = false;
    blendEdited_ = false;
    autoGainEdited_ = false;

    blendMethod->block(true);
    blendMethod->set_active(static_cast<int>(pp->doubleExposure.blendMode));
    blendMethod->block(false);
    autoGain->set_active(pp->doubleExposure.autoGain);
    baseEv->setValue(pp->doubleExposure.baseEv);
    fillShadows->setValue(pp->doubleExposure.fillShadows);

    rebuildLayerRows();
    refreshLayerSelector();
    updateSensitivity();

    enableListener();
}

void DoubleExposure::write(ProcParams* pp, ParamsEdited* pedited)
{
    pp->doubleExposure.enabled = getEnabled();
    pp->doubleExposure.layers = layers;

    const int blendRow = blendMethod->get_active_row_number();
    pp->doubleExposure.blendMode = static_cast<DoubleExposureParams::BlendMode>(blendRow < 0 ? 0 : blendRow);
    pp->doubleExposure.autoGain = autoGain->get_active();
    pp->doubleExposure.baseEv = baseEv->getValue();
    pp->doubleExposure.fillShadows = fillShadows->getValue();

    // Layer EV/opacity edits land directly in `layers` via adjusterChanged.

    if (pedited) {
        pedited->doubleExposure.enabled = !get_inconsistent();
        pedited->doubleExposure.layers = layersEdited_;
        pedited->doubleExposure.blendMode = blendEdited_;
        pedited->doubleExposure.autoGain = autoGainEdited_ || !autoGain->get_inconsistent();
        pedited->doubleExposure.baseEv = baseEv->getEditedState();
        pedited->doubleExposure.fillShadows = fillShadows->getEditedState();
    }
}

void DoubleExposure::setDefaults(const ProcParams* defParams, const ParamsEdited* pedited)
{
    baseEv->setDefault(defParams->doubleExposure.baseEv);
    fillShadows->setDefault(defParams->doubleExposure.fillShadows);
    layerEv->setDefault(0.0);
    layerOpacity->setDefault(100.0);

    if (pedited) {
        baseEv->setDefaultEditedState(pedited->doubleExposure.baseEv ? Edited : UnEdited);
        fillShadows->setDefaultEditedState(pedited->doubleExposure.fillShadows ? Edited : UnEdited);
    } else {
        baseEv->setDefaultEditedState(Irrelevant);
        fillShadows->setDefaultEditedState(Irrelevant);
    }
}

void DoubleExposure::adjusterChanged(Adjuster* a, double newval)
{
    if (a == layerEv || a == layerOpacity) {
        const int idx = selectedLayerIndex();

        if (idx >= 0) {
            if (a == layerEv) {
                layers[idx].ev = newval;
            } else {
                layers[idx].opacity = newval;
            }

            layersEdited_ = true;
            autoEnable();

            if (listener && getEnabled()) {
                listener->panelChanged(EvDELayerSettings, a->getTextValue());
            }
        }

        return;
    }

    autoEnable();

    if (listener && getEnabled()) {
        if (a == baseEv) {
            listener->panelChanged(EvDEBaseEv, a->getTextValue());
        } else if (a == fillShadows) {
            listener->panelChanged(EvDEFillShadows, a->getTextValue());
        }
    }
}

void DoubleExposure::enabledChanged()
{
    if (listener) {
        if (get_inconsistent()) {
            listener->panelChanged(EvDEEnabled, M("GENERAL_UNCHANGED"));
        } else if (getEnabled()) {
            listener->panelChanged(EvDEEnabled, M("GENERAL_ENABLED"));
        } else {
            listener->panelChanged(EvDEEnabled, M("GENERAL_DISABLED"));
        }
    }
}

void DoubleExposure::blendChanged()
{
    blendEdited_ = true;
    updateSensitivity();
    autoEnable();

    if (listener && getEnabled()) {
        listener->panelChanged(EvDEBlend, blendMethod->get_active_text());
    }
}

void DoubleExposure::autoGainToggled()
{
    autoGainEdited_ = true;
    autoEnable();

    if (listener && getEnabled()) {
        listener->panelChanged(EvDEAutoGain, autoGain->get_active() ? M("GENERAL_ENABLED") : M("GENERAL_DISABLED"));
    }
}

void DoubleExposure::layerSelChanged()
{
    loadSelectedLayer();
}

void DoubleExposure::setBatchMode(bool batchMode)
{
    ToolPanel::setBatchMode(batchMode);

    baseEv->showEditedCB();
    fillShadows->showEditedCB();
}
