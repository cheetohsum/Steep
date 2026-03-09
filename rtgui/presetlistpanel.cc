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

#include <glib/gstdio.h>

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
    modeOn_("preset-fill-on"),
    modeOff_("preset-fill-off"),
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
    load_->add(*Gtk::manage(new RTImage("preset-load", Gtk::ICON_SIZE_LARGE_TOOLBAR)));
    load_->get_style_context()->add_class("Left");
    setExpandAlignProperties(load_, false, true, Gtk::ALIGN_CENTER, Gtk::ALIGN_FILL);

    save_ = Gtk::manage(new Gtk::Button());
    save_->add(*Gtk::manage(new RTImage("preset-save", Gtk::ICON_SIZE_LARGE_TOOLBAR)));
    save_->get_style_context()->add_class("MiddleH");
    setExpandAlignProperties(save_, false, true, Gtk::ALIGN_CENTER, Gtk::ALIGN_FILL);

    copy_ = Gtk::manage(new Gtk::Button());
    copy_->add(*Gtk::manage(new RTImage("preset-copy", Gtk::ICON_SIZE_LARGE_TOOLBAR)));
    copy_->get_style_context()->add_class("MiddleH");
    setExpandAlignProperties(copy_, false, true, Gtk::ALIGN_CENTER, Gtk::ALIGN_FILL);

    paste_ = Gtk::manage(new Gtk::Button());
    paste_->add(*Gtk::manage(new RTImage("preset-paste", Gtk::ICON_SIZE_LARGE_TOOLBAR)));
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
    contentBox_->set_margin_end(3);
    contentBox_->pack_start(*toolbar, Gtk::PACK_SHRINK, 0);

    // Special entries FlowBox (Custom / Last Saved) — card-style
    specialFlowBox_ = Gtk::manage(new Gtk::FlowBox());
    specialFlowBox_->set_selection_mode(Gtk::SELECTION_NONE);
    specialFlowBox_->set_homogeneous(true);
    specialFlowBox_->set_min_children_per_line(3);
    specialFlowBox_->set_max_children_per_line(20);
    specialFlowBox_->set_column_spacing(1);
    specialFlowBox_->set_row_spacing(1);
    specialFlowBox_->set_no_show_all(true);

    customButton_ = Gtk::manage(new Gtk::Button());
    customButton_->set_relief(Gtk::RELIEF_NONE);
    customButton_->get_style_context()->add_class("preset-card");
    customButton_->get_style_context()->add_class("preset-special");
    customButton_->set_size_request(CARD_MIN_WIDTH, -1);
    {
        auto* vbox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 1));
        vbox->set_halign(Gtk::ALIGN_CENTER);
        auto* img = Gtk::manage(new Gtk::Image());
        img->set_size_request(-1, THUMB_HEIGHT);
        img->set_halign(Gtk::ALIGN_CENTER);
        vbox->pack_start(*img, Gtk::PACK_SHRINK);
        auto* label = Gtk::manage(new Gtk::Label());
        label->set_line_wrap(true);
        label->set_line_wrap_mode(Pango::WRAP_WORD_CHAR);
        label->set_max_width_chars(14);
        label->set_xalign(0.5);
        label->set_justify(Gtk::JUSTIFY_CENTER);
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
        vbox->set_halign(Gtk::ALIGN_CENTER);
        auto* img = Gtk::manage(new Gtk::Image());
        img->set_size_request(-1, THUMB_HEIGHT);
        img->set_halign(Gtk::ALIGN_CENTER);
        vbox->pack_start(*img, Gtk::PACK_SHRINK);
        auto* label = Gtk::manage(new Gtk::Label());
        label->set_line_wrap(true);
        label->set_line_wrap_mode(Pango::WRAP_WORD_CHAR);
        label->set_max_width_chars(14);
        label->set_xalign(0.5);
        label->set_justify(Gtk::JUSTIFY_CENTER);
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
    gridBox_ = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 0));
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
    categoryHeaders_.clear();

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
        flowBox->set_homogeneous(true);
        flowBox->set_min_children_per_line(3);
        flowBox->set_max_children_per_line(20);
        flowBox->set_column_spacing(1);
        flowBox->set_row_spacing(1);
        for (auto* entry : rootFiles) {
            flowBox->add(*createCard(entry));
        }
        gridBox_->pack_start(*flowBox, Gtk::PACK_SHRINK);
    }

    // Root-level folders as animated categories
    for (auto* entry : *entryList) {
        if (entry->parentFolderId == rootFolderId && entry->type == PSET_FOLDER) {
            Glib::ustring folderPath(ProfileStore::getInstance()->getPathFromId(entry->folderId));
            if (App::get().options().useBundledProfiles ||
                ((folderPath != "${G}") && (folderPath != "${U}"))) {
                // Animated category with Revealer
                auto* catBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 0));
                catBox->get_style_context()->add_class("preset-category");

                auto* headerEvBox = Gtk::manage(new Gtk::EventBox());
                auto* headerRow = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 4));
                auto* arrowLabel = Gtk::manage(new Gtk::Label());
                auto* nameLabel = Gtk::manage(new Gtk::Label(entry->label));
                nameLabel->get_style_context()->add_class("preset-category-label");
                nameLabel->set_xalign(0.0);
                headerRow->pack_start(*arrowLabel, Gtk::PACK_SHRINK);
                headerRow->pack_start(*nameLabel, Gtk::PACK_EXPAND_WIDGET);
                headerEvBox->add(*headerRow);

                auto* revealer = Gtk::manage(new Gtk::Revealer());
                revealer->set_transition_type(Gtk::REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
                revealer->set_transition_duration(200);

                bool expanded = true;
                auto expIt = categoryExpanded_.find(entry->folderId);
                if (expIt != categoryExpanded_.end()) {
                    expanded = expIt->second;
                }
                revealer->set_reveal_child(expanded);
                arrowLabel->set_text(expanded ? "\xe2\x96\xbe" : "\xe2\x96\xb8");
                categoryExpanded_[entry->folderId] = expanded;

                int folderId = entry->folderId;
                headerEvBox->signal_button_press_event().connect(
                    [this, revealer, arrowLabel, folderId](GdkEventButton* event) -> bool {
                        if (event->button == 1) {
                            bool nowExpanded = !revealer->get_reveal_child();
                            revealer->set_reveal_child(nowExpanded);
                            arrowLabel->set_text(nowExpanded ? "\xe2\x96\xbe" : "\xe2\x96\xb8");
                            categoryExpanded_[folderId] = nowExpanded;
                            return true;
                        }
                        if (event->button == 3) {
                            showGroupContextMenu(event, folderId);
                            return true;
                        }
                        return false;
                    }, false);

                auto* box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 0));
                revealer->add(*box);
                catBox->pack_start(*headerEvBox, Gtk::PACK_SHRINK);
                catBox->pack_start(*revealer, Gtk::PACK_SHRINK);

                buildCategoryContent(box, entry->folderId, entryList);
                gridBox_->pack_start(*catBox, Gtk::PACK_SHRINK);
                categoryHeaders_[folderId] = headerEvBox;
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
        flowBox->set_homogeneous(true);
        flowBox->set_min_children_per_line(3);
        flowBox->set_max_children_per_line(20);
        flowBox->set_column_spacing(1);
        flowBox->set_row_spacing(1);

        for (auto* entry : files) {
            auto* card = createCard(entry);
            flowBox->add(*card);
        }

        parent->pack_start(*flowBox, Gtk::PACK_SHRINK);
    }

    // Add sub-folders as animated categories
    for (auto* folder : folders) {
        Glib::ustring folderPath(ProfileStore::getInstance()->getPathFromId(folder->folderId));

        if (App::get().options().useBundledProfiles ||
            ((folderPath != "${G}") && (folderPath != "${U}"))) {
            auto* catBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 0));
            catBox->get_style_context()->add_class("preset-category");

            auto* headerEvBox = Gtk::manage(new Gtk::EventBox());
            auto* headerRow = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 4));
            auto* arrowLabel = Gtk::manage(new Gtk::Label());
            auto* nameLabel = Gtk::manage(new Gtk::Label(folder->label));
            nameLabel->get_style_context()->add_class("preset-category-label");
            nameLabel->set_xalign(0.0);
            headerRow->pack_start(*arrowLabel, Gtk::PACK_SHRINK);
            headerRow->pack_start(*nameLabel, Gtk::PACK_EXPAND_WIDGET);
            headerEvBox->add(*headerRow);

            auto* revealer = Gtk::manage(new Gtk::Revealer());
            revealer->set_transition_type(Gtk::REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
            revealer->set_transition_duration(200);

            bool expanded = true;
            auto expIt = categoryExpanded_.find(folder->folderId);
            if (expIt != categoryExpanded_.end()) {
                expanded = expIt->second;
            }
            revealer->set_reveal_child(expanded);
            arrowLabel->set_text(expanded ? "\xe2\x96\xbe" : "\xe2\x96\xb8");
            categoryExpanded_[folder->folderId] = expanded;

            int folderId = folder->folderId;
            headerEvBox->signal_button_press_event().connect(
                [this, revealer, arrowLabel, folderId](GdkEventButton* event) -> bool {
                    if (event->button == 1) {
                        bool nowExpanded = !revealer->get_reveal_child();
                        revealer->set_reveal_child(nowExpanded);
                        arrowLabel->set_text(nowExpanded ? "\xe2\x96\xbe" : "\xe2\x96\xb8");
                        categoryExpanded_[folderId] = nowExpanded;
                        return true;
                    }
                    if (event->button == 3) {
                        showGroupContextMenu(event, folderId);
                        return true;
                    }
                    return false;
                }, false);

            auto* box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 2));
            revealer->add(*box);
            catBox->pack_start(*headerEvBox, Gtk::PACK_SHRINK);
            catBox->pack_start(*revealer, Gtk::PACK_SHRINK);

            buildCategoryContent(box, folder->folderId, entryList);
            parent->pack_start(*catBox, Gtk::PACK_SHRINK);
            categoryHeaders_[folderId] = headerEvBox;
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

    auto* vbox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 0));
    vbox->set_halign(Gtk::ALIGN_CENTER);

    auto* img = Gtk::manage(new Gtk::Image());
    img->set_size_request(-1, THUMB_HEIGHT);
    img->set_halign(Gtk::ALIGN_CENTER);
    vbox->pack_start(*img, Gtk::PACK_SHRINK);

    auto* label = Gtk::manage(new Gtk::Label(entry->label));
    label->set_line_wrap(true);
    label->set_line_wrap_mode(Pango::WRAP_WORD_CHAR);
    label->set_max_width_chars(14);
    label->set_xalign(0.5);
    label->set_justify(Gtk::JUSTIFY_CENTER);
    label->get_style_context()->add_class("preset-card-label");
    {
        auto css = Gtk::CssProvider::create();
        css->load_from_data("label { font-size: 8px; }");
        label->get_style_context()->add_provider(css, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 200);
    }
    vbox->pack_start(*label, Gtk::PACK_SHRINK);

    card->add(*vbox);

    // Button press: right-click context menu + DnD start tracking
    card->add_events(Gdk::ENTER_NOTIFY_MASK | Gdk::LEAVE_NOTIFY_MASK |
                     Gdk::BUTTON_PRESS_MASK | Gdk::BUTTON_RELEASE_MASK |
                     Gdk::POINTER_MOTION_MASK);

    card->signal_button_press_event().connect(
        [this, entry](GdkEventButton* event) -> bool {
            if (event->button == 3) {
                showCardContextMenu(event, entry);
                return true;
            }
            if (event->button == 1) {
                dragEntry_ = entry;
                dragStartX_ = event->x_root;
                dragStartY_ = event->y_root;
                dragActive_ = false;
            }
            return false;
        }, false);

    card->signal_motion_notify_event().connect(
        [this](GdkEventMotion* event) -> bool {
            if (dragEntry_ && !dragActive_) {
                double dx = event->x_root - dragStartX_;
                double dy = event->y_root - dragStartY_;
                if (dx * dx + dy * dy > 25) {
                    dragActive_ = true;
                    if (auto win = scrolledWin_->get_window()) {
                        auto display = Gdk::Display::get_default();
                        auto cursor = Gdk::Cursor::create(display, Gdk::HAND1);
                        win->set_cursor(cursor);
                    }
                }
            }
            if (dragActive_) {
                highlightDropTarget(event->x_root, event->y_root);
            }
            return false;
        }, false);

    card->signal_button_release_event().connect(
        [this, entry](GdkEventButton* event) -> bool {
            if (dragActive_ && event->button == 1) {
                completeDrop(event->x_root, event->y_root);
                dragEntry_ = nullptr;
                dragActive_ = false;
                if (auto win = scrolledWin_->get_window()) {
                    win->set_cursor();
                }
                clearDropHighlight();
                return true;  // prevent "clicked" from firing
            }
            dragEntry_ = nullptr;
            dragActive_ = false;
            return false;
        }, false);

    // Click to select (only fires when release is inside button and not dragging)
    card->signal_clicked().connect([this, entry]() {
        if (!dragActive_) {
            selectEntry(entry);
        }
    });

    // Hover events for preview
    card->signal_enter_notify_event().connect([this, entry](GdkEventCrossing*) -> bool {
        if (!dragActive_) {
            startHoverTimer(entry);
        }
        return false;
    }, false);
    card->signal_leave_notify_event().connect([this](GdkEventCrossing*) -> bool {
        if (!dragActive_) {
            cancelHover();
        }
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
    fprintf(stderr, "DBG initProfile: enter\n");
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

    fprintf(stderr, "DBG initProfile: before updateProfileList\n");
    updateProfileList();

    if (lastsaved_) {
        addLastSavedEntry();
    }

    if (!(pse = ProfileStore::getInstance()->findEntryFromFullPath(profileFullPath))) {
        pse = ProfileStore::getInstance()->getInternalDefaultPSE();
    }

    defprofile = ProfileStore::getInstance()->getProfile(pse);

    fprintf(stderr, "DBG initProfile: before profileChange lastsaved_=%p tpc_=%p\n", (void*)lastsaved_, (void*)tpc_);
    if (lastsaved_) {
        selectEntry(lastSavedPSE_, false);

        if (tpc_) {
            tpc_->setDefaults(lastsaved_->pparams);
            fprintf(stderr, "DBG initProfile: calling profileChange (lastsaved)\n");
            tpc_->profileChange(lastsaved_, EvPhotoLoaded,
                getCurrentLabel(), nullptr, true);
            fprintf(stderr, "DBG initProfile: profileChange returned\n");
        }
    } else {
        selectEntry(pse, false);

        if (tpc_) {
            tpc_->setDefaults(defprofile->pparams);
            fprintf(stderr, "DBG initProfile: calling profileChange (default)\n");
            tpc_->profileChange(defprofile, EvPhotoLoaded, getCurrentLabel());
            fprintf(stderr, "DBG initProfile: profileChange returned\n");
        }
    }

    fprintf(stderr, "DBG initProfile: before startThumbnailGeneration\n");
    startThumbnailGeneration();
    fprintf(stderr, "DBG initProfile: done\n");
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

// ============================================================
// Path resolution helpers
// ============================================================

Glib::ustring PresetListPanel::resolveVirtualPath(const Glib::ustring& virtualPath) const
{
    auto& options = App::get().options();
    Glib::ustring result = virtualPath;
    if (result.length() >= 4 && result.substr(0, 4) == "${U}") {
        result = options.getUserProfilePath() + result.substr(4);
    } else if (result.length() >= 4 && result.substr(0, 4) == "${G}") {
        result = options.getGlobalProfilePath() + result.substr(4);
    }
    return result;
}

bool PresetListPanel::isUserPreset(const ProfileStoreEntry* entry) const
{
    if (!entry) return false;
    Glib::ustring path = ProfileStore::getInstance()->getPathFromId(entry->parentFolderId);
    return path.length() >= 4 && path.substr(0, 4) == "${U}";
}

// ============================================================
// Context menus (Phase 4)
// ============================================================

void PresetListPanel::showCardContextMenu(GdkEventButton* event, const ProfileStoreEntry* entry)
{
    auto* menu = Gtk::manage(new Gtk::Menu());

    // "Copy Edit Settings" — always available when an image is open
    if (ipc_) {
        auto* copyItem = Gtk::manage(new Gtk::MenuItem(M("PRESET_COPY_SETTINGS")));
        copyItem->signal_activate().connect([this]() {
            if (!ipc_) return;
            ProcParams currentParams;
            ipc_->getParams(&currentParams, false);
            clipboard.setProcParams(currentParams);
        });
        menu->append(*copyItem);
    }

    // "Paste Clipboard Settings" — visible when clipboard has data
    if (clipboard.hasProcParams()) {
        auto* pasteItem = Gtk::manage(new Gtk::MenuItem(M("PRESET_PASTE_CLIPBOARD")));
        pasteItem->signal_activate().connect([this]() {
            if (!clipboard.hasProcParams()) return;

            if (!custom_) {
                custom_ = new PartialProfile(true);
            }

            ProcParams pp = clipboard.getProcParams();
            *custom_->pparams = pp;
            custom_->pedited->set(true);
            custom_->pedited->locallab.spots.clear();
            custom_->pedited->locallab.spots.resize(pp.locallab.spots.size(),
                LocallabParamsEdited::LocallabSpotEdited(true));

            addCustomEntry();
            selectEntry(customPSE_, false);
            changeTo(custom_, M("HISTORY_FROMCLIPBOARD"));
        });
        menu->append(*pasteItem);
        menu->append(*Gtk::manage(new Gtk::SeparatorMenuItem()));
    }

    // "Apply" — always available
    auto* applyItem = Gtk::manage(new Gtk::MenuItem(M("PRESET_APPLY")));
    applyItem->signal_activate().connect([this, entry]() {
        selectEntry(entry);
    });
    menu->append(*applyItem);

    if (isUserPreset(entry)) {
        // "Rename"
        auto* renameItem = Gtk::manage(new Gtk::MenuItem(M("PRESET_RENAME")));
        renameItem->signal_activate().connect([this, entry]() {
            renamePreset(entry);
        });
        menu->append(*renameItem);

        // "Save Current Settings"
        auto* overwriteItem = Gtk::manage(new Gtk::MenuItem(M("PRESET_SAVE_CURRENT")));
        overwriteItem->signal_activate().connect([this, entry]() {
            overwritePreset(entry);
        });
        menu->append(*overwriteItem);

        menu->append(*Gtk::manage(new Gtk::SeparatorMenuItem()));

        // "Delete"
        auto* deleteItem = Gtk::manage(new Gtk::MenuItem(M("PRESET_DELETE")));
        deleteItem->signal_activate().connect([this, entry]() {
            deletePreset(entry);
        });
        menu->append(*deleteItem);
    }

    menu->show_all();
    menu->popup(event->button, event->time);
}

void PresetListPanel::renamePreset(const ProfileStoreEntry* entry)
{
    if (!entry || !isUserPreset(entry)) return;

    Glib::ustring virtualDir = ProfileStore::getInstance()->getPathFromId(entry->parentFolderId);
    Glib::ustring realDir = resolveVirtualPath(virtualDir);
    Glib::ustring oldPath = Glib::build_filename(realDir, entry->label + ".pp3");

    Gtk::Dialog dialog(M("PRESET_RENAME"), getToplevelWindow(scrolledWin_), true);
    dialog.add_button(M("GENERAL_CANCEL"), Gtk::RESPONSE_CANCEL);
    dialog.add_button(M("GENERAL_OK"), Gtk::RESPONSE_OK);

    auto* nameEntry = Gtk::manage(new Gtk::Entry());
    nameEntry->set_text(entry->label);
    nameEntry->set_activates_default(true);
    dialog.get_content_area()->pack_start(*nameEntry, Gtk::PACK_SHRINK, 8);
    dialog.set_default_response(Gtk::RESPONSE_OK);
    dialog.show_all();

    if (dialog.run() == Gtk::RESPONSE_OK) {
        Glib::ustring newName = nameEntry->get_text();
        if (!newName.empty() && newName != entry->label) {
            Glib::ustring newPath = Glib::build_filename(realDir, newName + ".pp3");
            if (g_rename(oldPath.c_str(), newPath.c_str()) == 0) {
                ProfileStore::getInstance()->parseProfiles();
            }
        }
    }
}

void PresetListPanel::overwritePreset(const ProfileStoreEntry* entry)
{
    if (!entry || !isUserPreset(entry) || !ipc_) return;

    Glib::ustring virtualDir = ProfileStore::getInstance()->getPathFromId(entry->parentFolderId);
    Glib::ustring realDir = resolveVirtualPath(virtualDir);
    Glib::ustring filePath = Glib::build_filename(realDir, entry->label + ".pp3");

    ProcParams currentParams;
    ipc_->getParams(&currentParams, false);

    if (currentParams.save(filePath) == 0) {
        ProfileStore::getInstance()->parseProfiles();
    }
}

void PresetListPanel::deletePreset(const ProfileStoreEntry* entry)
{
    if (!entry || !isUserPreset(entry)) return;

    // Confirmation dialog
    Glib::ustring msg = M("PRESET_CONFIRM_DELETE");
    auto pos = msg.find("%1");
    if (pos != Glib::ustring::npos) {
        msg.replace(pos, 2, entry->label);
    }

    Gtk::MessageDialog dialog(getToplevelWindow(scrolledWin_), msg,
                              false, Gtk::MESSAGE_QUESTION, Gtk::BUTTONS_YES_NO, true);
    if (dialog.run() != Gtk::RESPONSE_YES) return;

    Glib::ustring virtualDir = ProfileStore::getInstance()->getPathFromId(entry->parentFolderId);
    Glib::ustring realDir = resolveVirtualPath(virtualDir);
    Glib::ustring filePath = Glib::build_filename(realDir, entry->label + ".pp3");

    if (g_unlink(filePath.c_str()) == 0) {
        if (selectedEntry_ == entry) {
            selectedEntry_ = nullptr;
            selectedWidget_ = nullptr;
        }
        ProfileStore::getInstance()->parseProfiles();
    }
}

void PresetListPanel::showGroupContextMenu(GdkEventButton* event, int folderId)
{
    Glib::ustring folderPath = ProfileStore::getInstance()->getPathFromId(folderId);
    if (folderPath.length() < 4 || folderPath.substr(0, 4) != "${U}") return;

    auto* menu = Gtk::manage(new Gtk::Menu());

    auto* renameItem = Gtk::manage(new Gtk::MenuItem(M("PRESET_RENAME_GROUP")));
    renameItem->signal_activate().connect([this, folderId]() {
        renameGroup(folderId);
    });
    menu->append(*renameItem);

    menu->show_all();
    menu->popup(event->button, event->time);
}

void PresetListPanel::renameGroup(int folderId)
{
    Glib::ustring virtualPath = ProfileStore::getInstance()->getPathFromId(folderId);
    if (virtualPath.length() < 4 || virtualPath.substr(0, 4) != "${U}") return;

    Glib::ustring realPath = resolveVirtualPath(virtualPath);
    Glib::ustring oldName = Glib::path_get_basename(realPath);

    Gtk::Dialog dialog(M("PRESET_RENAME_GROUP"), getToplevelWindow(scrolledWin_), true);
    dialog.add_button(M("GENERAL_CANCEL"), Gtk::RESPONSE_CANCEL);
    dialog.add_button(M("GENERAL_OK"), Gtk::RESPONSE_OK);

    auto* nameEntry = Gtk::manage(new Gtk::Entry());
    nameEntry->set_text(oldName);
    nameEntry->set_activates_default(true);
    dialog.get_content_area()->pack_start(*nameEntry, Gtk::PACK_SHRINK, 8);
    dialog.set_default_response(Gtk::RESPONSE_OK);
    dialog.show_all();

    if (dialog.run() == Gtk::RESPONSE_OK) {
        Glib::ustring newName = nameEntry->get_text();
        if (!newName.empty() && newName != oldName) {
            Glib::ustring parentDir = Glib::path_get_dirname(realPath);
            Glib::ustring newPath = Glib::build_filename(parentDir, newName);
            if (g_rename(realPath.c_str(), newPath.c_str()) == 0) {
                ProfileStore::getInstance()->parseProfiles();
            }
        }
    }
}

// ============================================================
// Drag-and-Drop (Phase 5)
// ============================================================

void PresetListPanel::highlightDropTarget(double x_root, double y_root)
{
    clearDropHighlight();

    for (auto& pair : categoryHeaders_) {
        Gtk::Widget* header = pair.second;
        if (!header->get_realized() || !header->get_window()) continue;

        int screen_x, screen_y;
        header->get_window()->get_root_coords(0, 0, screen_x, screen_y);
        int hw = header->get_allocated_width();
        int hh = header->get_allocated_height();

        if (x_root >= screen_x && x_root <= screen_x + hw &&
            y_root >= screen_y && y_root <= screen_y + hh) {
            header->get_style_context()->add_class("preset-drop-target");
            dropTargetWidget_ = header;
            dropTargetFolderId_ = pair.first;
            return;
        }
    }

    dropTargetFolderId_ = -1;
}

void PresetListPanel::completeDrop(double x_root, double y_root)
{
    highlightDropTarget(x_root, y_root);

    if (dropTargetFolderId_ < 0 || !dragEntry_) return;
    if (!isUserPreset(dragEntry_)) return;

    // Check target is also a user folder
    Glib::ustring targetVirtualPath = ProfileStore::getInstance()->getPathFromId(dropTargetFolderId_);
    if (targetVirtualPath.length() < 4 || targetVirtualPath.substr(0, 4) != "${U}") return;

    // Don't drop onto same folder
    if (dropTargetFolderId_ == dragEntry_->parentFolderId) return;

    Glib::ustring srcVirtualDir = ProfileStore::getInstance()->getPathFromId(dragEntry_->parentFolderId);
    Glib::ustring srcRealDir = resolveVirtualPath(srcVirtualDir);
    Glib::ustring srcPath = Glib::build_filename(srcRealDir, dragEntry_->label + ".pp3");

    Glib::ustring dstRealDir = resolveVirtualPath(targetVirtualPath);
    Glib::ustring dstPath = Glib::build_filename(dstRealDir, dragEntry_->label + ".pp3");

    if (g_rename(srcPath.c_str(), dstPath.c_str()) == 0) {
        ProfileStore::getInstance()->parseProfiles();
    }
}

void PresetListPanel::clearDropHighlight()
{
    if (dropTargetWidget_) {
        dropTargetWidget_->get_style_context()->remove_class("preset-drop-target");
        dropTargetWidget_ = nullptr;
    }
    dropTargetFolderId_ = -1;
}
