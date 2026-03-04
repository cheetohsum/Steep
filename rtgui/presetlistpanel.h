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
#pragma once

#include "guiutils.h"
#include "pparamschangelistener.h"
#include "profilechangelistener.h"
#include "windows/partialpastedlg.h"

#include "rtengine/noncopyable.h"
#include "rtengine/profilestore.h"
#include "rtengine/procparams.h"

#include <gtkmm.h>

#include <atomic>
#include <map>
#include <thread>
#include <vector>

class Thumbnail;

namespace rtengine
{

class ProcEvent;
class StagedImageProcessor;

namespace procparams
{
class ProcParams;
class PartialProfile;
}

}

class RTImage;

class PresetListPanel final :
    public PParamsChangeListener,
    public ProfileStoreListener,
    public rtengine::NonCopyable
{
public:
    explicit PresetListPanel();
    ~PresetListPanel();

    Gtk::ScrolledWindow* getWidget() const { return scrolledWin_; }

    void setProfileChangeListener(ProfileChangeListener* ppl) { tpc_ = ppl; }
    void setImageProcessor(rtengine::StagedImageProcessor* ipc) { ipc_ = ipc; }
    void setThumbnail(::Thumbnail* thm);

    static void init(Gtk::Window* parentWindow);
    static void cleanup();

    // ProfileStoreListener (no longer a widget — these are plain virtual overrides)
    void storeCurrentValue() override;
    void updateProfileList() override;
    void restoreValue() override;

    void initProfile(const Glib::ustring& profileFullPath, rtengine::procparams::ProcParams* lastSaved);
    void setInitialFileName(const Glib::ustring& filename);

    // PParamsChangeListener
    void procParamsChanged(
        const rtengine::procparams::ProcParams* params,
        const rtengine::ProcEvent& ev,
        const Glib::ustring& descr,
        const ParamsEdited* paramsEdited = nullptr
    ) override;
    void clearParamChanges() override;

    void writeOptions();

private:
    static constexpr int THUMB_HEIGHT = 48;
    static constexpr int CARD_MIN_WIDTH = 80;

    // Toolbar callbacks
    void profileFillModeToggled();
    void save_clicked(GdkEventButton* event);
    void load_clicked(GdkEventButton* event);
    void copy_clicked(GdkEventButton* event);
    void paste_clicked(GdkEventButton* event);

    // Grid content
    void buildContent();
    void buildCategoryContent(Gtk::Box* parent, int parentFolderId,
                              const std::vector<const ProfileStoreEntry*>* entryList);
    Gtk::Button* createCard(const ProfileStoreEntry* entry);

    // Selection
    void selectEntry(const ProfileStoreEntry* entry, bool fireChange = true);
    void highlightSelection();
    void changeTo(const rtengine::procparams::PartialProfile* newpp, Glib::ustring profname);

    // Special entries
    bool isCustomSelected() const;
    bool isLastSavedSelected() const;
    const ProfileStoreEntry* getSelectedEntry() const;
    Glib::ustring getCurrentLabel() const;
    Glib::ustring getFullPathFromActiveRow() const;
    void addCustomEntry();
    void addLastSavedEntry();

    // Hover preview
    void applyHoverPreview(const ProfileStoreEntry* entry);
    void revertHoverPreview();
    void startHoverTimer(const ProfileStoreEntry* entry);
    void cancelHover();

    // Thumbnail generation
    void startThumbnailGeneration();
    void cancelThumbnailGeneration();
    void generateThumbnail(const ProfileStoreEntry* entry);
    void collectPresetEntries(std::vector<const ProfileStoreEntry*>& entries);

    // Layout widgets
    Gtk::ScrolledWindow* scrolledWin_;
    Gtk::Box* contentBox_;
    Gtk::FlowBox* specialFlowBox_;
    Gtk::Box* gridBox_;
    Gtk::Button* customButton_;
    Gtk::Button* lastSavedButton_;

    // Toolbar
    const Glib::ustring modeOn_, modeOff_;
    RTImage* profileFillImage_;
    Gtk::ToggleButton* fillMode_;
    Gtk::Button* save_;
    Gtk::Button* load_;
    Gtk::Button* copy_;
    Gtk::Button* paste_;

    // Card tracking
    std::map<const ProfileStoreEntry*, Gtk::Button*> cardMap_;
    std::map<const ProfileStoreEntry*, Gtk::Image*> thumbImageMap_;
    const ProfileStoreEntry* selectedEntry_;
    Gtk::Widget* selectedWidget_;

    // Profile state
    rtengine::procparams::PartialProfile* custom_;
    rtengine::procparams::PartialProfile* lastsaved_;
    ProfileStoreEntry* lastSavedPSE_;
    ProfileStoreEntry* customPSE_;
    ProfileChangeListener* tpc_;
    bool dontupdate_;
    rtengine::procparams::PartialProfile* storedPProfile_;
    Glib::ustring storedValue_;
    Glib::ustring lastFilename_;
    Glib::ustring imagePath_;

    // Hover state
    rtengine::StagedImageProcessor* ipc_;
    rtengine::procparams::ProcParams savedParams_;
    bool hasSavedParams_;
    const ProfileStoreEntry* lastHoveredEntry_;
    sigc::connection hoverTimeout_;

    // Thumbnail state
    ::Thumbnail* openThm_;
    std::map<const ProfileStoreEntry*, Glib::RefPtr<Gdk::Pixbuf>> thumbCache_;
    std::thread thumbThread_;
    std::atomic<bool> thumbCancelled_;

    static PartialPasteDlg* partialProfileDlg_;
    static Gtk::Window* parent_;
};
