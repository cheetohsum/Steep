/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2004-2010 Gabor Horvath <hgabor@rawtherapee.com>
 *  Copyright (c) 2010 Oliver Duis <www.oliverduis.de>
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
#include "editorpanel.h"

#include <cmath>
#include <iostream>

#include "rtengine/array2D.h"
#include "rtengine/imagesource.h"
#include "rtengine/iccstore.h"
#include "batchqueue.h"
#include "batchqueueentry.h"
#include "soundman.h"
#include "rtimage.h"
#include "filepanel.h"
#include "guiutils.h"
#include "options.h"
#include "dirbrowser.h"
#include "navigator.h"
#include "previewwindow.h"
#include "progressconnector.h"
#include "procparamchangers.h"
#include "placesbrowser.h"
#include "recentbrowser.h"
#include "pathutils.h"
#include "cachemanager.h"
#include "thumbnail.h"
#include "toolpanelcoord.h"
#include "clipboard.h"
#include "paramsedited.h"

#include <thread>
#include "widgets/basic/popupbutton.h"
#include "windows/rtappchooserdialog.h"
#include "windows/rtwindow.h"
#include "mcp/mcpserver.h"

#ifdef _WIN32
#include "rtengine/leanwindows.h"
#include "rtengine/winutils.h"
#endif

using namespace rtengine::procparams;

using ScopeType = Options::ScopeType;

namespace
{

// Box that caps its natural width so overlay children don't expand endlessly
class FixedWidthBox : public Gtk::Box {
public:
    explicit FixedWidthBox(int width) : fixedWidth_(width) {
        set_orientation(Gtk::ORIENTATION_VERTICAL);
    }
    void get_preferred_width_vfunc(int& minimum_width, int& natural_width) const override {
        Gtk::Box::get_preferred_width_vfunc(minimum_width, natural_width);
        minimum_width = std::min(minimum_width, fixedWidth_);
        natural_width = fixedWidth_;
    }
private:
    int fixedWidth_;
};

void setprogressStrUI(double val, const Glib::ustring str, MyProgressBar* pProgress)
{
    if (!str.empty()) {
        pProgress->set_text(M(str));
    }

    if (val >= 0.0) {
        pProgress->set_fraction(val);
    }

    // Show when there's active progress, hide when done (fraction <= 0 or >= 1)
    double frac = pProgress->get_fraction();
    if (frac > 0.0 && frac < 1.0) {
        pProgress->show();
    } else if (frac <= 0.0 || frac >= 1.0) {
        pProgress->hide();
    }
}

#if !defined(__APPLE__) // monitor profile not supported on apple
bool find_default_monitor_profile (GdkWindow *rootwin, Glib::ustring &defprof, Glib::ustring &defprofname)
{
#ifdef _WIN32
    HDC hDC = GetDC (nullptr);

    if (hDC != nullptr) {
        if (SetICMMode (hDC, ICM_ON)) {
            char profileName[MAX_PATH + 1];
            DWORD profileLength = MAX_PATH;

            if (GetICMProfileA (hDC, &profileLength, profileName)) {
                defprof = Glib::ustring (profileName);
                defprofname = Glib::path_get_basename (defprof);
                size_t pos = defprofname.rfind (".");

                if (pos != Glib::ustring::npos) {
                    defprofname = defprofname.substr (0, pos);
                }

                defprof = Glib::ustring ("file:") + defprof;
                return true;
            }

            // might fail if e.g. the monitor has no profile
        }

        ReleaseDC (NULL, hDC);
    }

#else
    // taken from geeqie (image.c) and adapted
    // Originally licensed as GPL v2+, with the following copyright:
    // * Copyright (C) 2006 John Ellis
    // * Copyright (C) 2008 - 2016 The Geeqie Team
    //
    guchar *prof = nullptr;
    gint proflen;
    GdkAtom type = GDK_NONE;
    gint format = 0;

    if (gdk_property_get (rootwin, gdk_atom_intern ("_ICC_PROFILE", FALSE), GDK_NONE, 0, 64 * 1024 * 1024, FALSE, &type, &format, &proflen, &prof) && proflen > 0) {
        cmsHPROFILE p = cmsOpenProfileFromMem (prof, proflen);

        if (p) {
            defprofname = "from GDK";
            defprof = Glib::build_filename (Options::rtdir, "GDK_ICC_PROFILE.icc");

            if (cmsSaveProfileToFile (p, defprof.c_str())) {
                cmsCloseProfile (p);

                if (prof) {
                    g_free (prof);
                }

                defprof = Glib::ustring ("file:") + defprof;
                return true;
            }
        }
    }

    if (prof) {
        g_free (prof);
    }

#endif
    return false;
}
#endif

bool hasUserOnlyPermission(const Glib::ustring &dirname)
{
#if defined(__linux__) || defined(__APPLE__)
    const Glib::RefPtr<Gio::File> file = Gio::File::create_for_path(dirname);
    const Glib::RefPtr<Gio::FileInfo> file_info = file->query_info("owner::user,unix::mode");

    if (!file_info) {
        return false;
    }

    const Glib::ustring owner = file_info->get_attribute_string("owner::user");
    const guint32 mode = file_info->get_attribute_uint32("unix::mode");

    return (mode & 0777) == 0700 && owner == Glib::get_user_name();
#elif defined(_WIN32)
    const Glib::RefPtr<Gio::File> file = Gio::File::create_for_path(dirname);
    const Glib::RefPtr<Gio::FileInfo> file_info = file->query_info("owner::user");
    if (!file_info) {
        return false;
    }

    // Current user must be the owner.
    const Glib::ustring user_name = Glib::get_user_name();
    const Glib::ustring owner = file_info->get_attribute_string("owner::user");
    if (user_name != owner) {
        return false;
    }

    // Get security descriptor and discretionary access control list.
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sec_desc_raw_ptr = nullptr;
    auto win_error = GetNamedSecurityInfo(
        dirname.c_str(),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        &dacl,
        nullptr,
        &sec_desc_raw_ptr
    );
    const WinLocalPtr<PSECURITY_DESCRIPTOR> sec_desc_ptr(sec_desc_raw_ptr);
    if (win_error != ERROR_SUCCESS) {
        return false;
    }

    // Must not inherit permissions.
    SECURITY_DESCRIPTOR_CONTROL sec_desc_control;
    DWORD revision;
    if (!(
        GetSecurityDescriptorControl(sec_desc_ptr, &sec_desc_control, &revision)
        && sec_desc_control & SE_DACL_PROTECTED
    )) {
        return false;
    }

    // Check that there is one entry allowing full access.
    ULONG acl_entry_count;
    PEXPLICIT_ACCESS acl_entry_list_raw = nullptr;
    win_error = GetExplicitEntriesFromAcl(dacl, &acl_entry_count, &acl_entry_list_raw);
    const WinLocalPtr<PEXPLICIT_ACCESS> acl_entry_list(acl_entry_list_raw);
    if (win_error != ERROR_SUCCESS || acl_entry_count != 1) {
        return false;
    }
    const EXPLICIT_ACCESS &ace = acl_entry_list[0];
    if (
        ace.grfAccessMode != GRANT_ACCESS
        || (ace.grfAccessPermissions & FILE_ALL_ACCESS) != FILE_ALL_ACCESS
        || ace.Trustee.TrusteeForm != TRUSTEE_IS_SID // Should already be SID, but double check.
    ) {
        return false;
    }

    // ACE must be for the current user.
    HANDLE process_token_raw;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_READ, &process_token_raw)) {
        return false;
    }
    const WinHandle process_token(process_token_raw);
    DWORD actual_token_info_size = 0;
    GetTokenInformation(process_token, TokenUser, nullptr, 0, &actual_token_info_size);
    if (!actual_token_info_size) {
        return false;
    }
    const WinHeapPtr<PTOKEN_USER> user_token_ptr(actual_token_info_size);
    if (!user_token_ptr || !GetTokenInformation(
        process_token,
        TokenUser,
        user_token_ptr,
        actual_token_info_size,
        &actual_token_info_size
    )) {
        return false;
    }
    return EqualSid(ace.Trustee.ptstrName, user_token_ptr->User.Sid);
#endif
    return false;
}

/**
 * Sets read and write permissions, and optionally the execute permission, for
 * the user and no permissions for others.
 */
void setUserOnlyPermission(const Glib::RefPtr<Gio::File> file, bool execute)
{
#if defined(__linux__) || defined(__APPLE__)
    const Glib::RefPtr<Gio::FileInfo> file_info = file->query_info("unix::mode");
    if (!file_info) {
        return;
    }

    guint32 mode = file_info->get_attribute_uint32("unix::mode");
    mode = (mode & ~0777) | (execute ? 0700 : 0600);
    try {
        file->set_attribute_uint32("unix::mode", mode, Gio::FILE_QUERY_INFO_NONE);
    } catch (Gio::Error &) {
    }
#elif defined(_WIN32)
    // Get the current user's SID.
    HANDLE process_token_raw;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_READ, &process_token_raw)) {
        return;
    }
    const WinHandle process_token(process_token_raw);
    DWORD actual_token_info_size = 0;
    GetTokenInformation(process_token, TokenUser, nullptr, 0, &actual_token_info_size);
    if (!actual_token_info_size) {
        return;
    }
    const WinHeapPtr<PTOKEN_USER> user_token_ptr(actual_token_info_size);
    if (!user_token_ptr || !GetTokenInformation(
        process_token,
        TokenUser,
        user_token_ptr,
        actual_token_info_size,
        &actual_token_info_size
    )) {
        return;
    }
    const PSID user_sid = user_token_ptr->User.Sid;

    // Get a handle to the file.
    const Glib::ustring filename = file->get_path();
    const HANDLE file_handle_raw = CreateFile(
        filename.c_str(),
        READ_CONTROL | WRITE_DAC,
        0,
        nullptr,
        OPEN_EXISTING,
        execute ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (file_handle_raw == INVALID_HANDLE_VALUE) {
        return;
    }
    const WinHandle file_handle(file_handle_raw);

    // Create the user-only permission and set it.
    EXPLICIT_ACCESS ea = {
        .grfAccessPermissions = FILE_ALL_ACCESS,
        .grfAccessMode = GRANT_ACCESS,
        .grfInheritance = NO_INHERITANCE,
        .Trustee = {},
    };
    BuildTrusteeWithSid(&(ea.Trustee), user_sid);
    PACL new_dacl_raw = nullptr;
    auto win_error = SetEntriesInAcl(1, &ea, nullptr, &new_dacl_raw);
    if (win_error != ERROR_SUCCESS) {
        return;
    }
    const WinLocalPtr<PACL> new_dacl(new_dacl_raw);
    SetSecurityInfo(
        file_handle,
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        new_dacl,
        nullptr
    );
#endif
}

/**
 * Gets the path to the temp directory, creating it if necessary.
 */
Glib::ustring getTmpDirectory()
{
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
    static Glib::ustring recent_dir = "";
    const Glib::ustring tmp_dir_root = Glib::get_tmp_dir();
    const Glib::ustring subdir_base =
        Glib::ustring::compose("rawtherapee-%1", Glib::get_user_name());
    Glib::ustring dir = Glib::build_filename(tmp_dir_root, subdir_base);

    // Returns true if the directory doesn't exist or has the right permissions.
    auto is_usable_dir = [](const Glib::ustring &dir_path) {
        return !Glib::file_test(dir_path, Glib::FILE_TEST_EXISTS) || (Glib::file_test(dir_path, Glib::FILE_TEST_IS_DIR) && hasUserOnlyPermission(dir_path));
    };

    if (!(is_usable_dir(dir) || recent_dir.empty())) {
        // Try to reuse the random suffix directory.
        dir = recent_dir;
    }

    if (!is_usable_dir(dir)) {
        // Create new directory with random suffix.
        gchar *const rand_dir = g_dir_make_tmp((subdir_base + "-XXXXXX").c_str(), nullptr);
        if (!rand_dir) {
            return tmp_dir_root;
        }
        dir = recent_dir = rand_dir;
        g_free(rand_dir);
        Glib::RefPtr<Gio::File> file = Gio::File::create_for_path(dir);
        setUserOnlyPermission(file, true);
    } else if (!Glib::file_test(dir, Glib::FILE_TEST_EXISTS)) {
        // Create the directory.
        Glib::RefPtr<Gio::File> file = Gio::File::create_for_path(dir);
        bool dir_created = file->make_directory();
        if (!dir_created) {
            return tmp_dir_root;
        }
        setUserOnlyPermission(file, true);
    }

    return dir;
#else
    return Glib::get_tmp_dir();
#endif
}
}

class EditorPanel::ColorManagementToolbar
{
private:
#if !defined(__APPLE__) // monitor profile not supported on apple
    MyComboBoxText profileBox;
#endif
    PopUpButton intentBox;
    Gtk::ToggleButton softProof;
    Gtk::ToggleButton spGamutCheck;
    sigc::connection profileConn, intentConn, softproofConn;
    Glib::ustring defprof;

    rtengine::StagedImageProcessor* const& processor;

private:
#if !defined(__APPLE__) // monitor profile not supported on apple
    void prepareProfileBox ()
    {
        profileBox.setPreferredWidth (70, 200);
        setExpandAlignProperties (&profileBox, false, false, Gtk::ALIGN_CENTER, Gtk::ALIGN_FILL);

        profileBox.append (M ("PREFERENCES_PROFILE_NONE"));
        Glib::ustring defprofname;

        if (find_default_monitor_profile (profileBox.get_root_window()->gobj(), defprof, defprofname)) {
            profileBox.append (M ("MONITOR_PROFILE_SYSTEM") + " (" + defprofname + ")");

            if (App::get().options().rtSettings.autoMonitorProfile) {
                rtengine::ICCStore::getInstance()->setDefaultMonitorProfileName (defprof);
                profileBox.set_active (1);
            } else {
                profileBox.set_active (0);
            }
        } else {
            profileBox.set_active (0);
        }

        const std::vector<Glib::ustring> profiles = rtengine::ICCStore::getInstance()->getProfiles (rtengine::ICCStore::ProfileType::MONITOR);

        for (const auto& profile : profiles) {
            profileBox.append (profile);
        }

        profileBox.set_tooltip_text (profileBox.get_active_text ());
    }
#endif

    void prepareIntentBox ()
    {
        // same order as the enum
        intentBox.addEntry ("intent-perceptual", M ("PREFERENCES_INTENT_PERCEPTUAL"));
        intentBox.addEntry ("intent-relative", M ("PREFERENCES_INTENT_RELATIVE"));
        intentBox.addEntry ("intent-absolute", M ("PREFERENCES_INTENT_ABSOLUTE"));
        setExpandAlignProperties (intentBox.buttonGroup, false, false, Gtk::ALIGN_CENTER, Gtk::ALIGN_FILL);

        intentBox.setSelected (1);
        intentBox.show ();
    }

    void prepareSoftProofingBox ()
    {
        Gtk::Image *softProofImage = Gtk::manage (new RTImage ("gamut-softproof", Gtk::ICON_SIZE_LARGE_TOOLBAR));
        softProofImage->set_padding (0, 0);
        softProof.add (*softProofImage);
        softProof.set_relief (Gtk::RELIEF_NONE);
        softProof.set_tooltip_markup (M ("SOFTPROOF_TOOLTIP"));

        softProof.set_active (false);
        softProof.show ();

        Gtk::Image *spGamutCheckImage = Gtk::manage (new RTImage ("gamut-warning", Gtk::ICON_SIZE_LARGE_TOOLBAR));
        spGamutCheckImage->set_padding (0, 0);
        spGamutCheck.add (*spGamutCheckImage);
        spGamutCheck.set_relief (Gtk::RELIEF_NONE);
        spGamutCheck.set_tooltip_markup (M ("SOFTPROOF_GAMUTCHECK_TOOLTIP"));

        spGamutCheck.set_active (false);
        spGamutCheck.set_sensitive (true);
        spGamutCheck.show ();
    }

#if !defined(__APPLE__)
    void profileBoxChanged ()
    {
        updateParameters ();
    }
#endif

    void intentBoxChanged (int)
    {
        updateParameters ();
    }

    void softProofToggled ()
    {
        updateSoftProofParameters ();
    }

    void spGamutCheckToggled ()
    {
        updateSoftProofParameters ();
    }

    void updateParameters (bool noEvent = false)
    {
#if !defined(__APPLE__) // monitor profile not supported on apple
        ConnectionBlocker profileBlocker (profileConn);
#endif
        ConnectionBlocker intentBlocker (intentConn);

        Glib::ustring profile;
        const auto& options = App::get().options();

#if !defined(__APPLE__) // monitor profile not supported on apple

        if (!defprof.empty() && profileBox.get_active_row_number () == 1) {
            profile = defprof;

            if (profile.empty ()) {
                profile = options.rtSettings.monitorProfile;
            }

            if (profile.empty ()) {
                profile = "sRGB IEC61966-2.1";
            }
        } else if (profileBox.get_active_row_number () > 0) {
            profile = profileBox.get_active_text ();
        }

#else
        profile = options.rtSettings.srgb;
#endif

#if !defined(__APPLE__) // monitor profile not supported on apple

        if (profileBox.get_active_row_number () == 0) {

            profile.clear ();

            intentBox.set_sensitive (false);
            intentBox.setSelected (1);
            softProof.set_sensitive (false);
            spGamutCheck.set_sensitive (false);

            profileBox.set_tooltip_text ("");

        } else {
            const uint8_t supportedIntents = rtengine::ICCStore::getInstance()->getProofIntents (profile);
            const bool supportsRelativeColorimetric = supportedIntents & 1 << INTENT_RELATIVE_COLORIMETRIC;
            const bool supportsPerceptual = supportedIntents & 1 << INTENT_PERCEPTUAL;
            const bool supportsAbsoluteColorimetric = supportedIntents & 1 << INTENT_ABSOLUTE_COLORIMETRIC;

            if (supportsPerceptual || supportsRelativeColorimetric || supportsAbsoluteColorimetric) {
                intentBox.set_sensitive (true);
                intentBox.setItemSensitivity (0, supportsPerceptual);
                intentBox.setItemSensitivity (1, supportsRelativeColorimetric);
                intentBox.setItemSensitivity (2, supportsAbsoluteColorimetric);
                softProof.set_sensitive (true);
                spGamutCheck.set_sensitive (true);
            } else {
                intentBox.setItemSensitivity (0, true);
                intentBox.setItemSensitivity (1, true);
                intentBox.setItemSensitivity (2, true);
                intentBox.set_sensitive (false);
                intentBox.setSelected (1);
                softProof.set_sensitive (false);
                spGamutCheck.set_sensitive (true);
            }

            profileBox.set_tooltip_text (profileBox.get_active_text ());
        }

#endif
        rtengine::RenderingIntent intent;

        switch (intentBox.getSelected ()) {
            default:
            case 0:
                intent = rtengine::RI_PERCEPTUAL;
                break;

            case 1:
                intent = rtengine::RI_RELATIVE;
                break;

            case 2:
                intent = rtengine::RI_ABSOLUTE;
                break;
        }

        if (!processor) {
            return;
        }

        if (!noEvent) {
            processor->beginUpdateParams ();
        }

        processor->setMonitorProfile (profile, intent);
        processor->setSoftProofing (softProof.get_sensitive() && softProof.get_active(), spGamutCheck.get_sensitive() && spGamutCheck.get_active());

        if (!noEvent) {
            processor->endUpdateParams (rtengine::EvMonitorTransform);
        }
    }

    void updateSoftProofParameters (bool noEvent = false)
    {
#if !defined(__APPLE__) // monitor profile not supported on apple
        softProof.set_sensitive (profileBox.get_active_row_number () > 0);
        spGamutCheck.set_sensitive(profileBox.get_active_row_number () > 0);
#endif


#if !defined(__APPLE__) // monitor profile not supported on apple

        if (profileBox.get_active_row_number () > 0) {
#endif

            if (processor) {
                if (!noEvent) {
                    processor->beginUpdateParams ();
                }

                processor->setSoftProofing (softProof.get_sensitive() && softProof.get_active(), spGamutCheck.get_active());

                if (!noEvent) {
                    processor->endUpdateParams (rtengine::EvMonitorTransform);
                }
            }

#if !defined(__APPLE__) // monitor profile not supported on apple
        }

#endif
    }

public:
    explicit ColorManagementToolbar (rtengine::StagedImageProcessor* const& ipc) :
        intentBox (Glib::ustring (), true),
        processor (ipc)
    {
#if !defined(__APPLE__) // monitor profile not supported on apple
        prepareProfileBox ();
#endif
        prepareIntentBox ();
        prepareSoftProofingBox ();

        reset ();

        softproofConn = softProof.signal_toggled().connect (sigc::mem_fun (this, &ColorManagementToolbar::softProofToggled));
        spGamutCheck.signal_toggled().connect (sigc::mem_fun (this, &ColorManagementToolbar::spGamutCheckToggled));
#if !defined(__APPLE__) // monitor profile not supported on apple
        profileConn = profileBox.signal_changed ().connect (sigc::mem_fun (this, &ColorManagementToolbar::profileBoxChanged));
#endif
        intentConn = intentBox.signal_changed ().connect (sigc::mem_fun (this, &ColorManagementToolbar::intentBoxChanged));
    }

    void pack_right_in (Gtk::Grid* grid)
    {
#if !defined(__APPLE__) // monitor profile not supported on apple
        grid->attach_next_to (profileBox, Gtk::POS_RIGHT, 1, 1);
#endif
        grid->attach_next_to (*intentBox.buttonGroup, Gtk::POS_RIGHT, 1, 1);
        grid->attach_next_to (softProof, Gtk::POS_RIGHT, 1, 1);
        grid->attach_next_to (spGamutCheck, Gtk::POS_RIGHT, 1, 1);
    }

    void updateProcessor()
    {
        if (processor) {
            updateParameters (true);
        }
    }

    void reset ()
    {
        const auto& options = App::get().options();

        ConnectionBlocker intentBlocker (intentConn);
#if !defined(__APPLE__) // monitor profile not supported on apple
        ConnectionBlocker profileBlocker (profileConn);

        if (!defprof.empty() && options.rtSettings.autoMonitorProfile) {
            profileBox.set_active (1);
        } else {
            setActiveTextOrIndex (profileBox, options.rtSettings.monitorProfile, 0);
        }

#endif

        switch (options.rtSettings.monitorIntent) {
            default:
            case rtengine::RI_PERCEPTUAL:
                intentBox.setSelected (0);
                break;

            case rtengine::RI_RELATIVE:
                intentBox.setSelected (1);
                break;

            case rtengine::RI_ABSOLUTE:
                intentBox.setSelected (2);
                break;
        }

        updateParameters ();
    }

    void updateHistogram()
    {
      updateParameters();
    }


    void defaultMonitorProfileChanged (const Glib::ustring &profile_name, bool auto_monitor_profile)
    {
        ConnectionBlocker profileBlocker (profileConn);

        if (auto_monitor_profile && !defprof.empty()) {
            rtengine::ICCStore::getInstance()->setDefaultMonitorProfileName (defprof);
#ifndef __APPLE__
            profileBox.set_active (1);
#endif
        } else {
            rtengine::ICCStore::getInstance()->setDefaultMonitorProfileName (profile_name);
#ifndef __APPLE__
            setActiveTextOrIndex (profileBox, profile_name, 0);
#endif
        }
    }

    // Accessors for options menu integration
    int getIntent () const { return intentBox.getSelected (); }
    void setIntent (int i) { intentBox.setSelected (i); updateParameters (); }

    bool getSoftProof () const { return softProof.get_active (); }
    void setSoftProof (bool a) { softProof.set_active (a); }

    bool getGamutCheck () const { return spGamutCheck.get_active (); }
    void setGamutCheck (bool a) { spGamutCheck.set_active (a); }

#if !defined(__APPLE__)
    int getProfileIndex () const { return profileBox.get_active_row_number (); }
    void setProfileIndex (int i) { profileBox.set_active (i); updateParameters (); }
    int getProfileCount () const { return profileBox.get_model ()->children ().size (); }
    Glib::ustring getProfileName (int i) const
    {
        auto row = profileBox.get_model ()->children ()[i];
        Glib::ustring text;
        row.get_value (0, text);
        return text;
    }
#else
    int getProfileIndex () const { return 0; }
    void setProfileIndex (int) {}
    int getProfileCount () const { return 0; }
    Glib::ustring getProfileName (int) const { return ""; }
#endif

};

EditorPanel::EditorPanel (FilePanel* filePanel)
    : catalogPane (nullptr), realized (false), tbBeforeLock (nullptr),
      editorToolbarTop_ (nullptr), editorToolbarBottom_ (nullptr),
      iHistoryShow (nullptr), iHistoryHide (nullptr),
      iTopPanel_1_Show (nullptr), iTopPanel_1_Hide (nullptr), iRightPanel_1_Show (nullptr), iRightPanel_1_Hide (nullptr),
      iBeforeLockON (nullptr), iBeforeLockOFF (nullptr),
      navigatorDialog_ (nullptr), historyDialog_ (nullptr),
      editorPlacesBrowser_ (nullptr), editorRecentBrowser_ (nullptr),
      editorDirBrowser_ (nullptr), editorPlacesPaned_ (nullptr),
      externalEditorChangedSignal (nullptr),
      previewHandler (nullptr), beforePreviewHandler (nullptr),
      beforeIarea (nullptr), beforeBox (nullptr), afterBox (nullptr), beforeLabel (nullptr), afterLabel (nullptr),
      beforeHeaderBox (nullptr), afterHeaderBox (nullptr), parent (nullptr), parentWindow (nullptr), openThm (nullptr),
      selectedFrame(0), isrc (nullptr), ipc (nullptr), beforeIpc (nullptr), err (0), isProcessing (false),
      histogram_observable(nullptr), histogram_scope_type(ScopeType::NONE)
{

    set_orientation(Gtk::ORIENTATION_VERTICAL);
    epih = new EditorPanelIdleHelper;
    epih->epanel = this;
    epih->destroyed = false;
    epih->pending = 0;
    //rtengine::befaf=true;
    processingStartedTime = 0;
    firstProcessingDone = false;

    // construct toolpanelcoordinator
    tpc = new ToolPanelCoordinator();
    tpc->setProgressListener(this);

    // build GUI

    // build left side panel
    leftbox = new Gtk::Paned (Gtk::ORIENTATION_VERTICAL);

    histogramPanel = nullptr;

    presetListPanel = new PresetListPanel();

    // Create navigator and history (NOT packed into sidebar — used in popup dialogs)
    navigator = new Navigator();
    navigator->previewWindow->set_size_request(-1, RTScalable::scalePixelSize(150));
    history = new History();

    // Build left sidebar: Places / Recent Folders / Directory Browser
    editorPlacesPaned_ = new Gtk::Box(Gtk::ORIENTATION_VERTICAL);
    editorPlacesPaned_->set_name("PlacesPaned");
    editorPlacesPaned_->set_size_request(250, -1);

    editorDirBrowser_ = Gtk::manage(new DirBrowser());
    editorPlacesBrowser_ = Gtk::manage(new PlacesBrowser());
    editorRecentBrowser_ = Gtk::manage(new RecentBrowser());
    albumBrowser_ = Gtk::manage(new AlbumBrowser());

    Gtk::Box* placesObox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
    placesObox->get_style_context()->add_class("plainback");

    placesObox->pack_start(*editorDirBrowser_, Gtk::PACK_EXPAND_WIDGET, 0);
    editorDirBrowser_->set_size_request(-1, 300);
    placesObox->pack_start(*editorRecentBrowser_, Gtk::PACK_SHRINK, 4);
    placesObox->pack_start(*albumBrowser_, Gtk::PACK_SHRINK, 0);

    editorPlacesPaned_->pack_start(*editorPlacesBrowser_, Gtk::PACK_SHRINK);
    editorPlacesPaned_->pack_start(*placesObox, Gtk::PACK_EXPAND_WIDGET);

    // Wire album selection to filmstrip filter and album view
    albumBrowser_->albumSelected().connect(sigc::mem_fun(*this, &EditorPanel::onAlbumSelected));
    albumBrowser_->albumViewRequested().connect(sigc::mem_fun(*this, &EditorPanel::onAlbumViewRequested));
    albumBrowser_->setCurrentFilePathGetter([this]() -> Glib::ustring { return fname; });

    // pack1 is reserved for histogram (when positioned on left side).
    // Use a minimal placeholder to keep the Paned layout stable.
    auto* histPlaceholder = Gtk::manage(new Gtk::Box());
    histPlaceholder->set_size_request(-1, 0);
    histPlaceholder->set_no_show_all(true);
    leftbox->pack1(*histPlaceholder, false, false);
    leftbox->pack2(*editorPlacesPaned_, true, true);
    leftbox->set_position(0);
    leftbox->show_all();

    // build the middle of the screen
    Gtk::Box* editbox = Gtk::manage (new Gtk::Box (Gtk::ORIENTATION_VERTICAL));

    beforeAfter = Gtk::manage (new Gtk::ToggleButton ());
    beforeAfter->set_image (*Gtk::manage (new RTImage ("compare", Gtk::ICON_SIZE_MENU)));
    beforeAfter->set_relief (Gtk::RELIEF_NONE);
    beforeAfter->set_tooltip_markup (M ("MAIN_TOOLTIP_TOGGLE"));

    iBeforeLockON = new RTImage ("ba-lock-on", Gtk::ICON_SIZE_SMALL_TOOLBAR);
    iBeforeLockOFF = new RTImage ("ba-lock-off", Gtk::ICON_SIZE_SMALL_TOOLBAR);


    hidehp = Gtk::manage (new Gtk::ToggleButton ());

    iHistoryShow = new RTImage ("panel-to-right", Gtk::ICON_SIZE_LARGE_TOOLBAR);
    iHistoryHide = new RTImage ("panel-to-left", Gtk::ICON_SIZE_LARGE_TOOLBAR);

    const auto& options = App::get().options();
    hidehp->set_relief (Gtk::RELIEF_NONE);
    hidehp->set_active (options.showHistory);
    hidehp->set_tooltip_markup (M ("MAIN_TOOLTIP_HIDEHP"));

    if (options.showHistory) {
        hidehp->set_image (*iHistoryHide);
    } else {
        hidehp->set_image (*iHistoryShow);
    }

    tbTopPanel_1 = nullptr;

    if (!App::get().isSimpleEditor() && filePanel) {
        tbTopPanel_1 = new Gtk::ToggleButton ();
        iTopPanel_1_Show = new RTImage ("panel-to-bottom", Gtk::ICON_SIZE_LARGE_TOOLBAR);
        iTopPanel_1_Hide = new RTImage ("panel-to-top", Gtk::ICON_SIZE_LARGE_TOOLBAR);
        tbTopPanel_1->set_relief (Gtk::RELIEF_NONE);
        tbTopPanel_1->set_active (true);
        tbTopPanel_1->set_tooltip_markup (M ("MAIN_TOOLTIP_SHOWHIDETP1"));
        tbTopPanel_1->set_image (*iTopPanel_1_Hide);
    }


    // Histogram profile toggle controls
    toggleHistogramProfile = Gtk::manage (new Gtk::ToggleButton ());
    Gtk::Image* histProfImg = Gtk::manage (new RTImage ("gamut-hist", Gtk::ICON_SIZE_LARGE_TOOLBAR));
    toggleHistogramProfile->add (*histProfImg);
    toggleHistogramProfile->set_relief (Gtk::RELIEF_NONE);
    toggleHistogramProfile->set_active (options.rtSettings.HistogramWorking);
    toggleHistogramProfile->set_tooltip_markup ( (M ("PREFERENCES_HISTOGRAM_TOOLTIP")));

    iareapanel = new ImageAreaPanel ();
    tpc->setEditProvider (iareapanel->imageArea);
    tpc->getToolBar()->setLockablePickerToolListener (iareapanel->imageArea);

    Gtk::Box* toolBarPanel = Gtk::manage (new Gtk::Box (Gtk::ORIENTATION_HORIZONTAL));
    toolBarPanel->set_name ("EditorTopPanel");
    if (tbTopPanel_1) {
        toolBarPanel->pack_start (*tbTopPanel_1, Gtk::PACK_SHRINK, 1);
    }
    toolBarPanel->pack_start (*tpc->getToolBar(), Gtk::PACK_SHRINK, 1);

    // Filter bar toggle button
    tbFilterBar = nullptr;
    filterBarRevealer = nullptr;
    filterBarBlockSignals = false;
    if (!App::get().isSimpleEditor() && filePanel) {
        tbFilterBar = Gtk::manage(new Gtk::ToggleButton());
        tbFilterBar->set_image(*Gtk::manage(new RTImage("filter-modern", Gtk::ICON_SIZE_MENU)));
        tbFilterBar->set_relief(Gtk::RELIEF_NONE);
        tbFilterBar->set_tooltip_markup(M("EDITOR_FILTER_TOOLTIP"));
        tbFilterBar->signal_toggled().connect(sigc::mem_fun(*this, &EditorPanel::filterBarToggled));
    }

    // Album view toggle button
    tbAlbumView_ = nullptr;
    albumViewSession_ = 0;
    albumViewMode_ = AlbumViewMode::GRID;
    albumThumbHeight_ = 120;
    albumShowInfo_ = false;
    albumZoomSlider_ = nullptr;
    albumInfoToggle_ = nullptr;
    albumModeGrid_ = nullptr;
    albumModeFit_ = nullptr;
    albumModeCollage_ = nullptr;
    albumGridStack_ = nullptr;
    albumCollageArea_ = nullptr;
    collageContentHeight_ = 0;
    filmstripSortBtn_ = nullptr;
    filmstripSortMenu_ = nullptr;
    albumSortBtn_ = nullptr;
    albumSortMenu_ = nullptr;
    if (!App::get().isSimpleEditor() && filePanel) {
        tbAlbumView_ = Gtk::manage(new Gtk::ToggleButton());
        tbAlbumView_->set_image(*Gtk::manage(new RTImage("fullscreen-leave", Gtk::ICON_SIZE_LARGE_TOOLBAR)));
        tbAlbumView_->set_relief(Gtk::RELIEF_NONE);
        tbAlbumView_->set_tooltip_markup(M("EDITOR_ALBUM_VIEW_TOOLTIP"));
        albumViewToggleConn_ = tbAlbumView_->signal_toggled().connect(sigc::mem_fun(*this, &EditorPanel::toggleAlbumView));
        toolBarPanel->pack_start(*tbAlbumView_, Gtk::PACK_SHRINK, 1);
    }

    // Filmstrip action bar (rating, color label, queue) — centered in toolbar
    filmstripCurrentRating = 0;
    filmstripActionBar = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 0));
    filmstripActionBar->set_name("FilmstripActions");
    {
        // Inline CSS for ultra-compact buttons
        auto css = Gtk::CssProvider::create();
        css->load_from_data(
            "#FilmstripActions { padding: 0; margin: 0; }"
            "#FilmstripActions button { min-height: 0; min-width: 0; padding: 1px 0; margin: 0; }"
            "#FilmstripActions button image { margin: -0.4em 0; padding: 0; min-width: 0; min-height: 0; }"
            "#FilmstripActions separator { margin: 0 3px; min-width: 1px; }"
        );
        filmstripActionBar->get_style_context()->add_provider(css, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 200);

        auto applyCSS = [&css](Gtk::Widget* w) {
            w->get_style_context()->add_provider(css, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 200);
        };

        // Before/After compare button
        applyCSS(beforeAfter);
        filmstripActionBar->pack_start(*beforeAfter, Gtk::PACK_SHRINK);

        // Filter bar toggle button
        if (tbFilterBar) {
            applyCSS(tbFilterBar);
            filmstripActionBar->pack_start(*tbFilterBar, Gtk::PACK_SHRINK);
        }

        auto* sep0 = Gtk::manage(new Gtk::Separator(Gtk::ORIENTATION_VERTICAL));
        applyCSS(sep0);
        filmstripActionBar->pack_start(*sep0, Gtk::PACK_SHRINK);

        // Star rating buttons: unrank + 5 stars
        Gtk::Button* unrankBtn = Gtk::manage(new Gtk::Button());
        unrankBtn->set_image(*Gtk::manage(new RTImage("star-hollow-small", Gtk::ICON_SIZE_MENU)));
        unrankBtn->set_relief(Gtk::RELIEF_NONE);
        unrankBtn->set_tooltip_markup(M("FILEBROWSER_UNRANK_TOOLTIP"));
        unrankBtn->signal_clicked().connect([this]() {
            if (fPanel && fPanel->fileCatalog && fPanel->fileCatalog->fileBrowser) {
                fPanel->fileCatalog->fileBrowser->requestRanking(0);
                filmstripCurrentRating = 0;
                updateFilmstripStars(0);
            }
        });
        applyCSS(unrankBtn);
        filmstripActionBar->pack_start(*unrankBtn, Gtk::PACK_SHRINK);

        for (int i = 0; i < 5; i++) {
            filmstripRankBtns[i] = Gtk::manage(new Gtk::Button());
            filmstripRankBtns[i]->set_image(*Gtk::manage(new RTImage("star-small", Gtk::ICON_SIZE_MENU)));
            filmstripRankBtns[i]->set_relief(Gtk::RELIEF_NONE);
            filmstripRankBtns[i]->signal_clicked().connect([this, i]() {
                if (fPanel && fPanel->fileCatalog && fPanel->fileCatalog->fileBrowser) {
                    int rank = i + 1;
                    fPanel->fileCatalog->fileBrowser->requestRanking(rank);
                    filmstripCurrentRating = rank;
                    updateFilmstripStars(rank);
                }
            });
            filmstripRankBtns[i]->signal_enter_notify_event().connect([this, i](GdkEventCrossing*) -> bool {
                updateFilmstripStars(i + 1);
                return false;
            });
            filmstripRankBtns[i]->signal_leave_notify_event().connect([this](GdkEventCrossing*) -> bool {
                updateFilmstripStars(filmstripCurrentRating);
                return false;
            });
            applyCSS(filmstripRankBtns[i]);
            filmstripActionBar->pack_start(*filmstripRankBtns[i], Gtk::PACK_SHRINK);
        }

        auto* sep1 = Gtk::manage(new Gtk::Separator(Gtk::ORIENTATION_VERTICAL));
        applyCSS(sep1);
        filmstripActionBar->pack_start(*sep1, Gtk::PACK_SHRINK);

        // Color label pill — multicolor trigger circle expands on hover
        {
            auto* pillEventBox = Gtk::manage(new Gtk::EventBox());
            auto* pillBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 0));
            pillBox->set_name("ColorLabelPill");

            // Inline CSS for pill shape
            auto pillCss = Gtk::CssProvider::create();
            pillCss->load_from_data(
                "#ColorLabelPill { border-radius: 12px; }"
            );
            pillBox->get_style_context()->add_provider(pillCss, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 200);

            // Trigger circle (always visible)
            auto* triggerBtn = Gtk::manage(new Gtk::Button());
            triggerBtn->set_image(*Gtk::manage(new RTImage("circle-multicolor-small", Gtk::ICON_SIZE_MENU)));
            triggerBtn->set_relief(Gtk::RELIEF_NONE);
            triggerBtn->set_tooltip_markup(M("FILEBROWSER_COLORLABEL_TOOLTIP"));
            applyCSS(triggerBtn);
            pillBox->pack_start(*triggerBtn, Gtk::PACK_SHRINK);

            // Revealer with the 6 color label buttons
            colorLabelRevealer_ = Gtk::manage(new Gtk::Revealer());
            colorLabelRevealer_->set_transition_type(Gtk::REVEALER_TRANSITION_TYPE_SLIDE_RIGHT);
            colorLabelRevealer_->set_transition_duration(150);
            colorLabelRevealer_->set_reveal_child(false);

            auto* labelBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 0));
            std::array<std::string, 6> clabelIcons = {"circle-gray-small", "circle-red-small", "circle-yellow-small", "circle-green-small", "circle-blue-small", "circle-purple-small"};
            for (int i = 0; i < 6; i++) {
                Gtk::Button* clabelBtn = Gtk::manage(new Gtk::Button());
                clabelBtn->set_image(*Gtk::manage(new RTImage(clabelIcons[i], Gtk::ICON_SIZE_MENU)));
                clabelBtn->set_relief(Gtk::RELIEF_NONE);
                clabelBtn->set_tooltip_markup(M("FILEBROWSER_COLORLABEL_TOOLTIP"));
                clabelBtn->signal_clicked().connect([this, i]() {
                    if (fPanel && fPanel->fileCatalog && fPanel->fileCatalog->fileBrowser)
                        fPanel->fileCatalog->fileBrowser->requestColorLabel(i);
                });
                applyCSS(clabelBtn);
                labelBox->pack_start(*clabelBtn, Gtk::PACK_SHRINK);
            }
            colorLabelRevealer_->add(*labelBox);
            pillBox->pack_start(*colorLabelRevealer_, Gtk::PACK_SHRINK);

            pillEventBox->add(*pillBox);
            pillEventBox->set_events(Gdk::ENTER_NOTIFY_MASK | Gdk::LEAVE_NOTIFY_MASK);
            pillEventBox->signal_enter_notify_event().connect([this](GdkEventCrossing*) -> bool {
                colorLabelRevealer_->set_reveal_child(true);
                return false;
            });
            pillEventBox->signal_leave_notify_event().connect([this](GdkEventCrossing*) -> bool {
                colorLabelRevealer_->set_reveal_child(false);
                return false;
            });

            filmstripActionBar->pack_start(*pillEventBox, Gtk::PACK_SHRINK);
        }

        auto* sep2 = Gtk::manage(new Gtk::Separator(Gtk::ORIENTATION_VERTICAL));
        applyCSS(sep2);
        filmstripActionBar->pack_start(*sep2, Gtk::PACK_SHRINK);

        // Add to queue button
        Gtk::Button* queueBtn = Gtk::manage(new Gtk::Button());
        queueBtn->set_image(*Gtk::manage(new RTImage("gears-small", Gtk::ICON_SIZE_MENU)));
        queueBtn->set_relief(Gtk::RELIEF_NONE);
        queueBtn->set_tooltip_markup(M("FILEBROWSER_POPUPPROCESS"));
        queueBtn->signal_clicked().connect([this]() {
            if (fPanel && fPanel->fileCatalog && fPanel->fileCatalog->fileBrowser)
                fPanel->fileCatalog->fileBrowser->requestDevelop();
        });
        applyCSS(queueBtn);
        filmstripActionBar->pack_start(*queueBtn, Gtk::PACK_SHRINK);

        // Add to Album button
        auto* sep3 = Gtk::manage(new Gtk::Separator(Gtk::ORIENTATION_VERTICAL));
        applyCSS(sep3);
        filmstripActionBar->pack_start(*sep3, Gtk::PACK_SHRINK);

        Gtk::Button* addToAlbumBtn = Gtk::manage(new Gtk::Button());
        addToAlbumBtn->set_image(*Gtk::manage(new RTImage("add-to-album", Gtk::ICON_SIZE_MENU)));
        addToAlbumBtn->set_relief(Gtk::RELIEF_NONE);
        addToAlbumBtn->set_tooltip_markup(M("EDITOR_ADD_TO_ALBUM_TOOLTIP"));
        addToAlbumBtn->signal_clicked().connect(sigc::mem_fun(*this, &EditorPanel::addCurrentImageToTargetAlbum));
        applyCSS(addToAlbumBtn);
        filmstripActionBar->pack_start(*addToAlbumBtn, Gtk::PACK_SHRINK);

        // Copy Edit Settings button + filter cog
        auto* sep4 = Gtk::manage(new Gtk::Separator(Gtk::ORIENTATION_VERTICAL));
        applyCSS(sep4);
        filmstripActionBar->pack_start(*sep4, Gtk::PACK_SHRINK);

        // Build copy filter menu (same structure as file browser)
        {
            editorCopyFilterMenu_ = Gtk::manage(new Gtk::Menu());
            editorCopyFilters_.clear();
            int p = 0;

            auto preventClose = [](Gtk::CheckMenuItem* item) {
                item->signal_button_release_event().connect(
                    [item](GdkEventButton*) {
                        item->set_active(!item->get_active());
                        return true;
                    }, false);
            };

            auto preventCloseAction = [](Gtk::MenuItem* item, std::function<void()> action) {
                item->signal_button_release_event().connect(
                    [action](GdkEventButton*) {
                        action();
                        return true;
                    }, false);
            };

            // Copy / Paste actions at top
            auto* doCopy = Gtk::manage(new MyImageMenuItem(M("PRESET_COPY_SETTINGS"), "preset-copy"));
            doCopy->signal_activate().connect([this]() {
                if (!openThm) return;
                const auto& srcPP = openThm->getProcParams();
                bool allActive = true;
                for (const auto& kv : editorCopyFilters_) {
                    if (!kv.second->get_active()) { allActive = false; break; }
                }
                if (allActive) {
                    clipboard.setProcParams(srcPP);
                } else {
                    ParamsEdited filterPE(true);
                    filterPE.locallab.spots.resize(srcPP.locallab.spots.size(), LocallabParamsEdited::LocallabSpotEdited(true));
                    ParamsEdited falsePE;
                    falsePE.locallab.spots.resize(srcPP.locallab.spots.size(), LocallabParamsEdited::LocallabSpotEdited(false));
                    filterPE.general = falsePE.general;
                    auto isOff = [this](const std::string& key) -> bool {
                        auto it = editorCopyFilters_.find(key);
                        return it != editorCopyFilters_.end() && !it->second->get_active();
                    };
                    if (isOff("wb"))            filterPE.wb = falsePE.wb;
                    if (isOff("toneCurve"))     filterPE.toneCurve = falsePE.toneCurve;
                    if (isOff("sh"))            filterPE.sh = falsePE.sh;
                    if (isOff("toneEqualizer")) filterPE.toneEqualizer = falsePE.toneEqualizer;
                    if (isOff("sharpening"))      filterPE.sharpening = falsePE.sharpening;
                    if (isOff("sharpenEdge"))     filterPE.sharpenEdge = falsePE.sharpenEdge;
                    if (isOff("sharpenMicro"))    filterPE.sharpenMicro = falsePE.sharpenMicro;
                    if (isOff("impulseDenoise"))  filterPE.impulseDenoise = falsePE.impulseDenoise;
                    if (isOff("dirpyrDenoise"))   filterPE.dirpyrDenoise = falsePE.dirpyrDenoise;
                    if (isOff("defringe"))        filterPE.defringe = falsePE.defringe;
                    if (isOff("dehaze"))          filterPE.dehaze = falsePE.dehaze;
                    if (isOff("dirpyrequalizer")) filterPE.dirpyrequalizer = falsePE.dirpyrequalizer;
                    if (isOff("labCurve"))       filterPE.labCurve = falsePE.labCurve;
                    if (isOff("rgbCurves"))      filterPE.rgbCurves = falsePE.rgbCurves;
                    if (isOff("colorToning"))    filterPE.colorToning = falsePE.colorToning;
                    if (isOff("chmixer"))        filterPE.chmixer = falsePE.chmixer;
                    if (isOff("blackwhite"))     filterPE.blackwhite = falsePE.blackwhite;
                    if (isOff("hsvequalizer"))   filterPE.hsvequalizer = falsePE.hsvequalizer;
                    if (isOff("filmSimulation")) filterPE.filmSimulation = falsePE.filmSimulation;
                    if (isOff("softlight"))      filterPE.softlight = falsePE.softlight;
                    if (isOff("vibrance"))       filterPE.vibrance = falsePE.vibrance;
                    if (isOff("distortion"))   filterPE.distortion = falsePE.distortion;
                    if (isOff("cacorrection")) filterPE.cacorrection = falsePE.cacorrection;
                    if (isOff("vignetting"))   filterPE.vignetting = falsePE.vignetting;
                    if (isOff("lensProf"))     filterPE.lensProf = falsePE.lensProf;
                    if (isOff("coarse"))       filterPE.coarse = falsePE.coarse;
                    if (isOff("rotate"))       filterPE.rotate = falsePE.rotate;
                    if (isOff("crop"))         filterPE.crop = falsePE.crop;
                    if (isOff("resize"))       filterPE.resize = falsePE.resize;
                    if (isOff("prsharpening")) filterPE.prsharpening = falsePE.prsharpening;
                    if (isOff("perspective"))  filterPE.perspective = falsePE.perspective;
                    if (isOff("commonTrans"))  filterPE.commonTrans = falsePE.commonTrans;
                    if (isOff("gradient"))     filterPE.gradient = falsePE.gradient;
                    if (isOff("framing"))      filterPE.framing = falsePE.framing;
                    if (isOff("retinex")) filterPE.retinex = falsePE.retinex;
                    if (isOff("wavelet")) filterPE.wavelet = falsePE.wavelet;
                    if (isOff("spot"))    filterPE.spot = falsePE.spot;
                    if (isOff("cg"))      filterPE.cg = falsePE.cg;
                    if (isOff("locallab")) filterPE.locallab = falsePE.locallab;
                    rtengine::procparams::ProcParams filteredPP;
                    filterPE.combine(filteredPP, srcPP, true);
                    rtengine::procparams::PartialProfile pp(&filteredPP, &filterPE);
                    clipboard.setPartialProfile(pp);
                }
            });
            editorCopyFilterMenu_->attach(*doCopy, 0, 1, p, p + 1); p++;

            auto* doPaste = Gtk::manage(new MyImageMenuItem(M("PRESET_PASTE_CLIPBOARD"), "preset-paste"));
            doPaste->signal_activate().connect([this]() {
                if (!clipboard.hasProcParams() || !ipc) return;
                ProcParams pp = clipboard.getProcParams();
                ProcParams* params = ipc->beginUpdateParams();
                *params = pp;
                ipc->endUpdateParams(rtengine::EvProfileChanged);
            });
            editorCopyFilterMenu_->attach(*doPaste, 0, 1, p, p + 1); p++;

            editorCopyFilterMenu_->attach(*Gtk::manage(new Gtk::SeparatorMenuItem()), 0, 1, p, p + 1); p++;

            // Global All / None
            auto* copyAll = Gtk::manage(new MyImageMenuItem(M("GENERAL_ALL"), "menu-select-all"));
            preventCloseAction(copyAll, [this]() {
                for (auto& kv : editorCopyFilters_) kv.second->set_active(true);
            });
            editorCopyFilterMenu_->attach(*copyAll, 0, 1, p, p + 1); p++;

            auto* copyNone = Gtk::manage(new MyImageMenuItem(M("GENERAL_NONE"), "menu-select-none"));
            preventCloseAction(copyNone, [this]() {
                for (auto& kv : editorCopyFilters_) kv.second->set_active(false);
            });
            editorCopyFilterMenu_->attach(*copyNone, 0, 1, p, p + 1); p++;

            editorCopyFilterMenu_->attach(*Gtk::manage(new Gtk::SeparatorMenuItem()), 0, 1, p, p + 1); p++;

            auto addGroup = [&](const Glib::ustring& groupLabel,
                                const std::vector<std::pair<std::string, Glib::ustring>>& items) {
                Gtk::MenuItem* groupItem = Gtk::manage(new Gtk::MenuItem(groupLabel));
                editorCopyFilterMenu_->attach(*groupItem, 0, 1, p, p + 1);
                p++;

                Gtk::Menu* sub = Gtk::manage(new Gtk::Menu());
                int s = 0;

                std::vector<Gtk::CheckMenuItem*> children;
                for (const auto& kv : items) {
                    auto* item = Gtk::manage(new Gtk::CheckMenuItem(kv.second));
                    item->set_active(true);
                    editorCopyFilters_[kv.first] = item;
                    preventClose(item);
                    children.push_back(item);
                }

                auto* grpAll = Gtk::manage(new MyImageMenuItem(M("GENERAL_ALL"), "menu-select-all"));
                preventCloseAction(grpAll, [children]() {
                    for (auto* c : children) c->set_active(true);
                });
                sub->attach(*grpAll, 0, 1, s, s + 1); s++;

                auto* grpNone = Gtk::manage(new MyImageMenuItem(M("GENERAL_NONE"), "menu-select-none"));
                preventCloseAction(grpNone, [children]() {
                    for (auto* c : children) c->set_active(false);
                });
                sub->attach(*grpNone, 0, 1, s, s + 1); s++;

                sub->attach(*Gtk::manage(new Gtk::SeparatorMenuItem()), 0, 1, s, s + 1); s++;

                for (auto* item : children) {
                    sub->attach(*item, 0, 1, s, s + 1);
                    s++;
                }

                sub->show_all();
                groupItem->set_submenu(*sub);
            };

            addGroup(M("PARTIALPASTE_BASICGROUP"), {
                {"wb",            M("PARTIALPASTE_WHITEBALANCE")},
                {"toneCurve",     M("PARTIALPASTE_EXPOSURE")},
                {"sh",            M("PARTIALPASTE_SHADOWSHIGHLIGHTS")},
                {"toneEqualizer", M("PARTIALPASTE_TONE_EQUALIZER")},
            });
            addGroup(M("PARTIALPASTE_DETAILGROUP"), {
                {"sharpening",      M("PARTIALPASTE_SHARPENING")},
                {"sharpenEdge",     M("PARTIALPASTE_SHARPENEDGE")},
                {"sharpenMicro",    M("PARTIALPASTE_SHARPENMICRO")},
                {"impulseDenoise",  M("PARTIALPASTE_IMPULSEDENOISE")},
                {"dirpyrDenoise",   M("PARTIALPASTE_DIRPYRDENOISE")},
                {"defringe",        M("PARTIALPASTE_DEFRINGE")},
                {"dehaze",          M("PARTIALPASTE_DEHAZE")},
                {"dirpyrequalizer", M("PARTIALPASTE_DIRPYREQUALIZER")},
            });
            addGroup(M("PARTIALPASTE_COLORGROUP"), {
                {"labCurve",       M("PARTIALPASTE_LABCURVE")},
                {"rgbCurves",      M("PARTIALPASTE_RGBCURVES")},
                {"colorToning",    M("PARTIALPASTE_COLORTONING")},
                {"chmixer",        M("PARTIALPASTE_CHANNELMIXER")},
                {"blackwhite",     M("PARTIALPASTE_CHANNELMIXERBW")},
                {"hsvequalizer",   M("PARTIALPASTE_HSVEQUALIZER")},
                {"filmSimulation", M("PARTIALPASTE_FILMSIMULATION")},
                {"softlight",      M("PARTIALPASTE_SOFTLIGHT")},
                {"vibrance",       M("PARTIALPASTE_VIBRANCE")},
            });
            addGroup(M("PARTIALPASTE_LENSGROUP"), {
                {"distortion",   M("PARTIALPASTE_DISTORTION")},
                {"cacorrection", M("PARTIALPASTE_CACORRECTION")},
                {"vignetting",   M("PARTIALPASTE_VIGNETTING")},
                {"lensProf",     M("PARTIALPASTE_LENSPROFILE")},
            });
            addGroup(M("PARTIALPASTE_COMPOSITIONGROUP"), {
                {"coarse",       M("PARTIALPASTE_COARSETRANS")},
                {"rotate",       M("PARTIALPASTE_ROTATION")},
                {"crop",         M("PARTIALPASTE_CROP")},
                {"resize",       M("PARTIALPASTE_RESIZE")},
                {"prsharpening", M("PARTIALPASTE_PRSHARPENING")},
                {"perspective",  M("PARTIALPASTE_PERSPECTIVE")},
                {"commonTrans",  M("PARTIALPASTE_COMMONTRANSFORMPARAMS")},
                {"gradient",     M("PARTIALPASTE_GRADIENT")},
                {"framing",      M("PARTIALPASTE_FRAMING")},
            });
            addGroup(M("PARTIALPASTE_ADVANCEDGROUP"), {
                {"retinex", M("PARTIALPASTE_RETINEX")},
                {"wavelet", M("PARTIALPASTE_EQUALIZER")},
                {"spot",    M("PARTIALPASTE_SPOT")},
                {"cg",      M("PARTIALPASTE_COMPRESSGAMUT")},
            });

            auto* locallabItem = Gtk::manage(new Gtk::CheckMenuItem(M("PARTIALPASTE_LOCALLABGROUP")));
            locallabItem->set_active(true);
            editorCopyFilters_["locallab"] = locallabItem;
            preventClose(locallabItem);
            editorCopyFilterMenu_->attach(*locallabItem, 0, 1, p, p + 1);
            p++;

            editorCopyFilterMenu_->show_all();
        }

        Gtk::Button* copySettingsBtn = Gtk::manage(new Gtk::Button());
        copySettingsBtn->set_image(*Gtk::manage(new RTImage("copy", Gtk::ICON_SIZE_MENU)));
        copySettingsBtn->set_relief(Gtk::RELIEF_NONE);
        copySettingsBtn->set_tooltip_markup(M("EDITOR_COPY_SETTINGS_TOOLTIP"));
        copySettingsBtn->signal_clicked().connect([this]() {
            if (!openThm) return;
            const auto& srcPP = openThm->getProcParams();

            // Check if all visible filters are active
            bool allActive = true;
            for (const auto& kv : editorCopyFilters_) {
                if (!kv.second->get_active()) {
                    allActive = false;
                    break;
                }
            }

            if (allActive) {
                clipboard.setProcParams(srcPP);
            } else {
                ParamsEdited filterPE(true);
                filterPE.locallab.spots.resize(srcPP.locallab.spots.size(), LocallabParamsEdited::LocallabSpotEdited(true));
                ParamsEdited falsePE;
                falsePE.locallab.spots.resize(srcPP.locallab.spots.size(), LocallabParamsEdited::LocallabSpotEdited(false));
                filterPE.general = falsePE.general;

                auto isOff = [this](const std::string& key) -> bool {
                    auto it = editorCopyFilters_.find(key);
                    return it != editorCopyFilters_.end() && !it->second->get_active();
                };

                if (isOff("wb"))            filterPE.wb = falsePE.wb;
                if (isOff("toneCurve"))     filterPE.toneCurve = falsePE.toneCurve;
                if (isOff("sh"))            filterPE.sh = falsePE.sh;
                if (isOff("toneEqualizer")) filterPE.toneEqualizer = falsePE.toneEqualizer;
                if (isOff("sharpening"))      filterPE.sharpening = falsePE.sharpening;
                if (isOff("sharpenEdge"))     filterPE.sharpenEdge = falsePE.sharpenEdge;
                if (isOff("sharpenMicro"))    filterPE.sharpenMicro = falsePE.sharpenMicro;
                if (isOff("impulseDenoise"))  filterPE.impulseDenoise = falsePE.impulseDenoise;
                if (isOff("dirpyrDenoise"))   filterPE.dirpyrDenoise = falsePE.dirpyrDenoise;
                if (isOff("defringe"))        filterPE.defringe = falsePE.defringe;
                if (isOff("dehaze"))          filterPE.dehaze = falsePE.dehaze;
                if (isOff("dirpyrequalizer")) filterPE.dirpyrequalizer = falsePE.dirpyrequalizer;
                if (isOff("labCurve"))       filterPE.labCurve = falsePE.labCurve;
                if (isOff("rgbCurves"))      filterPE.rgbCurves = falsePE.rgbCurves;
                if (isOff("colorToning"))    filterPE.colorToning = falsePE.colorToning;
                if (isOff("chmixer"))        filterPE.chmixer = falsePE.chmixer;
                if (isOff("blackwhite"))     filterPE.blackwhite = falsePE.blackwhite;
                if (isOff("hsvequalizer"))   filterPE.hsvequalizer = falsePE.hsvequalizer;
                if (isOff("filmSimulation")) filterPE.filmSimulation = falsePE.filmSimulation;
                if (isOff("softlight"))      filterPE.softlight = falsePE.softlight;
                if (isOff("vibrance"))       filterPE.vibrance = falsePE.vibrance;
                if (isOff("distortion"))   filterPE.distortion = falsePE.distortion;
                if (isOff("cacorrection")) filterPE.cacorrection = falsePE.cacorrection;
                if (isOff("vignetting"))   filterPE.vignetting = falsePE.vignetting;
                if (isOff("lensProf"))     filterPE.lensProf = falsePE.lensProf;
                if (isOff("coarse"))       filterPE.coarse = falsePE.coarse;
                if (isOff("rotate"))       filterPE.rotate = falsePE.rotate;
                if (isOff("crop"))         filterPE.crop = falsePE.crop;
                if (isOff("resize"))       filterPE.resize = falsePE.resize;
                if (isOff("prsharpening")) filterPE.prsharpening = falsePE.prsharpening;
                if (isOff("perspective"))  filterPE.perspective = falsePE.perspective;
                if (isOff("commonTrans"))  filterPE.commonTrans = falsePE.commonTrans;
                if (isOff("gradient"))     filterPE.gradient = falsePE.gradient;
                if (isOff("framing"))      filterPE.framing = falsePE.framing;
                if (isOff("retinex")) filterPE.retinex = falsePE.retinex;
                if (isOff("wavelet")) filterPE.wavelet = falsePE.wavelet;
                if (isOff("spot"))    filterPE.spot = falsePE.spot;
                if (isOff("cg"))      filterPE.cg = falsePE.cg;
                if (isOff("locallab")) filterPE.locallab = falsePE.locallab;

                rtengine::procparams::ProcParams filteredPP;
                filterPE.combine(filteredPP, srcPP, true);
                rtengine::procparams::PartialProfile pp(&filteredPP, &filterPE);
                clipboard.setPartialProfile(pp);
            }
        });
        // Copy filter dropdown — inset to the right of the copy button
        Gtk::MenuButton* copyFilterBtn = Gtk::manage(new Gtk::MenuButton());
        copyFilterBtn->set_image(*Gtk::manage(new RTImage("copy-filter", Gtk::ICON_SIZE_MENU)));
        copyFilterBtn->set_relief(Gtk::RELIEF_NONE);
        copyFilterBtn->set_tooltip_markup(M("FILEBROWSER_COPYPROFILE_SETTINGS"));
        copyFilterBtn->set_popup(*editorCopyFilterMenu_);

        // Group copy + filter as a visually joined button pair
        Gtk::Box* copyGroup = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 0));
        copyGroup->set_name("CopySettingsGroup");
        copyGroup->pack_start(*copySettingsBtn, Gtk::PACK_SHRINK);
        copyGroup->pack_start(*copyFilterBtn, Gtk::PACK_SHRINK);
        applyCSS(copyGroup);

        // Screen-level CSS for the joined pair
        {
            static bool cssAdded = false;
            if (!cssAdded) {
                auto css = Gtk::CssProvider::create();
                css->load_from_data(
                    "#CopySettingsGroup {"
                    "  margin: 0; padding: 0;"
                    "}"
                    "#CopySettingsGroup > button:first-child {"
                    "  border-top-right-radius: 0; border-bottom-right-radius: 0;"
                    "  margin-right: -2px; padding-right: 1px;"
                    "}"
                    "#CopySettingsGroup > menubutton > button {"
                    "  border-top-left-radius: 0; border-bottom-left-radius: 0;"
                    "  margin-left: -2px; padding-left: 1px;"
                    "  min-width: 12px; padding-right: 2px;"
                    "}"
                    "#CopySettingsGroup > menubutton {"
                    "  margin: 0; padding: 0;"
                    "}"
                    "#CopySettingsGroup > menubutton image {"
                    "  min-width: 12px; min-height: 12px;"
                    "}"
                );
                Gtk::StyleContext::add_provider_for_screen(
                    Gdk::Screen::get_default(), css,
                    GTK_STYLE_PROVIDER_PRIORITY_USER + 100);
                cssAdded = true;
            }
        }
        filmstripActionBar->pack_start(*copyGroup, Gtk::PACK_SHRINK);

        // Edit in External Editor button
        if (!App::get().isGimpPlugin()) {
            auto* sep5 = Gtk::manage(new Gtk::Separator(Gtk::ORIENTATION_VERTICAL));
            applyCSS(sep5);
            filmstripActionBar->pack_start(*sep5, Gtk::PACK_SHRINK);

            Gtk::Button* extEditorBtn = Gtk::manage(new Gtk::Button());
            extEditorBtn->set_image(*Gtk::manage(new RTImage("external-editor", Gtk::ICON_SIZE_MENU)));
            extEditorBtn->set_relief(Gtk::RELIEF_NONE);
            extEditorBtn->set_tooltip_markup(M("MAIN_BUTTON_SENDTOEDITOR_TOOLTIP"));
            extEditorBtn->signal_clicked().connect(sigc::mem_fun(*this, &EditorPanel::sendToExternalPressed));
            applyCSS(extEditorBtn);
            filmstripActionBar->pack_start(*extEditorBtn, Gtk::PACK_SHRINK);
        }

        // Sort button with popup menu
        {
            auto* sep6 = Gtk::manage(new Gtk::Separator(Gtk::ORIENTATION_VERTICAL));
            applyCSS(sep6);
            filmstripActionBar->pack_start(*sep6, Gtk::PACK_SHRINK);

            filmstripSortBtn_ = Gtk::manage(new Gtk::MenuButton());
            filmstripSortBtn_->set_image(*Gtk::manage(new RTImage("menu-sort", Gtk::ICON_SIZE_MENU)));
            filmstripSortBtn_->set_relief(Gtk::RELIEF_NONE);
            filmstripSortBtn_->set_tooltip_markup(M("FILEBROWSER_POPUPSORTBY"));
            applyCSS(filmstripSortBtn_);

            filmstripSortMenu_ = Gtk::manage(new Gtk::Menu());

            // Sort order
            Gtk::RadioButtonGroup sortOrderGrp;
            filmstripSortOrder_[0] = Gtk::manage(new Gtk::RadioMenuItem(sortOrderGrp, M("SORT_ASCENDING")));
            filmstripSortOrder_[1] = Gtk::manage(new Gtk::RadioMenuItem(sortOrderGrp, M("SORT_DESCENDING")));
            filmstripSortMenu_->append(*filmstripSortOrder_[0]);
            filmstripSortMenu_->append(*filmstripSortOrder_[1]);

            filmstripSortMenu_->append(*Gtk::manage(new Gtk::SeparatorMenuItem()));

            // Sort methods
            Gtk::RadioButtonGroup sortMethodGrp;
            const Glib::ustring sortLabels[Options::SORT_METHOD_COUNT] = {
                M("SORT_BY_NAME"), M("SORT_BY_DATE"), M("SORT_BY_EXIF"),
                M("SORT_BY_RANK"), M("SORT_BY_LABEL"), M("SORT_BY_FILETYPE")
            };
            for (int i = 0; i < Options::SORT_METHOD_COUNT; i++) {
                filmstripSortMethod_[i] = Gtk::manage(new Gtk::RadioMenuItem(sortMethodGrp, sortLabels[i]));
                filmstripSortMenu_->append(*filmstripSortMethod_[i]);
            }

            // Set initial state from options
            const auto& opts = App::get().options();
            filmstripSortOrder_[opts.sortDescending ? 1 : 0]->set_active(true);
            if (opts.sortMethod >= 0 && opts.sortMethod < Options::SORT_METHOD_COUNT)
                filmstripSortMethod_[opts.sortMethod]->set_active(true);

            // Connect signals
            for (int i = 0; i < 2; i++)
                filmstripSortOrder_[i]->signal_toggled().connect(sigc::mem_fun(*this, &EditorPanel::filmstripSortChanged));
            for (int i = 0; i < Options::SORT_METHOD_COUNT; i++)
                filmstripSortMethod_[i]->signal_toggled().connect(sigc::mem_fun(*this, &EditorPanel::filmstripSortChanged));

            filmstripSortMenu_->show_all();
            filmstripSortBtn_->set_popup(*filmstripSortMenu_);
            filmstripActionBar->pack_start(*filmstripSortBtn_, Gtk::PACK_SHRINK);
        }
    }
    toolBarPanel->set_center_widget(*filmstripActionBar);

    // Preview channel buttons (R/G/B/L) moved to Options menu
    // toolBarPanel->pack_end   (*iareapanel->imageArea->previewModePanel, Gtk::PACK_SHRINK, 0);
    // toolBarPanel->pack_end   (*vsepz4, Gtk::PACK_SHRINK, 2);

    afterBox = Gtk::manage (new Gtk::Box (Gtk::ORIENTATION_VERTICAL));
    afterBox->pack_start (*iareapanel);

    beforeAfterBox = Gtk::manage (new Gtk::Box (Gtk::ORIENTATION_HORIZONTAL));
    beforeAfterBox->set_name ("BeforeAfterContainer");
    beforeAfterBox->pack_start (*afterBox);

    MyScrolledToolbar *stb1 = Gtk::manage(new MyScrolledToolbar());
    stb1->set_name("EditorToolbarTop");
    stb1->add(*toolBarPanel);
    // Offset toolbar from sidebars so buttons aren't hidden by the overlay
    stb1->set_margin_start(options.showHistory ? options.dirBrowserWidth : 0);
    stb1->set_margin_end(std::min(options.toolPanelWidth, 400));
    editbox->pack_start (*stb1, Gtk::PACK_SHRINK, 0);
    editorToolbarTop_ = stb1;

    // Build filter bar (shown/hidden via revealer)
    if (tbFilterBar) {
        Gtk::Box* filterBar = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 1));
        filterBar->set_name("EditorFilterBar");
        filterBar->set_halign(Gtk::ALIGN_CENTER);

        auto filterCss = Gtk::CssProvider::create();
        filterCss->load_from_data(
            "#EditorFilterBar { padding: 2px 6px; }"
            "#EditorFilterBar button { min-height: 0; min-width: 0; padding: 1px 1px; margin: 0; }"
            "#EditorFilterBar button image { margin: 0; padding: 0; }"
            "#EditorFilterBar label { margin: 0 2px; }"
            "#EditorFilterBar entry { min-height: 0; padding: 1px 4px; }"
        );
        auto applyFilterCss = [&filterCss](Gtk::Widget* w) {
            w->get_style_context()->add_provider(filterCss, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 200);
        };
        applyFilterCss(filterBar);

        auto connectToggle = [this](Gtk::ToggleButton* tb) {
            tb->signal_toggled().connect(sigc::mem_fun(*this, &EditorPanel::filterBarChanged));
        };

        // Clear all button (before ratings)
        fbClearAll = Gtk::manage(new Gtk::Button());
        fbClearAll->set_image(*Gtk::manage(new RTImage("filter-clear", Gtk::ICON_SIZE_MENU)));
        fbClearAll->set_relief(Gtk::RELIEF_NONE);
        fbClearAll->set_tooltip_markup(M("EDITOR_FILTER_CLEAR_TOOLTIP"));
        applyFilterCss(fbClearAll);
        fbClearAll->signal_clicked().connect(sigc::mem_fun(*this, &EditorPanel::filterBarClearAll));
        filterBar->pack_start(*fbClearAll, Gtk::PACK_SHRINK);

        auto* fsep0 = Gtk::manage(new Gtk::Separator(Gtk::ORIENTATION_VERTICAL));
        filterBar->pack_start(*fsep0, Gtk::PACK_SHRINK);

        // Rating group
        fbUnRanked = Gtk::manage(new Gtk::ToggleButton());
        fbUnRanked->set_image(*Gtk::manage(new RTImage("star-hollow-small", Gtk::ICON_SIZE_MENU)));
        fbUnRanked->set_relief(Gtk::RELIEF_NONE);
        fbUnRanked->set_tooltip_markup(M("FILEBROWSER_SHOWUNRANKHINT"));
        applyFilterCss(fbUnRanked);
        connectToggle(fbUnRanked);
        filterBar->pack_start(*fbUnRanked, Gtk::PACK_SHRINK);

        for (int i = 0; i < 5; i++) {
            fbRank[i] = Gtk::manage(new Gtk::ToggleButton());
            fbRank[i]->set_image(*Gtk::manage(new RTImage("star-small", Gtk::ICON_SIZE_MENU)));
            fbRank[i]->set_relief(Gtk::RELIEF_NONE);
            Glib::ustring convergent = Glib::ustring::compose("FILEBROWSER_SHOWRANK%1HINT", i + 1);
            fbRank[i]->set_tooltip_markup(M(convergent));
            applyFilterCss(fbRank[i]);
            connectToggle(fbRank[i]);
            filterBar->pack_start(*fbRank[i], Gtk::PACK_SHRINK);
        }

        Gtk::Separator* fsep1 = Gtk::manage(new Gtk::Separator(Gtk::ORIENTATION_VERTICAL));
        filterBar->pack_start(*fsep1, Gtk::PACK_SHRINK);

        // Color label group
        std::array<std::string, 6> clabelIcons = {"circle-empty-gray-small", "circle-red-small", "circle-yellow-small", "circle-green-small", "circle-blue-small", "circle-purple-small"};
        std::array<Glib::ustring, 6> clabelHints = {
            "FILEBROWSER_SHOWUNCOLORHINT",
            "FILEBROWSER_SHOWCOLORLABEL1HINT",
            "FILEBROWSER_SHOWCOLORLABEL2HINT",
            "FILEBROWSER_SHOWCOLORLABEL3HINT",
            "FILEBROWSER_SHOWCOLORLABEL4HINT",
            "FILEBROWSER_SHOWCOLORLABEL5HINT"
        };

        fbUnCLabeled = Gtk::manage(new Gtk::ToggleButton());
        fbUnCLabeled->set_image(*Gtk::manage(new RTImage(clabelIcons[0], Gtk::ICON_SIZE_MENU)));
        fbUnCLabeled->set_relief(Gtk::RELIEF_NONE);
        fbUnCLabeled->set_tooltip_markup(M(clabelHints[0]));
        applyFilterCss(fbUnCLabeled);
        connectToggle(fbUnCLabeled);
        filterBar->pack_start(*fbUnCLabeled, Gtk::PACK_SHRINK);

        for (int i = 0; i < 5; i++) {
            fbCLabel[i] = Gtk::manage(new Gtk::ToggleButton());
            fbCLabel[i]->set_image(*Gtk::manage(new RTImage(clabelIcons[i + 1], Gtk::ICON_SIZE_MENU)));
            fbCLabel[i]->set_relief(Gtk::RELIEF_NONE);
            fbCLabel[i]->set_tooltip_markup(M(clabelHints[i + 1]));
            applyFilterCss(fbCLabel[i]);
            connectToggle(fbCLabel[i]);
            filterBar->pack_start(*fbCLabel[i], Gtk::PACK_SHRINK);
        }

        Gtk::Separator* fsep2 = Gtk::manage(new Gtk::Separator(Gtk::ORIENTATION_VERTICAL));
        filterBar->pack_start(*fsep2, Gtk::PACK_SHRINK);

        // Edited / Recently saved group
        fbEdited[0] = Gtk::manage(new Gtk::ToggleButton());
        fbEdited[0]->set_image(*Gtk::manage(new RTImage("tick-hollow-small", Gtk::ICON_SIZE_MENU)));
        fbEdited[0]->set_relief(Gtk::RELIEF_NONE);
        fbEdited[0]->set_tooltip_markup(M("FILEBROWSER_SHOWEDITEDNOTHINT"));
        applyFilterCss(fbEdited[0]);
        connectToggle(fbEdited[0]);
        filterBar->pack_start(*fbEdited[0], Gtk::PACK_SHRINK);

        fbEdited[1] = Gtk::manage(new Gtk::ToggleButton());
        fbEdited[1]->set_image(*Gtk::manage(new RTImage("tick-small", Gtk::ICON_SIZE_MENU)));
        fbEdited[1]->set_relief(Gtk::RELIEF_NONE);
        fbEdited[1]->set_tooltip_markup(M("FILEBROWSER_SHOWEDITEDHINT"));
        applyFilterCss(fbEdited[1]);
        connectToggle(fbEdited[1]);
        filterBar->pack_start(*fbEdited[1], Gtk::PACK_SHRINK);

        fbRecentlySaved[0] = Gtk::manage(new Gtk::ToggleButton());
        fbRecentlySaved[0]->set_image(*Gtk::manage(new RTImage("saved-no-small", Gtk::ICON_SIZE_MENU)));
        fbRecentlySaved[0]->set_relief(Gtk::RELIEF_NONE);
        fbRecentlySaved[0]->set_tooltip_markup(M("FILEBROWSER_SHOWRECENTLYSAVEDNOTHINT"));
        applyFilterCss(fbRecentlySaved[0]);
        connectToggle(fbRecentlySaved[0]);
        filterBar->pack_start(*fbRecentlySaved[0], Gtk::PACK_SHRINK);

        fbRecentlySaved[1] = Gtk::manage(new Gtk::ToggleButton());
        fbRecentlySaved[1]->set_image(*Gtk::manage(new RTImage("saved-yes-small", Gtk::ICON_SIZE_MENU)));
        fbRecentlySaved[1]->set_relief(Gtk::RELIEF_NONE);
        fbRecentlySaved[1]->set_tooltip_markup(M("FILEBROWSER_SHOWRECENTLYSAVEDHINT"));
        applyFilterCss(fbRecentlySaved[1]);
        connectToggle(fbRecentlySaved[1]);
        filterBar->pack_start(*fbRecentlySaved[1], Gtk::PACK_SHRINK);

        Gtk::Separator* fsep3 = Gtk::manage(new Gtk::Separator(Gtk::ORIENTATION_VERTICAL));
        filterBar->pack_start(*fsep3, Gtk::PACK_SHRINK);

        // Search entry
        fbSearchEntry = Gtk::manage(new Gtk::Entry());
        fbSearchEntry->set_placeholder_text(M("EDITOR_FILTER_SEARCH_PLACEHOLDER"));
        fbSearchEntry->set_width_chars(15);
        applyFilterCss(fbSearchEntry);
        fbSearchEntry->signal_changed().connect(sigc::mem_fun(*this, &EditorPanel::filterBarChanged));
        filterBar->pack_start(*fbSearchEntry, Gtk::PACK_SHRINK);

        filterBarRevealer = Gtk::manage(new Gtk::Revealer());
        filterBarRevealer->set_transition_type(Gtk::REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
        filterBarRevealer->set_transition_duration(200);
        filterBarRevealer->add(*filterBar);
        filterBarRevealer->set_reveal_child(false);

        editbox->pack_start(*filterBarRevealer, Gtk::PACK_SHRINK, 0);
    }

    // Image area goes directly in editbox; sidebars will overlay at hpanedr level
    editbox->pack_start(*beforeAfterBox);
    setExpandAlignProperties(leftbox, false, true, Gtk::ALIGN_START, Gtk::ALIGN_FILL);
    leftbox->set_size_request(options.dirBrowserWidth, -1);

    // build right side panel
    vboxright = new FixedWidthBox(options.toolPanelWidth);

    vsubboxright = new Gtk::Box (Gtk::ORIENTATION_VERTICAL, 0);
//    int rightsize = options.fontSize * 44;
//    vsubboxright->set_size_request (rightsize, rightsize - 50);
    vsubboxright->set_size_request (std::min(options.toolPanelWidth, 400), -1);

    // EXIF info strip above histogram — hover shows full info overlay on preview
    exifInfo = Gtk::manage(new Gtk::Label());
    exifInfo->set_name("ExifInfoLabel");
    exifInfo->set_markup("<span size='small'>  </span>");
    exifInfo->set_halign(Gtk::ALIGN_CENTER);
    exifInfo->set_margin_top(2);
    exifInfo->set_margin_bottom(2);

    Gtk::EventBox* exifInfoEventBox = Gtk::manage(new Gtk::EventBox());
    exifInfoEventBox->add(*exifInfo);
    exifInfoEventBox->set_above_child(true);
    exifInfoEventBox->set_visible_window(false);
    exifInfoEventBox->add_events(Gdk::ENTER_NOTIFY_MASK | Gdk::LEAVE_NOTIFY_MASK);
    exifInfoEventBox->signal_enter_notify_event().connect([this](GdkEventCrossing*) -> bool {
        iareapanel->imageArea->infoEnabled(true);
        return false;
    });
    exifInfoEventBox->signal_leave_notify_event().connect([this](GdkEventCrossing*) -> bool {
        iareapanel->imageArea->infoEnabled(false);
        return false;
    });
    // Row for histogram (when positioned on the right side)
    histogramRow_ = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 0));
    histogramRow_->set_margin_top(0);
    histogramRow_->set_margin_bottom(0);
    vsubboxright->pack_start(*histogramRow_, Gtk::PACK_SHRINK);

    // EXIF info between histogram and mode bar
    exifInfo->set_margin_top(0);
    exifInfo->set_margin_bottom(0);
    vsubboxright->pack_start(*exifInfoEventBox, Gtk::PACK_SHRINK);

    // Mode button bar + stack
    vsubboxright->pack_start (*tpc->modeButtonBar, Gtk::PACK_SHRINK, 0);
    vsubboxright->pack_start (*tpc->modeStack);

    // Add PresetListPanel's scrolled window directly to the mode stack as "presets"
    tpc->modeStack->add(*presetListPanel->getWidget(), "presets");

    vboxright->pack_start (*vsubboxright);

    // Save buttons
    Gtk::Grid *iops = Gtk::manage(new Gtk::Grid());
    iops->set_name ("IopsPanel");
    iops->set_orientation (Gtk::ORIENTATION_HORIZONTAL);
    iops->set_row_spacing (2);
    iops->set_column_spacing (2);

    Gtk::Image *saveButtonImage =  Gtk::manage (new RTImage ("save", Gtk::ICON_SIZE_LARGE_TOOLBAR));
    saveimgas = Gtk::manage (new Gtk::Button ());
    saveimgas->set_relief(Gtk::RELIEF_NONE);
    saveimgas->add (*saveButtonImage);
    saveimgas->set_tooltip_markup (M ("MAIN_BUTTON_SAVE_TOOLTIP"));
    setExpandAlignProperties (saveimgas, false, false, Gtk::ALIGN_CENTER, Gtk::ALIGN_FILL);

    Gtk::Image *queueButtonImage = Gtk::manage (new RTImage ("gears", Gtk::ICON_SIZE_LARGE_TOOLBAR));
    queueimg = Gtk::manage (new Gtk::Button ());
    queueimg->set_relief(Gtk::RELIEF_NONE);
    queueimg->add (*queueButtonImage);
    queueimg->set_tooltip_markup (M ("MAIN_BUTTON_PUTTOQUEUE_TOOLTIP"));
    setExpandAlignProperties (queueimg, false, false, Gtk::ALIGN_CENTER, Gtk::ALIGN_FILL);

    send_to_external = Gtk::manage(new PopUpButton("", false));
    send_to_external->set_tooltip_text(M("MAIN_BUTTON_SENDTOEDITOR_TOOLTIP"));
    send_to_external->setEmptyImage("palette-brush");
    setExpandAlignProperties(send_to_external->buttonGroup, false, false, Gtk::ALIGN_CENTER, Gtk::ALIGN_FILL);
    updateExternalEditorWidget(
        options.externalEditorIndex >= 0 ? options.externalEditorIndex : options.externalEditors.size(),
        options.externalEditors
    );
    send_to_external->show();

    // Status box
    progressLabel = Gtk::manage (new MyProgressBar (300));
    progressLabel->set_show_text (true);
    setExpandAlignProperties (progressLabel, true, false, Gtk::ALIGN_START, Gtk::ALIGN_FILL);
    progressLabel->set_fraction (0.0);
    progressLabel->set_no_show_all(true);
    progressLabel->hide();

    // tbRightPanel_1
    tbRightPanel_1 = Gtk::manage(new Gtk::ToggleButton());
    iRightPanel_1_Show = new RTImage ("panel-to-left", Gtk::ICON_SIZE_LARGE_TOOLBAR);
    iRightPanel_1_Hide = new RTImage ("panel-to-right", Gtk::ICON_SIZE_LARGE_TOOLBAR);
    tbRightPanel_1->set_relief (Gtk::RELIEF_NONE);
    tbRightPanel_1->set_active (true);
    tbRightPanel_1->set_tooltip_markup (M ("MAIN_TOOLTIP_SHOWHIDERP1"));
    tbRightPanel_1->set_image (*iRightPanel_1_Hide);
    setExpandAlignProperties (tbRightPanel_1, false, false, Gtk::ALIGN_CENTER, Gtk::ALIGN_FILL);

    // ShowHideSidePanels
    tbShowHideSidePanels = Gtk::manage(new Gtk::ToggleButton());
    iShowHideSidePanels = new RTImage ("crossed-arrows-out", Gtk::ICON_SIZE_LARGE_TOOLBAR);
    iShowHideSidePanels_exit = new RTImage ("crossed-arrows-in", Gtk::ICON_SIZE_LARGE_TOOLBAR);
    tbShowHideSidePanels->set_relief (Gtk::RELIEF_NONE);
    tbShowHideSidePanels->set_active (false);
    tbShowHideSidePanels->set_tooltip_markup (M ("MAIN_BUTTON_SHOWHIDESIDEPANELS_TOOLTIP"));
    tbShowHideSidePanels->set_image (*iShowHideSidePanels);
    setExpandAlignProperties (tbShowHideSidePanels, false, false, Gtk::ALIGN_CENTER, Gtk::ALIGN_FILL);

    navPrev = navNext = navSync = nullptr;

    if (!App::get().isSimpleEditor() && !options.tabbedUI) {
        // Navigation buttons — smaller icons, centered in toolbar
        Gtk::Image *navPrevImage = Gtk::manage (new RTImage ("nav-prev", Gtk::ICON_SIZE_SMALL_TOOLBAR));
        navPrev = Gtk::manage (new Gtk::Button ());
        navPrev->add (*navPrevImage);
        navPrev->set_relief (Gtk::RELIEF_NONE);
        navPrev->set_tooltip_markup (M ("MAIN_BUTTON_NAVPREV_TOOLTIP"));
        setExpandAlignProperties (navPrev, false, false, Gtk::ALIGN_CENTER, Gtk::ALIGN_CENTER);

        Gtk::Image *navNextImage = Gtk::manage (new RTImage ("nav-next", Gtk::ICON_SIZE_SMALL_TOOLBAR));
        navNext = Gtk::manage (new Gtk::Button ());
        navNext->add (*navNextImage);
        navNext->set_relief (Gtk::RELIEF_NONE);
        navNext->set_tooltip_markup (M ("MAIN_BUTTON_NAVNEXT_TOOLTIP"));
        setExpandAlignProperties (navNext, false, false, Gtk::ALIGN_CENTER, Gtk::ALIGN_CENTER);

    }

    // ==================  PACKING THE BOTTOM WIDGETS =================
    // Layout: [left section] <expand> [centered nav] <expand> [right section]

    // Color management toolbar (buttons moved to Options menu, but keep object alive)
    colorMgmtToolBar.reset (new ColorManagementToolbar (ipc));

    int col = 0;

    // --- Left section ---
    iops->attach(*hidehp, col++, 0, 1, 1);
    // send_to_external moved to filmstrip action bar
    iops->attach(*progressLabel, col++, 0, 1, 1);

    // --- Left spacer (expands to push nav buttons to center) ---
    Gtk::Label* spacerLeft = Gtk::manage(new Gtk::Label(""));
    setExpandAlignProperties(spacerLeft, true, false, Gtk::ALIGN_FILL, Gtk::ALIGN_FILL);
    iops->attach(*spacerLeft, col++, 0, 1, 1);

    // --- Centered navigation buttons with zoom panel in center ---
    if (!App::get().isSimpleEditor() && !options.tabbedUI) {
        iops->attach(*navPrev, col++, 0, 1, 1);
    }
    iops->attach(*iareapanel->imageArea->zoomPanel, col++, 0, 1, 1);
    if (!App::get().isSimpleEditor() && !options.tabbedUI) {
        iops->attach(*navNext, col++, 0, 1, 1);
    }

    // --- Right spacer (expands to push nav buttons to center) ---
    Gtk::Label* spacerRight = Gtk::manage(new Gtk::Label(""));
    setExpandAlignProperties(spacerRight, true, false, Gtk::ALIGN_FILL, Gtk::ALIGN_FILL);
    iops->attach(*spacerRight, col++, 0, 1, 1);

    // --- Right section ---
    iops->attach(*tbRightPanel_1, col++, 0, 1, 1);

    MyScrolledToolbar *stb2 = Gtk::manage(new MyScrolledToolbar());
    stb2->set_name("EditorToolbarBottom");
    stb2->add(*iops);

    editbox->pack_start (*stb2, Gtk::PACK_SHRINK, 0);
    editorToolbarBottom_ = stb2;
    editbox->show_all ();

    // build screen
    hpanedr = Gtk::manage (new Gtk::Overlay ());
    hpanedr->set_name ("EditorRightPaned");
    leftbox->reference ();
    vboxright->reference ();
    vboxright->set_name ("EditorModules");

    Gtk::Paned *viewpaned = Gtk::manage (new Gtk::Paned (Gtk::ORIENTATION_VERTICAL));
    viewpaned->set_name("EditorViewPaned");
    // Hide the paned separator between filmstrip and editor via screen-level CSS
    {
        static auto sepCss = Gtk::CssProvider::create();
        sepCss->load_from_data(
            "paned#EditorViewPaned > separator {"
            "  min-height: 0; min-width: 0; margin: 0; padding: 0;"
            "  border: none; background: none; background-color: transparent;"
            "  background-image: none; box-shadow: none; opacity: 0;"
            "}"
        );
        Gtk::StyleContext::add_provider_for_screen(
            Gdk::Screen::get_default(), sepCss,
            GTK_STYLE_PROVIDER_PRIORITY_USER + 200);
    }
    fPanel = filePanel;

    if (filePanel) {
        catalogPane = new Gtk::Paned();
        // Size to fit one row of filmstrip thumbnails without vertical scrollbar.
        // thumbSizeTab is the thumbnail height; add padding for borders + hscrollbar.
        int filmstripHeight = std::min(options.thumbSizeTab, 115) + 8;
        catalogPane->set_size_request(-1, filmstripHeight);
        // Inset filmstrip so sidebars don't overlap its content
        catalogPane->set_margin_start(options.showHistory ? options.dirBrowserWidth : 0);
        catalogPane->set_margin_end(std::min(options.toolPanelWidth, 400));
        viewpaned->pack1 (*catalogPane, false, true);
    }

    viewpaned->pack2(*editbox, true, true);

    // Album grid view
    albumViewBox_ = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
    albumViewBox_->set_name("AlbumView");

    // Album view header bar
    albumViewHeader_ = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 8));
    albumViewHeader_->set_name("AlbumViewHeader");

    Gtk::Button* backBtn = Gtk::manage(new Gtk::Button());
    backBtn->set_image(*Gtk::manage(new RTImage("arrow2-left", Gtk::ICON_SIZE_LARGE_TOOLBAR)));
    backBtn->set_relief(Gtk::RELIEF_NONE);
    backBtn->set_tooltip_text(M("ALBUM_VIEW_BACK"));
    backBtn->signal_clicked().connect(sigc::mem_fun(*this, &EditorPanel::hideAlbumView));
    albumViewHeader_->pack_start(*backBtn, Gtk::PACK_SHRINK);

    auto albumHeaderCss = Gtk::CssProvider::create();
    albumHeaderCss->load_from_data(
        "#AlbumViewHeader { padding: 4px 8px; }"
        "#AlbumViewHeader label { padding: 0; margin: 0; }"
        "#AlbumViewGrid { padding: 8px; }"
        "#AlbumViewItem { padding: 4px; }"
        "#AlbumViewHeader .linked radiobutton { padding: 2px 6px; min-height: 0; min-width: 0; }"
        "#AlbumViewHeader scale { min-width: 100px; }"
    );
    albumViewHeader_->get_style_context()->add_provider(albumHeaderCss, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 200);

    // View mode segmented control (Grid | Fit | Collage) — left side after back button
    {
        albumModeGrid_ = Gtk::manage(new Gtk::RadioButton(albumModeGroup_));
        albumModeGrid_->set_mode(false);
        albumModeGrid_->set_image(*Gtk::manage(new RTImage("album-view-grid", Gtk::ICON_SIZE_MENU)));
        albumModeGrid_->set_tooltip_text(M("ALBUM_VIEW_MODE_GRID"));
        albumModeGrid_->set_active(true);

        albumModeFit_ = Gtk::manage(new Gtk::RadioButton(albumModeGroup_));
        albumModeFit_->set_mode(false);
        albumModeFit_->set_image(*Gtk::manage(new RTImage("album-view-fit", Gtk::ICON_SIZE_MENU)));
        albumModeFit_->set_tooltip_text(M("ALBUM_VIEW_MODE_FIT"));

        albumModeCollage_ = Gtk::manage(new Gtk::RadioButton(albumModeGroup_));
        albumModeCollage_->set_mode(false);
        albumModeCollage_->set_image(*Gtk::manage(new RTImage("album-view-collage", Gtk::ICON_SIZE_MENU)));
        albumModeCollage_->set_tooltip_text(M("ALBUM_VIEW_MODE_COLLAGE"));

        Gtk::Box* modeBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 0));
        modeBox->get_style_context()->add_class("linked");
        modeBox->pack_start(*albumModeGrid_, Gtk::PACK_SHRINK);
        modeBox->pack_start(*albumModeFit_, Gtk::PACK_SHRINK);
        modeBox->pack_start(*albumModeCollage_, Gtk::PACK_SHRINK);
        modeBox->get_style_context()->add_provider(albumHeaderCss, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 200);

        albumModeGrid_->signal_toggled().connect(sigc::mem_fun(*this, &EditorPanel::albumViewModeChanged));
        albumModeFit_->signal_toggled().connect(sigc::mem_fun(*this, &EditorPanel::albumViewModeChanged));
        albumModeCollage_->signal_toggled().connect(sigc::mem_fun(*this, &EditorPanel::albumViewModeChanged));

        albumViewHeader_->pack_start(*modeBox, Gtk::PACK_SHRINK);
    }

    // Info toggle button
    albumInfoToggle_ = Gtk::manage(new Gtk::ToggleButton());
    albumInfoToggle_->set_image(*Gtk::manage(new RTImage("info", Gtk::ICON_SIZE_MENU)));
    albumInfoToggle_->set_relief(Gtk::RELIEF_NONE);
    albumInfoToggle_->set_tooltip_text(M("ALBUM_VIEW_INFO_TOGGLE"));
    albumInfoToggle_->set_active(albumShowInfo_);
    albumInfoToggle_->signal_toggled().connect(sigc::mem_fun(*this, &EditorPanel::albumInfoToggled));
    albumViewHeader_->pack_start(*albumInfoToggle_, Gtk::PACK_SHRINK);

    // Zoom slider
    albumZoomSlider_ = Gtk::manage(new Gtk::Scale(Gtk::ORIENTATION_HORIZONTAL));
    albumZoomSlider_->set_range(60, 400);
    albumZoomSlider_->set_value(albumThumbHeight_);
    albumZoomSlider_->set_draw_value(false);
    albumZoomSlider_->set_size_request(120, -1);
    albumZoomSlider_->get_style_context()->add_provider(albumHeaderCss, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 200);
    albumZoomConn_ = albumZoomSlider_->signal_value_changed().connect(sigc::mem_fun(*this, &EditorPanel::albumZoomChanged));
    albumViewHeader_->pack_start(*albumZoomSlider_, Gtk::PACK_SHRINK);

    // Album sort button
    {
        albumSortBtn_ = Gtk::manage(new Gtk::MenuButton());
        albumSortBtn_->set_image(*Gtk::manage(new RTImage("menu-sort", Gtk::ICON_SIZE_MENU)));
        albumSortBtn_->set_relief(Gtk::RELIEF_NONE);
        albumSortBtn_->set_tooltip_markup(M("FILEBROWSER_POPUPSORTBY"));

        albumSortMenu_ = Gtk::manage(new Gtk::Menu());

        Gtk::RadioButtonGroup aOrderGrp;
        albumSortOrder_[0] = Gtk::manage(new Gtk::RadioMenuItem(aOrderGrp, M("SORT_ASCENDING")));
        albumSortOrder_[1] = Gtk::manage(new Gtk::RadioMenuItem(aOrderGrp, M("SORT_DESCENDING")));
        albumSortMenu_->append(*albumSortOrder_[0]);
        albumSortMenu_->append(*albumSortOrder_[1]);

        albumSortMenu_->append(*Gtk::manage(new Gtk::SeparatorMenuItem()));

        Gtk::RadioButtonGroup aMethodGrp;
        const Glib::ustring aLabels[Options::SORT_METHOD_COUNT] = {
            M("SORT_BY_NAME"), M("SORT_BY_DATE"), M("SORT_BY_EXIF"),
            M("SORT_BY_RANK"), M("SORT_BY_LABEL"), M("SORT_BY_FILETYPE")
        };
        for (int i = 0; i < Options::SORT_METHOD_COUNT; i++) {
            albumSortMethod_[i] = Gtk::manage(new Gtk::RadioMenuItem(aMethodGrp, aLabels[i]));
            albumSortMenu_->append(*albumSortMethod_[i]);
        }

        const auto& opts = App::get().options();
        albumSortOrder_[opts.sortDescending ? 1 : 0]->set_active(true);
        if (opts.sortMethod >= 0 && opts.sortMethod < Options::SORT_METHOD_COUNT)
            albumSortMethod_[opts.sortMethod]->set_active(true);

        for (int i = 0; i < 2; i++)
            albumSortOrder_[i]->signal_toggled().connect(sigc::mem_fun(*this, &EditorPanel::albumSortChanged));
        for (int i = 0; i < Options::SORT_METHOD_COUNT; i++)
            albumSortMethod_[i]->signal_toggled().connect(sigc::mem_fun(*this, &EditorPanel::albumSortChanged));

        albumSortMenu_->show_all();
        albumSortBtn_->set_popup(*albumSortMenu_);
        albumViewHeader_->pack_start(*albumSortBtn_, Gtk::PACK_SHRINK);
    }

    // Album name — right side, expands to fill
    albumNameLabel_ = Gtk::manage(new Gtk::Label());
    albumNameLabel_->set_halign(Gtk::ALIGN_END);
    albumNameLabel_->get_style_context()->add_provider(albumHeaderCss, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 200);
    albumViewHeader_->pack_start(*albumNameLabel_, Gtk::PACK_EXPAND_WIDGET);

    // Photo count — far right
    albumCountLabel_ = Gtk::manage(new Gtk::Label());
    albumCountLabel_->set_halign(Gtk::ALIGN_END);
    albumCountLabel_->get_style_context()->add_provider(albumHeaderCss, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 200);
    albumViewHeader_->pack_end(*albumCountLabel_, Gtk::PACK_SHRINK);

    albumViewBox_->pack_start(*albumViewHeader_, Gtk::PACK_SHRINK);
    albumViewBox_->pack_start(*Gtk::manage(new Gtk::Separator(Gtk::ORIENTATION_HORIZONTAL)), Gtk::PACK_SHRINK);

    // Album view thumbnail grid
    albumViewScrolled_ = Gtk::manage(new Gtk::ScrolledWindow());
    albumViewScrolled_->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);

    albumViewGrid_ = Gtk::manage(new Gtk::FlowBox());
    albumViewGrid_->set_name("AlbumViewGrid");
    albumViewGrid_->set_homogeneous(true);
    albumViewGrid_->set_column_spacing(4);
    albumViewGrid_->set_row_spacing(4);
    albumViewGrid_->set_min_children_per_line(1);
    albumViewGrid_->set_max_children_per_line(50);
    albumViewGrid_->set_selection_mode(Gtk::SELECTION_SINGLE);
    albumViewGrid_->set_activate_on_single_click(false);
    albumViewGrid_->set_valign(Gtk::ALIGN_START);
    albumViewGrid_->get_style_context()->add_provider(albumHeaderCss, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 200);

    // Double-click to open image from grid (defer cacheMgr work off main thread)
    albumViewGrid_->signal_child_activated().connect([this](Gtk::FlowBoxChild* child) {
        int idx = child->get_index();
        if (idx < 0 || idx >= static_cast<int>(currentAlbumFiles_.size())) return;
        Glib::ustring filePath = currentAlbumFiles_[idx];
        hideAlbumView();
        std::thread([this, filePath]() {
            Thumbnail* thm = cacheMgr->getEntry(filePath);
            if (thm) {
                Glib::signal_idle().connect_once([this, thm]() {
                    if (fPanel && fPanel->fileCatalog) {
                        fPanel->fileCatalog->openRequested({thm});
                    } else {
                        thm->decreaseRef();
                    }
                });
            }
        }).detach();
    });

    // Collage DrawingArea
    albumCollageArea_ = Gtk::manage(new Gtk::DrawingArea());
    albumCollageArea_->set_name("AlbumCollageArea");
    albumCollageArea_->signal_draw().connect(sigc::mem_fun(*this, &EditorPanel::onCollageAreaDraw));
    albumCollageArea_->add_events(Gdk::BUTTON_PRESS_MASK);
    albumCollageArea_->signal_button_press_event().connect(sigc::mem_fun(*this, &EditorPanel::onCollageAreaClick));

    // Stack to switch between FlowBox grid and Collage DrawingArea
    albumGridStack_ = Gtk::manage(new Gtk::Stack());
    albumGridStack_->set_transition_type(Gtk::STACK_TRANSITION_TYPE_CROSSFADE);
    albumGridStack_->set_transition_duration(200);
    albumGridStack_->add(*albumViewGrid_, "grid");
    albumGridStack_->add(*albumCollageArea_, "collage");
    albumGridStack_->set_visible_child("grid");

    albumViewScrolled_->add(*albumGridStack_);
    albumViewBox_->pack_start(*albumViewScrolled_);

    // Wrap editor view and album view in a Gtk::Stack
    albumViewStack_ = Gtk::manage(new Gtk::Stack());
    albumViewStack_->set_transition_type(Gtk::STACK_TRANSITION_TYPE_CROSSFADE);
    albumViewStack_->set_transition_duration(250);
    albumViewStack_->add(*viewpaned, "editor");
    albumViewStack_->add(*albumViewBox_, "album");
    albumViewStack_->set_visible_child("editor");

    // Overlay layout: image area fills everything, sidebars float at edges
    hpanedr->add(*albumViewStack_);

    if (!options.showHistory) {
        leftbox->set_no_show_all(true);
    }

    // Sidebars float at full height over the entire editor (filmstrip + image)
    hpanedr->add_overlay(*leftbox);
    hpanedr->add_overlay(*vboxright);

    // Force overlay children to span the full height of the overlay
    hpanedr->signal_get_child_position().connect(
        [this](Gtk::Widget* child, Gdk::Rectangle& alloc) -> bool {
            int overlayW = hpanedr->get_allocated_width();
            int overlayH = hpanedr->get_allocated_height();
            int minW = 0, natW = 0;
            child->get_preferred_width(minW, natW);

            // Offset sidebars from bottom toolbar so toggle buttons stay accessible
            int botOff = editorToolbarBottom_ ? editorToolbarBottom_->get_allocated_height() : 0;
            int sideH = std::max(1, overlayH - botOff);

            if (child == leftbox) {
                alloc.set_x(0);
                alloc.set_y(0);
                alloc.set_width(natW);
                alloc.set_height(sideH);
                return true;
            } else if (child == vboxright) {
                // Slide offset for view transition animation
                double eased;
                if (editorAnimIn_) {
                    eased = 1.0 - std::pow(1.0 - editorAnimFraction_, 3); // ease-out-cubic
                } else {
                    eased = editorAnimFraction_ * editorAnimFraction_ * editorAnimFraction_; // ease-in-cubic
                }
                int slideOffset = static_cast<int>(natW * (1.0 - eased));
                alloc.set_x(overlayW - natW + slideOffset);
                alloc.set_y(0);
                alloc.set_width(natW);
                alloc.set_height(sideH);
                return true;
            }
            return false;
        }, false);

    pack_start (*hpanedr);

    updateHistogramPosition (0, options.histogramPosition);

    show_all ();
    /*
        // save as dialog
        if (Glib::file_test (options.lastSaveAsPath, Glib::FILE_TEST_IS_DIR))
            saveAsDialog = new SaveAsDialog (options.lastSaveAsPath);
        else
            saveAsDialog = new SaveAsDialog (safe_get_user_picture_dir());

        saveAsDialog->set_default_size (options.saveAsDialogWidth, options.saveAsDialogHeight);
    */
    // connect listeners
    presetListPanel->setProfileChangeListener (tpc);
    history->setProfileChangeListener (tpc);
    history->setHistoryBeforeLineListener (this);
    tpc->addPParamsChangeListener (presetListPanel);
    tpc->addPParamsChangeListener (history);
    tpc->addPParamsChangeListener (this);
    iareapanel->imageArea->setCropGUIListener (tpc->getCropGUIListener());
    iareapanel->imageArea->setPointerMotionListener (navigator);
    iareapanel->imageArea->setImageAreaToolListener (tpc);
    tpc->setLevelingGridCallback([this](bool show) {
        if (iareapanel && iareapanel->imageArea && iareapanel->imageArea->mainCropWindow) {
            iareapanel->imageArea->mainCropWindow->setShowLevelingGrid(show);
        }
    });

    // Wire editor's folder browser to FilePanel's FileCatalog
    if (filePanel && filePanel->fileCatalog) {
        DirBrowser::DirSelectionSignal dirSel = editorDirBrowser_->dirSelected();
        dirSel.connect(sigc::mem_fun(filePanel->fileCatalog, &FileCatalog::dirSelected));
        dirSel.connect(sigc::mem_fun(editorRecentBrowser_, &RecentBrowser::dirSelected));
        dirSel.connect(sigc::mem_fun(editorPlacesBrowser_, &PlacesBrowser::dirSelected));
        dirSel.connect([this](const Glib::ustring& dir, const Glib::ustring&) {
            if (albumBrowser_) albumBrowser_->setCurrentDirectory(dir);

            // If album view is currently visible, refresh it with the new folder
            if (albumViewStack_ && albumViewStack_->get_visible_child_name() == "album") {
                // Scan the directory for image files
                std::vector<Glib::ustring> files;
                const auto& opts = App::get().options();
                try {
                    auto gdir = Gio::File::create_for_path(dir);
                    auto enumerator = gdir->enumerate_children("standard::name,standard::type,standard::is-hidden");
                    while (auto file = enumerator->next_file()) {
                        if (!opts.fbShowHidden && file->is_hidden()) continue;
                        if (file->get_file_type() == Gio::FILE_TYPE_DIRECTORY) continue;
                        const Glib::ustring fname = file->get_name();
                        const auto lastdot = fname.find_last_of('.');
                        if (lastdot >= fname.length() - 1) continue;
                        if (opts.parsedExtensionsSet.find(fname.substr(lastdot + 1).lowercase()) == opts.parsedExtensionsSet.end()) continue;
                        files.push_back(Glib::build_filename(dir, fname));
                    }
                } catch (...) {}
                std::sort(files.begin(), files.end());
                showAlbumView(Glib::path_get_basename(dir), files);
            }
        });
    }
    editorPlacesBrowser_->setDirSelector(sigc::mem_fun(editorDirBrowser_, &DirBrowser::selectDir));
    editorRecentBrowser_->setDirSelector(sigc::mem_fun(editorDirBrowser_, &DirBrowser::selectDir));

    // initialize components
    tpc->readOptions ();

    // connect event handlers
    beforeAfter->signal_toggled().connect ( sigc::mem_fun (*this, &EditorPanel::beforeAfterToggled) );
    hidehp->signal_toggled().connect ( sigc::mem_fun (*this, &EditorPanel::hideHistoryActivated) );
    tbRightPanel_1->signal_toggled().connect ( sigc::mem_fun (*this, &EditorPanel::tbRightPanel_1_toggled) );
    // saveimgas and queueimg removed from bottom bar
    send_to_external->signal_changed().connect(sigc::mem_fun(*this, &EditorPanel::sendToExternalChanged));
    send_to_external->signal_pressed().connect(sigc::mem_fun(*this, &EditorPanel::sendToExternalPressed));
    toggleHistogramProfile->signal_toggled().connect( sigc::mem_fun (*this, &EditorPanel::histogramProfile_toggled) );

    if (navPrev) {
        navPrev->signal_pressed().connect ( sigc::mem_fun (*this, &EditorPanel::openPreviousEditorImage) );
    }

    if (navNext) {
        navNext->signal_pressed().connect ( sigc::mem_fun (*this, &EditorPanel::openNextEditorImage) );
    }

    // navSync removed — replaced by zoom panel in center position

    ShowHideSidePanelsconn = tbShowHideSidePanels->signal_toggled().connect ( sigc::mem_fun (*this, &EditorPanel::toggleSidePanels), true);

    // Connect filmstrip "Save Image" context menu action
    if (fPanel && fPanel->fileCatalog && fPanel->fileCatalog->fileBrowser) {
        fPanel->fileCatalog->fileBrowser->save_image_requested().connect(
            sigc::mem_fun(*this, &EditorPanel::saveAsPressed));
    }

    if (tbTopPanel_1) {
        tbTopPanel_1->signal_toggled().connect ( sigc::mem_fun (*this, &EditorPanel::tbTopPanel_1_toggled) );
    }

}

EditorPanel::~EditorPanel ()
{
    if (placeholderCancel_) {
        placeholderCancel_->store(true);
        placeholderCancel_.reset();
    }

    idle_register.destroy();

    history->setHistoryBeforeLineListener (nullptr);
    // the order is important!
    iareapanel->setBeforeAfterViews (nullptr, iareapanel);
    delete iareapanel;
    iareapanel = nullptr;

    if (beforeIpc) {
        beforeIpc->stopProcessing ();
    }

    delete beforeIarea;
    beforeIarea = nullptr;

    if (beforeIpc) {
        beforeIpc->setPreviewImageListener (nullptr);
    }

    delete beforePreviewHandler;
    beforePreviewHandler = nullptr;

    if (beforeIpc) {
        rtengine::StagedImageProcessor* old = beforeIpc;
        beforeIpc = nullptr;
        std::thread([old]() {
            rtengine::StagedImageProcessor::destroy(old);
        }).detach();
    }

    beforeIpc = nullptr;

    close ();

    if (epih->pending) {
        epih->destroyed = true;
    } else {
        delete epih;
    }

    delete presetListPanel;
    delete tpc;

    delete navigatorDialog_;
    delete historyDialog_;
    delete navigator;
    delete history;
    delete editorPlacesPaned_;
    delete leftbox;
    delete vsubboxright;
    delete vboxright;

    //delete saveAsDialog;
    delete catalogPane;
    delete iTopPanel_1_Show;
    delete iTopPanel_1_Hide;
    delete iHistoryShow;
    delete iHistoryHide;
    delete iBeforeLockON;
    delete iBeforeLockOFF;
    delete iRightPanel_1_Show;
    delete iRightPanel_1_Hide;
    delete iShowHideSidePanels_exit;
    delete iShowHideSidePanels;
}

void EditorPanel::leftPaneButtonReleased (GdkEventButton * /*event*/)
{
    // Left sidebar is now an overlay with fixed width; no paned position to save.
}

void EditorPanel::rightPaneButtonReleased (GdkEventButton * /*event*/)
{
    // Right sidebar now auto-sizes via Box layout, no position to save.
}

void EditorPanel::writeOptions()
{
    if (presetListPanel) {
        presetListPanel->writeOptions();
    }

    if (tpc) {
        tpc->writeOptions();
    }
}


void EditorPanel::writeToolExpandedStatus (std::vector<int> &tpOpen)
{
    if (tpc) {
        tpc->writeToolExpandedStatus (tpOpen);
    }
}

void EditorPanel::updateShowtooltipVisibility (bool showtooltip)
{
    if (tpc) {
        tpc->updateShowtooltipVisibility (showtooltip);
    }
}

void EditorPanel::showTopPanel (bool show)
{
    if (tbTopPanel_1->get_active() != show) {
        tbTopPanel_1->set_active (show);
    }
}

void EditorPanel::setAspect ()
{
    const auto& options = App::get().options();
    leftbox->set_size_request(options.dirBrowserWidth, -1);

    // Sync editor's folder browser with the browser panel's current directory.
    // Only re-open if the directory actually changed to avoid scroll jumps.
    if (fPanel && fPanel->fileCatalog && editorDirBrowser_) {
        Glib::signal_idle().connect_once([this]() {
            if (realized && fPanel && fPanel->fileCatalog && editorDirBrowser_) {
                Glib::ustring browserDir = fPanel->fileCatalog->lastSelectedDir();
                if (!browserDir.empty() && browserDir != lastSyncedEditorDir_) {
                    lastSyncedEditorDir_ = browserDir;
                    editorDirBrowser_->open(browserDir);
                    if (albumBrowser_) albumBrowser_->setCurrentDirectory(browserDir);
                }
            }
        });
    }
}

void EditorPanel::showNavigatorDialog ()
{
    if (navigatorDialog_) {
        navigatorDialog_->toggleVisibility();
    }
}

void EditorPanel::showHistoryDialog ()
{
    if (historyDialog_) {
        historyDialog_->toggleVisibility();
    }
}

void EditorPanel::on_realize ()
{
    realized = true;
    Gtk::Box::on_realize ();
    // This line is needed to avoid autoexpansion of the window :-/
    //vboxright->set_size_request (options.toolPanelWidth, -1);
    tpc->updateToolState();

    // Initialize editor's folder browser
    editorDirBrowser_->fillDirTree();
    editorPlacesBrowser_->refreshPlacesList();

    // dirBrowserHeight no longer used — Places is now a Box, not a Paned

    // Sync with browser's current directory on first show
    if (fPanel && fPanel->fileCatalog) {
        Glib::ustring browserDir = fPanel->fileCatalog->lastSelectedDir();
        if (!browserDir.empty()) {
            editorDirBrowser_->open(browserDir);
            if (albumBrowser_) albumBrowser_->setCurrentDirectory(browserDir);
        }
    }
}

void EditorPanel::updateFilmstripStars(int highlightUpTo)
{
    for (int i = 0; i < 5; i++) {
        if (i < highlightUpTo) {
            filmstripRankBtns[i]->set_image(*Gtk::manage(new RTImage("star-gold-small", Gtk::ICON_SIZE_MENU)));
        } else {
            filmstripRankBtns[i]->set_image(*Gtk::manage(new RTImage("star-small", Gtk::ICON_SIZE_MENU)));
        }
    }
}

void EditorPanel::filmstripSortChanged ()
{
    if (!fPanel || !fPanel->fileCatalog || !fPanel->fileCatalog->fileBrowser) return;

    // Determine which method is active
    int method = 0;
    for (int i = 0; i < Options::SORT_METHOD_COUNT; i++) {
        if (filmstripSortMethod_[i]->get_active()) {
            method = i;
            break;
        }
    }
    bool descending = filmstripSortOrder_[1]->get_active();

    auto& opts = App::get().mut_options();
    opts.sortMethod = Options::SortMethod(method);
    opts.sortDescending = descending;

    fPanel->fileCatalog->fileBrowser->resort();
}

void EditorPanel::albumSortChanged ()
{
    // Determine which method is active
    int method = 0;
    for (int i = 0; i < Options::SORT_METHOD_COUNT; i++) {
        if (albumSortMethod_[i]->get_active()) {
            method = i;
            break;
        }
    }
    bool descending = albumSortOrder_[1]->get_active();

    // Sort the album files list
    if (currentAlbumFiles_.empty()) return;

    // We need Thumbnail objects to compare — use cacheMgr
    // Build sortable pairs of (filepath, Thumbnail*)
    struct SortEntry {
        Glib::ustring path;
        Thumbnail* thm;
    };
    std::vector<SortEntry> entries;
    for (const auto& f : currentAlbumFiles_) {
        Thumbnail* thm = cacheMgr->getEntry(f);
        entries.push_back({f, thm});
    }

    Options::SortMethod sm = Options::SortMethod(method);

    std::sort(entries.begin(), entries.end(), [sm, descending](const SortEntry& a, const SortEntry& b) {
        if (!a.thm || !b.thm) return false;
        int cmp = 0;
        switch (sm) {
        case Options::SORT_BY_NAME:
            return descending
                ? a.path.casefold() > b.path.casefold()
                : a.path.casefold() < b.path.casefold();
        case Options::SORT_BY_DATE:
            cmp = a.thm->getDateTime().compare(b.thm->getDateTime());
            break;
        case Options::SORT_BY_EXIF:
            cmp = a.thm->getExifString().compare(b.thm->getExifString());
            break;
        case Options::SORT_BY_RANK:
            cmp = a.thm->getRank() - b.thm->getRank();
            break;
        case Options::SORT_BY_LABEL:
            cmp = a.thm->getColorLabel() - b.thm->getColorLabel();
            break;
        default: break;
        }
        if (!cmp) {
            cmp = a.path.casefold().compare(b.path.casefold());
        }
        return descending ? cmp > 0 : cmp < 0;
    });

    // Rebuild the file list and release refs
    currentAlbumFiles_.clear();
    for (auto& e : entries) {
        currentAlbumFiles_.push_back(e.path);
        if (e.thm) e.thm->decreaseRef();
    }

    // Rebuild the view
    if (albumViewMode_ == AlbumViewMode::COLLAGE) {
        recalculateCollageLayout();
    } else {
        rebuildAlbumGrid();
    }
}

void EditorPanel::filterBarToggled()
{
    if (!filterBarRevealer) return;

    bool show = tbFilterBar->get_active();
    filterBarRevealer->set_reveal_child(show);

    if (!show) {
        filterBarClearAll();
    }
}

void EditorPanel::collapseFilterBar()
{
    if (tbFilterBar && tbFilterBar->get_active()) {
        tbFilterBar->set_active(false);  // triggers filterBarToggled → clears & hides
    }
}

void EditorPanel::filterBarChanged()
{
    if (filterBarBlockSignals) return;
    applyEditorFilter();
}

void EditorPanel::filterBarClearAll()
{
    filterBarBlockSignals = true;

    fbUnRanked->set_active(false);
    for (int i = 0; i < 5; i++) {
        fbRank[i]->set_active(false);
    }

    fbUnCLabeled->set_active(false);
    for (int i = 0; i < 5; i++) {
        fbCLabel[i]->set_active(false);
    }

    for (int i = 0; i < 2; i++) {
        fbEdited[i]->set_active(false);
        fbRecentlySaved[i]->set_active(false);
    }

    fbSearchEntry->set_text("");

    filterBarBlockSignals = false;
    applyEditorFilter();
}

BrowserFilter EditorPanel::buildEditorFilter()
{
    BrowserFilter f;

    // Rating: if any rank toggle is active, show only those
    bool anyRank = fbUnRanked->get_active();
    for (int i = 0; i < 5 && !anyRank; i++) {
        anyRank = fbRank[i]->get_active();
    }
    if (anyRank) {
        f.showRanked[0] = fbUnRanked->get_active();
        for (int i = 0; i < 5; i++) {
            f.showRanked[i + 1] = fbRank[i]->get_active();
        }
    }

    // Color: if any color toggle is active, show only those
    bool anyColor = fbUnCLabeled->get_active();
    for (int i = 0; i < 5 && !anyColor; i++) {
        anyColor = fbCLabel[i]->get_active();
    }
    if (anyColor) {
        f.showCLabeled[0] = fbUnCLabeled->get_active();
        for (int i = 0; i < 5; i++) {
            f.showCLabeled[i + 1] = fbCLabel[i]->get_active();
        }
    }

    // Edited: if any edited toggle is active, show only those
    bool anyEdited = fbEdited[0]->get_active() || fbEdited[1]->get_active();
    if (anyEdited) {
        for (int i = 0; i < 2; i++) {
            f.showEdited[i] = fbEdited[i]->get_active();
        }
    }

    // Recently saved: if any saved toggle is active, show only those
    bool anySaved = fbRecentlySaved[0]->get_active() || fbRecentlySaved[1]->get_active();
    if (anySaved) {
        for (int i = 0; i < 2; i++) {
            f.showRecentlySaved[i] = fbRecentlySaved[i]->get_active();
        }
    }

    // Search
    Glib::ustring searchText = fbSearchEntry->get_text();
    if (!searchText.empty()) {
        f.vFilterStrings.clear();
        std::string upper = searchText.uppercase();
        f.vFilterStrings.push_back(upper);
    }

    // Album whitelist
    f.albumWhitelist = currentAlbumWhitelist_;

    // Always show non-trash, hide trash
    f.showTrash = false;
    f.showNotTrash = true;

    return f;
}

void EditorPanel::applyEditorFilter()
{
    if (!fPanel || !fPanel->fileCatalog || !fPanel->fileCatalog->fileBrowser) return;

    BrowserFilter f = buildEditorFilter();
    fPanel->fileCatalog->fileBrowser->applyFilter(f);
}

void EditorPanel::onAlbumSelected (const std::set<std::string>& whitelist)
{
    currentAlbumWhitelist_ = whitelist;
    applyEditorFilter();
}

void EditorPanel::addCurrentImageToTargetAlbum ()
{
    if (!albumBrowser_ || fname.empty()) return;
    albumBrowser_->addFileToTargetAlbum(fname);
}

void EditorPanel::onAlbumViewRequested (const Glib::ustring& albumName, const std::vector<Glib::ustring>& files)
{
    if (albumName.empty()) {
        hideAlbumView();
    } else {
        showAlbumView(albumName, files);
    }
}

void EditorPanel::showAlbumView (const Glib::ustring& albumName, const std::vector<Glib::ustring>& files)
{
    if (!albumViewStack_) return;

    // Cancel any in-flight thumbnail loading for previous content
    ++albumViewSession_;

    currentAlbumViewName_ = albumName;
    currentAlbumFiles_ = files;
    albumNameLabel_->set_markup("<b>" + Glib::Markup::escape_text(albumName) + "</b>");
    albumCountLabel_->set_text(Glib::ustring::compose(M("ALBUM_VIEW_PHOTOS"), files.size()));

    // Rebuild the grid/collage with current settings
    rebuildAlbumGrid();

    // Add left/right margins to account for sidebar overlays
    const auto& opts = App::get().options();
    albumViewBox_->set_margin_start(hidehp && hidehp->get_active() ? opts.dirBrowserWidth : 0);
    albumViewBox_->set_margin_end(tbRightPanel_1 && tbRightPanel_1->get_active() ? std::min(opts.toolPanelWidth, 400) : 0);

    albumViewStack_->set_visible_child("album");
    albumViewBuilt_ = true;

    // Update toggle button state without triggering signal
    if (tbAlbumView_) {
        albumViewToggleConn_.block();
        tbAlbumView_->set_active(true);
        albumViewToggleConn_.unblock();
    }

    // Load thumbnails that aren't already cached
    std::vector<Glib::ustring> filesToLoad;
    for (const auto& fpath : files) {
        if (albumThumbCache_.find(fpath) == albumThumbCache_.end() &&
            Glib::file_test(fpath, Glib::FILE_TEST_EXISTS)) {
            filesToLoad.push_back(fpath);
        }
    }
    if (!filesToLoad.empty()) {
        loadAlbumThumbnails(albumViewSession_, filesToLoad);
    }

    // Apply current view mode settings
    applyAlbumViewMode();
}

void EditorPanel::rebuildAlbumGrid ()
{
    if (!albumViewGrid_) return;

    // Clear existing grid
    for (auto* child : albumViewGrid_->get_children()) {
        albumViewGrid_->remove(*child);
    }

    int thumbH = albumThumbHeight_;
    int thumbW = static_cast<int>(thumbH * 4.0 / 3.0);
    int itemW = thumbW + 20;

    auto labelCss = Gtk::CssProvider::create();
    labelCss->load_from_data("label { font-size: 0.8em; }");

    for (const auto& fpath : currentAlbumFiles_) {
        if (!Glib::file_test(fpath, Glib::FILE_TEST_EXISTS)) continue;

        Gtk::Box* itemBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 2));
        itemBox->set_size_request(itemW, -1);
        itemBox->set_name("AlbumViewItem");
        itemBox->set_valign(Gtk::ALIGN_START);

        Gtk::Image* thumbImg = Gtk::manage(new Gtk::Image());
        thumbImg->set_size_request(thumbW, thumbH);
        thumbImg->set_name("AlbumThumb_" + fpath);

        // Use cached pixbuf, scaled to display size
        auto cacheIt = albumThumbCache_.find(fpath);
        if (cacheIt != albumThumbCache_.end()) {
            auto& src = cacheIt->second;
            int srcW = src->get_width();
            int srcH = src->get_height();
            // Scale to fit within thumbW x thumbH preserving aspect ratio
            double scale = std::min(static_cast<double>(thumbW) / srcW,
                                    static_cast<double>(thumbH) / srcH);
            int dispW = std::max(1, static_cast<int>(srcW * scale));
            int dispH = std::max(1, static_cast<int>(srcH * scale));
            thumbImg->set(src->scale_simple(dispW, dispH, Gdk::INTERP_BILINEAR));
        }

        itemBox->pack_start(*thumbImg, Gtk::PACK_SHRINK);

        // Filename label (only if info is shown)
        if (albumShowInfo_) {
            Glib::ustring basename = Glib::path_get_basename(fpath);
            Gtk::Label* label = Gtk::manage(new Gtk::Label(basename));
            label->set_ellipsize(Pango::ELLIPSIZE_MIDDLE);
            label->set_max_width_chars(18);
            label->set_tooltip_text(fpath);
            label->get_style_context()->add_provider(labelCss, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 200);
            itemBox->pack_start(*label, Gtk::PACK_SHRINK);
        }

        albumViewGrid_->add(*itemBox);
    }

    albumViewGrid_->show_all();
}

void EditorPanel::albumZoomChanged ()
{
    if (!albumZoomSlider_) return;
    int newH = static_cast<int>(albumZoomSlider_->get_value());
    if (newH == albumThumbHeight_) return;
    albumThumbHeight_ = newH;

    if (albumViewMode_ == AlbumViewMode::COLLAGE) {
        recalculateCollageLayout();
        if (albumCollageArea_) albumCollageArea_->queue_draw();
    } else {
        rebuildAlbumGrid();
    }
}

void EditorPanel::albumInfoToggled ()
{
    if (!albumInfoToggle_) return;
    albumShowInfo_ = albumInfoToggle_->get_active();

    if (albumViewMode_ == AlbumViewMode::COLLAGE) {
        if (albumCollageArea_) albumCollageArea_->queue_draw();
    } else {
        rebuildAlbumGrid();
    }
}

void EditorPanel::albumViewModeChanged ()
{
    AlbumViewMode newMode;
    if (albumModeGrid_ && albumModeGrid_->get_active()) {
        newMode = AlbumViewMode::GRID;
    } else if (albumModeFit_ && albumModeFit_->get_active()) {
        newMode = AlbumViewMode::FIT;
    } else {
        newMode = AlbumViewMode::COLLAGE;
    }

    if (newMode == albumViewMode_) return;
    albumViewMode_ = newMode;
    applyAlbumViewMode();
}

void EditorPanel::applyAlbumViewMode ()
{
    if (!albumGridStack_ || !albumViewScrolled_ || !albumZoomSlider_) return;

    // Disconnect any fit-mode resize handler
    albumFitResizeConn_.disconnect();

    switch (albumViewMode_) {
    case AlbumViewMode::GRID:
        albumGridStack_->set_visible_child("grid");
        albumViewScrolled_->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
        albumZoomSlider_->set_sensitive(true);
        albumZoomConn_.block();
        albumZoomSlider_->set_value(albumThumbHeight_);
        albumZoomConn_.unblock();
        rebuildAlbumGrid();
        break;

    case AlbumViewMode::FIT:
        albumGridStack_->set_visible_child("grid");
        albumViewScrolled_->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_NEVER);
        albumZoomSlider_->set_sensitive(false);
        fitLastW_ = 0;
        fitLastH_ = 0;
        recalculateFitSize();
        // Recalculate on viewport resize (with size-changed guard)
        albumFitResizeConn_ = albumViewScrolled_->signal_size_allocate().connect(
            [this](Gtk::Allocation& alloc) {
                if (alloc.get_width() == fitLastW_ && alloc.get_height() == fitLastH_) return;
                fitLastW_ = alloc.get_width();
                fitLastH_ = alloc.get_height();
                Glib::signal_idle().connect_once([this]() {
                    if (albumViewMode_ == AlbumViewMode::FIT) {
                        recalculateFitSize();
                    }
                });
            });
        break;

    case AlbumViewMode::COLLAGE:
        albumGridStack_->set_visible_child("collage");
        albumViewScrolled_->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
        albumZoomSlider_->set_sensitive(true);
        albumZoomConn_.block();
        albumZoomSlider_->set_value(albumThumbHeight_);
        albumZoomConn_.unblock();
        recalculateCollageLayout();
        break;
    }
}

void EditorPanel::recalculateFitSize ()
{
    if (currentAlbumFiles_.empty() || !albumViewScrolled_) return;

    int viewportW = albumViewScrolled_->get_allocated_width() - 16; // padding
    int viewportH = albumViewScrolled_->get_allocated_height() - 16;
    if (viewportW <= 0 || viewportH <= 0) return;

    int n = static_cast<int>(currentAlbumFiles_.size());
    int gap = 4;
    int labelH = albumShowInfo_ ? 20 : 0;

    // Binary search for largest thumbH where all items fit
    int lo = 30, hi = 600, best = 60;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int thumbW = static_cast<int>(mid * 4.0 / 3.0);
        int itemW = thumbW + 20;
        int itemH = mid + labelH + 8;

        int cols = std::max(1, (viewportW + gap) / (itemW + gap));
        int rows = (n + cols - 1) / cols;
        int totalH = rows * (itemH + gap) - gap;

        if (totalH <= viewportH) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }

    albumThumbHeight_ = best;
    // Update slider display (without triggering signal)
    if (albumZoomSlider_) {
        albumZoomConn_.block();
        albumZoomSlider_->set_value(best);
        albumZoomConn_.unblock();
    }
    rebuildAlbumGrid();
}

double EditorPanel::getAspectRatio (const std::string& filepath)
{
    auto it = albumAspectCache_.find(filepath);
    if (it != albumAspectCache_.end()) return it->second;

    // Try to get from cached pixbuf
    auto thumbIt = albumThumbCache_.find(filepath);
    if (thumbIt != albumThumbCache_.end() && thumbIt->second) {
        double ar = static_cast<double>(thumbIt->second->get_width()) / thumbIt->second->get_height();
        albumAspectCache_[filepath] = ar;
        return ar;
    }

    return 1.5; // default 3:2
}

void EditorPanel::recalculateCollageLayout ()
{
    collageLayout_.clear();
    collageScaledCache_.clear();
    collageContentHeight_ = 0;
    if (currentAlbumFiles_.empty() || !albumViewScrolled_) return;

    int viewportW = albumViewScrolled_->get_allocated_width() - 16;
    if (viewportW <= 0) viewportW = 800;

    int targetH = albumThumbHeight_;
    int gap = 4;
    int yOff = 8; // top padding

    // Justified row algorithm
    std::vector<std::pair<std::string, double>> items; // filepath, aspect ratio
    for (const auto& f : currentAlbumFiles_) {
        if (!Glib::file_test(f, Glib::FILE_TEST_EXISTS)) continue;
        items.push_back({f, getAspectRatio(f)});
    }

    size_t i = 0;
    while (i < items.size()) {
        // Accumulate items for this row
        double rowWidthSum = 0;
        size_t rowStart = i;
        while (i < items.size()) {
            double itemW = targetH * items[i].second;
            if (rowWidthSum + itemW + (i - rowStart) * gap > viewportW && i > rowStart) {
                break;
            }
            rowWidthSum += itemW;
            ++i;
        }

        size_t rowCount = i - rowStart;
        bool isLastRow = (i >= items.size());

        // Calculate actual row height to fill width exactly
        double totalGaps = (rowCount - 1) * gap;
        double rowH;
        if (!isLastRow && rowCount > 0) {
            // Scale row to fill viewport width
            double arSum = 0;
            for (size_t j = rowStart; j < i; ++j) arSum += items[j].second;
            rowH = (viewportW - totalGaps) / arSum;
        } else {
            rowH = targetH; // last row: don't stretch
        }

        int xOff = 8; // left padding
        for (size_t j = rowStart; j < i; ++j) {
            int w = static_cast<int>(rowH * items[j].second);
            int h = static_cast<int>(rowH);
            collageLayout_.push_back({xOff, yOff, w, h, items[j].first});
            xOff += w + gap;
        }

        yOff += static_cast<int>(rowH) + gap;
    }

    collageContentHeight_ = yOff + 8;
    if (albumCollageArea_) {
        albumCollageArea_->set_size_request(-1, collageContentHeight_);
        albumCollageArea_->queue_draw();
    }
}

bool EditorPanel::onCollageAreaDraw (const Cairo::RefPtr<Cairo::Context>& cr)
{
    if (collageLayout_.empty()) return true;

    // Get visible region for scroll culling
    double clipX1, clipY1, clipX2, clipY2;
    cr->get_clip_extents(clipX1, clipY1, clipX2, clipY2);

    for (const auto& item : collageLayout_) {
        // Skip items outside visible region
        if (item.y + item.h < clipY1 || item.y > clipY2) continue;

        auto cacheIt = albumThumbCache_.find(item.filepath);
        if (cacheIt != albumThumbCache_.end() && cacheIt->second) {
            // Use scaled pixbuf cache to avoid rescaling every draw
            int tw = std::max(1, item.w);
            int th = std::max(1, item.h);
            std::string cacheKey = item.filepath + "|" + std::to_string(tw) + "x" + std::to_string(th);
            auto& scaled = collageScaledCache_[cacheKey];
            if (!scaled) {
                scaled = cacheIt->second->scale_simple(tw, th, Gdk::INTERP_BILINEAR);
            }
            Gdk::Cairo::set_source_pixbuf(cr, scaled, item.x, item.y);
            cr->rectangle(item.x, item.y, item.w, item.h);
            cr->fill();
        } else {
            // Placeholder rectangle
            cr->set_source_rgba(0.3, 0.3, 0.3, 0.5);
            cr->rectangle(item.x, item.y, item.w, item.h);
            cr->fill();
        }

        // Draw filename if info is shown
        if (albumShowInfo_ && albumCollageArea_) {
            Glib::ustring basename = Glib::path_get_basename(item.filepath);
            cr->set_source_rgba(0, 0, 0, 0.6);
            cr->rectangle(item.x, item.y + item.h - 18, item.w, 18);
            cr->fill();

            cr->set_source_rgb(1, 1, 1);
            cr->move_to(item.x + 4, item.y + item.h - 4);
            auto layout = albumCollageArea_->create_pango_layout(basename);
            auto fontDesc = Pango::FontDescription("sans 8");
            layout->set_font_description(fontDesc);
            layout->set_width((item.w - 8) * Pango::SCALE);
            layout->set_ellipsize(Pango::ELLIPSIZE_MIDDLE);
            layout->show_in_cairo_context(cr);
        }
    }

    return true;
}

bool EditorPanel::onCollageAreaClick (GdkEventButton* ev)
{
    if (!ev || ev->type != GDK_2BUTTON_PRESS || ev->button != 1) return false;

    int mx = static_cast<int>(ev->x);
    int my = static_cast<int>(ev->y);

    for (const auto& item : collageLayout_) {
        if (mx >= item.x && mx < item.x + item.w &&
            my >= item.y && my < item.y + item.h) {
            Glib::ustring filePath(item.filepath);
            hideAlbumView();
            std::thread([this, filePath]() {
                Thumbnail* thm = cacheMgr->getEntry(filePath);
                if (thm) {
                    Glib::signal_idle().connect_once([this, thm]() {
                        if (fPanel && fPanel->fileCatalog) {
                            fPanel->fileCatalog->openRequested({thm});
                        } else {
                            thm->decreaseRef();
                        }
                    });
                }
            }).detach();
            return true;
        }
    }
    return false;
}

void EditorPanel::loadAlbumThumbnails (int session, const std::vector<Glib::ustring>& files)
{
    // Split work across multiple threads for parallel loading
    unsigned int nThreads = std::max(2u, std::thread::hardware_concurrency());
    if (nThreads > files.size()) {
        nThreads = files.size();
    }

    size_t chunkSize = (files.size() + nThreads - 1) / nThreads;

    for (unsigned int t = 0; t < nThreads; ++t) {
        size_t start = t * chunkSize;
        size_t end = std::min(start + chunkSize, files.size());
        if (start >= files.size()) break;

        std::vector<Glib::ustring> chunk(files.begin() + start, files.begin() + end);

        std::thread([this, session, chunk]() {
            for (const auto& fpath : chunk) {
                if (session != albumViewSession_) return;
                if (!Glib::file_test(fpath, Glib::FILE_TEST_EXISTS)) continue;

                Thumbnail* thm = cacheMgr->getEntry(fpath);
                if (!thm) continue;

                double scale;
                rtengine::IImage8* img = thm->processThumbImage(thm->getProcParams(), 300, scale);
                if (!img) {
                    thm->decreaseRef();
                    continue;
                }

                int w = img->getWidth();
                int h = img->getHeight();
                auto pixbuf = Gdk::Pixbuf::create_from_data(
                    img->getData(), Gdk::COLORSPACE_RGB, false, 8, w, h, w * 3);
                auto pixbufCopy = pixbuf->copy();
                delete img;
                thm->decreaseRef();

                Glib::ustring capturedPath = fpath;
                Glib::signal_idle().connect_once([this, session, capturedPath, pixbufCopy]() {
                    if (session != albumViewSession_) return;

                    albumThumbCache_[capturedPath] = pixbufCopy;
                    // Cache aspect ratio
                    if (pixbufCopy->get_height() > 0) {
                        albumAspectCache_[capturedPath] =
                            static_cast<double>(pixbufCopy->get_width()) / pixbufCopy->get_height();
                    }

                    if (albumViewMode_ == AlbumViewMode::COLLAGE) {
                        // Immediately show newly loaded thumbnail (using current layout)
                        if (albumCollageArea_) albumCollageArea_->queue_draw();
                        // Debounce collage relayout: schedule one idle relayout
                        // instead of relaying out for every single thumbnail
                        if (!collageRelayoutPending_) {
                            collageRelayoutPending_ = true;
                            Glib::signal_timeout().connect_once([this]() {
                                collageRelayoutPending_ = false;
                                if (albumViewMode_ == AlbumViewMode::COLLAGE) {
                                    recalculateCollageLayout();
                                }
                            }, 100); // batch updates over 100ms
                        }
                    } else {
                        // Update matching image widget in grid
                        for (auto* child : albumViewGrid_->get_children()) {
                            auto* flowChild = dynamic_cast<Gtk::FlowBoxChild*>(child);
                            if (!flowChild) continue;
                            auto* itemBox = dynamic_cast<Gtk::Box*>(flowChild->get_child());
                            if (!itemBox) continue;

                            for (auto* w : itemBox->get_children()) {
                                auto* thumbImg = dynamic_cast<Gtk::Image*>(w);
                                if (thumbImg && thumbImg->get_name() == "AlbumThumb_" + capturedPath) {
                                    int srcW = pixbufCopy->get_width();
                                    int srcH = pixbufCopy->get_height();
                                    int thumbW = static_cast<int>(albumThumbHeight_ * 4.0 / 3.0);
                                    double sc = std::min(static_cast<double>(thumbW) / srcW,
                                                         static_cast<double>(albumThumbHeight_) / srcH);
                                    int dispW = std::max(1, static_cast<int>(srcW * sc));
                                    int dispH = std::max(1, static_cast<int>(srcH * sc));
                                    thumbImg->set(pixbufCopy->scale_simple(dispW, dispH, Gdk::INTERP_BILINEAR));
                                    return;
                                }
                            }
                        }
                    }
                });
            }
        }).detach();
    }
}

void EditorPanel::hideAlbumView ()
{
    if (!albumViewStack_) return;
    albumFitResizeConn_.disconnect();
    albumViewStack_->set_visible_child("editor");

    if (tbAlbumView_ && tbAlbumView_->get_active()) {
        albumViewToggleConn_.block();
        tbAlbumView_->set_active(false);
        albumViewToggleConn_.unblock();
    }

    // Ensure filmstrip is visible and redrawn after returning from album view
    if (catalogPane) {
        catalogPane->set_opacity(1.0);
    }
    if (fPanel && fPanel->fileCatalog) {
        fPanel->fileCatalog->redrawAll();
    }
}

void EditorPanel::closeAlbumView ()
{
    hideAlbumView();
    if (albumBrowser_) {
        albumBrowser_->deselectAlbum();
    }
}

void EditorPanel::toggleAlbumView ()
{
    if (!tbAlbumView_ || !albumViewStack_) return;

    if (tbAlbumView_->get_active()) {
        Glib::ustring dirName;
        std::vector<Glib::ustring> files;

        if (fPanel && fPanel->fileCatalog && fPanel->fileCatalog->fileBrowser) {
            const auto& entries = fPanel->fileCatalog->fileBrowser->getEntries();
            for (const auto* entry : entries) {
                files.push_back(entry->filename);
            }
            dirName = fPanel->fileCatalog->lastSelectedDir();
            if (dirName.empty()) dirName = M("ALBUM_HEADER");
            else dirName = Glib::path_get_basename(dirName);
        }

        if (dirName.empty()) {
            albumViewStack_->set_visible_child("album");
            albumNameLabel_->set_markup("<b>" + Glib::ustring(M("ALBUM_HEADER")) + "</b>");
            albumCountLabel_->set_text(M("ALBUM_VIEW_SELECT"));
        } else if (albumViewBuilt_ && currentAlbumViewName_ == dirName && currentAlbumFiles_ == files) {
            albumViewStack_->set_visible_child("album");
            applyAlbumViewMode();
        } else {
            showAlbumView(dirName, files);
        }
    } else {
        albumFitResizeConn_.disconnect();
        albumViewStack_->set_visible_child("editor");
    }
}

void EditorPanel::open (Thumbnail* tmb, rtengine::InitialImage* isrc)
{
    fprintf(stderr, "DBG EditorPanel::open tmb=%p isrc=%p\n", (void*)tmb, (void*)isrc);
    close();

    isProcessing = true; // prevents closing-on-init

    // initialize everything
    openThm = tmb;

    // Update filmstrip action bar stars to reflect opened image's rating
    filmstripCurrentRating = openThm->getRank();
    updateFilmstripStars(filmstripCurrentRating);

    fname = openThm->getFileName();
    if (fPanel && fPanel->fileCatalog) {
        fPanel->fileCatalog->saveResetState();
    }
    lastSaveAsFileName = removeExtension (Glib::path_get_basename (fname));

    previewHandler = new PreviewHandler ();

    this->isrc = isrc;
    ipc = rtengine::StagedImageProcessor::create (isrc);

    fprintf(stderr, "DBG open: ipc created=%p\n", (void*)ipc);
    ipc->setProgressListener (this);
    colorMgmtToolBar->updateProcessor();
    ipc->setPreviewImageListener (previewHandler);
    ipc->setPreviewScale (10);  // Important

    fprintf(stderr, "DBG open: before initImage\n");
    tpc->initImage (ipc, tmb->getType() == FT_Raw);
    fprintf(stderr, "DBG open: after initImage\n");
    tpc->setThumbnail(openThm);

    // Notify MCP server about the active editor panel
    if (parent && parent->getMcpServer()) {
        parent->getMcpServer()->setEditorPanel(this);
    }

    ipc->setHistogramListener (this);
    iareapanel->imageArea->indClippedPanel->silentlyDisableSharpMask();

//    iarea->fitZoom ();   // tell to the editorPanel that the next image has to be fitted to the screen
    iareapanel->imageArea->setPreviewHandler (previewHandler);
    iareapanel->imageArea->setImProcCoordinator (ipc);
    navigator->previewWindow->setPreviewHandler (previewHandler);
    navigator->previewWindow->setImageArea (iareapanel->imageArea);

    rtengine::ImageSource* is = isrc->getImageSource();
    is->setProgressListener ( this );

    fprintf(stderr, "DBG open: before initProfile\n");
    // try to load the last saved parameters from the cache or from the paramfile file
    ProcParams* ldprof = openThm->createProcParamsForUpdate (true, false); // will be freed by initProfile

    const auto& options = App::get().options();
    // initialize profile
    Glib::ustring defProf = openThm->getType() == FT_Raw ? options.defProfRaw : options.defProfImg;
    presetListPanel->setImageProcessor(ipc);
    presetListPanel->setThumbnail(openThm);
    presetListPanel->initProfile (defProf, ldprof);
    fprintf(stderr, "DBG open: after initProfile\n");

    presetListPanel->setInitialFileName (fname);

    openThm->addThumbnailListener (this);

    // Update EXIF info strip
    {
        const rtengine::FramesMetaData* idata = ipc->getInitialImage()->getMetaData();
        if (idata && idata->hasExif()) {
            Glib::ustring exifStr = Glib::ustring::compose(
                "<span size='small'>ISO %1    %2mm    f/%3    %4sec</span>",
                idata->getISOSpeed(),
                Glib::ustring::format(std::fixed, std::setprecision(0), idata->getFocalLen()),
                Glib::ustring(idata->apertureToString(idata->getFNumber())),
                Glib::ustring(idata->shutterToString(idata->getShutterSpeed()))
            );
            exifInfo->set_markup(exifStr);
        } else {
            exifInfo->set_markup("");
        }
    }

    info_toggled ();

    if (beforeIarea) {
        beforeAfterToggled();
        beforeAfterToggled();
    }

    fprintf(stderr, "DBG open: before cropHandler, mainCropWindow=%p\n", (void*)iareapanel->imageArea->mainCropWindow);
    // If in single tab mode, the main crop window is not constructed the very first time
    // since there was no resize event
    if (iareapanel->imageArea->mainCropWindow) {
        iareapanel->imageArea->mainCropWindow->cropHandler.newImage (ipc, false);
    } else {
        fprintf(stderr, "DBG open: mainCropWindow is null, calling on_resized\n");
        Gtk::Allocation alloc;
        iareapanel->imageArea->on_resized (alloc);

        // When passing a photo as an argument to the RawTherapee executable, the user wants
        // this auto-loaded photo's thumbnail to be selected and visible in the Filmstrip.
        EditorPanel::syncFileBrowser();
    }

    history->resetSnapShotNumber();
    navigator->setInvalid(ipc->getFullWidth(),ipc->getFullHeight());

    // Set fit zoom for the new image so the placeholder renders at the right scale.
    // Normally zoomFit is called by initialImageArrived(), but that only fires when
    // the engine delivers the first crop — too late for the placeholder.
    if (iareapanel->imageArea->mainCropWindow) {
        iareapanel->imageArea->mainCropWindow->zoomFit();
    }

    // Generate placeholder thumbnail asynchronously so open() doesn't block the UI.
    // Cancel any pending placeholder from a previous image first.
    if (placeholderCancel_) {
        placeholderCancel_->store(true);
    }
    {
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        placeholderCancel_ = cancel;
        Thumbnail* thm = openThm;
        int fullW = ipc->getFullWidth();
        PreviewHandler* ph = previewHandler;
        ImageArea* ia = iareapanel->imageArea;

        std::thread([thm, fullW, ph, ia, cancel]() {
            double thumbScale;
            rtengine::IImage8* thumbImg = thm->processThumbImage(
                thm->getProcParams(), 400, thumbScale);
            if (!thumbImg) return;

            if (cancel->load()) {
                delete thumbImg;
                return;
            }

            int tw = thumbImg->getWidth();
            int th = thumbImg->getHeight();
            auto pixbuf = Gdk::Pixbuf::create_from_data(
                thumbImg->getData(), Gdk::COLORSPACE_RGB, false, 8,
                tw, th, tw * 3);
            auto copied = pixbuf->copy();
            double scale = static_cast<double>(fullW) / tw;
            delete thumbImg;

            Glib::signal_idle().connect_once([ph, copied, scale, ia, cancel]() {
                if (!cancel->load()) {
                    ph->setPlaceholder(copied, scale);
                    ia->queue_draw();
                }
            });
        }).detach();
    }

    // Defer directory browser navigation to an idle callback so it doesn't
    // block the image from appearing.  dirBrowser->open() scans the filesystem
    // which is very slow on cross-filesystem mounts (e.g. WSL2 /mnt/c/).
    if (editorDirBrowser_) {
        Glib::ustring dirName = Glib::path_get_dirname(fname);
        DirBrowser* db = editorDirBrowser_;
        Glib::signal_idle().connect_once(
            [db, dirName]() { db->open(dirName); },
            Glib::PRIORITY_LOW);
    }
}

void EditorPanel::close ()
{
    // Clear MCP server reference before closing
    if (parent && parent->getMcpServer()) {
        auto* mcpSrv = parent->getMcpServer();
        if (mcpSrv->getEditorPanel() == this) {
            mcpSrv->setEditorPanel(nullptr);
        }
    }

    // Cancel any pending async placeholder to prevent stale callbacks
    if (placeholderCancel_) {
        placeholderCancel_->store(true);
        placeholderCancel_.reset();
    }

    if (ipc) {
        // Signal the old processing thread to abort ASAP so it stops
        // competing for CPU with the new image's processing.
        ipc->signalStop();
        if (beforeIpc) {
            beforeIpc->signalStop();
        }

        // Disconnect preset panel from processor before closing
        presetListPanel->setImageProcessor(nullptr);
        presetListPanel->setThumbnail(nullptr);

        // Capture profile data for async save before losing ipc/openThm
        ProcParams savedParams;
        ipc->getParams (&savedParams);

        // Disconnect TPC — non-blocking (no stopProcessing join)
        tpc->closeImage ();
        tpc->writeOptions ();

        // Disconnect listeners so processing thread callbacks become no-ops.
        // Pointer writes are atomic on x86 and the processing thread null-checks
        // before each callback, so this is safe without a blocking join.
        rtengine::ImageSource* is = isrc->getImageSource();
        is->setProgressListener ( nullptr );

        if (ipc) {
            ipc->setPreviewImageListener (nullptr);
        }

        if (beforeIpc) {
            beforeIpc->setPreviewImageListener (nullptr);
        }

        if (iareapanel) {
            iareapanel->imageArea->setPreviewHandler (nullptr);
            iareapanel->imageArea->setImProcCoordinator (nullptr);
            tpc->editModeSwitchedOff();
        }

        navigator->previewWindow->setPreviewHandler (nullptr);

        // Defer heavy work (stopProcessing join, handler deletion, ipc
        // destruction, profile save) to a background thread so close()
        // returns instantly and the GUI can switch to the new image.
        {
            rtengine::StagedImageProcessor* old = ipc;
            PreviewHandler* oldHandler = previewHandler;
            Thumbnail* savedThm = nullptr;
            Glib::ustring savedFname = fname;

            if (Glib::file_test(savedFname, Glib::FILE_TEST_EXISTS)) {
                savedThm = openThm;
                savedThm->increaseRef(); // prevent deletion during async save
            }

            ipc = nullptr;
            previewHandler = nullptr;

            std::thread([old, oldHandler, savedParams, savedThm, savedFname]() {
                // Join the processing thread (was blocking GUI before)
                old->stopProcessing();

                // Safe to delete now — processing thread is done
                delete oldHandler;
                rtengine::StagedImageProcessor::destroy(old);

                // Save profile to disk (was blocking GUI before)
                if (savedThm) {
                    savedThm->setProcParams(savedParams, nullptr, EDITOR);
                    savedThm->decreaseRef();
                }
            }).detach();
        }

        // If the file was deleted somewhere, the openThm.descreaseRef delete the object, but we don't know here
        if (Glib::file_test (fname, Glib::FILE_TEST_EXISTS)) {
            openThm->removeThumbnailListener (this);
            openThm->decreaseRef ();
        }
    }
}

void EditorPanel::saveProfile ()
{
    if (!ipc || !openThm) {
        return;
    }

    // If the file was deleted, do not generate ghost entries
    if (Glib::file_test (fname, Glib::FILE_TEST_EXISTS)) {
        ProcParams params;
        ipc->getParams (&params);

        // Will call updateCache, which will update both the cached and sidecar files if necessary
        openThm->setProcParams (params, nullptr, EDITOR);
    }
}

Glib::ustring EditorPanel::getShortName ()
{
    if (openThm) {
        return Glib::path_get_basename (openThm->getFileName ());
    } else {
        return "";
    }
}

Glib::ustring EditorPanel::getFileName () const
{
    if (openThm) {
        return openThm->getFileName ();
    } else {
        return "";
    }
}

// TODO!!!
void EditorPanel::procParamsChanged(
    const rtengine::procparams::ProcParams* params,
    const rtengine::ProcEvent& ev,
    const Glib::ustring& descr,
    const ParamsEdited* paramsEdited
)
{

//    if (ev!=EvPhotoLoaded)
//        saveLabel->set_markup (Glib::ustring("<span foreground=\"#AA0000\" weight=\"bold\">") + M("MAIN_BUTTON_SAVE") + "</span>");

    rtengine::eSensorType sensorType = isrc->getImageSource()->getSensorType();

    selectedFrame = 0;
    if (sensorType == rtengine::ST_BAYER) {
        selectedFrame = params->raw.bayersensor.imageNum;
    //} else if (sensorType == rtengine::ST_FUJI_XTRANS) {
    //    selectedFrame = params->raw.xtranssensor.imageNum;
    }
    selectedFrame = rtengine::LIM<int>(selectedFrame, 0, isrc->getImageSource()->getMetaData()->getFrameCount() - 1);

    info_toggled();
}

void EditorPanel::clearParamChanges()
{
}

void EditorPanel::setProgress(double p)
{
    MyProgressBar* const pl = progressLabel;

    idle_register.add(
        [p, pl]() -> bool
        {
            setprogressStrUI(p, {}, pl);
            return false;
        }
    );
}

void EditorPanel::setProgressStr(const Glib::ustring& str)
{
    MyProgressBar* const pl = progressLabel;

    idle_register.add(
        [str, pl]() -> bool
        {
            setprogressStrUI(-1.0, str, pl);
            return false;
        }
    );
}

void EditorPanel::setProgressState(bool inProcessing)
{
    epih->pending++;

    idle_register.add(
        [this, inProcessing]() -> bool
        {
            if (epih->destroyed)
            {
                if (epih->pending == 1) {
                    delete epih;
                } else {
                    --epih->pending;
                }

                return false;
            }

            epih->epanel->refreshProcessingState(inProcessing);
            --epih->pending;

            return false;
        }
    );
}

void EditorPanel::error(const Glib::ustring& descr)
{
    parent->error(descr);
}

void EditorPanel::error(const Glib::ustring& title, const Glib::ustring& descr)
{
    epih->pending++;

    idle_register.add(
        [this, descr, title]() -> bool
        {
            if (epih->destroyed) {
                if (epih->pending == 1) {
                    delete epih;
                } else {
                    --epih->pending;
                }

                return false;
            }

            epih->epanel->displayError(title, descr);
            --epih->pending;

            return false;
        }
    );
}

void EditorPanel::displayError(const Glib::ustring& title, const Glib::ustring& descr)
{
    GtkWidget* msgd = gtk_message_dialog_new_with_markup (nullptr,
                      GTK_DIALOG_DESTROY_WITH_PARENT,
                      GTK_MESSAGE_ERROR,
                      GTK_BUTTONS_OK,
                      "<b>%s</b>",
                      descr.data());
    gtk_window_set_title ((GtkWindow*)msgd, title.data());
    g_signal_connect_swapped (msgd, "response",
                              G_CALLBACK (gtk_widget_destroy),
                              msgd);
    gtk_widget_show_all (msgd);
}

// This is only called from the ThreadUI, so within the gtk thread
void EditorPanel::refreshProcessingState (bool inProcessingP)
{
    double val;
    Glib::ustring str;

    if (inProcessingP) {
        if (processingStartedTime == 0) {
            processingStartedTime = ::time (nullptr);
        }

        val = 1.0;
        str = "PROGRESSBAR_PROCESSING";
    } else {
        // Set proc params of thumbnail. It saves it into the cache and updates the file browser.
        if (ipc && openThm && tpc->getChangedState()) {
            rtengine::procparams::ProcParams pparams;
            ipc->getParams (&pparams);
            openThm->setProcParams (pparams, nullptr, EDITOR, false);
        }

        // Ring a sound if it was a long event
        if (processingStartedTime != 0) {
            time_t curTime = ::time (nullptr);

            const auto& options = App::get().options();
            if (::difftime (curTime, processingStartedTime) > options.sndLngEditProcDoneSecs) {
                SoundManager::playSoundAsync (options.sndLngEditProcDone);
            }

            processingStartedTime = 0;
        }

        // Set progress bar "done"
        val = 0.0;
        str = "PROGRESSBAR_READY";

#ifdef _WIN32

        // Maybe accessing "parent", which is a Gtk object, can justify to get the Gtk lock...
        if (!firstProcessingDone && static_cast<RTWindow*> (parent)->getIsFullscreen()) {
            parent->fullscreen();
        }

#endif
        firstProcessingDone = true;
    }

    isProcessing = inProcessingP;

    setprogressStrUI(val, str, progressLabel);
}

void EditorPanel::info_toggled ()
{

    Glib::ustring infoString;
    Glib::ustring expcomp;

    if (!ipc || !openThm) {
        return;
    }

    const rtengine::FramesMetaData* idata = ipc->getInitialImage()->getMetaData();

    if (idata && idata->hasExif()) {
        infoString = Glib::ustring::compose ("%1 + %2\n<span size=\"small\">f/</span><span size=\"large\">%3</span>  <span size=\"large\">%4</span><span size=\"small\">s</span>  <span size=\"small\">%5</span><span size=\"large\">%6</span>  <span size=\"large\">%7</span><span size=\"small\">mm</span>",
                                              escapeHtmlChars (idata->getMake() + " " + idata->getModel()),
                                              escapeHtmlChars (idata->getLens()),
                                              Glib::ustring (idata->apertureToString (idata->getFNumber())),
                                              Glib::ustring (idata->shutterToString (idata->getShutterSpeed())),
                                              M ("QINFO_ISO"), idata->getISOSpeed(),
                                              Glib::ustring::format (std::setw (3), std::fixed, std::setprecision (2), idata->getFocalLen()));

        expcomp = Glib::ustring (idata->expcompToString (idata->getExpComp(), true)); // maskZeroexpcomp

        if (!expcomp.empty ()) {
            infoString = Glib::ustring::compose ("%1  <span size=\"large\">%2</span><span size=\"small\">EV</span>",
                                                  infoString,
                                                  expcomp /*Glib::ustring(idata->expcompToString(idata->getExpComp()))*/);
        }

        infoString = Glib::ustring::compose ("%1\n<span size=\"small\">%2</span><span>%3</span>",
                                              infoString,
                                              escapeHtmlChars (Glib::path_get_dirname (openThm->getFileName())) + G_DIR_SEPARATOR_S,
                                              escapeHtmlChars (Glib::path_get_basename (openThm->getFileName()))  );

        int ww = -1, hh = -1;
        idata->getDimensions(ww, hh);
        if (ww <= 0) {
            ww = ipc->getFullWidth();
            hh = ipc->getFullHeight();
        }

        //megapixels
        infoString = Glib::ustring::compose ("%1\n<span size=\"small\">%2 MP (%3x%4)</span>",
                                             infoString,
                                             Glib::ustring::format (std::setw (4), std::fixed, std::setprecision (1), (float)ww * hh / 1000000),
                                             ww, hh);

        //adding special characteristics
        bool isHDR = idata->getHDR();
        bool isPixelShift = idata->getPixelShift();
        unsigned int numFrames = idata->getFrameCount();
        if (isHDR) {
            infoString = Glib::ustring::compose ("%1\n" + M("QINFO_HDR"), infoString, numFrames);
            if (numFrames == 1) {
                int sampleFormat = idata->getSampleFormat();
                infoString = Glib::ustring::compose ("%1 / %2", infoString, M(Glib::ustring::compose("SAMPLEFORMAT_%1", sampleFormat)));
            }
        } else if (isPixelShift) {
            infoString = Glib::ustring::compose ("%1\n" + M("QINFO_PIXELSHIFT"), infoString, numFrames);
        } else if (numFrames > 1) {
            infoString = Glib::ustring::compose ("%1\n" + M("QINFO_FRAMECOUNT"), infoString, numFrames);
        }
    } else {
        infoString = M ("QINFO_NOEXIF");
    }

    iareapanel->imageArea->setInfoText (std::move(infoString));
}

void EditorPanel::hideHistoryActivated ()
{
    if (hidehp->get_active()) {
        leftbox->set_no_show_all(false);
        leftbox->show_all();
    } else {
        leftbox->hide();
        leftbox->set_no_show_all(true);
    }

    auto& options = App::get().mut_options();
    options.showHistory = hidehp->get_active();

    if (options.showHistory) {
        hidehp->set_image (*iHistoryHide);
    } else {
        hidehp->set_image (*iHistoryShow);
    }

    // Update filmstrip + toolbar margins so sidebars don't overlap content
    int leftMargin = hidehp->get_active() ? options.dirBrowserWidth : 0;
    if (catalogPane) {
        catalogPane->set_margin_start(leftMargin);
    }
    if (albumViewBox_) {
        albumViewBox_->set_margin_start(leftMargin);
    }
    if (editorToolbarTop_) {
        editorToolbarTop_->set_margin_start(leftMargin);
    }

    tbShowHideSidePanels_managestate();
}


void EditorPanel::tbRightPanel_1_toggled ()
{
    /*
        removeIfThere (hpanedr, vboxright, false);
        if (tbRightPanel_1->get_active()){
            hpanedr->pack2(*vboxright, false, true);
            tbRightPanel_1->set_image (*iRightPanel_1_Hide);
        }
        else {
            tbRightPanel_1->set_image (*iRightPanel_1_Show);
        }
        tbShowHideSidePanels_managestate();
        */
    if (vboxright) {
        if (tbRightPanel_1->get_active()) {
            vboxright->show();
            tbRightPanel_1->set_image (*iRightPanel_1_Hide);
        } else {
            vboxright->hide();
            tbRightPanel_1->set_image (*iRightPanel_1_Show);
        }

        // Update filmstrip + toolbar margins so sidebars don't overlap content
        const auto& opts = App::get().options();
        int rightMargin = tbRightPanel_1->get_active() ? std::min(opts.toolPanelWidth, 400) : 0;
        if (catalogPane) {
            catalogPane->set_margin_end(rightMargin);
        }
        if (albumViewBox_) {
            albumViewBox_->set_margin_end(rightMargin);
        }
        if (editorToolbarTop_) {
            editorToolbarTop_->set_margin_end(rightMargin);
        }

        tbShowHideSidePanels_managestate();
    }
}

void EditorPanel::tbTopPanel_1_visible (bool visible)
{
    if (!tbTopPanel_1) {
        return;
    }

    if (visible) {
        tbTopPanel_1->show();
    } else {
        tbTopPanel_1->hide();
    }
}

void EditorPanel::getQueueOverlayInsets (int& left, int& top, int& right) const
{
    // Left sidebar inset
    left = (leftbox && leftbox->get_visible()) ? leftbox->get_allocated_width () : 0;

    // Right sidebar inset
    right = (vboxright && vboxright->get_visible()) ? vboxright->get_allocated_width () : 0;

    // Top filmstrip inset
    top = (catalogPane && catalogPane->get_visible()) ? catalogPane->get_allocated_height () : 0;
}

void EditorPanel::animateEditorIn(bool skipFilmstrip)
{
    // Cancel any running animation
    editorAnimConn_.disconnect();
    editorAnimIn_ = true;
    editorAnimFraction_ = 0.0;

    // Show filmstrip if enabled (start transparent, animation fades it in)
    if (!skipFilmstrip && catalogPane && App::get().options().editorFilmStripOpened) {
        catalogPane->set_opacity(0.0);
        catalogPane->show();
    } else if (skipFilmstrip && catalogPane && App::get().options().editorFilmStripOpened) {
        // Hero transition manages filmstrip visibility; just make sure it's shown
        catalogPane->show();
    }

    // Start footer bar transparent
    if (editorToolbarBottom_) {
        editorToolbarBottom_->set_opacity(0.0);
    }

    // Start sidebar off-screen (fraction 0 = fully hidden)
    hpanedr->queue_resize();

    editorAnimConn_ = Glib::signal_timeout().connect([this, skipFilmstrip]() -> bool {
        editorAnimFraction_ += 16.0 / 250.0;  // 250ms total — smooth entrance
        if (editorAnimFraction_ >= 1.0) {
            editorAnimFraction_ = 1.0;
            if (!skipFilmstrip && catalogPane && catalogPane->get_visible()) {
                catalogPane->set_opacity(1.0);
            }
            if (editorToolbarBottom_) {
                editorToolbarBottom_->set_opacity(1.0);
            }
            hpanedr->queue_resize();
            return false;
        }

        double eased = 1.0 - std::pow(1.0 - editorAnimFraction_, 3); // ease-out-cubic

        // Filmstrip: smooth opacity fade only (skip during hero transition)
        if (!skipFilmstrip && catalogPane && catalogPane->get_visible()) {
            catalogPane->set_opacity(eased);
        }

        // Footer bar: fade in
        if (editorToolbarBottom_) {
            editorToolbarBottom_->set_opacity(eased);
        }

        // Sidebar slides in from right via signal_get_child_position
        hpanedr->queue_resize();
        return true;
    }, 16);
}

void EditorPanel::animateEditorOut(std::function<void()> onComplete)
{
    // Cancel any running animation
    editorAnimConn_.disconnect();
    editorAnimIn_ = false;
    editorAnimFraction_ = 1.0;

    editorAnimConn_ = Glib::signal_timeout().connect([this, onComplete]() -> bool {
        editorAnimFraction_ -= 16.0 / 180.0;  // 180ms total
        if (editorAnimFraction_ <= 0.0) {
            editorAnimFraction_ = 0.0;
            // Reset filmstrip state
            if (catalogPane) {
                catalogPane->set_opacity(1.0);
            }
            if (editorToolbarBottom_) {
                editorToolbarBottom_->set_opacity(1.0);
            }
            hpanedr->queue_resize();
            // Execute completion callback (switches notebook page)
            if (onComplete) {
                onComplete();
            }
            return false;
        }

        // Ease-in-cubic for exit: accelerates out smoothly
        double t = editorAnimFraction_;
        double eased = t * t * t;

        // Filmstrip: smooth opacity fade only
        if (catalogPane && catalogPane->get_visible()) {
            catalogPane->set_opacity(eased);
        }

        // Footer bar: fade out
        if (editorToolbarBottom_) {
            editorToolbarBottom_->set_opacity(eased);
        }

        // Sidebar slides out to right via signal_get_child_position
        hpanedr->queue_resize();
        return true;
    }, 16);
}

void EditorPanel::tbTopPanel_1_toggled ()
{

    if (catalogPane) { // catalogPane does not exist in multitab mode

        auto& options = App::get().mut_options();
        if (tbTopPanel_1->get_active()) {
            catalogPane->show();
            tbTopPanel_1->set_image (*iTopPanel_1_Hide);
            options.editorFilmStripOpened = true;
        } else {
            catalogPane->hide();
            tbTopPanel_1->set_image (*iTopPanel_1_Show);
            options.editorFilmStripOpened = false;
        }

        tbShowHideSidePanels_managestate();
    }
}

/*
 * WARNING: Take care of the simpleEditor value when adding or modifying shortcut keys,
 *          since handleShortcutKey is now also triggered in simple editor mode
 */
bool EditorPanel::handleShortcutKey (GdkEventKey* event)
{

    bool ctrl = event->state & GDK_CONTROL_MASK;
    bool shift = event->state & GDK_SHIFT_MASK;
    bool alt = event->state & GDK_MOD1_MASK;
#ifdef __WIN32__
    bool altgr = event->state & GDK_MOD2_MASK;
#else
    bool altgr = event->state & GDK_MOD5_MASK;
#endif

    // Editor Layout
    switch (event->keyval) {
        case GDK_KEY_L:
            if (tbTopPanel_1) {
                tbTopPanel_1->set_active (!tbTopPanel_1->get_active());    // toggle top panel
            }

            if (ctrl) {
                hidehp->set_active (!hidehp->get_active());    // toggle History (left panel)
            }

            if (alt) {
                tbRightPanel_1->set_active (!tbRightPanel_1->get_active());    // toggle right panel
            }

            return true;
            break;

        case GDK_KEY_l:
            if (!shift && !alt /*&& !ctrl*/) {
                hidehp->set_active (!hidehp->get_active()); // toggle History (left panel)
                return true;
            }

            if (alt && !ctrl) { // toggle right panel
                tbRightPanel_1->set_active (!tbRightPanel_1->get_active());
                return true;
            }

            if (alt && ctrl) { // toggle left and right panels
                hidehp->set_active (!hidehp->get_active());
                tbRightPanel_1->set_active (!tbRightPanel_1->get_active());
                return true;
            }

            break;

        case GDK_KEY_m: // Maximize preview panel: hide top AND right AND history panels
            if (!ctrl && !alt) {
                toggleSidePanels();
                return true;
            }

            break;

        case GDK_KEY_M: // Maximize preview panel: hide top AND right AND history panels AND (fit image preview)
            if (!ctrl && !alt) {
                toggleSidePanelsZoomFit();
                return true;
            }

            break;
    }

#ifdef __WIN32__

    if (!alt && !ctrl && !altgr && event->hardware_keycode == 0x39 ) {
        iareapanel->imageArea->previewModePanel->togglebackColor();
        return true;
    }

#else

    if (!alt && !ctrl && !altgr && event->hardware_keycode == 0x12 ) {
        iareapanel->imageArea->previewModePanel->togglebackColor();
        return true;
    }

#endif

    if (!alt) {
        if (!ctrl) {
            // Normal
            switch (event->keyval) {
                case GDK_KEY_bracketright:
                    tpc->coarse->rotateRight();
                    return true;

                case GDK_KEY_bracketleft:
                    tpc->coarse->rotateLeft();
                    return true;

                case GDK_KEY_i:
                case GDK_KEY_I:
                    iareapanel->imageArea->infoEnabled (!App::get().options().showInfo);
                    return true;

                case GDK_KEY_B:
                case GDK_KEY_backslash:
                    beforeAfter->set_active (!beforeAfter->get_active());
                    return true;

                case GDK_KEY_plus:
                case GDK_KEY_equal:
                case GDK_KEY_KP_Add:
                    iareapanel->imageArea->zoomPanel->zoomInClicked();
                    return true;

                case GDK_KEY_minus:
                case GDK_KEY_underscore:
                case GDK_KEY_KP_Subtract:
                    iareapanel->imageArea->zoomPanel->zoomOutClicked();
                    return true;

                case GDK_KEY_z://GDK_1
                    iareapanel->imageArea->zoomPanel->zoom11Clicked();
                    return true;

                /*
                #ifndef __WIN32__
                                case GDK_KEY_9: // toggle background color of the preview
                                    iareapanel->imageArea->previewModePanel->togglebackColor();
                                    return true;
                #endif
                */
                case GDK_KEY_r: //preview mode Red
                    iareapanel->imageArea->previewModePanel->toggleR();
                    return true;

                case GDK_KEY_g: //preview mode Green
                    iareapanel->imageArea->previewModePanel->toggleG();
                    return true;

                case GDK_KEY_b: //preview mode Blue
                    iareapanel->imageArea->previewModePanel->toggleB();
                    return true;

                case GDK_KEY_p: //preview mode Sharpening Contrast mask
                    iareapanel->imageArea->indClippedPanel->toggleSharpMask();
                    return true;

                case GDK_KEY_v: //preview mode Luminosity
                    iareapanel->imageArea->previewModePanel->toggleL();
                    return true;

                case GDK_KEY_F: //preview mode Focus Mask
                    iareapanel->imageArea->indClippedPanel->toggleFocusMask();
                    return true;

                case GDK_KEY_less:
                    iareapanel->imageArea->indClippedPanel->toggleClipped (false);
                    return true;

                case GDK_KEY_greater:
                    iareapanel->imageArea->indClippedPanel->toggleClipped (true);
                    return true;

                case GDK_KEY_f:
                    iareapanel->imageArea->zoomPanel->zoomFitCropClicked();
                    return true;

                case GDK_KEY_F5:
                    openThm->openDefaultViewer ((event->state & GDK_SHIFT_MASK) ? 2 : 1);
                    return true;

                case GDK_KEY_y: // synchronize filebrowser with image in Editor
                    if (!App::get().isSimpleEditor() && fPanel && !fname.empty()) {
                        fPanel->fileCatalog->selectImage (fname, false);
                        return true;
                    }

                    break; // to avoid gcc complain

                case GDK_KEY_x: // clear filters and synchronize filebrowser with image in Editor
                    if (!App::get().isSimpleEditor() && fPanel && !fname.empty()) {
                        fPanel->fileCatalog->selectImage (fname, true);
                        return true;
                    }

                    break; // to avoid gcc complain
            }
        } else {
            // With control
            switch (event->keyval) {
                case GDK_KEY_S:
                    saveProfile();
                    setProgressStr (M ("PROGRESSBAR_PROCESSING_PROFILESAVED"));
                    return true;

                case GDK_KEY_s:
                    if (!App::get().isGimpPlugin()) {
                        saveAsPressed();
                    }

                    return true;

                case GDK_KEY_b:
                    if (!App::get().isGimpPlugin() && !App::get().isSimpleEditor()) {
                        queueImgPressed();
                    }

                    return true;

                case GDK_KEY_e:
                    if (!App::get().isGimpPlugin()) {
                        sendToExternalPressed();
                    }

                    return true;

                case GDK_KEY_z:
                    history->undo ();
                    return true;

                case GDK_KEY_Z:
                    history->redo ();
                    return true;

                case GDK_KEY_F5:
                    openThm->openDefaultViewer (3);
                    return true;

                case GDK_KEY_f:
                case GDK_KEY_F:
                    // No action is performed to avoid Gtk-CRITICAL due to Locallab treeview when treeview isn't focused
                    return true;
            }
        } //if (!ctrl)
    } //if (!alt)

    if (alt) {
        switch (event->keyval) {
            case GDK_KEY_s:
                history->addBookmarkPressed ();
                setProgressStr (M ("PROGRESSBAR_SNAPSHOT_ADDED"));
                return true;

            case GDK_KEY_f:
                iareapanel->imageArea->zoomPanel->zoomFitClicked();
                return true;
        }
    }

    if (shift) {
        switch (event->keyval) {
            case GDK_KEY_F3: // open Previous image from Editor's perspective
                if (!App::get().isSimpleEditor() && fPanel && !fname.empty()) {
                    EditorPanel::openPreviousEditorImage();
                    return true;
                }

                break; // to avoid gcc complain

            case GDK_KEY_F4: // open next image from Editor's perspective
                if (!App::get().isSimpleEditor() && fPanel && !fname.empty()) {
                    EditorPanel::openNextEditorImage();
                    return true;
                }

                break; // to avoid gcc complain
        }
    }

    if (tpc->getToolBar() && tpc->getToolBar()->handleShortcutKey (event)) {
        return true;
    }

    if (tpc->handleShortcutKey (event)) {
        return true;
    }

    if (!App::get().isSimpleEditor() && fPanel) {
        if (fPanel->handleShortcutKey (event)) {
            return true;
        }
    }

    return false;
}

void EditorPanel::procParamsChanged (Thumbnail* thm, int whoChangedIt, bool upgradeHint)
{

    if (whoChangedIt != EDITOR) {
        PartialProfile pp (true);
        pp.set (true);
        * (pp.pparams) = openThm->getProcParams();
        pp.pedited->locallab.spots.resize(pp.pparams->locallab.spots.size(), LocallabParamsEdited::LocallabSpotEdited(true));
        tpc->profileChange (&pp, rtengine::EvProfileChangeNotification, M ("PROGRESSDLG_PROFILECHANGEDINBROWSER"));
        pp.deleteInstance();
    }
}

bool EditorPanel::idle_saveImage (ProgressConnector<rtengine::IImagefloat*> *pc, Glib::ustring fname, SaveFormat sf, rtengine::procparams::ProcParams &pparams)
{
    rtengine::IImagefloat* img = pc->returnValue();
    delete pc;

    if ( img ) {
        setProgressStr (M ("GENERAL_SAVE"));
        setProgress (0.9f);

        ProgressConnector<int> *ld = new ProgressConnector<int>();
        img->setSaveProgressListener (parent->getProgressListener());

        if (sf.format == "tif")
            ld->startFunc (sigc::bind (sigc::mem_fun (img, &rtengine::IImagefloat::saveAsTIFF), fname, sf.tiffBits, sf.tiffFloat, sf.tiffUncompressed, sf.bigTiff),
                           sigc::bind (sigc::mem_fun (*this, &EditorPanel::idle_imageSaved), ld, img, fname, sf, pparams));
        else if (sf.format == "png")
            ld->startFunc (sigc::bind (sigc::mem_fun (img, &rtengine::IImagefloat::saveAsPNG), fname, sf.pngBits),
                           sigc::bind (sigc::mem_fun (*this, &EditorPanel::idle_imageSaved), ld, img, fname, sf, pparams));
        else if (sf.format == "jpg")
            ld->startFunc (sigc::bind (sigc::mem_fun (img, &rtengine::IImagefloat::saveAsJPEG), fname, sf.jpegQuality, sf.jpegSubSamp),
                           sigc::bind (sigc::mem_fun (*this, &EditorPanel::idle_imageSaved), ld, img, fname, sf, pparams));
        else {
            delete ld;
        }
    } else {
        Glib::ustring msg_ = Glib::ustring ("<b>") + escapeHtmlChars(fname) + ": Error during image processing\n</b>";
        Gtk::MessageDialog msgd (*parent, msg_, true, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
        msgd.run ();

        saveimgas->set_sensitive (true);
        send_to_external->set_sensitive(send_to_external->getEntryCount());
        isProcessing = false;

    }

    rtengine::ImageSource* imgsrc = isrc->getImageSource ();
    imgsrc->setProgressListener (this);
    return false;
}

bool EditorPanel::idle_imageSaved (ProgressConnector<int> *pc, rtengine::IImagefloat* img, Glib::ustring fname, SaveFormat sf, rtengine::procparams::ProcParams &pparams)
{
    delete img;

    if (! pc->returnValue() ) {
        openThm->imageDeveloped ();

        // save processing parameters, if needed
        if (sf.saveParams) {
            // We keep the extension to avoid overwriting the profile when we have
            // the same output filename with different extension
            pparams.save (fname + ".out" + App::PARAM_FILE_EXTENSION);
        }
    } else {
        error (M ("MAIN_MSG_CANNOTSAVE"), fname);
    }

    saveimgas->set_sensitive (true);
    send_to_external->set_sensitive(send_to_external->getEntryCount());

    parent->setProgressStr ("");
    parent->setProgress (0.);

    setProgressState (false);

    delete pc;
    SoundManager::playSoundAsync (App::get().options().sndBatchQueueDone);
    isProcessing = false;
    return false;
}

BatchQueueEntry* EditorPanel::createBatchQueueEntry ()
{

    rtengine::procparams::ProcParams pparams;
    ipc->getParams (&pparams);
    //rtengine::ProcessingJob* job = rtengine::ProcessingJob::create (ipc->getInitialImage(), pparams);
    rtengine::ProcessingJob* job = rtengine::ProcessingJob::create (openThm->getFileName (), openThm->getType() == FT_Raw, pparams);
    int fullW = 0, fullH = 0;
    isrc->getImageSource()->getFullSize (fullW, fullH, pparams.coarse.rotate == 90 || pparams.coarse.rotate == 270 ? TR_R90 : TR_NONE);
    int prevh = BatchQueue::calcMaxThumbnailHeight();
    int prevw = int ((size_t)fullW * (size_t)prevh / (size_t)fullH);
    return new BatchQueueEntry (job, pparams, openThm->getFileName(), prevw, prevh, openThm, App::get().options().overwriteOutputFile);
}



void EditorPanel::saveAsPressed ()
{
    if (!ipc || !openThm) {
        return;
    }

    bool fnameOK = false;
    Glib::ustring fnameOut;

    SaveAsDialog* saveAsDialog;
    auto toplevel = static_cast<Gtk::Window*> (get_toplevel ());

    auto& options = App::get().mut_options();
    if (Glib::file_test (options.lastSaveAsPath, Glib::FILE_TEST_IS_DIR)) {
        saveAsDialog = new SaveAsDialog (options.lastSaveAsPath, toplevel);
    } else {
        saveAsDialog = new SaveAsDialog (PlacesBrowser::userPicturesDir (), toplevel);
    }

    saveAsDialog->set_default_size (options.saveAsDialogWidth, options.saveAsDialogHeight);
    saveAsDialog->setInitialFileName (lastSaveAsFileName);
    saveAsDialog->setImagePath (fname);

    do {
        int result = saveAsDialog->run ();

        // The SaveAsDialog ensure that a filename has been specified
        fnameOut = saveAsDialog->getFileName ();

        options.lastSaveAsPath = saveAsDialog->getDirectory ();
        saveAsDialog->get_size (options.saveAsDialogWidth, options.saveAsDialogHeight);
        options.autoSuffix = saveAsDialog->getAutoSuffix ();
        options.saveMethodNum = saveAsDialog->getSaveMethodNum ();
        lastSaveAsFileName = Glib::path_get_basename (removeExtension (fnameOut));
        SaveFormat sf = saveAsDialog->getFormat ();
        options.saveFormat = sf;
        options.forceFormatOpts = saveAsDialog->getForceFormatOpts ();

        if (result != Gtk::RESPONSE_OK) {
            break;
        }

        if (saveAsDialog->getImmediately ()) {
            // separate filename and the path to the destination directory
            Glib::ustring dstdir = Glib::path_get_dirname (fnameOut);
            Glib::ustring dstfname = Glib::path_get_basename (removeExtension (fnameOut));
            Glib::ustring dstext = getExtension (fnameOut);

            if (saveAsDialog->getAutoSuffix()) {

                Glib::ustring fnameTemp;

                for (int tries = 0; tries < 100; tries++) {
                    if (tries == 0) {
                        fnameTemp = Glib::ustring::compose ("%1.%2", Glib::build_filename (dstdir,  dstfname), dstext);
                    } else {
                        fnameTemp = Glib::ustring::compose ("%1-%2.%3", Glib::build_filename (dstdir,  dstfname), tries, dstext);
                    }

                    if (!Glib::file_test (fnameTemp, Glib::FILE_TEST_EXISTS)) {
                        fnameOut = fnameTemp;
                        fnameOK = true;
                        break;
                    }
                }
            }

            // check if it exists
            if (!fnameOK) {
                fnameOK = confirmOverwrite (*saveAsDialog, fnameOut);
            }

            if (fnameOK) {
                isProcessing = true;
                // save image
                rtengine::procparams::ProcParams pparams;
                ipc->getParams (&pparams);
                rtengine::ProcessingJob* job = rtengine::ProcessingJob::create (ipc->getInitialImage(), pparams);

                ProgressConnector<rtengine::IImagefloat*> *ld = new ProgressConnector<rtengine::IImagefloat*>();
                ld->startFunc (sigc::bind (sigc::ptr_fun (&rtengine::processImage), job, err, parent->getProgressListener(), false ),
                               sigc::bind (sigc::mem_fun ( *this, &EditorPanel::idle_saveImage ), ld, fnameOut, sf, pparams));
                saveimgas->set_sensitive (false);
                send_to_external->set_sensitive(false);
            }
        } else {
            BatchQueueEntry* bqe = createBatchQueueEntry ();
            bqe->outFileName = fnameOut;
            bqe->saveFormat = saveAsDialog->getFormat ();
            bqe->overwriteFile = !saveAsDialog->getAutoSuffix();
            bqe->forceFormatOpts = saveAsDialog->getForceFormatOpts ();
            parent->addBatchQueueJob (bqe, saveAsDialog->getToHeadOfQueue ());
            fnameOK = true;
        }

        // ask parent to redraw file browser
        // ... or does it automatically when the tab is switched to it
    } while (!fnameOK);

    saveAsDialog->hide();

    delete saveAsDialog;
}

void EditorPanel::queueImgPressed ()
{
    if (!ipc || !openThm) {
        return;
    }

    saveProfile ();
    parent->addBatchQueueJob (createBatchQueueEntry ());
}

void EditorPanel::sendToExternal()
{
    if (!ipc || !openThm) {
        return;
    }

    // develop image
    rtengine::procparams::ProcParams pparams;
    ipc->getParams (&pparams);
    if (App::get().options().editor_bypass_output_profile) {
        pparams.icm.outputProfile = rtengine::procparams::ColorManagementParams::NoProfileString;
    }

    if (!cached_exported_filename.empty() && cached_exported_image == ipc->getInitialImage() && pparams == cached_exported_pparams && Glib::file_test(cached_exported_filename, Glib::FILE_TEST_IS_REGULAR)) {
        idle_sentToGimp(nullptr, nullptr, cached_exported_filename);
        return;
    }

    cached_exported_image = ipc->getInitialImage();
    cached_exported_pparams = pparams;
    cached_exported_filename.clear();
    rtengine::ProcessingJob* job = rtengine::ProcessingJob::create (ipc->getInitialImage(), pparams);
    ProgressConnector<rtengine::IImagefloat*> *ld = new ProgressConnector<rtengine::IImagefloat*>();
    ld->startFunc (sigc::bind (sigc::ptr_fun (&rtengine::processImage), job, err, parent->getProgressListener(), false ),
                   sigc::bind (sigc::mem_fun ( *this, &EditorPanel::idle_sendToGimp ), ld, openThm->getFileName() ));
    saveimgas->set_sensitive (false);
    send_to_external->set_sensitive(false);
}

void EditorPanel::sendToExternalChanged(int)
{
    int index = send_to_external->getSelected();
    auto& options = App::get().mut_options();
    if (index >= 0 && static_cast<unsigned>(index) == options.externalEditors.size()) {
        index = -1;
    }
    options.externalEditorIndex = index;
    if (externalEditorChangedSignal) {
        externalEditorChangedSignal->emit();
    }
}

void EditorPanel::sendToExternalPressed()
{
    const auto& options = App::get().options();
    if (options.externalEditorIndex == -1) {
        // "Other" external editor. Show app chooser dialog to let user pick.
        RTAppChooserDialog *dialog = getAppChooserDialog();
        dialog->show();
    } else {
        struct ExternalEditor editor = options.externalEditors.at(options.externalEditorIndex);
        external_editor_info = {
            editor.name,
            editor.command,
            editor.native_command};
        sendToExternal();
    }
}


bool EditorPanel::saveImmediately (const Glib::ustring &filename, const SaveFormat &sf)
{
    rtengine::procparams::ProcParams pparams;
    ipc->getParams (&pparams);

    rtengine::ProcessingJob *job = rtengine::ProcessingJob::create (ipc->getInitialImage(), pparams);

    // save immediately
    rtengine::IImagefloat *img = rtengine::processImage (job, err, nullptr, false);

    int err = 0;

    if (App::get().isGimpPlugin()) {
        err = img->saveAsTIFF (filename, 32, true, true);
    } else if (sf.format == "tif") {
        err = img->saveAsTIFF (filename, sf.tiffBits, sf.tiffFloat, sf.tiffUncompressed, sf.bigTiff);
    } else if (sf.format == "png") {
        err = img->saveAsPNG (filename, sf.pngBits);
    } else if (sf.format == "jpg") {
        err = img->saveAsJPEG (filename, sf.jpegQuality, sf.jpegSubSamp);
    } else {
        err = 1;
    }

    delete img;
    return !err;
}


void EditorPanel::openPreviousEditorImage()
{
    if (!App::get().isSimpleEditor() && fPanel && !fname.empty()) {
        fPanel->fileCatalog->openNextPreviousEditorImage (fname, NAV_PREVIOUS);
    }
}

void EditorPanel::openNextEditorImage()
{
    if (!App::get().isSimpleEditor() && fPanel && !fname.empty()) {
        fPanel->fileCatalog->openNextPreviousEditorImage (fname, NAV_NEXT);
    }
}

void EditorPanel::syncFileBrowser()   // synchronize filebrowser with image in Editor
{
    if (!App::get().isSimpleEditor() && fPanel && !fname.empty()) {
        fPanel->fileCatalog->selectImage (fname, false);
    }
}

ExternalEditorChangedSignal * EditorPanel::getExternalEditorChangedSignal()
{
    return externalEditorChangedSignal;
}

void EditorPanel::setExternalEditorChangedSignal(ExternalEditorChangedSignal *signal)
{
    if (externalEditorChangedSignal) {
        externalEditorChangedSignalConnection.disconnect();
    }
    externalEditorChangedSignal = signal;
    if (signal) {
        externalEditorChangedSignalConnection = signal->connect(
                sigc::mem_fun(*this, &EditorPanel::updateExternalEditorSelection));
    }
}

void EditorPanel::histogramProfile_toggled()
{
    auto& options = App::get().mut_options();
    options.rtSettings.HistogramWorking = toggleHistogramProfile->get_active();
    colorMgmtToolBar->updateHistogram();
}

bool EditorPanel::idle_sendToGimp ( ProgressConnector<rtengine::IImagefloat*> *pc, Glib::ustring fname)
{

    rtengine::IImagefloat* img = pc->returnValue();
    delete pc;

    const auto& options = App::get().options();
    if (img) {
        // get file name base
        Glib::ustring shortname = removeExtension(Glib::path_get_basename(fname));
        Glib::ustring dirname;
        switch (options.editor_out_dir) {
        case Options::EDITOR_OUT_DIR_CURRENT:
            dirname = Glib::path_get_dirname(fname);
            break;
        case Options::EDITOR_OUT_DIR_CUSTOM:
            dirname = options.editor_custom_out_dir;
            break;
        default: // Options::EDITOR_OUT_DIR_TEMP
            dirname = getTmpDirectory();
            break;
        }
        Glib::ustring fullFileName = Glib::build_filename(dirname, shortname);

        SaveFormat sf;
        sf.format = "tif";
        if (options.editor_float32) {
            sf.tiffBits = 32;
            sf.tiffFloat = true;
        } else {
            sf.tiffBits = 16;
            sf.tiffFloat = false;
        }

        sf.tiffUncompressed = true;
        sf.saveParams = true;

        Glib::ustring fileName = Glib::ustring::compose ("%1.%2", fullFileName, sf.format);

        // TODO: Just list all file with a suitable name instead of brute force...
        int tries = 1;
        while (Glib::file_test (fileName, Glib::FILE_TEST_EXISTS) && tries < 1000) {
            fileName = Glib::ustring::compose ("%1-%2.%3", fullFileName, tries, sf.format);
            tries++;
        }

        if (tries == 1000) {
            delete img;
            return false;
        }

        ProgressConnector<int> *ld = new ProgressConnector<int>();
        img->setSaveProgressListener (parent->getProgressListener());
        ld->startFunc (sigc::bind (sigc::mem_fun (img, &rtengine::IImagefloat::saveAsTIFF), fileName, sf.tiffBits, sf.tiffFloat, sf.tiffUncompressed, sf.bigTiff),
                       sigc::bind (sigc::mem_fun (*this, &EditorPanel::idle_sentToGimp), ld, img, fileName));
    } else {
        Glib::ustring msg_ = Glib::ustring ("<b> Error during image processing\n</b>");
        Gtk::MessageDialog msgd (*parent, msg_, true, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
        msgd.run ();
        saveimgas->set_sensitive (true);
        send_to_external->set_sensitive(send_to_external->getEntryCount());
    }

    return false;
}

bool EditorPanel::idle_sentToGimp (ProgressConnector<int> *pc, rtengine::IImagefloat* img, Glib::ustring filename)
{
    if (img) {
        delete img;
        cached_exported_filename = filename;
    }
    int errore = 0;
    setProgressState(false);
    if (pc) {
        errore = pc->returnValue();
        delete pc;
    }

    if ((!img && Glib::file_test(filename, Glib::FILE_TEST_IS_REGULAR)) || (img && !errore)) {
        saveimgas->set_sensitive (true);
        send_to_external->set_sensitive(send_to_external->getEntryCount());
        parent->setProgressStr ("");
        parent->setProgress (0.);
        bool success = false;

        setUserOnlyPermission(Gio::File::create_for_path(filename), false);

        success = ExtProgStore::openInExternalEditor(filename, external_editor_info);

        if (!success) {
            Gtk::MessageDialog msgd (*parent, M ("MAIN_MSG_CANNOTSTARTEDITOR"), false, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
            msgd.set_secondary_text (M ("MAIN_MSG_CANNOTSTARTEDITOR_SECONDARY"));
            msgd.set_title (M ("MAIN_BUTTON_SENDTOEDITOR"));
            msgd.run ();
        }
    }

    return false;
}

RTAppChooserDialog *EditorPanel::getAppChooserDialog()
{
    if (!app_chooser_dialog.get()) {
        app_chooser_dialog.reset(new RTAppChooserDialog("image/tiff"));
        app_chooser_dialog->signal_response().connect(
            sigc::mem_fun(*this, &EditorPanel::onAppChooserDialogResponse)
        );
        app_chooser_dialog->set_modal();
    }

    return app_chooser_dialog.get();
}

void EditorPanel::onAppChooserDialogResponse(int responseId)
{
    switch (responseId) {
        case Gtk::RESPONSE_OK: {
            getAppChooserDialog()->close();
            const auto app_info = getAppChooserDialog()->get_app_info();
            external_editor_info = {
                app_info->get_name(),
                app_info->get_commandline(),
                false};
            sendToExternal();
            break;
        }
        case Gtk::RESPONSE_CANCEL:
        case Gtk::RESPONSE_CLOSE:
            getAppChooserDialog()->close();
            break;
        default:
            break;
    }
}

void EditorPanel::updateExternalEditorSelection()
{
    const auto& options = App::get().options();
    int index = send_to_external->getSelected();
    if (index >= 0 && static_cast<unsigned>(index) == options.externalEditors.size()) {
        index = -1;
    }
    if (options.externalEditorIndex != index) {
        send_to_external->setSelected(
            options.externalEditorIndex >= 0 ? options.externalEditorIndex : options.externalEditors.size());
    }
}

void EditorPanel::historyBeforeLineChanged (const rtengine::procparams::ProcParams& params)
{

    if (beforeIpc) {
        ProcParams* pparams = beforeIpc->beginUpdateParams ();
        *pparams = params;
        beforeIpc->endUpdateParams (rtengine::EvProfileChanged);  // starts the IPC processing
    }
}

void EditorPanel::beforeAfterToggled ()
{

    if (!ipc) {
        return;
    }

    removeIfThere (beforeAfterBox,  beforeBox, false);
    removeIfThere (afterBox,  afterHeaderBox, false);
    beforeAfterBox->set_homogeneous (false);

    if (beforeIarea) {
        if (beforeIpc) {
            beforeIpc->stopProcessing ();
        }

        iareapanel->setBeforeAfterViews (nullptr, iareapanel);
        iareapanel->imageArea->iLinkedImageArea = nullptr;
        delete beforeIarea;
        beforeIarea = nullptr;

        if (beforeIpc) {
            beforeIpc->setPreviewImageListener (nullptr);
        }

        delete beforePreviewHandler;
        beforePreviewHandler = nullptr;

        if (beforeIpc) {
            rtengine::StagedImageProcessor* old = beforeIpc;
            beforeIpc = nullptr;
            std::thread([old]() {
                rtengine::StagedImageProcessor::destroy(old);
            }).detach();
        }

        beforeIpc = nullptr;
    }

    if (beforeAfter->get_active ()) {

        int errorCode = 0;
        rtengine::InitialImage *beforeImg = rtengine::InitialImage::load ( isrc->getImageSource ()->getFileName(),  openThm->getType() == FT_Raw, &errorCode, nullptr);

        if ( !beforeImg || errorCode ) {
            return;
        }

        beforeIarea = new ImageAreaPanel ();

        int HeaderBoxHeight = 15;

        beforeLabel = Gtk::manage (new Gtk::Label (M ("GENERAL_BEFORE")));
        beforeLabel->get_style_context()->add_class("ba-label");
        tbBeforeLock = Gtk::manage (new Gtk::ToggleButton ());
        tbBeforeLock->get_style_context()->add_class("ba-lock");
        tbBeforeLock->set_relief(Gtk::RELIEF_NONE);
        tbBeforeLock->set_tooltip_markup (M ("MAIN_TOOLTIP_BEFOREAFTERLOCK"));
        tbBeforeLock->signal_toggled().connect ( sigc::mem_fun (*this, &EditorPanel::tbBeforeLock_toggled) );
        beforeHeaderBox = Gtk::manage (new Gtk::Box (Gtk::ORIENTATION_HORIZONTAL));
        beforeHeaderBox->get_style_context()->add_class("smallbuttonbox");
        beforeHeaderBox->pack_start (*beforeLabel, Gtk::PACK_EXPAND_WIDGET, 0);
        beforeHeaderBox->pack_end (*tbBeforeLock, Gtk::PACK_SHRINK, 0);
        beforeHeaderBox->set_size_request (0, HeaderBoxHeight);

        history->blistenerLock ? tbBeforeLock->set_image (*iBeforeLockON) : tbBeforeLock->set_image (*iBeforeLockOFF);
        tbBeforeLock->set_active (history->blistenerLock);

        beforeBox = Gtk::manage (new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
        beforeBox->pack_start (*beforeHeaderBox, Gtk::PACK_SHRINK, 0);
        beforeBox->pack_start (*beforeIarea);

        afterLabel = Gtk::manage (new Gtk::Label (M ("GENERAL_AFTER")));
        afterLabel->get_style_context()->add_class("ba-label");
        afterHeaderBox = Gtk::manage (new Gtk::Box (Gtk::ORIENTATION_HORIZONTAL));
        afterHeaderBox->set_size_request (0, HeaderBoxHeight);
        afterHeaderBox->pack_start (*afterLabel, Gtk::PACK_EXPAND_WIDGET, 0);
        afterBox->pack_start (*afterHeaderBox, Gtk::PACK_SHRINK, 0);
        afterBox->reorder_child (*afterHeaderBox, 0);

        beforeAfterBox->pack_start (*beforeBox);
        beforeAfterBox->reorder_child (*beforeBox, 0);
        beforeAfterBox->set_homogeneous (true);

        // Set up IPC BEFORE show_all so that when size_allocate fires,
        // ipc is already set and mainCropWindow can be created
        beforePreviewHandler = new PreviewHandler ();

        beforeIpc = rtengine::StagedImageProcessor::create (beforeImg);
        beforeIpc->setPreviewScale (10);
        beforeIpc->setPreviewImageListener (beforePreviewHandler);
        Glib::ustring monitorProfile;
        rtengine::RenderingIntent intent;
        ipc->getMonitorProfile(monitorProfile, intent);
        beforeIpc->setMonitorProfile(monitorProfile, intent);

        beforeIarea->imageArea->setPreviewHandler (beforePreviewHandler);
        beforeIarea->imageArea->setImProcCoordinator (beforeIpc);

        beforeIarea->imageArea->setPreviewModePanel (iareapanel->imageArea->previewModePanel);
        beforeIarea->imageArea->setIndicateClippedPanel (iareapanel->imageArea->indClippedPanel);
        iareapanel->imageArea->iLinkedImageArea = beforeIarea->imageArea;

        iareapanel->setBeforeAfterViews (beforeIarea, iareapanel);
        beforeIarea->setBeforeAfterViews (beforeIarea, iareapanel);

        // Show all AFTER IPC is fully configured
        beforeAfterBox->show_all ();

        rtengine::procparams::ProcParams params;

        if (history->getBeforeLineParams (params)) {
            historyBeforeLineChanged (params);
        }
    }
}

void EditorPanel::tbBeforeLock_toggled ()
{
    history->blistenerLock = tbBeforeLock->get_active();
    tbBeforeLock->get_active() ? tbBeforeLock->set_image (*iBeforeLockON) : tbBeforeLock->set_image (*iBeforeLockOFF);
}

void EditorPanel::histogramChanged(
    const LUTu& histRed,
    const LUTu& histGreen,
    const LUTu& histBlue,
    const LUTu& histLuma,
    const LUTu& histToneCurve,
    const LUTu& histLCurve,
    const LUTu& histCCurve,
    const LUTu& histLCAM,
    const LUTu& histCCAM,
    const LUTu& histRedRaw,
    const LUTu& histGreenRaw,
    const LUTu& histBlueRaw,
    const LUTu& histChroma,
    const LUTu& histLRETI,
    int vectorscopeScale,
    const array2D<int>& vectorscopeHC,
    const array2D<int>& vectorscopeHS,
    int waveformScale,
    const array2D<int>& waveformRed,
    const array2D<int>& waveformGreen,
    const array2D<int>& waveformBlue,
    const array2D<int>& waveformLuma
)
{
    if (histogramPanel) {
        histogramPanel->histogramChanged(histRed, histGreen, histBlue, histLuma, histChroma, histRedRaw, histGreenRaw, histBlueRaw, vectorscopeScale, vectorscopeHC, vectorscopeHS, waveformScale, waveformRed, waveformGreen, waveformBlue, waveformLuma);
    }

    tpc->updateCurveBackgroundHistogram(histToneCurve, histLCurve, histCCurve, histLCAM, histCCAM, histRed, histGreen, histBlue, histLuma, histLRETI);
}

void EditorPanel::setObservable(rtengine::HistogramObservable* observable)
{
    histogram_observable = observable;
}

bool EditorPanel::updateHistogram(void) const
{
    return histogram_scope_type == ScopeType::HISTOGRAM
        || histogram_scope_type == ScopeType::NONE;
}

bool EditorPanel::updateHistogramRaw(void) const
{
    return histogram_scope_type == ScopeType::HISTOGRAM_RAW
        || histogram_scope_type == ScopeType::NONE;
}

bool EditorPanel::updateVectorscopeHC(void) const
{
    return
        histogram_scope_type == ScopeType::VECTORSCOPE_HC
        || histogram_scope_type == ScopeType::NONE;
}

bool EditorPanel::updateVectorscopeHS(void) const
{
    return
        histogram_scope_type == ScopeType::VECTORSCOPE_HS
        || histogram_scope_type == ScopeType::NONE;
}

bool EditorPanel::updateWaveform(void) const
{
    return histogram_scope_type == ScopeType::WAVEFORM
        || histogram_scope_type == ScopeType::PARADE
        || histogram_scope_type == ScopeType::NONE;
}

void EditorPanel::scopeTypeChanged(ScopeType new_type)
{
    histogram_scope_type = new_type;

    if (!histogram_observable) {
        return;
    }

    // Make sure the new scope is updated since we only actively update the
    // current scope.
    switch (new_type) {
        case ScopeType::HISTOGRAM:
            histogram_observable->requestUpdateHistogram();
            break;
        case ScopeType::HISTOGRAM_RAW:
            histogram_observable->requestUpdateHistogramRaw();
            break;
        case ScopeType::VECTORSCOPE_HC:
            histogram_observable->requestUpdateVectorscopeHC();
            break;
        case ScopeType::VECTORSCOPE_HS:
            histogram_observable->requestUpdateVectorscopeHS();
            break;
        case ScopeType::PARADE:
        case ScopeType::WAVEFORM:
            histogram_observable->requestUpdateWaveform();
            break;
        case ScopeType::NONE:
            break;
    }
}

bool EditorPanel::CheckSidePanelsVisibility()
{
    if (tbTopPanel_1) {
        return tbTopPanel_1->get_active() || tbRightPanel_1->get_active() || hidehp->get_active();
    }

    return tbRightPanel_1->get_active() || hidehp->get_active();
}

void EditorPanel::toggleSidePanels()
{
    // Maximize preview panel:
    // toggle top AND right AND history panels

    bool bAllSidePanelsVisible;
    bAllSidePanelsVisible = CheckSidePanelsVisibility();

    if (tbTopPanel_1) {
        tbTopPanel_1->set_active (!bAllSidePanelsVisible);
    }

    tbRightPanel_1->set_active (!bAllSidePanelsVisible);
    hidehp->set_active (!bAllSidePanelsVisible);

    if (!bAllSidePanelsVisible) {
        tbShowHideSidePanels->set_image (*iShowHideSidePanels);
    } else {
        tbShowHideSidePanels->set_image (*iShowHideSidePanels_exit);
    }
}

void EditorPanel::toggleSidePanelsZoomFit()
{
    toggleSidePanels();

    // fit image preview
    // !!! TODO this does not want to work... seems to have an effect on a subsequent key press
    // iarea->imageArea->zoomPanel->zoomFitClicked();
}

void EditorPanel::tbShowHideSidePanels_managestate()
{
    bool bAllSidePanelsVisible;
    bAllSidePanelsVisible = CheckSidePanelsVisibility();
    ShowHideSidePanelsconn.block (true);

    tbShowHideSidePanels->set_active (!bAllSidePanelsVisible);

    ShowHideSidePanelsconn.block (false);
}

PreviewModePanel* EditorPanel::getPreviewModePanel()
{
    return iareapanel ? iareapanel->imageArea->previewModePanel : nullptr;
}

IndicateClippedPanel* EditorPanel::getIndicateClippedPanel()
{
    return iareapanel ? iareapanel->imageArea->indClippedPanel : nullptr;
}

// Color management accessors delegating to ColorManagementToolbar
int EditorPanel::getRenderingIntent () const { return colorMgmtToolBar ? colorMgmtToolBar->getIntent () : 0; }
void EditorPanel::setRenderingIntent (int i) { if (colorMgmtToolBar) colorMgmtToolBar->setIntent (i); }
bool EditorPanel::getSoftProofing () const { return colorMgmtToolBar ? colorMgmtToolBar->getSoftProof () : false; }
void EditorPanel::setSoftProofing (bool a) { if (colorMgmtToolBar) colorMgmtToolBar->setSoftProof (a); }
bool EditorPanel::getGamutCheck () const { return colorMgmtToolBar ? colorMgmtToolBar->getGamutCheck () : false; }
void EditorPanel::setGamutCheck (bool a) { if (colorMgmtToolBar) colorMgmtToolBar->setGamutCheck (a); }
int EditorPanel::getMonitorProfileIndex () const { return colorMgmtToolBar ? colorMgmtToolBar->getProfileIndex () : 0; }
void EditorPanel::setMonitorProfileIndex (int i) { if (colorMgmtToolBar) colorMgmtToolBar->setProfileIndex (i); }
int EditorPanel::getMonitorProfileCount () const { return colorMgmtToolBar ? colorMgmtToolBar->getProfileCount () : 0; }
Glib::ustring EditorPanel::getMonitorProfileName (int i) const { return colorMgmtToolBar ? colorMgmtToolBar->getProfileName (i) : ""; }

void EditorPanel::updateExternalEditorWidget(int selectedIndex, const std::vector<ExternalEditor> &editors)
{
    // Remove the editors.
    while (send_to_external->getEntryCount()) {
        send_to_external->removeEntry(send_to_external->getEntryCount() - 1);
    }

    // Create new radio button group because they cannot be reused: https://developer-old.gnome.org/gtkmm/3.16/classGtk_1_1RadioButtonGroup.html#details.
    send_to_external_radio_group = Gtk::RadioButtonGroup();

    // Add the editors.
    for (unsigned i = 0; i < editors.size(); i++) {
        const auto & name = editors[i].name.empty() ? Glib::ustring(" ") : editors[i].name;
        if (!editors[i].icon_serialized.empty()) {
            Glib::RefPtr<Gio::Icon> gioIcon;
            GError *e = nullptr;
            GVariant *icon_variant = g_variant_parse(
                nullptr, editors[i].icon_serialized.c_str(), nullptr, nullptr, &e);

            if (e) {
                std::cerr
                    << "Error loading external editor icon from \""
                    << editors[i].icon_serialized << "\": " << e->message
                    << std::endl;
                gioIcon = Glib::RefPtr<Gio::Icon>();
            } else {
                gioIcon = Gio::Icon::deserialize(Glib::VariantBase(icon_variant));
            }

            send_to_external->insertEntry(i, gioIcon, name, &send_to_external_radio_group);
        } else {
            send_to_external->insertEntry(i, "palette-brush", name, &send_to_external_radio_group);
        }
    }

#ifndef __APPLE__
    send_to_external->addEntry("palette-brush", M("GENERAL_OTHER"), &send_to_external_radio_group);
#endif
    send_to_external->set_sensitive(send_to_external->getEntryCount());
    send_to_external->setSelected(selectedIndex);
    send_to_external->show();
}

void EditorPanel::updateProfiles (const Glib::ustring &printerProfile, rtengine::RenderingIntent printerIntent, bool printerBPC)
{
}

void EditorPanel::updateTPVScrollbar (bool hide)
{
    tpc->updateTPVScrollbar (hide);
}

void EditorPanel::updateHistogramPosition (int oldPosition, int newPosition)
{
    const auto& options = App::get().options();

    switch (newPosition) {
        case 0:

            // No histogram
            if (oldPosition) {
                // A histogram actually exists, we delete it
                if (oldPosition == 2) {
                    removeIfThere(histogramRow_, histogramPanel, false);
                }
                delete histogramPanel;
                histogramPanel = nullptr;
            }

            // else no need to create it
            break;

        case 1:

            // Histogram on the left pane — remove placeholder if present
            if (auto* child1 = leftbox->get_child1()) {
                if (child1 != histogramPanel) {
                    leftbox->remove(*child1);
                }
            }
            if (oldPosition == 0) {
                // There was no Histogram before, so we create it
                histogramPanel = Gtk::manage (new HistogramPanel ());
                leftbox->pack1(*histogramPanel, false, false);
            } else if (oldPosition == 2) {
                // The histogram was on the right side, so we move it to the left
                histogramPanel->reference();
                removeIfThere (histogramRow_, histogramPanel, false);
                leftbox->pack1(*histogramPanel, false, false);
                histogramPanel->unreference();
            }

            leftbox->set_position(options.histogramHeight);
            histogramPanel->reorder (Gtk::POS_LEFT);
            break;

        case 2:
        default:

            // Histogram on the right pane (inside histogramRow_, beside filmstrip button)
            if (oldPosition == 0) {
                // There was no Histogram before, so we create it
                histogramPanel = Gtk::manage (new HistogramPanel ());
                histogramPanel->set_size_request(-1, 120);
                histogramRow_->pack_start (*histogramPanel);
            } else if (oldPosition == 1) {
                // The histogram was on the left side, so we move it to the right
                histogramPanel->reference();
                removeIfThere (leftbox, histogramPanel, false);
                histogramPanel->set_size_request(-1, 120);
                histogramRow_->pack_start (*histogramPanel);
                histogramPanel->unreference();
            }

            histogramPanel->reorder (Gtk::POS_RIGHT);
            break;
    }

    if (histogramPanel) {
        histogramPanel->setPanelListener(this);
    }

    iareapanel->imageArea->setPointerMotionHListener (histogramPanel);

}

void EditorPanel::updateToolPanelToolLocations(
    const std::vector<Glib::ustring> &favorites, bool cloneFavoriteTools)
{
    if (tpc) {
        tpc->updateToolLocations(favorites, cloneFavoriteTools);
    }
}

void EditorPanel::defaultMonitorProfileChanged (const Glib::ustring &profile_name, bool auto_monitor_profile)
{
    colorMgmtToolBar->defaultMonitorProfileChanged (profile_name, auto_monitor_profile);
}

