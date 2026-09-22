#pragma once

#include <atomic>
#include <functional>
#include <gtkmm.h>
#include "gradingpresets.h"
#include "guiutils.h"
#include "steeppopup.h"

class Thumbnail;

class GradingPresetMenu final : public Gtk::Button {
public:
    using Capture = std::function<bool(rtengine::procparams::ProcParams&, Thumbnail*&)>;
    using Apply = std::function<void(const gradingpresets::Grade&, const Glib::ustring&, bool)>;
    GradingPresetMenu(Capture capture, Apply apply, std::function<void()> cancel);
    ~GradingPresetMenu() override;
    void dismiss();
    void warm();
    void shutdown();

private:
    Capture capture_;
    Apply apply_;
    std::function<void()> cancel_;
    steepui::ListPopover popup_;
    Glib::ThreadPool pool_{1, true};
    IdleRegister idle_;
    std::atomic<unsigned> generation_{0};
    bool stopped_ = false, busy_ = false, open_ = false, keyboard_ = false;
    bool cached_ = false;
    gint64 retryAfter_ = 0;
    Glib::ustring cachedFile_, directory_;
    rtengine::procparams::ProcParams cachedParams_;
    gradingpresets::Features features_;
    sigc::connection hover_, leave_, dialog_, refresh_;
    void open();
    void requestAnalysis(const rtengine::procparams::ProcParams& params, Thumbnail* thumbnail);
    void schedulePreview(const gradingpresets::Preset& preset);
    void saveCurrent(const gradingpresets::Grade& grade);
    void manage(const gradingpresets::Grade& grade);
    bool editName(gradingpresets::Preset& preset, Gtk::Window* parent);
    void error(const Glib::ustring& message, Gtk::Window* parent = nullptr);
};
