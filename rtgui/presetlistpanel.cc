/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2004-2010 Gabor Horvath <hgabor@rawtherapee.com>
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
#include "presetlistpanel.h"

#include "clipboard.h"
#include "multilangmgr.h"
#include "options.h"
#include "paramsedited.h"
#include "pathutils.h"
#include "rtimage.h"
#include "thumbnail.h"

#include "rtengine/procparams.h"
#include "rtengine/procevents.h"
#include "rtengine/rtengine.h"

using namespace rtengine;
using namespace rtengine::procparams;

PartialPasteDlg* PresetListPanel::partialProfileDlg_ = nullptr;
Gtk::Window* PresetListPanel::parent_ = nullptr;

void PresetListPanel::init(Gtk::Window* parentWindow)
{
    parent_ = parentWindow;
}

void PresetListPanel::cleanup()
{
    delete partialProfileDlg_;
}

PresetListPanel::PresetListPanel() :
    modeOn_("profile-filled"),
    modeOff_("profile-partial"),
    profileFillImage_(Gtk::manage(new RTImage(App::get().options().filledProfile ? modeOn_ : modeOff_, Gtk::ICON_SIZE_LARGE_TOOLBAR))),
    selectedEntry_(nullptr),
    selectedWidget_(nullptr),
    custom_(nullptr),
    lastsaved_(nullptr),
    lastSavedPSE_(nullptr),
    customPSE_(nullptr),
    tpc_(nullptr),
    dontupdate_(false),
    storedPProfile_(nullptr),
    ipc_(nullptr),
    hasSavedParams_(false),
    lastHoveredEntry_(nullptr),
    openThm_(nullptr),
    thumbCancelled_(false)
{
    // --- Toolbar (compact) ---
    auto* toolbar = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 1));
    toolbar->set_margin_top(0);
    toolbar->set_margin_bottom(1);

    fillMode_ = Gtk::manage(new Gtk::ToggleButton());
    fillMode_->set_active(App::get().options().filledProfile);
    fillMode_->add(*profileFillImage_);
    fillMode_->signal_toggled().connect(sigc::mem_fun(*this, &PresetListPanel::profileFillModeToggled));
    fillMode_->set_tooltip_text(M("PROFILEPANEL_MODE_TOOLTIP"));
    setExpandAlignProperties(fillMode_, false, true, Gtk::ALIGN_CENTER, Gtk::ALIGN_FILL);

    load_ = Gtk::manage(new Gtk::Button());
    load_->add(*Gtk::manage(new RTImage("folder-open", Gtk::ICON_SIZE_LARGE_TOOLBAR)));
    load_->get_style_context()->add_class("Left");
    setExpandAlignProperties(load_, false, true, Gtk::ALIGN_CENTER, Gtk::ALIGN_FILL);

    save_ = Gtk::manage(new Gtk::Button());
    save_->add(*Gtk::manage(new RTImage("save", Gtk::ICON_SIZE_LARGE_TOOLBAR)));
    save_->get_style_context()->add_class("MiddleH");
    setExpandAlignProperties(save_, false, true, Gtk::ALIGN_CENTER, Gtk::ALIGN_FILL);

    copy_ = Gtk::manage(new Gtk::Button());
    copy_->add(*Gtk::manage(new RTImage("copy", Gtk::ICON_SIZE_LARGE_TOOLBAR)));
    copy_->get_style_context()->add_class("MiddleH");
    setExpandAlignProperties(copy_, false, true, Gtk::ALIGN_CENTER, Gtk::ALIGN_FILL);

    paste_ = Gtk::manage(new Gtk::Button());
    paste_->add(*Gtk::manage(new RTImage("paste", Gtk::ICON_SIZE_LARGE_TOOLBAR)));
    paste_->get_style_context()->add_class("Right");
    setExpandAlignProperties(paste_, false, true, Gtk::ALIGN_CENTER, Gtk::ALIGN_FILL);

    toolbar->pack_start(*fillMode_, Gtk::PACK_SHRINK);
    toolbar->pack_start(*load_, Gtk::PACK_SHRINK);
    toolbar->pack_start(*save_, Gtk::PACK_SHRINK);
    toolbar->pack_start(*copy_, Gtk::PACK_SHRINK);
    toolbar->pack_start(*paste_, Gtk::PACK_SHRINK);

    // --- Scrolled content (toolbar included so everything scrolls together) ---
    contentBox_ = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 0));
    contentBox_->set_valign(Gtk::ALIGN_START);
    contentBox_->set_margin_start(2);
    contentBox_->set_margin_end(2);
    contentBox_->pack_start(*toolbar, Gtk::PACK_SHRINK, 0);

    // Special entries FlowBox (Custom / Last Saved) — card-style
    specialFlowBox_ = Gtk::manage(new Gtk::FlowBox());
    specialFlowBox_->set_selection_mode(Gtk::SELECTION_NONE);
    specialFlowBox_->set_homogeneous(false);
    specialFlowBox_->set_min_children_per_line(1);
    specialFlowBox_->set_max_children_per_line(20);
    specialFlowBox_->set_column_spacing(4);
    specialFlowBox_->set_row_spacing(4);
    specialFlowBox_->set_no_show_all(true);

    customButton_ = Gtk::manage(new Gtk::Button());
    customButton_->set_relief(Gtk::RELIEF_NONE);
    customButton_->get_style_context()->add_class("preset-card");
    customButton_->get_style_context()->add_class("preset-special");
    customButton_->set_size_request(CARD_MIN_WIDTH, -1);
    {
        auto* vbox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 1));
        auto* img = Gtk::manage(new Gtk::Image());
        img->set_size_request(-1, THUMB_HEIGHT);
        img->set_halign(Gtk::ALIGN_START);
        vbox->pack_start(*img, Gtk::PACK_SHRINK);
        auto* label = Gtk::manage(new Gtk::Label());
        label->set_line_wrap(false);
        label->set_xalign(0.0);
        label->get_style_context()->add_class("preset-card-label");
        vbox->pack_start(*label, Gtk::PACK_SHRINK);
        customButton_->add(*vbox);
    }
    customButton_->signal_clicked().connect([this]() {
        if (customPSE_) selectEntry(customPSE_);
    });
    specialFlowBox_->add(*customButton_);
    customButton_->get_parent()->set_no_show_all(true);

    lastSavedButton_ = Gtk::manage(new Gtk::Button());
    lastSavedButton_->set_relief(Gtk::RELIEF_NONE);
    lastSavedButton_->get_style_context()->add_class("preset-card");
    lastSavedButton_->get_style_context()->add_class("preset-special");
    lastSavedButton_->set_size_request(CARD_MIN_WIDTH, -1);
    {
        auto* vbox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 1));
        auto* img = Gtk::manage(new Gtk::Image());
        img->set_size_request(-1, THUMB_HEIGHT);
        img->set_halign(Gtk::ALIGN_START);
        vbox->pack_start(*img, Gtk::PACK_SHRINK);
        auto* label = Gtk::manage(new Gtk::Label());
        label->set_line_wrap(false);
        label->set_xalign(0.0);
        label->get_style_context()->add_class("preset-card-label");
        vbox->pack_start(*label, Gtk::PACK_SHRINK);
        lastSavedButton_->add(*vbox);
    }
    lastSavedButton_->signal_clicked().connect([this]() {
        if (lastSavedPSE_) selectEntry(lastSavedPSE_);
    });
    specialFlowBox_->add(*lastSavedButton_);
    lastSavedButton_->get_parent()->set_no_show_all(true);

    contentBox_->pack_start(*specialFlowBox_, Gtk::PACK_SHRINK);

    // Grid area for preset cards
    gridBox_ = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 2));
    contentBox_->pack_start(*gridBox_, Gtk::PACK_SHRINK);

    scrolledWin_ = Gtk::manage(new Gtk::ScrolledWindow());
    scrolledWin_->set_name("PresetListPanel");
    scrolledWin_->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    scrolledWin_->add(*contentBox_);
    scrolledWin_->set_vexpand(true);

    // Connect button signals
    load_->signal_button_release_event().connect_notify(sigc::mem_fun(*this, &PresetListPanel::load_clicked));
    save_->signal_button_release_event().connect_notify(sigc::mem_fun(*this, &PresetListPanel::save_clicked));
    copy_->signal_button_release_event().connect_notify(sigc::mem_fun(*this, &PresetListPanel::copy_clicked));
    paste_->signal_button_release_event().connect_notify(sigc::mem_fun(*this, &PresetListPanel::paste_clicked));

    load_->set_tooltip_markup(M("PROFILEPANEL_TOOLTIPLOAD"));
    save_->set_tooltip_markup(M("PROFILEPANEL_TOOLTIPSAVE"));
    copy_->set_tooltip_markup(M("PROFILEPANEL_TOOLTIPCOPY"));
    paste_->set_tooltip_markup(M("PROFILEPANEL_TOOLTIPPASTE"));

    ProfileStore::getInstance()->addListener(this);

    scrolledWin_->show_all();
}

PresetListPanel::~PresetListPanel()
{
    cancelThumbnailGeneration();
    ProfileStore::getInstance()->removeListener(this);

    if (custom_) { custom_->deleteInstance(); delete custom_; }
    if (lastsaved_) { lastsaved_->deleteInstance(); delete lastsaved_; }
    delete lastSavedPSE_;
    delete customPSE_;
    if (storedPProfile_) { storedPProfile_->deleteInstance(); delete storedPProfile_; }
}

// ============================================================
// Grid content building
// ============================================================

void PresetListPanel::buildContent()
{
    cardMap_.clear();
    thumbImageMap_.clear();

    // Clear grid content
    for (auto* child : gridBox_->get_children()) {
        gridBox_->remove(*child);
    }

    const std::vector<const ProfileStoreEntry*>* entryList = ProfileStore::getInstance()->getFileList();
    int rootFolderId = entryList->at(0)->parentFolderId;

    // Collect root-level files, including Internal default
    std::vector<const ProfileStoreEntry*> rootFiles;
    if (rootFolderId != 0) {
        rootFiles.push_back(ProfileStore::getInstance()->getInternalDefaultPSE());
    }
    for (auto* entry : *entryList) {
        if (entry->parentFolderId == rootFolderId && entry->type != PSET_FOLDER) {
            rootFiles.push_back(entry);
        }
    }

    if (!rootFiles.empty()) {
        auto* flowBox = Gtk::manage(new Gtk::FlowBox());
        flowBox->set_selection_mode(Gtk::SELECTION_NONE);
        flowBox->set_homogeneous(false);
        flowBox->set_min_children_per_line(1);
        flowBox->set_max_children_per_line(20);
        flowBox->set_column_spacing(4);
        flowBox->set_row_spacing(4);
        for (auto* entry : rootFiles) {
            flowBox->add(*createCard(entry));
        }
        gridBox_->pack_start(*flowBox, Gtk::PACK_SHRINK);
    }

    // Root-level folders as expanders, sub-levels via buildCategoryContent
    for (auto* entry : *entryList) {
        if (entry->parentFolderId == rootFolderId && entry->type == PSET_FOLDER) {
            Glib::ustring folderPath(ProfileStore::getInstance()->getPathFromId(entry->folderId));
            if (App::get().options().useBundledProfiles ||
                ((folderPath != "${G}") && (folderPath != "${U}"))) {
                auto* expander = Gtk::manage(new Gtk::Expander(entry->label));
                expander->set_expanded(true);
                expander->get_style_context()->add_class("preset-category");
                auto* box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 2));
                expander->add(*box);
                buildCategoryContent(box, entry->folderId, entryList);
                gridBox_->pack_start(*expander, Gtk::PACK_SHRINK);
            } else {
                // Skip ${G}/${U} wrapper, promote children to parent level
                buildCategoryContent(gridBox_, entry->folderId, entryList);
            }
        }
    }

    ProfileStore::getInstance()->releaseFileList();
    gridBox_->show_all();
    highlightSelection();
}

void PresetListPanel::buildCategoryContent(
    Gtk::Box* parent, int parentFolderId,
    const std::vector<const ProfileStoreEntry*>* entryList)
{
    // Collect files and folders at this level
    std::vector<const ProfileStoreEntry*> files;
    std::vector<const ProfileStoreEntry*> folders;

    for (auto* entry : *entryList) {
        if (entry->parentFolderId == parentFolderId) {
            if (entry->type == PSET_FOLDER) {
                folders.push_back(entry);
            } else {
                files.push_back(entry);
            }
        }
    }

    // Add files to a FlowBox grid
    if (!files.empty()) {
        auto* flowBox = Gtk::manage(new Gtk::FlowBox());
        flowBox->set_selection_mode(Gtk::SELECTION_NONE);
        flowBox->set_homogeneous(false);
        flowBox->set_min_children_per_line(1);
        flowBox->set_max_children_per_line(20);
        flowBox->set_column_spacing(4);
        flowBox->set_row_spacing(4);

        for (auto* entry : files) {
            auto* card = createCard(entry);
            flowBox->add(*card);
        }

        parent->pack_start(*flowBox, Gtk::PACK_SHRINK);
    }

    // Add sub-folders as expanders
    for (auto* folder : folders) {
        Glib::ustring folderPath(ProfileStore::getInstance()->getPathFromId(folder->folderId));

        if (App::get().options().useBundledProfiles ||
            ((folderPath != "${G}") && (folderPath != "${U}"))) {
            auto* expander = Gtk::manage(new Gtk::Expander(folder->label));
            expander->set_expanded(true);
            expander->get_style_context()->add_class("preset-category");
            auto* box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 2));
            expander->add(*box);

            buildCategoryContent(box, folder->folderId, entryList);
            parent->pack_start(*expander, Gtk::PACK_SHRINK);
        } else {
            // Skip ${G}/${U} wrapper, promote children to parent level
            buildCategoryContent(parent, folder->folderId, entryList);
        }
    }
}

Gtk::Button* PresetListPanel::createCard(const ProfileStoreEntry* entry)
{
    auto* card = Gtk::manage(new Gtk::Button());
    card->set_relief(Gtk::RELIEF_NONE);
    card->get_style_context()->add_class("preset-card");
    card->set_size_request(CARD_MIN_WIDTH, -1);

    auto* vbox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 1));

    auto* img = Gtk::manage(new Gtk::Image());
    img->set_size_request(-1, THUMB_HEIGHT);
    img->set_halign(Gtk::ALIGN_START);
    vbox->pack_start(*img, Gtk::PACK_SHRINK);

    auto* label = Gtk::manage(new Gtk::Label(entry->label));
    label->set_line_wrap(false);
    label->set_xalign(0.0);
    label->get_style_context()->add_class("preset-card-label");
    vbox->pack_start(*label, Gtk::PACK_SHRINK);

    card->add(*vbox);

    // Click to select
    card->signal_clicked().connect([this, entry]() {
        selectEntry(entry);
    });

    // Hover events for preview
    card->add_events(Gdk::ENTER_NOTIFY_MASK | Gdk::LEAVE_NOTIFY_MASK);
    card->signal_enter_notify_event().connect([this, entry](GdkEventCrossing*) -> bool {
        startHoverTimer(entry);
        return false;
    }, false);
    card->signal_leave_notify_event().connect([this](GdkEventCrossing*) -> bool {
        cancelHover();
        return false;
    }, false);

    cardMap_[entry] = card;
    thumbImageMap_[entry] = img;

    return card;
}

// ============================================================
// Selection
// ============================================================

void PresetListPanel::selectEntry(const ProfileStoreEntry* entry, bool fireChange)
{
    cancelHover();
    selectedEntry_ = entry;
    highlightSelection();

    if (!fireChange) {
        return;
    }

    if (entry == customPSE_) {
        if (!dontupdate_) {
            changeTo(custom_, Glib::ustring("(" + M("PROFILEPANEL_PCUSTOM") + ")"));
        }
    } else if (entry == lastSavedPSE_) {
        changeTo(lastsaved_, Glib::ustring("(" + M("PROFILEPANEL_PLASTSAVED") + ")"));
    } else if (entry) {
        const PartialProfile* s = ProfileStore::getInstance()->getProfile(entry);
        if (s) {
            if (fillMode_->get_active() && s->pedited) {
                ParamsEdited pe(true);
                pe.locallab.spots.resize(s->pparams->locallab.spots.size(),
                    LocallabParamsEdited::LocallabSpotEdited(true));
                PartialProfile s2(s->pparams, &pe, false);
                changeTo(&s2, entry->label + "+");
            } else {
                changeTo(s, entry->label);
            }
        }
    }

    dontupdate_ = false;
}

void PresetListPanel::highlightSelection()
{
    // Remove highlight from previous widget
    if (selectedWidget_) {
        selectedWidget_->get_style_context()->remove_class("selected");
        selectedWidget_ = nullptr;
    }

    if (!selectedEntry_) {
        return;
    }

    // Find the widget for the selected entry
    Gtk::Widget* newWidget = nullptr;
    if (selectedEntry_ == customPSE_) {
        newWidget = customButton_;
    } else if (selectedEntry_ == lastSavedPSE_) {
        newWidget = lastSavedButton_;
    } else {
        auto it = cardMap_.find(selectedEntry_);
        if (it != cardMap_.end()) {
            newWidget = it->second;
        }
    }

    if (newWidget) {
        newWidget->get_style_context()->add_class("selected");
        selectedWidget_ = newWidget;
    }
}

void PresetListPanel::changeTo(const PartialProfile* newpp, Glib::ustring profname)
{
    if (!newpp) {
        return;
    }
    if (tpc_) {
        tpc_->profileChange(newpp, EvProfileChanged, profname);
    }
}

// ============================================================
// Special entries
// ============================================================

bool PresetListPanel::isCustomSelected() const
{
    return selectedEntry_ == customPSE_ && customPSE_ != nullptr;
}

bool PresetListPanel::isLastSavedSelected() const
{
    return selectedEntry_ == lastSavedPSE_ && lastSavedPSE_ != nullptr;
}

const ProfileStoreEntry* PresetListPanel::getSelectedEntry() const
{
    return selectedEntry_;
}

Glib::ustring PresetListPanel::getCurrentLabel() const
{
    return selectedEntry_ ? selectedEntry_->label : Glib::ustring();
}

Glib::ustring PresetListPanel::getFullPathFromActiveRow() const
{
    if (!selectedEntry_) {
        return {};
    }

    const ProfileStore* profileStore = ProfileStore::getInstance();
    if (selectedEntry_ == profileStore->getInternalDefaultPSE()) {
        return DEFPROFILE_INTERNAL;
    }
    if (selectedEntry_ == profileStore->getInternalDynamicPSE()) {
        return DEFPROFILE_DYNAMIC;
    }

    // Special entries don't have real paths
    if (selectedEntry_ == customPSE_ || selectedEntry_ == lastSavedPSE_) {
        return {};
    }

    return Glib::build_filename(
        profileStore->getPathFromId(selectedEntry_->parentFolderId),
        selectedEntry_->label);
}

void PresetListPanel::addCustomEntry()
{
    if (customPSE_) {
        delete customPSE_;
    }
    customPSE_ = new ProfileStoreEntry(
        Glib::ustring("(" + M("PROFILEPANEL_PCUSTOM") + ")"), PSET_FILE, 0, 0);

    // Update label in the card
    if (auto* vbox = dynamic_cast<Gtk::Box*>(customButton_->get_child())) {
        auto children = vbox->get_children();
        if (children.size() >= 2) {
            if (auto* lbl = dynamic_cast<Gtk::Label*>(children[1])) {
                lbl->set_text(customPSE_->label);
            }
        }
    }

    customButton_->get_parent()->show_all();
    specialFlowBox_->show();
}

void PresetListPanel::addLastSavedEntry()
{
    if (lastSavedPSE_) {
        delete lastSavedPSE_;
    }
    lastSavedPSE_ = new ProfileStoreEntry(
        Glib::ustring("(" + M("PROFILEPANEL_PLASTSAVED") + ")"), PSET_FILE, 0, 0);

    // Update label in the card
    if (auto* vbox = dynamic_cast<Gtk::Box*>(lastSavedButton_->get_child())) {
        auto children = vbox->get_children();
        if (children.size() >= 2) {
            if (auto* lbl = dynamic_cast<Gtk::Label*>(children[1])) {
                lbl->set_text(lastSavedPSE_->label);
            }
        }
    }

    lastSavedButton_->get_parent()->show_all();
    specialFlowBox_->show();
}

// ============================================================
// PParamsChangeListener
// ============================================================

void PresetListPanel::procParamsChanged(
    const rtengine::procparams::ProcParams* p,
    const rtengine::ProcEvent& ev,
    const Glib::ustring& descr,
    const ParamsEdited* paramsEdited)
{
    if (ev == EvProfileChanged || ev == EvPhotoLoaded) {
        return;
    }

    if (hasSavedParams_) {
        revertHoverPreview();
    }

    if (!isCustomSelected()) {
        dontupdate_ = true;

        if (!custom_) {
            custom_ = new PartialProfile(true);
            custom_->set(true);
            addCustomEntry();
        }
        selectEntry(customPSE_, false);
    }

    *custom_->pparams = *p;
    custom_->pedited->locallab.spots.clear();
    custom_->pedited->locallab.spots.resize(p->locallab.spots.size(),
        LocallabParamsEdited::LocallabSpotEdited(true));
}

void PresetListPanel::clearParamChanges()
{
}

// ============================================================
// Profile initialization
// ============================================================

void PresetListPanel::initProfile(const Glib::ustring& profileFullPath, ProcParams* lastSaved)
{
    const ProfileStoreEntry* pse = nullptr;
    const PartialProfile* defprofile = nullptr;

    if (custom_) {
        custom_->deleteInstance();
        delete custom_;
        custom_ = nullptr;
    }
    if (lastsaved_) {
        lastsaved_->deleteInstance();
        delete lastsaved_;
        lastsaved_ = nullptr;
    }

    // Hide special cards
    customButton_->get_parent()->hide();
    lastSavedButton_->get_parent()->hide();
    specialFlowBox_->hide();
    if (customPSE_) { delete customPSE_; customPSE_ = nullptr; }
    if (lastSavedPSE_) { delete lastSavedPSE_; lastSavedPSE_ = nullptr; }

    if (lastSaved) {
        ParamsEdited* pe = new ParamsEdited(true);
        pe->locallab.spots.resize(lastSaved->locallab.spots.size(),
            LocallabParamsEdited::LocallabSpotEdited(true));
        lastsaved_ = new PartialProfile(lastSaved, pe);
    }

    updateProfileList();

    if (lastsaved_) {
        addLastSavedEntry();
    }

    if (!(pse = ProfileStore::getInstance()->findEntryFromFullPath(profileFullPath))) {
        pse = ProfileStore::getInstance()->getInternalDefaultPSE();
    }

    defprofile = ProfileStore::getInstance()->getProfile(pse);

    if (lastsaved_) {
        selectEntry(lastSavedPSE_, false);

        if (tpc_) {
            tpc_->setDefaults(lastsaved_->pparams);
            tpc_->profileChange(lastsaved_, EvPhotoLoaded,
                getCurrentLabel(), nullptr, true);
        }
    } else {
        selectEntry(pse, false);

        if (tpc_) {
            tpc_->setDefaults(defprofile->pparams);
            tpc_->profileChange(defprofile, EvPhotoLoaded, getCurrentLabel());
        }
    }

    startThumbnailGeneration();
}

void PresetListPanel::setInitialFileName(const Glib::ustring& filename)
{
    lastFilename_ = Glib::path_get_basename(filename) + App::PARAM_FILE_EXTENSION;
    imagePath_ = Glib::path_get_dirname(filename);
}

// ============================================================
// Store / Restore
// ============================================================

void PresetListPanel::storeCurrentValue()
{
    storedValue_ = getFullPathFromActiveRow();

    if (!isCustomSelected() && !isLastSavedSelected()) {
        const PartialProfile* currProfile = nullptr;
        if (selectedEntry_) {
            currProfile = ProfileStore::getInstance()->getProfile(selectedEntry_);
        }
        if (currProfile) {
            storedPProfile_ = new PartialProfile(currProfile->pparams, currProfile->pedited, true);
        } else {
            storedPProfile_ = new PartialProfile(true);
        }
    }
}

void PresetListPanel::restoreValue()
{
    bool found = false;

    if (!storedValue_.empty()) {
        const ProfileStoreEntry* pse = ProfileStore::getInstance()->findEntryFromFullPath(storedValue_);
        if (pse && cardMap_.count(pse)) {
            selectEntry(pse, false);
            found = true;
        }
    }

    if (!found && storedPProfile_) {
        if (custom_) {
            delete custom_;
        }
        custom_ = new PartialProfile(storedPProfile_->pparams, storedPProfile_->pedited, true);
        addCustomEntry();
        selectEntry(customPSE_, false);
    }

    storedValue_ = "";
    if (storedPProfile_) {
        storedPProfile_->deleteInstance();
        delete storedPProfile_;
        storedPProfile_ = nullptr;
    }
}

void PresetListPanel::updateProfileList()
{
    buildContent();

    if (custom_) {
        addCustomEntry();
    }
    if (lastsaved_) {
        addLastSavedEntry();
    }
}

// ============================================================
// Toolbar callbacks
// ============================================================

void PresetListPanel::profileFillModeToggled()
{
    if (fillMode_->get_active()) {
        profileFillImage_->set_from_icon_name(modeOn_);
    } else {
        profileFillImage_->set_from_icon_name(modeOff_);
    }
}

void PresetListPanel::writeOptions()
{
    App::get().mut_options().filledProfile = fillMode_->get_active();
}

void PresetListPanel::save_clicked(GdkEventButton* event)
{
    if (event->button != 1) return;

    const PartialProfile* toSave;

    if (isCustomSelected()) {
        toSave = custom_;
    } else if (isLastSavedSelected()) {
        toSave = lastsaved_;
    } else {
        const auto entry = getSelectedEntry();
        toSave = entry ? ProfileStore::getInstance()->getProfile(entry) : nullptr;
    }

    if (toSave == nullptr) return;

    const auto isPartial = event->state & Gdk::CONTROL_MASK;
    if (isPartial) {
        if (!partialProfileDlg_) {
            partialProfileDlg_ = new PartialPasteDlg(Glib::ustring(), parent_);
        }
        partialProfileDlg_->set_title(M("PROFILEPANEL_SAVEPPASTE"));
        partialProfileDlg_->updateSpotWidget(toSave->pparams);
        if (partialProfileDlg_->run() != Gtk::RESPONSE_OK) {
            partialProfileDlg_->hide();
            return;
        }
        partialProfileDlg_->hide();
    }

    auto& options = App::get().mut_options();
    Gtk::FileChooserDialog dialog(getToplevelWindow(scrolledWin_), M("PROFILEPANEL_SAVEDLGLABEL"), Gtk::FILE_CHOOSER_ACTION_SAVE);
    bindCurrentFolder(dialog, options.loadSaveProfilePath);
    dialog.set_current_name(lastFilename_);

    try { dialog.add_shortcut_folder(options.getPreferredProfilePath()); } catch (Glib::Error&) {}
    try { dialog.add_shortcut_folder(imagePath_); } catch (Glib::Error&) {}

    dialog.add_button(M("GENERAL_CANCEL"), Gtk::RESPONSE_CANCEL);
    dialog.add_button(M("GENERAL_SAVE"), Gtk::RESPONSE_OK);

    auto filter_pp = Gtk::FileFilter::create();
    filter_pp->set_name(M("FILECHOOSER_FILTER_PP"));
    filter_pp->add_pattern("*" + App::PARAM_FILE_EXTENSION);
    dialog.add_filter(filter_pp);
    auto filter_any = Gtk::FileFilter::create();
    filter_any->set_name(M("FILECHOOSER_FILTER_ANY"));
    filter_any->add_pattern("*");
    dialog.add_filter(filter_any);

    while (true) {
        if (dialog.run() != Gtk::RESPONSE_OK) break;

        auto fname = dialog.get_filename();
        if (("." + getExtension(fname)) != App::PARAM_FILE_EXTENSION)
            fname += App::PARAM_FILE_EXTENSION;
        if (!confirmOverwrite(dialog, fname)) continue;

        lastFilename_ = Glib::path_get_basename(fname);
        int retCode = -1;

        if (isPartial) {
            PartialProfile ppTemp(true);
            partialProfileDlg_->applyPaste(ppTemp.pparams, ppTemp.pedited, toSave->pparams, nullptr);
            retCode = ppTemp.pparams->save(fname, "", true, ppTemp.pedited);
            ppTemp.deleteInstance();
        } else {
            retCode = toSave->pparams->save(fname);
        }

        if (retCode == 0) {
            ProfileStore::getInstance()->parseProfiles();
            break;
        } else {
            writeFailed(dialog, fname);
        }
    }
}

void PresetListPanel::load_clicked(GdkEventButton* event)
{
    if (event->button != 1) return;

    Gtk::FileChooserDialog dialog(getToplevelWindow(scrolledWin_), M("PROFILEPANEL_LOADDLGLABEL"), Gtk::FILE_CHOOSER_ACTION_OPEN);
    auto& options = App::get().mut_options();
    bindCurrentFolder(dialog, options.loadSaveProfilePath);

    try { dialog.add_shortcut_folder(options.getPreferredProfilePath()); } catch (Glib::Error&) {}
    try { dialog.add_shortcut_folder(imagePath_); } catch (Glib::Error&) {}

    dialog.add_button(M("GENERAL_CANCEL"), Gtk::RESPONSE_CANCEL);
    dialog.add_button(M("GENERAL_OPEN"), Gtk::RESPONSE_OK);

    auto filter_pp = Gtk::FileFilter::create();
    filter_pp->set_name(M("FILECHOOSER_FILTER_PP"));
    filter_pp->add_pattern("*" + App::PARAM_FILE_EXTENSION);
    dialog.add_filter(filter_pp);
    auto filter_any = Gtk::FileFilter::create();
    filter_any->set_name(M("FILECHOOSER_FILTER_ANY"));
    filter_any->add_pattern("*");
    dialog.add_filter(filter_any);

    int result = dialog.run();
    dialog.hide();

    if (result != Gtk::RESPONSE_OK) return;

    Glib::ustring fname = dialog.get_filename();
    bool customCreated = false;

    if (!custom_) {
        custom_ = new PartialProfile(true);
        customCreated = true;
    }

    ProcParams pp;
    ParamsEdited pe;
    int err = pp.load(fname, &pe);

    if (!err) {
        if (!customCreated && fillMode_->get_active()) {
            custom_->pparams->setDefaults();
            custom_->pedited->locallab.spots.clear();
        }

        for (int i = 0; i < (int)pe.locallab.spots.size(); i++) {
            pe.locallab.spots.at(i).set(true);
        }

        custom_->set(true);
        addCustomEntry();
        selectEntry(customPSE_, false);

        if (event->state & Gdk::CONTROL_MASK) {
            if (!partialProfileDlg_)
                partialProfileDlg_ = new PartialPasteDlg(Glib::ustring(), parent_);
            partialProfileDlg_->set_title(M("PROFILEPANEL_LOADPPASTE"));
            partialProfileDlg_->updateSpotWidget(&pp);
            int i = partialProfileDlg_->run();
            partialProfileDlg_->hide();
            if (i != Gtk::RESPONSE_OK) return;
            partialProfileDlg_->applyPaste(custom_->pparams,
                !fillMode_->get_active() ? custom_->pedited : nullptr, &pp, &pe);
        } else {
            pe.combine(*custom_->pparams, pp, true);
            if (!fillMode_->get_active()) {
                *custom_->pedited = pe;
            } else {
                custom_->pedited->locallab.spots.resize(pe.locallab.spots.size(),
                    LocallabParamsEdited::LocallabSpotEdited(true));
            }
        }

        changeTo(custom_, M("PROFILEPANEL_PFILE"));
    } else if (customCreated) {
        custom_->deleteInstance();
        delete custom_;
        custom_ = nullptr;
    }
}

void PresetListPanel::copy_clicked(GdkEventButton* event)
{
    if (event->button != 1) return;

    const PartialProfile* toSave;

    if (isCustomSelected()) {
        toSave = custom_;
    } else if (isLastSavedSelected()) {
        toSave = lastsaved_;
    } else {
        const ProfileStoreEntry* entry = getSelectedEntry();
        toSave = entry ? ProfileStore::getInstance()->getProfile(entry) : nullptr;
    }

    if (!toSave) return;

    if (event->state & Gdk::CONTROL_MASK) {
        if (!partialProfileDlg_)
            partialProfileDlg_ = new PartialPasteDlg(Glib::ustring(), parent_);
        partialProfileDlg_->set_title(M("PROFILEPANEL_COPYPPASTE"));
        partialProfileDlg_->updateSpotWidget(toSave->pparams);
        int i = partialProfileDlg_->run();
        partialProfileDlg_->hide();
        if (i != Gtk::RESPONSE_OK) return;

        PartialProfile ppTemp(true);
        partialProfileDlg_->applyPaste(ppTemp.pparams, ppTemp.pedited, toSave->pparams, toSave->pedited);
        clipboard.setPartialProfile(ppTemp);
        ppTemp.deleteInstance();
    } else {
        clipboard.setProcParams(*toSave->pparams);
    }
}

void PresetListPanel::paste_clicked(GdkEventButton* event)
{
    if (event->button != 1) return;
    if (!clipboard.hasProcParams()) return;

    if (!custom_) {
        custom_ = new PartialProfile(true);

        if (isLastSavedSelected()) {
            *custom_->pparams = *lastsaved_->pparams;
            custom_->pedited->locallab.spots.clear();
            custom_->pedited->locallab.spots.resize(custom_->pparams->locallab.spots.size(),
                LocallabParamsEdited::LocallabSpotEdited(false));
        } else {
            const ProfileStoreEntry* entry = getSelectedEntry();
            if (entry && entry != customPSE_ && entry != lastSavedPSE_) {
                const PartialProfile* partProfile = ProfileStore::getInstance()->getProfile(entry);
                if (partProfile) {
                    *custom_->pparams = *partProfile->pparams;
                    custom_->pedited->locallab.spots.clear();
                    custom_->pedited->locallab.spots.resize(custom_->pparams->locallab.spots.size(),
                        LocallabParamsEdited::LocallabSpotEdited(false));
                }
            }
        }

        addCustomEntry();
        selectEntry(customPSE_, false);
    } else {
        if (fillMode_->get_active()) {
            custom_->pparams->setDefaults();
            custom_->pedited->locallab.spots.clear();
        } else if (!isCustomSelected()) {
            if (isLastSavedSelected()) {
                *custom_->pparams = *lastsaved_->pparams;
                custom_->pedited->locallab.spots.clear();
                custom_->pedited->locallab.spots.resize(custom_->pparams->locallab.spots.size(),
                    LocallabParamsEdited::LocallabSpotEdited(true));
            } else {
                const ProfileStoreEntry* entry = getSelectedEntry();
                if (entry && entry != customPSE_ && entry != lastSavedPSE_) {
                    const PartialProfile* partProfile = ProfileStore::getInstance()->getProfile(entry);
                    if (partProfile) {
                        *custom_->pparams = *partProfile->pparams;
                        custom_->pedited->locallab.spots.clear();
                        custom_->pedited->locallab.spots.resize(custom_->pparams->locallab.spots.size(),
                            LocallabParamsEdited::LocallabSpotEdited(true));
                    }
                }
            }
        }

        selectEntry(customPSE_, false);
    }

    custom_->pedited->set(true);

    ProcParams pp = clipboard.getProcParams();

    if (clipboard.hasPEdited()) {
        ParamsEdited pe = clipboard.getParamsEdited();

        if (event->state & Gdk::CONTROL_MASK) {
            if (!partialProfileDlg_)
                partialProfileDlg_ = new PartialPasteDlg(Glib::ustring(), parent_);
            partialProfileDlg_->set_title(M("PROFILEPANEL_PASTEPPASTE"));
            partialProfileDlg_->updateSpotWidget(&pp);
            int i = partialProfileDlg_->run();
            partialProfileDlg_->hide();
            if (i != Gtk::RESPONSE_OK) return;
            partialProfileDlg_->applyPaste(custom_->pparams,
                !fillMode_->get_active() ? custom_->pedited : nullptr, &pp, &pe);
        } else {
            pe.combine(*custom_->pparams, pp, true);
            if (!fillMode_->get_active()) {
                *custom_->pedited = pe;
            } else {
                custom_->pedited->locallab.spots.clear();
                custom_->pedited->locallab.spots.resize(custom_->pparams->locallab.spots.size(),
                    LocallabParamsEdited::LocallabSpotEdited(true));
            }
        }
    } else {
        if (event->state & Gdk::CONTROL_MASK) {
            if (!partialProfileDlg_)
                partialProfileDlg_ = new PartialPasteDlg(Glib::ustring(), parent_);
            partialProfileDlg_->set_title(M("PROFILEPANEL_PASTEPPASTE"));
            partialProfileDlg_->updateSpotWidget(&pp);
            int i = partialProfileDlg_->run();
            partialProfileDlg_->hide();
            if (i != Gtk::RESPONSE_OK) return;
            partialProfileDlg_->applyPaste(custom_->pparams, nullptr, &pp, nullptr);
            custom_->pedited->locallab.spots.clear();
            custom_->pedited->locallab.spots.resize(custom_->pparams->locallab.spots.size(),
                LocallabParamsEdited::LocallabSpotEdited(true));
        } else {
            *custom_->pparams = pp;
            custom_->pedited->locallab.spots.clear();
            custom_->pedited->locallab.spots.resize(custom_->pparams->locallab.spots.size(),
                LocallabParamsEdited::LocallabSpotEdited(true));
        }
    }

    changeTo(custom_, M("HISTORY_FROMCLIPBOARD"));
}

// ============================================================
// Hover preview
// ============================================================

void PresetListPanel::startHoverTimer(const ProfileStoreEntry* entry)
{
    if (entry == lastHoveredEntry_) {
        return;
    }

    hoverTimeout_.disconnect();
    lastHoveredEntry_ = entry;

    hoverTimeout_ = Glib::signal_timeout().connect([this, entry]() {
        applyHoverPreview(entry);
        return false;
    }, 400);
}

void PresetListPanel::cancelHover()
{
    hoverTimeout_.disconnect();
    lastHoveredEntry_ = nullptr;
    if (hasSavedParams_) {
        revertHoverPreview();
    }
}

void PresetListPanel::applyHoverPreview(const ProfileStoreEntry* entry)
{
    if (!ipc_) return;

    // Don't preview special entries
    if (entry == customPSE_ || entry == lastSavedPSE_) return;

    if (!hasSavedParams_) {
        ipc_->getParams(&savedParams_, false);
        hasSavedParams_ = true;
    }

    const PartialProfile* profile = ProfileStore::getInstance()->getProfile(entry);
    if (!profile) return;

    ProcParams merged = savedParams_;
    profile->applyTo(&merged);

    ProcParams* params = ipc_->beginUpdateParams();
    *params = merged;
    ipc_->endUpdateParams(EvProfileChanged);
}

void PresetListPanel::revertHoverPreview()
{
    if (!ipc_ || !hasSavedParams_) return;

    ProcParams* params = ipc_->beginUpdateParams();
    *params = savedParams_;
    ipc_->endUpdateParams(EvProfileChanged);
    hasSavedParams_ = false;
}

// ============================================================
// Thumbnail generation
// ============================================================

void PresetListPanel::setThumbnail(::Thumbnail* thm)
{
    openThm_ = thm;
    startThumbnailGeneration();
}

void PresetListPanel::startThumbnailGeneration()
{
    cancelThumbnailGeneration();
    thumbCache_.clear();

    if (!openThm_) return;

    std::vector<const ProfileStoreEntry*> entries;
    collectPresetEntries(entries);

    thumbCancelled_ = false;

    thumbThread_ = std::thread([this, entries]() {
        for (const auto* entry : entries) {
            if (thumbCancelled_) break;
            generateThumbnail(entry);
        }
    });
}

void PresetListPanel::cancelThumbnailGeneration()
{
    thumbCancelled_ = true;
    if (thumbThread_.joinable()) {
        thumbThread_.join();
    }
}

void PresetListPanel::generateThumbnail(const ProfileStoreEntry* entry)
{
    if (!openThm_) return;

    const PartialProfile* profile = ProfileStore::getInstance()->getProfile(entry);
    if (!profile) return;

    ProcParams merged;
    merged.setDefaults();
    if (profile->pedited) {
        profile->pedited->combine(merged, *profile->pparams, true);
    } else {
        merged = *profile->pparams;
    }

    double scale;
    rtengine::IImage8* img = openThm_->processThumbImage(merged, THUMB_HEIGHT, scale);
    if (!img) return;

    int w = img->getWidth();
    int h = img->getHeight();

    auto pixbuf = Gdk::Pixbuf::create_from_data(
        img->getData(), Gdk::COLORSPACE_RGB, false, 8,
        w, h, w * 3);
    auto pixbufCopy = pixbuf->copy();
    delete img;

    const ProfileStoreEntry* capturedEntry = entry;
    Glib::signal_idle().connect_once([this, capturedEntry, pixbufCopy]() {
        if (!thumbCancelled_) {
            thumbCache_[capturedEntry] = pixbufCopy;
            auto it = thumbImageMap_.find(capturedEntry);
            if (it != thumbImageMap_.end() && it->second) {
                it->second->set(pixbufCopy);
            }
        }
    });
}

void PresetListPanel::collectPresetEntries(std::vector<const ProfileStoreEntry*>& entries)
{
    for (const auto& pair : cardMap_) {
        entries.push_back(pair.first);
    }
}
