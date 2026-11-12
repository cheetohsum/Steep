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
#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>

namespace rtengine
{

/**
 * Where the memory went, and a way to give some back.
 *
 * The big owners (decoded sources, pipeline buffers, caches) keep cheap live
 * counters here. A watcher thread, started by the GUI only, samples the
 * process every few seconds, writes a line to steep-memory.log whenever the
 * picture changed, and asks the registered trimmers to drop what is cheap to
 * rebuild when the process passes its budget or the system runs short of
 * commit. Without the watcher everything here is a handful of atomic adds.
 */
class MemGuard
{
public:
    enum Counter {
        ImageSources,   // decoded RawImageSource / StdImageSource
        RawImages,
        Coordinators,
        Crops,
        Imagefloats,
        LabImages,
        CounterCount
    };

    static void objectCreated(Counter c)
    {
        counts[c].fetch_add(1, std::memory_order_relaxed);
    }
    static void objectDestroyed(Counter c)
    {
        counts[c].fetch_sub(1, std::memory_order_relaxed);
    }
    // Callers remember what they added and subtract exactly that, so a
    // buffer that was swapped or resized in between cannot drive it negative.
    static void addBytes(Counter c, long long delta)
    {
        if (delta) {
            bytes[c].fetch_add(delta, std::memory_order_relaxed);
        }
    }
    static long long objectCount(Counter c)
    {
        return counts[c].load(std::memory_order_relaxed);
    }
    static long long objectBytes(Counter c)
    {
        return bytes[c].load(std::memory_order_relaxed);
    }

    // Named gauges for owners that already know their own size. Any thread.
    static void setGauge(const char* name, long long valueBytes);   // logged in MiB
    static void setCountGauge(const char* name, long long value);   // logged as is
    // For owners whose size changes in too many places to push: read is
    // called when a log line is written, from the watcher or a pipeline
    // thread, and takes the owner's own mutex. Capture a weak_ptr if the
    // owner can go away.
    static void registerProbe(const std::string& name, std::function<long long()> read, bool isBytes = true);

    static unsigned long long processPrivateBytes();   // 0 when unknown
    static unsigned long long commitHeadroomBytes();   // what the system can still commit; ~0ULL when unknown
    static unsigned long long physicalMemoryBytes();   // 0 when unknown
    // min(60% of physical RAM, 28 GiB), or STEEP_MEM_BUDGET_GB.
    static unsigned long long budgetBytes();

    // level 1: drop caches that are cheap to rebuild. level 2: everything not
    // needed to show the current photo. Runs on the watcher thread or on a
    // pipeline thread that just ran out of memory, so a trimmer takes its
    // owner's own mutex (try-lock anything a running pass may hold), never
    // calls GTK, and returns quickly.
    static void registerTrimmer(const std::string& name, std::function<void(int level)> trim);

    // Runs the trimmers on the calling thread and logs private bytes before
    // and after. Refused (returns false) within 10 s of the previous trim.
    static bool trim(int level, const char* reason);

    // One log line now, whatever changed (no-op before startWatcher()).
    static void logNow(const char* reason);

    // Starts the sampling thread; logFile is UTF-8. Call once, from the GUI.
    static void startWatcher(const std::string& logFile);

    // No trimming from here on: the owners are about to be torn down.
    static void shutdown();

private:
    static std::atomic<long long> counts[CounterCount];
    static std::atomic<long long> bytes[CounterCount];
};

}
