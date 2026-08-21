/*
 *  This file is part of RawTherapee.
 *
 *  Interactive-edit latency instrumentation. See edittrace.h.
 */

#include "edittrace.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

namespace
{

// Power of two so the modulo is a mask. 256 in-flight edits is far more than
// the engine can ever have outstanding (it coalesces), but the ring costs
// nothing and removes any chance of a lost stamp during a fast drag.
constexpr unsigned long long kRingSize = 256;

struct SubmitSlot {
    std::atomic<unsigned long long> serial{0};
    std::atomic<long long> submitUs{-1};
};

SubmitSlot g_ring[kRingSize];
std::atomic<unsigned long long> g_nextSerial{1};
std::atomic<unsigned long long> g_publishedSerial{0};
std::atomic<unsigned long long> g_publishedNewestSerial{0};

bool envFlag(const char* name)
{
    const char* value = std::getenv(name);

    return value != nullptr
        && value[0] != '\0'
        && std::strcmp(value, "0") != 0
        && std::strcmp(value, "false") != 0
        && std::strcmp(value, "FALSE") != 0;
}

std::chrono::steady_clock::time_point traceEpoch()
{
    static const std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();
    return epoch;
}

}

namespace rtengine
{

namespace edittrace
{

bool enabled()
{
    static const bool value = []() {
        const bool on = envFlag("STEEP_EDIT_TRACE");

        if (on) {
            // Big buffer, flushed at exit: keeps the tracer off the critical
            // path of the very frames it is timing.
            static char buffer[1 << 20];
            std::setvbuf(stdout, buffer, _IOFBF, sizeof(buffer));
            std::atexit([]() { std::fflush(stdout); });
        }

        return on;
    }();

    return value;
}

bool verbose()
{
    static const bool value = envFlag("STEEP_EDIT_TRACE_VERBOSE");
    return value;
}

long long nowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now() - traceEpoch())
        .count();
}

void logf(const char* fmt, ...)
{
    if (!enabled()) {
        return;
    }

    // One line per call: build it, then a single fputs, so concurrent engine
    // and GUI threads cannot interleave halves of a line.
    char body[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);

    char line[1152];
    std::snprintf(line, sizeof(line), "[editTrace t=%.1fms] %s\n", nowUs() / 1000.0, body);

    // Deliberately unflushed. An fflush per line is a write syscall per line,
    // and at ~20 lines per rendered frame that perturbs exactly the timings
    // being measured (it cost several ms per frame and made the tracer look
    // like a bottleneck in the GUI thread). Output is flushed at exit, or when
    // the buffer fills.
    std::fputs(line, stdout);
}

unsigned long long noteSubmit()
{
    if (!enabled() && dumpDir() == nullptr) {
        return 0;
    }

    const unsigned long long serial = g_nextSerial.fetch_add(1, std::memory_order_relaxed);
    SubmitSlot& slot = g_ring[serial % kRingSize];
    // Timestamp first: a reader that sees the serial must find a usable time.
    slot.submitUs.store(nowUs(), std::memory_order_relaxed);
    slot.serial.store(serial, std::memory_order_release);

    return serial;
}

long long submitUs(unsigned long long serial)
{
    if (serial == 0) {
        return -1;
    }

    const SubmitSlot& slot = g_ring[serial % kRingSize];

    if (slot.serial.load(std::memory_order_acquire) != serial) {
        return -1;  // evicted by a later edit
    }

    return slot.submitUs.load(std::memory_order_relaxed);
}

double ageMs(unsigned long long serial)
{
    const long long submitted = submitUs(serial);

    if (submitted < 0) {
        return -1.0;
    }

    return (nowUs() - submitted) / 1000.0;
}

void notePublish(unsigned long long serial, unsigned long long newestSerial)
{
    g_publishedNewestSerial.store(newestSerial, std::memory_order_release);
    g_publishedSerial.store(serial, std::memory_order_release);
}

unsigned long long lastPublishedSerial()
{
    return g_publishedSerial.load(std::memory_order_acquire);
}

unsigned long long lastPublishedNewestSerial()
{
    return g_publishedNewestSerial.load(std::memory_order_acquire);
}

const char* dumpDir()
{
    static const char* const dir = []() -> const char* {
        const char* value = std::getenv("STEEP_DUMP_CROP");
        return (value != nullptr && value[0] != '\0') ? value : nullptr;
    }();

    return dir;
}

void dumpFrame(const char* tag, const unsigned char* rgb, int width, int height, int rowStride)
{
    const char* dir = dumpDir();

    if (dir == nullptr || rgb == nullptr || width <= 0 || height <= 0) {
        return;
    }

    // Per-tag sequence, guarded because preview and crop publish from
    // different points and (with a future multi-worker executor) could race.
    static std::mutex mutex;
    static std::map<std::string, int> sequences;

    int sequence = 0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        sequence = sequences[tag]++;
    }

    char path[1024];
    std::snprintf(path, sizeof(path), "%s/%s-%04d.ppm", dir, tag, sequence);

    std::FILE* file = std::fopen(path, "wb");

    if (file == nullptr) {
        std::fprintf(stderr, "editTrace: cannot open '%s' for the frame dump\n", path);
        return;
    }

    std::fprintf(file, "P6\n%d %d\n255\n", width, height);

    for (int y = 0; y < height; ++y) {
        std::fwrite(rgb + static_cast<std::size_t>(y) * rowStride, 1, static_cast<std::size_t>(width) * 3, file);
    }

    std::fclose(file);
}

}

}
