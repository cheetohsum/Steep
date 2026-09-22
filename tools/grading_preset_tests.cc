#include "gradingpresets.h"
#include "paramsedited.h"
#include "rtengine/imagefloat.h"
#include "rtengine/improcfun.h"
#include "rtengine/labimage.h"
#include <giomm.h>
#include <glibmm/fileutils.h>
#include <iostream>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <set>
#include <limits>
#include <stdexcept>

using namespace gradingpresets;
using namespace rtengine::procparams;
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void apply(const Grade& grade, ProcParams& target)
{
    AutoPartialProfile profile;
    profile.set(false);
    profile.pparams->colorGrading = grade;
    ParamsEdited all(true);
    profile.pedited->colorGrading = all.colorGrading;
    profile.applyTo(&target, true);
}
std::string order(const std::vector<Preset>& presets)
{
    std::string result;
    for (const auto& p : presets) result += p.id + ";";
    return result;
}
}

int main(int argc, char** argv)
{
    try {
        Gio::init();
        require(argc == 2, "usage: steep-grading-tests <empty scratch directory>");
        const Glib::ustring directory(argv[1]);
        auto presets = bundled();
        require(presets.size() == 24, "twenty-four bundled grades");
        std::set<std::string> ids;
        std::vector<double> midtoneImpact;
        for (const auto& p : presets) {
            require(ids.insert(p.id).second, "unique bundled identities");
            auto shift = tonalShift(p.grade,50);
            midtoneImpact.push_back(std::hypot(shift[1],shift[2]));
        }
        std::sort(midtoneImpact.begin(),midtoneImpact.end());
        require(midtoneImpact[midtoneImpact.size()/2] > 8.5, "bundled midtones must have pronounced color character after blending");
        require(midtoneImpact.front() < 2 && midtoneImpact.back() > 12,
            "retain a gentle portrait option and genuinely bold grades");
        std::cout << "Median native midtone chroma shift: " << midtoneImpact[midtoneImpact.size()/2] << " Lab units\n";
        ProcParams baseline;
        baseline.toneCurve.expcomp = -1.3;
        baseline.toneCurve.histmatching = baseline.toneCurve.fromHistMatching = true;
        baseline.wb.temperature = 4370;
        baseline.crop.enabled = true;
        baseline.crop.x = 19; baseline.crop.y = 37;
        baseline.crop.w = 307; baseline.crop.h = 479;
        baseline.rotate.degree = 2.7;
        baseline.perspective.horizontal = 15;
        baseline.filmPresets.enabled = true;
        baseline.filmPresets.strength = 72;
        baseline.locallab.enabled = true;
        baseline.locallab.spots.emplace_back();
        baseline.spot.enabled = true;
        baseline.spot.entries.emplace_back();
        for (const auto& p : presets) {
            require(valid(p.grade) && p.grade.enabled, "bundled grade valid and enabled");
            require(p.grade.shadowsLum == 0 && p.grade.midtonesLum == 0
                && p.grade.highlightsLum == 0 && p.grade.globalLum == 0, "no bundled exposure manipulation");
            ProcParams fixture;
            fixture.colorGrading = p.grade;
            ParamsEdited selected(false), all(true);
            selected.colorGrading = all.colorGrading;
            require(fixture.save(Glib::build_filename(directory,p.id+".pp3"), "", true, &selected) == 0,
                "write grade-only native render fixture");
            rtengine::LabImage native(101,1,true,false);
            for (int x=0; x<101; ++x) { native.L[0][x] = x*327.68f; native.a[0][x]=5*327.68f; native.b[0][x]=12*327.68f; }
            rtengine::ImProcFunctions renderer(&fixture,false);
            renderer.colorGrading(&native,0,101,0,1,false);
            for (int x=0; x<101; ++x) {
                const auto shift = tonalShift(p.grade,x);
                require(std::abs(native.L[0][x]/327.68-x) < .0001, "native grades preserve lightness at all levels");
                require(std::abs(native.a[0][x]/327.68-5-shift[1]) < .0001
                    && std::abs(native.b[0][x]/327.68-12-shift[2]) < .0001, "ranking tonal model matches native grade math");
            }
            auto midsOnly = fixture;
            midsOnly.colorGrading.shadowsSat = midsOnly.colorGrading.highlightsSat = 0;
            midsOnly.colorGrading.globalSat = 0;
            rtengine::LabImage midtones(101,1,true,false);
            for (int x=0; x<101; ++x) { midtones.L[0][x] = x*327.68f; midtones.a[0][x]=midtones.b[0][x]=0; }
            rtengine::ImProcFunctions midtoneRenderer(&midsOnly,false);
            midtoneRenderer.colorGrading(&midtones,0,101,0,1,false);
            const double fullImpact = 40*p.grade.midtonesSat*p.grade.blending*.01;
            double previous = 0;
            for (int x=0; x<101; ++x) {
                const double impact = std::hypot(midtones.a[0][x],midtones.b[0][x])/327.68;
                if (x >= 25 && x <= 75) require(std::abs(impact-fullImpact) < .0001,
                    "midtone color must reach full requested strength across the native plateau");
                if (x == 0 || x == 100) require(impact < .0001, "midtone-only grade leaves tonal endpoints unchanged");
                require(std::abs(impact-previous) <= fullImpact*.061+.0001,
                    "stronger midtones retain smooth tonal transitions");
                previous = impact;
            }
            auto expected = baseline;
            expected.colorGrading = p.grade;
            for (const auto& previous : presets) {
                auto actual = baseline;
                apply(previous.grade, actual);
                apply(p.grade, actual);
                require(actual == expected, "switching touches only complete grading group");
                apply(p.grade, actual);
                require(actual == expected, "repeat application must be idempotent");
            }
        }
        std::cout << "PASS complete grading ownership, repeatability, switching and neutral luminance\n";

        auto custom = presets[2];
        custom.personal = true; custom.id.clear(); custom.name = "My / grade ..";
        custom.grade.shadowsLum = -17.25; custom.grade.midtonesLum = 6.5;
        custom.grade.highlightsLum = -3.2; custom.grade.globalLum = 1.25;
        custom.grade.globalHue = 359.8; custom.grade.globalSat = .13;
        custom.grade.blending = 43.75; custom.grade.balance = -27.5;
        save(directory, custom);
        unsigned rejected = 0;
        auto loaded = load(directory, rejected);
        require(loaded.size() == 1 && !rejected && loaded[0].grade == custom.grade, "exact fourteen-field round trip");
        require(loaded[0].name == custom.name && loaded[0].id == custom.id, "display names cannot escape storage directory");
        custom.name = "Renamed";
        save(directory, custom);
        require(load(directory,rejected).size() == 1, "update replaces instead of duplicating");
        auto duplicate = custom;
        duplicate.id.clear();
        save(directory,duplicate);
        require(duplicate.id != custom.id && load(directory,rejected).size() == 2, "same name remains distinct");
        auto invalid = custom.grade;
        invalid.shadowsSat = std::numeric_limits<double>::quiet_NaN();
        require(!valid(invalid), "reject NaN");
        invalid = custom.grade; invalid.balance = 101;
        require(!valid(invalid), "reject out of range");
        Glib::file_set_contents(Glib::build_filename(directory,"broken.grade"), "not a preset");
        require(load(directory,rejected).size() == 2 && rejected == 1, "corrupt entry cannot hide valid presets");
        bool protectedBuiltin = false;
        try { erase(directory,presets[0]); } catch (...) { protectedBuiltin = true; }
        require(protectedBuiltin, "cannot delete builtin");
        erase(directory,custom); erase(directory,duplicate);
        require(load(directory,rejected).empty(), "delete personal grades");
        std::cout << "PASS storage, updates, duplicate names, validation and safe deletion\n";

        rtengine::Imagefloat image(64,64);
        const auto fill = [&](float r, float g, float b) {
            for (int y=0; y<64; ++y) for (int x=0; x<64; ++x) {
                const float k = .3f + .7f*y/63;
                image.r(y,x) = r*k*65535; image.g(y,x) = g*k*65535; image.b(y,x) = b*k*65535;
            }
        };
        fill(.8,.55,.3);
        auto warm = analyze(image);
        require(warm.valid && warm.warm > warm.cool, "warm color analysis");
        const auto warmOrder = order(rank(presets,warm));
        require(warmOrder == order(rank(presets,warm)), "deterministic ranking");
        fill(.3,.55,.8);
        auto cool = analyze(image);
        require(cool.valid && cool.cool > cool.warm, "cool color analysis");
        require(warmOrder != order(rank(presets,cool)), "ranking responds to image colors");
        auto expressive = *std::find_if(presets.begin(),presets.end(),[](const Preset& p) { return p.id == "glacier"; });
        auto restrained = expressive;
        restrained.id = "test-muted-glacier";
        restrained.grade.midtonesSat *= .5;
        require(rank({restrained,expressive},cool).front().id == expressive.id,
            "suitable expressive mids must not lose to the same palette merely weakened");
        auto none = presets.front();
        none.id = "test-neutral";
        none.grade = Grade(); none.grade.enabled = true;
        auto choices = presets;
        choices.push_back(none);
        require(rank(choices,warm).front().id != none.id, "neutral/no-op must not win just for being weak");
        auto damaged = none;
        damaged.id = "test-extreme";
        damaged.grade.globalSat = .8; damaged.grade.globalHue = 150;
        damaged.grade.globalLum = 45;
        choices.push_back(damaged);
        auto checked = rank(choices,warm);
        for (size_t i=0; i<4; ++i) require(checked[i].id != damaged.id, "recommendations reject damaging casts and brightening");
        auto varied = rank(presets,cool);
        choices = presets;
        for (int i=0; i<6; ++i) {
            auto duplicate = varied.front();
            duplicate.personal = true;
            duplicate.id = "duplicate-"+std::to_string(i);
            choices.push_back(duplicate);
        }
        checked = rank(choices,cool);
        int repeats = 0;
        for (size_t i=0; i<4; ++i) repeats += checked[i].grade == varied.front().grade;
        require(repeats == 1, "recommended set must not be filled by duplicate palettes");

        const std::array<std::array<float,3>,5> scenes{{{{.8f,.55f,.3f}},{{.3f,.55f,.8f}},
            {{.32f,.65f,.24f}},{{.76f,.55f,.43f}},{{.1f,.22f,.28f}}}};
        std::set<std::string> leadingLooks;
        const auto started = std::chrono::steady_clock::now();
        for (const auto& scene : scenes) {
            fill(scene[0],scene[1],scene[2]);
            const auto features = analyze(image);
            require(features.samples.size() <= 1024, "bounded spatial samples");
            const auto ranked = rank(presets,features);
            leadingLooks.insert(ranked.front().id);
            std::cout << "Scene " << scene[0] << ',' << scene[1] << ',' << scene[2] << ":";
            for (int i=0; i<4; ++i) std::cout << ' ' << ranked[i].id;
            std::cout << '\n';
        }
        require(leadingLooks.size() >= 3, "different scene palettes need distinct leading suggestions");
        std::cout << "Five scene rankings: " << std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now()-started).count() << " ms\n";
        fill(0,0,0);
        require(!analyze(image).valid, "black fallback");
        fill(.5,.5,.5);
        require(!analyze(image).valid, "neutral fallback");
        require(order(rank(presets,Features())) == order(presets), "stable unavailable fallback");
        auto round = swatch(0,.2), wrap = swatch(360,.2);
        for (int i=0; i<3; ++i) require(std::abs(round[i]-wrap[i]) < 1e-10, "hue wraps continuously");
        Grade global;
        global.enabled = true; global.blending = 80;
        global.globalHue = 45; global.globalSat = .3;
        for (int zone=0; zone<3; ++zone) {
            const auto chip = swatch(global,zone);
            require(*std::max_element(chip.begin(),chip.end())-*std::min_element(chip.begin(),chip.end()) > .1,
                "chips show global-only grades");
        }
        auto midpoint = tonalShift(presets[2].grade,50);
        const auto oldChip = swatch(presets[2].grade.midtonesHue,presets[2].grade.midtonesSat);
        const auto newChip = swatch(presets[2].grade,1);
        const auto spread = [](const std::array<double,3>& rgb) { return *std::max_element(rgb.begin(),rgb.end())-*std::min_element(rgb.begin(),rgb.end()); };
        require(spread(newChip) > 1.4*spread(oldChip), "palette chips should be easier to distinguish");
        require(tonalShift(presets[2].grade,50) == midpoint, "swatch emphasis cannot mutate the grade");
        global.enabled = false;
        for (double l : {0.0,10.0,50.0,90.0,100.0}) {
            require(tonalShift(global,l) == std::array<double,3>{0,0,0}, "disabled grade has no effect");
        }
        for (double balance : {-100.0,0.0,100.0}) {
            global = custom.grade; global.balance = balance;
            for (double l : {0.0,12.0,35.0,50.0,77.0,100.0}) {
                const auto step = [](double a, double b, double x) { const double t=std::max(0.0,std::min(1.0,(x-a)/(b-a))); return t*t*(3-2*t); };
                const double sh=1-step(0,25+.15*balance,l), hi=step(75-.15*balance,100,l), mid=std::max(0.0,1-sh-hi);
                const double expected=(sh*global.shadowsLum+mid*global.midtonesLum+hi*global.highlightsLum+global.globalLum)*global.blending*.01;
                require(std::abs(tonalShift(global,l)[0]-expected) < 1e-10, "tonal model respects native balance/blending");
            }
        }
        std::cout << "PASS color-sensitive ranking, determinism, dark/neutral fallback and hue wrap\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; }
    catch (const Glib::Error& e) { std::cerr << "FAIL " << e.what() << '\n'; }
    return 1;
}
