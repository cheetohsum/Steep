#include "gradingpresetmenu.h"

#include <algorithm>
#include "multilangmgr.h"
#include "options.h"
#include "rtimage.h"
#include "thumbnail.h"
#include "rtengine/imagefloat.h"

using namespace gradingpresets;
using rtengine::procparams::ProcParams;

namespace {
Glib::ustring title(const Preset& p) { return p.personal ? p.name : M(p.name); }
bool contains(Gtk::Widget& widget, int px, int py, int padding = 0)
{
    auto* top = widget.get_toplevel();
    if (!top || !top->get_window()) return false;
    int x = 0, y = 0, ox = 0, oy = 0;
    if (!widget.translate_coordinates(*top, 0, 0, x, y)) return false;
    top->get_window()->get_origin(ox, oy);
    return px >= ox+x-padding && py >= oy+y-padding
        && px < ox+x+widget.get_allocated_width()+padding
        && py < oy+y+widget.get_allocated_height()+padding;
}
}

GradingPresetMenu::GradingPresetMenu(Capture capture, Apply apply, std::function<void()> cancel) :
    capture_(std::move(capture)), apply_(std::move(apply)), cancel_(std::move(cancel)), popup_(*this),
    directory_(Glib::build_filename(Options::rtdir, "grading-presets"))
{
    // The anchor owns a native reference too; keep the C++ member alive until
    // its destructor when GTK disposes the parent button first.
    popup_.reference();
    set_name("GradingPresetButton");
    set_relief(Gtk::RELIEF_NONE);
    auto* icons = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 0));
    icons->pack_start(*Gtk::manage(new RTImage("profile-filled", Gtk::ICON_SIZE_MENU)), Gtk::PACK_SHRINK);
    icons->pack_start(*Gtk::manage(new RTImage("arrow-down-small", Gtk::ICON_SIZE_MENU)), Gtk::PACK_SHRINK);
    add(*icons);
    icons->show_all();
    signal_clicked().connect([this]() { if (open_) dismiss(); else open(); });
    popup_.set_name("GradingPresetMenu");
    popup_.signal_hide().connect([this]() {
        const bool wasOpen = open_;
        open_ = false;
        hover_.disconnect();
        leave_.disconnect();
        if (wasOpen && cancel_) cancel_();
    });
    popup_.signal_preview_left().connect([this]() { hover_.disconnect(); if (open_ && cancel_) cancel_(); });
    popup_.signal_key_press_event().connect([this](GdkEventKey*) { keyboard_ = true; return false; }, false);
    popup_.add_events(Gdk::POINTER_MOTION_MASK);
    popup_.signal_motion_notify_event().connect([this](GdkEventMotion*) { keyboard_ = false; return false; }, false);
    refresh_ = Glib::signal_timeout().connect([this]() {
        if (get_mapped() && !open_ && !busy_) warm();
        return true;
    }, 800);
}

GradingPresetMenu::~GradingPresetMenu() { shutdown(); }

void GradingPresetMenu::shutdown()
{
    if (stopped_) return;
    stopped_ = true;
    ++generation_;
    hover_.disconnect(); leave_.disconnect(); dialog_.disconnect(); refresh_.disconnect();
    // Join the bounded worker before destroying its idle register or callbacks.
    pool_.shutdown();
    idle_.destroy();
    cancel_ = {};
    capture_ = {};
    apply_ = {};
    popup_.hide();
}

void GradingPresetMenu::dismiss()
{
    hover_.disconnect(); leave_.disconnect();
    const bool wasOpen = open_;
    open_ = false;
    if (wasOpen) popup_.popdown();
    if (cancel_) cancel_();
}

void GradingPresetMenu::warm()
{
    if (stopped_) return;
    ProcParams params;
    Thumbnail* thumbnail = nullptr;
    if (capture_(params, thumbnail)) requestAnalysis(params, thumbnail);
}

void GradingPresetMenu::requestAnalysis(const ProcParams& source, Thumbnail* thumbnail)
{
    if (!thumbnail || busy_) return;
    ProcParams params = source;
    params.colorGrading = Grade();
    const auto file = thumbnail->getFileName();
    if (cachedFile_ == file && cachedParams_ == params
        && (cached_ || g_get_monotonic_time() < retryAfter_)) return;
    busy_ = true;
    const unsigned generation = ++generation_;
    thumbnail->increaseRef();
    pool_.push([this, params, thumbnail, file, generation]() {
        Features result;
        bool analyzed = false;
        try {
            // Work with a private cached thumbnail. No RAW upgrade and no
            // shared thumbnail lock held during rendering or feature analysis.
            auto image = thumbnail->processCachedAnalysisImage(params, 192);
            analyzed = bool(image);
            if (image && !params.blackwhite.enabled) result = analyze(*image);
        } catch (...) { }
        thumbnail->decreaseRef();
        if (generation_.load() != generation) return;
        idle_.add([this, params, file, result, analyzed, generation]() {
            busy_ = false;
            if (generation_.load() != generation) return false;
            cached_ = analyzed;
            retryAfter_ = g_get_monotonic_time()+3000000;
            cachedFile_ = file;
            cachedParams_ = params;
            features_ = result;
            // Do not reorder an open popup under the user's pointer.
            return false;
        });
    });
}

void GradingPresetMenu::schedulePreview(const Preset& preset)
{
    hover_.disconnect();
    hover_ = Glib::signal_timeout().connect([this, preset]() {
        if (open_ && apply_) apply_(preset.grade, title(preset), false);
        return false;
    }, 140);
}

void GradingPresetMenu::open()
{
    ProcParams params;
    Thumbnail* thumbnail = nullptr;
    if (stopped_ || !capture_(params, thumbnail)) return;
    dismiss();
    popup_.clear();
    unsigned rejected = 0;
    auto presets = bundled();
    try {
        auto personal = load(directory_, rejected);
        presets.insert(presets.end(), personal.begin(), personal.end());
    } catch (...) { ++rejected; }
    auto baseline = params;
    baseline.colorGrading = Grade();
    const bool ranked = cached_ && features_.valid && thumbnail && cachedFile_ == thumbnail->getFileName() && cachedParams_ == baseline;
    if (ranked) presets = rank(std::move(presets), features_);
    requestAnalysis(params, thumbnail);

    for (size_t i = 0; i < presets.size(); ++i) {
        if (ranked && (i == 0 || i == 4)) {
            if (i) popup_.addSeparator();
            auto* heading = popup_.addItem(M(i ? "TP_GRADING_MORE" : "TP_GRADING_SUGGESTED"), {});
            heading->set_sensitive(false);
            heading->get_style_context()->add_class("GradingPresetHeading");
        }
        const Preset preset = presets[i];
        auto* row = popup_.addItem(title(preset), [this, preset]() {
            dismiss();
            if (apply_) apply_(preset.grade, title(preset), true);
        }, [this, preset]() { if (open_) schedulePreview(preset); });
        row->remove();
        auto* content = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 7));
        auto* label = Gtk::manage(new Gtk::Label(title(preset)));
        label->set_xalign(0);
        label->set_max_width_chars(22);
        label->set_ellipsize(Pango::ELLIPSIZE_END);
        auto* selected = Gtk::manage(new Gtk::Image());
        selected->set_size_request(16, 16);
        if (preset.grade == params.colorGrading) selected->set_from_icon_name("object-select-symbolic", Gtk::ICON_SIZE_MENU);
        content->pack_start(*selected, Gtk::PACK_SHRINK);
        content->pack_start(*label, Gtk::PACK_EXPAND_WIDGET);
        auto* colors = Gtk::manage(new Gtk::DrawingArea());
        colors->set_size_request(57, 18);
        colors->signal_draw().connect([preset](const Cairo::RefPtr<Cairo::Context>& cr) {
            for (int n = 0; n < 3; ++n) {
                const auto rgb = swatch(preset.grade,n);
                cr->set_source_rgb(rgb[0], rgb[1], rgb[2]);
                cr->rectangle(n*19, 2, 17, 14); cr->fill();
            }
            return true;
        });
        content->pack_start(*colors, Gtk::PACK_SHRINK);
        row->add(*content);
    }
    popup_.addSeparator();
    popup_.addItem(M("TP_GRADING_RESET"), [this]() { dismiss(); apply_(Grade(), M("TP_GRADING_RESET"), true); });
    auto* save = popup_.addItem(M("TP_GRADING_SAVE"), [this, grade = params.colorGrading]() {
        dismiss();
        dialog_ = Glib::signal_idle().connect([this, grade]() { saveCurrent(grade); return false; });
    });
    save->set_sensitive(params.colorGrading.enabled);
    popup_.addItem(M("TP_GRADING_MANAGE"), [this, grade = params.colorGrading]() {
        dismiss();
        dialog_ = Glib::signal_idle().connect([this, grade]() { this->manage(grade); return false; });
    });
    if (rejected) {
        auto* warning = popup_.addItem(M("TP_GRADING_UNREADABLE"), {});
        warning->set_sensitive(false);
    }
    open_ = true;
    keyboard_ = false;
    popup_.show_all_children();
    popup_.popup();
    auto outside = std::make_shared<int>(0);
    leave_ = Glib::signal_timeout().connect([this, outside]() {
        if (!open_) return false;
        if (keyboard_) return true;
        auto display = Gdk::Display::get_default();
        auto seat = display ? display->get_default_seat() : Glib::RefPtr<Gdk::Seat>();
        auto pointer = seat ? seat->get_pointer() : Glib::RefPtr<Gdk::Device>();
        if (!pointer) return true;
        Glib::RefPtr<Gdk::Screen> screen;
        int x = 0, y = 0;
        pointer->get_position(screen, x, y);
        if (contains(*this,x,y,6) || contains(popup_,x,y,6)) { *outside = 0; return true; }
        if (++*outside < 4) return true;
        dismiss();
        return false;
    }, 50);
}

void GradingPresetMenu::error(const Glib::ustring& message, Gtk::Window* parent)
{
    Gtk::MessageDialog dialog(M("TP_GRADING_ERROR"), false, Gtk::MESSAGE_ERROR);
    if (!parent) parent = dynamic_cast<Gtk::Window*>(get_toplevel());
    if (parent) dialog.set_transient_for(*parent);
    dialog.set_secondary_text(message);
    dialog.run();
}

bool GradingPresetMenu::editName(Preset& preset, Gtk::Window* parent)
{
    Gtk::Dialog dialog(M("TP_GRADING_SAVE"), true);
    if (parent) dialog.set_transient_for(*parent);
    Gtk::Entry entry;
    entry.set_max_length(80);
    entry.set_text(preset.name);
    entry.set_activates_default(true);
    entry.set_margin_start(12); entry.set_margin_end(12);
    entry.set_margin_top(12); entry.set_margin_bottom(12);
    entry.set_placeholder_text(M("TP_GRADING_NAME"));
    dialog.get_content_area()->pack_start(entry);
    dialog.add_button(M("GENERAL_CANCEL"), Gtk::RESPONSE_CANCEL);
    dialog.add_button(M("GENERAL_SAVE"), Gtk::RESPONSE_OK);
    dialog.set_default_response(Gtk::RESPONSE_OK);
    entry.signal_changed().connect([&]() { dialog.set_response_sensitive(Gtk::RESPONSE_OK, !entry.get_text().empty()); });
    dialog.set_response_sensitive(Gtk::RESPONSE_OK, !entry.get_text().empty());
    dialog.show_all();
    entry.grab_focus();
    entry.select_region(0,-1);
    if (dialog.run() != Gtk::RESPONSE_OK) return false;
    preset.name = entry.get_text();
    return true;
}

void GradingPresetMenu::saveCurrent(const Grade& grade)
{
    if (!grade.enabled) return;
    Preset preset{"", "", grade, true};
    if (!editName(preset, dynamic_cast<Gtk::Window*>(get_toplevel()))) return;
    try { save(directory_, preset); }
    catch (const std::exception& e) { error(e.what()); }
    catch (const Glib::Error& e) { error(e.what()); }
}

void GradingPresetMenu::manage(const Grade& grade)
{
    Gtk::Dialog dialog(M("TP_GRADING_MANAGE"), true);
    if (auto* parent = dynamic_cast<Gtk::Window*>(get_toplevel())) dialog.set_transient_for(*parent);
    Gtk::ComboBoxText list;
    list.set_margin_start(12); list.set_margin_end(12);
    list.set_margin_top(12); list.set_margin_bottom(12);
    dialog.get_content_area()->pack_start(list);
    dialog.add_button(M("TP_GRADING_RENAME"), 1);
    dialog.add_button(M("TP_GRADING_UPDATE"), 2);
    dialog.add_button(M("TP_GRADING_DUPLICATE"), 3);
    dialog.add_button(M("TP_GRADING_DELETE"), 4);
    dialog.add_button(M("GENERAL_CLOSE"), Gtk::RESPONSE_CLOSE);
    std::vector<Preset> presets;
    auto refresh = [&]() {
        unsigned rejected = 0;
        presets = load(directory_, rejected);
        list.remove_all();
        for (const auto& p : presets) list.append(p.name);
        if (!presets.empty()) list.set_active(0);
        for (int i = 1; i <= 4; ++i) dialog.set_response_sensitive(i, !presets.empty() && (i != 2 || grade.enabled));
    };
    try { refresh(); }
    catch (...) { error(M("TP_GRADING_UNREADABLE")); return; }
    dialog.show_all();
    while (true) {
        const int response = dialog.run();
        if (response < 1 || response > 4) break;
        const int index = list.get_active_row_number();
        if (index < 0 || index >= static_cast<int>(presets.size())) continue;
        auto preset = presets[index];
        try {
            if (response == 1 || response == 3) {
                if (response == 3) preset.id.clear();
                if (!editName(preset, &dialog)) continue;
                save(directory_, preset);
            } else {
                Gtk::MessageDialog confirm(dialog, M(response == 2 ? "TP_GRADING_CONFIRM_UPDATE" : "TP_GRADING_CONFIRM_DELETE"),
                    false, Gtk::MESSAGE_QUESTION, Gtk::BUTTONS_OK_CANCEL, true);
                confirm.set_secondary_text(preset.name);
                if (confirm.run() != Gtk::RESPONSE_OK) continue;
                if (response == 2) { preset.grade = grade; save(directory_, preset); }
                else erase(directory_, preset);
            }
            refresh();
        } catch (const std::exception& e) { error(e.what(), &dialog); }
        catch (const Glib::Error& e) { error(e.what(), &dialog); }
    }
}
