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
#include "batchqueuepanel.h"

#include "multilangmgr.h"
#include "options.h"
#include "rtimage.h"
#include "soundman.h"
#include "windows/rtwindow.h"

static Glib::ustring makeFolderLabel(Glib::ustring path)
{
    if (!Glib::file_test (path, Glib::FILE_TEST_IS_DIR)) {
        return "(" + M("GENERAL_NONE") + ")";
    }

    if (path.size() > 40) {
        size_t last_ds = path.find_last_of (G_DIR_SEPARATOR);

        if (last_ds != Glib::ustring::npos && last_ds > 10) {
            path = "..." + path.substr(last_ds);
        }
    }

    return path;
}

BatchQueuePanel::BatchQueuePanel (FileCatalog* aFileCatalog) : parent(nullptr)
{
    const auto& options = App::get().options();

    set_orientation(Gtk::ORIENTATION_VERTICAL);
    batchQueue = Gtk::manage( new BatchQueue(aFileCatalog) );

    Gtk::Box* batchQueueButtonBox = Gtk::manage (new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
    batchQueueButtonBox->set_name("BatchQueueButtons");

    qStartStop = Gtk::manage (new Gtk::Button("Export Queue"));
    qStartStop->set_tooltip_markup (M("QUEUE_STARTSTOP_TOOLTIP"));
    qStartStop->signal_clicked().connect (sigc::mem_fun(*this, &BatchQueuePanel::startOrStopBatchProc));

    qAutoStart = Gtk::manage (new Gtk::CheckButton (M("QUEUE_AUTOSTART")));
    qAutoStart->set_tooltip_text (M("QUEUE_AUTOSTART_TOOLTIP"));
    qAutoStart->set_active (options.procQueueEnabled);

    queueShouldRun = false;
    isQueueRunning_ = false;

    batchQueueButtonBox->pack_start (*qStartStop, Gtk::PACK_SHRINK, 4);
    batchQueueButtonBox->pack_start (*qAutoStart, Gtk::PACK_SHRINK, 4);
    Gtk::Frame *bbox = Gtk::manage(new Gtk::Frame(M("MAIN_FRAME_QUEUE")));
    bbox->set_label_align(0.025, 0.5);
    bbox->add(*batchQueueButtonBox);

    // Output directory selection
    fdir = Gtk::manage (new Gtk::Frame (M("QUEUE_LOCATION_TITLE")));
    fdir->set_label_align(0.025, 0.5);
    Gtk::Box* odvb = Gtk::manage (new Gtk::Box(Gtk::ORIENTATION_VERTICAL));

    // Folder row (top option)
    Gtk::Box* hb3 = Gtk::manage (new Gtk::Box ());
    useFolder = Gtk::manage (new Gtk::RadioButton (M("QUEUE_LOCATION_FOLDER")));
    hb3->pack_start (*useFolder, Gtk::PACK_SHRINK, 4);

#if 0 //defined(__APPLE__) || defined(__linux__)
    // At the time of writing (2013-11-11) the gtkmm FileChooserButton with ACTION_SELECT_FOLDER
    // is so buggy on these platforms (OS X and Linux) that we rather employ this ugly button hack.
    // When/if GTKMM gets fixed we can go back to use the FileChooserButton, like we do on Windows.
    outdirFolderButton = Gtk::manage (new Gtk::Button("(" + M("GENERAL_NONE") + ")"));
    outdirFolderButton->set_alignment(0.0, 0.0);
    hb3->pack_start (*outdirFolderButton);
    outdirFolderButton->signal_pressed().connect( sigc::mem_fun(*this, &BatchQueuePanel::pathFolderButtonPressed) );
    outdirFolderButton->set_label(makeFolderLabel(options.savePathFolder));
    Gtk::Image* folderImg = Gtk::manage (new RTImage ("folder-closed", Gtk::ICON_SIZE_LARGE_TOOLBAR));
    folderImg->show ();
    outdirFolderButton->set_image (*folderImg);
    outdirFolder = nullptr;
#else
    outdirFolder = Gtk::manage (new MyFileChooserButton (M("QUEUE_LOCATION_FOLDER"), Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER));
    hb3->pack_start (*outdirFolder);
    outdirFolder->signal_selection_changed().connect (sigc::mem_fun(*this, &BatchQueuePanel::pathFolderChanged));

    if (Glib::file_test (options.savePathFolder, Glib::FILE_TEST_IS_DIR)) {
        outdirFolder->set_current_folder (options.savePathFolder);
    } else {
        outdirFolder->set_current_folder (Glib::get_home_dir());
    }

    outdirFolderButton = 0;
#endif

    odvb->pack_start (*hb3, Gtk::PACK_SHRINK, 4);

    // Template row (below folder)
    Gtk::Box* hb2 = Gtk::manage (new Gtk::Box ());
    useTemplate = Gtk::manage (new Gtk::RadioButton (M("QUEUE_LOCATION_TEMPLATE")));
    hb2->pack_start (*useTemplate, Gtk::PACK_SHRINK, 4);
    outdirTemplate = Gtk::manage (new Gtk::Entry ());
    hb2->pack_start (*outdirTemplate);
    templateHelpButton = Gtk::manage (new Gtk::ToggleButton("?"));
    templateHelpButton->set_tooltip_markup (M ("QUEUE_LOCATION_TEMPLATE_HELP_BUTTON_TOOLTIP"));
    hb2->pack_start (*templateHelpButton, Gtk::PACK_SHRINK, 0);
    outdirTemplate->set_tooltip_markup (M("QUEUE_LOCATION_TEMPLATE_TOOLTIP"));
    useTemplate->set_tooltip_markup (M("QUEUE_LOCATION_TEMPLATE_TOOLTIP"));
    odvb->pack_start (*hb2, Gtk::PACK_SHRINK, 0);

    destinationPreviewLabel = Gtk::manage (new Gtk::Label ());
    destinationPreviewLabel->set_tooltip_markup (M("QUEUE_DESTPREVIEW_TOOLTIP"));
    destinationPreviewLabel->set_selectable (true);  // so users can copy the path to the clipboard
    destinationPreviewLabel->set_halign (Gtk::ALIGN_START);
    auto destinationPreviewScrolledWindow = Gtk::manage(new Gtk::ScrolledWindow ());
    destinationPreviewScrolledWindow->set_policy (Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    destinationPreviewScrolledWindow->add (*destinationPreviewLabel);
    odvb->pack_start (*destinationPreviewScrolledWindow, Gtk::PACK_SHRINK);
    Gtk::RadioButton::Group g = useFolder->get_group();
    useTemplate->set_group (g);
    fdir->add (*odvb);

    // Output file format selection
    fformat = Gtk::manage (new Gtk::Frame (M("QUEUE_FORMAT_TITLE")));
    fformat->set_label_align(0.025, 0.5);
    fformat->set_size_request(280, -1);
    saveFormatPanel = Gtk::manage (new SaveFormatPanel ());
    setExpandAlignProperties(saveFormatPanel, true, false, Gtk::ALIGN_FILL, Gtk::ALIGN_START);

    // Optional cap on exported image resolution, tucked under a collapsed
    // "Options" expander so the format frame stays clean
    auto* maxSizeEnabled = Gtk::manage (new Gtk::CheckButton (M("QUEUE_MAXSIZE_ENABLED")));
    maxSizeEnabled->set_tooltip_text (M("QUEUE_MAXSIZE_ENABLED_HINT"));
    auto* maxSizeSpin = Gtk::manage (new Gtk::SpinButton ());
    maxSizeSpin->set_range (256, 20000);
    maxSizeSpin->set_increments (128, 512);
    maxSizeSpin->set_digits (0);
    maxSizeSpin->set_tooltip_text (M("QUEUE_MAXSIZE_HINT"));

    auto* maxSizeRow = Gtk::manage (new Gtk::Box (Gtk::ORIENTATION_HORIZONTAL, 4));
    auto* maxSizeLabel = Gtk::manage (new Gtk::Label (M("QUEUE_MAXSIZE_LABEL")));
    auto* maxSizePx = Gtk::manage (new Gtk::Label (M("QUEUE_MAXSIZE_PX")));
    maxSizeRow->set_margin_start (22);  // indent under the checkbox
    maxSizeRow->pack_start (*maxSizeLabel, Gtk::PACK_SHRINK);
    maxSizeRow->pack_start (*maxSizeSpin, Gtk::PACK_SHRINK);
    maxSizeRow->pack_start (*maxSizePx, Gtk::PACK_SHRINK);

    auto* formatOptionsBox = Gtk::manage (new Gtk::Box (Gtk::ORIENTATION_VERTICAL, 2));
    formatOptionsBox->set_margin_top (2);
    formatOptionsBox->pack_start (*maxSizeEnabled, Gtk::PACK_SHRINK);
    formatOptionsBox->pack_start (*maxSizeRow, Gtk::PACK_SHRINK);

    auto* formatOptionsExpander = Gtk::manage (new Gtk::Expander (M("QUEUE_FORMAT_OPTIONS")));
    formatOptionsExpander->add (*formatOptionsBox);
    {
        const auto& opts = App::get().options();
        maxSizeEnabled->set_active (opts.exportMaxSizeEnabled);
        maxSizeSpin->set_value (opts.exportMaxLongEdge);
        maxSizeRow->set_sensitive (opts.exportMaxSizeEnabled);
        // Surface the section when the cap is active so it isn't forgotten
        formatOptionsExpander->set_expanded (opts.exportMaxSizeEnabled);
    }
    maxSizeEnabled->signal_toggled().connect ([maxSizeEnabled, maxSizeRow]() {
        App::get().mut_options().exportMaxSizeEnabled = maxSizeEnabled->get_active();
        maxSizeRow->set_sensitive (maxSizeEnabled->get_active());
    });
    maxSizeSpin->signal_value_changed().connect ([maxSizeSpin]() {
        App::get().mut_options().exportMaxLongEdge = maxSizeSpin->get_value_as_int();
    });

    auto* formatVBox = Gtk::manage (new Gtk::Box (Gtk::ORIENTATION_VERTICAL, 2));
    formatVBox->pack_start (*saveFormatPanel, Gtk::PACK_SHRINK);
    formatVBox->pack_start (*formatOptionsExpander, Gtk::PACK_SHRINK, 2);
    fformat->add (*formatVBox);

    // Watermark settings
    fwatermark = Gtk::manage (new Gtk::Frame (M("WATERMARK_TITLE")));
    fwatermark->set_label_align(0.025, 0.5);
    watermarkPanel = Gtk::manage (new WatermarkPanel ());
    auto* wmScroll = Gtk::manage (new Gtk::ScrolledWindow ());
    wmScroll->set_policy (Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    wmScroll->add (*watermarkPanel);
    wmScroll->set_size_request (-1, 200);
    fwatermark->add (*wmScroll);

    outdirTemplate->set_text (options.savePathTemplate);
    useFolder->set_active (true);
    destinationPreviewLabel->set_text ("");

    // setup signal handlers
    outdirTemplate->signal_changed().connect (sigc::mem_fun(*this, &BatchQueuePanel::saveOptions));
    useTemplate->signal_toggled().connect (sigc::mem_fun(*this, &BatchQueuePanel::saveOptions));
    useFolder->signal_toggled().connect (sigc::mem_fun(*this, &BatchQueuePanel::saveOptions));
    templateHelpButton->signal_toggled().connect (sigc::mem_fun(*this, &BatchQueuePanel::templateHelpButtonToggled));
    saveFormatPanel->setListener (this);

    // setup button bar
    topBox = Gtk::manage (new Gtk::Box ());
    topBox->set_halign(Gtk::ALIGN_START);
    pack_start (*topBox, Gtk::PACK_SHRINK);
    topBox->set_name("BatchQueueButtonsMainContainer");

    // Placeholder expander for the fast-export settings; filled by
    // embedFastExportSettings() once the file panel hands them over
    fastExportExpander_ = Gtk::manage(new Gtk::Expander(M("MAIN_TAB_EXPORT")));
    fastExportExpander_->set_expanded(false);
    fastExportExpander_->set_no_show_all(true);
    fastExportExpander_->hide();
    pack_start(*fastExportExpander_, Gtk::PACK_SHRINK, 4);

    topBox->pack_start (*bbox, Gtk::PACK_SHRINK, 4);
    topBox->pack_start (*fdir, Gtk::PACK_SHRINK, 4);
    topBox->pack_start (*fformat, Gtk::PACK_SHRINK, 4);
    topBox->pack_start (*fwatermark, Gtk::PACK_SHRINK, 4);

    middleSplitPane = Gtk::manage (new Gtk::Paned(Gtk::ORIENTATION_HORIZONTAL));
    templateHelpTextView = Gtk::manage (new Gtk::TextView());
    templateHelpTextView->set_editable(false);
    templateHelpTextView->set_wrap_mode(Gtk::WRAP_WORD);
    scrolledTemplateHelpWindow = Gtk::manage(new Gtk::ScrolledWindow());
    scrolledTemplateHelpWindow->add(*templateHelpTextView);
    middleSplitPane->pack1 (*scrolledTemplateHelpWindow);
    middleSplitPane->pack2 (*batchQueue);
    scrolledTemplateHelpWindow->set_visible(false); // initially hidden, templateHelpButton shows it
    scrolledTemplateHelpWindow->set_no_show_all(true);

    // add middle browser area
    pack_start (*middleSplitPane);

    // lower box with thumbnail zoom
    bottomBox = Gtk::manage (new Gtk::Box ());
    bottomBox->set_name ("BatchQueueBottomBox");
    pack_start (*bottomBox, Gtk::PACK_SHRINK);

    // thumbnail zoom slider
    Gtk::Box* zoomBox = Gtk::manage (new Gtk::Box ());
    zoomBox->pack_start (*Gtk::manage (new Gtk::Separator(Gtk::ORIENTATION_VERTICAL)), Gtk::PACK_SHRINK, 4);

    zoomSlider = Gtk::manage (new Gtk::Scale (Gtk::ORIENTATION_HORIZONTAL));
    zoomSlider->set_range (0, options.thumbnailZoomRatios.size () - 1);
    zoomSlider->set_increments (1, 1);
    zoomSlider->set_draw_value (false);
    zoomSlider->set_size_request (150, -1);
    zoomSlider->set_tooltip_markup (M ("FILEBROWSER_THUMBSIZE"));

    // Initialize slider position to match current thumbnail height
    {
        int curHeight = std::max (std::min (options.thumbSizeQueue, 200), 10);
        int maxHeight = BatchQueue::calcMaxThumbnailHeight ();
        int bestIdx = 0;
        int bestDiff = std::abs (curHeight - (int)(options.thumbnailZoomRatios[0] * maxHeight));
        for (size_t i = 1; i < options.thumbnailZoomRatios.size (); i++) {
            int h = (int)(options.thumbnailZoomRatios[i] * maxHeight);
            int diff = std::abs (curHeight - h);
            if (diff < bestDiff) {
                bestDiff = diff;
                bestIdx = i;
            }
        }
        zoomSlider->set_value (bestIdx);
    }

    zoomSlider->signal_value_changed ().connect ([this]() {
        const auto& opts = App::get().options ();
        int idx = (int)zoomSlider->get_value ();
        if (idx < 0) idx = 0;
        if (idx >= (int)opts.thumbnailZoomRatios.size ()) idx = opts.thumbnailZoomRatios.size () - 1;
        int maxHeight = BatchQueue::calcMaxThumbnailHeight ();
        int newHeight = (int)(opts.thumbnailZoomRatios[idx] * maxHeight);
        batchQueue->setThumbnailHeight (newHeight);
    });

    zoomBox->pack_end (*zoomSlider, Gtk::PACK_SHRINK);
    bottomBox->pack_end (*zoomBox, Gtk::PACK_SHRINK);


    batchQueue->setBatchQueueListener (this);

    show_all ();

    if (batchQueue->loadBatchQueue()) {
        idle_register.add(
            [this]() -> bool
            {
                batchQueue->resizeLoadedQueue();
                return false;
            },
            G_PRIORITY_LOW
        );
    }
}

BatchQueuePanel::~BatchQueuePanel()
{
    idle_register.destroy();
}

void BatchQueuePanel::init (RTWindow *parent)
{
    this->parent = parent;

    saveFormatPanel->init (App::get().options().saveFormatBatch);
    saveFormatPanel->setCompactMode();
    watermarkPanel->init (App::get().options().watermark);
}

// it is expected to have a non null forceOrientation value on Preferences update only. In this case, qsize is ignored and computed automatically
void BatchQueuePanel::updateTab (int qsize, int forceOrientation)
{
    Gtk::Notebook *nb = dynamic_cast<Gtk::Notebook *>(this->get_parent());

    if (forceOrientation > 0) {
        qsize = batchQueue->getEntries().size();
    }

    Gtk::Grid* grid = Gtk::manage (new Gtk::Grid ());
    if ((forceOrientation == 0 && App::get().options().mainNBVertical) || (forceOrientation == 2)) {
        Gtk::Label* l;

        if(!qsize ) {
            grid->attach_next_to(*Gtk::manage (new RTImage ("gears", Gtk::ICON_SIZE_LARGE_TOOLBAR)), Gtk::POS_TOP, 1, 1);
            l = Gtk::manage (new Gtk::Label (Glib::ustring(" ") + M("MAIN_FRAME_QUEUE")) );
        } else if (isQueueRunning_) {
            grid->attach_next_to(*Gtk::manage (new RTImage ("gears-play", Gtk::ICON_SIZE_LARGE_TOOLBAR)), Gtk::POS_TOP, 1, 1);
            l = Gtk::manage (new Gtk::Label (Glib::ustring(" ") + M("MAIN_FRAME_QUEUE") + " [" + Glib::ustring::format( qsize ) + "]"));
        } else {
            grid->attach_next_to(*Gtk::manage (new RTImage ("gears-pause", Gtk::ICON_SIZE_LARGE_TOOLBAR)), Gtk::POS_TOP, 1, 1);
            l = Gtk::manage (new Gtk::Label (Glib::ustring(" ") + M("MAIN_FRAME_QUEUE") + " [" + Glib::ustring::format( qsize ) + "]" ));
        }

        l->set_angle (90);
        grid->attach_next_to(*l, Gtk::POS_TOP, 1, 1);
        grid->set_tooltip_markup (M("MAIN_FRAME_QUEUE_TOOLTIP"));
        grid->show_all ();

        if (nb) {
            nb->set_tab_label(*this, *grid);
        }
    } else {
        if (!qsize ) {
            grid->attach_next_to(*Gtk::manage (new RTImage ("gears", Gtk::ICON_SIZE_LARGE_TOOLBAR)), Gtk::POS_RIGHT, 1, 1);
            grid->attach_next_to(*Gtk::manage (new Gtk::Label (M("MAIN_FRAME_QUEUE") )), Gtk::POS_RIGHT, 1, 1);
        } else if (isQueueRunning_) {
            grid->attach_next_to(*Gtk::manage (new RTImage ("gears-play", Gtk::ICON_SIZE_LARGE_TOOLBAR)), Gtk::POS_RIGHT, 1, 1);
            grid->attach_next_to(*Gtk::manage (new Gtk::Label (M("MAIN_FRAME_QUEUE") + " [" + Glib::ustring::format( qsize ) + "]" )), Gtk::POS_RIGHT, 1, 1);
        } else {
            grid->attach_next_to(*Gtk::manage (new RTImage ("gears-pause", Gtk::ICON_SIZE_LARGE_TOOLBAR)), Gtk::POS_RIGHT, 1, 1);
            grid->attach_next_to(*Gtk::manage (new Gtk::Label (M("MAIN_FRAME_QUEUE") + " [" + Glib::ustring::format( qsize ) + "]" )), Gtk::POS_RIGHT, 1, 1);
        }

        grid->set_tooltip_markup (M("MAIN_FRAME_QUEUE_TOOLTIP"));
        grid->show_all ();

        if (nb) {
            nb->set_tab_label(*this, *grid);
        }
    }
}

void BatchQueuePanel::queueSizeChanged(int qsize, bool queueRunning, bool queueError, const Glib::ustring& queueErrorMessage)
{
    setGuiFromBatchState(queueRunning, qsize);

    if (!queueRunning && qsize == 0 && queueShouldRun) {
        // There was work, but it is all done now.
        queueShouldRun = false;

        SoundManager::playSoundAsync(App::get().options().sndBatchQueueDone);
    }

    if (queueError) {
        Gtk::MessageDialog msgd (queueErrorMessage, true, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
        msgd.run ();
    }
}

void BatchQueuePanel::startOrStopBatchProc()
{
    if (isQueueRunning_) {
        stopBatchProc();
    } else {
        startBatchProc();
    }
}

void BatchQueuePanel::startBatchProc ()
{
    if (batchQueue->hasJobs()) {
        // Update the *desired* state of the queue, then launch it.  The switch
        // state is not updated here; it is changed by the queueSizeChanged()
        // callback in response to the *reported* state.
        queueShouldRun = true;

        saveOptions();
        batchQueue->startProcessing ();
    }
}

void BatchQueuePanel::stopBatchProc ()
{
    // There is nothing much to do here except set the desired state, which the
    // background queue thread must check.  It will notify queueSizeChanged()
    // when it stops.
    queueShouldRun = false;
}

void BatchQueuePanel::setGuiFromBatchState(bool queueRunning, int qsize)
{
    isQueueRunning_ = queueRunning;

    // Change the GUI state in response to the reported queue state
    if (qsize == 0 || (qsize == 1 && queueRunning)) {
        qStartStop->set_sensitive(false);
    } else {
        qStartStop->set_sensitive(true);
    }

    fdir->set_sensitive (!queueRunning);
    fformat->set_sensitive (!queueRunning);
    fwatermark->set_sensitive (!queueRunning);

    updateTab(qsize);
}

void BatchQueuePanel::templateHelpButtonToggled()
{
    bool visible = templateHelpButton->get_active();
    auto buffer = templateHelpTextView->get_buffer();
    if (buffer->get_text().empty()) {
        // Populate the help text the first time it's shown
        populateTemplateHelpBuffer(buffer);
        const auto fullWidth = middleSplitPane->get_width();
        middleSplitPane->set_position(fullWidth / 2);
    }
    scrolledTemplateHelpWindow->set_visible(visible);
    templateHelpTextView->set_visible(visible);
}

void BatchQueuePanel::populateTemplateHelpBuffer(Glib::RefPtr<Gtk::TextBuffer> buffer)
{
    auto& options = App::get().mut_options();

    auto pos = buffer->begin();
    const auto insertTopicHeading = [&pos, buffer](const Glib::ustring& text) {
        pos = buffer->insert_markup(pos, Glib::ustring::format("\n\n<u><b>", text, "</b></u>\n"));
    };
    const auto insertTopicBody = [&pos, buffer](const Glib::ustring& text) {
        pos = buffer->insert_markup(pos, Glib::ustring::format("\n", text, "\n"));
    };
    const auto mainTitle = M("QUEUE_LOCATION_TEMPLATE_HELP_TITLE");
    pos = buffer->insert_markup(pos, Glib::ustring::format("<big><b>", mainTitle, "</b></big>\n"));
    pos = buffer->insert_markup(pos, M("QUEUE_LOCATION_TEMPLATE_HELP_INTRO"));

    insertTopicHeading(M("QUEUE_LOCATION_TEMPLATE_HELP_EXAMPLES_TITLE"));
    pos = buffer->insert_markup(pos, M("QUEUE_LOCATION_TEMPLATE_HELP_EXAMPLES_BODY"));

    insertTopicHeading(M("QUEUE_LOCATION_TEMPLATE_HELP_PATHS_TITLE"));
    pos = buffer->insert_markup(pos, M("QUEUE_LOCATION_TEMPLATE_HELP_PATHS_INTRO"));
    pos = buffer->insert(pos, "\n");
#ifdef _WIN32
    pos = buffer->insert_markup(pos, M("QUEUE_LOCATION_TEMPLATE_HELP_PATHS_INTRO_WINDOWS"));
    pos = buffer->insert(pos, "\n");
#endif
    pos = buffer->insert(pos, "\n");
    pos = buffer->insert_markup(pos, M("QUEUE_LOCATION_TEMPLATE_HELP_PATHS_BODY_1"));
#ifdef _WIN32
    const auto exampleFilePath = M("QUEUE_LOCATION_TEMPLATE_HELP_PATHS_EXAMPLE_WINDOWS");
#else
    const auto exampleFilePath = M("QUEUE_LOCATION_TEMPLATE_HELP_PATHS_EXAMPLE_LINUX");
#endif
    pos = buffer->insert_markup(pos, Glib::ustring::format("\n  ", exampleFilePath, "\n"));
    pos = buffer->insert_markup(pos, M("QUEUE_LOCATION_TEMPLATE_HELP_PATHS_BODY_2"));
    // Examples are generated from exampleFilePath using the actual template processing function
    const Options savedOptions = options; // to be restored after generating example results
    options.saveUsePathTemplate = true;
    // Since this code only ever runs once (the first time the help text is presented), no attempt is
    // made to be efficient. Use a brute-force method to discover the number of elements in exampleFilePath.
    int pathElementCount = 0;
    for (int n=9; n>=0; n--) {
        options.savePathTemplate = Glib::ustring::format("%d", n);
        const auto result = BatchQueue::calcAutoFileNameBase(exampleFilePath);
        if (!result.empty()) {
            // The 'd' specifier returns an empty string if N exceeds the number of path elements, so
            // the largest N that does not return an empty string is the number of elements in exampleFilePath.
            pathElementCount = n;
            break;
        }
    }
    // Function inserts examples for a particular specifier, with every valid N value for the
    // number of elements in the path.
    const auto insertPathExamples = [&buffer, &pos, pathElementCount, exampleFilePath](char letter, int offset1, int mult1, int offset2, int mult2)
    {
        auto& options = App::get().mut_options();
        for (int n=0; n<pathElementCount; n++) {
            auto path1 = Glib::ustring::format("%", letter, offset1+n*mult1);
            auto path2 = Glib::ustring::format("%", letter, offset2+n*mult2);
            options.savePathTemplate = path1;
            auto result1 = BatchQueue::calcAutoFileNameBase(exampleFilePath);
            options.savePathTemplate = path2;
            auto result2 = BatchQueue::calcAutoFileNameBase(exampleFilePath);
            pos = buffer->insert_markup(pos, Glib::ustring::format("\n  <tt><b>", path1, "</b> = <b>", path2, "</b> = <i>", result1, "</i></tt>"));
            if (result1 != result2) {
                // If this error appears, it indicates a coding error in either BatchQueue::calcAutoFileNameBase
                // or BatchQueuePanel::populateTemplateHelpBuffer.
                pos = buffer->insert_markup(pos, Glib::ustring::format(" ", M("QUEUE_LOCATION_TEMPLATE_HELP_RESULT_MISMATCH"), " ", result2));
            }
        }
    };
    // Example outputs in comments below are for a 4-element path.
    insertPathExamples('d', pathElementCount, -1, -1, -1);  //   <b>%d4</b> = <b>%d-1</b> = <i>home</i>
    insertPathExamples('p', 1, 1, -pathElementCount, 1);    //   <b>%p1</b> = <b>%p-4</b> = <i>/home/tom/photos/2010-10-31/</i>
    insertPathExamples('P', 1, 1, -pathElementCount, 1);    //   <b>%P1</b> = <b>%P-4</b> = <i>2010-10-31/</i>
    {
        const Glib::ustring fspecifier("%f");
        options.savePathTemplate = fspecifier;
        const auto result = BatchQueue::calcAutoFileNameBase(exampleFilePath);
        pos = buffer->insert_markup(pos, Glib::ustring::format("\n  <tt><b>", fspecifier, "</b> = <i>", result, "</i></tt>"));
    }

    insertTopicHeading(M("QUEUE_LOCATION_TEMPLATE_HELP_FORMAT_TITLE"));
    pos = buffer->insert_markup(pos, M("QUEUE_LOCATION_TEMPLATE_HELP_FORMAT_BODY"));

    insertTopicHeading(M("QUEUE_LOCATION_TEMPLATE_HELP_RANK_TITLE"));
    pos = buffer->insert_markup(pos, M("QUEUE_LOCATION_TEMPLATE_HELP_RANK_BODY"));

    insertTopicHeading(M("QUEUE_LOCATION_TEMPLATE_HELP_SEQUENCE_TITLE"));
    pos = buffer->insert_markup(pos, M("QUEUE_LOCATION_TEMPLATE_HELP_SEQUENCE_BODY"));

    insertTopicHeading(M("QUEUE_LOCATION_TEMPLATE_HELP_TIMESTAMP_TITLE"));
    pos = buffer->insert_markup(pos, M("QUEUE_LOCATION_TEMPLATE_HELP_TIMESTAMP_BODY"));
    const Glib::ustring dateTimeFormatExamples[] = {
        "%Y-%m-%d",
        "%Y%m%d_%H%M%S",
        "%y/%b/%-d/"
    };
    const auto timezone = Glib::DateTime::create_now_local().get_timezone();
    const auto timeForExamples = Glib::DateTime::create_from_iso8601("2001-02-03T04:05:06.123456", timezone);
    for (auto && fmt : dateTimeFormatExamples) {
        const auto result = timeForExamples.format(fmt);
        pos = buffer->insert_markup(pos, Glib::ustring::format("\n  <tt><b>%tE\"", fmt, "\"</b> = <i>", result, "</i></tt>"));
    }

    pos = buffer->insert(pos, "\n");
    options = savedOptions; // Do not add any lines in this function below here
}

void BatchQueuePanel::embedFastExportSettings (Gtk::Widget* widget)
{
    if (!widget || !fastExportExpander_) {
        return;
    }

    widget->set_size_request(-1, 320);
    fastExportExpander_->add(*widget);
    widget->show_all();
    fastExportExpander_->set_no_show_all(false);
    fastExportExpander_->show();
}

void BatchQueuePanel::addBatchQueueJobs(const std::vector<BatchQueueEntry*>& entries, bool head)
{
    batchQueue->addEntries(entries, head);

    if (!isQueueRunning_ && qAutoStart->get_active()) {
        // Auto-start as if the user had pressed the qStartStop switch
        startBatchProc ();
    }
}

void BatchQueuePanel::saveOptions ()
{
    auto& options = App::get().mut_options();

    options.savePathTemplate    = outdirTemplate->get_text();
    options.saveUsePathTemplate = useTemplate->get_active();
    options.procQueueEnabled    = qAutoStart->get_active();
    options.watermark           = watermarkPanel->getOptions();
    batchQueue->updateDestinationPathPreview();
}

bool BatchQueuePanel::handleShortcutKey (GdkEventKey* event)
{
    bool ctrl = event->state & GDK_CONTROL_MASK;

    if (ctrl) {
        switch(event->keyval) {
        case GDK_KEY_s:
            if (isQueueRunning_) {
                stopBatchProc();
            } else {
                startBatchProc();
            }

            return true;
        }
    }

    return batchQueue->keyPressed (event);
}

bool BatchQueuePanel::canStartNext ()
{
    // This function is called from the background BatchQueue thread.  It
    // cannot call UI functions; we keep the desired state in an atomic.
    return queueShouldRun;
}

void BatchQueuePanel::setDestinationPreviewText(const Glib::ustring &destinationPath)
{
    destinationPreviewLabel->set_text(destinationPath);
}

void BatchQueuePanel::pathFolderButtonPressed ()
{
    auto& options = App::get().mut_options();

    Gtk::FileChooserDialog fc (getToplevelWindow (this), M("QUEUE_LOCATION_FOLDER"), Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER );
    fc.add_button( "_Cancel", Gtk::RESPONSE_CANCEL); // STOCKICON WAS THERE
    fc.add_button( "_OK", Gtk::RESPONSE_OK); // STOCKICON WAS THERE
    fc.set_filename(options.savePathFolder);
    fc.set_transient_for(*parent);
    int result = fc.run();

    if (result == Gtk::RESPONSE_OK) {
        if (Glib::file_test(fc.get_current_folder(), Glib::FILE_TEST_IS_DIR)) {
            options.savePathFolder = fc.get_filename ();
            outdirFolderButton->set_label(makeFolderLabel(options.savePathFolder));
        }
    }
}

// We only want to save the following when it changes,
// since these settings are shared with editorpanel :
void BatchQueuePanel::pathFolderChanged ()
{
    auto& options = App::get().mut_options();
    options.savePathFolder = outdirFolder->get_filename();
    batchQueue->updateDestinationPathPreview();
}

void BatchQueuePanel::formatChanged(const Glib::ustring& format)
{
    auto& options = App::get().mut_options();
    options.saveFormatBatch = saveFormatPanel->getFormat();
    batchQueue->updateDestinationPathPreview();
}

void BatchQueuePanel::setOverlayMode (bool overlay)
{
    if (overlay) {
        batchQueue->setHScrollVisible (false);
    }
}
