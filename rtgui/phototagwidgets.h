#pragma once
#include <functional>
#include <gtkmm.h>
#include "phototags.h"

class PhotoTagFilter final : public Gtk::MenuButton
{
public:
    PhotoTagFilter();
    ~PhotoTagFilter() override;
    PhotoTags::Tags selected() const { return selected_; }
    bool matchAll() const { return all_.get_active(); }
    bool untagged() const { return untagged_.get_active(); }
    void clear();
    void setSelection(const PhotoTags::Tags& tags, bool all, bool untagged);
    sigc::signal<void>& changed() { return changed_; }
    // An editing action kept inside the dropdown, under the filter controls.
    void setEditAction(const Glib::ustring& label, std::function<void()> action);
private:
    void rebuild();
    void updateLabel();
    Gtk::Popover popover_;
    Gtk::Box box_, rows_;
    Gtk::SearchEntry search_;
    Gtk::CheckButton all_, untagged_;
    Gtk::ScrolledWindow scroll_;
    Gtk::Button clear_;
    Gtk::Button edit_;
    std::function<void()> editAction_;
    PhotoTags::Tags selected_;
    bool syncing_ = false;
    sigc::connection storeConnection_;
    sigc::signal<void> changed_;
};

bool editPhotoTags(Gtk::Window* parent, const std::vector<Glib::ustring>& paths);
bool applyPhotoTags(Gtk::Window* parent, const std::vector<Glib::ustring>& paths,
                    const PhotoTags::Tags& add, const PhotoTags::Tags& remove = {});
Glib::ustring photoTagSummary(const Glib::ustring& path);
