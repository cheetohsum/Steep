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

#include <atomic>
#include <memory>
#include <thread>

#include "browserfilter.h"
#include "exportpanel.h"
#include "extprog.h"
#include "filebrowserentry.h"
#include "pparamschangelistener.h"
#include "thumbbrowserbase.h"
#include "widgets/basic/lwbutton.h"
#include "windows/partialpastedlg.h"

#include "rtengine/noncopyable.h"
#include "rtengine/imageformat.h"
#include "rtengine/profilestore.h"

#include <gtkmm.h>

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class FileBrowser;
class FileBrowserEntry;
class ProfileStoreLabel;

class FileBrowserListener
{
public:
    virtual ~FileBrowserListener() = default;
    virtual void filterApplied() = 0;
    virtual void openRequested(const std::vector<Thumbnail*>& tbe, eRTNav preloadDirectionHint = NAV_NONE) = 0;
    virtual void developRequested(const std::vector<FileBrowserEntry*>& tbe, bool fastmode) = 0;
    virtual void renameRequested(const std::vector<FileBrowserEntry*>& tbe) = 0;
    virtual void deleteRequested(const std::vector<FileBrowserEntry*>& tbe, bool inclBatchProcessed, bool onlySelected) = 0;
    virtual void copyMoveRequested(const std::vector<FileBrowserEntry*>& tbe, bool moveRequested) = 0;
    virtual void selectionChanged(const std::vector<Thumbnail*>& tbe) = 0;
    virtual void clearFromCacheRequested(const std::vector<FileBrowserEntry*>& tbe, bool leavenotrace) = 0;
    virtual void quickActionProgress(const Glib::ustring& text, double progress) = 0;
    virtual bool transientEditPreviewRequested(
        const Glib::ustring& filename,
        const rtengine::procparams::ProcParams* params,
        bool restore) = 0;
    virtual bool isInTabMode() const = 0;
};

/*
 * Class handling actions common to all thumbnails of the file browser
 */
class FileBrowser final : public ThumbBrowserBase,
    public LWButtonListener,
    public ExportPanelListener,
    public ProfileStoreListener,
    public rtengine::NonCopyable
{
private:
    typedef sigc::signal<void> type_trash_changed;
    typedef sigc::signal<void> type_save_image_requested;
    typedef sigc::signal<void> type_external_editor_requested;

    using ThumbBrowserBase::redrawNeeded;

    IdleRegister idle_register;
    unsigned int session_id_;
    bool selectionNotifyIdlePending_;

    std::vector<ThumbBrowserEntryBase*> pendingDeletion_;
    std::unordered_set<std::string> entryKeys_;
    std::unordered_map<std::string, ThumbBrowserEntryBase*> entriesByKey_;
    std::unordered_map<std::string, std::vector<ThumbBrowserEntryBase*>> originalFamilies_;
    bool originalFamiliesCurrent_ = true;
    std::unordered_map<std::string, size_t> entryIndex_;
    Glib::ustring lastOpenRequestedFname_;
    sigc::connection deletionConnection_;
    bool onDeletionIdle_();
    void markEntryIndexDirty_();
    ThumbBrowserEntryBase* findEntryLocked_(const Glib::ustring& fname);
    std::ptrdiff_t findEntryIndexLocked_(const Glib::ustring& fname);
    std::ptrdiff_t findEntryIndexReadLocked_(const Glib::ustring& fname);
    std::ptrdiff_t findEntryIndexLocked_(ThumbBrowserEntryBase* entry);
    void flushPendingInsertsForSelection_();
    void entriesOrderChanged_() override;
    void entriesInserted_(const std::vector<ThumbBrowserEntryBase*>& entries) override;
    void addOriginalFamilyEntry_(ThumbBrowserEntryBase* entry);
    void removeOriginalFamilyEntry_(ThumbBrowserEntryBase* entry);
    void ensureOriginalFamiliesCurrent_();
    void clearOriginalMarks_();
    void refreshOriginalFamily_(const std::string& familyKey);
    void refreshAllOriginalFamilies_();

protected:
    MyImageMenuItem* rank[6];
    MyImageMenuItem* colorlabel[6];
    MyImageMenuItem* pickFlag;
    MyImageMenuItem* rejectFlag;
    MyImageMenuItem* unflagFlag;
    MyImageMenuItem* trash;
    MyImageMenuItem* untrash;
    MyImageMenuItem* develop;
    MyImageMenuItem* developfast;
    MyImageMenuItem* saveImage;
    MyImageMenuItem* editExternal;
    MyImageMenuItem* rename;
    MyImageMenuItem* remove;
    MyImageMenuItem* removeInclProc;
    MyImageMenuItem* open;
    MyImageMenuItem* inspect;
    MyImageMenuItem* selall;
    Gtk::RadioMenuItem* sortMethod[Options::SORT_METHOD_COUNT];
    Gtk::RadioMenuItem* sortOrder[2];
    MyImageMenuItem* copyTo;
    MyImageMenuItem* moveTo;

    MyImageMenuItem* menuSort;
    MyImageMenuItem* menuRank;
    MyImageMenuItem* menuLabel;
    MyImageMenuItem* menuFileOperations;
    MyImageMenuItem* menuProfileOperations;
    Gtk::MenuItem* menuExtProg;
    Gtk::MenuItem** amiExtProg;
    MyImageMenuItem* miOpenDefaultViewer;
    std::map<Glib::ustring, const ExtProgAction*> mMenuExtProgs;  // key is menuitem label

    MyImageMenuItem* menuDF;
    MyImageMenuItem* selectDF;
    MyImageMenuItem* thisIsDF;
    MyImageMenuItem* autoDF;

    MyImageMenuItem* menuFF;
    MyImageMenuItem* selectFF;
    MyImageMenuItem* thisIsFF;
    MyImageMenuItem* autoFF;

    MyImageMenuItem* copyprof;
    MyImageMenuItem* copyprofSettings;
    std::map<std::string, Gtk::CheckMenuItem*> copyFilters_;
    MyImageMenuItem* pasteprof;
    MyImageMenuItem* partpasteprof;
    MyImageMenuItem* applyprof;
    MyImageMenuItem* applypartprof;
    MyImageMenuItem* resetdefaultprof;
    MyImageMenuItem* clearprof;
    MyImageMenuItem* cachemenu;
    MyImageMenuItem* aiDenoise;
    MyImageMenuItem* autoEditMenu;
    MyImageMenuItem* autoGrade;
    MyImageMenuItem* autoGradeFilm;
    MyImageMenuItem* autoGradedFilm;
    MyImageMenuItem* autoLevel;
    MyImageMenuItem* duplicate;
    MyImageMenuItem* clearFromCache;
    MyImageMenuItem* clearFromCacheFull;
    Gtk::Menu* pmenu;

    MyImageMenuItem* colorlabel_pop[6];
    Gtk::Menu* pmenuColorLabels;
    void* colorLabel_actionData;
    void menuColorlabelActivated (Gtk::MenuItem* m); // use only when menu is invoked via FileBrowser::buttonPressed to pass actionData

    Glib::RefPtr<Gtk::AccelGroup> pmaccelgroup;

    BatchPParamsChangeListener* bppcl;
    FileBrowserListener* tbl;
    BrowserFilter filter;
    bool filterPassThrough_;
    int numFiltered;

    void toTrashRequested   (std::vector<FileBrowserEntry*> tbe);
    void fromTrashRequested (std::vector<FileBrowserEntry*> tbe);
    void sortMethodRequested (int method);
    void sortOrderRequested (int order);
    void rankingRequested   (std::vector<FileBrowserEntry*> tbe, int rank);
    void colorlabelRequested   (std::vector<FileBrowserEntry*> tbe, int colorlabel);
    void pickRequested (std::vector<FileBrowserEntry*> tbe, int pick);
    void notifySelectionListener ();
    void scheduleSelectionNotify ();
    void openRequested( std::vector<FileBrowserEntry*> mselected);
    void inspectRequested( std::vector<FileBrowserEntry*> mselected);
    ExportPanel* exportPanel;

    type_trash_changed m_trash_changed;
    type_save_image_requested m_save_image_requested;
    type_external_editor_requested m_external_editor_requested;

    MyImageMenuItem* setAlbumCover;
    MyImageMenuItem* addToAlbum;
    std::function<void(const Glib::ustring&)> albumCoverSetter_;
    std::function<void(const Glib::ustring&)> addToAlbumSetter_;
    std::function<bool()> isInAlbumMode_;
    sigc::connection autoLevelPollConnection_;
    std::thread autoLevelThread_;
    std::shared_ptr<std::atomic<bool>> autoLevelCancel_;
    std::unique_ptr<Glib::ThreadPool> autoEditHoverPool_;
    std::shared_ptr<std::atomic<unsigned>> autoEditHoverGeneration_;
    sigc::connection autoEditHoverDelayConnection_;
    sigc::connection autoEditHoverTrackingConnection_;
    Gtk::MenuItem* autoEditHoverItem_ = nullptr;
    Glib::ustring autoEditHoverPreviewFile_;
    bool autoEditHoverInFlight_ = false;
    bool quickActionRunning_ = false;

    void startAutoEditHoverPreview(Gtk::MenuItem* item);
    void cancelAutoEditHoverPreview(Gtk::MenuItem* item = nullptr, bool restore = true);

public:
    FileBrowser ();
    ~FileBrowser () override;

    void addEntry (FileBrowserEntry* entry);                    ///< Moves the execution to the main UI thread
    void reserveEntries (std::size_t additionalEntries);
    bool addEntry_ (FileBrowserEntry* entry);                   ///< Must be called only when the execution is in the main UI thread
    std::size_t addEntries_ (std::vector<FileBrowserEntry*>& entries); ///< Keeps only accepted entries in the vector
    FileBrowserEntry*  delEntry (const Glib::ustring& fname);    // return the entry if found here return NULL otherwise
    std::vector<FileBrowserEntry*> delEntries (const std::set<Glib::ustring>& fnames); // bulk remove, single redraw
    void close ();

    unsigned int session_id() const { return session_id_; }

    void setBatchPParamsChangeListener (BatchPParamsChangeListener* l)
    {
        bppcl = l;
    }
    void setFileBrowserListener (FileBrowserListener* l)
    {
        tbl = l;
    }

    void menuItemActivated (Gtk::MenuItem* m);
    void applyMenuItemActivated (ProfileStoreLabel *label);
    void applyPartialMenuItemActivated (ProfileStoreLabel *label);

    void applyFilter (const BrowserFilter& filter);
    bool applyPassThroughFilterFast (const BrowserFilter& filter);
    int getNumFiltered()
    {
        return numFiltered;
    }

    void buttonPressed (LWButton* button, int actionCode, void* actionData) override;
    void redrawNeeded  (LWButton* button) override;
    bool checkFilter (ThumbBrowserEntryBase* entry) const override;
    void rightClicked () override;
    void doubleClicked (ThumbBrowserEntryBase* entry) override;
    bool keyPressed (GdkEventKey* event) override;

    void saveThumbnailHeight (int height) override;
    int  getThumbnailHeight () override;


    void enableTabMode(bool enable);
    bool isInTabMode() override
    {
        return tbl ? tbl->isInTabMode() : false;
    }

    void requestRanking (int rank);
    void requestColorLabel(int colorlabel);
    void requestPick(int pick);
    void requestDevelop();

    void openNextImage();
    void openPrevImage();
    void selectImage(const Glib::ustring& fname, bool doScroll = true);
    FileBrowserEntry* findEntry(const Glib::ustring& fname);
    Thumbnail* getSelectedThumbnail();  // returns lastClicked or first selected

    struct AdjacentEntry {
        Glib::ustring fname;
        std::string fnameRaw;
        size_t estimatedBytes;
        rtengine::eSensorType sensorType;
        rtengine::IIOSampleFormat sampleFormat;
        unsigned int frameCount;
        bool isRaw;
        bool preferredSide;
    };
    std::vector<AdjacentEntry> getAdjacentEntries(const Glib::ustring& fname, int count);
    std::vector<AdjacentEntry> getAdjacentEntriesAndRefresh(const Glib::ustring& fname, int preloadCount, int refreshCount, int quickPreviewWarmCount = 0, eRTNav preferredDirection = NAV_NONE);
    void cancelCachedQuickPreviewWarm();
    void refreshAdjacentThumbnails(const Glib::ustring& fname, int count);

    void copyProfile ();
    void pasteProfile ();
    void partPasteProfile ();
    void openEditorImage(const Glib::ustring& fname, eRTNav preloadDirectionHint = NAV_NONE);
    void openNextPreviousEditorImage(const Glib::ustring& fname, eRTNav eNextPrevious);

#ifdef _WIN32
    void openDefaultViewer (int destination);
#endif

    void thumbRearrangementNeeded () override;

    void selectionChanged () override;

    void setExportPanel (ExportPanel* expanel);
    // exportpanel interface
    void exportRequested() override;

    void storeCurrentValue() override;
    void updateProfileList() override;
    void restoreValue() override;

    type_trash_changed trash_changed();
    type_save_image_requested& save_image_requested();
    type_external_editor_requested& external_editor_requested();

    void setAlbumCoverSetter(std::function<void(const Glib::ustring&)> setter) {
        albumCoverSetter_ = std::move(setter);
    }
    void setAddToAlbumSetter(std::function<void(const Glib::ustring&)> setter) {
        addToAlbumSetter_ = std::move(setter);
    }
    void setAlbumModeChecker(std::function<bool()> checker) {
        isInAlbumMode_ = std::move(checker);
    }
};
