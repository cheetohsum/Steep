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

#ifdef __GNUC__
#if defined(__FAST_MATH__)
#error Using the -ffast-math CFLAG is known to lead to problems. Disable it to compile RawTherapee.
#endif
#endif

#include <gtkmm.h>
#include <giomm.h>
#include <iostream>
#include <tiffio.h>
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <system_error>
#include <typeinfo>
#include <locale.h>
#include <lensfun.h>

#include "autoedit.h"
#include "cachemanager.h"
#include "config.h"
#include "editorpanel.h"
#include "extprog.h"
#include "filecatalog.h"
#include "filepanel.h"
#include "options.h"
#include "pathutils.h"
#include "rtimage.h"
#include "soundman.h"
#include "windows/rtwindow.h"
#include "version.h"

#include "rtengine/dynamicprofile.h"
#include "rtengine/procparams.h"

#ifndef _WIN32
#include <glibmm/fileutils.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <glibmm/threads.h>
#else
#include "rtengine/leanwindows.h"
#include <conio.h>
#include <windows.h>
#include <mmsystem.h>

#include <glibmm/thread.h>
#endif

// Set this to 1 to make RT work when started with Eclipse and arguments, at least on Windows platform
#define ECLIPSE_ARGS 0

// stores path to data files
Glib::ustring argv2;

namespace {

// This recursive mutex will be used by gdk_threads_enter/leave instead of a simple mutex
static Glib::Threads::RecMutex myGdkRecMutex;

static void myGdkLockEnter()
{
    myGdkRecMutex.lock();
}
static void myGdkLockLeave()
{
    // Automatic gdk_flush for non main thread
#if AUTO_GDK_FLUSH
    //if (Glib::Thread::self() != mainThread) {
    //    gdk_flush();
    //}

#endif
    myGdkRecMutex.unlock();
}


/* Process line command options
 * Returns
 *  0 if process in batch has executed
 *  1 to start GUI (with a dir or file option)
 *  2 to start GUI because no files found
 *  -1 if there is an error in parameters
 *  -2 if an error occurred during processing
 *  -3 if at least one required procparam file was not found */
//int processLineParams ( int argc, char **argv );
int processLineParams ( int argc, char **argv )
{
    auto& app = App::get();

    int ret = 1;
    for ( int iArg = 1; iArg < argc; iArg++) {
        Glib::ustring currParam (argv[iArg]);
        if ( currParam.empty() ) {
            continue;
        }
#if ECLIPSE_ARGS
        currParam = currParam.substr (1, currParam.length() - 2);
#endif

        if ( currParam.at (0) == '-' && currParam.size() > 1 ) {
            switch ( currParam.at (1) ) {
                case '-':
                    // GTK --argument, we're skipping it
                    break;

#ifdef _WIN32

                case 'w': // This case is handled outside this function
                    break;
#endif

                case 'v':
                    printf("Steep, version %s\n", RTVERSION);
                    ret = 0;
                    break;

#ifndef __APPLE__ // TODO agriggio - there seems to be already some "single instance app" support for OSX in rtwindow. Disabling it here until I understand how to merge the two

                case 'R':
                    if (!app.isGimpPlugin()) {
                        app.setIsRemote(true);
                    }

                    break;
#endif

                case 'g':
                    if (currParam == "-gimp") {
                        app.setIsGimpPlugin(true);
                        app.setIsSimpleEditor(true);
                        app.setIsRemote(false);
                        break;
                    }

                // fall through

                case 'h':
                case '?':
                default: {
                    printf("  An advanced, cross-platform program for developing raw photos.\n\n");
                    printf("  Website: http://www.rawtherapee.com/\n");
                    printf("  Documentation: http://rawpedia.rawtherapee.com/\n");
                    printf("  Forum: https://discuss.pixls.us/c/software/rawtherapee\n");
                    printf("  Code and bug reports: https://github.com/RawTherapee/RawTherapee\n\n");
                    printf("Symbols:\n");
                    printf("  <Chevrons> indicate parameters you can change.\n\n");
                    printf("Usage:\n");
                    printf("  %s <folder>           Start File Browser inside folder.\n",Glib::path_get_basename (argv[0]).c_str());
                    printf("  %s <file>             Start Image Editor with file.\n\n",Glib::path_get_basename (argv[0]).c_str());
                    std::cout << std::endl;
                    printf("Options:\n");
#ifdef _WIN32
                    printf("  -w Do not open the Windows console\n");
#endif
                    printf("  -v Print RawTherapee version number and exit\n");
#ifndef __APPLE__
                    printf("  -R Raise an already running RawTherapee instance (if available)\n");
#endif
                    printf("  -h -? Display this help message\n");

                    ret = -1;
                    break;
                }
            }
        } else {
            if (app.argv1().empty()) {
                app.setArgv1(Glib::ustring (fname_to_utf8 (argv[iArg])));
#if ECLIPSE_ARGS
                app.setArgv1(app.argv1().substr (1, app.argv1().length() - 2));
#endif
            } else if (app.isGimpPlugin()) {
                argv2 = Glib::ustring (fname_to_utf8 (argv[iArg]));
                break;
            }

            if (!app.isGimpPlugin()) {
                break;
            }
        }
    }

    return ret;
}


bool init_rt()
{
    extProgStore->init();
    SoundManager::init();

    if (!rtengine::settings->verbose) {
        TIFFSetWarningHandler (nullptr);   // avoid annoying message boxes
    }

#ifndef _WIN32

    const auto& options = App::get().options();
    // Move the old path to the new one if the new does not exist
    if (Glib::file_test (Glib::build_filename (options.rtdir, "cache"), Glib::FILE_TEST_IS_DIR) && !Glib::file_test (options.cacheBaseDir, Glib::FILE_TEST_IS_DIR)) {
        g_rename (Glib::build_filename (options.rtdir, "cache").c_str (), options.cacheBaseDir.c_str ());
    }

#endif

    return true;
}


void cleanup_rt()
{
    rtengine::cleanup();
}


RTWindow *create_rt_window()
{
    Glib::ustring icon_path = Glib::build_filename (App::get().argv0(), "icons");
    Glib::RefPtr<Gtk::IconTheme> defaultIconTheme = Gtk::IconTheme::get_default();
    defaultIconTheme->append_search_path (icon_path);

    //gdk_threads_enter ();
    RTWindow *rtWindow = new RTWindow();
    rtWindow->setWindowSize(); // Need to be called after RTWindow creation to work with all OS Windows Manager
    return rtWindow;
}


class RTApplication: public Gtk::Application
{
public:
    RTApplication():
        Gtk::Application ("com.rawtherapee.application",
                          Gio::APPLICATION_HANDLES_OPEN),
        rtWindow (nullptr)
    {
    }

    ~RTApplication() override
    {
        if (rtWindow) {
            delete rtWindow;
        }

        cleanup_rt();
    }

private:
    bool create_window()
    {
        if (rtWindow) {
            return true;
        }

        if (!init_rt()) {
            Gtk::MessageDialog msgd ("Fatal error!\nThe RT_SETTINGS and/or RT_PATH environment variables are set, but use a relative path. The path must be absolute!", true, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
            add_window (msgd);
            msgd.run ();
            return false;
        } else {
            rtWindow = create_rt_window();
            add_window (*rtWindow);
            return true;
        }
    }

    // Override default signal handlers:
    void on_activate() override
    {
        if (create_window()) {
            rtWindow->present();
            // Debug rig, inert unless STEEP_AUTOEDIT_SELFTEST is set.
            runSteepAutoEditSelfTest();
        }
    }

    void on_open (const Gio::Application::type_vec_files& files,
                  const Glib::ustring& hint) override
    {
        if (create_window()) {
            struct Data {
                std::vector<Thumbnail *> entries;
                Glib::ustring lastfilename;
                FileCatalog *filecatalog;
            };
            Data *d = new Data;
            d->filecatalog = rtWindow->fpanel->fileCatalog;

            for (const auto &f : files) {
                Thumbnail *thm = cacheMgr->getEntry (f->get_path());

                if (thm) {
                    d->entries.push_back (thm);
                    d->lastfilename = f->get_path();
                }
            }

            if (!d->entries.empty()) {
                const auto doit =
                [] (gpointer data) -> gboolean {
                    Data *d = static_cast<Data *> (data);
                    d->filecatalog->openRequested (d->entries);
                    delete d;
                    return FALSE;
                };
                gdk_threads_add_idle (doit, d);
            } else {
                delete d;
            }

            rtWindow->present();
        }
    }

private:
    RTWindow *rtWindow;
};

void show_gimp_plugin_info_dialog(Gtk::Window *parent)
{
    auto& options = App::get().mut_options();
    if (options.gimpPluginShowInfoDialog) {
        Gtk::MessageDialog info(*parent, M("GIMP_PLUGIN_INFO"), false, Gtk::MESSAGE_INFO, Gtk::BUTTONS_OK, true);
        Gtk::Box *box = info.get_message_area();
        Gtk::CheckButton dontshowagain(M("DONT_SHOW_AGAIN"));
        dontshowagain.show();
        box->pack_start(dontshowagain);
        info.run();
        options.gimpPluginShowInfoDialog = !dontshowagain.get_active();
    }
}

} // namespace


// An uncaught C++ exception ends in terminate()/abort(), which does NOT pass
// through SetUnhandledExceptionFilter — those crashes previously left nothing
// but libstdc++'s one-line message. Log the exception and a stack trace so the
// throwing site can be identified.
static void steepTerminateHandler()
{
    static std::atomic<bool> reentered{false};

    if (reentered.exchange(true)) {
        std::abort();
    }

    fprintf(stderr, "\n=== TERMINATE (uncaught exception) ===\n");

    if (auto current = std::current_exception()) {
        try {
            std::rethrow_exception(current);
        } catch (const std::system_error& e) {
            fprintf(stderr, "std::system_error: %s (code=%d category=%s)\n",
                    e.what(), e.code().value(), e.code().category().name());
        } catch (const std::exception& e) {
            fprintf(stderr, "%s: %s\n", typeid(e).name(), e.what());
        } catch (...) {
            fprintf(stderr, "non-standard exception\n");
        }
    } else {
        fprintf(stderr, "no active exception\n");
    }

#ifdef _WIN32
    fprintf(stderr, "Stack trace:\n");
    void* stack[64];
    USHORT frames = CaptureStackBackTrace(0, 64, stack, NULL);

    for (USHORT i = 0; i < frames; i++) {
        HMODULE hMod = NULL;
        char modName[MAX_PATH] = "???";

        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)stack[i], &hMod)) {
            GetModuleFileNameA(hMod, modName, MAX_PATH);
            char* slash = strrchr(modName, '\\');
            char* name = slash ? slash + 1 : modName;
            fprintf(stderr, "  [%2d] %p (%s+0x%llx)\n", i, stack[i], name,
                    (unsigned long long)((char*)stack[i] - (char*)hMod));
        } else {
            fprintf(stderr, "  [%2d] %p\n", i, stack[i]);
        }
    }
#endif

    fflush(stderr);
    std::abort();
}

#ifdef _WIN32
static LONG WINAPI crashHandler(EXCEPTION_POINTERS* ep)
{
    fprintf(stderr, "\n=== CRASH ===\n");
    fprintf(stderr, "Exception code: 0x%08lX\n", ep->ExceptionRecord->ExceptionCode);
    fprintf(stderr, "Exception addr: %p\n", ep->ExceptionRecord->ExceptionAddress);
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        fprintf(stderr, "Access violation %s address %p\n",
            ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
            (void*)ep->ExceptionRecord->ExceptionInformation[1]);
    }
#if defined(_M_ARM64) || defined(__aarch64__)
    fprintf(stderr, "PC=%p SP=%p\n", (void*)ep->ContextRecord->Pc, (void*)ep->ContextRecord->Sp);
#elif defined(_M_X64) || defined(__x86_64__)
    fprintf(stderr, "RIP=%p RSP=%p\n", (void*)ep->ContextRecord->Rip, (void*)ep->ContextRecord->Rsp);
#endif

    // Walk the stack with module names
    fprintf(stderr, "Stack trace:\n");
    void* stack[64];
    USHORT frames = CaptureStackBackTrace(0, 64, stack, NULL);
    for (USHORT i = 0; i < frames; i++) {
        HMODULE hMod = NULL;
        char modName[MAX_PATH] = "???";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)stack[i], &hMod)) {
            GetModuleFileNameA(hMod, modName, MAX_PATH);
            // Just print the filename part
            char* slash = strrchr(modName, '\\');
            char* name = slash ? slash + 1 : modName;
            fprintf(stderr, "  [%2d] %p (%s+0x%llx)\n", i, stack[i], name,
                    (unsigned long long)((char*)stack[i] - (char*)hMod));
        } else {
            fprintf(stderr, "  [%2d] %p\n", i, stack[i]);
        }
    }

    // Also resolve the crash address itself
    {
        HMODULE hMod = NULL;
        char modName[MAX_PATH] = "???";
        void* crashAddr = ep->ExceptionRecord->ExceptionAddress;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)crashAddr, &hMod)) {
            GetModuleFileNameA(hMod, modName, MAX_PATH);
            char* slash = strrchr(modName, '\\');
            char* name = slash ? slash + 1 : modName;
            fprintf(stderr, "Crash in: %s+0x%llx\n", name,
                    (unsigned long long)((char*)crashAddr - (char*)hMod));
        }
    }

    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

int main (int argc, char **argv)
{
    std::set_terminate(steepTerminateHandler);
#ifdef _WIN32
    SetUnhandledExceptionFilter(crashHandler);

    // Windows schedules timers on a ~15.6ms tick unless a process asks for
    // finer resolution, and GLib's main loop inherits that. Every interactive
    // timeout is rounded up to the next tick, which put a hard floor under the
    // slider debounce -- a 20ms delay actually fired at ~31ms, so dragging
    // could not update faster than ~24 times a second no matter how quick the
    // render was. Released in the RAII guard below at exit.
    struct TimerResolutionGuard {
        bool raised;
        TimerResolutionGuard() : raised(timeBeginPeriod(1) == TIMERR_NOERROR) {}
        ~TimerResolutionGuard()
        {
            if (raised) {
                timeEndPeriod(1);
            }
        }
    } timerResolutionGuard;
#endif
    setlocale (LC_ALL, "");
    setlocale (LC_NUMERIC, "C"); // to set decimal point to "."

    Glib::init();  // called by Gtk::Main, but this may be important for thread handling, so we call it ourselves now
    Gio::init ();

#ifdef _WIN32
    if (GetFileType (GetStdHandle (STD_OUTPUT_HANDLE)) == 0x0003) {
        // started from msys2 console => do not buffer stdout
        setbuf(stdout, NULL);
    }
#endif

    auto& app = App::get();

#ifdef BUILD_BUNDLE
    char exname[512] = {0};
    Glib::ustring exePath;
    // get the path where the rawtherapee executable is stored
#ifdef _WIN32
    WCHAR exnameU[512] = {0};
    GetModuleFileNameW (NULL, exnameU, 511);
    WideCharToMultiByte (CP_UTF8, 0, exnameU, -1, exname, 511, 0, 0 );
#else

    if (readlink ("/proc/self/exe", exname, 511) < 0) {
        strncpy (exname, argv[0], 511);
    }

#endif // _WIN32
    exePath = Glib::path_get_dirname (exname);

    // set paths
    if (Glib::path_is_absolute (DATA_SEARCH_PATH)) {
        app.setArgv0(DATA_SEARCH_PATH);
    } else {
        app.setArgv0(Glib::build_filename(exePath, DATA_SEARCH_PATH));
    }

    if (Glib::path_is_absolute (CREDITS_SEARCH_PATH)) {
        app.setCreditsPath(CREDITS_SEARCH_PATH);
    } else {
        app.setCreditsPath(Glib::build_filename(exePath, CREDITS_SEARCH_PATH));
    }

    if (Glib::path_is_absolute (LICENCE_SEARCH_PATH)) {
        app.setLicensePath(LICENCE_SEARCH_PATH);
    } else {
        app.setLicensePath(Glib::build_filename(exePath, LICENCE_SEARCH_PATH));
    }
#else
    app.setArgv0(DATA_SEARCH_PATH);
    app.setCreditsPath(CREDITS_SEARCH_PATH);
    app.setLicensePath(LICENCE_SEARCH_PATH);
#endif // BUILD_BUNDLE

    Glib::ustring lensfunDbPath = LENSFUN_DB_PATH;
#ifdef BUILD_BUNDLE
    if (!lensfunDbPath.empty() && !Glib::path_is_absolute(lensfunDbPath)) {
        lensfunDbPath = Glib::build_filename(exePath, lensfunDbPath);
    }
#endif
    app.mut_options().rtSettings.lensfunDbDirectory = lensfunDbPath;
    app.mut_options().rtSettings.lensfunDbBundleDirectory = lensfunDbPath;

#ifdef _WIN32
    bool consoleOpened = false;

    // suppression of annoying error boxes
    SetErrorMode (SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    if (argc > 1) {
        if (!app.isRemote() && !Glib::file_test (app.argv1(), Glib::FILE_TEST_EXISTS ) && !Glib::file_test (app.argv1(), Glib::FILE_TEST_IS_DIR)) {
            const bool stdoutRedirecttoConsole = (GetFileType (GetStdHandle (STD_OUTPUT_HANDLE)) == 0x0000);
            // open console, if stdout is invalid
            if (stdoutRedirecttoConsole) {
                // check if parameter -w was passed.
                // We have to do that in this step, because it controls whether to open a console to show the output of following steps
                bool Console = true;

                for (int i = 1; i < argc; i++)
                    if (!strcmp (argv[i], "-w") || !strcmp (argv[i], "-R") || !strcmp (argv[i], "-gimp")) {
                        Console = false;
                        break;
                    }

                if (Console && AllocConsole()) {
                    AttachConsole ( GetCurrentProcessId() ) ;
                    // Don't allow CTRL-C in console to terminate RT
                    SetConsoleCtrlHandler ( NULL, true );
                    // Set title of console
                    char consoletitle[128];
                    snprintf(consoletitle, sizeof(consoletitle), "Steep %s Console", RTVERSION);
                    SetConsoleTitle (consoletitle);
                    // increase size of screen buffer
                    COORD c;
                    c.X = 200;
                    c.Y = 1000;
                    SetConsoleScreenBufferSize ( GetStdHandle ( STD_OUTPUT_HANDLE ), c );
                    // Disable console-Cursor
                    CONSOLE_CURSOR_INFO cursorInfo;
                    cursorInfo.dwSize = 100;
                    cursorInfo.bVisible = false;
                    SetConsoleCursorInfo ( GetStdHandle ( STD_OUTPUT_HANDLE ), &cursorInfo );

                    // we also redirect stderr to console
                    freopen ( "CON", "w", stdout ) ;
                    freopen ( "CON", "w", stderr ) ;

                    freopen ( "CON", "r", stdin ) ;

                    consoleOpened = true;
                }
            }
        }
        int ret = processLineParams ( argc, argv);

        if ( ret <= 0 ) {
            fflush(stdout);
            if (consoleOpened) {
                printf ("Press any key to exit RawTherapee\n");
                FlushConsoleInputBuffer (GetStdHandle (STD_INPUT_HANDLE));
                getch();
            }

            return ret;
        }
    }

#else

    if (argc > 1) {
        int ret = processLineParams ( argc, argv);

        if ( ret <= 0 ) {
            return ret;
        }
    }

#endif

    Glib::ustring fatalError;

    try {
        Options::load();
    } catch (Options::Error &e) {
        fatalError = e.get_msg();
    }

    if (app.isGimpPlugin()) {
        if (!Glib::file_test (app.argv1(), Glib::FILE_TEST_EXISTS) || Glib::file_test (app.argv1(), Glib::FILE_TEST_IS_DIR)) {
            printf ("Error: argv1 doesn't exist\n");
            return 1;
        }

        if (argv2.empty()) {
            printf ("Error: -gimp requires two arguments\n");
            return 1;
        }
    } else if (!app.isRemote() && Glib::file_test(app.argv1(), Glib::FILE_TEST_EXISTS) && !Glib::file_test(app.argv1(), Glib::FILE_TEST_IS_DIR)) {
        app.setIsSimpleEditor(true);
    }

    int ret = 0;

    gdk_threads_set_lock_functions (G_CALLBACK (myGdkLockEnter), (G_CALLBACK (myGdkLockLeave)));
    gdk_threads_init();
    gtk_init (&argc, &argv);  // use the "--g-fatal-warnings" command line flag to make warnings fatal

    if (fatalError.empty() && app.isRemote()) {
        char *app_argv[2] = { const_cast<char *> (app.argv0().c_str()) };
        int app_argc = 1;

        if (!app.argv1().empty()) {
            app_argc = 2;
            app_argv[1] = const_cast<char *> (app.argv1().c_str());
        }

        RTApplication app;
        ret = app.run (app_argc, app_argv);
    } else {
        if (fatalError.empty() && init_rt()) {
            Gtk::Main m (&argc, &argv);
            gdk_threads_enter();
            const std::unique_ptr<RTWindow> rtWindow (create_rt_window());
            // Debug rig, inert unless STEEP_AUTOEDIT_SELFTEST is set. This is
            // the ordinary startup path — RTApplication::on_activate only runs
            // for the remote/single-instance one — so it has to be here too.
            runSteepAutoEditSelfTest();
            if (app.isGimpPlugin()) {
                show_gimp_plugin_info_dialog(rtWindow.get());
            }
            m.run (*rtWindow);
            gdk_threads_leave();

            if (app.isGimpPlugin() && rtWindow->epanel && rtWindow->epanel->isRealized()) {
                if (!rtWindow->epanel->saveImmediately(argv2, SaveFormat())) {
                    ret = -2;
                }
            }

            cleanup_rt();
        } else {
            Gtk::Main m (&argc, &argv);
            Gtk::MessageDialog msgd (Glib::ustring::compose("FATAL ERROR!\n\n%1", fatalError), true, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
            msgd.run ();
            ret = -2;
        }
    }

#ifdef _WIN32

    if (consoleOpened) {
        printf ("Press any key to exit RawTherapee\n");
        fflush(stdout);
        FlushConsoleInputBuffer (GetStdHandle (STD_INPUT_HANDLE));
        getch();
    }

#endif

    return ret;
}
