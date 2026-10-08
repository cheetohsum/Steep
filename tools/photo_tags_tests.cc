#include "../rtgui/phototags.h"
#include <cassert>
#include <iostream>
#include <glib/gstdio.h>

int main(int argc, char** argv)
{
    Glib::init();
    assert(argc == 2);
    const Glib::ustring catalog = argv[1];
    const Glib::ustring first = "C:/Photos/Study/One.RAF", second = "C:/Photos/Study/Two.RAF";
    g_unlink(catalog.c_str());
    Glib::ustring error;
    PhotoTags store(catalog);
    assert(store.edit({first}, {"thresholds", "borrowed light"}, {},
        {{"Thresholds", "Between inside and outside"}, {"Borrowed Light", "Light entering a dark room"},
         {"Café", "Unicode title"}}, error));
    assert(store.edit({second}, {"thresholds"}, {}, {}, error));
    assert(store.matches(first, {"thresholds", "borrowed light"}, true));
    assert(!store.matches(second, {"thresholds", "borrowed light"}, true));
    assert(store.matches(second, {"borrowed light", "thresholds"}, false));
    assert(store.matches("C:/Photos/Absent.JPG", {}, false, true));
    assert(!store.matches(first, {}, false, true));
    PhotoTags restored(catalog);
    assert(restored.tags(first) == store.tags(first));
    assert(restored.recentTags() == std::vector<std::string>({"thresholds", "borrowed light"}));
    assert(restored.lastAppliedTags() == PhotoTags::Tags({"thresholds"}));
    // Re-applying an existing tag does not replace the last actual addition.
    assert(restored.edit({second}, {"thresholds"}, {}, {}, error));
    assert(restored.recentTags() == store.recentTags());
    assert(restored.lastAppliedTags() == store.lastAppliedTags());
#ifdef _WIN32
    assert(restored.tags("c:\\PHOTOS\\Study\\One.RAF") == store.tags(first));
#endif
    assert(PhotoTags::tagKey(" CAFE\xCC\x81 ") == PhotoTags::tagKey("Café"));
    // A bulk change preserves unrelated tags on individual photos.
    assert(restored.edit({first, second}, {"café"}, {"thresholds"}, {}, error));
    assert(restored.tags(first) == PhotoTags::Tags({"borrowed light", "café"}));
    assert(restored.tags(second) == PhotoTags::Tags({"café"}));
    assert(restored.edit({}, {}, {}, {{"THRESHOLDS", "duplicate"}}, error));
    assert(restored.definitions().size() == 3);
    assert(restored.edit({first, second}, {}, {}, {}, error,
        {{first, {"thresholds"}}, {second, {"borrowed light"}}}));
    assert(restored.matches(first, {"thresholds", "borrowed light"}, true));
    assert(restored.matches(second, {"borrowed light"}, true));
    assert(!restored.edit({first}, {"unknown"}, {}, {}, error));
    assert(!restored.edit({}, {}, {}, {{"bad;name", "invalid"}}, error));
    PhotoTags check(catalog);
    assert(check.tags(first) == restored.tags(first));
    const auto recent = check.recentTags();
    const auto last = check.lastAppliedTags();
    // A library import preserves the user's recent choices, including after reload.
    assert(check.edit({"C:/Photos/Imported.RAF"}, {"café"}, {}, {}, error, {}, false));
    PhotoTags imported(catalog);
    assert(imported.recentTags() == recent);
    assert(imported.lastAppliedTags() == last);
    // Multiple tags applied together can be repeated as a set; the list is bounded.
    std::vector<PhotoTags::Definition> many;
    PhotoTags::Tags manyKeys;
    for (int i = 0; i < 15; ++i) {
        const auto name = "Study " + std::to_string(i);
        many.push_back({name, ""}); manyKeys.insert(PhotoTags::tagKey(name));
    }
    assert(imported.edit({first, second}, manyKeys, {}, many, error));
    PhotoTags history(catalog);
    assert(history.recentTags().size() == 12);
    assert(history.lastAppliedTags() == manyKeys);
    assert(history.edit({first, second}, history.lastAppliedTags(), {}, {}, error));
    assert(history.recentTags().size() == 12);
    // Library counts include photos outside the active selection and unused tags.
    assert(history.usageCounts().at("café") == 3);
    assert(history.usageCounts().at("borrowed light") == 2);
    assert(history.usageCounts().at("study 0") == 2);
#ifdef _WIN32
    assert(history.edit({"c:\\PHOTOS\\Study\\One.RAF"}, {"café"}, {}, {}, error));
    assert(history.usageCounts().at("café") == 3);
#endif
    assert(history.edit({"C:/Photos/Only-deleted.RAF"}, {"solo"}, {},
        {{"Solo", "A disposable test tag"}, {"Unused", "No assignments"}}, error));
    assert(history.usageCounts().at("unused") == 0);
    const auto untouched = history.tags(second);
    assert(history.edit({first}, {}, {}, {}, error, {}, true, {" CAFÉ ", "Solo"}));
    PhotoTags deleted(catalog);
    assert(!deleted.definitions().count("café") && !deleted.definitions().count("solo"));
    assert(!deleted.usageCounts().count("café"));
    assert(deleted.tags("C:/Photos/Imported.RAF").empty());
    assert(deleted.tags("C:/Photos/Only-deleted.RAF").empty());
    auto remaining = untouched;
    remaining.erase("café");
    assert(deleted.tags(second) == remaining);
    assert(deleted.tags(first).count("thresholds"));
    assert(deleted.lastAppliedTags().empty());
    for (const auto& tag : deleted.recentTags()) assert(tag != "café" && tag != "solo");
    // A conflicting add/delete request cannot partly mutate the catalog.
    assert(!deleted.edit({first}, {"thresholds"}, {}, {}, error, {}, true, {"thresholds"}));
    assert(deleted.definitions().count("thresholds"));
    // Deletion has the same atomic rollback guarantee as assignment edits.
    const auto deleteFailure = catalog + ".delete-failure";
    PhotoTags failureSeed(deleteFailure);
    assert(failureSeed.edit({first}, {"keep"}, {}, {{"Keep", "Rollback test"}}, error));
    PhotoTags failure(deleteFailure);
    assert(g_unlink(deleteFailure.c_str()) == 0);
    assert(g_mkdir(deleteFailure.c_str(), 0700) == 0);
    assert(!failure.edit({}, {}, {}, {}, error, {}, true, {"keep"}));
    assert(failure.definitions().count("keep"));
    assert(failure.usageCounts().at("keep") == 1);
    assert(failure.lastAppliedTags() == PhotoTags::Tags({"keep"}));
    assert(g_rmdir(deleteFailure.c_str()) == 0);
    // Failed disk writes and unsupported catalogs must not discard existing data.
    PhotoTags impossible(catalog + "/missing/catalog");
    assert(!impossible.edit({first}, {"thresholds"}, {}, {{"Thresholds", ""}}, error));
    assert(impossible.tags(first).empty());
    assert(impossible.recentTags().empty());
    assert(impossible.lastAppliedTags().empty());
    const auto future = catalog + ".future";
    g_file_set_contents(future.c_str(), "[Catalog]\nVersion=999\n", -1, nullptr);
    PhotoTags newer(future);
    assert(!newer.edit({first}, {"thresholds"}, {}, {{"Thresholds", ""}}, error));
    g_file_set_contents(future.c_str(), "not a key file", -1, nullptr);
    PhotoTags malformed(future);
    assert(!malformed.edit({first}, {"thresholds"}, {}, {{"Thresholds", ""}}, error));
    g_unlink(future.c_str());
    std::cout << "Photo tags: persistence, Unicode/path normalization, any/all, untagged, bulk preservation, usage counts, global deletion and failure recovery passed\n";
}
