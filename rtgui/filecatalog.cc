/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2004-2010 Gabor Horvath <hgabor@rawtherapee.com>
 *  Copyright (c) 2011 Michael Ezra <www.michaelezra.com>
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
#include "filecatalog.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iterator>
#include <iostream>
#include <iomanip>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>

#include <glib/gstdio.h>

#include "rtengine/rt_math.h"
#include "rtengine/procparams.h"

#include "batchqueue.h"
#include "batchqueueentry.h"
#include "cachemanager.h"
#include "coarsepanel.h"
#include "filepanel.h"
#include "guiutils.h"
#include "inspector.h"
#include "multilangmgr.h"
#include "options.h"
#include "pathutils.h"
#include "placesbrowser.h"
#include "rtimage.h"
#include "rtscalable.h"
#include "thumbimageupdater.h"
#include "thumbnail.h"
#include "toolbar.h"
#include "windows/renamedlg.h"

using namespace std;

namespace {

std::string foldedPathKey(const Glib::ustring& path)
{
    std::string key = path.casefold().raw();
    std::replace(key.begin(), key.end(), '\\', '/');

    return key;
}

std::string catalogPathKey(const Glib::ustring& path)
{
    if (Glib::path_is_absolute(path)) {
        return foldedPathKey(path);
    }

    const std::string rawPath = path.raw();
    Glib::ustring normalized = path;

    try {
        Glib::RefPtr<Gio::File> file;

        if (rawPath.rfind("file:", 0) == 0) {
            file = Gio::File::create_for_uri(path);
        } else {
            file = Gio::File::create_for_path(path);
        }

        const Glib::ustring nativePath = file->get_path();
        if (!nativePath.empty()) {
            normalized = nativePath;
        } else {
            const Glib::ustring parseName = file->get_parse_name();
            if (!parseName.empty()) {
                normalized = parseName;
            }
        }
    } catch (const Glib::Exception&) {
    }

    return foldedPathKey(normalized);
}

bool folderLoadBenchmarkEnabled()
{
    static const bool enabled = []() -> bool {
        const char* value = g_getenv("RT_FOLDER_LOAD_BENCH");

        return value != nullptr
            && value[0] != '\0'
            && g_strcmp0(value, "0") != 0
            && g_ascii_strcasecmp(value, "false") != 0;
    }();

    return enabled;
}

bool navigationBenchmarkEnabled()
{
    static const bool enabled = []() -> bool {
        const char* value = g_getenv("STEEP_NAV_BENCH");

        return value != nullptr
            && value[0] != '\0'
            && g_strcmp0(value, "0") != 0
            && g_ascii_strcasecmp(value, "false") != 0;
    }();

    return enabled;
}

bool navigationBenchmarkRawOnlyEnabled()
{
    static const bool enabled = []() -> bool {
        const char* value = g_getenv("STEEP_NAV_BENCH_RAW_ONLY");

        return value != nullptr
            && value[0] != '\0'
            && g_strcmp0(value, "0") != 0
            && g_ascii_strcasecmp(value, "false") != 0;
    }();

    return enabled;
}

int navigationBenchmarkEnvInt(const char* name, int fallback, int minimum, int maximum)
{
    const char* value = g_getenv(name);
    if (!value || value[0] == '\0') {
        return fallback;
    }

    char* end = nullptr;
    const gint64 parsed = g_ascii_strtoll(value, &end, 10);
    if (end == value) {
        return fallback;
    }

    return static_cast<int>(std::max<gint64>(
        minimum,
        std::min<gint64>(parsed, maximum)));
}

bool isRawNavigationBenchmarkPath(const Glib::ustring& path)
{
    const Glib::ustring basename = Glib::path_get_basename(path);
    const auto lastdot = basename.find_last_of('.');
    if (lastdot >= basename.length() - 1) {
        return false;
    }

    static const std::set<Glib::ustring> rawExtensions = {
        "3fr", "arw", "arq", "cr2", "cr3", "crf", "crw", "dcr", "dng",
        "fff", "iiq", "kdc", "mef", "mos", "mrw", "nef", "nrw", "orf",
        "ori", "pef", "raf", "raw", "rw2", "rwl", "rwz", "sr2", "srf",
        "srw", "x3f"
    };

    return rawExtensions.count(basename.substr(lastdot + 1).lowercase()) > 0;
}

bool isEnabledImageName(const Glib::ustring& name)
{
    const auto lastdot = name.find_last_of('.');

    if (lastdot >= name.length() - 1) {
        return false;
    }

    const auto& extensions = App::get().options().parsedExtensionsSet;
    return extensions.find(name.substr(lastdot + 1).lowercase()) != extensions.end();
}

bool isKnownEmptyFileInfo(const Glib::RefPtr<Gio::FileInfo>& info)
{
    return info
        && info->has_attribute(G_FILE_ATTRIBUTE_STANDARD_SIZE)
        && info->get_size() <= 0;
}

bool isEnabledImagePath(const Glib::ustring& path)
{
    if (!isEnabledImageName(Glib::path_get_basename(path))) {
        return false;
    }

    try {
        const auto file = Gio::File::create_for_path(path);
        if (!file) {
            return false;
        }

        const auto info = file->query_info(
            std::string(G_FILE_ATTRIBUTE_STANDARD_TYPE) + "," +
            G_FILE_ATTRIBUTE_STANDARD_SIZE);
        return info
            && info->get_file_type() != Gio::FILE_TYPE_DIRECTORY
            && !isKnownEmptyFileInfo(info);
    } catch (const Glib::Exception&) {
        return false;
    }
}

bool isXmpSidecarPath(const Glib::ustring& path)
{
    const Glib::ustring basename = Glib::path_get_basename(path);
    const auto lastdot = basename.find_last_of('.');

    return lastdot < basename.length() - 1
        && basename.substr(lastdot + 1).lowercase() == "xmp";
}

bool isProcParamSidecarPath(const Glib::ustring& path)
{
    const Glib::ustring basename = Glib::path_get_basename(path);
    const auto lastdot = basename.find_last_of('.');

    return lastdot < basename.length() - 1
        && basename.substr(lastdot + 1).lowercase() == "pp3";
}

void getFilesRecursively(
    const Glib::ustring &dir_path,
    int max_depth,
    int &dir_quota,
    std::vector<Glib::ustring> &file_names,
    std::vector<Glib::RefPtr<Gio::File>> *directories_explored)
{
    const auto& options = App::get().options();
    try {
        const auto dir = Gio::File::create_for_path(dir_path);

        static const auto enumerate_attrs =
            std::string(G_FILE_ATTRIBUTE_STANDARD_NAME) + "," +
            G_FILE_ATTRIBUTE_STANDARD_TYPE + "," +
            G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN + "," +
            G_FILE_ATTRIBUTE_STANDARD_SIZE;
        auto enumerator = dir->enumerate_children(
            enumerate_attrs,
            options.browseRecursiveFollowLinks
                ? Gio::FileQueryInfoFlags::FILE_QUERY_INFO_NONE
                : Gio::FileQueryInfoFlags::FILE_QUERY_INFO_NOFOLLOW_SYMLINKS);

        if (directories_explored) {
            directories_explored->push_back(dir);
        }

        while (true) {
            try {
                const auto file = enumerator->next_file();
                if (!file) {
                    break;
                }

                if (!options.fbShowHidden && file->is_hidden()) {
                    continue;
                }

                if (file->get_file_type() == Gio::FILE_TYPE_DIRECTORY) {
                    if (max_depth > 0 && dir_quota > 0) {
                        const Glib::ustring child_dir_path = Glib::build_filename(dir_path, file->get_name());
                        getFilesRecursively(child_dir_path, max_depth - 1, --dir_quota, file_names, directories_explored);
                    }
                    continue;
                }

                const Glib::ustring fname = file->get_name();
                if (!isEnabledImageName(fname) || isKnownEmptyFileInfo(file)) {
                    continue;
                }

                file_names.emplace_back(Glib::build_filename(dir_path, fname));
            } catch (const Glib::Exception& exception) {
                if (rtengine::settings->verbose) {
                    std::cerr << exception.what() << std::endl;
                }
            }
        }

    } catch (const Glib::Exception& exception) {
        if (rtengine::settings->verbose) {
            std::cerr << "Failed to list directory \"" << dir_path << "\": " << exception.what() << std::endl;
        }
    }
}


// Streaming variant: calls onFile for each matching file.
// Returns false if cancelled (onFile returned false).
template <typename OnFile>
bool getFilesRecursivelyStreaming(
    const Glib::ustring &dir_path,
    int max_depth,
    int &dir_quota,
    OnFile&& onFile,
    std::vector<Glib::RefPtr<Gio::File>> *directories_explored)
{
    const auto& options = App::get().options();
    try {
        const auto dir = Gio::File::create_for_path(dir_path);

        static const auto enumerate_attrs =
            std::string(G_FILE_ATTRIBUTE_STANDARD_NAME) + "," +
            G_FILE_ATTRIBUTE_STANDARD_TYPE + "," +
            G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN + "," +
            G_FILE_ATTRIBUTE_STANDARD_SIZE;
        auto enumerator = dir->enumerate_children(
            enumerate_attrs,
            options.browseRecursiveFollowLinks
                ? Gio::FileQueryInfoFlags::FILE_QUERY_INFO_NONE
                : Gio::FileQueryInfoFlags::FILE_QUERY_INFO_NOFOLLOW_SYMLINKS);

        if (directories_explored) {
            directories_explored->push_back(dir);
        }

        std::vector<Glib::ustring> childDirs;

        while (true) {
            try {
                const auto file = enumerator->next_file();
                if (!file) {
                    break;
                }

                if (!options.fbShowHidden && file->is_hidden()) {
                    continue;
                }

                if (file->get_file_type() == Gio::FILE_TYPE_DIRECTORY) {
                    if (max_depth > 0 && dir_quota > 0) {
                        --dir_quota;
                        childDirs.emplace_back(Glib::build_filename(dir_path, file->get_name()));
                    }
                    continue;
                }

                const Glib::ustring fname = file->get_name();
                if (!isEnabledImageName(fname) || isKnownEmptyFileInfo(file)) {
                    continue;
                }

                if (!onFile(Glib::build_filename(dir_path, fname))) {
                    return false; // cancelled
                }
            } catch (const Glib::Exception& exception) {
                if (rtengine::settings->verbose) {
                    std::cerr << exception.what() << std::endl;
                }
            }
        }

        for (const auto& child_dir_path : childDirs) {
            if (!getFilesRecursivelyStreaming(child_dir_path, max_depth - 1, dir_quota, onFile, directories_explored)) {
                return false;
            }
        }

    } catch (const Glib::Exception& exception) {
        if (rtengine::settings->verbose) {
            std::cerr << "Failed to list directory \"" << dir_path << "\": " << exception.what() << std::endl;
        }
    }
    return true;
}

} // namespace

FileCatalog::FileCatalog (CoarsePanel* cp, ToolBar* tb, FilePanel* filepanel) :
    filepanel(filepanel),
    selectedDirectoryId(1),
    readyQuickPreviewWarmDirectoryId_(0),
    actionNextPrevious(NAV_NONE),
    listener(nullptr),
    fslistener(nullptr),
    iatlistener(nullptr),
    progressImage(nullptr),
    progressLabel(nullptr),
    hasValidCurrentEFS(false),
    filterPanel(nullptr),
    exportPanel(nullptr),
    previewsToLoad(0),
    previewsLoaded(0),
    modifierKey(0),
    coarsePanel(cp),
    toolBar(tb)
{

    set_orientation(Gtk::ORIENTATION_VERTICAL);

    inTabMode = false;
    inAlbumMode_ = false;

    for (const auto& extension : App::get().options().defaultFiletypeFilter) {
        std::string normalized = extension.raw();
        std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char value) {
            return static_cast<char>(std::toupper(value));
        });
        if (!normalized.empty()) {
            selectedFiletypes_.insert(normalized);
        }
    }

    set_name ("FileBrowser");

    //  construct and initialize thumbnail browsers
    fileBrowser = Gtk::manage( new FileBrowser() );
    fileBrowser->setFileBrowserListener (this);
    fileBrowser->setArrangement (ThumbBrowserBase::TB_Vertical);
    fileBrowser->show ();

    set_size_request(0, 250);
    // construct trash panel with the extra "empty trash" button
    trashButtonBox = Gtk::manage( new Gtk::Box(Gtk::ORIENTATION_VERTICAL) );
    Gtk::Button* emptyT = Gtk::manage( new Gtk::Button ());
    emptyT->set_tooltip_markup (M("FILEBROWSER_EMPTYTRASHHINT"));
    emptyT->set_image (*Gtk::manage(new RTImage ("trash-delete", Gtk::ICON_SIZE_LARGE_TOOLBAR)));
    emptyT->signal_pressed().connect (sigc::mem_fun(*this, &FileCatalog::emptyTrash));
    trashButtonBox->pack_start (*emptyT, Gtk::PACK_SHRINK, 4);
    emptyT->show ();
    trashButtonBox->show ();

    //initialize hbToolBar1 — widgets created here, packed into buttonBar below
    hbToolBar1 = Gtk::manage(new Gtk::Box ());
    hbToolBar1->set_spacing(4);
    hbToolBar1->set_margin_start(2);

    //setup BrowsePath
    iRefreshWhite = new RTImage("refresh-modern", Gtk::ICON_SIZE_BUTTON);
    iRefreshRed = new RTImage("refresh-modern", Gtk::ICON_SIZE_BUTTON);

    BrowsePath = Gtk::manage(new Gtk::Entry ());
    BrowsePath->set_width_chars (25);
    BrowsePath->set_max_width_chars (30);
    BrowsePath->set_hexpand (false);
    BrowsePath->set_tooltip_markup (M("FILEBROWSER_BROWSEPATHHINT"));
    Gtk::Box* hbBrowsePath = Gtk::manage(new Gtk::Box ());
    hbBrowsePath->set_valign(Gtk::ALIGN_CENTER);
    buttonBrowsePath = Gtk::manage(new Gtk::Button ());
    buttonBrowsePath->set_image (*iRefreshWhite);
    buttonBrowsePath->set_tooltip_markup (M("FILEBROWSER_BROWSEPATHBUTTONHINT"));
    buttonBrowsePath->set_relief (Gtk::RELIEF_NONE);
    buttonBrowsePath->signal_clicked().connect( sigc::mem_fun(*this, &FileCatalog::buttonBrowsePathPressed) );
    hbBrowsePath->pack_start (*BrowsePath, Gtk::PACK_SHRINK, 0);
    hbBrowsePath->pack_start (*buttonBrowsePath, Gtk::PACK_SHRINK, 0);

    // Path + search + reload live behind a magnifier toggle; the cluster
    // slides out only when needed.
    searchToggle_ = Gtk::manage(new Gtk::ToggleButton());
    searchToggle_->set_image(*Gtk::manage(new RTImage("search-toolbar", Gtk::ICON_SIZE_BUTTON)));
    searchToggle_->set_relief(Gtk::RELIEF_NONE);
    searchToggle_->set_tooltip_markup(M("FILEBROWSER_SEARCHTOGGLEHINT"));
    hbToolBar1->pack_start(*searchToggle_, Gtk::PACK_SHRINK, 0);

    searchRevealer_ = Gtk::manage(new Gtk::Revealer());
    searchRevealer_->set_transition_type(Gtk::REVEALER_TRANSITION_TYPE_SLIDE_RIGHT);
    searchRevealer_->set_transition_duration(180);
    searchRevealer_->set_reveal_child(false);
    Gtk::Box* searchCluster = Gtk::manage(new Gtk::Box());
    searchCluster->pack_start(*hbBrowsePath, Gtk::PACK_SHRINK, 0);
    searchRevealer_->add(*searchCluster);
    hbToolBar1->pack_start(*searchRevealer_, Gtk::PACK_SHRINK, 0);

    searchToggle_->signal_toggled().connect([this]() {
        const bool show = searchToggle_->get_active();
        searchRevealer_->set_reveal_child(show);
        if (show && Query) {
            Query->grab_focus();
        }
    });

    BrowsePath->signal_activate().connect (sigc::mem_fun(*this, &FileCatalog::buttonBrowsePathPressed)); //respond to the Enter key
    BrowsePath->signal_key_press_event().connect(sigc::mem_fun(*this, &FileCatalog::BrowsePath_key_pressed));

    //setup Query
    iQueryClear = new RTImage("cancel-small", Gtk::ICON_SIZE_BUTTON);
    Query = Gtk::manage(new Gtk::Entry ()); // cannot use Gtk::manage here as FileCatalog::getFilter will fail on Query->get_text()
    Query->set_text("");
    Query->set_placeholder_text("Search");
    Query->set_width_chars (15);
    Query->set_max_width_chars (20);
    Query->set_tooltip_markup (M("FILEBROWSER_QUERYHINT"));

    Gtk::Box* hbQuery = Gtk::manage(new Gtk::Box ());
    hbQuery->set_valign(Gtk::ALIGN_CENTER);
    buttonQueryClear = Gtk::manage(new Gtk::Button ());
    buttonQueryClear->set_image (*iQueryClear);
    buttonQueryClear->set_tooltip_markup (M("FILEBROWSER_QUERYBUTTONHINT"));
    buttonQueryClear->set_relief (Gtk::RELIEF_NONE);
    buttonQueryClear->signal_clicked().connect( sigc::mem_fun(*this, &FileCatalog::buttonQueryClearPressed) );
    hbQuery->pack_start (*Query, Gtk::PACK_SHRINK, 0);
    hbQuery->pack_start (*buttonQueryClear, Gtk::PACK_SHRINK, 0);
    // Inside the magnifier-revealed cluster, next to the path entry
    static_cast<Gtk::Box*>(searchRevealer_->get_child())->pack_start (*hbQuery, Gtk::PACK_SHRINK, 0);

    // Hide query clear button by default; show when search text is non-empty
    buttonQueryClear->set_no_show_all(true);
    buttonQueryClear->hide();
    Query->signal_changed().connect([this]() {
        if (Query->get_text().empty()) {
            buttonQueryClear->hide();
        } else {
            buttonQueryClear->show();
        }
    });

    Query->signal_activate().connect (sigc::mem_fun(*this, &FileCatalog::executeQuery)); //respond to the Enter key
    Query->signal_key_press_event().connect(sigc::mem_fun(*this, &FileCatalog::Query_key_pressed));

    const auto& options = App::get().options();

    // setup button bar
    buttonBar = Gtk::manage( new Gtk::Box () );
    buttonBar->set_name ("ToolBarPanelFileBrowser");
    stb_ = Gtk::manage(new MyScrolledToolbar());
    stb_->set_name("FileBrowserIconToolbar");
    stb_->add(*buttonBar);
    pack_start (*stb_, Gtk::PACK_SHRINK);

    tbLeftPanel_1 = new Gtk::ToggleButton ();
    iLeftPanel_1_Show = new RTImage("panel-to-right", Gtk::ICON_SIZE_LARGE_TOOLBAR);
    iLeftPanel_1_Hide = new RTImage("panel-to-left", Gtk::ICON_SIZE_LARGE_TOOLBAR);

    tbLeftPanel_1->set_relief(Gtk::RELIEF_NONE);
    tbLeftPanel_1->set_active (true);
    tbLeftPanel_1->set_tooltip_markup (M("MAIN_TOOLTIP_SHOWHIDELP1"));
    tbLeftPanel_1->set_image (*iLeftPanel_1_Hide);
    tbLeftPanel_1->signal_toggled().connect( sigc::mem_fun(*this, &FileCatalog::tbLeftPanel_1_toggled) );

    vSepiLeftPanel = new Gtk::Separator(Gtk::ORIENTATION_VERTICAL);

    // Filter toggle button (shows/hides the filter bar revealer)
    bFilterToggle_ = Gtk::manage(new Gtk::ToggleButton());
    bFilterToggle_->set_image(*Gtk::manage(new RTImage("filter-modern", Gtk::ICON_SIZE_BUTTON)));
    bFilterToggle_->set_relief(Gtk::RELIEF_NONE);
    bFilterToggle_->set_tooltip_markup(M("FILEBROWSER_SHOWDIRHINT"));
    bFilterToggle_->signal_toggled().connect(sigc::mem_fun(*this, &FileCatalog::filterToggled));
    buttonBar->pack_start(*bFilterToggle_, Gtk::PACK_SHRINK);

    // bFilterClear still exists for categoryButtons logic but is not on main bar
    iFilterClear = new RTImage ("filter-clear", Gtk::ICON_SIZE_LARGE_TOOLBAR);
    igFilterClear = new RTImage ("filter", Gtk::ICON_SIZE_LARGE_TOOLBAR);
    bFilterClear = Gtk::manage(new Gtk::ToggleButton ());
    bFilterClear->set_active (true);
    bFilterClear->set_image(*iFilterClear);
    bFilterClear->set_relief (Gtk::RELIEF_NONE);
    bFilterClear->set_tooltip_markup (M("FILEBROWSER_SHOWDIRHINT"));
    bFilterClear->signal_button_press_event().connect (sigc::mem_fun(*this, &FileCatalog::capture_event), false);
    bCateg[0] = bFilterClear->signal_toggled().connect (sigc::bind(sigc::mem_fun(*this, &FileCatalog::categoryButtonToggled), bFilterClear, true));

    // Create the filter bar (will go inside a Revealer)
    Gtk::Box* filterBar = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL));
    filterBar->set_name("BrowserFilterBar");
    filterBar->pack_start(*bFilterClear, Gtk::PACK_SHRINK);
    filterBar->pack_start(*Gtk::manage(new Gtk::Separator(Gtk::ORIENTATION_VERTICAL)), Gtk::PACK_SHRINK);

    fltrVbox1 = Gtk::manage (new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
    fltrRankbox = Gtk::manage (new Gtk::Box());
    fltrRankbox->get_style_context()->add_class("smallbuttonbox");
    fltrLabelbox = Gtk::manage (new Gtk::Box());
    fltrLabelbox->get_style_context()->add_class("smallbuttonbox");

    iUnRanked = new RTImage ("star-gold-hollow-small", Gtk::ICON_SIZE_BUTTON);
    igUnRanked = new RTImage ("star-hollow-small", Gtk::ICON_SIZE_BUTTON);
    bUnRanked = Gtk::manage( new Gtk::ToggleButton () );
    bUnRanked->get_style_context()->add_class("smallbutton");
    bUnRanked->set_active (false);
    bUnRanked->set_image (*igUnRanked);
    bUnRanked->set_relief (Gtk::RELIEF_NONE);
    bUnRanked->set_tooltip_markup (M("FILEBROWSER_SHOWUNRANKHINT"));
    bCateg[1] = bUnRanked->signal_toggled().connect (sigc::bind(sigc::mem_fun(*this, &FileCatalog::categoryButtonToggled), bUnRanked, true));
    fltrRankbox->pack_start (*bUnRanked, Gtk::PACK_SHRINK);
    bUnRanked->signal_button_press_event().connect (sigc::mem_fun(*this, &FileCatalog::capture_event), false);

    for (int i = 0; i < 5; i++) {
        iranked[i] = new RTImage ("star-gold-small", Gtk::ICON_SIZE_BUTTON);
        igranked[i] = new RTImage ("star-small", Gtk::ICON_SIZE_BUTTON);
        iranked[i]->show ();
        igranked[i]->show ();
        bRank[i] = Gtk::manage( new Gtk::ToggleButton () );
        bRank[i]->get_style_context()->add_class("smallbutton");
        bRank[i]->set_image (*igranked[i]);
        bRank[i]->set_relief (Gtk::RELIEF_NONE);
        fltrRankbox->pack_start (*bRank[i], Gtk::PACK_SHRINK);
        bCateg[i + 2] = bRank[i]->signal_toggled().connect (sigc::bind(sigc::mem_fun(*this, &FileCatalog::categoryButtonToggled), bRank[i], true));
        bRank[i]->signal_button_press_event().connect (sigc::mem_fun(*this, &FileCatalog::capture_event), false);
    }

    // Toolbar
    // Similar image arrays in filebrowser.cc
    std::array<std::string, 6> clabelActiveIcons = {"circle-gray-small", "circle-red-small", "circle-yellow-small", "circle-green-small", "circle-blue-small", "circle-purple-small"};
    std::array<std::string, 6> clabelInactiveIcons = {"circle-empty-gray-small", "circle-empty-red-small", "circle-empty-yellow-small", "circle-empty-green-small", "circle-empty-blue-small", "circle-empty-purple-small"};

    iUnCLabeled = new RTImage(clabelActiveIcons[0], Gtk::ICON_SIZE_BUTTON);
    igUnCLabeled = new RTImage(clabelInactiveIcons[0], Gtk::ICON_SIZE_BUTTON);
    bUnCLabeled = Gtk::manage(new Gtk::ToggleButton());
    bUnCLabeled->get_style_context()->add_class("smallbutton");
    bUnCLabeled->set_active(false);
    bUnCLabeled->set_image(*igUnCLabeled);
    bUnCLabeled->set_relief(Gtk::RELIEF_NONE);
    bUnCLabeled->set_tooltip_markup(M("FILEBROWSER_SHOWUNCOLORHINT"));
    bCateg[7] = bUnCLabeled->signal_toggled().connect (sigc::bind(sigc::mem_fun(*this, &FileCatalog::categoryButtonToggled), bUnCLabeled, true));
    fltrLabelbox->pack_start(*bUnCLabeled, Gtk::PACK_SHRINK);
    bUnCLabeled->signal_button_press_event().connect (sigc::mem_fun(*this, &FileCatalog::capture_event), false);

    for (int i = 0; i < 5; i++) {
        iCLabeled[i] = new RTImage(clabelActiveIcons[i+1], Gtk::ICON_SIZE_BUTTON);
        igCLabeled[i] = new RTImage(clabelInactiveIcons[i+1], Gtk::ICON_SIZE_BUTTON);
        iCLabeled[i]->show();
        igCLabeled[i]->show();
        bCLabel[i] = Gtk::manage(new Gtk::ToggleButton());
        bCLabel[i]->get_style_context()->add_class("smallbutton");
        bCLabel[i]->set_image(*igCLabeled[i]);
        bCLabel[i]->set_relief(Gtk::RELIEF_NONE);
        fltrLabelbox->pack_start(*bCLabel[i], Gtk::PACK_SHRINK);
        bCateg[i + 8] = bCLabel[i]->signal_toggled().connect (sigc::bind(sigc::mem_fun(*this, &FileCatalog::categoryButtonToggled), bCLabel[i], true));
        bCLabel[i]->signal_button_press_event().connect (sigc::mem_fun(*this, &FileCatalog::capture_event), false);
    }

    // Create compact multi-color summary indicator (5 tiny colored dots)
    colorLabelSummary_ = Gtk::manage(new Gtk::DrawingArea());
    colorLabelSummary_->set_size_request(RTScalable::scalePixelSize(36), RTScalable::scalePixelSize(16));
    colorLabelSummary_->signal_draw().connect([this](const Cairo::RefPtr<Cairo::Context>& cr) -> bool {
        const double colors[][3] = {{1,0,0}, {1,1,0}, {0,0.75,0}, {0,0.4,1}, {0.6,0,0.8}};
        const int w = colorLabelSummary_->get_allocated_width();
        const int h = colorLabelSummary_->get_allocated_height();
        const double r = std::min(w / 12.0, h / 2.0 - 1.0);
        const double spacing = (w - 2.0) / 5.0;
        const double alpha = colorSummaryOpacity_;
        for (int i = 0; i < 5; i++) {
            cr->set_source_rgba(colors[i][0], colors[i][1], colors[i][2], alpha);
            cr->arc(1.0 + spacing * (i + 0.5), h / 2.0, r, 0, 2 * M_PI);
            cr->fill();
        }
        return true;
    });
    colorSummaryOpacity_ = 1.0;
    colorLabelExpanded_ = false;

    // Connect enter/leave on the summary DrawingArea (has its own GdkWindow)
    colorLabelSummary_->add_events(Gdk::ENTER_NOTIFY_MASK | Gdk::LEAVE_NOTIFY_MASK);
    colorLabelSummary_->signal_enter_notify_event().connect(
        sigc::mem_fun(*this, &FileCatalog::onColorLabelChildEnter));
    colorLabelSummary_->signal_leave_notify_event().connect(
        sigc::mem_fun(*this, &FileCatalog::onColorLabelChildLeave));

    // Connect enter/leave on each color label button (each has its own GdkWindow)
    bUnCLabeled->signal_enter_notify_event().connect(
        sigc::mem_fun(*this, &FileCatalog::onColorLabelChildEnter));
    bUnCLabeled->signal_leave_notify_event().connect(
        sigc::mem_fun(*this, &FileCatalog::onColorLabelChildLeave));
    for (int i = 0; i < 5; i++) {
        bCLabel[i]->signal_enter_notify_event().connect(
            sigc::mem_fun(*this, &FileCatalog::onColorLabelChildEnter));
        bCLabel[i]->signal_leave_notify_event().connect(
            sigc::mem_fun(*this, &FileCatalog::onColorLabelChildLeave));
    }

    // Put individual color label buttons inside a Revealer for animated expand
    colorLabelRevealer_ = Gtk::manage(new Gtk::Revealer());
    colorLabelRevealer_->set_transition_type(Gtk::REVEALER_TRANSITION_TYPE_SLIDE_RIGHT);
    colorLabelRevealer_->set_transition_duration(250);
    colorLabelRevealer_->add(*fltrLabelbox);
    colorLabelRevealer_->set_reveal_child(false);

    // Container with summary + revealer (no EventBox needed)
    colorLabelContainer_ = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 0));
    colorLabelContainer_->pack_start(*colorLabelSummary_, Gtk::PACK_SHRINK);
    colorLabelContainer_->pack_start(*colorLabelRevealer_, Gtk::PACK_SHRINK);

    fltrVbox1->pack_start (*fltrRankbox, Gtk::PACK_SHRINK, 0);
    fltrVbox1->pack_start (*colorLabelContainer_, Gtk::PACK_SHRINK, 0);
    filterBar->pack_start (*fltrVbox1, Gtk::PACK_SHRINK);

    bRank[0]->set_tooltip_markup (M("FILEBROWSER_SHOWRANK1HINT"));
    bRank[1]->set_tooltip_markup (M("FILEBROWSER_SHOWRANK2HINT"));
    bRank[2]->set_tooltip_markup (M("FILEBROWSER_SHOWRANK3HINT"));
    bRank[3]->set_tooltip_markup (M("FILEBROWSER_SHOWRANK4HINT"));
    bRank[4]->set_tooltip_markup (M("FILEBROWSER_SHOWRANK5HINT"));

    bCLabel[0]->set_tooltip_markup (M("FILEBROWSER_SHOWCOLORLABEL1HINT"));
    bCLabel[1]->set_tooltip_markup (M("FILEBROWSER_SHOWCOLORLABEL2HINT"));
    bCLabel[2]->set_tooltip_markup (M("FILEBROWSER_SHOWCOLORLABEL3HINT"));
    bCLabel[3]->set_tooltip_markup (M("FILEBROWSER_SHOWCOLORLABEL4HINT"));
    bCLabel[4]->set_tooltip_markup (M("FILEBROWSER_SHOWCOLORLABEL5HINT"));

    filterBar->pack_start (*Gtk::manage(new Gtk::Separator(Gtk::ORIENTATION_VERTICAL)), Gtk::PACK_SHRINK);

    // Pick/Reject/Unflag filter buttons
    {
        Gtk::Box* fltrPickbox = Gtk::manage(new Gtk::Box());
        fltrPickbox->get_style_context()->add_class("smallbuttonbox");

        iPicked = new RTImage("flag-pick", Gtk::ICON_SIZE_BUTTON);
        igPicked = new RTImage("flag-pick", Gtk::ICON_SIZE_BUTTON);
        bPicked = Gtk::manage(new Gtk::ToggleButton());
        bPicked->get_style_context()->add_class("smallbutton");
        bPicked->set_active(false);
        bPicked->set_image(*igPicked);
        bPicked->set_relief(Gtk::RELIEF_NONE);
        bPicked->set_tooltip_markup(M("FILEBROWSER_SHOWPICKEDHINT"));
        bCateg[20] = bPicked->signal_toggled().connect(sigc::bind(sigc::mem_fun(*this, &FileCatalog::categoryButtonToggled), bPicked, true));
        fltrPickbox->pack_start(*bPicked, Gtk::PACK_SHRINK);
        bPicked->signal_button_press_event().connect(sigc::mem_fun(*this, &FileCatalog::capture_event), false);

        iUnflagged = new RTImage("flag-unflagged", Gtk::ICON_SIZE_BUTTON);
        igUnflagged = new RTImage("flag-unflagged", Gtk::ICON_SIZE_BUTTON);
        bUnflagged = Gtk::manage(new Gtk::ToggleButton());
        bUnflagged->get_style_context()->add_class("smallbutton");
        bUnflagged->set_active(false);
        bUnflagged->set_image(*igUnflagged);
        bUnflagged->set_relief(Gtk::RELIEF_NONE);
        bUnflagged->set_tooltip_markup(M("FILEBROWSER_SHOWUNFLAGGEDHINT"));
        bCateg[21] = bUnflagged->signal_toggled().connect(sigc::bind(sigc::mem_fun(*this, &FileCatalog::categoryButtonToggled), bUnflagged, true));
        fltrPickbox->pack_start(*bUnflagged, Gtk::PACK_SHRINK);
        bUnflagged->signal_button_press_event().connect(sigc::mem_fun(*this, &FileCatalog::capture_event), false);

        iRejected = new RTImage("flag-reject", Gtk::ICON_SIZE_BUTTON);
        igRejected = new RTImage("flag-reject", Gtk::ICON_SIZE_BUTTON);
        bRejected = Gtk::manage(new Gtk::ToggleButton());
        bRejected->get_style_context()->add_class("smallbutton");
        bRejected->set_active(false);
        bRejected->set_image(*igRejected);
        bRejected->set_relief(Gtk::RELIEF_NONE);
        bRejected->set_tooltip_markup(M("FILEBROWSER_SHOWREJECTEDHINT"));
        bCateg[22] = bRejected->signal_toggled().connect(sigc::bind(sigc::mem_fun(*this, &FileCatalog::categoryButtonToggled), bRejected, true));
        fltrPickbox->pack_start(*bRejected, Gtk::PACK_SHRINK);
        // Right-click opens hide-rejects and auto-cull options; other
        // presses go to the normal modifier-capture path.
        bRejected->signal_button_press_event().connect(
            [this](GdkEventButton* event) -> bool {
                if (event && event->button == 3) {
                    showRejectsPopover();
                    return true;
                }
                return capture_event(event);
            },
            false);

        // Rejects view: shows only rejected photos and offers mass delete
        bRejectsView = Gtk::manage(new Gtk::ToggleButton());
        bRejectsView->get_style_context()->add_class("smallbutton");
        bRejectsView->set_active(false);
        bRejectsView->set_image(*Gtk::manage(new RTImage("trash-small", Gtk::ICON_SIZE_BUTTON)));
        bRejectsView->set_relief(Gtk::RELIEF_NONE);
        bRejectsView->set_tooltip_markup(M("FILEBROWSER_SHOWREJECTSVIEWHINT"));
        bRejectsView->signal_toggled().connect(sigc::mem_fun(*this, &FileCatalog::rejectsViewToggled));
        fltrPickbox->pack_start(*bRejectsView, Gtk::PACK_SHRINK);

        filterBar->pack_start(*fltrPickbox, Gtk::PACK_SHRINK);

        // "Delete all rejects" action box, shown while the rejects view is active
        rejectsButtonBox = Gtk::manage(new Gtk::Box());
        Gtk::Button* deleteRejects = Gtk::manage(new Gtk::Button(M("FILEBROWSER_DELETEREJECTS")));
        deleteRejects->set_tooltip_markup(M("FILEBROWSER_DELETEREJECTSHINT"));
        deleteRejects->signal_clicked().connect(sigc::mem_fun(*this, &FileCatalog::deleteAllRejects));
        rejectsButtonBox->pack_start(*deleteRejects, Gtk::PACK_SHRINK);
        rejectsButtonBox->show_all();
    }

    filterBar->pack_start (*Gtk::manage(new Gtk::Separator(Gtk::ORIENTATION_VERTICAL)), Gtk::PACK_SHRINK);

    fltrVbox2 = Gtk::manage (new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
    fltrEditedBox = Gtk::manage (new Gtk::Box());
    fltrEditedBox->get_style_context()->add_class("smallbuttonbox");
    fltrRecentlySavedBox = Gtk::manage (new Gtk::Box());
    fltrRecentlySavedBox->get_style_context()->add_class("smallbuttonbox");

    // bEdited
    // TODO The "g" variant was the more transparent variant of the icon, used
    // when the button was not toggled. Simplify this, change to ordinary
    // togglebutton, use CSS for opacity change.
    iEdited[0] = new RTImage ("tick-hollow-small", Gtk::ICON_SIZE_BUTTON);
    igEdited[0] = new RTImage ("tick-hollow-small", Gtk::ICON_SIZE_BUTTON);
    iEdited[1] = new RTImage ("tick-small", Gtk::ICON_SIZE_BUTTON);
    igEdited[1] = new RTImage ("tick-small", Gtk::ICON_SIZE_BUTTON);

    for (int i = 0; i < 2; i++) {
        iEdited[i]->show ();
        bEdited[i] = Gtk::manage(new Gtk::ToggleButton ());
        bEdited[i]->get_style_context()->add_class("smallbutton");
        bEdited[i]->set_active (false);
        bEdited[i]->set_image (*igEdited[i]);
        bEdited[i]->set_relief (Gtk::RELIEF_NONE);
        fltrEditedBox->pack_start (*bEdited[i], Gtk::PACK_SHRINK);
        //13, 14
        bCateg[i + 13] = bEdited[i]->signal_toggled().connect (sigc::bind(sigc::mem_fun(*this, &FileCatalog::categoryButtonToggled), bEdited[i], true));
        bEdited[i]->signal_button_press_event().connect (sigc::mem_fun(*this, &FileCatalog::capture_event), false);
    }

    bEdited[0]->set_tooltip_markup (M("FILEBROWSER_SHOWEDITEDNOTHINT"));
    bEdited[1]->set_tooltip_markup (M("FILEBROWSER_SHOWEDITEDHINT"));

    // RecentlySaved
    // TODO The "g" variant was the more transparent variant of the icon, used
    // when the button was not toggled. Simplify this, change to ordinary
    // togglebutton, use CSS for opacity change.
    iRecentlySaved[0] = new RTImage ("saved-no-small", Gtk::ICON_SIZE_BUTTON);
    igRecentlySaved[0] = new RTImage ("saved-no-small", Gtk::ICON_SIZE_BUTTON);
    iRecentlySaved[1] = new RTImage ("saved-yes-small", Gtk::ICON_SIZE_BUTTON);
    igRecentlySaved[1] = new RTImage ("saved-yes-small", Gtk::ICON_SIZE_BUTTON);

    for (int i = 0; i < 2; i++) {
        iRecentlySaved[i]->show ();
        bRecentlySaved[i] = Gtk::manage(new Gtk::ToggleButton ());
        bRecentlySaved[i]->get_style_context()->add_class("smallbutton");
        bRecentlySaved[i]->set_active (false);
        bRecentlySaved[i]->set_image (*igRecentlySaved[i]);
        bRecentlySaved[i]->set_relief (Gtk::RELIEF_NONE);
        fltrRecentlySavedBox->pack_start (*bRecentlySaved[i], Gtk::PACK_SHRINK);
        //15, 16
        bCateg[i + 15] = bRecentlySaved[i]->signal_toggled().connect (sigc::bind(sigc::mem_fun(*this, &FileCatalog::categoryButtonToggled), bRecentlySaved[i], true));
        bRecentlySaved[i]->signal_button_press_event().connect (sigc::mem_fun(*this, &FileCatalog::capture_event), false);
    }

    bRecentlySaved[0]->set_tooltip_markup (M("FILEBROWSER_SHOWRECENTLYSAVEDNOTHINT"));
    bRecentlySaved[1]->set_tooltip_markup (M("FILEBROWSER_SHOWRECENTLYSAVEDHINT"));

    fltrVbox2->pack_start (*fltrEditedBox, Gtk::PACK_SHRINK, 0);
    fltrVbox2->pack_start (*fltrRecentlySavedBox, Gtk::PACK_SHRINK, 0);
    filterBar->pack_start (*fltrVbox2, Gtk::PACK_SHRINK);

    filterBar->pack_start (*Gtk::manage(new Gtk::Separator(Gtk::ORIENTATION_VERTICAL)), Gtk::PACK_SHRINK);

    // Trash
    iTrashShowEmpty = new RTImage("trash-modern", Gtk::ICON_SIZE_BUTTON) ;
    iTrashShowFull  = new RTImage("trash-modern", Gtk::ICON_SIZE_BUTTON) ;

    bTrash = Gtk::manage( new Gtk::ToggleButton () );
    bTrash->set_image (*iTrashShowEmpty);
    bTrash->set_relief (Gtk::RELIEF_NONE);
    bTrash->set_tooltip_markup (M("FILEBROWSER_SHOWTRASHHINT"));
    bCateg[17] = bTrash->signal_toggled().connect (sigc::bind(sigc::mem_fun(*this, &FileCatalog::categoryButtonToggled), bTrash, true));
    bTrash->signal_button_press_event().connect (sigc::mem_fun(*this, &FileCatalog::capture_event), false);

    iNotTrash = new RTImage("trash-hide-deleted", Gtk::ICON_SIZE_LARGE_TOOLBAR) ;
    iOriginal = new RTImage("filter-original", Gtk::ICON_SIZE_LARGE_TOOLBAR);

    bNotTrash = Gtk::manage( new Gtk::ToggleButton () );
    bNotTrash->set_image (*iNotTrash);
    bNotTrash->set_relief (Gtk::RELIEF_NONE);
    bNotTrash->set_tooltip_markup (M("FILEBROWSER_SHOWNOTTRASHHINT"));
    bCateg[18] = bNotTrash->signal_toggled().connect (sigc::bind(sigc::mem_fun(*this, &FileCatalog::categoryButtonToggled), bNotTrash, true));
    bNotTrash->signal_button_press_event().connect (sigc::mem_fun(*this, &FileCatalog::capture_event), false);

    bOriginal = Gtk::manage( new Gtk::ToggleButton () );
    bOriginal->set_image (*iOriginal);
    bOriginal->set_tooltip_markup (M("FILEBROWSER_SHOWORIGINALHINT"));
    bOriginal->set_relief (Gtk::RELIEF_NONE);
    bCateg[19] = bOriginal->signal_toggled().connect (sigc::bind(sigc::mem_fun(*this, &FileCatalog::categoryButtonToggled), bOriginal, true));
    bOriginal->signal_button_press_event().connect (sigc::mem_fun(*this, &FileCatalog::capture_event), false);

    bRecursive = Gtk::manage(new Gtk::ToggleButton());
    bRecursive->set_image(*Gtk::manage(new RTImage("folder-subfolder", Gtk::ICON_SIZE_LARGE_TOOLBAR)));
    bRecursive->set_tooltip_text(M("FILEBROWSER_SHOWRECURSIVE"));
    bRecursive->set_relief(Gtk::RELIEF_NONE);
    bRecursive->set_active(options.browseRecursive);
    bRecursive->signal_toggled().connect(sigc::mem_fun(*this, &FileCatalog::showRecursiveToggled));

    // bTrash and bRecursive stay on main bar; bOriginal goes in filter bar
    filterBar->pack_start (*bOriginal, Gtk::PACK_SHRINK);
    // bNotTrash is not packed (removed from UI, always inactive)

    // Filetype filter dropdown
    filterBar->pack_start(*Gtk::manage(new Gtk::Separator(Gtk::ORIENTATION_VERTICAL)), Gtk::PACK_SHRINK);
    {
        filetypeButton_ = Gtk::manage(new Gtk::MenuButton());
        filetypeButton_->set_label(M("FILEBROWSER_FILETYPE_ALL"));
        filetypeButton_->set_relief(Gtk::RELIEF_NONE);
        filetypeButton_->set_tooltip_markup(M("FILEBROWSER_FILETYPE_TOOLTIP"));
        filetypeButton_->get_style_context()->add_class("smallbutton");

        filetypePopover_ = Gtk::manage(new Gtk::Popover(*filetypeButton_));
        filetypeBox_ = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 4));
        filetypeBox_->set_margin_start(10);
        filetypeBox_->set_margin_end(10);
        filetypeBox_->set_margin_top(8);
        filetypeBox_->set_margin_bottom(8);
        filetypeBox_->set_size_request(140, -1);

        filetypeAllCheck_ = Gtk::manage(new Gtk::CheckButton(M("FILEBROWSER_FILETYPE_SELECTALL")));
        filetypeAllCheck_->set_active(selectedFiletypes_.empty());
        filetypeAllCheck_->signal_toggled().connect(sigc::mem_fun(*this, &FileCatalog::onFiletypeAllToggled));
        filetypeBox_->pack_start(*filetypeAllCheck_, Gtk::PACK_SHRINK);
        filetypeBox_->pack_start(*Gtk::manage(new Gtk::Separator(Gtk::ORIENTATION_HORIZONTAL)), Gtk::PACK_SHRINK);

        filetypeDefaultCheck_ = Gtk::manage(new Gtk::CheckButton(M("FILEBROWSER_FILETYPE_DEFAULT")));
        filetypeDefaultCheck_->set_tooltip_markup(M("FILEBROWSER_FILETYPE_DEFAULT_TOOLTIP"));
        filetypeDefaultCheck_->signal_toggled().connect(sigc::mem_fun(*this, &FileCatalog::onFiletypeDefaultToggled));
        filetypeBox_->pack_end(*filetypeDefaultCheck_, Gtk::PACK_SHRINK);
        filetypeBox_->pack_end(*Gtk::manage(new Gtk::Separator(Gtk::ORIENTATION_HORIZONTAL)), Gtk::PACK_SHRINK);

        filetypePopover_->add(*filetypeBox_);
        filetypePopover_->set_position(Gtk::POS_BOTTOM);
        filetypeButton_->set_popover(*filetypePopover_);
        filterBar->pack_start(*filetypeButton_, Gtk::PACK_SHRINK);
        updateFiletypeButtonLabel();
        updateFiletypeDefaultCheck_();
    }

    // Wrap filterBar in a Revealer
    filterRevealer_ = Gtk::manage(new Gtk::Revealer());
    filterRevealer_->set_transition_type(Gtk::REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    filterRevealer_->set_transition_duration(200);
    filterRevealer_->add(*filterBar);
    filterRevealer_->set_reveal_child(false);

    fileBrowser->trash_changed().connect( sigc::mem_fun(*this, &FileCatalog::trashChanged) );

    categoryButtons[0] = bFilterClear;
    categoryButtons[1] = bUnRanked;
    for (int i = 0; i < 5; i++) {
        categoryButtons[i + 2] = bRank[i];
    }
    categoryButtons[7] = bUnCLabeled;
    for (int i = 0; i < 5; i++) {
        categoryButtons[i + 8] = bCLabel[i];
    }
    for (int i = 0; i < 2; i++) {
        categoryButtons[i + 13] = bEdited[i];
    }
    for (int i = 0; i < 2; i++) {
        categoryButtons[i + 15] = bRecentlySaved[i];
    }
    categoryButtons[17] = bTrash;
    categoryButtons[18] = bNotTrash;
    categoryButtons[19] = bOriginal;
    categoryButtons[20] = bPicked;
    categoryButtons[21] = bUnflagged;
    categoryButtons[22] = bRejected;

    exifInfo = Gtk::manage(new Gtk::ToggleButton ());
    exifInfo->set_image (*Gtk::manage(new RTImage ("info-modern", Gtk::ICON_SIZE_BUTTON)));
    exifInfo->set_relief (Gtk::RELIEF_NONE);
    exifInfo->set_tooltip_markup (M("FILEBROWSER_SHOWEXIFINFO"));
    exifInfo->set_active( options.showFileNames );
    exifInfo->signal_toggled().connect(sigc::mem_fun(*this, &FileCatalog::exifInfoButtonToggled));

    // thumbnail zoom slider — uses MyHScale for custom tick mark drawing
    zoomSlider_ = Gtk::manage(new MyHScale());
    zoomSlider_->set_range(0, options.thumbnailZoomRatios.size() - 1);
    zoomSlider_->set_increments(1, 1);
    zoomSlider_->set_draw_value(false);
    zoomSlider_->set_size_request(200, -1);
    zoomSlider_->set_valign(Gtk::ALIGN_CENTER);
    // Same compact slider CSS as Adjuster (MyHScale draws its own thumb)
    {
        auto sliderCss = Gtk::CssProvider::create();
        try {
            sliderCss->load_from_data(
                "scale { padding: 0; margin: 0; min-height: 0; }"
                " scale trough { min-height: 3px; margin: 0; padding: 0 4px; }"
                " scale slider { min-height: 0; min-width: 0; padding: 7px; margin: -7px;"
                "   background: transparent; border-color: transparent;"
                "   border: none; box-shadow: none; }"
                " scale trough highlight { margin: 0; padding: 0; min-height: 0; }"
            );
            zoomSlider_->get_style_context()->add_provider(
                sliderCss, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 200
            );
        } catch (...) {}
    }
    // Set initial slider position based on current thumbnail height
    {
        int curHeight = fileBrowser->getThumbnailHeight();
        int maxHeight = options.maxThumbnailHeight;
        int bestIdx = 0;
        int bestDiff = std::abs(curHeight - (int)(options.thumbnailZoomRatios[0] * maxHeight));
        for (size_t i = 1; i < options.thumbnailZoomRatios.size(); i++) {
            int h = (int)(options.thumbnailZoomRatios[i] * maxHeight);
            int diff = std::abs(curHeight - h);
            if (diff < bestDiff) {
                bestDiff = diff;
                bestIdx = i;
            }
        }
        zoomSlider_->set_value(bestIdx);
    }
    zoomSlider_->signal_value_changed().connect(sigc::mem_fun(*this, &FileCatalog::zoomSliderChanged));

    // Rotate moved to the context menu's inline quick actions

    tbRightPanel_1 = new Gtk::ToggleButton ();
    iRightPanel_1_Show = new RTImage("panel-to-left", Gtk::ICON_SIZE_LARGE_TOOLBAR);
    iRightPanel_1_Hide = new RTImage("panel-to-right", Gtk::ICON_SIZE_LARGE_TOOLBAR);

    tbRightPanel_1->set_relief(Gtk::RELIEF_NONE);
    tbRightPanel_1->set_active (true);
    tbRightPanel_1->set_tooltip_markup (M("MAIN_TOOLTIP_SHOWHIDERP1"));
    tbRightPanel_1->set_image (*iRightPanel_1_Hide);
    tbRightPanel_1->signal_toggled().connect( sigc::mem_fun(*this, &FileCatalog::tbRightPanel_1_toggled) );

    // --- Single row layout ---
    // [filter] [trash] [info] [BrowsePath + Query] [zoom] [rotate]  ...  [rightPanel]
    buttonBar->pack_start (*bTrash, Gtk::PACK_SHRINK);
    buttonBar->pack_start (*exifInfo, Gtk::PACK_SHRINK);
    buttonBar->pack_start (*hbToolBar1, Gtk::PACK_SHRINK, 0);
    zoomSlider_->set_margin_start(8);
    buttonBar->pack_start(*zoomSlider_, Gtk::PACK_SHRINK);
    buttonBar->pack_end (*tbRightPanel_1, Gtk::PACK_SHRINK);

    // Hide hand tool in browser context — not needed for browsing
    if (toolBar) {
        toolBar->hideHandTool();
    }

    // CoarsePanel still exists for editor use but not shown in browser bar
    coarsePanel->set_no_show_all(true);
    coarsePanel->hide();

    // Pack filter revealer below the toolbar
    pack_start (*filterRevealer_, Gtk::PACK_SHRINK);

    // add default panel
    hBox = Gtk::manage( new Gtk::Box () );
    hBox->show ();
    hBox->pack_end (*fileBrowser);
    hBox->set_name ("FilmstripPanel");
    fileBrowser->applyFilter (getFilter()); // warning: can call this only after all objects used in getFilter (e.g. Query) are instantiated
    //printf("FileCatalog::FileCatalog  fileBrowser->applyFilter (getFilter())\n");
    pack_start (*hBox);

    enabled = true;

    lastScrollPos = 0;

    for (int i = 0; i < 18; i++) {
        hScrollPos[i] = 0;
        vScrollPos[i] = 0;
    }
}

bool FileCatalog::onColorLabelChildEnter(GdkEventCrossing* /*event*/)
{
    // Pointer entered one of our widgets — cancel any pending collapse
    colorCollapseDelay_.disconnect();

    if (!colorLabelExpanded_) {
        colorLabelExpanded_ = true;
        colorLabelRevealer_->set_reveal_child(true);

        // Start fade animation
        if (!colorFadeConn_.connected()) {
            colorFadeConn_ = Glib::signal_timeout().connect(
                sigc::mem_fun(*this, &FileCatalog::onColorLabelFadeTick), 16);
        }
    }
    return false;
}

bool FileCatalog::onColorLabelChildLeave(GdkEventCrossing* event)
{
    // Ignore if pointer moved to a child widget (e.g. button label inside button)
    if (event->detail == GDK_NOTIFY_INFERIOR) return false;

    // Schedule collapse after a short delay — gives the pointer time to
    // reach the next sibling widget, which will cancel this on enter.
    if (!colorCollapseDelay_.connected()) {
        colorCollapseDelay_ = Glib::signal_timeout().connect([this]() -> bool {
            colorLabelExpanded_ = false;
            colorLabelRevealer_->set_reveal_child(false);

            // Start fade-in animation for summary
            if (!colorFadeConn_.connected()) {
                colorFadeConn_ = Glib::signal_timeout().connect(
                    sigc::mem_fun(*this, &FileCatalog::onColorLabelFadeTick), 16);
            }
            return false; // one-shot
        }, 300);
    }
    return false;
}

bool FileCatalog::onColorLabelFadeTick()
{
    const double step = 0.07;
    if (colorLabelExpanded_) {
        colorSummaryOpacity_ = std::max(0.0, colorSummaryOpacity_ - step);
    } else {
        colorSummaryOpacity_ = std::min(1.0, colorSummaryOpacity_ + step);
    }
    colorLabelSummary_->queue_draw();

    bool done = (colorLabelExpanded_ && colorSummaryOpacity_ <= 0.0) ||
                (!colorLabelExpanded_ && colorSummaryOpacity_ >= 1.0);
    return !done;
}

FileCatalog::~FileCatalog()
{
    delete rejectsPopover_;
    colorFadeConn_.disconnect();
    colorCollapseDelay_.disconnect();
    reparseDirectoryConn_.disconnect();
    filmstripCenterConnection_.disconnect();
    if (navigationBenchmarkTimeoutId_ != 0) {
        g_source_remove(navigationBenchmarkTimeoutId_);
        navigationBenchmarkTimeoutId_ = 0;
    }
    idle_register.destroy();

    for (int i = 0; i < 5; i++) {
        delete iranked[i];
        delete igranked[i];
        delete iCLabeled[i];
        delete igCLabeled[i];
    }

    for (int i = 0; i < 2; i++) {
        delete iEdited[i];
        delete igEdited[i];
        delete iRecentlySaved[i];
        delete igRecentlySaved[i];
    }

    delete iFilterClear;
    delete igFilterClear;
    delete iUnRanked;
    delete igUnRanked;
    delete iUnCLabeled;
    delete igUnCLabeled;
    delete iTrashShowEmpty;
    delete iTrashShowFull;
    delete iNotTrash;
    delete iOriginal;
    delete iRefreshWhite;
    delete iRefreshRed;
    delete iQueryClear;
    delete iLeftPanel_1_Show;
    delete iLeftPanel_1_Hide;
    delete iRightPanel_1_Show;
    delete iRightPanel_1_Hide;
}

bool FileCatalog::capture_event(GdkEventButton* event)
{
    // need to record modifiers on the button press, because signal_toggled does not pass the event.
    modifierKey = event->state;
    return false;
}

void FileCatalog::filterToggled()
{
    if (!filterRevealer_) return;

    bool show = bFilterToggle_->get_active();
    filterRevealer_->set_reveal_child(show);

    if (!show) {
        // When hiding filter bar, reset to "show all" (activate bFilterClear)
        const int numCateg = sizeof(bCateg) / sizeof(bCateg[0]);
        for (int i = 0; i < numCateg; i++) {
            bCateg[i].block(true);
        }

        bFilterClear->set_active(true);
        for (int i = 1; i < 20; i++) {
            categoryButtons[i]->set_active(false);
        }

        for (int i = 0; i < numCateg; i++) {
            bCateg[i].block(false);
        }

        fileBrowser->applyFilter(getFilter());
        fileBrowser->redraw();
    }
}

void FileCatalog::exifInfoButtonToggled()
{
    auto& options = App::get().mut_options();
    if (inTabMode) {
        options.filmStripShowFileNames =  exifInfo->get_active();
    } else {
        options.showFileNames =  exifInfo->get_active();
    }

    fileBrowser->refreshThumbImages ();
    refreshHeight();
}

void FileCatalog::on_realize()
{

    Gtk::Box::on_realize();
    Pango::FontDescription fontd = get_style_context()->get_font();
    fileBrowser->get_pango_context()->set_font_description (fontd);
//    batchQueue->get_pango_context()->set_font_description (fontd);
}

void FileCatalog::closeDir ()
{

    if (filterPanel) {
        filterPanel->set_sensitive (false);
    }

    if (exportPanel) {
        exportPanel->set_sensitive (false);
    }

    dirMonitors.clear();

    // ignore old requests
    ++selectedDirectoryId;
    earlySelectDone_ = false;
    directoryScanComplete_ = true;
    previewsFinishedPending_ = false;
    previewsFinishRetryQueued_ = false;
    previewBatchFirstDrainPending_.store(false, std::memory_order_release);
    if (navigationBenchmarkTimeoutId_ != 0) {
        g_source_remove(navigationBenchmarkTimeoutId_);
        navigationBenchmarkTimeoutId_ = 0;
    }
    navigationBenchmarkStarted_ = false;
    navigationBenchmarkRemaining_ = 0;
    navigationBenchmarkIntervalMs_ = 0;
    navigationBenchmarkIndex_ = 0;
    navigationBenchmarkDirection_ = NAV_NEXT;
    navigationBenchmarkRawOnly_ = false;
    stopFolderLoadTiming_();
    filetypeUpdateQueued_ = false;
    reparseDirectoryQueued_ = false;
    reparseDirectoryConn_.disconnect();

    // terminate thumbnail preview loading
    previewLoader->removeAllJobs ();

    // discard any pending preview batch entries
    {
        std::lock_guard<std::mutex> lock(previewBatchMutex_);
        for (auto& p : pendingPreviews_) {
            delete p.second;
        }
        pendingPreviews_.clear();
        previewBatchPending_ = false;
    }

    // terminate thumbnail updater
    thumbImageUpdater->removeAllJobs ();

    // remove entries
    selectedDirectory = "";
    fileBrowser->close ();
    fileNameList.clear ();
    queuedPreviewKeys_.clear();

    {
        MyMutex::MyLock lock(dirEFSMutex);
        dirEFS.clear ();
    }
    hasValidCurrentEFS = false;

    // Clear filetype UI for the new directory, but preserve selectedFiletypes_
    // so the filter persists across folder switches
    filetypeBlockSignals_ = true;
    knownFiletypes_.clear();
    for (auto& pair : filetypeChecks_) {
        filetypeBox_->remove(*pair.second);
    }
    filetypeChecks_.clear();
    filetypeAllCheck_->set_active(selectedFiletypes_.empty());
    filetypeBlockSignals_ = false;
    updateFiletypeButtonLabel();

    redrawAll ();
}

void FileCatalog::startFolderLoadTiming_()
{
    if (!folderLoadBenchmarkEnabled()) {
        folderLoadTimingActive_ = false;
        folderLoadFirstPreviewLogged_ = false;
        folderLoadNextPreviewMilestone_ = 0;
        return;
    }

    folderLoadStart_ = std::chrono::steady_clock::now();
    folderLoadTimingActive_ = true;
    folderLoadFirstPreviewLogged_ = false;
    folderLoadNextPreviewMilestone_ = 16;
    logFolderLoadTiming_("start");
}

void FileCatalog::stopFolderLoadTiming_()
{
    folderLoadTimingActive_ = false;
    folderLoadFirstPreviewLogged_ = false;
    folderLoadNextPreviewMilestone_ = 0;
}

void FileCatalog::logFolderLoadTiming_(const char* stage) const
{
    if (!folderLoadTimingActive_) {
        return;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - folderLoadStart_);

    std::cout
        << "RT_FOLDER_LOAD_BENCH"
        << " stage=" << stage
        << " elapsed_ms=" << elapsed.count()
        << " loaded=" << previewsLoaded
        << " total=" << previewsToLoad
        << " dir=\"" << selectedDirectory.raw() << "\""
        << std::endl;
}

std::vector<Glib::ustring> FileCatalog::getFileList(std::vector<Glib::RefPtr<Gio::File>> *dirs_explored)
{
    std::vector<Glib::ustring> names;

    const auto& options = App::get().options();
    int dirs_left = options.browseRecursive ? options.browseRecursiveMaxDirs : 0;
    getFilesRecursively(selectedDirectory, options.browseRecursiveDepth, dirs_left, names, dirs_explored);

    return names;
}

void FileCatalog::dirSelected (const Glib::ustring& dirname, const Glib::ustring& openfile)
{
    // Exit album mode when navigating to a directory
    inAlbumMode_ = false;

    try {
        const Glib::RefPtr<Gio::File> dir = Gio::File::create_for_path(dirname);

        if (!dir) {
            return;
        }

        // Skip reload if same directory is already loaded and no specific file to open
        if (openfile.empty() && selectedDirectory == dir->get_parse_name()) {
            return;
        }

        closeDir();
        previewsToLoad = 0;
        previewsLoaded = 0;

        // if openfile exists, we have to open it first (it is a command line argument)
        if (!openfile.empty()) {
            addAndOpenFile (openfile);
        }

        // Tell the preview loader to prioritize jobs near the target image
        // so the filmstrip shows relevant thumbnails first.
        const Glib::ustring priorityFile = !openfile.empty() ? openfile : imageToSelect_fname;
        std::string priorityFileKey;
        if (!priorityFile.empty()) {
            priorityFileKey = openfile.empty() && priorityFile == imageToSelect_fname && !imageToSelect_key.empty()
                ? imageToSelect_key
                : catalogPathKey(priorityFile);
            previewLoader->setPriorityHint(priorityFile, std::move(priorityFileKey));
        }

        selectedDirectory = dir->get_parse_name();
        startFolderLoadTiming_();

        BrowsePath->set_text(selectedDirectory);
        buttonBrowsePath->set_image(*iRefreshWhite);
        filepanel->loadingThumbs(M("PROGRESSBAR_LOADINGTHUMBS"), 0);

        // Warm-start the target thumbnail before the directory scanner reaches
        // it. Large folders can stream for a while before hitting the selected
        // file; queueing it now makes keyboard navigation and restored
        // selections feel immediate. The normal scanner path will skip it via
        // queuedPreviewKeys_ when it arrives later.
        if (openfile.empty()
            && !priorityFile.empty()
            && isEnabledImagePath(priorityFile)
            && catalogPathKey(Glib::path_get_dirname(priorityFile)) == catalogPathKey(selectedDirectory)) {
            addFile(priorityFile);
        }

        // Enumerate files in the background and feed previews progressively.
        // Large folders should start showing thumbnails while the scan is
        // still running instead of waiting for a complete file list first.
        directoryScanComplete_ = false;
        previewsFinishedPending_ = false;
        previewBatchFirstDrainPending_.store(true, std::memory_order_release);
        const int dirId = selectedDirectoryId.load();
        const Glib::ustring selDir = selectedDirectory;
        std::thread([this, dirId, selDir]() {
            std::vector<Glib::RefPtr<Gio::File>> allDirs;
            const auto& opts = App::get().options();
            int dirs_left = opts.browseRecursive ? opts.browseRecursiveMaxDirs : 0;

            // Phase 1: Collect all filenames (fast — just readdir)
            std::vector<Glib::ustring> allFiles;
            std::vector<Glib::ustring> batch;
            std::vector<std::string> batchKeys;
            constexpr std::size_t INITIAL_BATCH = 1;
            constexpr std::size_t VIEWPORT_FILL_BATCH = 32;
            constexpr std::size_t VIEWPORT_FILL_BATCHES = 3;
            constexpr std::size_t STEADY_BATCH = 256;
            constexpr std::size_t STEADY_DIRECTORY_SCAN_THRESHOLD = 64;
            allFiles.reserve(STEADY_BATCH);
            batch.reserve(STEADY_BATCH);
            batchKeys.reserve(STEADY_BATCH);
            const bool deduplicateScan = opts.browseRecursive;
            std::unordered_set<std::string> seenKeys;
            if (deduplicateScan) {
                seenKeys.reserve(STEADY_BATCH * 4);
            }
            bool firstBatch = true;
            std::size_t viewportFillBatchesRemaining = VIEWPORT_FILL_BATCHES;
            const std::size_t firstBatchPrecomputeThreshold = STEADY_DIRECTORY_SCAN_THRESHOLD;
            auto dispatchBatch = [
                this,
                dirId,
                &firstBatch,
                &viewportFillBatchesRemaining,
                firstBatchPrecomputeThreshold,
                steadyBatch = STEADY_BATCH
            ](
                std::vector<Glib::ustring>& files,
                std::vector<std::string>& fileKeys) {
                if (files.empty()) {
                    return;
                }

                auto batchToDispatch = std::make_shared<std::vector<Glib::ustring>>();
                batchToDispatch->swap(files);
                auto batchKeys = std::make_shared<std::vector<std::string>>();
                batchKeys->swap(fileKeys);
                files.reserve(steadyBatch);
                fileKeys.reserve(steadyBatch);
                const bool firstDispatch = firstBatch;
                const bool viewportFillDispatch =
                    !firstBatch
                    && viewportFillBatchesRemaining > 0
                    && batchToDispatch->size() >= VIEWPORT_FILL_BATCH;
                firstBatch = false;
                if (viewportFillDispatch) {
                    --viewportFillBatchesRemaining;
                }
                const bool dispatchBeforePrecompute = firstDispatch || viewportFillDispatch;

                if (dirId != selectedDirectoryId.load(std::memory_order_relaxed)) {
                    return;
                }

                std::vector<Glib::ustring> rawFiles;
                rawFiles.reserve(batchToDispatch->size());
                for (const auto& f : *batchToDispatch) {
                    if (isRawNavigationBenchmarkPath(f)) {
                        rawFiles.push_back(f);
                    }
                }
                if (dispatchBeforePrecompute) {
                    idle_register.add(
                        [this, dirId, batchToDispatch, batchKeys]() -> bool {
                            if (dirId == selectedDirectoryId.load()) {
                                addFiles(std::move(*batchToDispatch), std::move(*batchKeys));
                            }

                            return false;
                        },
                        (firstDispatch || viewportFillDispatch) ? G_PRIORITY_HIGH_IDLE : G_PRIORITY_DEFAULT_IDLE
                    );

                    // Keep the early UI dispatches immediate. Only the first
                    // batch gets synchronous warmup; the next viewport-fill
                    // batches should not compete with preview workers for
                    // cache locks and disk probes.
                    if (firstDispatch) {
                        const auto precomputeFiles = std::make_shared<std::vector<Glib::ustring>>(*batchToDispatch);
                        cacheMgr->precomputeEntryMD5(*precomputeFiles, firstBatchPrecomputeThreshold);
                    }

                    return;
                }

                idle_register.add(
                    [this, dirId, batchToDispatch, batchKeys]() -> bool {
                        if (dirId == selectedDirectoryId.load()) {
                            addFiles(std::move(*batchToDispatch), std::move(*batchKeys));
                        }
                        return false;
                    },
                    G_PRIORITY_HIGH_IDLE
                );

                cacheMgr->precomputeMD5(*batchToDispatch, STEADY_DIRECTORY_SCAN_THRESHOLD);

                cacheMgr->precomputeEntryMD5(rawFiles, STEADY_DIRECTORY_SCAN_THRESHOLD, false, false);
            };

            const bool completed = getFilesRecursivelyStreaming(
                selDir, opts.browseRecursiveDepth, dirs_left,
                [&](const Glib::ustring& fname) -> bool {
                    if (dirId != selectedDirectoryId.load(std::memory_order_relaxed)) {
                        return false;
                    }

                    std::string fileKey = catalogPathKey(fname);
                    if (deduplicateScan && !seenKeys.insert(fileKey).second) {
                        return true;
                    }

                    allFiles.push_back(fname);
                    batch.push_back(fname);
                    batchKeys.push_back(std::move(fileKey));

                    const std::size_t dispatchThreshold = firstBatch
                        ? INITIAL_BATCH
                        : (viewportFillBatchesRemaining > 0 ? VIEWPORT_FILL_BATCH : STEADY_BATCH);
                    if (batch.size() >= dispatchThreshold) {
                        dispatchBatch(batch, batchKeys);
                    }

                    return true;
                },
                &allDirs);

            if (!completed || dirId != selectedDirectoryId.load(std::memory_order_relaxed)) {
                return;
            }

            auto listedFiles = std::make_shared<std::vector<Glib::ustring>>(std::move(allFiles));

            dispatchBatch(batch, batchKeys);

            idle_register.add([this, dirId, allDirs, listedFiles]() -> bool {
                if (dirId != selectedDirectoryId.load()) {
                    return false;
                }
                fileNameList.swap(*listedFiles);
                directoryScanComplete_ = true;
                logFolderLoadTiming_("scan-complete");
                _refreshProgressBar();
                if (fileNameList.empty() && previewsToLoad == 0 && previewsLoaded == 0) {
                    filepanel->loadingThumbs(M("PROGRESSBAR_NOIMAGES"), 0);
                }
                refreshDirectoryMonitors(allDirs);
                previewLoader->setPostScanDrainMode(true);
                previewLoader->wakePendingWorkers();

                previewsFinishedPending_ = true;
                idle_register.add(
                    [this, dirId]() -> bool
                    {
                        previewsFinishedUI(dirId);
                        return false;
                    },
                    G_PRIORITY_HIGH_IDLE + 1
                );
                return false;
            }, G_PRIORITY_HIGH_IDLE);
        }).detach();
    } catch (Glib::Exception& ex) {
        std::cout << ex.what();
    }
}

void FileCatalog::refreshDirectoryMonitors(const std::vector<Glib::RefPtr<Gio::File>> &dirs_to_monitor)
{
    std::unordered_set<std::string> updatedDirNames;
    updatedDirNames.reserve(dirs_to_monitor.size());

    for (const auto& updatedDir : dirs_to_monitor) {
        updatedDirNames.insert(updatedDir->get_path());
    }

    // Remove monitors on directories that are no longer shown.
    dirMonitors.erase(
        std::remove_if(dirMonitors.begin(), dirMonitors.end(),
            [&updatedDirNames](const FileMonitorInfo &fileMonitorInfo) {
                return updatedDirNames.find(fileMonitorInfo.filePath.raw()) == updatedDirNames.end();
            }),
        dirMonitors.end());

    // Add monitors that do not exist yet.
    std::unordered_set<std::string> monitoredDirNames;
    monitoredDirNames.reserve(dirMonitors.size() + dirs_to_monitor.size());
    for (const auto& dirMonitor : dirMonitors) {
        monitoredDirNames.insert(dirMonitor.filePath.raw());
    }

    for (const auto &dir_to_monitor : dirs_to_monitor) {
        const auto dir_path = dir_to_monitor->get_path();
        if (monitoredDirNames.find(dir_path) != monitoredDirNames.end()) {
            continue; // A monitor exists already.
        }
        auto dir_monitor = dir_to_monitor->monitor_directory();
        dir_monitor->signal_changed().connect(sigc::bind(sigc::mem_fun(*this, &FileCatalog::on_dir_changed), false));
        dirMonitors.emplace_back(dir_monitor, dir_path);
        monitoredDirNames.insert(dir_path);
    }
}

void FileCatalog::enableTabMode(bool enable)
{
    inTabMode = enable;

    const auto& options = App::get().options();
    if (enable) {
        // Add CSS class for filmstrip-specific styling (zero padding)
        get_style_context()->add_class("filmstrip");
        fileBrowser->get_style_context()->add_class("filmstrip");

        // Collapse the filter bar when entering filmstrip mode to prevent
        // it from overlapping the filmstrip thumbnails.
        if (bFilterToggle_ && bFilterToggle_->get_active()) {
            bFilterToggle_->set_active(false);  // triggers filterToggled → hides & clears
        }
        // Hide the revealer widget entirely so it contributes zero pixels
        filterRevealer_->hide();

        if (options.showFilmStripToolBar) {
            showToolBar();
        } else {
            hideToolBar();
        }

        fltrVbox1->hide();
        exifInfo->set_active( options.filmStripShowFileNames );

    } else {
        get_style_context()->remove_class("filmstrip");
        fileBrowser->get_style_context()->remove_class("filmstrip");

        stb_->show();
        buttonBar->show();
        hbToolBar1->show();
        filterRevealer_->show();
        fltrVbox1->show();
        exifInfo->set_active( options.showFileNames );
    }

    fileBrowser->enableTabMode(inTabMode);

    filmstripCenterConnection_.disconnect();
    if (enable) {
        auto centerSelected = [this]() {
            if (!inTabMode) {
                return;
            }

            if (Thumbnail* selectedThumbnail = fileBrowser->getSelectedThumbnail()) {
                fileBrowser->selectImage(selectedThumbnail->getFileName(), true);
            }
        };

        // Center once with the existing allocation, then again after GTK has
        // reparented and allocated the narrower editor filmstrip.
        centerSelected();
        filmstripCenterConnection_ = Glib::signal_timeout().connect(
            [this, centerSelected]() -> bool {
                centerSelected();
                return false;
            },
            16,
            G_PRIORITY_HIGH_IDLE);
    }

    // Reset size request now that inTabMode flag is set.
    // In tab mode this clears the large height from browser mode;
    // in browser mode this recalculates the proper height.
    refreshHeight();

    if (!enable) {
        // Reapply the browser's own filter to clear any editor-applied filter
        filterChanged();
    }

    // Entering filmstrip mode is redrawn by ThumbBrowserBase once its entry
    // geometry lock is available. A second synchronous redraw here can block
    // the GTK thread behind thumbnail workers while a large folder is loading.
    if (!enable) {
        redrawAll();
    }
}

void FileCatalog::_refreshProgressBar ()
{
    // In tab mode, no progress bar at all
    // Also mention that this progress bar only measures the FIRST pass (quick thumbnails)
    // The second, usually longer pass is done multithreaded down in the single entries and is NOT measured by this
    if (!inTabMode && (!previewsToLoad || std::floor(100.f * previewsLoaded / previewsToLoad) != std::floor(100.f * (previewsLoaded - 1) / previewsToLoad))) {

        const auto& options = App::get().options();
        if (!progressImage || !progressLabel) {
            // create tab label once
            Gtk::Notebook *nb = (Gtk::Notebook *)(filepanel->get_parent());
            Gtk::Grid* grid = Gtk::manage(new Gtk::Grid());
            setExpandAlignProperties (grid, false, false, Gtk::ALIGN_CENTER, Gtk::ALIGN_CENTER);
            progressImage = Gtk::manage(new RTImage("folder-closed", Gtk::ICON_SIZE_LARGE_TOOLBAR));
            progressLabel = Gtk::manage(new Gtk::Label(M("MAIN_FRAME_FILEBROWSER")));
            grid->attach_next_to(*progressImage, options.mainNBVertical ? Gtk::POS_TOP : Gtk::POS_RIGHT, 1, 1);
            grid->attach_next_to(*progressLabel, options.mainNBVertical ? Gtk::POS_TOP : Gtk::POS_RIGHT, 1, 1);
            grid->set_tooltip_markup(M("MAIN_FRAME_FILEBROWSER_TOOLTIP"));
            grid->show_all();
            if (options.mainNBVertical) {
                progressLabel->set_angle(90);
            }
            if (nb) {
                nb->set_tab_label(*filepanel, *grid);
            }
        }
        if (!previewsToLoad) {
            progressImage->set_from_icon_name("folder-closed", Gtk::ICON_SIZE_LARGE_TOOLBAR);
            int filteredCount = min(fileBrowser->getNumFiltered(), previewsLoaded);
            progressLabel->set_text(M("MAIN_FRAME_FILEBROWSER") +
                                    (filteredCount != previewsLoaded ? " [" + Glib::ustring::format(filteredCount) + "/" : " (")
                                    + Glib::ustring::format(previewsLoaded) +
                                    (filteredCount != previewsLoaded ? "]" : ")"));
        } else {
            progressImage->set_from_icon_name("magnifier", Gtk::ICON_SIZE_LARGE_TOOLBAR);
            progressLabel->set_text(M("MAIN_FRAME_FILEBROWSER") + " ["
                                    + Glib::ustring::format(previewsLoaded) + "/"
                                    + Glib::ustring::format(previewsToLoad) + "]" );
            filepanel->loadingThumbs("", (double)previewsLoaded / previewsToLoad);
        }
    }
}

void FileCatalog::previewReady (int dir_id, FileBrowserEntry* fdn)
{
    PreviewLoaderListener::PreviewReadyBatch entries;
    entries.emplace_back(dir_id, fdn);
    previewReadyBatch(std::move(entries));
}

void FileCatalog::previewReadyBatch (PreviewLoaderListener::PreviewReadyBatch&& entries)
{
    if (entries.empty()) {
        return;
    }

    const int currentDirectoryId = selectedDirectoryId.load(std::memory_order_acquire);
    auto keep = entries.begin();
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (it->first == currentDirectoryId) {
            if (keep != it) {
                *keep = std::move(*it);
            }
            ++keep;
        } else {
            delete it->second;
        }
    }
    entries.erase(keep, entries.end());
    if (entries.empty()) {
        return;
    }

    // Collect entries from background PreviewLoader threads into a batch.
    // A single idle source drains the queue in bounded chunks, avoiding both
    // thousands of idle callbacks and one long GTK-main-loop monopolizer when
    // loading folders with many images.
    bool needSchedule = false;
    {
        std::lock_guard<std::mutex> lock(previewBatchMutex_);
        for (auto& entry : entries) {
            pendingPreviews_.push_back(std::move(entry));
        }
        if (!previewBatchPending_
            && previewBatchPauseDepth_.load(std::memory_order_acquire) == 0) {
            previewBatchPending_ = true;
            needSchedule = true;
        }
    }

    if (needSchedule) {
        const bool firstDrain = previewBatchFirstDrainPending_.exchange(false, std::memory_order_acq_rel);

        idle_register.add(
            [this, firstDrain]() -> bool {
                const bool keepActive = processPendingPreviews_();

                if (firstDrain && keepActive) {
                    idle_register.add(
                        [this]() -> bool {
                            return processPendingPreviews_();
                        },
                        G_PRIORITY_HIGH_IDLE
                    );
                    return false;
                }

                return keepActive;
            },
            G_PRIORITY_HIGH_IDLE
        );
    }
}

bool FileCatalog::processPendingPreviews_()
{
    using clock = std::chrono::steady_clock;

    if (previewBatchPauseDepth_.load(std::memory_order_acquire) > 0) {
        std::lock_guard<std::mutex> lock(previewBatchMutex_);
        previewBatchPending_ = false;
        return false;
    }

    constexpr std::size_t MAX_PREVIEWS_PER_IDLE = 64;
    constexpr std::size_t POST_SCAN_MAX_PREVIEWS_PER_IDLE = 128;
    constexpr std::size_t INITIAL_MIN_PREVIEWS_PER_IDLE = 1;
    constexpr std::size_t STEADY_MIN_PREVIEWS_PER_IDLE = 8;
    constexpr std::size_t POST_SCAN_MIN_PREVIEWS_PER_IDLE = 16;
    constexpr auto PREVIEW_IDLE_BUDGET = std::chrono::milliseconds(4);
    constexpr auto POST_SCAN_PREVIEW_IDLE_BUDGET = std::chrono::milliseconds(6);
    const bool filmstripDrain = fileBrowser->isInTabMode();
    const bool initialDrain = fileBrowser->getEntries().empty();
    const bool postScanDrain = directoryScanComplete_ && !initialDrain;
    const std::size_t maxPreviewsPerIdle = filmstripDrain
        ? 48
        : (postScanDrain ? POST_SCAN_MAX_PREVIEWS_PER_IDLE : MAX_PREVIEWS_PER_IDLE);
    const std::size_t minPreviewsPerIdle = filmstripDrain
        ? 4
        : (initialDrain
            ? INITIAL_MIN_PREVIEWS_PER_IDLE
            : (postScanDrain ? POST_SCAN_MIN_PREVIEWS_PER_IDLE : STEADY_MIN_PREVIEWS_PER_IDLE));
    const auto previewIdleBudget = filmstripDrain
        ? std::chrono::milliseconds(3)
        : (postScanDrain ? POST_SCAN_PREVIEW_IDLE_BUDGET : PREVIEW_IDLE_BUDGET);

    auto& previewChunk = previewChunkScratch_;
    previewChunk.clear();
    previewChunk.reserve(maxPreviewsPerIdle);

    auto& entriesToAdd = entriesToAddScratch_;
    entriesToAdd.clear();
    entriesToAdd.reserve(maxPreviewsPerIdle);
    if (!earlySelectDone_ && !imageToSelect_fname.empty() && imageToSelect_key.empty()) {
        imageToSelect_key = catalogPathKey(imageToSelect_fname);
    }

    const bool hasSelectTargetKey =
        !earlySelectDone_ && !imageToSelect_fname.empty() && !imageToSelect_key.empty();
    bool selectTargetArrived = false;

    const auto start = clock::now();
    std::size_t processed = 0;
    bool filetypesChanged = false;

    {
        std::lock_guard<std::mutex> lock(previewBatchMutex_);
        const std::size_t count = std::min(pendingPreviews_.size(), maxPreviewsPerIdle);

        for (std::size_t i = 0; i < count; ++i) {
            previewChunk.push_back(std::move(pendingPreviews_.front()));
            pendingPreviews_.pop_front();
        }

    }

    std::size_t nextPreview = 0;
    while (nextPreview < previewChunk.size()) {
        std::pair<int, FileBrowserEntry*> p = std::move(previewChunk[nextPreview++]);

        ++processed;
        const int dir_id = p.first;
        FileBrowserEntry* fdn = p.second;

        if (dir_id != selectedDirectoryId) {
            delete fdn;
            continue;
        }

        // put it into the "full directory" browser
        fdn->setImageAreaToolListener(iatlistener);
        entriesToAdd.push_back(fdn);

        if (processed >= minPreviewsPerIdle
            && clock::now() - start >= previewIdleBudget) {
            break;
        }
    }

    if (nextPreview < previewChunk.size()) {
        std::lock_guard<std::mutex> lock(previewBatchMutex_);

        for (std::size_t i = previewChunk.size(); i > nextPreview; --i) {
            pendingPreviews_.push_front(std::move(previewChunk[i - 1]));
        }
    }

    if (!entriesToAdd.empty()) {
        const std::size_t attempted = entriesToAdd.size();
        fileBrowser->addEntries_(entriesToAdd);
        const std::size_t rejected = attempted - entriesToAdd.size();

        if (rejected > 0 && previewsToLoad > 0) {
            previewsToLoad = std::max(0, previewsToLoad - static_cast<int>(rejected));
        }

        previewsLoaded += static_cast<int>(entriesToAdd.size());
        if (hasSelectTargetKey) {
            for (const FileBrowserEntry* fdn : entriesToAdd) {
                if (fdn->filename == imageToSelect_fname || fdn->getBrowserPathKey() == imageToSelect_key) {
                    selectTargetArrived = true;
                    break;
                }
            }
        }
    }

    if (!entriesToAdd.empty()) {
        MyMutex::MyLock lock(dirEFSMutex);

        const auto oldFiletypeCount = dirEFS.filetypes.size();
        for (const FileBrowserEntry* fdn : entriesToAdd) {
            const CacheImageData* cfs = fdn->thumbnail->getCacheImageData();
            if (cfs->exifValid) {
                dirEFS.fnumberFrom = std::min(dirEFS.fnumberFrom, cfs->fnumber);
                dirEFS.fnumberTo = std::max(dirEFS.fnumberTo, cfs->fnumber);
                dirEFS.shutterFrom = std::min(dirEFS.shutterFrom, cfs->shutter);
                dirEFS.shutterTo = std::max(dirEFS.shutterTo, cfs->shutter);
                dirEFS.focalFrom = std::min(dirEFS.focalFrom, cfs->focalLen);
                dirEFS.focalTo = std::max(dirEFS.focalTo, cfs->focalLen);
                if (cfs->iso > 0) {
                    dirEFS.isoFrom = std::min(dirEFS.isoFrom, static_cast<unsigned>(cfs->iso));
                    dirEFS.isoTo = std::max(dirEFS.isoTo, static_cast<unsigned>(cfs->iso));
                }
            }

            dirEFS.filetypes.insert(cfs->getFiletypeRaw());
            dirEFS.cameras.insert(cfs->getCameraName());
            dirEFS.lenses.insert(cfs->getLensRaw());
            dirEFS.expcomp.insert(cfs->getExpCompRaw());
        }

        filetypesChanged = dirEFS.filetypes.size() != oldFiletypeCount;
    }

    if (processed > 0) {
        _refreshProgressBar();
    }
    if (folderLoadTimingActive_ && !folderLoadFirstPreviewLogged_ && previewsLoaded > 0) {
        folderLoadFirstPreviewLogged_ = true;
        logFolderLoadTiming_("first-preview");
    }
    while (folderLoadTimingActive_
            && folderLoadNextPreviewMilestone_ > 0
            && previewsLoaded >= folderLoadNextPreviewMilestone_) {
        const Glib::ustring stage = Glib::ustring::compose(
            "preview-%1", folderLoadNextPreviewMilestone_);
        logFolderLoadTiming_(stage.c_str());
        folderLoadNextPreviewMilestone_ *= 2;
    }
    if (filetypesChanged) {
        scheduleFiletypeFilterUpdate_();
    }

    // Early scroll-to-selection: as soon as the target image appears in a
    // batch, select+scroll to it so the user sees it immediately instead of
    // waiting for ALL previews to finish loading.
    if (selectTargetArrived) {
        fileBrowser->selectImage(imageToSelect_fname);
        if (fileBrowser->getSelectedThumbnail()) {
            earlySelectDone_ = true;
        }
    }

    // Check if more entries arrived while we were processing
    bool schedulePreviewDrain = false;
    bool scheduleFinishedCheck = false;
    int finishedCheckDirId = -1;
    {
        std::lock_guard<std::mutex> lock(previewBatchMutex_);
        if (pendingPreviews_.empty()) {
            previewBatchPending_ = false;
            scheduleFinishedCheck = previewsFinishedPending_ && directoryScanComplete_;
            finishedCheckDirId = selectedDirectoryId.load(std::memory_order_relaxed);
        } else {
            // More entries pending — keep the idle source active
            schedulePreviewDrain = true;
        }
    }

    if (schedulePreviewDrain) {
        // Let other high-idle UI work, especially scan-complete, run between
        // preview chunks instead of monopolizing the GTK main loop.
        idle_register.add(
            [this]() -> bool {
                return processPendingPreviews_();
            },
            G_PRIORITY_HIGH_IDLE
        );
        return false;
    }

    if (scheduleFinishedCheck) {
        idle_register.add(
            [this, finishedCheckDirId]() -> bool {
                previewsFinishedUI(finishedCheckDirId);
                return false;
            },
            G_PRIORITY_HIGH_IDLE + 1
        );
    }

    return false;
}

void FileCatalog::pausePreviewBatchProcessing()
{
    previewBatchPauseDepth_.fetch_add(1, std::memory_order_acq_rel);
}

void FileCatalog::resumePreviewBatchProcessing()
{
    unsigned depth = previewBatchPauseDepth_.load(std::memory_order_acquire);
    while (depth > 0) {
        if (previewBatchPauseDepth_.compare_exchange_weak(
                depth,
                depth - 1,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            if (depth > 1) {
                return;
            }
            break;
        }
    }

    bool needSchedule = false;
    {
        std::lock_guard<std::mutex> lock(previewBatchMutex_);
        if (!pendingPreviews_.empty() && !previewBatchPending_) {
            previewBatchPending_ = true;
            needSchedule = true;
        }
    }

    if (needSchedule) {
        idle_register.add(
            [this]() -> bool {
                return processPendingPreviews_();
            },
            G_PRIORITY_HIGH_IDLE
        );
    }
}

void FileCatalog::schedulePreviewsFinishedRetry_(int dir_id, unsigned int delayMs)
{
    previewsFinishedPending_ = true;
    if (previewsFinishRetryQueued_) {
        return;
    }

    previewsFinishRetryQueued_ = true;
    Glib::signal_timeout().connect_once(
        [this, dir_id]() {
            previewsFinishRetryQueued_ = false;
            if (previewsFinishedPending_) {
                previewsFinishedUI(dir_id);
            }
        },
        delayMs,
        G_PRIORITY_LOW
    );
}

void FileCatalog::scheduleNavigationBenchmark_()
{
    if (!navigationBenchmarkEnabled()
        || navigationBenchmarkStarted_
        || App::get().options().tabbedUI
        || fileNameList.size() < 2
        || !fileBrowser) {
        return;
    }

    navigationBenchmarkStarted_ = true;
    navigationBenchmarkRemaining_ = navigationBenchmarkEnvInt("STEEP_NAV_BENCH_COUNT", 8, 1, 200);
    navigationBenchmarkIntervalMs_ = navigationBenchmarkEnvInt("STEEP_NAV_BENCH_INTERVAL_MS", 1200, 100, 10000);
    navigationBenchmarkRawOnly_ = navigationBenchmarkRawOnlyEnabled();
    navigationBenchmarkIndex_ = 0;
    navigationBenchmarkDirection_ = NAV_NEXT;
    const int dirId = selectedDirectoryId.load(std::memory_order_acquire);

    if (navigationBenchmarkRawOnly_) {
        std::size_t rawCount = 0;
        for (std::size_t i = 0; i < fileNameList.size(); ++i) {
            if (isRawNavigationBenchmarkPath(fileNameList[i])) {
                if (rawCount == 0) {
                    navigationBenchmarkIndex_ = i;
                }
                ++rawCount;
            }
        }

        if (rawCount < 2) {
            navigationBenchmarkStarted_ = false;
            navigationBenchmarkIntervalMs_ = 0;
            navigationBenchmarkRawOnly_ = false;
            std::cout
                << "[navBench] aborted raw_only=1 raw_files=" << rawCount
                << " files=" << fileNameList.size()
                << " dir=" << selectedDirectory
                << std::endl;
            return;
        }
    }

    fileBrowser->selectImage(fileNameList[navigationBenchmarkIndex_], false);

    std::cout
        << "[navBench] scheduled count=" << navigationBenchmarkRemaining_
        << " interval_ms=" << navigationBenchmarkIntervalMs_
        << " raw_only=" << static_cast<int>(navigationBenchmarkRawOnly_)
        << " files=" << fileNameList.size()
        << " dir=" << selectedDirectory
        << std::endl;

    scheduleNavigationBenchmarkStep_(dirId);
}

void FileCatalog::scheduleNavigationBenchmarkStep_(int dirId)
{
    if (!navigationBenchmarkStarted_
        || navigationBenchmarkRemaining_ <= 0
        || navigationBenchmarkIntervalMs_ <= 0) {
        return;
    }

    if (navigationBenchmarkTimeoutId_ != 0) {
        g_source_remove(navigationBenchmarkTimeoutId_);
        navigationBenchmarkTimeoutId_ = 0;
    }

    struct NavigationBenchmarkTimeoutData {
        FileCatalog* catalog;
        int dirId;
    };

    auto* data = new NavigationBenchmarkTimeoutData{this, dirId};
    navigationBenchmarkTimeoutId_ = g_timeout_add_full(
        G_PRIORITY_DEFAULT,
        navigationBenchmarkIntervalMs_,
        [](gpointer userData) -> gboolean {
            auto* timeoutData = static_cast<NavigationBenchmarkTimeoutData*>(userData);
            timeoutData->catalog->navigationBenchmarkTimeoutId_ = 0;
            const bool keepRunning = timeoutData->catalog->runNavigationBenchmarkStep_(timeoutData->dirId);
            if (keepRunning) {
                timeoutData->catalog->scheduleNavigationBenchmarkStep_(timeoutData->dirId);
            }
            return G_SOURCE_REMOVE;
        },
        data,
        [](gpointer userData) {
            delete static_cast<NavigationBenchmarkTimeoutData*>(userData);
        });
}

bool FileCatalog::runNavigationBenchmarkStep_(int dirId)
{
    if (!navigationBenchmarkStarted_
        || dirId != selectedDirectoryId.load(std::memory_order_acquire)
        || App::get().options().tabbedUI
        || fileNameList.size() < 2
        || !fileBrowser) {
        navigationBenchmarkStarted_ = false;
        navigationBenchmarkIntervalMs_ = 0;
        navigationBenchmarkRawOnly_ = false;
        std::cout
            << "[navBench] aborted dir_current=" << selectedDirectoryId.load(std::memory_order_acquire)
            << " dir_expected=" << dirId
            << " files=" << fileNameList.size()
            << " tabbed=" << App::get().options().tabbedUI
            << std::endl;
        return false;
    }

    if (navigationBenchmarkRemaining_ <= 0) {
        navigationBenchmarkStarted_ = false;
        navigationBenchmarkIntervalMs_ = 0;
        navigationBenchmarkRawOnly_ = false;
        std::cout << "[navBench] done" << std::endl;
        return false;
    }

    auto findNextBenchmarkIndex = [this](std::size_t start, eRTNav direction, std::size_t& nextIndex) -> bool {
        if (direction != NAV_NEXT && direction != NAV_PREVIOUS) {
            return false;
        }

        if (!navigationBenchmarkRawOnly_) {
            if (direction == NAV_NEXT) {
                if (start + 1 >= fileNameList.size()) {
                    return false;
                }
                nextIndex = start + 1;
                return true;
            }

            if (start == 0) {
                return false;
            }
            nextIndex = start - 1;
            return true;
        }

        if (direction == NAV_NEXT) {
            for (std::size_t i = start + 1; i < fileNameList.size(); ++i) {
                if (isRawNavigationBenchmarkPath(fileNameList[i])) {
                    nextIndex = i;
                    return true;
                }
            }
            return false;
        }

        for (std::size_t i = start; i > 0; --i) {
            const std::size_t candidate = i - 1;
            if (isRawNavigationBenchmarkPath(fileNameList[candidate])) {
                nextIndex = candidate;
                return true;
            }
        }
        return false;
    };

    std::size_t targetIndex = navigationBenchmarkIndex_;
    if (!findNextBenchmarkIndex(navigationBenchmarkIndex_, navigationBenchmarkDirection_, targetIndex)) {
        navigationBenchmarkDirection_ = navigationBenchmarkDirection_ == NAV_NEXT ? NAV_PREVIOUS : NAV_NEXT;
        if (!findNextBenchmarkIndex(navigationBenchmarkIndex_, navigationBenchmarkDirection_, targetIndex)) {
            navigationBenchmarkStarted_ = false;
            navigationBenchmarkIntervalMs_ = 0;
            navigationBenchmarkRawOnly_ = false;
            std::cout << "[navBench] done" << std::endl;
            return false;
        }
    }

    const eRTNav direction = navigationBenchmarkDirection_;
    if (direction != NAV_NEXT && direction != NAV_PREVIOUS) {
        navigationBenchmarkStarted_ = false;
        navigationBenchmarkIntervalMs_ = 0;
        navigationBenchmarkRawOnly_ = false;
        return false;
    }

    const std::size_t anchorIndex = navigationBenchmarkIndex_;
    const Glib::ustring anchor = fileNameList[anchorIndex];

    navigationBenchmarkIndex_ = targetIndex;

    const Glib::ustring target = fileNameList[navigationBenchmarkIndex_];
    const int stepNumber = navigationBenchmarkEnvInt("STEEP_NAV_BENCH_COUNT", 8, 1, 200)
        - navigationBenchmarkRemaining_ + 1;

    std::cout
        << "[navBench] step=" << stepNumber
        << " dir=" << (direction == NAV_NEXT ? "next" : "prev")
        << " anchor=" << anchor
        << " target=" << target
        << std::endl;

    if (navigationBenchmarkRawOnly_) {
        fileBrowser->openEditorImage(target, direction);
    } else {
        fileBrowser->openNextPreviousEditorImage(anchor, direction);
    }

    --navigationBenchmarkRemaining_;

    if (navigationBenchmarkRemaining_ <= 0) {
        navigationBenchmarkStarted_ = false;
        navigationBenchmarkIntervalMs_ = 0;
        navigationBenchmarkRawOnly_ = false;
        std::cout << "[navBench] done" << std::endl;
        return false;
    }

    return true;
}

// Called within GTK UI thread
void FileCatalog::previewsFinishedUI(int dir_id)
{
    if ( dir_id != selectedDirectoryId ) {
        return;
    }

    if (!directoryScanComplete_) {
        previewsFinishedPending_ = true;
        return;
    }

    {
        std::lock_guard<std::mutex> lock(previewBatchMutex_);
        if (previewBatchPending_ || !pendingPreviews_.empty()) {
            schedulePreviewsFinishedRetry_(dir_id, 25);
            return;
        }
    }

    if (previewLoader->hasPendingWork()) {
        previewLoader->wakePendingWorkers();
        schedulePreviewsFinishedRetry_(dir_id, 100);
        return;
    }

    previewsFinishedPending_ = false;
    previewLoader->setPostScanDrainMode(false);
    logFolderLoadTiming_("finished");
    previewsToLoad = 0;

    if (filterPanel) {
        filterPanel->set_sensitive(true);
        if (!hasValidCurrentEFS) {
            MyMutex::MyLock lock(dirEFSMutex);
            filterPanel->setFilter(dirEFS, true);
        } else {
            filterPanel->setFilter(currentEFS, false);
        }
    }

    if (exportPanel) {
        exportPanel->set_sensitive(true);
    }

    const BrowserFilter finalFilter = getFilter();
    if (!fileBrowser->applyPassThroughFilterFast(finalFilter)) {
        fileBrowser->applyFilter(finalFilter);  // refresh total image count
    }
    // restart anything visible that might have been loaded low quality
    fileBrowser->refreshQuickThumbImages();
    _refreshProgressBar();
    flushFiletypeFilterUpdate_();

    filepanel->loadingThumbs(M("PROGRESSBAR_READY"), 0);

    if (!imageToSelect_fname.empty()) {
        fileBrowser->selectImage(imageToSelect_fname, false);
        imageToSelect_fname = "";
        imageToSelect_key.clear();
    }

    if (!refImageForOpen_fname.empty() && actionNextPrevious != NAV_NONE) {
        fileBrowser->openNextPreviousEditorImage(refImageForOpen_fname, actionNextPrevious);
        refImageForOpen_fname = "";
        actionNextPrevious = NAV_NONE;
    }

    // newly added item might have been already trashed in a previous session
    trashChanged();

    logFolderLoadTiming_("browser-ready");
    stopFolderLoadTiming_();

    Glib::ustring quickWarmAnchor;
    if (Thumbnail* selectedThumbnail = fileBrowser->getSelectedThumbnail()) {
        quickWarmAnchor = selectedThumbnail->getFileName();
    }
    if (quickWarmAnchor.empty() && !fileNameList.empty()) {
        quickWarmAnchor = fileNameList.front();
    }
    if (!quickWarmAnchor.empty() && readyQuickPreviewWarmDirectoryId_ != dir_id) {
        readyQuickPreviewWarmDirectoryId_ = dir_id;
        constexpr int READY_QUICK_PREVIEW_WARM_RADIUS = 16;
        fileBrowser->getAdjacentEntriesAndRefresh(
            quickWarmAnchor,
            0,
            0,
            READY_QUICK_PREVIEW_WARM_RADIUS,
            NAV_NEXT);
    }

    scheduleNavigationBenchmark_();
}

void FileCatalog::previewsFinished (int dir_id)
{
    idle_register.add(
        [this, dir_id]() -> bool
        {
            previewsFinishedUI(dir_id);
            return false;
        },
        // keep priority lower than on the other interface functions to make sure callbacks will not be executed out of order
        G_PRIORITY_HIGH_IDLE + 1
    );
}

void FileCatalog::setEnabled (bool e)
{
    enabled = e;
}

void FileCatalog::redrawAll ()
{
    fileBrowser->redraw ();
}

void FileCatalog::refreshThumbImages ()
{
    fileBrowser->refreshThumbImages ();
}

void FileCatalog::refreshHeight ()
{
    if (inTabMode) {
        // In filmstrip mode, do NOT set a size request on FileCatalog.
        // The parent (catalogPane) constrains the height; FileCatalog
        // just fills whatever space it gets. Setting a large size request
        // here would force the parent to grow beyond its cap.
        set_size_request(0, 0);
        return;
    }

    int newHeight = fileBrowser->getEffectiveHeight();

    if (newHeight < 5) {  // This may occur if there's no thumbnail.
        int w, h;
        get_size_request(w, h);
        newHeight = h;
    }

    if (buttonBar->is_visible()) {
        newHeight += buttonBar->get_height();
    }

    set_size_request(0, newHeight);
}

void FileCatalog::_openImage(const std::vector<Thumbnail*>& tmb, eRTNav preloadDirectionHint)
{
    if (enabled && listener) {
        for (size_t i = 0; i < tmb.size(); i++) {
            // fileSelected does not complete with a fully loaded image, but it does do some preliminary checks
            const bool selected = filepanel && listener == filepanel
                ? filepanel->fileSelected(tmb[i], preloadDirectionHint)
                : listener->fileSelected(tmb[i]);

            if (!selected) {
                tmb[i]->decreaseRef();
            } else if (!App::get().options().tabbedUI) {
                // allow only one image in single editor mode
                for (++i; i < tmb.size(); i++) {
                    tmb[i]->decreaseRef();
                }
                break;
            }
        }
    }
}

void FileCatalog::filterApplied()
{
    idle_register.add(
        [this]() -> bool
        {
            _refreshProgressBar();
            return false;
        }
    );
}

void FileCatalog::quickActionProgress(const Glib::ustring& text, double progress)
{
    if (filepanel) {
        filepanel->loadingThumbs(text, progress);
    }
}

bool FileCatalog::transientEditPreviewRequested(
    const Glib::ustring& filename,
    const rtengine::procparams::ProcParams* params,
    bool restore)
{
    return filepanel && filepanel->transientEditPreviewRequested(filename, params, restore);
}

void FileCatalog::openRequested(const std::vector<Thumbnail*>& tmb, eRTNav preloadDirectionHint)
{
    for (const auto thumb : tmb) {
        thumb->increaseRef();
    }

    // Always open synchronously on the main thread. All callers (click, key
    // nav, menu) run on the UI thread, and the idle roundtrip added ~300ms
    // of perceived latency between click and the instant-preview paint.
    _openImage(tmb, preloadDirectionHint);
}

void FileCatalog::deleteRequested(const std::vector<FileBrowserEntry*>& tbe, bool inclBatchProcessed, bool onlySelected)
{
    if (tbe.empty()) {
        return;
    }

    auto& options = App::get().mut_options();

    if (options.confirmDeleteFiles) {
        Gtk::MessageDialog msd (getToplevelWindow(this), M("FILEBROWSER_DELETEDIALOG_HEADER"), true, Gtk::MESSAGE_QUESTION, Gtk::BUTTONS_YES_NO, true);
        if (onlySelected) {
            msd.set_secondary_text(Glib::ustring::compose (inclBatchProcessed ? M("FILEBROWSER_DELETEDIALOG_SELECTEDINCLPROC") : M("FILEBROWSER_DELETEDIALOG_SELECTED"), tbe.size()), true);
        } else {
            msd.set_secondary_text(Glib::ustring::compose (M("FILEBROWSER_DELETEDIALOG_ALL"), tbe.size()), true);
        }

        Gtk::CheckButton dontAsk(M("GENERAL_DONT_ASK_AGAIN"));
        msd.get_content_area()->pack_end(dontAsk, false, false, 4);
        dontAsk.show();

        if (msd.run() != Gtk::RESPONSE_YES) {
            return;
        }

        if (dontAsk.get_active()) {
            options.confirmDeleteFiles = false;
        }
    }

    {
        // Collect filenames and build the set for bulk removal
        std::set<Glib::ustring> fnameSet;
        std::vector<Glib::ustring> filenames;
        filenames.reserve(tbe.size());
        for (const auto* entry : tbe) {
            fnameSet.insert(entry->filename);
            filenames.push_back(entry->filename);
        }

        std::set<std::string> filenameKeys;
        for (const auto& fname : filenames) {
            filenameKeys.insert(catalogPathKey(fname));
        }

        // Pre-compute batch-processed paths on main thread (needs App::get())
        std::vector<std::pair<Glib::ustring, Glib::ustring>> batchPaths;
        if (inclBatchProcessed) {
            const auto& options = App::get().options();
            batchPaths.reserve(filenames.size());
            for (const auto& fname : filenames) {
                Glib::ustring base = BatchQueue::calcAutoFileNameBase(fname);
                Glib::ustring procf = Glib::ustring::compose("%1.%2", base, options.saveFormatBatch.format);
                Glib::ustring paramf = Glib::ustring::compose("%1.%2.out%3", base, options.saveFormatBatch.format, App::PARAM_FILE_EXTENSION);
                batchPaths.emplace_back(std::move(procf), std::move(paramf));
            }
        }

        // Bulk-remove entries from browser (single pass, single redraw)
        auto removed = fileBrowser->delEntries(fnameSet);
        for (auto* entry : removed) {
            delete entry;
        }

        for (const auto& key : filenameKeys) {
            queuedPreviewKeys_.erase(key);
        }
        fileNameList.erase(
            std::remove_if(fileNameList.begin(), fileNameList.end(),
                [&filenameKeys](const Glib::ustring& fname) {
                    return filenameKeys.find(catalogPathKey(fname)) != filenameKeys.end();
                }),
            fileNameList.end());

        previewsLoaded -= static_cast<int>(filenames.size());
        _refreshProgressBar();

        // Filesystem + cache deletion on background thread
        const Glib::ustring paramExt = App::PARAM_FILE_EXTENSION;
        const auto filenamesForDelete = std::make_shared<std::vector<Glib::ustring>>(std::move(filenames));
        const auto batchPathsForDelete = std::make_shared<std::vector<std::pair<Glib::ustring, Glib::ustring>>>(std::move(batchPaths));
        std::thread([filenamesForDelete, batchPathsForDelete, paramExt]() {
            for (const auto& fname : *filenamesForDelete) {
                // delete from cache
                cacheMgr->deleteEntry(fname);
                // delete from file system
                ::g_remove(fname.c_str());
                // delete paramfile if found
                ::g_remove((fname + paramExt).c_str());
                ::g_remove((removeExtension(fname) + paramExt).c_str());
                // delete .thm file
                ::g_remove((removeExtension(fname) + ".thm").c_str());
                ::g_remove((removeExtension(fname) + ".THM").c_str());
            }

            for (const auto& bp : *batchPathsForDelete) {
                ::g_remove(bp.first.c_str());
                ::g_remove(bp.second.c_str());
            }
        }).detach();
    }
}

void FileCatalog::copyMoveRequested(const std::vector<FileBrowserEntry*>& tbe, bool moveRequested)
{
    if (tbe.empty()) {
        return;
    }

    Glib::ustring fc_title;

    if (moveRequested) {
        fc_title = M("FILEBROWSER_POPUPMOVETO");
    } else {
        fc_title = M("FILEBROWSER_POPUPCOPYTO");
    }

    Gtk::FileChooserDialog fc (getToplevelWindow (this), fc_title, Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER );
    fc.add_button( M("GENERAL_CANCEL"), Gtk::RESPONSE_CANCEL);
    fc.add_button( M("GENERAL_OK"), Gtk::RESPONSE_OK);
    auto& options = App::get().mut_options();
    if (!options.lastCopyMovePath.empty() && Glib::file_test(options.lastCopyMovePath, Glib::FILE_TEST_IS_DIR)) {
        fc.set_current_folder(options.lastCopyMovePath);
    } else {
        // open dialog at the 1-st file's path
        fc.set_current_folder(Glib::path_get_dirname(tbe[0]->filename));
    }
    //!!! TODO prevent dialog closing on "enter" key press

    if( fc.run() == Gtk::RESPONSE_OK ) {
        options.lastCopyMovePath = fc.get_current_folder();

        // iterate through selected files
        for (unsigned int i = 0; i < tbe.size(); i++) {
            Glib::ustring src_fPath = tbe[i]->filename;
            Glib::ustring src_Dir = Glib::path_get_dirname(src_fPath);
            Glib::RefPtr<Gio::File> src_file = Gio::File::create_for_path ( src_fPath );

            if( !src_file ) {
                continue;    // if file is missing - skip it
            }

            Glib::ustring fname = src_file->get_basename();
            Glib::ustring fname_noExt = removeExtension(fname);
            Glib::ustring fname_Ext = getExtension(fname);

            // construct  destination File Paths
            Glib::ustring dest_fPath = Glib::build_filename (options.lastCopyMovePath, fname);
            Glib::ustring dest_fPath_param = dest_fPath + App::PARAM_FILE_EXTENSION;

            if (moveRequested && (src_Dir == options.lastCopyMovePath)) {
                continue;
            }

            /* comparison of src_Dir and dest_Dir is done per image for compatibility with
            possible future use of Collections as source where each file's source path may be different.*/

            bool filecopymovecomplete = false;
            int i_copyindex = 1;

            while(!filecopymovecomplete) {
                // check for filename conflicts at destination - prevent overwriting (actually RT will crash on overwriting attempt)
                if (!Glib::file_test(dest_fPath, Glib::FILE_TEST_EXISTS) && !Glib::file_test(dest_fPath_param, Glib::FILE_TEST_EXISTS)) {
                    // copy/move file to destination
                    Glib::RefPtr<Gio::File> dest_file = Gio::File::create_for_path ( dest_fPath );

                    if (moveRequested) {
                        // move file
                        src_file->move(dest_file);
                        // re-attach cache files
                        cacheMgr->renameEntry (src_fPath, tbe[i]->thumbnail->getMD5(), dest_fPath);
                        // remove from browser
                        fileBrowser->delEntry (src_fPath);
                        const std::string srcKey = catalogPathKey(src_fPath);
                        queuedPreviewKeys_.erase(srcKey);
                        fileNameList.erase(
                            std::remove_if(fileNameList.begin(), fileNameList.end(),
                                [&srcKey](const Glib::ustring& fname) {
                                    return catalogPathKey(fname) == srcKey;
                                }),
                            fileNameList.end());

                        previewsLoaded--;
                    } else {
                        src_file->copy(dest_file);
                    }


                    // attempt to copy/move paramFile only if it exist next to the src
                    Glib::RefPtr<Gio::File> scr_param = Gio::File::create_for_path (  src_fPath + App::PARAM_FILE_EXTENSION );

                    if (Glib::file_test( src_fPath + App::PARAM_FILE_EXTENSION, Glib::FILE_TEST_EXISTS)) {
                        Glib::RefPtr<Gio::File> dest_param = Gio::File::create_for_path ( dest_fPath_param);

                        // copy/move paramFile to destination
                        if (moveRequested) {
                            if (Glib::file_test( dest_fPath + App::PARAM_FILE_EXTENSION, Glib::FILE_TEST_EXISTS)) {
                                // profile already got copied to destination from cache after cacheMgr->renameEntry
                                // delete source profile as cleanup
                                ::g_remove ((src_fPath + App::PARAM_FILE_EXTENSION).c_str ());
                            } else {
                                scr_param->move(dest_param);
                            }
                        } else {
                            scr_param->copy(dest_param);
                        }
                    }

                    filecopymovecomplete = true;
                } else {
                    // adjust destination fname to avoid conflicts (append "_<index>", preserve extension)
                    Glib::ustring dest_fname = Glib::ustring::compose("%1%2%3%4%5", fname_noExt, "_", i_copyindex, ".", fname_Ext);
                    // re-construct  destination File Paths
                    dest_fPath = Glib::build_filename (options.lastCopyMovePath, dest_fname);
                    dest_fPath_param = dest_fPath + App::PARAM_FILE_EXTENSION;
                    i_copyindex++;
                }
            }//while
        } // i<tbe.size() loop

        redrawAll ();

        _refreshProgressBar();
    } // Gtk::RESPONSE_OK
}

void FileCatalog::developRequested(const std::vector<FileBrowserEntry*>& tbe, bool fastmode)
{
    if (listener) {
        std::vector<BatchQueueEntry*> entries;

        // TODO: (HOMBRE) should we still use parallelization here, now that thumbnails are processed asynchronously...?
        //#pragma omp parallel for ordered
        for (size_t i = 0; i < tbe.size(); i++) {
            FileBrowserEntry* fbe = tbe[i];
            Thumbnail* th = fbe->thumbnail;
            rtengine::procparams::ProcParams params = th->getProcParams();

            const auto& options = App::get().options();

            // if fast mode is selected, override (disable) params
            // controlling time and resource consuming tasks
            // and also those which effect is not pronounced after reducing the image size
            // TODO!!! could expose selections below via preferences
            if (fastmode) {
                if (!options.fastexport_use_fast_pipeline) {
                    if (options.fastexport_bypass_sharpening) {
                        params.sharpening.enabled = false;
                    }

                    if (options.fastexport_bypass_sharpenEdge) {
                        params.sharpenEdge.enabled = false;
                    }

                    if (options.fastexport_bypass_sharpenMicro) {
                        params.sharpenMicro.enabled = false;
                    }

                    //if (options.fastexport_bypass_lumaDenoise) params.lumaDenoise.enabled = false;
                    //if (options.fastexport_bypass_colorDenoise) params.colorDenoise.enabled = false;
                    if (options.fastexport_bypass_defringe) {
                        params.defringe.enabled = false;
                    }

                    if (options.fastexport_bypass_dirpyrDenoise) {
                        params.dirpyrDenoise.enabled = false;
                    }

                    if (options.fastexport_bypass_dirpyrequalizer) {
                        params.dirpyrequalizer.enabled = false;
                    }

                    if (options.fastexport_bypass_wavelet) {
                        params.wavelet.enabled = false;
                    }

                    //if (options.fastexport_bypass_raw_bayer_all_enhance) params.raw.bayersensor.all_enhance = false;
                    if (options.fastexport_bypass_raw_bayer_dcb_iterations) {
                        params.raw.bayersensor.dcb_iterations = 0;
                    }

                    if (options.fastexport_bypass_raw_bayer_dcb_enhance) {
                        params.raw.bayersensor.dcb_enhance = false;
                    }

                    if (options.fastexport_bypass_raw_bayer_lmmse_iterations) {
                        params.raw.bayersensor.lmmse_iterations = 0;
                    }

                    if (options.fastexport_bypass_raw_bayer_linenoise) {
                        params.raw.bayersensor.linenoise = 0;
                    }

                    if (options.fastexport_bypass_raw_bayer_greenthresh) {
                        params.raw.bayersensor.greenthresh = 0;
                    }

                    if (options.fastexport_bypass_raw_ccSteps) {
                        params.raw.bayersensor.ccSteps = params.raw.xtranssensor.ccSteps = 0;
                    }

                    if (options.fastexport_bypass_raw_ca) {
                        params.raw.ca_autocorrect = false;
                        params.raw.cared = 0;
                        params.raw.cablue = 0;
                    }

                    if (options.fastexport_bypass_raw_df) {
                        params.raw.df_autoselect = false;
                        params.raw.dark_frame = "";
                    }

                    if (options.fastexport_bypass_raw_ff) {
                        params.raw.ff_AutoSelect = false;
                        params.raw.ff_file = "";
                    }

                    params.raw.bayersensor.method = options.fastexport_raw_bayer_method;
                    params.raw.xtranssensor.method = options.fastexport_raw_xtrans_method;
                    params.icm.inputProfile = options.fastexport_icm_input_profile;
                    params.icm.workingProfile = options.fastexport_icm_working_profile;
                    params.icm.outputProfile = options.fastexport_icm_output_profile;
                    params.icm.outputIntent = rtengine::RenderingIntent(options.fastexport_icm_outputIntent);
                    params.icm.outputBPC = options.fastexport_icm_outputBPC;
                }

                if (params.resize.enabled) {
                    params.resize.width = rtengine::min(params.resize.width, options.fastexport_resize_width);
                    params.resize.height = rtengine::min(params.resize.height, options.fastexport_resize_height);
                    params.resize.longedge = rtengine::min(params.resize.longedge, options.fastexport_resize_longedge);
                    params.resize.shortedge = rtengine::min(params.resize.shortedge, options.fastexport_resize_shortedge);
                } else {
                    params.resize.width = options.fastexport_resize_width;
                    params.resize.height = options.fastexport_resize_height;
                    params.resize.longedge = options.fastexport_resize_longedge;
                    params.resize.shortedge = options.fastexport_resize_shortedge;
                }

                params.resize.enabled = options.fastexport_resize_enabled;
                params.resize.scale = options.fastexport_resize_scale;
                params.resize.appliesTo = options.fastexport_resize_appliesTo;
                params.resize.method = options.fastexport_resize_method;
                params.resize.dataspec = options.fastexport_resize_dataspec;
                params.resize.allowUpscaling = false;
            }

            rtengine::ProcessingJob* pjob = rtengine::ProcessingJob::create (fbe->filename, th->getType() == FT_Raw, params, fastmode && options.fastexport_use_fast_pipeline);

            int pw;
            int ph = BatchQueue::calcMaxThumbnailHeight();
            th->getThumbnailSize (pw, ph);

            // processThumbImage is the processing intensive part, but adding to queue must be ordered
            //#pragma omp ordered
            //{
            BatchQueueEntry* bqh = new BatchQueueEntry (pjob, params, fbe->filename, pw, ph, th, options.overwriteOutputFile);
            entries.push_back(bqh);
            //}
        }

        listener->addBatchQueueJobs( entries );
    }
}

void FileCatalog::renameRequested(const std::vector<FileBrowserEntry*>& tbe)
{
    RenameDialog* renameDlg = new RenameDialog ((Gtk::Window*)get_toplevel());

    for (size_t i = 0; i < tbe.size(); i++) {
        renameDlg->initName (Glib::path_get_basename (tbe[i]->filename), tbe[i]->thumbnail->getCacheImageData());

        Glib::ustring ofname = tbe[i]->filename;
        Glib::ustring dirName = Glib::path_get_dirname (tbe[i]->filename);
        Glib::ustring baseName = Glib::path_get_basename (tbe[i]->filename);

        bool success = false;

        do {
            if (renameDlg->run () == Gtk::RESPONSE_OK) {
                Glib::ustring nBaseName = renameDlg->getNewName ();

                // if path has directory components, exit
                if (Glib::path_get_dirname (nBaseName) != ".") {
                    continue;
                }

                // if no extension is given, concatenate the extension of the original file
                Glib::ustring ext = getExtension (nBaseName);

                if (ext.empty()) {
                    nBaseName += "." + getExtension (baseName);
                }

                Glib::ustring nfname = Glib::build_filename (dirName, nBaseName);

                /* check if filename already exists*/
                if (Glib::file_test (nfname, Glib::FILE_TEST_EXISTS)) {
                    Glib::ustring msg_ = Glib::ustring("<b>") + escapeHtmlChars(nfname) + ": " + M("MAIN_MSG_ALREADYEXISTS") + "</b>";
                    Gtk::MessageDialog msgd (msg_, true, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
                    msgd.run ();
                } else {
                    success = true;

                    if (::g_rename (ofname.c_str (), nfname.c_str ()) == 0) {
                        cacheMgr->renameEntry (ofname, tbe[i]->thumbnail->getMD5(), nfname);
                        ::g_remove((ofname + App::PARAM_FILE_EXTENSION).c_str ());
                        reparseDirectory ();
                    }
                }
            } else {
                success = true;
            }
        } while (!success);

        renameDlg->hide ();
    }

    delete renameDlg;
}

void FileCatalog::selectionChanged(const std::vector<Thumbnail*>& tbe)
{
    if (fslistener) {
        fslistener->selectionChanged (tbe);
    }

    // Show coarse panel (rotate/flip) only when images are selected
    if (coarsePanel) {
        if (tbe.empty()) {
            coarsePanel->hide();
        } else {
            coarsePanel->show();
        }
    }

}

void FileCatalog::clearFromCacheRequested(const std::vector<FileBrowserEntry*>& tbe, bool leavenotrace)
{
    if (tbe.empty()) {
        return;
    }

    for (unsigned int i = 0; i < tbe.size(); i++) {
        Glib::ustring fname = tbe[i]->filename;
        // remove from cache
        cacheMgr->clearFromCache (fname, leavenotrace);
    }
}

bool FileCatalog::isInTabMode() const
{
    return inTabMode;
}

void FileCatalog::categoryButtonToggled (Gtk::ToggleButton* b, bool isMouseClick)
{

    //was control key pressed (ignored if was not mouse click)
    bool control_down = modifierKey & GDK_CONTROL_MASK && isMouseClick;

    //was shift key pressed (ignored if was not mouse click)
    bool shift_down   = modifierKey & GDK_SHIFT_MASK && isMouseClick;

    // The event is process here, we can clear modifierKey now, it'll be set again on the next even
    modifierKey = 0;

    const int numCateg = sizeof(bCateg) / sizeof(bCateg[0]);
    const int numButtons = sizeof(categoryButtons) / sizeof(categoryButtons[0]);

    for (int i = 0; i < numCateg; i++) {
        bCateg[i].block (true);
    }

    // button already toggled when entering this function from a mouse click, so
    // we switch it back to its initial state.
    if (isMouseClick) {
        b->set_active(!b->get_active());
    }

    //if both control and shift keys were pressed, do nothing
    if (!(control_down && shift_down)) {

        fileBrowser->getScrollPosition (hScrollPos[lastScrollPos], vScrollPos[lastScrollPos]);

        //we look how many stars are already toggled on, if any
        int toggled_stars_count = 0, buttons = 0, start_star = 0, toggled_button = 0;

        for (int i = 0; i < numButtons; i++) {
            if (categoryButtons[i]->get_active()) {
                if (i > 0 && i < 17) {
                    toggled_stars_count ++;
                    start_star = i;
                }

                buttons |= (1 << i);
            }

            if (categoryButtons[i] == b) {
                toggled_button = i;
            }
        }

        // if no modifier key is pressed,
        if (!(control_down || shift_down)) {
            // Pick-flag filters (picked/unflagged/rejected, indices 20-22)
            // form their own independent group: clicking toggles just that
            // button and leaves the other filter groups untouched.
            if (toggled_button >= 20 && toggled_button <= 22) {
                const bool wasActive = buttons & (1 << toggled_button);
                categoryButtons[toggled_button]->set_active(!wasActive);

                if (!wasActive) {
                    // a pick filter is now active — clear-filters goes off
                    categoryButtons[0]->set_active(false);
                } else if (!(buttons & ~((1u << toggled_button) | 1u))) {
                    // deactivated the last remaining filter — back to clear
                    categoryButtons[0]->set_active(true);
                }
            }
            // if we're deselecting original (or bNotTrash, which is hidden)
            else if (toggled_button >= 18 && toggled_button <= 19 && (buttons & (1 << toggled_button))) {
                categoryButtons[0]->set_active (true);

                for (int i = 1; i < numButtons; i++) {
                    categoryButtons[i]->set_active (false);
                }
            }
            // if we're deselecting the only star still active
            else if (toggled_stars_count == 1 && (buttons & (1 << toggled_button))) {
                // activate clear-filters
                categoryButtons[0]->set_active (true);
                // deactivate the toggled filter
                categoryButtons[toggled_button]->set_active (false);
            }
            // if we're deselecting trash
            else if (toggled_button == 17 && (buttons & (1 << toggled_button))) {
                categoryButtons[0]->set_active (true);
                categoryButtons[17]->set_active (false);
            } else {
                // activate the toggled filter, deactivate the rest
                for (int i = 0; i < numButtons; i++) {
                    categoryButtons[i]->set_active (i == toggled_button);
                }
            }
        }
        //modifier key allowed only for stars and color labels...
        else if (toggled_button > 0 && toggled_button < 17) {
            if (control_down) {
                //control is pressed
                if (toggled_stars_count == 1 && (buttons & (1 << toggled_button))) {
                    //we're deselecting the only star still active, so we activate clear-filters
                    categoryButtons[0]->set_active(true);
                    //and we deselect the toggled star
                    categoryButtons[toggled_button]->set_active (false);
                } else if (toggled_stars_count >= 1) {
                    //we toggle the state of a star (eventually another one than the only one selected)
                    categoryButtons[toggled_button]->set_active(!categoryButtons[toggled_button]->get_active());
                } else {
                    //no star selected
                    //we deselect the 2 non star filters
                    if (buttons &  1    ) {
                        categoryButtons[0]->set_active(false);
                    }

                    if (buttons & (1 << 17)) {
                        categoryButtons[17]->set_active(false);
                    }

                    //and we toggle on the star
                    categoryButtons[toggled_button]->set_active (true);
                }
            } else {
                //shift is pressed, only allowed if 0 or 1 star & labels is selected
                if (!toggled_stars_count) {
                    //we deselect the 2 non star filters
                    if (buttons &  1      ) {
                        categoryButtons[0]->set_active(false);
                    }

                    if (buttons & (1 << 7)) {
                        categoryButtons[7]->set_active(false);
                    }

                    if (buttons & (1 << 13)) {
                        categoryButtons[13]->set_active(false);
                    }

                    if (buttons & (1 << 17)) {
                        categoryButtons[17]->set_active(false);
                    }

                    //and we set the start star to 1 (unrated images)
                    start_star = 1;
                    //we act as if one star were selected
                    toggled_stars_count = 1;
                }

                if (toggled_stars_count == 1) {
                    int current_star = min(start_star, toggled_button);
                    int last_star   = max(start_star, toggled_button);

                    //we permute the start and the end star for the next loop
                    for (; current_star <= last_star; current_star++) {
                        //we toggle on all the star in the range
                        if (!(buttons & (1 << current_star))) {
                            categoryButtons[current_star]->set_active(true);
                        }
                    }
                }

                //if more than one star & color label is selected, do nothing
            }
        }
        // ...or non-trashed or original with Control modifier
        else if (toggled_button >= 18 && toggled_button <= 19 && control_down) {
            Gtk::ToggleButton* categoryButton = categoryButtons[toggled_button];
            categoryButton->set_active (!categoryButton->get_active ());

            // If it was the first or last one, we reset the clear filter.
            if (buttons == 1 || buttons == (1 << toggled_button)) {
                bFilterClear->set_active (!categoryButton->get_active ());
            }
        }

        bool active_now, active_before;

        // FilterClear: set the right images
        // TODO: swapping FilterClear icon needs more work in categoryButtonToggled
        /*active_now = bFilterClear->get_active();
        active_before = buttons & (1 << (0)); // 0
        if      ( active_now && !active_before) bFilterClear->set_image (*iFilterClear);
        else if (!active_now &&  active_before) bFilterClear->set_image (*igFilterClear);*/

        // rank: set the right images
        for (int i = 0; i < 5; i++) {
            active_now = bRank[i]->get_active();
            active_before = buttons & (1 << (i + 2)); // 2,3,4,5,6

            if      ( active_now && !active_before) {
                bRank[i]->set_image (*iranked[i]);
            } else if (!active_now &&  active_before) {
                bRank[i]->set_image (*igranked[i]);
            }
        }

        active_now = bUnRanked->get_active();
        active_before = buttons & (1 << (1)); // 1

        if      ( active_now && !active_before) {
            bUnRanked->set_image (*iUnRanked);
        } else if (!active_now &&  active_before) {
            bUnRanked->set_image (*igUnRanked);
        }

        // color labels: set the right images
        for (int i = 0; i < 5; i++) {
            active_now = bCLabel[i]->get_active();
            active_before = buttons & (1 << (i + 8)); // 8,9,10,11,12

            if      ( active_now && !active_before) {
                bCLabel[i]->set_image (*iCLabeled[i]);
            } else if (!active_now &&  active_before) {
                bCLabel[i]->set_image (*igCLabeled[i]);
            }
        }

        active_now = bUnCLabeled->get_active();
        active_before = buttons & (1 << (7)); // 7

        if      ( active_now && !active_before) {
            bUnCLabeled->set_image (*iUnCLabeled);
        } else if (!active_now &&  active_before) {
            bUnCLabeled->set_image (*igUnCLabeled);
        }

        // Edited: set the right images
        for (int i = 0; i < 2; i++) {
            active_now = bEdited[i]->get_active();
            active_before = buttons & (1 << (i + 13)); //13,14

            if      ( active_now && !active_before) {
                bEdited[i]->set_image (*iEdited[i]);
            } else if (!active_now &&  active_before) {
                bEdited[i]->set_image (*igEdited[i]);
            }
        }

        // RecentlySaved: set the right images
        for (int i = 0; i < 2; i++) {
            active_now = bRecentlySaved[i]->get_active();
            active_before = buttons & (1 << (i + 15)); //15,16

            if      ( active_now && !active_before) {
                bRecentlySaved[i]->set_image (*iRecentlySaved[i]);
            } else if (!active_now &&  active_before) {
                bRecentlySaved[i]->set_image (*igRecentlySaved[i]);
            }
        }

        fileBrowser->applyFilter (getFilter ());
        _refreshProgressBar();

        //rearrange panels according to the selected filter
        removeIfThere (hBox, trashButtonBox);

        if (bTrash->get_active ()) {
            hBox->pack_start (*trashButtonBox, Gtk::PACK_SHRINK, 4);
        }

        hBox->queue_draw ();

        fileBrowser->setScrollPosition (hScrollPos[lastScrollPos], vScrollPos[lastScrollPos]);
    }

    for (int i = 0; i < numCateg; i++) {
        bCateg[i].block (false);
    }
}

void FileCatalog::rejectsViewToggled ()
{
    rejectsViewActive_ = bRejectsView && bRejectsView->get_active();

    removeIfThere (hBox, rejectsButtonBox);
    if (rejectsViewActive_) {
        hBox->pack_start (*rejectsButtonBox, Gtk::PACK_SHRINK, 4);
    }
    hBox->queue_draw ();

    fileBrowser->applyFilter (getFilter ());
    _refreshProgressBar ();
}

void FileCatalog::deleteAllRejects ()
{
    const auto rejects = fileBrowser->getRejectedEntries ();
    if (rejects.empty ()) {
        return;
    }

    // Standard delete flow with its confirmation dialog; the list holds all
    // rejected photos of the currently browsed folder.
    deleteRequested (rejects, false, false);
}

void FileCatalog::showRejectsPopover ()
{
    const auto& options = App::get().options();

    if (!rejectsPopover_) {
        rejectsPopover_ = new Gtk::Popover(*bRejected);

        auto* box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 6));
        box->set_margin_start(10);
        box->set_margin_end(10);
        box->set_margin_top(8);
        box->set_margin_bottom(8);

        hideRejectsCheck_ = Gtk::manage(new Gtk::CheckButton(M("FILEBROWSER_HIDEREJECTS")));
        hideRejectsCheck_->set_tooltip_markup(M("FILEBROWSER_HIDEREJECTSHINT"));
        hideRejectsCheck_->signal_toggled().connect([this]() {
            App::get().mut_options().browserHideRejects = hideRejectsCheck_->get_active();
            // Persist immediately — the preference must survive even a
            // crash or force-kill, not just a clean exit.
            Options::save();
            fileBrowser->applyFilter(getFilter());
            _refreshProgressBar();
        });
        box->pack_start(*hideRejectsCheck_, Gtk::PACK_SHRINK);

        box->pack_start(*Gtk::manage(new Gtk::Separator(Gtk::ORIENTATION_HORIZONTAL)), Gtk::PACK_SHRINK);

        auto* cullTitle = Gtk::manage(new Gtk::Label());
        cullTitle->set_markup("<b>" + Glib::Markup::escape_text(M("FILEBROWSER_AUTOCULL")) + "</b>");
        cullTitle->set_halign(Gtk::ALIGN_START);
        box->pack_start(*cullTitle, Gtk::PACK_SHRINK);

        auto addScale = [&box](const Glib::ustring& label) {
            auto* l = Gtk::manage(new Gtk::Label(label));
            l->set_halign(Gtk::ALIGN_START);
            box->pack_start(*l, Gtk::PACK_SHRINK);
            auto* s = Gtk::manage(new Gtk::Scale(Gtk::ORIENTATION_HORIZONTAL));
            s->set_range(0.0, 100.0);
            s->set_increments(1.0, 10.0);
            s->set_digits(0);
            s->set_draw_value(true);
            s->set_value_pos(Gtk::POS_RIGHT);
            s->set_size_request(220, -1);
            box->pack_start(*s, Gtk::PACK_SHRINK);
            return s;
        };
        cullFocusScale_ = addScale(M("FILEBROWSER_AUTOCULL_FOCUS"));
        cullExposureScale_ = addScale(M("FILEBROWSER_AUTOCULL_EXPOSURE"));

        auto* runButton = Gtk::manage(new Gtk::Button(M("FILEBROWSER_AUTOCULL_RUN")));
        runButton->signal_clicked().connect([this]() {
            auto& mutOptions = App::get().mut_options();
            mutOptions.autoCullFocusTolerance =
                static_cast<int>(cullFocusScale_->get_value() + 0.5);
            mutOptions.autoCullExposureTolerance =
                static_cast<int>(cullExposureScale_->get_value() + 0.5);
            Options::save();
            rejectsPopover_->popdown();
            fileBrowser->startAutoCull(
                mutOptions.autoCullFocusTolerance,
                mutOptions.autoCullExposureTolerance);
        });
        box->pack_start(*runButton, Gtk::PACK_SHRINK);

        cullUndoButton_ = Gtk::manage(new Gtk::Button(M("FILEBROWSER_AUTOCULL_UNDO")));
        cullUndoButton_->signal_clicked().connect([this]() {
            fileBrowser->undoAutoCull();
            cullUndoButton_->set_sensitive(false);
        });
        box->pack_start(*cullUndoButton_, Gtk::PACK_SHRINK);

        rejectsPopover_->add(*box);
        rejectsPopover_->set_position(Gtk::POS_BOTTOM);
        box->show_all();
    }

    // Sync current state each time it opens
    hideRejectsCheck_->set_active(options.browserHideRejects);
    cullFocusScale_->set_value(options.autoCullFocusTolerance);
    cullExposureScale_->set_value(options.autoCullExposureTolerance);
    cullUndoButton_->set_sensitive(
        fileBrowser->hasAutoCullUndo() && !fileBrowser->isQuickActionRunning());

    rejectsPopover_->popup();
}

void FileCatalog::reapplyBrowserFilter ()
{
    fileBrowser->applyFilter (getFilter ());
    _refreshProgressBar ();
    // Nudge preview loading in case background work was left paused by the
    // editor's foreground-priority machinery.
    previewLoader->resume ();
}

void FileCatalog::showRecursiveToggled()
{
    bool state = bRecursive->get_active();

    if (state == App::get().options().browseRecursive) {
        // avoid unnecessary calls to dirSelected; this can happen when file catalog is reset to an older state
        return;
    }

    App::get().mut_options().browseRecursive = state;

    // Killing background threads can sometimes block the UI for a long time,
    // This may be a spot where giving the user information is needed.
    previewLoader->removeAllJobs();
    thumbImageUpdater->removeAllJobs();

    idle_register.add(
        [this]() -> bool
        {
            dirSelected(selectedDirectory, "");
            return false;
        }
    );
}

BrowserFilter FileCatalog::getFilter ()
{

    BrowserFilter filter;

    bool anyRankFilterActive = bUnRanked->get_active () || bRank[0]->get_active () || bRank[1]->get_active () || bRank[2]->get_active () || bRank[3]->get_active () || bRank[4]->get_active ();
    bool anyCLabelFilterActive = bUnCLabeled->get_active () || bCLabel[0]->get_active () || bCLabel[1]->get_active () || bCLabel[2]->get_active () || bCLabel[3]->get_active () || bCLabel[4]->get_active ();
    bool anyPickFilterActive = bPicked->get_active() || bRejected->get_active() || bUnflagged->get_active();
    bool anyEditedFilterActive = bEdited[0]->get_active() || bEdited[1]->get_active();
    bool anyRecentlySavedFilterActive = bRecentlySaved[0]->get_active() || bRecentlySaved[1]->get_active();
    const bool anySupplementaryActive = bOriginal->get_active();
    /*
     * filter is setup in 2 steps
     * Step 1: handle individual filters
    */
    filter.showRanked[0] = bFilterClear->get_active() || bUnRanked->get_active () || bTrash->get_active () || anySupplementaryActive ||
                           anyCLabelFilterActive || anyEditedFilterActive || anyRecentlySavedFilterActive;

    filter.showCLabeled[0] = bFilterClear->get_active() || bUnCLabeled->get_active () || bTrash->get_active ()  || anySupplementaryActive ||
                             anyRankFilterActive || anyEditedFilterActive || anyRecentlySavedFilterActive;

    for (int i = 1; i <= 5; i++) {
        filter.showRanked[i] = bFilterClear->get_active() || bRank[i - 1]->get_active () || bTrash->get_active () || anySupplementaryActive ||
                               anyCLabelFilterActive || anyEditedFilterActive || anyRecentlySavedFilterActive;

        filter.showCLabeled[i] = bFilterClear->get_active() || bCLabel[i - 1]->get_active () || bTrash->get_active ()  || anySupplementaryActive ||
                                 anyRankFilterActive || anyEditedFilterActive || anyRecentlySavedFilterActive;
    }

    for (int i = 0; i < 2; i++) {
        filter.showEdited[i] = bFilterClear->get_active() || bEdited[i]->get_active () || bTrash->get_active ()  || anySupplementaryActive ||
                               anyRankFilterActive || anyCLabelFilterActive || anyRecentlySavedFilterActive;

        filter.showRecentlySaved[i] = bFilterClear->get_active() || bRecentlySaved[i]->get_active () || bTrash->get_active ()  || anySupplementaryActive ||
                                      anyRankFilterActive || anyCLabelFilterActive || anyEditedFilterActive;
    }


    /*
     * Step 2
     * handle the case when more than 1 filter is selected. This overrides values set in Step
     * if no filters in a group are active, filter.show for each member of that group will be set to true
     * otherwise they are set based on UI input
     */
    if ((anyRankFilterActive && anyCLabelFilterActive ) ||
            (anyRankFilterActive && anyEditedFilterActive ) ||
            (anyRankFilterActive && anyRecentlySavedFilterActive ) ||
            (anyCLabelFilterActive && anyEditedFilterActive ) ||
            (anyCLabelFilterActive && anyRecentlySavedFilterActive ) ||
            (anyEditedFilterActive && anyRecentlySavedFilterActive) ||
            (anySupplementaryActive && (anyRankFilterActive || anyCLabelFilterActive || anyEditedFilterActive || anyRecentlySavedFilterActive))) {

        filter.showRanked[0] = anyRankFilterActive ? bUnRanked->get_active () : true;
        filter.showCLabeled[0] = anyCLabelFilterActive ? bUnCLabeled->get_active () : true;

        for (int i = 1; i <= 5; i++) {
            filter.showRanked[i] = anyRankFilterActive ? bRank[i - 1]->get_active () : true;
            filter.showCLabeled[i] = anyCLabelFilterActive ? bCLabel[i - 1]->get_active () : true;
        }

        for (int i = 0; i < 2; i++) {
            filter.showEdited[i] = anyEditedFilterActive ? bEdited[i]->get_active() : true;
            filter.showRecentlySaved[i] = anyRecentlySavedFilterActive ? bRecentlySaved[i]->get_active() : true;
        }
    }


    filter.showTrash = bTrash->get_active();
    filter.showNotTrash = !bTrash->get_active();
    filter.showOriginal = bOriginal->get_active();

    // Pick filter: if no pick filter buttons are active or filter is cleared, show all
    if (anyPickFilterActive && !bFilterClear->get_active()) {
        filter.showPicked = bPicked->get_active();
        filter.showRejected = bRejected->get_active();
        filter.showUnflagged = bUnflagged->get_active();
    } else {
        filter.showPicked = true;
        filter.showRejected = true;
        filter.showUnflagged = true;
    }

    // Standing hide-rejects preference — yields when the user explicitly
    // filters for rejected photos
    filter.hideRejects = App::get().options().browserHideRejects
        && !(anyPickFilterActive && bRejected->get_active());

    // The rejects view overrides everything pick-related
    if (rejectsViewActive_) {
        filter.showPicked = false;
        filter.showRejected = true;
        filter.showUnflagged = false;
        filter.hideRejects = false;
    }

    if (!filterPanel) {
        filter.exifFilterEnabled = false;
    } else {
        if (!hasValidCurrentEFS) {
            MyMutex::MyLock lock(dirEFSMutex);
            filter.exifFilter = dirEFS;
        } else {
            filter.exifFilter = currentEFS;
        }

        filter.exifFilterEnabled = filterPanel->isEnabled ();
    }

    //TODO add support for more query options. e.g by date, iso, f-number, etc
    //TODO could use date:<value>;iso:<value>  etc
    // default will be filename

    Glib::ustring decodedQueryFileName = Query->get_text(); // for now Query is only by file name

    // Determine the match mode - check if the first 2 characters are equal to "!="
    if (decodedQueryFileName.find("!=") == 0) {
        decodedQueryFileName = decodedQueryFileName.substr(2);
        filter.matchEqual = false;
    } else {
        filter.matchEqual = true;
    }

    // Consider that queryFileName consist of comma separated values (FilterString)
    // Evaluate if ANY of these FilterString are contained in the filename
    // This will construct OR filter within the queryFileName
    filter.vFilterStrings.clear();
    const std::vector<Glib::ustring> filterStrings = Glib::Regex::split_simple(",", decodedQueryFileName.uppercase());
    for (const auto& entry : filterStrings) {
        // ignore empty filterStrings. Otherwise filter will always return true if
        // e.g. queryFileName ends on "," and will stop being a filter
        if (!entry.empty()) {
            filter.vFilterStrings.push_back(entry);
        }
    }
    filter.albumWhitelist = albumWhitelist_;
    filter.filetypeFilter = selectedFiletypes_;

    return filter;
}

void FileCatalog::setAlbumWhitelist (const std::set<std::string>& whitelist)
{
    albumWhitelist_.clear();
    albumWhitelist_.reserve(whitelist.size());

    for (const auto& path : whitelist) {
        albumWhitelist_.insert(catalogPathKey(path));
    }

    filterChanged();
}

void FileCatalog::showAlbumFiles (const Glib::ustring& albumName, const std::vector<Glib::ustring>& files)
{
    // Save current directory so we can restore when exiting album mode
    if (!inAlbumMode_) {
        savedDirectory_ = selectedDirectory;
    }

    // Close current dir and load album files
    closeDir();
    inAlbumMode_ = true;

    // Clear the album whitelist filter — in album view mode we show
    // exactly the files we load, no extra filtering needed
    albumWhitelist_.clear();

    // Update the browse path to show album name
    BrowsePath->set_text(Glib::ustring::compose("Album: %1", albumName));

    // Add each file from the album
    for (const auto& f : files) {
        if (Glib::file_test(f, Glib::FILE_TEST_EXISTS)) {
            addFile(f);
        }
    }

    // Apply filter and redraw
    fileBrowser->applyFilter(getFilter());
    _refreshProgressBar();
}

void FileCatalog::exitAlbumMode ()
{
    if (!inAlbumMode_) return;

    inAlbumMode_ = false;
    albumWhitelist_.clear();

    // Restore saved directory if available
    if (!savedDirectory_.empty()) {
        dirSelected(savedDirectory_, "");
    }
}

void FileCatalog::filterChanged ()
{
    //TODO !!! there is too many repetitive and unnecessary executions of
    // " fileBrowser->applyFilter (getFilter()); " throughout the code
    // this needs further analysis and cleanup
    fileBrowser->applyFilter (getFilter());
    _refreshProgressBar();
}

void FileCatalog::updateFiletypeFilter ()
{
    // Collect filetypes from dirEFS (uppercased for display consistency)
    std::set<std::string> types;
    {
        MyMutex::MyLock lock(dirEFSMutex);
        for (const auto& ft : dirEFS.filetypes) {
            std::string upper = ft;
            std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
            types.insert(upper);
        }
    }

    // Nothing new to add?
    if (types == knownFiletypes_) return;

    filetypeBlockSignals_ = true;

    // Add checkboxes for newly discovered types
    bool hasFilter = !selectedFiletypes_.empty();
    for (const auto& ft : types) {
        if (knownFiletypes_.find(ft) != knownFiletypes_.end()) continue;

        auto* cb = Gtk::manage(new Gtk::CheckButton(ft));
        // If a filter is active from a previous folder, only check types that
        // are in the persisted selection; otherwise start all checked
        cb->set_active(!hasFilter || selectedFiletypes_.count(ft) > 0);
        cb->signal_toggled().connect(
            sigc::bind(sigc::mem_fun(*this, &FileCatalog::onFiletypeCheckToggled), ft));
        filetypeBox_->pack_start(*cb, Gtk::PACK_SHRINK);
        filetypeChecks_[ft] = cb;
    }

    knownFiletypes_ = types;

    // Keep persisted selection intact during progressive loading —
    // we can't prune yet because not all types have been discovered.
    // The "All" checkbox reflects whether a filter is active.
    if (hasFilter) {
        filetypeAllCheck_->set_active(false);
    }

    filetypeBox_->show_all();
    filetypeBlockSignals_ = false;

    updateFiletypeButtonLabel();
    updateFiletypeDefaultCheck_();
    // Re-apply filter so newly discovered types respect the selection
    if (!selectedFiletypes_.empty()) {
        filterChanged();
    }
}

void FileCatalog::scheduleFiletypeFilterUpdate_()
{
    if (filetypeUpdateQueued_) {
        return;
    }

    filetypeUpdateQueued_ = true;
    const int dirId = selectedDirectoryId.load();

    Glib::signal_timeout().connect_once(
        [this, dirId]() {
            if (dirId != selectedDirectoryId.load() || !filetypeUpdateQueued_) {
                return;
            }

            filetypeUpdateQueued_ = false;
            updateFiletypeFilter();
        },
        150,
        G_PRIORITY_LOW
    );
}

void FileCatalog::flushFiletypeFilterUpdate_()
{
    filetypeUpdateQueued_ = false;
    updateFiletypeFilter();
}

void FileCatalog::onFiletypeCheckToggled (const std::string& filetype)
{
    if (filetypeBlockSignals_) return;

    // Rebuild selectedFiletypes_ from checkbox states
    selectedFiletypes_.clear();
    bool allChecked = true;
    for (const auto& pair : filetypeChecks_) {
        if (pair.second->get_active()) {
            selectedFiletypes_.insert(pair.first);
        } else {
            allChecked = false;
        }
    }

    // If all are checked, clear the set (means "show all")
    if (allChecked) {
        selectedFiletypes_.clear();
    }

    // Sync the "All" checkbox
    filetypeBlockSignals_ = true;
    filetypeAllCheck_->set_active(allChecked);
    filetypeBlockSignals_ = false;

    updateFiletypeButtonLabel();
    updateFiletypeDefaultCheck_();
    filterChanged();
}

void FileCatalog::onFiletypeAllToggled ()
{
    if (filetypeBlockSignals_) return;

    bool all = filetypeAllCheck_->get_active();
    filetypeBlockSignals_ = true;
    for (auto& pair : filetypeChecks_) {
        pair.second->set_active(all);
    }
    filetypeBlockSignals_ = false;

    if (all) {
        selectedFiletypes_.clear();
        filetypeButton_->set_label(M("FILEBROWSER_FILETYPE_ALL"));
    } else {
        // "All" unchecked = hide everything (unusual but consistent)
        selectedFiletypes_.clear();
        // Actually, unchecking "All" should deselect all types
        // The filter will show nothing — user must pick specific types
    }

    updateFiletypeButtonLabel();
    updateFiletypeDefaultCheck_();
    filterChanged();
}

void FileCatalog::setSelectedFiletypes (const std::set<std::string>& sel)
{
    selectedFiletypes_ = sel;

    // Sync checkbox state
    filetypeBlockSignals_ = true;
    bool allChecked = sel.empty();
    for (auto& pair : filetypeChecks_) {
        pair.second->set_active(allChecked || sel.count(pair.first) > 0);
    }
    filetypeAllCheck_->set_active(allChecked);
    filetypeBlockSignals_ = false;

    updateFiletypeButtonLabel();
    updateFiletypeDefaultCheck_();
    filterChanged();
}

bool FileCatalog::isCurrentFiletypeFilterDefault() const
{
    if (selectedFiletypes_.empty()) {
        return false;
    }

    std::set<std::string> saved;
    for (const auto& extension : App::get().options().defaultFiletypeFilter) {
        std::string normalized = extension.raw();
        std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char value) {
            return static_cast<char>(std::toupper(value));
        });
        if (!normalized.empty()) {
            saved.insert(normalized);
        }
    }

    return selectedFiletypes_ == saved;
}

void FileCatalog::setCurrentFiletypeFilterAsDefault(bool active)
{
    if (!active && !isCurrentFiletypeFilterDefault()) {
        updateFiletypeDefaultCheck_();
        return;
    }

    auto& saved = App::get().mut_options().defaultFiletypeFilter;
    saved.clear();
    if (active) {
        for (const auto& extension : selectedFiletypes_) {
            saved.emplace_back(extension);
        }
    }

    updateFiletypeDefaultCheck_();

    try {
        Options::save();
    } catch (Options::Error& error) {
        Gtk::MessageDialog dialog(
            getToplevelWindow(this), error.get_msg(), true,
            Gtk::MESSAGE_WARNING, Gtk::BUTTONS_CLOSE, true);
        dialog.run();
    }
}

void FileCatalog::onFiletypeDefaultToggled()
{
    if (filetypeBlockSignals_) {
        return;
    }

    setCurrentFiletypeFilterAsDefault(filetypeDefaultCheck_->get_active());
}

void FileCatalog::updateFiletypeDefaultCheck_()
{
    if (!filetypeDefaultCheck_) {
        return;
    }

    filetypeBlockSignals_ = true;
    filetypeDefaultCheck_->set_sensitive(!selectedFiletypes_.empty());
    filetypeDefaultCheck_->set_active(isCurrentFiletypeFilterDefault());
    filetypeBlockSignals_ = false;
}

void FileCatalog::updateFiletypeButtonLabel ()
{
    if (selectedFiletypes_.empty()) {
        filetypeButton_->set_label(M("FILEBROWSER_FILETYPE_ALL"));
    } else if (selectedFiletypes_.size() == 1) {
        filetypeButton_->set_label(*selectedFiletypes_.begin() + " Only");
    } else if (selectedFiletypes_.size() == 2) {
        auto it = selectedFiletypes_.begin();
        Glib::ustring first = *it++;
        filetypeButton_->set_label(first + ", " + *it);
    } else {
        filetypeButton_->set_label(
            Glib::ustring::compose("%1 +%2 more", *selectedFiletypes_.begin(),
                                   selectedFiletypes_.size() - 1));
    }
}

void FileCatalog::resetFiletypeFilter ()
{
    filetypeBlockSignals_ = true;
    selectedFiletypes_.clear();
    for (auto& pair : filetypeChecks_) {
        pair.second->set_active(true);
    }
    filetypeAllCheck_->set_active(true);
    filetypeBlockSignals_ = false;
    filetypeButton_->set_label(M("FILEBROWSER_FILETYPE_ALL"));
    updateFiletypeDefaultCheck_();
}

void FileCatalog::saveResetState ()
{
    resetData.directory = selectedDirectory;
    resetData.recursive = App::get().options().browseRecursive;
}

bool FileCatalog::restoreResetState ()
{
    bool ret = false;

    if (resetData.recursive != App::get().options().browseRecursive) {
        // I think the program flow here needs to be explained:
        // The App::get().mut_options().browseRecursive is indeed set by the signal of the toggle button "showRecursiveToggled()"
        // However, the signal is handled only after the execution returns to the framework. The "buttonBrowsePathPressed()"
        // on the other hand instantly executes "DirBrowser::selectDir". If the recursive value is not set when this executes, the
        // directory might not contain the image of the "refImageForOpen_fname" variable or the one next to it that is supposed to
        // be opened.
        if (resetData.directory != selectedDirectory) {
            // this can be set only when "DirBrowser::selectDir" is called regardless of the recursive toggle or the toggle will get blocked
            App::get().mut_options().browseRecursive = resetData.recursive;
        }
        bRecursive->set_active(resetData.recursive);
        ret = true;
    }

    if (resetData.directory != selectedDirectory) {
        BrowsePath->set_text(resetData.directory);
        buttonBrowsePathPressed ();
        ret = true;
    }

    return ret;
}

void FileCatalog::reparseDirectory ()
{

    if (selectedDirectory.empty()) {
        return;
    }

    if (!Glib::file_test(selectedDirectory, Glib::FILE_TEST_IS_DIR)) {
        closeDir();
        return;
    }

    // check if a thumbnailed file has been deleted or is not in a directory of interest
    const std::vector<ThumbBrowserEntryBase*>& t = fileBrowser->getEntries();
    std::set<Glib::ustring> fileNamesToDel;
    std::set<Glib::ustring> fileNamesToRemove;

    for (const auto& entry : t) {
        if (!Glib::file_test(entry->filename, Glib::FILE_TEST_EXISTS)) {
            fileNamesToDel.insert(entry->filename);
            fileNamesToRemove.insert(entry->filename);
        }
        else if (!App::get().options().browseRecursive && Glib::path_get_dirname(entry->filename) != selectedDirectory) {
            fileNamesToRemove.insert(entry->filename);
        }
    }

    if (!fileNamesToRemove.empty()) {
        const auto removedEntries = fileBrowser->delEntries(fileNamesToRemove);

        for (const auto& toRemove : fileNamesToRemove) {
            queuedPreviewKeys_.erase(catalogPathKey(toRemove));
        }

        previewsLoaded = std::max(0, previewsLoaded - static_cast<int>(removedEntries.size()));

        for (auto* removedEntry : removedEntries) {
            delete removedEntry;
        }
    }

    for (const auto& toDelete : fileNamesToDel) {
        cacheMgr->deleteEntry(toDelete);
    }

    if (!fileNamesToDel.empty() || !fileNamesToRemove.empty()) {
        _refreshProgressBar();
    }

    // check if a new file has been added
    // build a set of collate-keys for faster search
    std::unordered_set<std::string> oldNames;
    oldNames.reserve(fileNameList.size());
    for (const auto& oldName : fileNameList) {
        oldNames.insert(catalogPathKey(oldName));
    }

    std::vector<Glib::RefPtr<Gio::File>> allDirs;
    fileNameList = getFileList(&allDirs);
    std::vector<Glib::ustring> newNames;
    for (const auto& newName : fileNameList) {
        if (oldNames.find(catalogPathKey(newName)) == oldNames.end()) {
            newNames.push_back(newName);
        }
    }

    if (!newNames.empty()) {
        addFiles(std::move(newNames));
        _refreshProgressBar();
    }

    refreshDirectoryMonitors(allDirs);
}

void FileCatalog::scheduleReparseDirectory_()
{
    if (reparseDirectoryQueued_) {
        return;
    }

    reparseDirectoryQueued_ = true;
    reparseDirectoryConn_ = Glib::signal_timeout().connect(
        [this]() -> bool {
            reparseDirectoryQueued_ = false;
            reparseDirectory();
            return false;
        },
        150,
        G_PRIORITY_DEFAULT_IDLE
    );
}

void FileCatalog::on_dir_changed (const Glib::RefPtr<Gio::File>& file, const Glib::RefPtr<Gio::File>& other_file, Gio::FileMonitorEvent event_type, bool internal)
{
    if (rtengine::settings->metadata_xmp_sync != rtengine::Settings::MetadataXmpSync::NONE
        && (event_type == Gio::FILE_MONITOR_EVENT_CREATED
            || event_type == Gio::FILE_MONITOR_EVENT_DELETED
            || event_type == Gio::FILE_MONITOR_EVENT_CHANGED)
        && isXmpSidecarPath(file->get_parse_name())) {
        cacheMgr->invalidateMD5(file->get_parse_name());
    }

    if ((event_type == Gio::FILE_MONITOR_EVENT_CREATED
            || event_type == Gio::FILE_MONITOR_EVENT_DELETED
            || event_type == Gio::FILE_MONITOR_EVENT_CHANGED)
        && isProcParamSidecarPath(file->get_parse_name())) {
        cacheMgr->invalidateMD5(file->get_parse_name());
    }

    if ((App::get().options().has_retained_extention(file->get_parse_name())
            && (event_type == Gio::FILE_MONITOR_EVENT_CREATED || event_type == Gio::FILE_MONITOR_EVENT_DELETED || event_type == Gio::FILE_MONITOR_EVENT_CHANGED))
             || (event_type == Gio::FILE_MONITOR_EVENT_CREATED && Glib::file_test(file->get_path(), Glib::FileTest::FILE_TEST_IS_DIR))
             || (event_type == Gio::FILE_MONITOR_EVENT_DELETED && std::find_if(dirMonitors.cbegin(), dirMonitors.cend(), [&file](const FileMonitorInfo &monitor) { return monitor.filePath == file->get_path(); }) != dirMonitors.cend()))
    {
        scheduleReparseDirectory_();
    }
}

void FileCatalog::addFile (const Glib::ustring& fName)
{
    if (!fName.empty()) {
        std::string fileKey = catalogPathKey(fName);
        if (!queuedPreviewKeys_.insert(fileKey).second) {
            return;
        }

        previewsFinishedPending_ = false;
        previewLoader->add(selectedDirectoryId, fName, std::move(fileKey), this);
        previewsToLoad++;
    }
}

void FileCatalog::addFiles (const std::vector<Glib::ustring>& fNames)
{
    std::vector<Glib::ustring> copy;
    copy.reserve(fNames.size());
    copy.insert(copy.end(), fNames.begin(), fNames.end());
    addFiles(std::move(copy));
}

void FileCatalog::addFiles (std::vector<Glib::ustring>&& fNames)
{
    std::vector<std::string> fNameKeys;
    fNameKeys.reserve(fNames.size());

    for (const auto& fName : fNames) {
        fNameKeys.push_back(fName.empty() ? std::string() : catalogPathKey(fName));
    }

    addFiles(std::move(fNames), std::move(fNameKeys));
}

void FileCatalog::addFiles (std::vector<Glib::ustring>&& fNames, std::vector<std::string>&& fNameKeys)
{
    std::vector<Glib::ustring> toQueue;
    std::vector<std::string> toQueueKeys;
    toQueue.reserve(fNames.size());
    toQueueKeys.reserve(fNames.size());
    queuedPreviewKeys_.reserve(queuedPreviewKeys_.size() + fNames.size());

    if (fNameKeys.size() != fNames.size()) {
        fNameKeys.clear();
        fNameKeys.reserve(fNames.size());

        for (const auto& fName : fNames) {
            fNameKeys.push_back(fName.empty() ? std::string() : catalogPathKey(fName));
        }
    }

    for (std::size_t i = 0; i < fNames.size(); ++i) {
        auto& fName = fNames[i];
        if (fName.empty()) {
            continue;
        }

        std::string fileKey = std::move(fNameKeys[i]);
        if (fileKey.empty()) {
            fileKey = catalogPathKey(fName);
        }

        if (!queuedPreviewKeys_.insert(fileKey).second) {
            continue;
        }

        toQueueKeys.push_back(std::move(fileKey));
        toQueue.push_back(std::move(fName));
    }

    if (toQueue.empty()) {
        return;
    }

    previewsFinishedPending_ = false;
    previewsToLoad += static_cast<int>(toQueue.size());
    fileBrowser->reserveEntries(toQueue.size());
    cacheMgr->reserveEntries(toQueue.size());
    previewLoader->addBatch(selectedDirectoryId, std::move(toQueue), std::move(toQueueKeys), this);
}

void FileCatalog::addAndOpenFile (const Glib::ustring& fname)
{
    auto file = Gio::File::create_for_path(fname);

    if (!file ) {
        return;
    }

    if (!file->query_exists()) {
        return;
    }

    try {

        const auto info = file->query_info();

        if (!info) {
            return;
        }

        const auto lastdot = info->get_name().find_last_of('.');
        if (lastdot != Glib::ustring::npos) {
            if (!App::get().options().is_extention_enabled(info->get_name().substr(lastdot + 1))) {
                return;
            }
        } else {
            return;
        }


        // if supported, load thumbnail first
        const auto tmb = cacheMgr->getEntry(file->get_parse_name());

        if (!tmb) {
            return;
        }

        FileBrowserEntry* entry = new FileBrowserEntry(tmb, file->get_parse_name());
        queuedPreviewKeys_.insert(catalogPathKey(entry->filename));
        previewReady(selectedDirectoryId, entry);
        // open the file
        tmb->increaseRef();
        idle_register.add(
            [this, tmb]() -> bool
            {
                _openImage({tmb});
                return false;
            }
        );

    } catch(Gio::Error&) {}
}

void FileCatalog::emptyTrash ()
{

    const auto& t = fileBrowser->getEntries();
    std::vector<FileBrowserEntry*> toDel;

    for (const auto entry : t) {
        if ((static_cast<FileBrowserEntry*>(entry))->thumbnail->getTrashed()) {
            toDel.push_back(static_cast<FileBrowserEntry*>(entry));
        }
    }
    if (toDel.size() > 0) {
        deleteRequested(toDel, false, false);
        trashChanged();
    }
}

bool FileCatalog::trashIsEmpty ()
{

    const auto& t = fileBrowser->getEntries();

    for (const auto entry : t) {
        if ((static_cast<FileBrowserEntry*>(entry))->thumbnail->getTrashed()) {
            return false;
        }
    }
    return true;
}

void FileCatalog::zoomIn ()
{

    fileBrowser->zoomIn ();
    refreshHeight();

}
void FileCatalog::zoomOut ()
{

    fileBrowser->zoomOut ();
    refreshHeight();

}
void FileCatalog::zoomSliderChanged ()
{
    const auto& options = App::get().options();
    int idx = (int)zoomSlider_->get_value();
    if (idx < 0) idx = 0;
    if (idx >= (int)options.thumbnailZoomRatios.size()) idx = options.thumbnailZoomRatios.size() - 1;

    int newHeight = (int)(options.thumbnailZoomRatios[idx] * options.maxThumbnailHeight);
    fileBrowser->setThumbnailHeight(newHeight);
    refreshHeight();
}

void FileCatalog::refreshEditedState (const std::set<Glib::ustring>& efiles)
{

    editedFiles = efiles;
    fileBrowser->refreshEditedState (efiles);
}

void FileCatalog::exportRequested()
{
}

// Called within GTK UI thread
void FileCatalog::exifFilterChanged ()
{

    currentEFS = filterPanel->getFilter ();
    hasValidCurrentEFS = true;
    fileBrowser->applyFilter (getFilter ());
    _refreshProgressBar();
}

void FileCatalog::setFilterPanel (FilterPanel* fpanel)
{

    filterPanel = fpanel;
    filterPanel->set_sensitive (false);
    filterPanel->setFilterPanelListener (this);
}

void FileCatalog::setExportPanel(ExportPanel* expanel)
{
    exportPanel = expanel;
    exportPanel->set_sensitive (false);
    exportPanel->setExportPanelListener (this);
    fileBrowser->setExportPanel(expanel);
}

void FileCatalog::trashChanged ()
{
    if (trashIsEmpty()) {
        bTrash->set_image(*iTrashShowEmpty);
    } else {
        bTrash->set_image(*iTrashShowFull);
    }
}

// Called within GTK UI thread
void FileCatalog::buttonQueryClearPressed ()
{
    Query->set_text("");
    FileCatalog::executeQuery ();
}

// Called within GTK UI thread
void FileCatalog::executeQuery()
{
    // if BrowsePath text was changed, do a full browse;
    // otherwise filter only

    if (BrowsePath->get_text() != selectedDirectory) {
        buttonBrowsePathPressed ();
    } else {
        FileCatalog::filterChanged ();
    }
}

bool FileCatalog::Query_key_pressed (GdkEventKey *event)
{

    bool shift = event->state & GDK_SHIFT_MASK;

    switch (event->keyval) {
    case GDK_KEY_Escape:

        // Clear Query if the Escape character is pressed within it
        if (!shift) {
            FileCatalog::buttonQueryClearPressed ();
            return true;
        }

        break;

    default:
        break;
    }

    return false;
}

void FileCatalog::updateFBQueryTB (bool /*singleRow*/)
{
    // Always single-row layout — nothing to toggle
}

void FileCatalog::updateFBToolBarVisibility (bool showFilmStripToolBar)
{
    if (showFilmStripToolBar) {
        showToolBar();
    } else {
        hideToolBar();
    }

    refreshHeight();
}

void FileCatalog::buttonBrowsePathPressed ()
{
    Glib::ustring BrowsePathValue = BrowsePath->get_text();
    Glib::ustring DecodedPathPrefix = "";
    Glib::ustring FirstChar;

    // handle shortcuts in the BrowsePath -- START
    // read the 1-st character from the path
    FirstChar = BrowsePathValue.substr (0, 1);

    if (FirstChar == "~") { // home directory
        DecodedPathPrefix = PlacesBrowser::userHomeDir ();
    } else if (FirstChar == "!") { // user's pictures directory
        DecodedPathPrefix = PlacesBrowser::userPicturesDir ();
    }

    if (!DecodedPathPrefix.empty()) {
        BrowsePathValue = Glib::ustring::compose ("%1%2", DecodedPathPrefix, BrowsePathValue.substr (1, BrowsePath->get_text_length() - 1));
        BrowsePath->set_text(BrowsePathValue);
    }

    // handle shortcuts in the BrowsePath -- END

    // validate the path
    if (Glib::file_test(BrowsePathValue, Glib::FILE_TEST_IS_DIR) && selectDir) {
        selectDir (BrowsePathValue);
    } else
        // error, likely path not found: show red arrow
    {
        buttonBrowsePath->set_image (*iRefreshRed);
    }
}

bool FileCatalog::BrowsePath_key_pressed (GdkEventKey *event)
{

    bool shift = event->state & GDK_SHIFT_MASK;

    switch (event->keyval) {
    case GDK_KEY_Escape:

        // On Escape character Reset BrowsePath to selectedDirectory
        if (!shift) {
            BrowsePath->set_text(selectedDirectory);
            // place cursor at the end
            BrowsePath->select_region(BrowsePath->get_text_length(), BrowsePath->get_text_length());
            return true;
        }

        break;

    default:
        break;
    }

    return false;
}

void FileCatalog::tbLeftPanel_1_visible (bool visible)
{
    if (visible) {
        tbLeftPanel_1->show();
        vSepiLeftPanel->show();
    } else {
        tbLeftPanel_1->hide();
        vSepiLeftPanel->hide();
    }
}
void FileCatalog::tbRightPanel_1_visible (bool visible)
{
    if (visible) {
        tbRightPanel_1->show();
    } else {
        tbRightPanel_1->hide();
    }
}
void FileCatalog::tbLeftPanel_1_toggled ()
{
    removeIfThere (filepanel->dirpaned, filepanel->placespaned, false);

    auto& options = App::get().mut_options();
    if (tbLeftPanel_1->get_active()) {
        filepanel->dirpaned->pack1 (*filepanel->placespaned, false, true);
        tbLeftPanel_1->set_image (*iLeftPanel_1_Hide);
        options.browserDirPanelOpened = true;
    } else {
        tbLeftPanel_1->set_image (*iLeftPanel_1_Show);
        options.browserDirPanelOpened = false;
    }
}

void FileCatalog::tbRightPanel_1_toggled ()
{
    auto& options = App::get().mut_options();
    if (tbRightPanel_1->get_active()) {
        filepanel->rightBox->show();
        tbRightPanel_1->set_image (*iRightPanel_1_Hide);
        options.browserToolPanelOpened = true;
    } else {
        filepanel->rightBox->hide();
        tbRightPanel_1->set_image (*iRightPanel_1_Show);
        options.browserToolPanelOpened = false;
    }
}

bool FileCatalog::CheckSidePanelsVisibility()
{
    return tbLeftPanel_1->get_active() || tbRightPanel_1->get_active();
}

void FileCatalog::toggleSidePanels()
{
    // toggle left AND right panels

    bool bAllSidePanelsVisible;
    bAllSidePanelsVisible = CheckSidePanelsVisibility();

    tbLeftPanel_1->set_active (!bAllSidePanelsVisible);
    tbRightPanel_1->set_active (!bAllSidePanelsVisible);
}

void FileCatalog::toggleLeftPanel()
{
    tbLeftPanel_1->set_active (!tbLeftPanel_1->get_active());
}

void FileCatalog::toggleRightPanel()
{
    tbRightPanel_1->set_active (!tbRightPanel_1->get_active());
}


void FileCatalog::selectImage (Glib::ustring fname, bool clearFilters)
{
    if (clearFilters) { // clear all filters
        Query->set_text("");
        categoryButtonToggled(bFilterClear, false);

        // disable exif filters
        if (filterPanel->isEnabled()) {
            filterPanel->setEnabled (false);
        }
    }

    if (restoreResetState()) {
        // Directory was changed -
        // If the user has traversed around directories in the File Browser and now wants to
        // reset the file catalog to the directory of the opened image with X or Y key
        //
        // the actual selection of image will be handled asynchronously at the end of FileCatalog::previewsFinishedUI
        imageToSelect_fname = fname;
        imageToSelect_key = catalogPathKey(fname);
    } else {
        fileBrowser->selectImage(fname);
        imageToSelect_fname = "";
        imageToSelect_key.clear();
    }
}


void FileCatalog::openNextPreviousEditorImage (Glib::ustring fname, eRTNav nextPrevious)
{
    if (restoreResetState()) {
        // Directory was changed -
        // If the user has traversed around directories in the File Browser and now wants to
        // continue from the image opened in the editor with SHIFT+F3/F4 keys
        refImageForOpen_fname = fname;
        actionNextPrevious = nextPrevious;
    } else {
        fileBrowser->openNextPreviousEditorImage(fname, nextPrevious);
        refImageForOpen_fname = "";
        actionNextPrevious = NAV_NONE;
    }
}

bool FileCatalog::handleShortcutKey (GdkEventKey* event)
{

    bool ctrl = event->state & GDK_CONTROL_MASK;
    bool shift = event->state & GDK_SHIFT_MASK;
    bool alt = event->state & GDK_MOD1_MASK;
#ifdef __WIN32__
    bool altgr = event->state & GDK_MOD2_MASK;
#else
    bool altgr = event->state & GDK_MOD5_MASK;
#endif
    modifierKey = event->state;

    // GUI Layout
    switch(event->keyval) {
    case GDK_KEY_l:
        if (!alt) {
            tbLeftPanel_1->set_active (!tbLeftPanel_1->get_active());    // toggle left panel
        }

        if (alt && !ctrl) {
            tbRightPanel_1->set_active (!tbRightPanel_1->get_active());    // toggle right panel
        }

        if (alt && ctrl) {
            tbLeftPanel_1->set_active (!tbLeftPanel_1->get_active()); // toggle left panel
            tbRightPanel_1->set_active (!tbRightPanel_1->get_active()); // toggle right panel
        }

        return true;

    case GDK_KEY_m:
        if (!ctrl && !alt) {
            toggleSidePanels();
        }

        return true;
    }

    if (shift) {
        switch(event->keyval) {
        case GDK_KEY_Escape:
            BrowsePath->set_text(selectedDirectory);
            // set focus on something neutral, this is useful to remove focus from BrowsePath and Query
            // when need to execute a shortcut, which otherwise will be typed into those fields
            filepanel->grab_focus();
            return true;
        }
    }

#ifdef __WIN32__

    if (!alt && shift && !altgr) {
        switch(event->hardware_keycode) {
        case 0x30:
            categoryButtonToggled(bUnRanked, false);
            return true;

        case 0x31:
            categoryButtonToggled(bRank[0], false);
            return true;

        case 0x32:
            categoryButtonToggled(bRank[1], false);
            return true;

        case 0x33:
            categoryButtonToggled(bRank[2], false);
            return true;

        case 0x34:
            categoryButtonToggled(bRank[3], false);
            return true;

        case 0x35:
            categoryButtonToggled(bRank[4], false);
            return true;

        case 0x36:
            categoryButtonToggled(bEdited[0], false);
            return true;

        case 0x37:
            categoryButtonToggled(bEdited[1], false);
            return true;
        }
    }

    if (!alt && !shift) {
        switch(event->keyval) {

        case GDK_KEY_Return:
        case GDK_KEY_KP_Enter:
            if (BrowsePath->is_focus()) {
                FileCatalog::buttonBrowsePathPressed ();
                return true;
            }

            break;
        }
    }

    if (alt && !shift) { // shift is reserved for color labeling
        switch(event->hardware_keycode) {
        case 0x30:
            categoryButtonToggled(bUnCLabeled, false);
            return true;

        case 0x31:
            categoryButtonToggled(bCLabel[0], false);
            return true;

        case 0x32:
            categoryButtonToggled(bCLabel[1], false);
            return true;

        case 0x33:
            categoryButtonToggled(bCLabel[2], false);
            return true;

        case 0x34:
            categoryButtonToggled(bCLabel[3], false);
            return true;

        case 0x35:
            categoryButtonToggled(bCLabel[4], false);
            return true;

        case 0x36:
            categoryButtonToggled(bRecentlySaved[0], false);
            return true;

        case 0x37:
            categoryButtonToggled(bRecentlySaved[1], false);
            return true;
        }
    }

#else

    if (!alt && shift && !altgr) {
        switch(event->hardware_keycode) {
        case 0x13:
            categoryButtonToggled(bUnRanked, false);
            return true;

        case 0x0a:
            categoryButtonToggled(bRank[0], false);
            return true;

        case 0x0b:
            categoryButtonToggled(bRank[1], false);
            return true;

        case 0x0c:
            categoryButtonToggled(bRank[2], false);
            return true;

        case 0x0d:
            categoryButtonToggled(bRank[3], false);
            return true;

        case 0x0e:
            categoryButtonToggled(bRank[4], false);
            return true;

        case 0x0f:
            categoryButtonToggled(bEdited[0], false);
            return true;

        case 0x10:
            categoryButtonToggled(bEdited[1], false);
            return true;
        }
    }

    if (!alt && !shift) {
        switch(event->keyval) {

        case GDK_KEY_Return:
        case GDK_KEY_KP_Enter:
            if (BrowsePath->is_focus()) {
                FileCatalog::buttonBrowsePathPressed ();
                return true;
            }

            break;
        }
    }

    if (alt && !shift) { // shift is reserved for color labeling
        switch(event->hardware_keycode) {
        case 0x13:
            categoryButtonToggled(bUnCLabeled, false);
            return true;

        case 0x0a:
            categoryButtonToggled(bCLabel[0], false);
            return true;

        case 0x0b:
            categoryButtonToggled(bCLabel[1], false);
            return true;

        case 0x0c:
            categoryButtonToggled(bCLabel[2], false);
            return true;

        case 0x0d:
            categoryButtonToggled(bCLabel[3], false);
            return true;

        case 0x0e:
            categoryButtonToggled(bCLabel[4], false);
            return true;

        case 0x0f:
            categoryButtonToggled(bRecentlySaved[0], false);
            return true;

        case 0x10:
            categoryButtonToggled(bRecentlySaved[1], false);
            return true;
        }
    }

#endif

    if (!ctrl && !alt) {
        switch(event->keyval) {
        case GDK_KEY_d:
        case GDK_KEY_D:
            categoryButtonToggled(bFilterClear, false);
            return true;
        }
    }

    auto& options = App::get().mut_options();
    if (!ctrl || (alt && !options.tabbedUI)) {
        switch(event->keyval) {

        case GDK_KEY_bracketright:
            coarsePanel->rotateRight();
            return true;

        case GDK_KEY_bracketleft:
            coarsePanel->rotateLeft();
            return true;

        case GDK_KEY_i:
        case GDK_KEY_I:
            exifInfo->set_active (!exifInfo->get_active());
            return true;

        case GDK_KEY_plus:
        case GDK_KEY_equal:
            zoomIn();
            return true;

        case GDK_KEY_minus:
        case GDK_KEY_underscore:
            zoomOut();
            return true;
        default: // do nothing, avoids a cppcheck false positive
            break;
        }
    }

    if (ctrl && !alt) {
        switch (event->keyval) {
        case GDK_KEY_o:
            BrowsePath->select_region(0, BrowsePath->get_text_length());
            BrowsePath->grab_focus();
            return true;

        case GDK_KEY_f:
            Query->select_region(0, Query->get_text_length());
            Query->grab_focus();
            return true;

        case GDK_KEY_t:
        case GDK_KEY_T:
            modifierKey = 0; // HOMBRE: yet another hack.... otherwise the shortcut won't work
            categoryButtonToggled(bTrash, false);
            return true;
        }
    }

    if (!ctrl && !alt && shift) {
        switch (event->keyval) {
        case GDK_KEY_t:
        case GDK_KEY_T:
            if (inTabMode) {
                if (options.showFilmStripToolBar) {
                    hideToolBar();
                } else {
                    showToolBar();
                }

                options.showFilmStripToolBar = !options.showFilmStripToolBar;
            }

            return true;
        }
    }

    if (!ctrl && !alt && !shift) {
        switch (event->keyval) {
        case GDK_KEY_t:
        case GDK_KEY_T:
            if (inTabMode) {
                if (options.showFilmStripToolBar) {
                    hideToolBar();
                } else {
                    showToolBar();
                }

                options.showFilmStripToolBar = !options.showFilmStripToolBar;
            }

            refreshHeight();
            return true;
        }
    }

    if (!ctrl && !alt) {
        switch (event->keyval) {
        case GDK_KEY_f:
            fileBrowser->getInspector()->showWindow(false, true);
            return true;
        case GDK_KEY_F:
            fileBrowser->getInspector()->showWindow(false, false);
            return true;
        }
    }

    return fileBrowser->keyPressed(event);
}

bool FileCatalog::handleShortcutKeyRelease(GdkEventKey* event)
{
    bool ctrl = event->state & GDK_CONTROL_MASK;
    bool alt = event->state & GDK_MOD1_MASK;

    if (!ctrl && !alt) {
        switch (event->keyval) {
        case GDK_KEY_f:
        case GDK_KEY_F:
            fileBrowser->getInspector()->hideWindow();
            return true;
        }
    }

    return false;
}

void FileCatalog::showToolBar()
{
    if (inTabMode) stb_->show();
    buttonBar->show();
}

void FileCatalog::hideToolBar()
{
    buttonBar->hide();
    if (inTabMode) stb_->hide();
}
