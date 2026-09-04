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
#include <thread>

#include "doubleexposure.h"

#include "rtengine/aisegmentation.h"

#include "partnerthumb.h"

#include "eventmapper.h"
#include "windows/doubleexposuredlg.h"

#include "rtengine/procparams.h"

#include <glibmm/miscutils.h>

using namespace rtengine;
using namespace rtengine::procparams;

const Glib::ustring DoubleExposure::TOOL_NAME = "doubleexposure";

namespace
{

constexpr int ROW_THUMB_H = 34;

// winpthreads can report ESRCH from detach() when the thread has already
// finished; harmless here.
void detachQuietly(std::thread&& thread)
{
    try {
        thread.detach();
    } catch (const std::system_error&) {
    }
}

} // namespace

DoubleExposure::DoubleExposure() :
    FoldableToolPanel(this, TOOL_NAME, M("TP_DOUBLEEXPOSURE_LABEL"), false, true),
    aliveToken_(std::make_shared<std::atomic<bool>>(true)),
    layersEdited_(false),
    autoGainEdited_(false)
{
    auto m = ProcEventMapper::getInstance();
    EvDEEnabled = m->newEvent(HDR, "HISTORY_MSG_DOUBLEEXPOSURE_ENABLED");
    EvDELayers = m->newEvent(HDR, "HISTORY_MSG_DOUBLEEXPOSURE_LAYERS");
    EvDELayerSettings = m->newEvent(HDR, "HISTORY_MSG_DOUBLEEXPOSURE_LAYER_SETTINGS");
    EvDEBlend = m->newEvent(HDR, "HISTORY_MSG_DOUBLEEXPOSURE_BLEND");
    EvDEAutoGain = m->newEvent(HDR, "HISTORY_MSG_DOUBLEEXPOSURE_AUTOGAIN");
    EvDEBaseEv = m->newEvent(HDR, "HISTORY_MSG_DOUBLEEXPOSURE_BASEEV");
    EvDEGate = m->newEvent(HDR, "HISTORY_MSG_DOUBLEEXPOSURE_GATE");
    EvDECompare = m->newEvent(HDR, "HISTORY_MSG_DOUBLEEXPOSURE_COMPARE");
    EvDESoftness = m->newEvent(HDR, "HISTORY_MSG_DOUBLEEXPOSURE_SOFTNESS");
    EvDELatitude = m->newEvent(HDR, "HISTORY_MSG_DOUBLEEXPOSURE_LATITUDE");
    EvDEPlacement = m->newEvent(HDR, "HISTORY_MSG_DOUBLEEXPOSURE_PLACEMENT");
    EvDESubject = m->newEvent(HDR, "HISTORY_MSG_DOUBLEEXPOSURE_SUBJECT");

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

    // Placement over the base frame; the picker's preview drags these too.
    layerOffsetX = Gtk::manage(new Adjuster(M("TP_DOUBLEEXPOSURE_OFFSETX"), -150.0, 150.0, 0.5, 0.0));
    layerOffsetX->setAdjusterListener(this);
    layerOffsetX->set_tooltip_text(M("TP_DOUBLEEXPOSURE_PLACEMENT_TOOLTIP"));
    layerOffsetX->show();

    layerOffsetY = Gtk::manage(new Adjuster(M("TP_DOUBLEEXPOSURE_OFFSETY"), -150.0, 150.0, 0.5, 0.0));
    layerOffsetY->setAdjusterListener(this);
    layerOffsetY->set_tooltip_text(M("TP_DOUBLEEXPOSURE_PLACEMENT_TOOLTIP"));
    layerOffsetY->show();

    layerScale = Gtk::manage(new Adjuster(M("TP_DOUBLEEXPOSURE_SCALE"), 10.0, 400.0, 1.0, 100.0));
    layerScale->setAdjusterListener(this);
    layerScale->set_tooltip_text(M("TP_DOUBLEEXPOSURE_PLACEMENT_TOOLTIP"));
    layerScale->show();

    layerRotate = Gtk::manage(new Adjuster(M("TP_DOUBLEEXPOSURE_ROTATE"), -180.0, 180.0, 0.5, 0.0));
    layerRotate->setAdjusterListener(this);
    layerRotate->set_tooltip_text(M("TP_DOUBLEEXPOSURE_ROTATE_TOOLTIP"));
    layerRotate->show();

    // Patterning turns the placed frame into one tile of a grid; its size is
    // the Layer size above, so there is no second size control.
    patternMethod = Gtk::manage(new MyComboBoxText());
    patternMethod->append(M("TP_DOUBLEEXPOSURE_PATTERN_OFF"));
    patternMethod->append(M("TP_DOUBLEEXPOSURE_PATTERN_REPEAT"));
    patternMethod->append(M("TP_DOUBLEEXPOSURE_PATTERN_MIRROR"));
    patternMethod->append(M("TP_DOUBLEEXPOSURE_PATTERN_RADIAL"));
    patternMethod->set_active(0);
    patternMethod->setPreferredWidth(150, 200);
    patternMethod->set_tooltip_text(M("TP_DOUBLEEXPOSURE_PATTERN_TOOLTIP"));
    patternMethod->connect(patternMethod->signal_changed().connect(sigc::mem_fun(*this, &DoubleExposure::patternChanged)));
    patternMethod->show();

    Gtk::Box* patternRow = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 4));
    Gtk::Label* patternLabel = Gtk::manage(new Gtk::Label(M("TP_DOUBLEEXPOSURE_PATTERN") + ":", Gtk::ALIGN_START));
    patternLabel->set_tooltip_text(M("TP_DOUBLEEXPOSURE_PATTERN_TOOLTIP"));
    patternRow->pack_start(*patternLabel, Gtk::PACK_SHRINK);
    patternRow->pack_start(*patternMethod, Gtk::PACK_EXPAND_WIDGET);
    patternRow->show_all();

    layerFlipH = Gtk::manage(new Gtk::CheckButton(M("TP_DOUBLEEXPOSURE_FLIPH")));
    layerFlipH->set_tooltip_text(M("TP_DOUBLEEXPOSURE_FLIPH_TOOLTIP"));
    flipConn = layerFlipH->signal_toggled().connect(sigc::mem_fun(*this, &DoubleExposure::flipToggled));
    layerFlipH->show();

    patternSpacing = Gtk::manage(new Adjuster(M("TP_DOUBLEEXPOSURE_PATTERN_SPACING"), 0.0, 200.0, 1.0, 0.0));
    patternSpacing->setAdjusterListener(this);
    patternSpacing->set_tooltip_text(M("TP_DOUBLEEXPOSURE_PATTERN_SPACING_TOOLTIP"));
    patternSpacing->set_no_show_all(true);
    patternSpacing->show();

    patternStagger = Gtk::manage(new Adjuster(M("TP_DOUBLEEXPOSURE_PATTERN_STAGGER"), 0.0, 100.0, 1.0, 0.0));
    patternStagger->setAdjusterListener(this);
    patternStagger->set_tooltip_text(M("TP_DOUBLEEXPOSURE_PATTERN_STAGGER_TOOLTIP"));
    patternStagger->set_no_show_all(true);
    patternStagger->show();

    patternCount = Gtk::manage(new Adjuster(M("TP_DOUBLEEXPOSURE_PATTERN_COUNT"), 1.0, 24.0, 1.0, 6.0));
    patternCount->setAdjusterListener(this);
    patternCount->set_tooltip_text(M("TP_DOUBLEEXPOSURE_PATTERN_COUNT_TOOLTIP"));
    patternCount->set_no_show_all(true);
    patternCount->show();

    edgeFeather = Gtk::manage(new Adjuster(M("TP_DOUBLEEXPOSURE_EDGEFEATHER"), 0.0, 100.0, 1.0, 35.0));
    edgeFeather->setAdjusterListener(this);
    edgeFeather->set_tooltip_text(M("TP_DOUBLEEXPOSURE_EDGEFEATHER_TOOLTIP"));
    edgeFeather->show();

    patternDiameter = Gtk::manage(new Adjuster(M("TP_DOUBLEEXPOSURE_PATTERN_DIAMETER"), 0.0, 200.0, 1.0, 60.0));
    patternDiameter->setAdjusterListener(this);
    patternDiameter->set_tooltip_text(M("TP_DOUBLEEXPOSURE_PATTERN_DIAMETER_TOOLTIP"));
    patternDiameter->set_no_show_all(true);
    patternDiameter->show();

    // Subject selection, segmented on the partner itself. The whole group is
    // hidden when this build has no segmentation model, rather than offered
    // and dead; the params stay in the file either way.
    subjectMethod = Gtk::manage(new MyComboBoxText());
    subjectMethod->append(M("TP_DOUBLEEXPOSURE_SUBJECT_OFF"));
    subjectMethod->append(M("TP_DOUBLEEXPOSURE_SUBJECT_SUBJECT"));
    subjectMethod->append(M("TP_DOUBLEEXPOSURE_SUBJECT_PERSON"));
    subjectMethod->append(M("TP_DOUBLEEXPOSURE_SUBJECT_SKY"));
    subjectMethod->append(M("TP_DOUBLEEXPOSURE_SUBJECT_VEGETATION"));
    subjectMethod->append(M("TP_DOUBLEEXPOSURE_SUBJECT_BUILDING"));
    subjectMethod->append(M("TP_DOUBLEEXPOSURE_SUBJECT_VEHICLE"));
    subjectMethod->append(M("TP_DOUBLEEXPOSURE_SUBJECT_ANIMAL"));
    subjectMethod->set_active(0);
    subjectMethod->setPreferredWidth(150, 200);
    subjectMethod->set_tooltip_text(M("TP_DOUBLEEXPOSURE_SUBJECT_TOOLTIP"));
    subjectMethod->connect(subjectMethod->signal_changed().connect(sigc::mem_fun(*this, &DoubleExposure::subjectChanged)));

    subjectRow = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 4));
    Gtk::Label* subjectLabel = Gtk::manage(new Gtk::Label(M("TP_DOUBLEEXPOSURE_SUBJECT") + ":", Gtk::ALIGN_START));
    subjectLabel->set_tooltip_text(M("TP_DOUBLEEXPOSURE_SUBJECT_TOOLTIP"));
    subjectRow->pack_start(*subjectLabel, Gtk::PACK_SHRINK);
    subjectRow->pack_start(*subjectMethod, Gtk::PACK_EXPAND_WIDGET);
    subjectRow->show_all();
    subjectRow->set_no_show_all(true);

    subjectOptionsRow = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 8));
    subjectInvert = Gtk::manage(new Gtk::CheckButton(M("TP_DOUBLEEXPOSURE_SUBJECT_INVERT")));
    subjectInvert->set_tooltip_text(M("TP_DOUBLEEXPOSURE_SUBJECT_INVERT_TOOLTIP"));
    subjectInvertConn = subjectInvert->signal_toggled().connect(sigc::mem_fun(*this, &DoubleExposure::subjectToggled));
    subjectCrop = Gtk::manage(new Gtk::CheckButton(M("TP_DOUBLEEXPOSURE_SUBJECT_CROP")));
    subjectCrop->set_tooltip_text(M("TP_DOUBLEEXPOSURE_SUBJECT_CROP_TOOLTIP"));
    subjectCropConn = subjectCrop->signal_toggled().connect(sigc::mem_fun(*this, &DoubleExposure::subjectToggled));
    subjectOptionsRow->pack_start(*subjectInvert, Gtk::PACK_SHRINK);
    subjectOptionsRow->pack_start(*subjectCrop, Gtk::PACK_SHRINK);
    subjectOptionsRow->show_all();
    subjectOptionsRow->set_no_show_all(true);

    subjectFeather = Gtk::manage(new Adjuster(M("TP_DOUBLEEXPOSURE_SUBJECT_FEATHER"), 0.0, 100.0, 1.0, 25.0));
    subjectFeather->setAdjusterListener(this);
    subjectFeather->set_tooltip_text(M("TP_DOUBLEEXPOSURE_SUBJECT_FEATHER_TOOLTIP"));
    subjectFeather->set_no_show_all(true);

    blendMethod = Gtk::manage(new MyComboBoxText());
    blendMethod->append(M("TP_DOUBLEEXPOSURE_BLEND_ADD"));
    blendMethod->append(M("TP_DOUBLEEXPOSURE_BLEND_SCREEN"));
    blendMethod->append(M("TP_DOUBLEEXPOSURE_BLEND_MULTIPLY"));
    blendMethod->append(M("TP_DOUBLEEXPOSURE_BLEND_LIGHTEN"));
    blendMethod->append(M("TP_DOUBLEEXPOSURE_BLEND_DARKEN"));
    blendMethod->append(M("TP_DOUBLEEXPOSURE_BLEND_DIFFERENCE"));
    blendMethod->set_active(0);
    blendMethod->setPreferredWidth(150, 200);
    blendMethod->connect(blendMethod->signal_changed().connect(sigc::mem_fun(*this, &DoubleExposure::blendChanged)));
    blendMethod->show();

    Gtk::Box* blendRow = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 4));
    Gtk::Label* blendLabel = Gtk::manage(new Gtk::Label(M("TP_DOUBLEEXPOSURE_BLEND") + ":", Gtk::ALIGN_START));
    blendRow->pack_start(*blendLabel, Gtk::PACK_SHRINK);
    blendRow->pack_start(*blendMethod, Gtk::PACK_EXPAND_WIDGET);
    blendRow->show_all();

    // Comparative bright/dark only: how the winner is picked and how wide
    // the hand-over between the two frames is. Both rows hide for the other
    // modes (no_show_all so a stray show_all() cannot bring them back).
    compareMode = Gtk::manage(new MyComboBoxText());
    compareMode->append(M("TP_DOUBLEEXPOSURE_COMPARE_LUMINANCE"));
    compareMode->append(M("TP_DOUBLEEXPOSURE_COMPARE_CHANNEL"));
    compareMode->set_active(0);
    compareMode->setPreferredWidth(150, 200);
    compareMode->set_tooltip_text(M("TP_DOUBLEEXPOSURE_COMPARE_TOOLTIP"));
    compareMode->connect(compareMode->signal_changed().connect(sigc::mem_fun(*this, &DoubleExposure::compareChanged)));

    compareRow = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 4));
    Gtk::Label* compareLabel = Gtk::manage(new Gtk::Label(M("TP_DOUBLEEXPOSURE_COMPARE") + ":", Gtk::ALIGN_START));
    compareLabel->set_tooltip_text(M("TP_DOUBLEEXPOSURE_COMPARE_TOOLTIP"));
    compareRow->pack_start(*compareLabel, Gtk::PACK_SHRINK);
    compareRow->pack_start(*compareMode, Gtk::PACK_EXPAND_WIDGET);
    compareRow->show_all();
    compareRow->set_no_show_all(true);

    softness = Gtk::manage(new Adjuster(M("TP_DOUBLEEXPOSURE_SOFTNESS"), 0.0, 2.0, 0.05, 0.5));
    softness->setAdjusterListener(this);
    softness->set_tooltip_text(M("TP_DOUBLEEXPOSURE_SOFTNESS_TOOLTIP"));
    softness->set_no_show_all(true);
    softness->show();

    // "Reveal in" gate: confine the selected layer to a luminance window.
    gateSource = Gtk::manage(new MyComboBoxText());
    gateSource->append(M("TP_DOUBLEEXPOSURE_GATE_BASE"));
    gateSource->append(M("TP_DOUBLEEXPOSURE_GATE_LAYER"));
    gateSource->set_active(0);
    gateSource->setPreferredWidth(150, 200);
    gateSource->connect(gateSource->signal_changed().connect(sigc::mem_fun(*this, &DoubleExposure::gateSourceChanged)));
    gateSource->show();

    Gtk::Box* gateRow = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 4));
    Gtk::Label* gateLabel = Gtk::manage(new Gtk::Label(M("TP_DOUBLEEXPOSURE_GATE") + ":", Gtk::ALIGN_START));
    gateLabel->set_tooltip_text(M("TP_DOUBLEEXPOSURE_GATE_TOOLTIP"));
    gateRow->pack_start(*gateLabel, Gtk::PACK_SHRINK);
    gateRow->pack_start(*gateSource, Gtk::PACK_EXPAND_WIDGET);
    gateRow->show_all();

    gateLow = Gtk::manage(new Adjuster(M("TP_DOUBLEEXPOSURE_GATE_LOW"), 0.0, 100.0, 1.0, 0.0));
    gateLow->setAdjusterListener(this);
    gateLow->show();

    gateHigh = Gtk::manage(new Adjuster(M("TP_DOUBLEEXPOSURE_GATE_HIGH"), 0.0, 100.0, 1.0, 35.0));
    gateHigh->setAdjusterListener(this);
    gateHigh->show();

    gateFeather = Gtk::manage(new Adjuster(M("TP_DOUBLEEXPOSURE_GATE_FEATHER"), 0.0, 100.0, 1.0, 33.0));
    gateFeather->setAdjusterListener(this);
    gateFeather->show();

    gateStrength = Gtk::manage(new Adjuster(M("TP_DOUBLEEXPOSURE_GATE_STRENGTH"), 0.0, 100.0, 1.0, 25.0));
    gateStrength->setAdjusterListener(this);
    gateStrength->set_tooltip_text(M("TP_DOUBLEEXPOSURE_GATE_TOOLTIP"));
    gateStrength->show();

    autoGain = Gtk::manage(new Gtk::CheckButton(M("TP_DOUBLEEXPOSURE_AUTOGAIN")));
    autoGain->set_active(true);
    autoGain->set_tooltip_text(M("TP_DOUBLEEXPOSURE_AUTOGAIN_TOOLTIP"));
    autoGain->signal_toggled().connect(sigc::mem_fun(*this, &DoubleExposure::autoGainToggled));
    autoGain->show();

    baseEv = Gtk::manage(new Adjuster(M("TP_DOUBLEEXPOSURE_BASEEV"), -4.0, 4.0, 0.05, 0.0));
    baseEv->setAdjusterListener(this);
    baseEv->show();

    // The film shoulder on the finished stack (group-wide, mode-independent).
    highlightLatitude = Gtk::manage(new Adjuster(M("TP_DOUBLEEXPOSURE_LATITUDE"), 0.0, 100.0, 1.0, 50.0));
    highlightLatitude->setAdjusterListener(this);
    highlightLatitude->set_tooltip_text(M("TP_DOUBLEEXPOSURE_LATITUDE_TOOLTIP"));
    highlightLatitude->show();

    Gtk::Box* chooseRow = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 2));
    chooseRow->pack_start(*chooseButton, Gtk::PACK_EXPAND_WIDGET);
    chooseRow->pack_start(*clearButton, Gtk::PACK_SHRINK);
    chooseRow->show();

    getSummaryBox()->pack_start(*chooseRow);
    getSummaryBox()->show_all();

    adjustSection = Gtk::manage(new AdvancedSection(M("TP_DOUBLEEXPOSURE_LAYERADJUST")));
    adjustSection->getContentBox()->pack_start(*layerOffsetX);
    adjustSection->getContentBox()->pack_start(*layerOffsetY);
    adjustSection->getContentBox()->pack_start(*layerScale);
    adjustSection->getContentBox()->pack_start(*layerRotate);
    adjustSection->getContentBox()->pack_start(*layerFlipH);
    adjustSection->getContentBox()->pack_start(*edgeFeather);
    adjustSection->setExpanded(false);

    patternSection = Gtk::manage(new AdvancedSection(M("TP_DOUBLEEXPOSURE_PATTERN")));
    patternSection->getContentBox()->pack_start(*patternRow);
    patternSection->getContentBox()->pack_start(*patternSpacing);
    patternSection->getContentBox()->pack_start(*patternStagger);
    patternSection->getContentBox()->pack_start(*patternCount);
    patternSection->getContentBox()->pack_start(*patternDiameter);
    patternSection->getContentBox()->pack_start(*subjectRow);
    patternSection->getContentBox()->pack_start(*subjectOptionsRow);
    patternSection->getContentBox()->pack_start(*subjectFeather);
    patternSection->setExpanded(false);

    pack_start(*layersBox);
    pack_start(*layerSelRow);
    pack_start(*layerEv);
    pack_start(*layerOpacity);
    pack_start(*adjustSection);
    pack_start(*patternSection);
    pack_start(*blendRow);
    pack_start(*compareRow);
    pack_start(*softness);
    pack_start(*gateRow);
    pack_start(*gateLow);
    pack_start(*gateHigh);
    pack_start(*gateFeather);
    pack_start(*gateStrength);
    pack_start(*autoGain);
    pack_start(*baseEv);
    pack_start(*highlightLatitude);

    updateSensitivity();
}

DoubleExposure::~DoubleExposure()
{
    *aliveToken_ = false;
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

void DoubleExposure::setOpenPartnerHandler(std::function<void(const Glib::ustring&, const Glib::ustring&)> handler)
{
    openPartnerHandler_ = std::move(handler);
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

    std::vector<Glib::ustring> missingThumbs;

    for (size_t i = 0; i < layers.size(); ++i) {
        const bool fileExists = Glib::file_test(layers[i].path, Glib::FILE_TEST_EXISTS);

        Gtk::Box* row = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 4));

        Gtk::Image* thumb = Gtk::manage(new Gtk::Image());
        const auto pix = rowThumbs_.find(layers[i].path);

        if (pix != rowThumbs_.end() && pix->second) {
            thumb->set(pix->second);
        } else {
            thumb->set_size_request(3 * ROW_THUMB_H / 2, ROW_THUMB_H);

            if (fileExists) {
                missingThumbs.push_back(layers[i].path);
            }
        }

        row->pack_start(*thumb, Gtk::PACK_SHRINK);

        Gtk::Label* name = Gtk::manage(new Gtk::Label(Glib::path_get_basename(layers[i].path), Gtk::ALIGN_START));
        name->set_ellipsize(Pango::ELLIPSIZE_MIDDLE);
        name->set_tooltip_text(layers[i].path);

        if (!fileExists) {
            name->set_tooltip_text(layers[i].path + "\n" + M("TP_DOUBLEEXPOSURE_MISSING"));
            name->get_style_context()->add_class("error-label");
        }

        row->pack_start(*name, Gtk::PACK_EXPAND_WIDGET);

        const size_t idx = i;

        Gtk::Button* edit = Gtk::manage(new Gtk::Button("\xE2\x9C\x8E")); // pencil
        edit->set_relief(Gtk::RELIEF_NONE);
        edit->set_tooltip_text(M("TP_DOUBLEEXPOSURE_EDIT"));
        edit->set_sensitive(fileExists);
        edit->signal_clicked().connect([this, idx]() {
            if (idx < layers.size() && openPartnerHandler_) {
                openPartnerHandler_(layers[idx].path, editedFilePath_);
            }
        });
        row->pack_start(*edit, Gtk::PACK_SHRINK);

        Gtk::Button* remove = Gtk::manage(new Gtk::Button());
        remove->set_image_from_icon_name("window-close-symbolic", Gtk::ICON_SIZE_MENU);
        remove->set_relief(Gtk::RELIEF_NONE);
        remove->set_tooltip_text(M("TP_DOUBLEEXPOSURE_REMOVE"));
        remove->signal_clicked().connect([this, idx]() { removeLayer(idx); });
        row->pack_start(*remove, Gtk::PACK_SHRINK);

        row->show_all();
        layersBox->pack_start(*row, Gtk::PACK_SHRINK);
    }

    requestRowThumbs(missingThumbs);
}

void DoubleExposure::requestRowThumbs(const std::vector<Glib::ustring>& paths)
{
    std::vector<Glib::ustring> needed;

    for (const auto& path : paths) {
        if (pendingRowThumbs_.insert(path).second) {
            needed.push_back(path);
        }
    }

    if (needed.empty()) {
        return;
    }

    auto alive = aliveToken_;

    detachQuietly(std::thread([this, alive, needed]() {
        for (const auto& path : needed) {
            if (!*alive) {
                return;
            }

            const auto pix = partnerthumb::load(path, ROW_THUMB_H, false);

            Glib::signal_idle().connect_once([this, alive, path, pix]() {
                if (!*alive) {
                    return;
                }

                pendingRowThumbs_.erase(path);

                if (pix) {
                    rowThumbs_[path] = pix;
                    rebuildLayerRows();
                }
            });
        }
    }));
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
    layerOffsetX->setValue(layers[idx].offsetX);
    layerOffsetY->setValue(layers[idx].offsetY);
    layerScale->setValue(layers[idx].scale);
    layerRotate->setValue(layers[idx].rotate);
    patternSpacing->setValue(layers[idx].patternSpacing);
    patternStagger->setValue(layers[idx].patternStagger);
    patternCount->setValue(layers[idx].patternCount);
    patternDiameter->setValue(layers[idx].patternDiameter);
    edgeFeather->setValue(layers[idx].edgeFeather);

    flipConn.block(true);
    layerFlipH->set_active(layers[idx].flipH);
    flipConn.block(false);

    patternMethod->block(true);
    patternMethod->set_active(static_cast<int>(layers[idx].pattern));
    patternMethod->block(false);

    subjectMethod->block(true);
    subjectMethod->set_active(static_cast<int>(layers[idx].maskClass));
    subjectMethod->block(false);

    subjectInvertConn.block(true);
    subjectInvert->set_active(layers[idx].maskInvert);
    subjectInvertConn.block(false);

    subjectCropConn.block(true);
    subjectCrop->set_active(layers[idx].cropToSubject);
    subjectCropConn.block(false);

    subjectFeather->setValue(layers[idx].maskFeather);

    blendMethod->block(true);
    blendMethod->set_active(static_cast<int>(layers[idx].blendMode));
    blendMethod->block(false);

    gateSource->block(true);
    gateSource->set_active(static_cast<int>(layers[idx].gateSource));
    gateSource->block(false);

    gateLow->setValue(layers[idx].gateLow);
    gateHigh->setValue(layers[idx].gateHigh);
    gateFeather->setValue(layers[idx].gateFeather);
    gateStrength->setValue(layers[idx].gateStrength);

    compareMode->block(true);
    compareMode->set_active(layers[idx].compare == DoubleExposureParams::Compare::CHANNEL ? 1 : 0);
    compareMode->block(false);
    softness->setValue(layers[idx].softness);

    updateSensitivity();
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
    layerOffsetX->set_sensitive(haveLayers);
    layerOffsetY->set_sensitive(haveLayers);
    layerScale->set_sensitive(haveLayers);
    layerRotate->set_sensitive(haveLayers);
    layerFlipH->set_sensitive(haveLayers);
    edgeFeather->set_sensitive(haveLayers);
    patternMethod->set_sensitive(haveLayers);
    blendMethod->set_sensitive(haveLayers);
    gateSource->set_sensitive(haveLayers);
    gateLow->set_sensitive(haveLayers);
    gateHigh->set_sensitive(haveLayers);
    gateFeather->set_sensitive(haveLayers);
    gateStrength->set_sensitive(haveLayers);
    highlightLatitude->set_sensitive(haveLayers);
    clearButton->set_visible(haveLayers);

    // Compare / softness only mean something for the comparative modes, and
    // softness only for the whole-pixel compare.
    const int idx = selectedLayerIndex();
    const bool comparative = idx >= 0
                             && (layers[idx].blendMode == DoubleExposureParams::BlendMode::LIGHTEN
                                 || layers[idx].blendMode == DoubleExposureParams::BlendMode::DARKEN);
    compareRow->set_visible(haveLayers && comparative);
    softness->set_visible(haveLayers && comparative && layers[idx].compare == DoubleExposureParams::Compare::LUMINANCE);

    // Gutters and brick courses belong to the grid patterns; the ring
    // controls belong to the radial one. The section opens itself when the
    // selected exposure is patterned, so the settings are never hidden behind
    // a closed header when they are actually doing something.
    const DoubleExposureParams::Pattern pattern =
        idx >= 0 ? layers[idx].pattern : DoubleExposureParams::Pattern::OFF;
    const bool tiled = pattern != DoubleExposureParams::Pattern::OFF;
    const bool grid = pattern == DoubleExposureParams::Pattern::REPEAT
                      || pattern == DoubleExposureParams::Pattern::MIRROR;
    const bool radial = pattern == DoubleExposureParams::Pattern::RADIAL;
    patternSpacing->set_visible(haveLayers && grid);
    patternStagger->set_visible(haveLayers && grid);
    patternCount->set_visible(haveLayers && radial);
    patternDiameter->set_visible(haveLayers && radial);

    if (haveLayers && tiled) {
        patternSection->setExpanded(true);
    }

    // Subject selection needs a segmentation model; without one the controls
    // are absent rather than present and inert.
#ifdef RT_AI_MASKING
    const bool haveSegmentation = rtengine::getAISegmentationEngine().isInitialized();
#else
    const bool haveSegmentation = false;
#endif
    const bool masked = idx >= 0 && layers[idx].maskClass != DoubleExposureParams::MaskClass::OFF;
    subjectRow->set_visible(haveSegmentation && haveLayers);
    subjectOptionsRow->set_visible(haveSegmentation && haveLayers && masked);
    subjectFeather->set_visible(haveSegmentation && haveLayers && masked);
    subjectMethod->set_sensitive(haveLayers);

    // The film-gain compensation only applies to light that stacks: enabled
    // additive layers.
    bool anyAdd = false;

    for (const auto& layer : layers) {
        if (layer.enabled && layer.blendMode == DoubleExposureParams::BlendMode::ADD) {
            anyAdd = true;
            break;
        }
    }

    autoGain->set_sensitive(anyAdd);
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
    current.autoGain = autoGain->get_active();
    current.baseEv = baseEv->getValue();
    current.highlightLatitude = highlightLatitude->getValue();

    BrowserFilter browserFilter;
    bool haveBrowserFilter = false;

    if (browserFilterProvider_) {
        browserFilter = browserFilterProvider_();
        haveBrowserFilter = true;
    }

    const Glib::ustring browserDir = browserDirProvider_ ? browserDirProvider_() : Glib::ustring();

    DoubleExposureDlg dialog(toplevel, editedFilePath_, current, haveBrowserFilter ? &browserFilter : nullptr, browserDir);

    Glib::ustring partnerToEdit;

    if (dialog.run() == Gtk::RESPONSE_OK) {
        const DoubleExposureParams result = dialog.getResult();

        disableListener();
        layers = result.layers;
        autoGain->set_active(result.autoGain);
        baseEv->setValue(result.baseEv);
        highlightLatitude->setValue(result.highlightLatitude);
        rebuildLayerRows();
        refreshLayerSelector();
        updateSensitivity();
        enableListener();

        layersEdited_ = true;
        autoGainEdited_ = true;

        if (!layers.empty()) {
            setEnabled(true);
        }

        if (listener) {
            listener->panelChanged(EvDELayers, Glib::ustring::compose("%1", layers.size()));
        }

        partnerToEdit = dialog.getEditRequestPath();
    }

    dialog.hide();

    if (!partnerToEdit.empty() && openPartnerHandler_) {
        openPartnerHandler_(partnerToEdit, editedFilePath_);
    }
}

void DoubleExposure::read(const ProcParams* pp, const ParamsEdited* pedited)
{
    disableListener();

    if (pedited) {
        baseEv->setEditedState(pedited->doubleExposure.baseEv ? Edited : UnEdited);
        highlightLatitude->setEditedState(pedited->doubleExposure.highlightLatitude ? Edited : UnEdited);
        set_inconsistent(multiImage && !pedited->doubleExposure.enabled);
        autoGain->set_inconsistent(!pedited->doubleExposure.autoGain);
    }

    setEnabled(pp->doubleExposure.enabled);
    layers = pp->doubleExposure.layers;
    layersEdited_ = false;
    autoGainEdited_ = false;

    autoGain->set_active(pp->doubleExposure.autoGain);
    baseEv->setValue(pp->doubleExposure.baseEv);
    highlightLatitude->setValue(pp->doubleExposure.highlightLatitude);

    rebuildLayerRows();
    refreshLayerSelector();
    updateSensitivity();

    enableListener();
}

void DoubleExposure::write(ProcParams* pp, ParamsEdited* pedited)
{
    pp->doubleExposure.enabled = getEnabled();
    pp->doubleExposure.layers = layers;
    pp->doubleExposure.autoGain = autoGain->get_active();
    pp->doubleExposure.baseEv = baseEv->getValue();
    pp->doubleExposure.highlightLatitude = highlightLatitude->getValue();

    // Per-layer edits (EV, opacity, blend, compare, softness, gate) land
    // directly in `layers` via their change handlers.

    if (pedited) {
        pedited->doubleExposure.enabled = !get_inconsistent();
        pedited->doubleExposure.layers = layersEdited_;
        pedited->doubleExposure.autoGain = autoGainEdited_ || !autoGain->get_inconsistent();
        pedited->doubleExposure.baseEv = baseEv->getEditedState();
        pedited->doubleExposure.highlightLatitude = highlightLatitude->getEditedState();
    }
}

void DoubleExposure::setDefaults(const ProcParams* defParams, const ParamsEdited* pedited)
{
    baseEv->setDefault(defParams->doubleExposure.baseEv);
    highlightLatitude->setDefault(defParams->doubleExposure.highlightLatitude);
    layerEv->setDefault(0.0);
    layerOpacity->setDefault(100.0);

    const DoubleExposureParams::Layer defLayer;
    softness->setDefault(defLayer.softness);
    layerOffsetX->setDefault(defLayer.offsetX);
    layerOffsetY->setDefault(defLayer.offsetY);
    layerScale->setDefault(defLayer.scale);
    layerRotate->setDefault(defLayer.rotate);
    patternSpacing->setDefault(defLayer.patternSpacing);
    patternStagger->setDefault(defLayer.patternStagger);
    patternCount->setDefault(defLayer.patternCount);
    patternDiameter->setDefault(defLayer.patternDiameter);
    edgeFeather->setDefault(defLayer.edgeFeather);
    subjectFeather->setDefault(defLayer.maskFeather);
    gateLow->setDefault(defLayer.gateLow);
    gateHigh->setDefault(defLayer.gateHigh);
    gateFeather->setDefault(defLayer.gateFeather);
    gateStrength->setDefault(defLayer.gateStrength);

    if (pedited) {
        baseEv->setDefaultEditedState(pedited->doubleExposure.baseEv ? Edited : UnEdited);
        highlightLatitude->setDefaultEditedState(pedited->doubleExposure.highlightLatitude ? Edited : UnEdited);
    } else {
        baseEv->setDefaultEditedState(Irrelevant);
        highlightLatitude->setDefaultEditedState(Irrelevant);
    }
}

void DoubleExposure::adjusterChanged(Adjuster* a, double newval)
{
    const bool isPlacementAdj = a == layerOffsetX || a == layerOffsetY || a == layerScale
                                || a == layerRotate || a == patternSpacing || a == patternStagger
                                || a == patternCount || a == patternDiameter || a == edgeFeather;
    const bool isSubjectAdj = a == subjectFeather;
    const bool isLayerAdj = a == layerEv || a == layerOpacity || a == softness
                            || isPlacementAdj || isSubjectAdj;
    const bool isGateAdj = a == gateLow || a == gateHigh || a == gateFeather || a == gateStrength;

    if (isLayerAdj || isGateAdj) {
        const int idx = selectedLayerIndex();

        if (idx >= 0) {
            if (a == layerEv) {
                layers[idx].ev = newval;
            } else if (a == layerOpacity) {
                layers[idx].opacity = newval;
            } else if (a == softness) {
                layers[idx].softness = newval;
            } else if (a == layerOffsetX) {
                layers[idx].offsetX = newval;
            } else if (a == layerOffsetY) {
                layers[idx].offsetY = newval;
            } else if (a == layerScale) {
                layers[idx].scale = newval;
            } else if (a == layerRotate) {
                layers[idx].rotate = newval;
            } else if (a == patternSpacing) {
                layers[idx].patternSpacing = newval;
            } else if (a == patternStagger) {
                layers[idx].patternStagger = newval;
            } else if (a == patternCount) {
                layers[idx].patternCount = newval;
            } else if (a == patternDiameter) {
                layers[idx].patternDiameter = newval;
            } else if (a == edgeFeather) {
                layers[idx].edgeFeather = newval;
            } else if (a == subjectFeather) {
                layers[idx].maskFeather = newval;
            } else if (a == gateLow) {
                layers[idx].gateLow = newval;
            } else if (a == gateHigh) {
                layers[idx].gateHigh = newval;
            } else if (a == gateFeather) {
                layers[idx].gateFeather = newval;
            } else {
                layers[idx].gateStrength = newval;
            }

            layersEdited_ = true;
            autoEnable();

            if (listener && getEnabled()) {
                listener->panelChanged(isGateAdj ? EvDEGate
                                       : (a == softness ? EvDESoftness
                                          : (isSubjectAdj ? EvDESubject
                                             : (isPlacementAdj ? EvDEPlacement : EvDELayerSettings))),
                                       a->getTextValue());
            }
        }

        return;
    }

    autoEnable();

    if (listener && getEnabled()) {
        if (a == baseEv) {
            listener->panelChanged(EvDEBaseEv, a->getTextValue());
        } else if (a == highlightLatitude) {
            listener->panelChanged(EvDELatitude, a->getTextValue());
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
    const int idx = selectedLayerIndex();

    if (idx < 0) {
        return;
    }

    const int blendRow = blendMethod->get_active_row_number();
    layers[idx].blendMode = static_cast<DoubleExposureParams::BlendMode>(blendRow < 0 ? 0 : blendRow);
    layersEdited_ = true;
    updateSensitivity();
    autoEnable();

    if (listener && getEnabled()) {
        listener->panelChanged(EvDEBlend, blendMethod->get_active_text());
    }
}

void DoubleExposure::compareChanged()
{
    const int idx = selectedLayerIndex();

    if (idx < 0) {
        return;
    }

    layers[idx].compare = compareMode->get_active_row_number() == 1
                          ? DoubleExposureParams::Compare::CHANNEL
                          : DoubleExposureParams::Compare::LUMINANCE;

    // Layers migrated from per-channel files carry softness 0 (the legacy
    // pick has no hand-over); switching them to whole-pixel compare should
    // not land in the hard-edged corner case, so give them the default band.
    if (layers[idx].compare == DoubleExposureParams::Compare::LUMINANCE && layers[idx].softness <= 0.0) {
        const DoubleExposureParams::Layer defLayer;
        layers[idx].softness = defLayer.softness;
        softness->setValue(layers[idx].softness);
    }

    layersEdited_ = true;
    updateSensitivity();
    autoEnable();

    if (listener && getEnabled()) {
        listener->panelChanged(EvDECompare, compareMode->get_active_text());
    }
}

void DoubleExposure::gateSourceChanged()
{
    const int idx = selectedLayerIndex();

    if (idx < 0) {
        return;
    }

    layers[idx].gateSource = gateSource->get_active_row_number() == 1
                             ? DoubleExposureParams::GateSource::LAYER
                             : DoubleExposureParams::GateSource::BASE;
    layersEdited_ = true;
    autoEnable();

    if (listener && getEnabled()) {
        listener->panelChanged(EvDEGate, gateSource->get_active_text());
    }
}

void DoubleExposure::patternChanged()
{
    const int idx = selectedLayerIndex();

    if (idx < 0) {
        return;
    }

    const int row = patternMethod->get_active_row_number();
    layers[idx].pattern = static_cast<DoubleExposureParams::Pattern>(row < 0 ? 0 : row);
    layersEdited_ = true;
    updateSensitivity();
    autoEnable();

    if (listener && getEnabled()) {
        listener->panelChanged(EvDEPlacement, patternMethod->get_active_text());
    }
}

void DoubleExposure::flipToggled()
{
    const int idx = selectedLayerIndex();

    if (idx < 0) {
        return;
    }

    layers[idx].flipH = layerFlipH->get_active();
    layersEdited_ = true;
    autoEnable();

    if (listener && getEnabled()) {
        listener->panelChanged(EvDEPlacement,
                               layerFlipH->get_active() ? M("GENERAL_ENABLED") : M("GENERAL_DISABLED"));
    }
}

void DoubleExposure::subjectChanged()
{
    const int idx = selectedLayerIndex();

    if (idx < 0) {
        return;
    }

    const int row = subjectMethod->get_active_row_number();
    layers[idx].maskClass = static_cast<DoubleExposureParams::MaskClass>(row < 0 ? 0 : row);
    layersEdited_ = true;
    updateSensitivity();
    autoEnable();

    if (listener && getEnabled()) {
        listener->panelChanged(EvDESubject, subjectMethod->get_active_text());
    }
}

void DoubleExposure::subjectToggled()
{
    const int idx = selectedLayerIndex();

    if (idx < 0) {
        return;
    }

    layers[idx].maskInvert = subjectInvert->get_active();
    layers[idx].cropToSubject = subjectCrop->get_active();
    layersEdited_ = true;
    autoEnable();

    if (listener && getEnabled()) {
        listener->panelChanged(EvDESubject, M("HISTORY_CHANGED"));
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
    highlightLatitude->showEditedCB();
}
