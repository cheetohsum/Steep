#pragma once
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace rtengine
{
// Opt-in timings without photo paths or pixel data in the log.
class EffectPerfScope
{
    const char* stage;
    int width, height;
    std::chrono::steady_clock::time_point start;
    static bool enabled()
    {
        static const bool value = std::getenv("STEEP_EFFECT_PERF") != nullptr;
        return value;
    }
public:
    explicit EffectPerfScope(const char* name, int w = 0, int h = 0)
        : stage(name), width(w), height(h), start(enabled() ? std::chrono::steady_clock::now() :
                                                             std::chrono::steady_clock::time_point{}) {}
    ~EffectPerfScope()
    {
        if (!enabled()) return;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        static std::mutex mutex;
        std::lock_guard<std::mutex> guard(mutex);
        std::fprintf(stderr, "[effect-perf] stage=%s ms=%.3f size=%dx%d\n", stage, ms, width, height);
    }
};
}
