// Model-free: c++ -std=c++14 -pthread tools/smart_repair_tests.cc -o smart-repair-tests
// Model tier: also compile rtengine/aiinpainting.cc with RT_AI_MASKING and link ONNX Runtime.
#include "../rtengine/repairmath.h"
#include "../rtengine/smartrepair.h"
#ifdef RT_AI_MASKING
#include "../rtengine/aiinpainting.h"
#endif
#include <chrono>
#include <future>
#include <iostream>
#include <thread>

using namespace rtengine;

void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
void near(float a, float b, float tolerance = .0001f) {
    require(std::fabs(a - b) <= tolerance, "numeric mismatch");
}

void mathTests() {
    using namespace repairmath;
    near(feather(5, 10, 20), 1);
    near(feather(15, 10, 20), .5);
    near(feather(20, 10, 20), 0);
    near(feather(11, 10, 10), 0);
    float previous = 1;
    for (int i = 0; i <= 1000; ++i) {
        const float value = feather(10 + i / 100.f, 10, 20);
        require(value <= previous, "feather must be monotonic");
        previous = value;
    }
    std::vector<float> source(15), resized(6);
    for (int i = 0; i < 15; ++i) source[i] = i;
    resample(source.data(), 5, 3, resized.data(), 3, 2);
    float mean = 0;
    for (float value : resized) mean += value / 6;
    near(mean, 7);
    std::vector<float> identity(15);
    resample(source.data(), 5, 3, identity.data(), 5, 3);
    require(identity == source, "identity resampling");
    std::fill(source.begin(), source.end(), .231f);
    resample(source.data(), 5, 3, resized.data(), 3, 2);
    for (float value : resized) near(value, .231f);
    std::vector<float> delta{0, 100, 200, 0};
    near(averageDelta(delta, 2, 2, 0, 0, 2, 8, 8, 0, 0), 75);
    near(averageDelta(delta, 2, 2, -1, -1, 2, 8, 8, 0, 0), 0);
    near(averageDelta(delta, 2, 2, 1, 0, 2, 8, 8, 2, 0), 25);
    near(averageDelta(delta, 2, 2, 1, 0, 2, 3, 8, 2, 0), 50);
    near(averageDelta(delta, 2, 2, 5, 5, 2, 8, 8, 0, 0), 0);
    // A fake fill deliberately correlates color with alpha: independent
    // color/mask averaging would give 150, not the correct 100.
    delta = {0 * 100.f, 1 * 200.f};
    near(averageDelta(delta, 2, 1, 0, 0, 2, 2, 1, 0, 0), 100);
    for (int x = 0; x < 512; ++x) {
        require(overlapWeight(x, 512, true, true, 128) == 1, "outer edge attenuation");
        require(overlapWeight(x, 512, false, false, 128) > 0, "uncovered overlap");
    }
    auto job = std::make_shared<SmartRepairControl>(3, 8);
    bool terminated = false;
    job->setCancelRun([&]() { terminated = true; });
    job->cancel();
    require(terminated && job->status().stage == SmartRepairStage::Cancelled, "cancel callback");
    job->setCancelRun({});
    bool threw = false;
    try { job->checkpoint(); } catch (const SmartRepairCancelled&) { threw = true; }
    require(threw, "cancel checkpoint");
    auto suspended = std::make_shared<SmartRepairControl>(4, 8);
    suspended->suspended = true;
    suspended->stage = SmartRepairStage::Cancelled;
    suspended->setStage(SmartRepairStage::Ready);
    require(suspended->stage == SmartRepairStage::Cancelled, "cancelled cannot become ready");
    std::cout << "PASS: resampling, premultiplied composition, borders, feather, overlap, job cancellation\n";
}

#ifdef RT_AI_MASKING
void modelTests(const char* path) {
    AIInpaintingEngine engine;
    require(engine.init(path), "model initialization");
    const int w = 384, h = 160, n = w * h;
    std::vector<float> r(n, .01f), g(n, .02f), b(n, .03f), mask(n, 0), ro(n), go(n), bo(n);
    // First image is almost black: output normalization must not depend on it.
    require(engine.inpaint(r.data(), g.data(), b.data(), mask.data(), w, h, ro.data(), go.data(), bo.data(), 2), "dark inference");
    for (int i = 0; i < n; ++i) near(ro[i], r[i], .0001f);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int i = y * w + x;
            r[i] = 140000.f * x / (w - 1);
            g[i] = 70000.f * y / (h - 1);
            b[i] = 5000.f;
        }
    }
    require(engine.inpaint(r.data(), g.data(), b.data(), mask.data(), w, h, ro.data(), go.data(), bo.data(), 2), "HDR rectangle inference");
    for (int i = 0; i < n; ++i) {
        near(ro[i], r[i], .1f); near(go[i], g[i], .1f); near(bo[i], b[i], .1f);
    }
    for (int y = 55; y < 105; ++y)
        for (int x = 160; x < 220; ++x) mask[y * w + x] = 1;
    require(engine.inpaint(r.data(), g.data(), b.data(), mask.data(), w, h, ro.data(), go.data(), bo.data(), 2), "masked inference");
    const auto first = ro;
    require(engine.inpaint(r.data(), g.data(), b.data(), mask.data(), w, h, ro.data(), go.data(), bo.data(), 2), "repeat inference");
    for (int i = 0; i < n; ++i) {
        require(std::isfinite(ro[i]) && std::isfinite(go[i]) && std::isfinite(bo[i]), "finite inference");
        near(ro[i], first[i], .1f);
    }
    auto job = std::make_shared<SmartRepairControl>(2, 8);
    auto future = std::async(std::launch::async, [&]() {
        try { engine.inpaint(r.data(), g.data(), b.data(), mask.data(), w, h, ro.data(), go.data(), bo.data(), 2, job); }
        catch (const SmartRepairCancelled&) { return true; }
        return false;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto start = std::chrono::steady_clock::now();
    job->cancel();
    require(future.get(), "in-flight inference cancellation");
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    require(milliseconds < 5000, "cancellation latency");
    require(engine.inpaint(r.data(), g.data(), b.data(), mask.data(), w, h, ro.data(), go.data(), bo.data(), 2), "recovery after cancel");
    std::cout << "PASS: installed model, dark-first contract, HDR, rectangular identity, deterministic fill, cancel/retry (" << milliseconds << "ms)\n";
}
#endif

int main(int argc, char** argv) {
    try {
        mathTests();
#ifdef RT_AI_MASKING
        if (argc > 1) modelTests(argv[1]);
#endif
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
