#pragma once

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include <glibmm.h>
#include <sigc++/sigc++.h>

// A durable catalog beside options/albums, independent of disposable thumbnails
// and processing presets. Tag edits never rewrite photographs or their PP3s.
class PhotoTags final
{
public:
    struct Definition { Glib::ustring name, description; };
    using Tags = std::set<std::string>;
    static PhotoTags& instance();
    explicit PhotoTags(const Glib::ustring& file);
    static std::string tagKey(const Glib::ustring& name);
    static std::string pathKey(const Glib::ustring& path);
    Tags tags(const Glib::ustring& path) const;
    bool matches(const Glib::ustring& path, const Tags& wanted, bool all, bool untagged = false) const;
    std::map<std::string, Definition> definitions() const;
    std::map<std::string, size_t> usageCounts() const;
    std::vector<Glib::ustring> taggedPaths() const;
    std::vector<std::string> recentTags() const;
    Tags lastAppliedTags() const;
    // Single atomic transaction; an unsuccessful write leaves memory unchanged.
    // deleteDefinitions removes those tags from the entire catalog, beyond paths.
    bool edit(const std::vector<Glib::ustring>& paths, const Tags& add, const Tags& remove,
              const std::vector<Definition>& define, Glib::ustring& error,
              const std::map<Glib::ustring, Tags>& individualAdd = {}, bool rememberRecent = true,
              const Tags& deleteDefinitions = {});
    sigc::signal<void>& changed() { return changed_; }
private:
    struct Record { Glib::ustring path; Tags tags; };
    Glib::ustring file_;
    mutable std::mutex mutex_;
    std::map<std::string, Definition> definitions_;
    std::map<std::string, Record> records_;
    std::vector<std::string> recentTags_;
    Tags lastAppliedTags_;
    Glib::ustring loadError_;
    sigc::signal<void> changed_;
};
