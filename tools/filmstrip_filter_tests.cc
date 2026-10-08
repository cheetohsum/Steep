// Integration test against the real catalog, asynchronous preview loader and
// editor filter controls. Run with isolated RT_SETTINGS/RT_CACHE and test photos.
#include <gtkmm.h>
#include <giomm.h>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include "../rtgui/editorpanel.h"
#include "../rtgui/filecatalog.h"
#include "../rtgui/filepanel.h"
#include "../rtgui/filebrowser.h"
#include "../rtgui/thumbbrowserentrybase.h"
#include "../rtgui/cachemanager.h"
#include "../rtgui/extprog.h"
#include "../rtgui/soundman.h"
#include "../rtgui/options.h"
#include "../rtgui/thumbnail.h"
#include "../rtgui/windows/rtwindow.h"

class FilmstripFilterTests {
    static void require(bool pass, const char* message) {
        if (!pass) throw std::runtime_error(message);
    }
    static void pump() {
        for (int i = 0; i < 20 && Gtk::Main::events_pending(); ++i) Gtk::Main::iteration(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    static void expect(FileCatalog& catalog, std::set<std::string> names, const char* message) {
        std::set<std::string> actual;
        for (auto* entry : catalog.fileBrowser->getEntries()) {
            if (!entry->filtered) actual.insert(Glib::path_get_basename(entry->filename));
        }
        if (actual != names) {
            std::cerr << message << ":";
            for (const auto& name : actual) std::cerr << ' ' << name;
            std::cerr << '\n';
            for (auto* entry : catalog.fileBrowser->getEntries()) {
                const auto* data = entry->thumbnail->getCacheImageData();
                std::cerr << entry->filename << " rank=" << entry->thumbnail->getRank()
                          << " pick=" << entry->thumbnail->getPick() << " type=" << data->getFiletypeUpper()
                          << " tags=" << PhotoTags::instance().tags(entry->filename).size() << '\n';
            }
        }
        require(actual == names, message);
        require(catalog.fileBrowser->getNumFiltered() == static_cast<int>(names.size()), "Visible count disagrees with filmstrip");
    }
public:
    static void run(RTWindow& window, const Glib::ustring& fixture) {
        auto& catalog = *window.fpanel->fileCatalog;
        auto& editor = *window.epanel;
        Glib::ustring error;
        const auto a = Gio::File::create_for_path(Glib::build_filename(fixture, "A.jpg"))->get_path();
        const auto b = Gio::File::create_for_path(Glib::build_filename(fixture, "B.jpg"))->get_path();
        const auto c = Gio::File::create_for_path(Glib::build_filename(fixture, "C.png"))->get_path();
        require(PhotoTags::instance().edit({a,b,c}, {}, {}, {{"Red", "Test"},{"Blue", "Test"}}, error,
            {{a,{"red"}},{b,{"blue"}},{c,{"red","blue"}}}), "Cannot seed fixture tags");
        for (const auto& path : {a,b,c}) {
            auto* thumbnail = cacheMgr->getEntry(path);
            require(thumbnail != nullptr, "Cannot read test photo");
            thumbnail->createProcParamsForUpdate(false, false, true);
            thumbnail->setRank(path == b ? 1 : 5);
            thumbnail->setPick(path == b ? 0 : 1);
            thumbnail->updateCache();
            thumbnail->decreaseRef();
        }
        catalog.Query->set_text("B.jpg"); // Deliberately different browser filter.
        catalog.enableTabMode(true);
        editor.filterBarClearAll();
        editor.fbRank[4]->set_active(true);
        editor.fbPicked->set_active(true);
        editor.editorPhotoTagFilter_->setSelection({"red"}, false, false);
        catalog.setSelectedFiletypes({"JPG"});
        editor.restoreEditorFilter();
        catalog.dirSelected(fixture, "");
        require(catalog.getFilter().tagFilter == PhotoTags::Tags({"red"}), "Loading replaced editor tags");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (catalog.previewsToLoad != 0 || !catalog.directoryScanComplete_
               || catalog.fileBrowser->getEntries().size() != 3) {
            if (std::chrono::steady_clock::now() >= deadline) {
                std::cerr << "load: pending=" << catalog.previewsToLoad << " loaded=" << catalog.previewsLoaded
                          << " entries=" << catalog.fileBrowser->getEntries().size()
                          << " scan=" << catalog.directoryScanComplete_ << '\n';
            }
            require(std::chrono::steady_clock::now() < deadline, "Fixture loading timed out");
            pump();
        }
        expect(catalog, {"A.jpg"}, "Loading lost combined rating/flag/tag/filetype filter");
        require(catalog.getFilter().vFilterStrings.empty(), "Hidden browser search leaked into editor");
        editor.filterBarClearAll();
        editor.editorPhotoTagFilter_->setSelection({"blue"}, false, false);
        expect(catalog, {"B.jpg","C.png"}, "Changing tags did not reveal loaded matches");
        editor.fbSearchEntry->set_text("NO-MATCH");
        editor.applyEditorFilter();
        catalog.previewsFinishedUI(catalog.selectedDirectoryId.load());
        expect(catalog, {}, "Completion replaced a zero-result editor filter");
        editor.fbSearchEntry->set_text("");
        editor.applyEditorFilter();
        catalog.filterChanged(); // Late metadata/filetype/browser-widget notification.
        expect(catalog, {"B.jpg","C.png"}, "Late filter notification replaced editor filter");
        const auto adjacent = catalog.fileBrowser->getAdjacentEntries(b, 1);
        require(adjacent.size() == 1 && adjacent.front().fname == c, "Preloading did not follow filtered neighbors");
        catalog.enableTabMode(false);
        catalog.reapplyBrowserFilter();
        expect(catalog, {"B.jpg"}, "Browser filter was not restored");
        catalog.enableTabMode(true);
        editor.restoreEditorFilter();
        expect(catalog, {"B.jpg","C.png"}, "Editor filter was not restored");
        editor.filterBarClearAll();
        expect(catalog, {"A.jpg","B.jpg","C.png"}, "Clearing filters did not restore the filmstrip");
        catalog.closeDir();
        std::cout << "Filmstrip filters: progressive loading, rating/flag/tag/filetype combination, zero results, clearing, filtered neighbors and tab restoration passed\n";
    }
};

int main(int argc, char** argv) {
    if (argc != 3 || !g_getenv("RT_SETTINGS") || !g_getenv("RT_CACHE")) return 2;
    try {
        Glib::init(); Gio::init();
        Gtk::Main gtk(argc, argv);
        App::get().setArgv0(argv[1]);
        Options::load();
        auto& options = App::get().mut_options();
        options.tabbedUI = false;
        options.browserHideRejects = false;
        options.startupDir = STARTUPDIR_CUSTOM;
        options.startupPath = argv[2];
        options.version = App::VERSION;
        extProgStore->init();
        SoundManager::init();
        Gtk::IconTheme::get_default()->append_search_path(Glib::build_filename(App::get().argv0(), "icons"));
        auto window = std::unique_ptr<RTWindow>(new RTWindow()); // Never presented.
        window->setWindowSize();
        FilmstripFilterTests::run(*window, argv[2]);
        window.reset();
        rtengine::cleanup();
    } catch (const std::exception& error) {
        std::cerr << "Filmstrip test failed: " << error.what() << '\n';
        return 1;
    }
}
