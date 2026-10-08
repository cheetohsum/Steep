#include "phototags.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <algorithm>

namespace {
Glib::ustring trim(const Glib::ustring& text)
{
    auto result = text;
    while (!result.empty() && g_unichar_isspace(result[0])) result.erase(0, 1);
    while (!result.empty() && g_unichar_isspace(result[result.size() - 1])) result.erase(result.size() - 1);
    return result;
}
}

std::string PhotoTags::tagKey(const Glib::ustring& name)
{
    return trim(name).normalize().casefold().raw();
}

std::string PhotoTags::pathKey(const Glib::ustring& path)
{
    gchar* canonical = g_canonicalize_filename(path.c_str(), nullptr);
    Glib::ustring key(canonical);
    g_free(canonical);
#ifdef _WIN32
    auto raw = key.raw();
    for (auto& c : raw) if (c == '\\') c = '/';
    return Glib::ustring(raw).casefold().raw();
#else
    return key.raw();
#endif
}

PhotoTags::PhotoTags(const Glib::ustring& file) : file_(file)
{
    if (!Glib::file_test(file_, Glib::FILE_TEST_EXISTS)) return;
    try {
        Glib::KeyFile kf;
        kf.load_from_file(file_);
        if (kf.get_integer("Catalog", "Version") != 1) throw std::runtime_error("Unsupported tag catalog version");
        for (const auto& group : kf.get_groups()) {
            if (group.raw().find("Tag_") == 0) {
                Definition d{kf.get_string(group, "Name"), kf.get_string(group, "Description")};
                definitions_[tagKey(d.name)] = d;
            } else if (group.raw().find("Photo_") == 0) {
                Record r;
                r.path = kf.get_string(group, "Path");
                for (const auto& tag : kf.get_string_list(group, "Tags")) r.tags.insert(tagKey(tag));
                records_[pathKey(r.path)] = std::move(r);
            }
        }
        if (kf.has_group("Recent")) {
            if (kf.has_key("Recent", "Tags")) {
                for (const auto& name : kf.get_string_list("Recent", "Tags")) {
                    const auto key = tagKey(name);
                    if (definitions_.count(key) && std::find(recentTags_.begin(), recentTags_.end(), key) == recentTags_.end()) recentTags_.push_back(key);
                    if (recentTags_.size() == 12) break;
                }
            }
            if (kf.has_key("Recent", "LastApplied")) {
                for (const auto& name : kf.get_string_list("Recent", "LastApplied")) {
                    const auto key = tagKey(name);
                    if (definitions_.count(key)) lastAppliedTags_.insert(key);
                }
            }
        }
    } catch (const Glib::Error& e) {
        definitions_.clear();
        records_.clear();
        recentTags_.clear(); lastAppliedTags_.clear();
        loadError_ = e.what();
    } catch (const std::exception& e) {
        // Never overwrite a damaged or newer catalog with a partial load.
        definitions_.clear();
        records_.clear();
        recentTags_.clear(); lastAppliedTags_.clear();
        loadError_ = e.what();
    }
}

PhotoTags::Tags PhotoTags::tags(const Glib::ustring& path) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = records_.find(pathKey(path));
    return it == records_.end() ? Tags{} : it->second.tags;
}

bool PhotoTags::matches(const Glib::ustring& path, const Tags& wanted, bool all, bool untagged) const
{
    if (wanted.empty() && !untagged) return true;
    const auto have = tags(path);
    if (untagged) return have.empty();
    for (const auto& tag : wanted) {
        const bool found = have.count(tagKey(tag));
        if (all && !found) return false;
        if (!all && found) return true;
    }
    return all;
}

std::map<std::string, PhotoTags::Definition> PhotoTags::definitions() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return definitions_;
}

std::vector<Glib::ustring> PhotoTags::taggedPaths() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Glib::ustring> paths;
    for (const auto& r : records_) if (!r.second.tags.empty()) paths.push_back(r.second.path);
    return paths;
}

std::map<std::string, size_t> PhotoTags::usageCounts() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<std::string, size_t> counts;
    for (const auto& d : definitions_) counts[d.first] = 0;
    // Records are keyed by normalized paths, so aliases count as one photo.
    for (const auto& record : records_) for (const auto& tag : record.second.tags) ++counts[tag];
    return counts;
}

std::vector<std::string> PhotoTags::recentTags() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return recentTags_;
}

PhotoTags::Tags PhotoTags::lastAppliedTags() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return lastAppliedTags_;
}

bool PhotoTags::edit(const std::vector<Glib::ustring>& paths, const Tags& add, const Tags& remove,
                     const std::vector<Definition>& define, Glib::ustring& error,
                     const std::map<Glib::ustring, Tags>& individualAdd, bool rememberRecent,
                     const Tags& deleteDefinitions)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!loadError_.empty()) { error = loadError_; return false; }
        auto defs = definitions_;
        auto records = records_;
        auto recent = recentTags_;
        auto lastApplied = lastAppliedTags_;
        Tags newlyApplied;
        for (const auto& d : define) {
            const auto key = tagKey(d.name);
            if (key.empty() || key.size() > 160 || key.find_first_of(";\r\n\t") != std::string::npos) {
                error = "Tag names must be 1–160 bytes and cannot contain semicolons or line breaks.";
                return false;
            }
            auto it = defs.find(key);
            if (it == defs.end()) defs[key] = {trim(d.name), d.description};
        }
        Tags deleted;
        for (const auto& tag : deleteDefinitions) deleted.insert(tagKey(tag));
        for (const auto& tag : deleted) defs.erase(tag);
        for (auto it = records.begin(); it != records.end();) {
            for (const auto& tag : deleted) it->second.tags.erase(tag);
            if (it->second.tags.empty()) it = records.erase(it); else ++it;
        }
        recent.erase(std::remove_if(recent.begin(), recent.end(),
            [&deleted](const std::string& tag) { return deleted.count(tag); }), recent.end());
        for (const auto& tag : deleted) lastApplied.erase(tag);
        for (const auto& tag : add) if (!defs.count(tagKey(tag))) {
            error = "Unknown tag: " + tag;
            return false;
        }
        for (const auto& item : individualAdd) for (const auto& tag : item.second) if (!defs.count(tagKey(tag))) {
            error = "Unknown tag: " + tag;
            return false;
        }
        for (const auto& path : paths) {
            if (path.empty()) continue;
            const auto key = pathKey(path);
            auto& record = records[key];
            record.path = path;
            const auto before = record.tags;
            for (const auto& tag : remove) record.tags.erase(tagKey(tag));
            for (const auto& tag : add) record.tags.insert(tagKey(tag));
            const auto extra = individualAdd.find(path);
            if (extra != individualAdd.end()) for (const auto& tag : extra->second) record.tags.insert(tagKey(tag));
            for (const auto& tag : record.tags) if (!before.count(tag)) newlyApplied.insert(tag);
            if (record.tags.empty()) records.erase(key);
        }
        if (rememberRecent && !newlyApplied.empty()) {
            lastApplied = newlyApplied;
            for (const auto& tag : newlyApplied) {
                recent.erase(std::remove(recent.begin(), recent.end(), tag), recent.end());
            }
            recent.insert(recent.begin(), newlyApplied.begin(), newlyApplied.end());
            if (recent.size() > 12) recent.resize(12);
        }
        try {
            Glib::KeyFile kf;
            kf.set_integer("Catalog", "Version", 1);
            kf.set_string_list("Recent", "Tags", std::vector<Glib::ustring>(recent.begin(), recent.end()));
            kf.set_string_list("Recent", "LastApplied", std::vector<Glib::ustring>(lastApplied.begin(), lastApplied.end()));
            int i = 0;
            for (const auto& d : defs) {
                const auto group = "Tag_" + std::to_string(i++);
                kf.set_string(group, "Name", d.second.name);
                kf.set_string(group, "Description", d.second.description);
            }
            i = 0;
            for (const auto& r : records) {
                const auto group = "Photo_" + std::to_string(i++);
                kf.set_string(group, "Path", r.second.path);
                std::vector<Glib::ustring> tags(r.second.tags.begin(), r.second.tags.end());
                kf.set_string_list(group, "Tags", tags);
            }
            const auto data = kf.to_data();
            GError* err = nullptr;
            if (!g_file_set_contents(file_.c_str(), data.c_str(), data.bytes(), &err)) {
                error = err ? err->message : "Could not save tags";
                if (err) g_error_free(err);
                return false;
            }
            definitions_.swap(defs);
            records_.swap(records);
            recentTags_.swap(recent);
            lastAppliedTags_.swap(lastApplied);
        } catch (const Glib::Error& e) { error = e.what(); return false; }
        catch (const std::exception& e) { error = e.what(); return false; }
    }
    changed_.emit();
    return true;
}
