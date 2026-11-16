/*
 *  This file is part of RawTherapee.
 *
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

#include <future>
#include <memory>
#include <string>

#include <set>

#include <gtkmm.h>

#include "threadutils.h"
#include "thumbbrowserbase.h"
#include "widgets/basic/lwbutton.h"
#include "widgets/basic/lwbuttonset.h"

#include "rtengine/rtengine.h"
#include "rtengine/noncopyable.h"

class BatchQueueEntry;

class BatchQueueListener
{

public:
    virtual ~BatchQueueListener() = default;
    virtual void queueSizeChanged(int qsize, bool queueRunning, bool queueError, const Glib::ustring& queueErrorMessage) = 0;
    virtual bool canStartNext() = 0;
    virtual void setDestinationPreviewText(const Glib::ustring& destinationPath) = 0;
};

class FileCatalog;

class BatchQueue final :
    public ThumbBrowserBase,
    public rtengine::BatchProcessingListener,
    public LWButtonListener,
    public rtengine::NonCopyable
{
public:
    explicit BatchQueue (FileCatalog* aFileCatalog);
    ~BatchQueue () override;

    void addEntries (const std::vector<BatchQueueEntry*>& entries, bool head = false, bool save = true);
    void cancelItems (const std::vector<ThumbBrowserEntryBase*>& items);
    void headItems (const std::vector<ThumbBrowserEntryBase *>& items);
    void tailItems (const std::vector<ThumbBrowserEntryBase *>& items);
    void selectAll ();
    void openItemInEditor(ThumbBrowserEntryBase* item);
    void openLastSelectedItemInEditor();
    void updateDestinationPathPreview();

    void startProcessing ();

    bool hasJobs ()
    {
        MYREADERLOCK(l, entryRW);
        return (!fd.empty());
    }

    void setProgress(double p) override;
    void setProgressStr(const Glib::ustring& str) override;
    void setProgressState(bool inProcessing) override;
    void error(const Glib::ustring& descr) override;
    rtengine::ProcessingJob* imageReady(rtengine::IImagefloat* img) override;

    void rightClicked () override;
    void doubleClicked (ThumbBrowserEntryBase* entry) override;
    bool keyPressed (GdkEventKey* event) override;
    void buttonPressed (LWButton* button, int actionCode, void* actionData) override;
    void redrawNeeded  (LWButton* button) override;
    void selectionChanged () override;

    void setBatchQueueListener (BatchQueueListener* l)
    {
        listener = l;
    }

    bool loadBatchQueue ();
    void resizeLoadedQueue();

    static Glib::ustring calcAutoFileNameBase (const Glib::ustring& origFileName, int sequence=0, const Glib::ustring& format="");
    static int calcMaxThumbnailHeight();

private:
    // The finished image of the previous job is watermarked, encoded and
    // written on the IO lane while the next job processes. At most one
    // write is in flight: a new image waits for it first, so output names
    // (made unique against files on disk) and memory stay as before.
    std::future<void> pendingWrite_;
    std::shared_ptr<std::string> pendingWriteError_;
    void finishPendingWrite_();
    int getMaxThumbnailHeight() const override;
    void saveThumbnailHeight (int height) override;
    int  getThumbnailHeight () override;

    Glib::ustring autoCompleteFileName (const Glib::ustring& fileName, const Glib::ustring& format);
    Glib::ustring getTempFilenameForParams( const Glib::ustring &filename );
    bool saveBatchQueue ();
    void notifyListener ();

    using ThumbBrowserBase::redrawNeeded;

    BatchQueueEntry* processing;  // holds the currently processed image
    FileCatalog* fileCatalog;
    int sequence; // holds the current sequence index

    Glib::ustring nameTemplate;

    MyImageMenuItem* cancel;
    MyImageMenuItem* head;
    MyImageMenuItem* tail;
    Gtk::MenuItem* selall;
    Gtk::MenuItem* open;
    Glib::RefPtr<Gtk::AccelGroup> pmaccelgroup;
    Gtk::Menu pmenu;

    BatchQueueListener* listener;

    std::set<BatchQueueEntry*> removable_batch_queue_entries;
    MyMutex mutex_removable_batch_queue_entries;

    IdleRegister idle_register;
};
