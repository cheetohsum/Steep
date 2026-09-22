#include "gradingpresets.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <glibmm/fileutils.h>
#include <glibmm/keyfile.h>
#include <glibmm/miscutils.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include "rtengine/imagefloat.h"

namespace gradingpresets {
namespace {
constexpr double pi = 3.14159265358979323846;
struct Field { const char* name; double Grade::*value; double low, high; };
const Field fields[] = {
    {"ShadowsHue", &Grade::shadowsHue, 0, 360}, {"ShadowsSat", &Grade::shadowsSat, 0, 1},
    {"ShadowsLum", &Grade::shadowsLum, -100, 100},
    {"MidtonesHue", &Grade::midtonesHue, 0, 360}, {"MidtonesSat", &Grade::midtonesSat, 0, 1},
    {"MidtonesLum", &Grade::midtonesLum, -100, 100},
    {"HighlightsHue", &Grade::highlightsHue, 0, 360}, {"HighlightsSat", &Grade::highlightsSat, 0, 1},
    {"HighlightsLum", &Grade::highlightsLum, -100, 100},
    {"GlobalHue", &Grade::globalHue, 0, 360}, {"GlobalSat", &Grade::globalSat, 0, 1},
    {"GlobalLum", &Grade::globalLum, -100, 100},
    {"Blending", &Grade::blending, 0, 100}, {"Balance", &Grade::balance, -100, 100}
};
bool validId(const std::string& id)
{
    return id.size() == 36 && std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'f') || (c >= '0' && c <= '9') || c == '-';
    });
}
double clamp(double x) { return std::max(0.0, std::min(1.0, x)); }
double smoothstep(double low, double high, double x)
{
    const double t = clamp((x-low)/(high-low));
    return t*t*(3-2*t);
}
struct TonalModel {
    std::array<std::array<double, 3>, 3> shifts;
    double shadowEnd, highlightStart;
    explicit TonalModel(const Grade& g)
    {
        const double hue[] = {g.shadowsHue, g.midtonesHue, g.highlightsHue};
        const double sat[] = {g.shadowsSat, g.midtonesSat, g.highlightsSat};
        const double lum[] = {g.shadowsLum, g.midtonesLum, g.highlightsLum};
        const double strength = g.enabled ? g.blending*.01 : 0;
        for (int i = 0; i < 3; ++i) {
            shifts[i] = {strength*(lum[i]+g.globalLum),
                40*strength*(sat[i]*std::cos(hue[i]*pi/180) + g.globalSat*std::cos(g.globalHue*pi/180)),
                40*strength*(sat[i]*std::sin(hue[i]*pi/180) + g.globalSat*std::sin(g.globalHue*pi/180))};
        }
        shadowEnd = 25+.15*g.balance;
        highlightStart = 75-.15*g.balance;
    }
    std::array<double, 3> at(double lightness) const
    {
        const double sh = 1-smoothstep(0,shadowEnd,lightness);
        const double hi = smoothstep(highlightStart,100,lightness);
        const double mid = std::max(0.0,1-sh-hi);
        std::array<double, 3> result{};
        for (int i = 0; i < 3; ++i) result[i] = sh*shifts[0][i]+mid*shifts[1][i]+hi*shifts[2][i];
        return result;
    }
};
std::array<double, 3> linearRGB(double l, double a, double b)
{
    const double fy = (l+16)/116;
    const auto inv = [](double f) { return f > 6.0/29 ? f*f*f : (f-16.0/116)/7.787037037; };
    const double x = .96422*inv(fy+a/500), y = inv(fy), z = .82521*inv(fy-b/200);
    return {3.1338561*x-1.6168667*y-.4906146*z,
            -.9787684*x+1.9161415*y+.0334540*z,
            .0719453*x-.2289914*y+1.4052427*z};
}
std::array<double, 3> displayRGB(double l, double a, double b)
{
    auto rgb = linearRGB(l,a,b);
    for (auto& c : rgb) c = clamp(c <= .0031308 ? 12.92*c : 1.055*std::pow(c,1/2.4)-.055);
    return rgb;
}
std::array<double, 3> lab(double r, double g, double b)
{
    // The analysis renderer returns encoded sRGB at 0..65535. Adapted D50
    // primaries match the Lab hue convention used by the grading wheels.
    const auto linear = [](double c) { c = clamp(c); return c <= .04045 ? c / 12.92 : std::pow((c + .055) / 1.055, 2.4); };
    r = linear(r); g = linear(g); b = linear(b);
    const auto f = [](double v) { return v > .008856451679 ? std::cbrt(v) : 7.787037037 * v + 16.0 / 116.0; };
    const double x = f((.4360747*r + .3850649*g + .1430804*b) / .96422);
    const double y = f(.2225045*r + .7168786*g + .0606169*b);
    const double z = f((.0139322*r + .0971045*g + .7141733*b) / .82521);
    return {116*y - 16, 500*(x-y), 200*(y-z)};
}
double distance(const Grade& a, const Grade& b)
{
    const TonalModel first(a), second(b);
    double result = 0;
    for (double light : {10.0,50.0,90.0}) {
        const auto x = first.at(light), y = second.at(light);
        for (int i = 0; i < 3; ++i) result += (x[i]-y[i])*(x[i]-y[i]);
    }
    return std::sqrt(result/3);
}
double score(const Grade& g, const Features& f)
{
    const TonalModel model(g);
    double impact = 0, risk = 0, harmony = 0;
    const auto overshoot = [](const std::array<double,3>& rgb) {
        double sum = 0;
        for (double c : rgb) sum += std::max(0.0,c-1.02) + std::max(0.0,-c-.01);
        return sum;
    };
    for (const auto& sample : f.samples) {
        const double l = sample[0], a = sample[1], b = sample[2];
        const auto delta = model.at(l);
        const double c = std::hypot(a,b), amount = std::hypot(delta[1],delta[2]);
        const double nextL = l+delta[0], nextA = a+delta[1], nextB = b+delta[2];
        const double nextC = std::hypot(nextA,nextB);
        impact += amount;
        if (c > 8) harmony += (a*delta[1]+b*delta[2])/(c*std::max(3.0,amount));
        const double hueTurn = std::abs(a*delta[2]-b*delta[1])/std::max(c,8.0);
        // Skin-like colors are uncertain evidence. Penalize large cross-hue
        // shifts, not every hint of warmth or every nonzero midtone adjustment.
        const double skin = smoothstep(25,40,l)*(1-smoothstep(82,95,l))
            * smoothstep(2,9,a)*(1-smoothstep(27,38,a))
            * smoothstep(5,13,b)*(1-smoothstep(32,45,b));
        risk += skin*(.07*std::pow(std::max(0.0,hueTurn-2.5),2)
                    + .09*std::pow(std::max(0.0,3-nextA),2));
        const double white = smoothstep(82,97,l)*(1-smoothstep(5,14,c));
        risk += white*.06*std::pow(std::max(0.0,nextC-7),2);
        risk += .018*std::pow(std::max(0.0,nextC-std::max(65.0,c+8)),2);
        risk += 8*std::max(0.0, overshoot(linearRGB(nextL,nextA,nextB))-overshoot(linearRGB(l,a,b)));
        risk += .025*delta[0]*delta[0] + .2*(std::max(0.0,-nextL)+std::max(0.0,nextL-100));
    }
    const double n = std::max<size_t>(1,f.samples.size());
    impact /= n; risk /= n; harmony /= n;
    const double desiredImpact = 9-2*f.chroma;
    const double character = 4*(1-std::exp(-impact/4))
        -.10*std::pow(std::max(0.0,impact-desiredImpact),2);
    const auto shadow = model.at(10), highlight = model.at(90);
    const double separation = std::hypot(shadow[1]-highlight[1],shadow[2]-highlight[2]);
    const double usableRange = clamp((f.p90-f.p10)/.55);
    // Favor split palettes on images with an actual tonal range. Existing
    // green/blue regions inform the direction without forcing a named recipe.
    const double shadowHue = f.foliage > .3 ? 160 : f.cool > .3 ? 265 : 240;
    const double highlightHue = f.cool > .45 && f.warm < .1 ? 300 : 65;
    const auto affinity = [](const std::array<double,3>& d, double hue) {
        const double amount = std::hypot(d[1],d[2]);
        return (d[1]*std::cos(hue*pi/180)+d[2]*std::sin(hue*pi/180))/std::max(4.0,amount);
    };
    const double palette = .30*usableRange*(affinity(shadow,shadowHue)+affinity(highlight,highlightHue))
        + .6*f.foliage*affinity(model.at(50),130);
    return character + .22*harmony + .4*usableRange*std::min(1.0,separation/12) + palette - risk;
}
} // namespace

bool valid(const Grade& grade)
{
    for (const auto& f : fields) {
        const double v = grade.*(f.value);
        if (!std::isfinite(v) || v < f.low || v > f.high) return false;
    }
    return true;
}

std::vector<Preset> bundled()
{
    std::vector<Preset> out;
    const auto add = [&](const char* id, const char* name, double sh, double ss, double mh, double ms, double hh, double hs, double blend = 78) {
        Grade g;
        g.enabled = true;
        g.shadowsHue = sh; g.shadowsSat = ss;
        g.midtonesHue = mh; g.midtonesSat = ms;
        g.highlightsHue = hh; g.highlightsSat = hs;
        g.blending = blend;
        out.push_back({id, name, g, false});
    };
    // The native midtone plateau receives full weight, but blend still scales
    // its chroma. Expressive recipes target roughly 7-13 Lab units after blend.
    add("warm-paper", "TP_GRADING_WARM_PAPER", 265, .18, 75, .13, 78, .30);
    add("cool-daylight", "TP_GRADING_COOL_DAYLIGHT", 250, .25, 265, .22, 265, .25);
    add("amber-slate", "TP_GRADING_AMBER_SLATE", 245, .42, 70, .27, 72, .44);
    add("rose-olive", "TP_GRADING_ROSE_OLIVE", 135, .32, 25, .23, 25, .34);
    add("soft-portrait", "TP_GRADING_SOFT_PORTRAIT", 250, .12, 60, .045, 65, .18, 75);
    add("copper-dusk", "TP_GRADING_COPPER_DUSK", 270, .40, 60, .30, 55, .43);
    add("night-cyan", "TP_GRADING_NIGHT_CYAN", 220, .38, 245, .29, 80, .24);
    add("quiet-plum", "TP_GRADING_QUIET_PLUM", 320, .34, 50, .16, 85, .30);
    add("teal-amber", "TP_GRADING_TEAL_AMBER", 215, .50, 65, .34, 70, .50, 80);
    add("blue-gold", "TP_GRADING_BLUE_GOLD", 285, .43, 85, .31, 90, .48);
    add("indigo-rose", "TP_GRADING_INDIGO_ROSE", 295, .42, 345, .29, 30, .40);
    add("lavender-cream", "TP_GRADING_LAVENDER_CREAM", 305, .33, 315, .22, 95, .34);
    add("sage-copper", "TP_GRADING_SAGE_COPPER", 150, .36, 72, .23, 60, .38);
    add("emerald-peach", "TP_GRADING_EMERALD_PEACH", 170, .40, 52, .24, 48, .40);
    add("forest-brass", "TP_GRADING_FOREST_BRASS", 145, .40, 115, .28, 94, .43);
    add("coastal-peach", "TP_GRADING_COASTAL_PEACH", 225, .37, 230, .25, 48, .42);
    add("rosewater", "TP_GRADING_ROSEWATER", 350, .34, 22, .30, 38, .32);
    add("apricot-light", "TP_GRADING_APRICOT_LIGHT", 300, .21, 62, .30, 74, .42);
    add("golden-amber", "TP_GRADING_GOLDEN_AMBER", 62, .27, 79, .33, 91, .45);
    add("silver-blue", "TP_GRADING_SILVER_BLUE", 260, .31, 250, .25, 270, .30);
    add("glacier", "TP_GRADING_GLACIER", 220, .40, 235, .34, 235, .38);
    add("nocturne", "TP_GRADING_NOCTURNE", 282, .48, 290, .36, 335, .35, 80);
    add("neon-orchid", "TP_GRADING_NEON_ORCHID", 210, .50, 330, .40, 350, .48, 80);
    add("petrol-rust", "TP_GRADING_PETROL_RUST", 205, .46, 45, .28, 38, .46);
    return out;
}

std::vector<Preset> load(const Glib::ustring& directory, unsigned& rejected)
{
    std::vector<Preset> result;
    rejected = 0;
    if (!Glib::file_test(directory, Glib::FILE_TEST_IS_DIR)) return result;
    Glib::Dir dir(directory);
    for (const auto& name : dir) {
        if (name.size() < 6 || name.substr(name.size()-6) != ".grade") continue;
        try {
            const auto path = Glib::build_filename(directory, name);
            GStatBuf stat;
            if (g_stat(path.c_str(), &stat) != 0 || stat.st_size > 16384) throw std::runtime_error("Invalid grading preset size");
            Glib::KeyFile key;
            key.load_from_file(path);
            if (key.get_integer("Grading Preset", "Version") != 1) throw std::runtime_error("Unsupported grading preset");
            Preset preset;
            preset.id = key.get_string("Grading Preset", "Id");
            preset.name = key.get_string("Grading Preset", "Name");
            if (!validId(preset.id) || name != preset.id + ".grade" || preset.name.empty() || preset.name.size() > 80) throw std::runtime_error("Invalid grading preset identity");
            preset.personal = true;
            preset.grade.enabled = key.get_boolean("Color Grading", "Enabled");
            for (const auto& f : fields) preset.grade.*(f.value) = key.get_double("Color Grading", f.name);
            if (!valid(preset.grade) || !preset.grade.enabled) throw std::runtime_error("Invalid grading preset values");
            result.push_back(std::move(preset));
        } catch (...) { ++rejected; }
    }
    std::sort(result.begin(), result.end(), [](const Preset& a, const Preset& b) { return a.id < b.id; });
    return result;
}

void save(const Glib::ustring& directory, Preset& preset)
{
    if (!preset.personal || preset.name.empty() || preset.name.size() > 80 || !preset.grade.enabled || !valid(preset.grade)) throw std::runtime_error("Invalid grading preset");
    if (preset.id.empty()) {
        gchar* uuid = g_uuid_string_random();
        preset.id = uuid;
        g_free(uuid);
    }
    if (!validId(preset.id)) throw std::runtime_error("Invalid grading preset identity");
    if (g_mkdir_with_parents(directory.c_str(), 0700) != 0) throw std::runtime_error("Cannot create grading preset directory");
    Glib::KeyFile key;
    key.set_integer("Grading Preset", "Version", 1);
    key.set_string("Grading Preset", "Id", preset.id);
    key.set_string("Grading Preset", "Name", preset.name);
    key.set_boolean("Color Grading", "Enabled", preset.grade.enabled);
    for (const auto& f : fields) key.set_double("Color Grading", f.name, preset.grade.*(f.value));
    const auto data = key.to_data();
    GFile* file = g_file_new_for_path(Glib::build_filename(directory, preset.id + ".grade").c_str());
    GError* error = nullptr;
    const bool ok = g_file_replace_contents(file, data.c_str(), data.bytes(), nullptr, false,
        G_FILE_CREATE_PRIVATE, nullptr, nullptr, &error);
    g_object_unref(file);
    if (!ok) {
        const std::string message = error ? error->message : "Cannot save grading preset";
        g_clear_error(&error);
        throw std::runtime_error(message);
    }
}

void erase(const Glib::ustring& directory, const Preset& preset)
{
    if (!preset.personal || !validId(preset.id)) throw std::runtime_error("Cannot delete a bundled grade");
    if (g_remove(Glib::build_filename(directory, preset.id + ".grade").c_str()) != 0) throw std::runtime_error("Cannot delete grading preset");
}

Features analyze(const rtengine::Imagefloat& image)
{
    Features f;
    std::vector<double> light;
    const int sx = std::max(1, (image.getWidth()+31)/32), sy = std::max(1, (image.getHeight()+31)/32);
    f.samples.reserve(1024);
    for (int y = sy/2; y < image.getHeight(); y += sy) {
        for (int x = sx/2; x < image.getWidth(); x += sx) {
            const double r = image.r(y,x)/65535.0, g = image.g(y,x)/65535.0, b = image.b(y,x)/65535.0;
            if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b)) continue;
            const auto v = lab(r,g,b);
            f.samples.push_back(v);
            const double l = v[0]/100, c = std::hypot(v[1],v[2]);
            light.push_back(l);
            const int zone = l < .30 ? 0 : l > .75 ? 2 : 1;
            f.weight[zone] += 1;
            // Bound extreme lights so a few saturated pixels cannot dominate.
            const double bound = c > 40 ? 40/c : 1;
            f.a[zone] += v[1]*bound; f.b[zone] += v[2]*bound;
            f.shadows += l < .20; f.highlights += l > .90;
            f.neutralHighlights += l > .80 && c < 8;
            f.warm += v[2] > 8 && v[1] > -5;
            f.cool += v[2] < -8;
            f.foliage += v[1] < -8 && v[2] > 8;
            f.skin += l > .30 && l < .85 && v[1] > 5 && v[1] < 28 && v[2] > 8 && v[2] < 35;
            f.chroma += std::min(c, 80.0)/80;
        }
    }
    if (light.size() < 64) return f;
    const double n = light.size();
    for (int i = 0; i < 3; ++i) {
        if (f.weight[i]) { f.a[i] /= f.weight[i]; f.b[i] /= f.weight[i]; }
        f.weight[i] /= n;
    }
    f.shadows /= n; f.highlights /= n; f.neutralHighlights /= n;
    f.warm /= n; f.cool /= n; f.foliage /= n; f.skin /= n; f.chroma /= n;
    std::sort(light.begin(), light.end());
    f.p10 = light[light.size()/10]; f.median = light[light.size()/2]; f.p90 = light[light.size()*9/10];
    f.valid = f.p90 > .03 && f.chroma > .025;
    return f;
}

std::vector<Preset> rank(std::vector<Preset> presets, const Features& f)
{
    if (!f.valid || f.samples.empty()) return presets;
    struct Candidate { Preset preset; double score; };
    std::vector<Candidate> candidates;
    for (auto& preset : presets) {
        const double s = score(preset.grade,f);
        candidates.push_back({std::move(preset),s});
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return a.score == b.score ? a.preset.id < b.preset.id : a.score > b.score;
    });
    // Evaluate each candidate once. Keep the leading choices visibly distinct,
    // including when several personal presets duplicate the same palette.
    for (size_t i = 1; i < std::min<size_t>(4, candidates.size()); ++i) {
        auto best = candidates.begin()+i;
        double bestScore = -1e30;
        for (auto it = best; it != candidates.end(); ++it) {
            double s = it->score;
            for (size_t j = 0; j < i; ++j) s -= .85*std::exp(-distance(it->preset.grade,candidates[j].preset.grade)/3);
            if (s > bestScore) { best = it; bestScore = s; }
        }
        std::rotate(candidates.begin()+i, best, best+1);
    }
    presets.clear();
    for (auto& candidate : candidates) presets.push_back(std::move(candidate.preset));
    return presets;
}

std::array<double, 3> swatch(double hue, double saturation)
{
    return displayRGB(68,40*saturation*std::cos(hue*pi/180),40*saturation*std::sin(hue*pi/180));
}

std::array<double, 3> tonalShift(const Grade& grade, double lightness)
{
    return TonalModel(grade).at(lightness);
}

std::array<double, 3> swatch(const Grade& grade, int zone)
{
    const double sampleLight[] = {8,50,95}, displayLight[] = {48,66,80};
    zone = std::max(0,std::min(2,zone));
    const auto shift = tonalShift(grade,sampleLight[zone]);
    // Small palette chips need extra chroma to stay legible. This display-only
    // gain never changes the exact grade used by hover, apply or saved files.
    return displayRGB(displayLight[zone],2*shift[1],2*shift[2]);
}
} // namespace gradingpresets
