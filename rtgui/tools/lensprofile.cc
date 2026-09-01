/*
*  This file is part of RawTherapee.
*
*  Copyright (c) 2012 Oliver Duis <oduis@oliverduis.de>
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
#include <map>
#include <set>
#include <iostream>
#include <sstream>
#include <vector>

#include <glibmm/ustring.h>

#include "lensprofile.h"

#include "guiutils.h"
#include "rtimage.h"
#include "rtscalable.h"
#include "options.h"

#include "rtengine/lcp.h"
#include "rtengine/procparams.h"
#include "rtengine/rtlensfun.h"
#include "rtengine/lensmetadata.h"

using namespace rtengine;
using namespace rtengine::procparams;

const Glib::ustring LensProfilePanel::TOOL_NAME = "lensprof";

namespace {

bool tokensMatch(const Glib::ustring& hayFold, const std::vector<Glib::ustring>& tokens)
{
    for (const auto& t : tokens) {
        if (hayFold.find(t) == Glib::ustring::npos) {
            return false;
        }
    }

    return true;
}

std::vector<Glib::ustring> splitTokens(const Glib::ustring& text)
{
    std::vector<Glib::ustring> tokens;

    for (const auto& t : Glib::Regex::split_simple("\\s+", text.casefold())) {
        if (!t.empty()) {
            tokens.push_back(t);
        }
    }

    return tokens;
}

} // namespace

LensListPicker::LensListPicker() :
    displayCol_(nullptr),
    label_(Gtk::manage(new Gtk::Label())),
    popover_(nullptr),
    search_(nullptr),
    view_(nullptr)
{
    label_->set_xalign(0.f);
    label_->set_ellipsize(Pango::ELLIPSIZE_MIDDLE);
    add(*label_);
    label_->show();
    set_size_request(RTScalable::scalePixelSize(50), -1);

    signal_clicked().connect([this]() {
        if (!popover_) {
            return;
        }

        if (popover_->is_visible()) {
            popover_->popdown();
        } else {
            openPopover();
        }
    });
}

void LensListPicker::setModel(const Glib::RefPtr<Gtk::TreeStore>& model,
                              const Gtk::TreeModelColumn<Glib::ustring>& displayCol)
{
    model_ = model;
    displayCol_ = &displayCol;

    filter_ = Gtk::TreeModelFilter::create(model_);
    filter_->set_visible_func([this](const Gtk::TreeModel::const_iterator& it) {
        return rowVisible(it);
    });

    view_ = Gtk::manage(new Gtk::TreeView(filter_));
    view_->set_headers_visible(false);
    view_->set_enable_search(false);
    view_->set_show_expanders(false);
    view_->set_level_indentation(RTScalable::scalePixelSize(12));
    view_->set_activate_on_single_click(true);
    view_->get_selection()->set_mode(Gtk::SELECTION_BROWSE);
    // Only model rows are choices; the makes are section headers.
    view_->get_selection()->set_select_function(
        [](const Glib::RefPtr<Gtk::TreeModel>&, const Gtk::TreeModel::Path& path, bool) {
            return path.size() == 2;
        });

    Gtk::CellRendererText* const renderer = Gtk::manage(new Gtk::CellRendererText());
    renderer->property_ellipsize() = Pango::ELLIPSIZE_END;
    Gtk::TreeViewColumn* const col = Gtk::manage(new Gtk::TreeViewColumn());
    col->pack_start(*renderer, true);
    col->set_cell_data_func(*renderer, [this, renderer](Gtk::CellRenderer*, const Gtk::TreeModel::iterator& it) {
        renderer->property_text() = static_cast<Glib::ustring>((*it)[*displayCol_]);
        renderer->property_weight() = (*it).parent() ? Pango::WEIGHT_NORMAL : Pango::WEIGHT_BOLD;
    });
    view_->append_column(*col);

    view_->signal_row_activated().connect([this](const Gtk::TreeModel::Path& path, Gtk::TreeViewColumn*) {
        if (path.size() != 2) {
            return;
        }

        const auto it = filter_->get_iter(path);

        if (it) {
            commitRow(filter_->convert_iter_to_child_iter(it));
            popover_->popdown();
        }
    });

    search_ = Gtk::manage(new Gtk::SearchEntry());
    search_->signal_changed().connect([this]() {
        filter_->refilter();
        view_->expand_all();
    });
    // Enter takes the first model row the search left visible.
    search_->signal_activate().connect([this]() {
        for (const auto& make : filter_->children()) {
            const auto& models = make.children();

            if (!models.empty()) {
                commitRow(filter_->convert_iter_to_child_iter(models.begin()));
                popover_->popdown();
                return;
            }
        }
    });

    Gtk::ScrolledWindow* const scrolled = Gtk::manage(new Gtk::ScrolledWindow());
    scrolled->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    scrolled->set_min_content_width(RTScalable::scalePixelSize(300));
    scrolled->set_max_content_height(RTScalable::scalePixelSize(420));
    scrolled->set_propagate_natural_height(true);
    scrolled->add(*view_);

    Gtk::Box* const box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 4));
    box->pack_start(*search_, Gtk::PACK_SHRINK);
    box->pack_start(*scrolled, Gtk::PACK_EXPAND_WIDGET);

    popover_ = Gtk::manage(new Gtk::Popover(*this));
    popover_->set_position(Gtk::POS_BOTTOM);
    popover_->add(*box);
    popover_->show_all_children();
}

Gtk::TreeModel::iterator LensListPicker::get_active() const
{
    return active_;
}

void LensListPicker::set_active(const Gtk::TreeModel::iterator& iter)
{
    commitRow(iter);
}

void LensListPicker::set_active(int)
{
    // The only index the panel ever passes is -1: clear, as on a combo.
    if (!active_) {
        return;
    }

    active_ = Gtk::TreeModel::iterator();
    updateLabel();
    changed_.emit();
}

sigc::signal<void>& LensListPicker::signal_changed()
{
    return changed_;
}

void LensListPicker::openPopover()
{
    search_->set_text("");
    view_->expand_all();

    const auto sel = view_->get_selection();
    sel->unselect_all();

    if (active_) {
        const auto it = filter_->convert_child_iter_to_iter(active_);

        if (it) {
            const auto path = filter_->get_path(it);
            sel->select(it);
            view_->scroll_to_row(path, 0.5f);
        }
    }

    popover_->popup();
    search_->grab_focus();
}

void LensListPicker::commitRow(const Gtk::TreeModel::iterator& storeIter)
{
    if (!storeIter) {
        set_active(-1);
        return;
    }

    if (active_ && model_->get_path(active_) == model_->get_path(storeIter)) {
        return;
    }

    active_ = storeIter;
    updateLabel();
    changed_.emit();
}

bool LensListPicker::rowVisible(const Gtk::TreeModel::const_iterator& iter) const
{
    if (!search_) {
        return true;
    }

    const Glib::ustring text = search_->get_text();

    if (text.empty()) {
        return true;
    }

    const auto tokens = splitTokens(text);
    const auto& row = *iter;
    const Glib::ustring own = static_cast<Glib::ustring>(row[*displayCol_]).casefold();
    const auto parent = row.parent();

    if (parent) {
        return tokensMatch(static_cast<Glib::ustring>((*parent)[*displayCol_]).casefold() + " " + own, tokens);
    }

    // A make row survives while any of its models does.
    for (const auto& child : row.children()) {
        if (tokensMatch(own + " " + static_cast<Glib::ustring>(child[*displayCol_]).casefold(), tokens)) {
            return true;
        }
    }

    return false;
}

void LensListPicker::updateLabel()
{
    if (active_) {
        const Glib::ustring text = (*active_)[*displayCol_];
        label_->set_text(text);
        set_tooltip_text(text);
    } else {
        label_->set_text("");
        set_tooltip_text("");
    }
}

LensProfilePanel::LensProfilePanel() :
    FoldableToolPanel(this, TOOL_NAME, M("TP_LENSPROFILE_LABEL"), false, true),
    lcModeChanged(false),
    lcpFileChanged(false),
    useDistChanged(false),
    useVignChanged(false),
    useCAChanged(false),
    useLensfunChanged(false),
    lensfunAutoChanged(false),
    lensfunCameraChanged(false),
    lensfunLensChanged(false),
    allowFocusDep(true),
    isRaw(true),
    metadata(nullptr),
    modesGrid(Gtk::manage(new Gtk::Grid())),
    distGrid(Gtk::manage((new Gtk::Grid()))),
    corrUnchangedRB(Gtk::manage((new Gtk::RadioButton(M("GENERAL_UNCHANGED"))))),
    corrOffRB(Gtk::manage((new Gtk::RadioButton(corrGroup, M("GENERAL_NONE"))))),
    corrMetadata(Gtk::manage((new Gtk::RadioButton(corrGroup, M("TP_LENSPROFILE_CORRECTION_METADATA"))))),
    corrLensfunAutoRB(Gtk::manage((new Gtk::RadioButton(corrGroup, M("TP_LENSPROFILE_CORRECTION_AUTOMATCH"))))),
    corrLensfunManualRB(Gtk::manage((new Gtk::RadioButton(corrGroup, M("TP_LENSPROFILE_CORRECTION_MANUAL"))))),
    corrLcpFileRB(Gtk::manage((new Gtk::RadioButton(corrGroup, M("TP_LENSPROFILE_CORRECTION_LCPFILE"))))),
    corrLcpFileChooser(Gtk::manage((new MyFileChooserButton(M("TP_LENSPROFILE_LABEL"), Gtk::FILE_CHOOSER_ACTION_OPEN)))),
    lensfunCamerasLbl(Gtk::manage((new Gtk::Label(M("EXIFFILTER_CAMERA"))))),
    lensfunCameras(Gtk::manage((new LensListPicker()))),
    lensfunLensesLbl(Gtk::manage((new Gtk::Label(M("EXIFFILTER_LENS"))))),
    lensfunLenses(Gtk::manage((new LensListPicker()))),
    warning(Gtk::manage(new RTImage("warning", Gtk::ICON_SIZE_LARGE_TOOLBAR))),
    ckbUseDist(Gtk::manage((new Gtk::CheckButton(M("TP_LENSPROFILE_USE_GEOMETRIC"))))),
    ckbUseVign(Gtk::manage((new Gtk::CheckButton(M("TP_LENSPROFILE_USE_VIGNETTING"))))),
    ckbUseCA(Gtk::manage((new Gtk::CheckButton(M("TP_LENSPROFILE_USE_CA")))))
{
    if (!lf) {
        lf = new LFDbHelper();
    }

    // Main containers:

    correctExpanded = false;

    modesGrid->get_style_context()->add_class("grid-spacing");
    setExpandAlignProperties(modesGrid, true, false, Gtk::ALIGN_FILL, Gtk::ALIGN_CENTER);

    distGrid->get_style_context()->add_class("grid-spacing");
    setExpandAlignProperties(distGrid, true, false, Gtk::ALIGN_FILL, Gtk::ALIGN_CENTER);

    // Mode choice widgets:

    setExpandAlignProperties(corrLcpFileChooser, true, false, Gtk::ALIGN_FILL, Gtk::ALIGN_CENTER);

    // Manually-selected profile widgets:

    setExpandAlignProperties(lensfunCamerasLbl, false, false, Gtk::ALIGN_END, Gtk::ALIGN_CENTER);

    lensfunCameras->setModel(lf->lensfunCameraModel, lf->lensfunModelCam.model);
    setExpandAlignProperties(lensfunCameras, true, false, Gtk::ALIGN_FILL, Gtk::ALIGN_CENTER);

    setExpandAlignProperties(lensfunLensesLbl, false, false, Gtk::ALIGN_END, Gtk::ALIGN_CENTER);

    lensfunLenses->setModel(lf->lensfunLensModel, lf->lensfunModelLens.prettylens);
    setExpandAlignProperties(lensfunLenses, true, false, Gtk::ALIGN_FILL, Gtk::ALIGN_CENTER);

    warning->set_tooltip_text(M("TP_LENSPROFILE_LENS_WARNING"));
    warning->hide();

    // LCP file filter config:

    const Glib::RefPtr<Gtk::FileFilter> filterLCP = Gtk::FileFilter::create();
    filterLCP->set_name(M("FILECHOOSER_FILTER_LCP"));
    filterLCP->add_pattern("*.lcp");
    filterLCP->add_pattern("*.LCP");
    corrLcpFileChooser->add_filter(filterLCP);

    const Glib::ustring defDir = LCPStore::getInstance()->getDefaultCommonDirectory();

    auto& options = App::get().mut_options();
    if (!defDir.empty()) {
#ifdef _WIN32
        corrLcpFileChooser->set_show_hidden(true);  // ProgramData is hidden on Windows
#endif
        corrLcpFileChooser->set_current_folder(defDir);
    } else if (!options.lastLensProfileDir.empty()) {
        corrLcpFileChooser->set_current_folder(options.lastLensProfileDir);
    }

    bindCurrentFolder(*corrLcpFileChooser, options.lastLensProfileDir);

    // Top-level mode radios:

    // Every source of a lens correction is listed here, always visible, so
    // the choice is discoverable. Previously "From file metadata" and "LCP
    // file" lived inside the indented sub-box, which only appeared after
    // picking "Manually selected" — so from a fresh enable there was no way
    // to see that they existed.
    // "None" has to be on screen too. This tool runs in flat mode, which
    // hides the expander header — and with it the enable checkbox that used
    // to serve as the off switch. Without a visible "None" there was no way
    // to turn the correction off, and (worse) a photo with no correction
    // loaded as "disabled", which hid every option and left the tool blank.
    Gtk::Label* sourceHeader = Gtk::manage(new Gtk::Label());
    sourceHeader->set_markup(Glib::ustring("<b>") + M("TP_LENSPROFILE_SOURCE_HEADER") + "</b>");
    sourceHeader->set_halign(Gtk::ALIGN_START);
    sourceHeader->set_margin_bottom(2);

    // Sits beside the header: whether a photo that carries no correction
    // (or a manual one from before the lens database was available) should
    // be put on automatic matching when it opens.
    defaultAutoChk = Gtk::manage(new Gtk::CheckButton(M("TP_LENSPROFILE_DEFAULT_AUTO")));
    defaultAutoChk->set_tooltip_text(M("TP_LENSPROFILE_DEFAULT_AUTO_TOOLTIP"));
    defaultAutoChk->set_active(App::get().options().lensProfDefaultAuto);
    defaultAutoChk->signal_toggled().connect([this]() {
        App::get().mut_options().lensProfDefaultAuto = defaultAutoChk->get_active();
    });

    Gtk::Box* sourceHeaderRow = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 8));
    sourceHeaderRow->pack_start(*sourceHeader, Gtk::PACK_SHRINK);
    sourceHeaderRow->pack_end(*defaultAutoChk, Gtk::PACK_SHRINK);

    // Automatic matching is otherwise silent about what it found; this says
    // which camera and lens it settled on, right under the option.
    autoMatchLabel = Gtk::manage(new Gtk::Label());
    autoMatchLabel->set_halign(Gtk::ALIGN_START);
    autoMatchLabel->set_margin_start(20);
    autoMatchLabel->set_line_wrap(true);
    autoMatchLabel->set_ellipsize(Pango::ELLIPSIZE_MIDDLE);
    autoMatchLabel->set_no_show_all(true);

    modesGrid->attach(*sourceHeaderRow, 0, 0, 3, 1);
    modesGrid->attach(*corrOffRB, 0, 1, 3, 1);
    modesGrid->attach(*corrLensfunAutoRB, 0, 2, 3, 1);
    modesGrid->attach(*autoMatchLabel, 0, 3, 3, 1);
    modesGrid->attach(*corrLensfunManualRB, 0, 4, 3, 1);
    modesGrid->attach(*corrMetadata, 0, 5, 3, 1);
    modesGrid->attach(*corrLcpFileRB, 0, 6, 3, 1);

    // Sub-options belonging to whichever source is selected: the camera and
    // lens pickers for manual matching, the file chooser for an LCP.

    Gtk::Grid *manualGrid = Gtk::manage(new Gtk::Grid());
    manualGrid->get_style_context()->add_class("grid-spacing");
    setExpandAlignProperties(manualGrid, true, false, Gtk::ALIGN_FILL, Gtk::ALIGN_CENTER);

    manualGrid->attach(*lensfunCamerasLbl, 0, 0, 1, 1);
    manualGrid->attach(*lensfunCameras, 1, 0, 1, 1);
    manualGrid->attach(*lensfunLensesLbl, 0, 1, 1, 1);
    manualGrid->attach(*lensfunLenses, 1, 1, 1, 1);
    manualGrid->attach(*warning, 2, 0, 1, 2);
    manualGrid->attach(*corrLcpFileChooser, 0, 2, 2, 1);

    manualSubBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
    manualSubBox->set_margin_start(16);
    manualSubBox->pack_start(*manualGrid, Gtk::PACK_SHRINK);
    manualSubBox->set_no_show_all(true);

    // Clickable "Correct" header
    correctLabel = Gtk::manage(new Gtk::Label());
    correctLabel->set_markup(Glib::ustring("<b>\u25B8 ") + M("TP_LENSPROFILE_USE_HEADER") + "</b>");
    correctLabel->set_halign(Gtk::ALIGN_START);
    correctLabel->set_margin_top(8);
    correctLabel->set_margin_bottom(2);

    Gtk::Button *correctHeader = Gtk::manage(new Gtk::Button());
    correctHeader->set_name("CollapsibleHeader");
    correctHeader->set_relief(Gtk::RELIEF_NONE);
    correctHeader->set_can_focus(false);
    correctHeader->set_halign(Gtk::ALIGN_START);
    correctHeader->add(*correctLabel);
    correctHeader->signal_clicked().connect(
        sigc::mem_fun(*this, &LensProfilePanel::toggleCorrect));

    // Correct content (hidden by default)
    correctContent = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
    correctContent->set_no_show_all(true);

    distGrid->attach(*ckbUseDist, 0, 0, 1, 1);
    distGrid->attach(*ckbUseVign, 0, 1, 1, 1);
    distGrid->attach(*ckbUseCA, 0, 2, 1, 1);

    correctContent->pack_start(*distGrid, Gtk::PACK_SHRINK);

    // Wrap all content in a single hideable box
    contentWrapper = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
    contentWrapper->pack_start(*modesGrid, Gtk::PACK_SHRINK);
    contentWrapper->pack_start(*manualSubBox, Gtk::PACK_SHRINK);
    contentWrapper->pack_start(*correctHeader, Gtk::PACK_SHRINK);
    contentWrapper->pack_start(*correctContent, Gtk::PACK_SHRINK);
    contentWrapper->set_no_show_all(true);
    pack_start(*contentWrapper, Gtk::PACK_EXPAND_WIDGET);

    // Signals:

    conLCPFile = corrLcpFileChooser->signal_file_set().connect(sigc::mem_fun(*this, &LensProfilePanel::onLCPFileChanged));
    conUseDist = ckbUseDist->signal_toggled().connect(sigc::mem_fun(*this, &LensProfilePanel::onUseDistChanged));
    ckbUseVign->signal_toggled().connect(sigc::mem_fun(*this, &LensProfilePanel::onUseVignChanged));
    ckbUseCA->signal_toggled().connect(sigc::mem_fun(*this, &LensProfilePanel::onUseCAChanged));

    lensfunCameras->signal_changed().connect(sigc::mem_fun(*this, &LensProfilePanel::onLensfunCameraChanged));
    lensfunLenses->signal_changed().connect(sigc::mem_fun(*this, &LensProfilePanel::onLensfunLensChanged));
    corrMetadata->signal_toggled().connect(sigc::bind(sigc::mem_fun(*this, &LensProfilePanel::onCorrModeChanged), corrMetadata));
    corrLensfunAutoRB->signal_toggled().connect(sigc::bind(sigc::mem_fun(*this, &LensProfilePanel::onCorrModeChanged), corrLensfunAutoRB));
    corrLensfunManualRB->signal_toggled().connect(sigc::bind(sigc::mem_fun(*this, &LensProfilePanel::onCorrModeChanged), corrLensfunManualRB));
    corrLcpFileRB->signal_toggled().connect(sigc::bind(sigc::mem_fun(*this, &LensProfilePanel::onCorrModeChanged), corrLcpFileRB));
    corrOffRB->signal_toggled().connect(sigc::bind(sigc::mem_fun(*this, &LensProfilePanel::onCorrModeChanged), corrOffRB));
}

LensProfilePanel::~LensProfilePanel()
{
    // The deferred "apply automatic" callback captures this.
    autoDefaultConn_.disconnect();
}

void LensProfilePanel::read(const rtengine::procparams::ProcParams* pp, const ParamsEdited* pedited)
{
    disableListener();
    conUseDist.block(true);

    // corrLensfunAutoRB->set_sensitive(true);

    // Helper to show content + manual sub-box
    auto showContent = [this](bool showManualSub) {
        contentWrapper->set_no_show_all(false);
        contentWrapper->show_all();
        contentWrapper->set_no_show_all(true);
        if (showManualSub) {
            manualSubBox->set_no_show_all(false);
            manualSubBox->show_all();
            manualSubBox->set_no_show_all(true);
        } else {
            manualSubBox->hide();
        }
        // Keep correctContent in its current state
        if (!correctExpanded) {
            correctContent->hide();
        }
    };

    // "Auto by default": a photo carrying no correction, or a manual one with
    // nothing actually chosen (every manual profile saved while the lens
    // database was missing looks like this), starts on automatic matching.
    // A manual profile that names a real lens is left alone — that was a
    // deliberate choice and overriding it would lose work.
    const bool autoDefaultWanted = App::get().options().lensProfDefaultAuto
        && (pp->lensProf.lcMode == procparams::LensProfParams::LcMode::NONE
            || (pp->lensProf.lcMode == procparams::LensProfParams::LcMode::LENSFUNMANUAL
                && pp->lensProf.lfCameraModel.empty() && pp->lensProf.lfLens.empty()));

    if (autoDefaultWanted) {
        setEnabled(true);
        corrLensfunAutoRB->set_active(true);
        showContent(true);
        updateSubOptionVisibility();

        // Apply it for real once the load settles, so the rendered photo and
        // the panel agree instead of the panel merely claiming automatic.
        autoDefaultConn_.disconnect();
        autoDefaultConn_ = Glib::signal_idle().connect([this]() -> bool {
            if (listener && corrLensfunAutoRB->get_active()) {
                listener->panelChanged(EvLensCorrMode, M("TP_LENSPROFILE_CORRECTION_AUTOMATCH"));
            }
            return false;
        });

        conUseDist.block(false);
        enableListener();
        return;
    }

    switch (pp->lensProf.lcMode) {
        case procparams::LensProfParams::LcMode::LCP: {
            setEnabled(true);
            corrLcpFileRB->set_active(true);
            showContent(true);
            updateSubOptionVisibility();
            break;
        }

        case procparams::LensProfParams::LcMode::LENSFUNAUTOMATCH: {
            setEnabled(true);
            corrLensfunAutoRB->set_active(true);
            if (batchMode) {
                setManualParamsVisibility(false);
            }
            showContent(false);
            break;
        }

        case procparams::LensProfParams::LcMode::LENSFUNMANUAL: {
            setEnabled(true);
            corrLensfunManualRB->set_active(true);
            showContent(true);
            updateSubOptionVisibility();
            break;
        }

        case procparams::LensProfParams::LcMode::METADATA: {
            if (metadata) {
                auto metadataCorrection= rtengine::MetadataLensCorrectionFinder::findCorrection(metadata);
                if (metadataCorrection) {
                    setEnabled(true);
                    corrMetadata->set_active(true);
                    corrMetadata->set_sensitive(true);
                    showContent(true);
                    updateSubOptionVisibility();
                } else {
                    corrMetadata->set_sensitive(false);
                    setEnabled(false);
                    contentWrapper->hide();
                }
            } else {
                corrMetadata->set_sensitive(false);
                setEnabled(false);
                contentWrapper->hide();
            }
            break;
        }

        case procparams::LensProfParams::LcMode::NONE: {
            // The common case for a photo with no correction yet. Keep the
            // choices on screen — hiding them made the tool look empty and
            // unusable, which is the state most photos opened in.
            setEnabled(false);
            corrOffRB->set_active(true);
            showContent(false);
            updateSubOptionVisibility();
            break;
        }
    }

    if (pp->lensProf.lcMode == procparams::LensProfParams::LcMode::LCP) {
        if (pp->lensProf.lcpFile.empty()) {
            const Glib::ustring lastFolder = corrLcpFileChooser->get_current_folder();
            corrLcpFileChooser->set_current_folder(lastFolder);
            corrLcpFileChooser->unselect_all();
            bindCurrentFolder(*corrLcpFileChooser, App::get().mut_options().lastLensProfileDir);
            updateLCPDisabled(false);
        }
        else if (LCPStore::getInstance()->isValidLCPFileName(pp->lensProf.lcpFile)) {
            corrLcpFileChooser->set_filename(pp->lensProf.lcpFile);

            if (corrLcpFileRB->get_active()) {
                updateLCPDisabled(true);
            }
        }
        else {
            corrLcpFileChooser->unselect_filename(corrLcpFileChooser->get_filename());
            updateLCPDisabled(false);
        }
    }

    const LFDatabase* const db = LFDatabase::getInstance();
    LFCamera c;

    if (pp->lensProf.lfAutoMatch()) {
        if (metadata) {
            c = db->findCamera(metadata->getMake(), metadata->getModel(), true);
            setLensfunCamera(c.getMake(), c.getModel());
        }
    } else if (pp->lensProf.lfManual()) {
        setLensfunCamera(pp->lensProf.lfCameraMake, pp->lensProf.lfCameraModel);
    }

    if (pp->lensProf.lfAutoMatch()) {
        if (metadata) {
            const LFLens l = db->findLens(c, metadata->getLens(), true);
            setLensfunLens(l.getLens());
        }
    } else if (pp->lensProf.lfManual()) {
        setLensfunLens(pp->lensProf.lfLens);
    }


    /*
   if (!batchMode && !checkLensfunCanCorrect(true)) {
        if (corrLensfunAutoRB->get_active()) {
            corrOffRB->set_active(true);
        }

        corrLensfunAutoRB->set_sensitive(false);
    }

    if (!batchMode && corrLensfunManualRB->get_active() && !checkLensfunCanCorrect(false)) {
        corrOffRB->set_active(true);
    }
    */

    ckbUseDist->set_active(pp->lensProf.useDist);
    ckbUseVign->set_active(pp->lensProf.useVign);
    ckbUseCA->set_active(pp->lensProf.useCA);

    if (pedited) {
        corrUnchangedRB->set_active(!pedited->lensProf.lcMode);
        ckbUseDist->set_inconsistent(!pedited->lensProf.useDist);
        ckbUseVign->set_inconsistent(!pedited->lensProf.useVign);
        ckbUseCA->set_inconsistent(!pedited->lensProf.useCA);

        if (!pedited->lensProf.lfCameraMake || !pedited->lensProf.lfCameraModel) {
            setLensfunCamera("", "");
        }
        if (!pedited->lensProf.lfLens) {
            setLensfunLens("");
        }

        ckbUseDist->set_sensitive(true);
        ckbUseVign->set_sensitive(true);
        ckbUseCA->set_sensitive(true);
    }

    lcModeChanged = lcpFileChanged = useDistChanged = useVignChanged = useCAChanged = false;
    useLensfunChanged = lensfunAutoChanged = lensfunCameraChanged = lensfunLensChanged = false;

    updateLensfunWarning();
    enableListener();
    conUseDist.block(false);
}

void LensProfilePanel::write(rtengine::procparams::ProcParams* pp, ParamsEdited* pedited)
{
    if (!getEnabled()) {
        pp->lensProf.lcMode = procparams::LensProfParams::LcMode::NONE;
    }
    else if (corrLcpFileRB->get_active()) {
        pp->lensProf.lcMode = procparams::LensProfParams::LcMode::LCP;
    }
    else if (corrMetadata->get_active()) {
        pp->lensProf.lcMode = procparams::LensProfParams::LcMode::METADATA;
    }
    else if (corrLensfunManualRB->get_active()) {
        pp->lensProf.lcMode = procparams::LensProfParams::LcMode::LENSFUNMANUAL;
    }
    else if (corrLensfunAutoRB->get_active()) {
        pp->lensProf.lcMode = procparams::LensProfParams::LcMode::LENSFUNAUTOMATCH;
    }
    else {
        pp->lensProf.lcMode = procparams::LensProfParams::LcMode::NONE;
    }

    if (LCPStore::getInstance()->isValidLCPFileName(corrLcpFileChooser->get_filename())) {
        pp->lensProf.lcpFile = corrLcpFileChooser->get_filename();
    } else {
        pp->lensProf.lcpFile = "";
    }

    pp->lensProf.useDist = ckbUseDist->get_active();
    pp->lensProf.useVign = ckbUseVign->get_active();
    pp->lensProf.useCA   = ckbUseCA->get_active();

    const auto itc = lensfunCameras->get_active();

    if (itc && !corrLensfunAutoRB->get_active()) {
        pp->lensProf.lfCameraMake = (*itc)[lf->lensfunModelCam.make];
        pp->lensProf.lfCameraModel = (*itc)[lf->lensfunModelCam.model];
    } else {
        pp->lensProf.lfCameraMake = "";
        pp->lensProf.lfCameraModel = "";
    }

    const auto itl = lensfunLenses->get_active();

    if (itl && !corrLensfunAutoRB->get_active()) {
        pp->lensProf.lfLens = (*itl)[lf->lensfunModelLens.lens];
    } else {
        pp->lensProf.lfLens = "";
    }

    if (pedited) {
        pedited->lensProf.lcMode = lcModeChanged;
        pedited->lensProf.lcpFile = lcpFileChanged;
        pedited->lensProf.useDist = useDistChanged;
        pedited->lensProf.useVign = useVignChanged;
        pedited->lensProf.useCA   = useCAChanged;
        pedited->lensProf.useLensfun = useLensfunChanged;
        pedited->lensProf.lfAutoMatch = lensfunAutoChanged;
        pedited->lensProf.lfCameraMake = lensfunCameraChanged;
        pedited->lensProf.lfCameraModel = lensfunCameraChanged;
        pedited->lensProf.lfLens = lensfunLensChanged;
    }
}

void LensProfilePanel::setRawMeta(bool raw, const rtengine::FramesMetaData* pMeta)
{
    if ((!raw || pMeta->getFocusDist() <= 0) && !batchMode) {
        disableListener();

        // CA is very focus layer dependent, otherwise it might even worsen things
        allowFocusDep = false;

        enableListener();
    }

    corrMetadata->set_sensitive(false);
    if (pMeta) {
        metadataCorrection = MetadataLensCorrectionFinder::findCorrection(pMeta);
        if (metadataCorrection) {
            corrMetadata->set_sensitive(true);
        }
    }

    isRaw = raw;
    metadata = pMeta;
}

void LensProfilePanel::onLCPFileChanged()
{
    lcpFileChanged = true;
    const bool valid = LCPStore::getInstance()->isValidLCPFileName(corrLcpFileChooser->get_filename());
    updateLCPDisabled(valid);

    if (listener) {
        if (valid) {
            disableListener();
            corrLcpFileRB->set_active(true);
            enableListener();
        }

        listener->panelChanged(EvLCPFile, Glib::path_get_basename(corrLcpFileChooser->get_filename()));
    }
}

void LensProfilePanel::onUseDistChanged()
{
    useDistChanged = true;
    if (ckbUseDist->get_inconsistent()) {
        ckbUseDist->set_inconsistent(false);
        ckbUseDist->set_active(false);
    }

    if (listener) {
        listener->panelChanged(EvLCPUseDist, ckbUseDist->get_active() ? M("GENERAL_ENABLED") : M("GENERAL_DISABLED"));
    }
}

void LensProfilePanel::onUseVignChanged()
{
    useVignChanged = true;
    if (ckbUseVign->get_inconsistent()) {
        ckbUseVign->set_inconsistent(false);
        ckbUseVign->set_active(false);
    }

    if (listener) {
        listener->panelChanged(EvLCPUseVign, ckbUseVign->get_active() ? M("GENERAL_ENABLED") : M("GENERAL_DISABLED"));
    }
}

void LensProfilePanel::onUseCAChanged()
{
    useCAChanged = true;
    if (ckbUseCA->get_inconsistent()) {
        ckbUseCA->set_inconsistent(false);
        ckbUseCA->set_active(false);
    }

    if (listener) {
        listener->panelChanged(EvLCPUseCA, ckbUseCA->get_active() ? M("GENERAL_ENABLED") : M("GENERAL_DISABLED"));
    }
}

void LensProfilePanel::setBatchMode(bool yes)
{
    FoldableToolPanel::setBatchMode(yes);

    corrUnchangedRB->set_group(corrGroup);
    modesGrid->attach_next_to(*corrUnchangedRB, Gtk::POS_TOP, 3, 1);
    corrUnchangedRB->signal_toggled().connect(sigc::bind(sigc::mem_fun(*this, &LensProfilePanel::onCorrModeChanged), corrUnchangedRB));
    corrUnchangedRB->set_active(true);
    corrUnchangedRB->show();
}

void LensProfilePanel::onLensfunCameraChanged()
{
    const auto iter = lensfunCameras->get_active();

    if (iter) {
        lensfunCameraChanged = true;

        if (listener) {
            disableListener();
            corrLensfunManualRB->set_active(true);
            enableListener();

            const Glib::ustring name = (*iter)[lf->lensfunModelCam.model];
            listener->panelChanged(EvLensCorrLensfunCamera, name);
        }
    }

    updateLensfunWarning();
}

void LensProfilePanel::onLensfunLensChanged()
{
    const auto iter = lensfunLenses->get_active();

    if (iter) {
        lensfunLensChanged = true;

        if (listener) {
            disableListener();
            corrLensfunManualRB->set_active(true);
            enableListener();

            const Glib::ustring name = (*iter)[lf->lensfunModelLens.prettylens];
            listener->panelChanged(EvLensCorrLensfunLens, name);
        }
    }

    updateLensfunWarning();
}

void LensProfilePanel::toggleCorrect()
{
    correctExpanded = !correctExpanded;
    if (correctExpanded) {
        correctLabel->set_markup(Glib::ustring("<b>\u25BE ") + M("TP_LENSPROFILE_USE_HEADER") + "</b>");
        correctContent->set_no_show_all(false);
        correctContent->show_all();
        correctContent->set_no_show_all(true);
    } else {
        correctLabel->set_markup(Glib::ustring("<b>\u25B8 ") + M("TP_LENSPROFILE_USE_HEADER") + "</b>");
        correctContent->hide();
    }
}

void LensProfilePanel::enabledChanged()
{
    if (getEnabled()) {
        contentWrapper->set_no_show_all(false);
        contentWrapper->show_all();
        contentWrapper->set_no_show_all(true);

        // If no radio is active (first enable), default to auto-match
        if (!corrMetadata->get_active() && !corrLensfunAutoRB->get_active() &&
            !corrLensfunManualRB->get_active() && !corrLcpFileRB->get_active()) {
            corrLensfunAutoRB->set_active(true);
        }

        updateSubOptionVisibility();

        // Keep correctContent collapsed
        if (!correctExpanded) {
            correctContent->hide();
        }
    } else {
        // Keep the options on screen while off. Flat mode hides the expander
        // header, so hiding the body too would leave nothing at all — and no
        // way back on, since "None" is one of these radios.
        contentWrapper->set_no_show_all(false);
        contentWrapper->show_all();
        contentWrapper->set_no_show_all(true);
        corrOffRB->set_active(true);
        updateSubOptionVisibility();

        if (!correctExpanded) {
            correctContent->hide();
        }
    }

    if (listener) {
        listener->panelChanged(EvLensCorrMode,
            getEnabled() ? M("GENERAL_ENABLED") : M("GENERAL_DISABLED"));
    }
}

void LensProfilePanel::onCorrModeChanged(const Gtk::RadioButton* rbChanged)
{
    if (rbChanged->get_active()) {
        // because the method gets called for the enabled AND the disabled RadioButton, we do the processing only for the enabled one
        Glib::ustring mode;

        if (rbChanged == corrOffRB) {
            // Off switch: the tool's own enable flag follows the choice, so
            // the options stay on screen and can be picked again.
            lcModeChanged = true;
            setEnabled(false);
            mode = M("GENERAL_NONE");

        } else if (rbChanged == corrLensfunAutoRB) {
            lcModeChanged = true;
            useLensfunChanged = true;
            lensfunAutoChanged = true;
            lensfunCameraChanged = true;
            lensfunLensChanged = true;
            lcpFileChanged = false;

            ckbUseDist->set_sensitive(true);
            ckbUseVign->set_sensitive(true);
            ckbUseCA->set_sensitive(true);

            const bool disabled = disableListener();
            if (batchMode) {
                setLensfunCamera("", "");
                setLensfunLens("");
            } else if (metadata) {
                const LFDatabase* const db = LFDatabase::getInstance();
                const LFCamera c = db->findCamera(metadata->getMake(), metadata->getModel(), true);
                const LFLens l = db->findLens(c, metadata->getLens(), true);
                setLensfunCamera(c.getMake(), c.getModel());
                setLensfunLens(l.getLens());
            }
            if (disabled) {
                enableListener();
            }

            mode = M("TP_LENSPROFILE_CORRECTION_AUTOMATCH");

        } else if (rbChanged == corrLensfunManualRB) {
            lcModeChanged = true;
            useLensfunChanged = true;
            lensfunAutoChanged = true;
            lensfunCameraChanged = true;
            lensfunLensChanged = true;
            lcpFileChanged = false;

            ckbUseDist->set_sensitive(true);
            ckbUseVign->set_sensitive(true);
            ckbUseCA->set_sensitive(false);

            mode = M("TP_LENSPROFILE_CORRECTION_MANUAL");

        } else if (rbChanged == corrLcpFileRB) {
            lcModeChanged = true;
            useLensfunChanged = true;
            lensfunAutoChanged = true;
            lcpFileChanged = true;

            updateLCPDisabled(true);

            mode = M("TP_LENSPROFILE_CORRECTION_LCPFILE");

        } else if (rbChanged == corrMetadata) {
            lcModeChanged = true;
            useLensfunChanged = true;
            lensfunAutoChanged = true;
            lcpFileChanged = true;

            updateMetadataDisabled();

            mode = M("TP_LENSPROFILE_CORRECTION_METADATA");

        } else if (rbChanged == corrUnchangedRB) {
            lcModeChanged = false;
            useLensfunChanged = false;
            lensfunAutoChanged = false;
            lcpFileChanged = false;
            lensfunCameraChanged = false;
            lensfunLensChanged = false;

            ckbUseDist->set_sensitive(true);
            ckbUseVign->set_sensitive(true);
            ckbUseCA->set_sensitive(true);

            mode = M("GENERAL_UNCHANGED");
        }

        updateLensfunWarning();

        // Any real correction source implies the tool is on.
        if (rbChanged != corrOffRB && !getEnabled()) {
            setEnabled(true);
        }

        updateSubOptionVisibility();

        if (listener) {
            listener->panelChanged(EvLensCorrMode, mode);
        }
    }
}

//-----------------------------------------------------------------------------
// LFDbHelper
//-----------------------------------------------------------------------------

LensProfilePanel::LFDbHelper::LFDbHelper()
{
    lensfunCameraModel = Gtk::TreeStore::create(lensfunModelCam);
    lensfunLensModel = Gtk::TreeStore::create(lensfunModelLens);

#ifdef _OPENMP
#pragma omp parallel sections if (!settings->verbose)
#endif
    {
#ifdef _OPENMP
#pragma omp section
#endif
        {
            fillLensfunCameras();
        }
#ifdef _OPENMP
#pragma omp section
#endif
        {
            fillLensfunLenses();
        }
    }
}

void LensProfilePanel::LFDbHelper::fillLensfunCameras()
{
    if (settings->verbose) {
        std::cout << "LENSFUN, scanning cameras:" << std::endl;
    }

    std::map<Glib::ustring, std::set<Glib::ustring>> camnames;
    const auto camlist = LFDatabase::getInstance()->getCameras();

    for (const auto& c : camlist) {
        camnames[c.getMake()].insert(c.getModel());

        if (settings->verbose) {
            std::cout << "  found: " << c.getDisplayString().c_str() << std::endl;
        }
    }

    for (const auto& p : camnames) {
        Gtk::TreeModel::Row row = *(lensfunCameraModel->append());
        row[lensfunModelCam.make] = p.first;
        row[lensfunModelCam.model] = p.first;

        for (const auto& c : p.second) {
            Gtk::TreeModel::Row child = *(lensfunCameraModel->append(row.children()));
            child[lensfunModelCam.make] = p.first;
            child[lensfunModelCam.model] = c;
        }
    }
}

void LensProfilePanel::LFDbHelper::fillLensfunLenses()
{
    if (settings->verbose) {
        std::cout << "LENSFUN, scanning lenses:" << std::endl;
    }

    std::map<Glib::ustring, std::set<Glib::ustring>> lenses;
    const auto lenslist = LFDatabase::getInstance()->getLenses();

    for (const auto& l : lenslist) {
        const auto& name = l.getLens();
        const auto& make = l.getMake();
        lenses[make].insert(name);

        if (settings->verbose) {
            std::cout << "  found: " << l.getDisplayString().c_str() << std::endl;
        }
    }

    for (const auto& p : lenses) {
        Gtk::TreeModel::Row row = *(lensfunLensModel->append());
        row[lensfunModelLens.lens] = p.first;
        row[lensfunModelLens.prettylens] = p.first;

        for (auto &c : p.second) {
            Gtk::TreeModel::Row child = *(lensfunLensModel->append(row.children()));
            child[lensfunModelLens.lens] = c;

            if (c.find(p.first, p.first.size() + 1) == p.first.size() + 1) {
                child[lensfunModelLens.prettylens] = c.substr(p.first.size() + 1);
            } else {
                child[lensfunModelLens.prettylens] = c;
            }
        }
    }
}

void LensProfilePanel::updateLCPDisabled(bool enable)
{
    if (!batchMode) {
        ckbUseDist->set_sensitive(enable);
        ckbUseVign->set_sensitive(enable && isRaw);
        ckbUseCA->set_sensitive(enable && allowFocusDep);
    }
}

void LensProfilePanel::updateMetadataDisabled()
{
    if (!batchMode) {
        if (metadataCorrection) {
            ckbUseDist->set_sensitive(metadataCorrection->hasDistortionCorrection());
            ckbUseVign->set_sensitive(metadataCorrection->hasVignettingCorrection());
            ckbUseCA->set_sensitive(metadataCorrection->hasCACorrection());
        } else {
            ckbUseDist->set_sensitive(false);
            ckbUseVign->set_sensitive(false);
            ckbUseCA->set_sensitive(false);
        }
    }
}

bool LensProfilePanel::setLensfunCamera(const Glib::ustring& make, const Glib::ustring& model)
{
    if (!make.empty() && !model.empty()) {
        const auto camera_it = lensfunCameras->get_active();

        if (camera_it && (*camera_it)[lf->lensfunModelCam.make] == make && (*camera_it)[lf->lensfunModelCam.model] == model) {
            return true;
        }

        // search for the active row
        for (const auto& row : lf->lensfunCameraModel->children()) {
            if (row[lf->lensfunModelCam.make] == make) {
                const auto& c = row.children();

                for (auto model_it = c.begin(), end = c.end(); model_it != end; ++model_it) {
                    const auto& childrow = *model_it;

                    if (childrow[lf->lensfunModelCam.model] == model) {
                        lensfunCameras->set_active(model_it);
                        return true;
                    }
                }

                break;
            }
        }
    }

    lensfunCameras->set_active(-1);
    return false;
}

bool LensProfilePanel::setLensfunLens(const Glib::ustring& lens)
{
    if (!lens.empty()) {
        const auto lens_it = lensfunLenses->get_active();

        if (lens_it && (*lens_it)[lf->lensfunModelLens.lens] == lens) {
            return true;
        }

        bool first_maker_found = false;

        for (const auto& row : lf->lensfunLensModel->children()) {
            if (lens.find(row[lf->lensfunModelLens.lens]) == 0) {
                const auto& c = row.children();

                for (auto model_it = c.begin(), end = c.end(); model_it != end; ++model_it) {
                    const auto& childrow = *model_it;

                    if (childrow[lf->lensfunModelLens.lens] == lens) {
                        lensfunLenses->set_active(model_it);
                        return true;
                    }
                }

                // we do not break immediately here, because there might be multiple makers
                // sharing the same prefix (e.g. "Leica" and "Leica Camera AG").
                // therefore, we break below when the lens doesn't match any of them
                first_maker_found = true;
            } else if (first_maker_found) {
                break;
            }
        }
    }

    lensfunLenses->set_active(-1);
    return false;
}

bool LensProfilePanel::checkLensfunCanCorrect(bool automatch)
{
    if (!metadata) {
        return false;
    }

    rtengine::procparams::ProcParams lpp;
    write(&lpp);
    const std::unique_ptr<LFModifier> mod(LFDatabase::getInstance()->findModifier(lpp.lensProf, metadata, 100, 100, lpp.coarse, -1));
    return static_cast<bool>(mod);
}

void LensProfilePanel::updateAutoMatchLabel()
{
    if (!corrLensfunAutoRB->get_active()) {
        autoMatchLabel->hide();
        return;
    }

    Glib::ustring text;

    if (batchMode || !metadata) {
        text = M("TP_LENSPROFILE_AUTOMATCH_UNKNOWN");
    } else {
        const LFDatabase* const db = LFDatabase::getInstance();
        const LFCamera cam = db->findCamera(metadata->getMake(), metadata->getModel(), true);
        const LFLens lens = db->findLens(cam, metadata->getLens(), true);

        const Glib::ustring camName = cam.getDisplayString();
        const Glib::ustring lensName = lens.getDisplayString();

        if (camName.empty() && lensName.empty()) {
            text = M("TP_LENSPROFILE_AUTOMATCH_NONE");
        } else {
            text = Glib::ustring::compose("%1 · %2",
                                          camName.empty() ? M("TP_LENSPROFILE_AUTOMATCH_NOCAM") : camName,
                                          lensName.empty() ? M("TP_LENSPROFILE_AUTOMATCH_NOLENS") : lensName);
        }
    }

    autoMatchLabel->set_markup("<small><i>" + Glib::Markup::escape_text(text) + "</i></small>");
    autoMatchLabel->set_no_show_all(false);
    autoMatchLabel->show();
}

void LensProfilePanel::updateSubOptionVisibility()
{
    // The indented box carries only the controls the selected source needs:
    // camera/lens pickers for manual matching, the file chooser for an LCP.
    // Automatic and metadata sources need nothing, so it collapses away.
    const bool manual = corrLensfunManualRB->get_active();
    const bool lcp = corrLcpFileRB->get_active();

    if (manual || lcp) {
        manualSubBox->set_no_show_all(false);
        manualSubBox->show_all();
        manualSubBox->set_no_show_all(true);
    }

    setManualParamsVisibility(manual);
    corrLcpFileChooser->set_visible(lcp);

    if (!manual && !lcp) {
        manualSubBox->hide();
    }

    updateAutoMatchLabel();
}

void LensProfilePanel::setManualParamsVisibility(bool setVisible)
{
    if (setVisible) {
        lensfunCamerasLbl->show();
        lensfunCameras->show();
        lensfunLensesLbl->show();
        lensfunLenses->show();
        updateLensfunWarning();
    } else {
        lensfunCamerasLbl->hide();
        lensfunCameras->hide();
        lensfunLensesLbl->hide();
        lensfunLenses->hide();
        warning->hide();
    }
}

void LensProfilePanel::updateLensfunWarning()
{
    warning->hide();

    if (corrLensfunManualRB->get_active() || corrLensfunAutoRB->get_active()) {
        const LFDatabase* const db = LFDatabase::getInstance();

        const auto itc = lensfunCameras->get_active();

        if (!itc) {
            return;
        }

        const LFCamera c = db->findCamera((*itc)[lf->lensfunModelCam.make], (*itc)[lf->lensfunModelCam.model], false);
        const auto itl = lensfunLenses->get_active();

        if (!itl) {
            return;
        }

        const LFLens l = db->findLens(c, (*itl)[lf->lensfunModelLens.lens], false);
        const float lenscrop = l.getCropFactor();
        const float camcrop = c.getCropFactor();

        if (lenscrop <= 0 || camcrop <= 0 || lenscrop / camcrop >= 1.01f) {
            warning->show();
        }

        ckbUseVign->set_sensitive(l.hasVignettingCorrection());
        ckbUseDist->set_sensitive(l.hasDistortionCorrection());
        ckbUseCA->set_sensitive(l.hasCACorrection());

        if (!isRaw || !l.hasVignettingCorrection()) {
            ckbUseVign->set_active(false);
        }

        if (!l.hasDistortionCorrection()) {
            ckbUseDist->set_active(false);
        }

        if (!l.hasCACorrection()) {
            ckbUseCA->set_active(false);
        }
    }
}

LensProfilePanel::LFDbHelper* LensProfilePanel::lf(nullptr);
