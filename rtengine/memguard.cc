/*
 *  This file is part of RawTherapee.
 *
 *  RawTherapee is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  RawTherapee is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with RawTherapee.  If not, see <https://www.gnu.org/licenses/>.
 */
#ifdef _WIN32
// K32GetProcessMemoryInfo lives in kernel32, so neither executable needs
// psapi on its link line.
#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <unistd.h>
#endif

#include "memguard.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <glib.h>
#include <glib/gstdio.h>

#include "aiidlereaper.h"

namespace rtengine
{

std::atomic<long long> MemGuard::counts[MemGuard::CounterCount];
std::atomic<long long> MemGuard::bytes[MemGuard::CounterCount];

namespace
{

constexpr unsigned long long kMiB = 1024ULL * 1024;
constexpr unsigned long long kGiB = 1024ULL * kMiB;
constexpr unsigned long long kUnknown = ~0ULL;

constexpr int kSampleSeconds = 5;
constexpr long long kMinTrimIntervalMs = 10000;
// A full trim that freed next to nothing will not do better ten seconds on;
// throwing the neighbour cache away every time only makes browsing slow.
constexpr long long kFutileTrimIntervalMs = 60000;
constexpr unsigned long long kLogStep = 512 * kMiB;
constexpr unsigned long long kLogHeadroom = 4 * kGiB;
constexpr unsigned long long kTrimHeadroom = 3 * kGiB;
constexpr long kMaxLogBytes = 1024 * 1024;

struct Gauge {
    std::string name;
    long long value;
    bool isBytes;
};

struct Trimmer {
    std::string name;
    std::function<void(int)> trim;
};

struct Probe {
    std::string name;
    std::function<long long()> read;
    bool isBytes;
};

struct State {
    std::mutex mutex;           // gauges, trimmers
    std::vector<Gauge> gauges;  // few, and logged in the order they first appeared
    std::vector<Trimmer> trimmers;
    std::vector<Probe> probes;

    std::mutex logMutex;
    std::string logFile;
    std::atomic<bool> watching{false};
    // Set at exit: the owners behind the trimmers are being destroyed.
    std::atomic<bool> exiting{false};

    std::mutex trimMutex;       // one trim at a time
    std::atomic<long long> lastTrimMs{-kMinTrimIntervalMs};
};

State& state()
{
    static State* const s = new State();   // never destroyed: gauges are set from static destructors too
    return *s;
}

long long nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void storeGauge(const char* name, long long value, bool isBytes)
{
    if (!name) {
        return;
    }
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    for (auto& gauge : s.gauges) {
        if (gauge.name == name) {
            gauge.value = value;
            return;
        }
    }
    s.gauges.push_back(Gauge{name, value, isBytes});
}

#ifndef _WIN32
// A "Key:   123 kB" line of /proc/meminfo, in bytes.
unsigned long long procMeminfo(const char* key)
{
    unsigned long long result = kUnknown;
    if (FILE* const f = std::fopen("/proc/meminfo", "r")) {
        char line[256];
        const std::size_t keyLength = std::strlen(key);
        while (std::fgets(line, sizeof(line), f)) {
            if (!std::strncmp(line, key, keyLength) && line[keyLength] == ':') {
                result = std::strtoull(line + keyLength + 1, nullptr, 10) * 1024ULL;
                break;
            }
        }
        std::fclose(f);
    }
    return result;
}
#endif

void appendGiB(std::string& line, const char* label, unsigned long long value)
{
    char buffer[96];
    if (value == kUnknown) {
        std::snprintf(buffer, sizeof(buffer), " %s=?", label);
    } else {
        std::snprintf(buffer, sizeof(buffer), " %s=%.2fG", label, static_cast<double>(value) / kGiB);
    }
    line += buffer;
}

std::string describe(const char* reason, unsigned long long privateBytes, unsigned long long headroom)
{
    char buffer[160];
    const std::time_t now = std::time(nullptr);
    std::tm local {};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local);

    std::string line(buffer);
    appendGiB(line, "private", privateBytes);
    appendGiB(line, "headroom", headroom);

    static const char* const names[MemGuard::CounterCount] = {
        "sources", "rawimages", "coordinators", "crops", "imagefloat", "labimage"
    };
    for (int c = 0; c < MemGuard::CounterCount; ++c) {
        const long long count = MemGuard::objectCount(static_cast<MemGuard::Counter>(c));
        const long long size = MemGuard::objectBytes(static_cast<MemGuard::Counter>(c));
        if (c == MemGuard::Coordinators || c == MemGuard::Crops) {
            std::snprintf(buffer, sizeof(buffer), " %s=%lld", names[c], count);
        } else {
            std::snprintf(buffer, sizeof(buffer), " %s=%lld/%lldM", names[c], count, size / static_cast<long long>(kMiB));
        }
        line += buffer;
    }

    std::vector<Gauge> gauges;
    std::vector<Probe> probes;
    {
        State& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        gauges = s.gauges;
        probes = s.probes;
    }
    // Read outside the lock: a probe takes its owner's mutex.
    for (const auto& probe : probes) {
        long long value = 0;
        try {
            value = probe.read();
        } catch (...) {
        }
        gauges.push_back(Gauge{probe.name, value, probe.isBytes});
    }
    for (const auto& gauge : gauges) {
        if (gauge.isBytes) {
            std::snprintf(buffer, sizeof(buffer), " %s=%lldM", gauge.name.c_str(), gauge.value / static_cast<long long>(kMiB));
        } else {
            std::snprintf(buffer, sizeof(buffer), " %s=%lld", gauge.name.c_str(), gauge.value);
        }
        line += buffer;
    }

    if (reason && *reason) {
        line += " | ";
        line += reason;
    }
    line += '\n';
    return line;
}

void writeLine(const std::string& line)
{
    State& s = state();
    std::lock_guard<std::mutex> lock(s.logMutex);
    if (s.logFile.empty()) {
        return;
    }

    FILE* f = g_fopen(s.logFile.c_str(), "ab");
    if (!f) {
        return;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fclose(f);

    if (size > kMaxLogBytes) {
        // Keep the recent half: the lines that led up to now are the ones
        // anybody opens this file for.
        std::string tail;
        if ((f = g_fopen(s.logFile.c_str(), "rb"))) {
            const long keep = kMaxLogBytes / 2;
            std::fseek(f, size - keep, SEEK_SET);
            tail.resize(keep);
            tail.resize(std::fread(&tail[0], 1, keep, f));
            std::fclose(f);
            const std::string::size_type firstLine = tail.find('\n');
            tail.erase(0, firstLine == std::string::npos ? tail.size() : firstLine + 1);
        }
        if ((f = g_fopen(s.logFile.c_str(), "wb"))) {
            std::fwrite(tail.data(), 1, tail.size(), f);
            std::fclose(f);
        }
    }

    if ((f = g_fopen(s.logFile.c_str(), "ab"))) {
        std::fwrite(line.data(), 1, line.size(), f);
        std::fclose(f);
    }
}

void watch()
{
    State& s = state();
    const unsigned long long budget = MemGuard::budgetBytes();
    unsigned long long lastLogged = 0;
    bool loggedOnce = false;
    int nextLevel = 1;
    long long nextTrimMs = 0;

    {
        char reason[96];
        std::snprintf(reason, sizeof(reason), "watching, budget %.1fG", static_cast<double>(budget) / kGiB);
        MemGuard::logNow(reason);
    }

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(kSampleSeconds));
        if (s.exiting) {
            return;
        }

        const unsigned long long privateBytes = MemGuard::processPrivateBytes();
        const unsigned long long headroom = MemGuard::commitHeadroomBytes();
        const unsigned long long moved = privateBytes > lastLogged ? privateBytes - lastLogged : lastLogged - privateBytes;

        if (!loggedOnce || moved >= kLogStep || headroom < kLogHeadroom) {
            writeLine(describe(headroom < kLogHeadroom ? "low commit headroom" : "", privateBytes, headroom));
            lastLogged = privateBytes;
            loggedOnce = true;
        }

        const bool overBudget = budget && privateBytes > budget;
        // Short of commit with next to nothing of our own to give back is
        // somebody else's problem; emptying the caches would not help it.
        const bool lowHeadroom = headroom < kTrimHeadroom && privateBytes > 1536 * kMiB;
        if (!overBudget && !lowHeadroom) {
            nextLevel = 1;
            continue;
        }
        if (nowMs() < nextTrimMs) {
            continue;
        }

        const int level = nextLevel;
        if (MemGuard::trim(level, overBudget ? "over budget" : "low commit headroom")) {
            const unsigned long long after = MemGuard::processPrivateBytes();
            const bool futile = level >= 2 && privateBytes < after + 256 * kMiB;
            nextTrimMs = nowMs() + (futile ? kFutileTrimIntervalMs : kMinTrimIntervalMs);
            // Still over when this comes round again: everything goes.
            nextLevel = 2;
            lastLogged = after;
        }
    }
}

}

void MemGuard::setGauge(const char* name, long long valueBytes)
{
    storeGauge(name, valueBytes, true);
}

void MemGuard::setCountGauge(const char* name, long long value)
{
    storeGauge(name, value, false);
}

unsigned long long MemGuard::processPrivateBytes()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters;
    ZeroMemory(&counters, sizeof(counters));
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
        return counters.PrivateUsage;
    }
    return 0;
#else
    // Resident pages: the nearest thing statm has to what the process owns.
    unsigned long long total = 0;
    unsigned long long resident = 0;
    if (FILE* const f = std::fopen("/proc/self/statm", "r")) {
        if (std::fscanf(f, "%llu %llu", &total, &resident) != 2) {
            resident = 0;
        }
        std::fclose(f);
    }
    const long pageSize = sysconf(_SC_PAGESIZE);
    return pageSize > 0 ? resident * static_cast<unsigned long long>(pageSize) : 0;
#endif
}

unsigned long long MemGuard::commitHeadroomBytes()
{
#ifdef _WIN32
    MEMORYSTATUSEX status;
    ZeroMemory(&status, sizeof(status));
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        return status.ullAvailPageFile;
    }
    return kUnknown;
#else
    // Linux overcommits, so the commit limit says little; what can still be
    // had without swapping is the figure that matters there.
    return procMeminfo("MemAvailable");
#endif
}

unsigned long long MemGuard::physicalMemoryBytes()
{
#ifdef _WIN32
    MEMORYSTATUSEX status;
    ZeroMemory(&status, sizeof(status));
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        return status.ullTotalPhys;
    }
    return 0;
#elif defined(_SC_PHYS_PAGES)
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long pageSize = sysconf(_SC_PAGESIZE);
    return pages > 0 && pageSize > 0 ? static_cast<unsigned long long>(pages) * static_cast<unsigned long long>(pageSize) : 0;
#else
    return 0;
#endif
}

unsigned long long MemGuard::budgetBytes()
{
    if (const char* const env = std::getenv("STEEP_MEM_BUDGET_GB")) {
        const double gb = std::atof(env);
        if (gb > 0.0) {
            return static_cast<unsigned long long>(gb * static_cast<double>(kGiB));
        }
    }
    const unsigned long long physical = physicalMemoryBytes();
    const unsigned long long cap = 28 * kGiB;
    return physical ? std::min(physical / 10 * 6, cap) : cap;
}

void MemGuard::registerProbe(const std::string& name, std::function<long long()> read, bool isBytes)
{
    if (!read) {
        return;
    }
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.probes.push_back(Probe{name, std::move(read), isBytes});
}

void MemGuard::registerTrimmer(const std::string& name, std::function<void(int level)> trim)
{
    if (!trim) {
        return;
    }
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.trimmers.push_back(Trimmer{name, std::move(trim)});
}

bool MemGuard::trim(int level, const char* reason)
{
    State& s = state();
    std::unique_lock<std::mutex> trimLock(s.trimMutex, std::try_to_lock);
    if (!trimLock.owns_lock() || s.exiting || nowMs() - s.lastTrimMs.load() < kMinTrimIntervalMs) {
        return false;
    }
    s.lastTrimMs = nowMs();

    std::vector<Trimmer> trimmers;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        trimmers = s.trimmers;
    }

    const unsigned long long before = processPrivateBytes();

    // Models nobody is running right now reload on their next use.
    AIIdleReaper::releaseNow();

    for (const auto& trimmer : trimmers) {
        if (s.exiting) {
            break;
        }
        try {
            trimmer.trim(level);
        } catch (...) {
            // A trimmer that cannot free must not take the watcher with it.
        }
    }

    const unsigned long long after = processPrivateBytes();
    char text[192];
    std::snprintf(text, sizeof(text), "trim level %d (%s): private %.2fG -> %.2fG, %d trimmers",
                  level, reason ? reason : "", static_cast<double>(before) / kGiB,
                  static_cast<double>(after) / kGiB, static_cast<int>(trimmers.size()));
    std::fprintf(stderr, "MemGuard: %s\n", text);
    writeLine(describe(text, after, commitHeadroomBytes()));
    return true;
}

void MemGuard::logNow(const char* reason)
{
    if (state().watching) {
        writeLine(describe(reason, processPrivateBytes(), commitHeadroomBytes()));
    }
}

void MemGuard::shutdown()
{
    state().exiting = true;
}

void MemGuard::startWatcher(const std::string& logFile)
{
    State& s = state();
    if (logFile.empty() || s.watching.exchange(true)) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(s.logMutex);
        s.logFile = logFile;
        gchar* const dir = g_path_get_dirname(logFile.c_str());
        g_mkdir_with_parents(dir, 0755);
        g_free(dir);
    }

    std::atexit([]() { state().exiting = true; });

    // Detached and never joined, like the idle reaper: it may be mid-trim at
    // exit, when static destructors must not wait on it.
    std::thread(watch).detach();
}

}
