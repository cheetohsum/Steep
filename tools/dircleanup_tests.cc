// Directory cleanup: parser checks, plus a dry run over real folders.
//   steep-dircleanup-tests                       -> unit checks
//   steep-dircleanup-tests <cacheDataDir> <root>... -> print suggestions, move nothing
#include "rtgui/dircleanupcore.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <glibmm.h>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace dircleanup;

static int failures = 0;

static void check(bool ok, const char* what)
{
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

static std::string ymd(int day)
{
    int y, m, d;
    dayToYMD(day, y, m, d);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, d);
    return buf;
}

static void expectSpan(const char* name, FolderKind kind, const char* first, const char* last)
{
    const FolderInfo fi = parseFolderName(name);
    char what[256];
    std::snprintf(what, sizeof(what), "%s -> kind %d (want %d)", name, int(fi.kind), int(kind));
    check(fi.kind == kind, what);
    if (first) {
        std::snprintf(what, sizeof(what), "%s -> %s..%s (got %s..%s)", name, first, last,
                      fi.spans.empty() ? "-" : ymd(fi.spans.front().first).c_str(),
                      fi.spans.empty() ? "-" : ymd(fi.spans.back().last).c_str());
        check(!fi.spans.empty() && ymd(fi.spans.front().first) == first && ymd(fi.spans.back().last) == last, what);
    }
}

static void unitChecks()
{
    expectSpan("July 2024", FolderKind::Month, "2024-07-01", "2024-07-31");
    expectSpan("October2024", FolderKind::Month, "2024-10-01", "2024-10-31");
    expectSpan("AugustSeptember2024", FolderKind::Month, "2024-08-01", "2024-09-30");
    expectSpan("JanuaryandFebruary2025", FolderKind::Month, "2025-01-01", "2025-02-28");
    expectSpan("DecemberJanuary2025", FolderKind::Month, "2024-12-01", "2025-01-31");
    expectSpan("7_24_23", FolderKind::Month, "2023-07-24", "2023-07-24");
    expectSpan("2024-07", FolderKind::Month, "2024-07-01", "2024-07-31");
    expectSpan("2024-07-12", FolderKind::Month, "2024-07-12", "2024-07-12");
    expectSpan("Sept 2025", FolderKind::Month, "2025-09-01", "2025-09-30");
    expectSpan("August 2023 - Taliesin", FolderKind::Event, "2023-08-01", "2023-08-31");
    expectSpan("Kimo", FolderKind::Purpose, nullptr, nullptr);
    expectSpan("toprint", FolderKind::Purpose, nullptr, nullptr);
    expectSpan("_Favorites", FolderKind::Purpose, nullptr, nullptr);
    expectSpan("Lightroom", FolderKind::Purpose, nullptr, nullptr);
    expectSpan("OneDrive2025", FolderKind::Purpose, nullptr, nullptr);
    expectSpan("Photos", FolderKind::Purpose, nullptr, nullptr);
    expectSpan("Mayhem", FolderKind::Purpose, nullptr, nullptr);
    expectSpan("Marching band", FolderKind::Purpose, nullptr, nullptr);

    int y, m, d, hh, mm;
    check(dateFromFileName("IMG_20240712_101500.jpg", y, m, d, hh, mm) && y == 2024 && m == 7 && d == 12 && hh == 10, "IMG_ date");
    check(dateFromFileName("2024-07-12 18.03.22.jpg", y, m, d, hh, mm) && y == 2024 && m == 7 && d == 12, "dashed date");
    check(dateFromFileName("PXL_20231001_093000123.jpg", y, m, d, hh, mm) && y == 2023 && m == 10, "PXL date");
    check(!dateFromFileName("DSCF8024.JPG", y, m, d, hh, mm), "DSCF has no date");
    check(!dateFromFileName("DSCF20991.RAF", y, m, d, hh, mm), "short number is not a date");

    check(dayNumber(1970, 1, 1) == 0, "epoch");
    dayToYMD(dayNumber(2024, 2, 29), y, m, d);
    check(y == 2024 && m == 2 && d == 29, "leap day round trip");
}

#ifdef _WIN32
// The thumbnail cache's file identity (rtgui/cachemanager.cc getMD5).
static std::string cacheMd5(const std::string& path)
{
    gunichar2* w = g_utf8_to_utf16(path.c_str(), -1, nullptr, nullptr, nullptr);
    WIN32_FILE_ATTRIBUTE_DATA a;
    std::string out;
    if (w && GetFileAttributesExW(reinterpret_cast<wchar_t*>(w), GetFileExInfoStandard, &a)) {
        const auto id = Glib::ustring::compose("%1-%2-%3-%4", a.nFileSizeLow, a.ftCreationTime.dwHighDateTime,
                                               a.ftCreationTime.dwLowDateTime, Glib::ustring(path));
        out = Glib::Checksum::compute_checksum(Glib::Checksum::CHECKSUM_MD5, id);
    }
    g_free(w);
    return out;
}
#endif

int main(int argc, char** argv)
{
    Glib::init();
    unitChecks();
    if (argc < 3) {
        std::printf(failures ? "%d failure(s)\n" : "all checks passed\n", failures);
        return failures ? 1 : 0;
    }

#ifdef _WIN32
    const std::string dataDir = argv[1];
    std::vector<std::string> roots(argv + 2, argv + argc);
    const std::set<std::string> exts = {"raf", "jpg", "jpeg", "png", "tif", "tiff", "heic", "dng", "nef", "cr2", "cr3", "arw"};
    int hits = 0, misses = 0, exifHits = 0;
    const DateReader reader = [&](const std::string& p, int& y, int& m, int& d, int& hh, int& mm) {
        const std::string md5 = cacheMd5(p);
        const bool ok = !md5.empty()
            && readCacheDateTime(Glib::build_filename(dataDir, Glib::path_get_basename(p) + "." + md5 + ".txt"), y, m, d, hh, mm);
        (ok ? hits : misses)++;
        return ok || (exifDateFast(p, y, m, d, hh, mm) && ++exifHits);
    };

    const auto folders = listFolders(roots, 3);
    std::vector<FolderScan> scans;
    int groups = 0;
    for (const auto& f : folders) {
        scans.push_back(scanFolder(f.first, f.second, exts, reader));
        inferRunDates(scans.back());
        groups += static_cast<int>(scans.back().groups.size());
    }
    std::printf("%zu folders, %d photo groups, cache dates %d hit / %d miss\n", folders.size(), groups, hits, misses);
    for (const auto& s : scans) {
        const char* k = s.info.kind == FolderKind::Month ? "month" : s.info.kind == FolderKind::Event ? "event"
                      : s.info.kind == FolderKind::Root ? "root" : "purpose";
        int undated = 0;
        for (const auto& g : s.groups) undated += g.day < 0;
        std::printf("  [%-7s] %-48s %5zu groups %4d undated\n", k, s.path.c_str(), s.groups.size(), undated);
    }
    const auto sugg = suggest(scans);
    std::printf("\n%zu suggestion(s)\n", sugg.size());
    for (const auto& s : sugg) {
        std::printf("  %s%s  %s -> %s  %zu  %s..%s  %.2f %c%s%s\n",
                    s.dots() == 3 ? "***" : s.dots() == 2 ? "**-" : "*--",
                    s.createDest ? " +" : "", s.source.c_str(), s.dest.c_str(), s.groups.size(),
                    ymd(s.firstDay).c_str(), ymd(s.lastDay).c_str(), s.score, char(s.reason),
                    s.crossDrive ? " cross-drive" : "", s.alternative ? " alt" : "");
        int shown = 0;
        for (const auto& g : s.groups) {
            if (shown++ >= 4) break;
            std::printf("        %s (%zu files) %s\n", g.stem.c_str(), g.files.size(), ymd(g.day).c_str());
        }
    }
#endif
    return failures ? 1 : 0;
}
