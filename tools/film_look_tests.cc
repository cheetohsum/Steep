#include "rtengine/procparams.h"
#include "paramsedited.h"

#include <giomm.h>
#include <glibmm/fileutils.h>
#include <clocale>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace rtengine::procparams;

namespace {
void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}
}

int main(int argc, char** argv)
{
    try {
        Gio::init();
        std::setlocale(LC_NUMERIC, "C");
        require(argc == 3, "usage: steep-profile-tests <Film Looks directory> <scratch directory>");
        const Glib::ustring directory(argv[1]);
        const Glib::ustring scratch(argv[2]);
        require(Glib::file_test(scratch, Glib::FILE_TEST_IS_DIR), "scratch directory must exist");

        ProcParams baseline;
        baseline.toneCurve.expcomp = -0.73;
        baseline.toneCurve.shcompr = 11;
        baseline.toneCurve.hlcompr = 27;
        baseline.toneCurve.histmatching = true;
        baseline.toneCurve.fromHistMatching = true;
        baseline.wb.temperature = 4321;
        baseline.wb.green = 1.13;
        baseline.crop.enabled = true;
        baseline.crop.x = 39;
        baseline.crop.y = 51;
        baseline.crop.w = 413;
        baseline.crop.h = 529;
        baseline.rotate.degree = 2.3;
        baseline.perspective.horizontal = 12;
        baseline.perspective.vertical = -17;
        baseline.locallab.enabled = true;
        baseline.locallab.spots.emplace_back();
        baseline.spot.enabled = true;
        baseline.spot.entries.emplace_back();
        baseline.aiDenoise.enabled = true;
        baseline.aiDenoise.blend = 71;
        baseline.grain.enabled = true;
        baseline.grain.strength = 19;
        baseline.filmPresets.enabled = true;
        baseline.filmPresets.preset = "ember";
        baseline.filmPresets.warmth = 67;
        baseline.filmPresets.shadowTint = 39;
        baseline.filmPresets.pushPull = 1.75;

        std::vector<Glib::ustring> files;
        Glib::Dir dir(directory);
        for (const auto& name : dir) {
            if (name.size() >= 4 && name.substr(name.size() - 4) == ".pp3") {
                files.push_back(Glib::build_filename(directory, name));
            }
        }
        require(files.size() >= 10, "expected refreshed and new film looks");
        std::vector<FilmPresetsParams> recipes;
        for (const auto& file : files) {
            AutoPartialProfile look;
            require(look.load(file) == 0, "load " + file.raw());
            require(look.pedited->filmLook, "missing film-only policy: " + file.raw());
            require(look.pparams->filmPresets.modelVersion == 5, "look must opt into V5");
            require(!look.pedited->toneCurve.expcomp && !look.pedited->wb.temperature
                && !look.pedited->crop.enabled && !look.pedited->grain.enabled,
                "look selects protected settings");

            auto expected = baseline;
            expected.filmPresets = look.pparams->filmPresets;
            auto actual = baseline;
            look.applyTo(&actual);
            require(actual == expected, "look changed unrelated edits or inherited film fields: " + file.raw());
            look.applyTo(&actual);
            require(actual == expected, "reapplying look compounds its effect");
            recipes.push_back(actual.filmPresets);

            for (const auto& previous : recipes) {
                auto switched = baseline;
                switched.filmPresets = previous;
                look.applyTo(&switched);
                require(switched == expected, "A -> B left previous look settings active");
            }

            const auto saved = Glib::build_filename(scratch, "saved.pp3");
            require(actual.save(saved) == 0, "save full edited profile");
            AutoPartialProfile reopened;
            require(reopened.load(saved) == 0, "reopen full edited profile");
            require(!reopened.pedited->filmLook, "look application policy leaked into saved image state");
            require(reopened.pparams->filmPresets == actual.filmPresets, "saved film recipe changed");
            require(reopened.pparams->crop == actual.crop && reopened.pparams->wb == actual.wb,
                "saved crop/WB changed");
            std::cout << "PASS " << Glib::path_get_basename(file) << '\n';
        }

        const auto fixture = Glib::build_filename(scratch, "fixture.pp3");
        const std::string sparse = "[Steep Look]\nVersion=1\nKind=film\n"
            "[Film Presets]\nEnabled=true\nModelVersion=5\nPreset=sovereign\n";
        Glib::file_set_contents(fixture, sparse);
        AutoPartialProfile look;
        look.pparams->filmPresets.warmth = 99;
        require(look.load(fixture) == 0, "sparse recipe load");
        auto applied = baseline;
        look.applyTo(&applied);
        require(applied.filmPresets.warmth == 0, "sparse recipe inherited warmth");
        Glib::file_set_contents(fixture, sparse + "[White Balance]\nTemperature=8000\n");
        require(look.load(fixture) != 0, "unsafe recipe accepted WB override");
        Glib::file_set_contents(fixture, "[Steep Look]\nVersion=2\nKind=film\n"
            "[Film Presets]\nModelVersion=5\n");
        require(look.load(fixture) != 0, "unknown look version accepted");
        Glib::file_set_contents(fixture, "[Film Presets]\nEnabled=true\nModelVersion=4\nWarmth=12\n");
        require(look.load(fixture) == 0 && !look.pedited->filmLook, "legacy partial policy changed");
        auto legacy = baseline;
        look.applyTo(&legacy);
        require(legacy.filmPresets.modelVersion == 4 && legacy.filmPresets.warmth == 12
            && legacy.filmPresets.shadowTint == baseline.filmPresets.shadowTint,
            "legacy partial recipe no longer merges normally");
        std::cout << "PASS preservation, repeatability, switching, serialization, validation and legacy compatibility\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    } catch (const Glib::Error& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
