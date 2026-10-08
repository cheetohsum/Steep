#include "phototagwidgets.h"
#include "options.h"
#include "multilangmgr.h"

PhotoTags& PhotoTags::instance()
{
    static PhotoTags store(Glib::build_filename(Options::rtdir, "photo-tags"));
    return store;
}

PhotoTagFilter::PhotoTagFilter() : box_(Gtk::ORIENTATION_VERTICAL, 8),
    rows_(Gtk::ORIENTATION_VERTICAL, 2), all_(M("TAGS_MATCH_ALL")),
    untagged_(M("TAGS_UNTAGGED")), clear_(M("TAGS_CLEAR_FILTER"))
{
    set_name("PhotoTagFilter");
    set_relief(Gtk::RELIEF_NONE);
    set_tooltip_text(M("TAGS_FILTER_HINT"));
    set_popover(popover_);
    box_.set_border_width(12);
    box_.set_size_request(280, -1);
    search_.set_placeholder_text(M("TAGS_SEARCH"));
    box_.pack_start(search_, Gtk::PACK_SHRINK);
    box_.pack_start(all_, Gtk::PACK_SHRINK);
    box_.pack_start(untagged_, Gtk::PACK_SHRINK);
    scroll_.set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    scroll_.set_min_content_height(220);
    scroll_.set_max_content_height(340);
    scroll_.add(rows_);
    box_.pack_start(scroll_);
    box_.pack_start(clear_, Gtk::PACK_SHRINK);
    popover_.add(box_);
    search_.signal_search_changed().connect(sigc::mem_fun(*this, &PhotoTagFilter::rebuild));
    all_.signal_toggled().connect([this]() { if (!syncing_) changed_.emit(); });
    untagged_.signal_toggled().connect([this]() {
        if (syncing_) return;
        if (untagged()) selected_.clear();
        rebuild(); updateLabel(); changed_.emit();
    });
    clear_.signal_clicked().connect(sigc::mem_fun(*this, &PhotoTagFilter::clear));
    storeConnection_ = PhotoTags::instance().changed().connect([this]() { rebuild(); updateLabel(); changed_.emit(); });
    rebuild(); updateLabel();
}

PhotoTagFilter::~PhotoTagFilter()
{
    storeConnection_.disconnect();
    for (auto* child : rows_.get_children()) { rows_.remove(*child); delete child; }
}

void PhotoTagFilter::updateLabel()
{
    set_label(untagged() ? M("TAGS_UNTAGGED") : selected_.empty() ? M("TAGS_FILTER") :
        Glib::ustring::compose(M("TAGS_FILTER_COUNT"), selected_.size()));
    get_style_context()->remove_class("tags-active");
    if (untagged() || !selected_.empty()) get_style_context()->add_class("tags-active");
}

void PhotoTagFilter::rebuild()
{
    syncing_ = true;
    for (auto* child : rows_.get_children()) { rows_.remove(*child); delete child; }
    const auto query = search_.get_text().casefold();
    const auto definitions = PhotoTags::instance().definitions();
    for (auto it = selected_.begin(); it != selected_.end();) {
        if (!definitions.count(*it)) it = selected_.erase(it); else ++it;
    }
    int count = 0;
    for (const auto& d : definitions) {
        if (!query.empty() && d.second.name.casefold().find(query) == Glib::ustring::npos &&
            d.second.description.casefold().find(query) == Glib::ustring::npos) continue;
        auto* check = new Gtk::CheckButton(d.second.name);
        check->set_active(selected_.count(d.first));
        check->set_tooltip_text(d.second.description);
        check->signal_toggled().connect([this, check, key = d.first]() {
            if (syncing_) return;
            if (check->get_active()) selected_.insert(key); else selected_.erase(key);
            syncing_ = true; untagged_.set_active(false); syncing_ = false;
            updateLabel(); changed_.emit();
        });
        rows_.pack_start(*check, Gtk::PACK_SHRINK);
        ++count;
    }
    if (!count) rows_.pack_start(*new Gtk::Label(M("TAGS_NO_MATCHES")), Gtk::PACK_SHRINK);
    rows_.show_all();
    box_.show_all();
    syncing_ = false;
}

void PhotoTagFilter::clear() { setSelection({}, false, false); }

void PhotoTagFilter::setSelection(const PhotoTags::Tags& tags, bool all, bool untagged)
{
    syncing_ = true;
    selected_ = tags;
    all_.set_active(all);
    untagged_.set_active(untagged);
    syncing_ = false;
    rebuild(); updateLabel(); changed_.emit();
}

Glib::ustring photoTagSummary(const Glib::ustring& path)
{
    Glib::ustring text;
    const auto definitions = PhotoTags::instance().definitions();
    for (const auto& tag : PhotoTags::instance().tags(path)) {
        if (!text.empty()) text += " · ";
        const auto it = definitions.find(tag);
        text += it == definitions.end() ? Glib::ustring(tag) : it->second.name;
    }
    return text;
}

bool applyPhotoTags(Gtk::Window* parent, const std::vector<Glib::ustring>& paths,
                    const PhotoTags::Tags& add, const PhotoTags::Tags& remove)
{
    if (paths.empty()) return false;
    Glib::ustring error;
    if (PhotoTags::instance().edit(paths, add, remove, {}, error)) return true;
    Gtk::MessageDialog message(M("TAGS_SAVE_ERROR"), false, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK);
    if (parent) message.set_transient_for(*parent);
    message.set_secondary_text(error);
    message.run();
    return false;
}

bool editPhotoTags(Gtk::Window* parent, const std::vector<Glib::ustring>& paths)
{
    if (paths.empty()) return false;
    Gtk::Dialog dlg(M("TAGS_EDIT"), true);
    if (parent) dlg.set_transient_for(*parent);
    dlg.set_default_size(640, 600);
    dlg.add_button(M("GENERAL_CANCEL"), Gtk::RESPONSE_CANCEL);
    dlg.add_button(M("TAGS_APPLY"), Gtk::RESPONSE_OK);
    auto* content = dlg.get_content_area();
    content->set_border_width(16);
    content->set_spacing(10);
    Gtk::Label title(Glib::ustring::compose(M("TAGS_SELECTION"), paths.size()));
    title.set_halign(Gtk::ALIGN_START);
    Gtk::Label hint(M("TAGS_EDIT_HINT"));
    hint.set_line_wrap(true); hint.set_halign(Gtk::ALIGN_START);
    Gtk::Label countHint(M("TAGS_USAGE_HINT"));
    countHint.set_line_wrap(true); countHint.set_halign(Gtk::ALIGN_START);
    Gtk::SearchEntry search;
    search.set_placeholder_text(M("TAGS_SEARCH"));
    Gtk::ScrolledWindow scroll;
    scroll.set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    Gtk::Box rows(Gtk::ORIENTATION_VERTICAL, 4);
    scroll.add(rows);
    content->pack_start(title, Gtk::PACK_SHRINK);
    content->pack_start(hint, Gtk::PACK_SHRINK);
    content->pack_start(countHint, Gtk::PACK_SHRINK);
    content->pack_start(search, Gtk::PACK_SHRINK);
    content->pack_start(scroll);
    std::map<std::string, Gtk::CheckButton*> checks;
    std::map<std::string, Gtk::Box*> tagRows;
    std::map<std::string, int> counts;
    PhotoTags::Tags deleted;
    Gtk::Label deleteHint(M("TAGS_DELETE_HINT"));
    deleteHint.set_line_wrap(true); deleteHint.set_halign(Gtk::ALIGN_START);
    deleteHint.set_no_show_all(true);
    const auto totals = PhotoTags::instance().usageCounts();
    for (const auto& path : paths) for (const auto& tag : PhotoTags::instance().tags(path)) ++counts[tag];
    for (const auto& d : PhotoTags::instance().definitions()) {
        auto* row = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 10));
        auto* check = Gtk::manage(new Gtk::CheckButton(d.second.name));
        check->set_tooltip_text(d.second.description);
        check->set_active(counts[d.first] == static_cast<int>(paths.size()));
        check->set_inconsistent(counts[d.first] > 0 && counts[d.first] < static_cast<int>(paths.size()));
        check->signal_toggled().connect([check]() { check->set_inconsistent(false); });
        checks[d.first] = check;
        tagRows[d.first] = row;
        const auto total = totals.find(d.first);
        auto* usage = Gtk::manage(new Gtk::Label(Glib::ustring::compose(M("TAGS_USAGE_COUNT"),
            counts[d.first], paths.size(), total == totals.end() ? 0 : total->second)));
        usage->set_halign(Gtk::ALIGN_END);
        auto* deleteButton = Gtk::manage(new Gtk::Button(M("TAGS_DELETE")));
        deleteButton->set_tooltip_text(M("TAGS_DELETE_HINT"));
        deleteButton->signal_clicked().connect([&, check, deleteButton, key = d.first]() {
            if (deleted.count(key)) deleted.erase(key); else deleted.insert(key);
            const bool pending = deleted.count(key);
            check->set_sensitive(!pending);
            deleteButton->set_label(M(pending ? "TAGS_UNDO_DELETE" : "TAGS_DELETE"));
            deleteHint.set_visible(!deleted.empty());
        });
        row->pack_start(*check, Gtk::PACK_EXPAND_WIDGET);
        row->pack_start(*usage, Gtk::PACK_SHRINK);
        row->pack_start(*deleteButton, Gtk::PACK_SHRINK);
        rows.pack_start(*row, Gtk::PACK_SHRINK);
    }
    search.signal_search_changed().connect([&]() {
        auto query = search.get_text().casefold();
        for (const auto& c : checks) tagRows.at(c.first)->set_visible(query.empty() || c.second->get_label().casefold().find(query) != Glib::ustring::npos);
    });
    Gtk::Entry name, description;
    name.set_placeholder_text(M("TAGS_NEW_NAME"));
    description.set_placeholder_text(M("TAGS_NEW_DESCRIPTION"));
    content->pack_start(name, Gtk::PACK_SHRINK);
    content->pack_start(description, Gtk::PACK_SHRINK);
    content->pack_start(deleteHint, Gtk::PACK_SHRINK);
    dlg.show_all_children();
    while (dlg.run() == Gtk::RESPONSE_OK) {
        PhotoTags::Tags add, remove;
        for (const auto& c : checks) {
            if (deleted.count(c.first)) continue;
            if (c.second->get_inconsistent()) continue;
            if (c.second->get_active()) add.insert(c.first); else remove.insert(c.first);
        }
        std::vector<PhotoTags::Definition> define;
        if (!PhotoTags::tagKey(name.get_text()).empty()) {
            define.push_back({name.get_text(), description.get_text()});
            add.insert(PhotoTags::tagKey(name.get_text()));
        }
        Glib::ustring error;
        if (PhotoTags::instance().edit(paths, add, remove, define, error, {}, true, deleted)) return true;
        Gtk::MessageDialog message(dlg, M("TAGS_SAVE_ERROR"), false, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK);
        message.set_secondary_text(error);
        message.run();
    }
    return false;
}
