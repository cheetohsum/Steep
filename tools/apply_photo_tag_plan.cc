#include "../rtgui/phototags.h"
#include <iostream>
#include <giomm.h>

// Apply a reviewed per-photo plan in one transaction. Never touches PP3s or images.
int main(int argc, char** argv)
{
    Gio::init();
    if (argc != 3) { std::cerr << "Usage: apply-photo-tag-plan CATALOG PLAN.ini\n"; return 2; }
    try {
        Glib::KeyFile plan;
        plan.load_from_file(argv[2]);
        if (plan.get_integer("Pilot", "Version") != 1) return 2;
        std::vector<PhotoTags::Definition> definitions;
        std::vector<Glib::ustring> paths;
        std::map<Glib::ustring, PhotoTags::Tags> assignments;
        int count = 0;
        for (const auto& group : plan.get_groups()) {
            if (group.raw().find("Tag_") == 0) definitions.push_back({plan.get_string(group,"Name"),plan.get_string(group,"Description")});
            if (group.raw().find("Photo_") == 0) {
                const auto path = plan.get_string(group,"Path");
                if (!Glib::file_test(path, Glib::FILE_TEST_IS_REGULAR)) { std::cerr << "Missing photo: " << path << '\n'; return 1; }
                paths.push_back(path);
                for (const auto& tag : plan.get_string_list(group,"Tags")) { assignments[path].insert(PhotoTags::tagKey(tag)); ++count; }
            }
        }
        PhotoTags store(argv[1]);
        Glib::ustring error;
        // Curatorial imports do not displace the user's recent quick-tag choices.
        if (!store.edit(paths, {}, {}, definitions, error, assignments, false)) { std::cerr << error << '\n'; return 1; }
        PhotoTags readback(argv[1]);
        for (const auto& item : assignments) if (!readback.matches(item.first, item.second, true)) { std::cerr << "Readback mismatch\n"; return 1; }
        std::cout << "Applied and verified " << count << " assignments on " << paths.size() << " photos; " << definitions.size() << " collection themes\n";
    } catch (const Glib::Error& e) { std::cerr << e.what() << '\n'; return 1; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
