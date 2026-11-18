// Checks rtengine/stuckpixelmemory: what counts as a session, when a
// photosite becomes known, when a camera is mature, and that it all survives
// a save and reload. CMake target steep-stuck-pixel-memory-tests.
#include "../rtengine/stuckpixelmemory.h"

#include <cstdio>
#include <string>
#include <vector>

#include <glib.h>
#include <glib/gstdio.h>

using rtengine::StuckPixelCandidate;
using rtengine::StuckPixelMemory;

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    failures += !ok;
}

bool knows(const StuckPixelMemory::Verdict& v, int row, int col)
{
    for (const auto& p : v.known) {
        if (p.first == row && p.second == col) {
            return true;
        }
    }
    return false;
}

constexpr std::int64_t kDay = 24 * 3600;
constexpr std::int64_t kT0 = 1700000000;

} // namespace

int main()
{
    StuckPixelMemory& m = StuckPixelMemory::instance();
    m.setDirectory(""); // in memory only

    const std::string cam = "FUJIFILM X-T4 12345 6264x4180";
    auto v = m.update(cam, kT0, 400, {{10, 10, 50.f}, {20, 20, 40.f}});
    check(v.known.empty() && v.sessions == 1 && !v.mature, "first sighting: nothing known yet");

    v = m.update(cam, kT0 + 3600, 400, {{10, 10, 50.f}});
    check(v.known.empty() && v.sessions == 1, "an hour later is the same session (a burst counts once)");

    v = m.update(cam, kT0 + kDay, 400, {{10, 10, 50.f}});
    check(knows(v, 10, 10) && !knows(v, 20, 20) && v.sessions == 2, "seen again a day later: known; the one-off is not");

    v = m.update(cam, kT0 - 2 * kDay, 400, {});
    check(v.sessions == 3 && v.mature, "three sessions (in any order): the camera is mature");

    const size_t before = v.known.size();
    v = m.update(cam, kT0 + kDay, 400, {{10, 10, 50.f}}); // exactly the day-two frame again
    check(v.known.size() == before && v.sessions == 3, "reprocessing a frame adds nothing");

    v = m.update(cam, 0, 400, {{30, 30, 99.f}});
    check(v.sessions == 3 && !knows(v, 30, 30), "no capture time: nothing learned");

    // Noisy frames don't teach: high gain, or more candidates than a clean
    // frame has. Warm photosites spike at the same coordinates in every
    // ISO 6400 frame, so two such sessions would otherwise "know" them.
    {
        const std::string noisy = "Noisy Cam 1 100x100";
        std::vector<StuckPixelCandidate> many;
        for (int i = 0; i < 1000; ++i) {
            many.push_back({i, 0, float(i)});
        }
        m.update(noisy, kT0, 400, many);
        v = m.update(noisy, kT0 + kDay, 400, many);
        check(v.known.empty() && v.sessions == 0 && !v.learned, "a frame full of candidates teaches nothing");

        m.update(noisy, kT0 + 2 * kDay, 6400, {{3, 3, 90.f}});
        v = m.update(noisy, kT0 + 3 * kDay, 12800, {{3, 3, 90.f}});
        check(v.known.empty() && v.sessions == 0, "high-ISO sightings in two sessions are not a defect");

        m.update(noisy, kT0 + 4 * kDay, StuckPixelMemory::kLearnMaxISO, {{3, 3, 90.f}});
        v = m.update(noisy, kT0 + 5 * kDay, 160, {{3, 3, 90.f}});
        check(knows(v, 3, 3) && v.learned && v.sessions == 2, "the same photosite in two clean sessions is");

        v = m.update(noisy, kT0 + 6 * kDay, 25600, {{50, 50, 90.f}});
        check(knows(v, 3, 3) && !v.learned, "a noisy frame still gets the known fixes");

        std::vector<StuckPixelCandidate> limit(StuckPixelMemory::kLearnMaxCandidates, StuckPixelCandidate{0, 0, 1.f});
        for (size_t i = 0; i < limit.size(); ++i) {
            limit[i] = {60, int(i), 20.f};
        }
        v = m.update(noisy, kT0 + 7 * kDay, 0, limit);
        check(v.learned, "kLearnMaxCandidates candidates and an unknown ISO still teach");
    }

    // Flooding with one-off noise never evicts a known photosite.
    {
        const std::string flood = "Flood Cam 1 5000x5000";
        m.update(flood, kT0, 400, {{7, 7, 80.f}});
        m.update(flood, kT0 + kDay, 400, {{7, 7, 80.f}});
        for (int frame = 0; frame < 100; ++frame) {
            std::vector<StuckPixelCandidate> junk;
            for (int i = 0; i < 300; ++i) {
                junk.push_back({1000 + frame, i, 10.f});
            }
            m.update(flood, kT0 + (2 + frame) * kDay, 400, junk);
        }
        v = m.query(flood);
        check(knows(v, 7, 7), "30000 one-off sightings later the known photosite is still known");
    }

    check(m.query("Other Cam 2 6264x4180").known.empty(), "cameras don't share what they learn");

    // Persistence.
    {
        gchar* dir = g_dir_make_tmp("steep-stuckpix-XXXXXX", nullptr);
        m.setDirectory(dir);
        const std::string saved = "FUJIFILM X-T4 99999 6264x4180";
        m.update(saved, kT0, 400, {{5, 6, 30.f}, {8, 9, 30.f}});
        m.update(saved, kT0 + kDay, 400, {{5, 6, 30.f}});
        m.update(saved, kT0 + 2 * kDay, 400, {});
        m.clearCache();
        v = m.query(saved);
        check(knows(v, 5, 6) && !knows(v, 8, 9) && v.sessions == 3 && v.mature, "saved and read back intact");
        v = m.update(saved, kT0 + 3 * kDay, 400, {{8, 9, 30.f}});
        check(knows(v, 8, 9), "a reloaded one-off sighting still counts towards a second session");

        // A format-1 file (learned from every frame) is set aside, not read.
        const std::string old = "FUJIFILM X-T4 77777 6264x4180";
        m.update(old, kT0, 400, {{1, 1, 30.f}}); // writes the file; then turn it into a v1 one
        m.clearCache();
        {
            GDir* scan = g_dir_open(dir, 0, nullptr);
            while (const gchar* name = scan ? g_dir_read_name(scan) : nullptr) {
                if (std::string(name).find("77777") == std::string::npos && std::string(name).find("X-T4_7") == std::string::npos) {
                    continue;
                }
                gchar* path = g_build_filename(dir, name, nullptr);
                FILE* f = g_fopen(path, "w");
                std::fprintf(f, "steep-stuckpixels 1\ncamera %s\ntouch 3\nsession %lld\nsession %lld\npx 1 1 2 %lld %lld 1\n", old.c_str(),
                             (long long)kT0, (long long)(kT0 + kDay), (long long)kT0, (long long)(kT0 + kDay));
                std::fclose(f);
                g_free(path);
            }
            if (scan) {
                g_dir_close(scan);
            }
        }
        v = m.query(old);
        check(v.known.empty() && v.sessions == 0 && !v.mature, "a format-1 file is not trusted: the camera starts over");
        v = m.update(old, kT0 + 2 * kDay, 400, {{1, 1, 30.f}});
        v = m.update(old, kT0 + 3 * kDay, 400, {{1, 1, 30.f}});
        check(knows(v, 1, 1), "... and relearns its real defects from clean frames");

        GDir* d = g_dir_open(dir, 0, nullptr);
        int files = 0;
        while (const gchar* name = d ? g_dir_read_name(d) : nullptr) {
            gchar* path = g_build_filename(dir, name, nullptr);
            g_remove(path);
            g_free(path);
            ++files;
        }
        if (d) {
            g_dir_close(d);
        }
        g_rmdir(dir);
        g_free(dir);
        check(files == 3, "one file per camera plus the set-aside v1 file, no stray temporaries");
    }

    std::printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures ? 1 : 0;
}
