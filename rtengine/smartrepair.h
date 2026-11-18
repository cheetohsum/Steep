#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace rtengine {

enum class SmartRepairStage { Idle, Queued, Preparing, Generating, Blending, Presenting, Ready, Failed, Cancelled };

struct SmartRepairStatus {
    SmartRepairStage stage = SmartRepairStage::Idle;
    unsigned int methods = 0;
    unsigned int generation = 0;
    int completedTiles = 0;
    int totalTiles = 0;
};

class SmartRepairCancelled : public std::runtime_error {
public:
    SmartRepairCancelled() : std::runtime_error("Repair cancelled") {}
};

// One coordinator generation owns this token. No callbacks into GTK are made
// from inference; the UI polls the cheap atomic snapshot instead.
class SmartRepairControl {
    std::mutex cancelMutex_;
    std::function<void()> cancelRun_;
public:
    const unsigned int generation;
    const unsigned int methods;
    std::atomic<bool> cancelled{false};
    bool suspended = false;
    std::atomic<SmartRepairStage> stage{SmartRepairStage::Queued};
    std::atomic<int> completedTiles{0}, totalTiles{0};
    std::atomic<bool> composed{false};

    SmartRepairControl(unsigned int generation, unsigned int methods) : generation(generation), methods(methods) {}
    void cancel() {
        std::lock_guard<std::mutex> lock(cancelMutex_);
        cancelled = true;
        stage = SmartRepairStage::Cancelled;
        if (cancelRun_) cancelRun_();
    }
    void setCancelRun(std::function<void()> callback) {
        std::lock_guard<std::mutex> lock(cancelMutex_);
        cancelRun_ = std::move(callback);
        if (cancelled && cancelRun_) cancelRun_();
    }
    void checkpoint() const {
        if (cancelled) throw SmartRepairCancelled();
    }
    void setStage(SmartRepairStage value) {
        checkpoint();
        if (suspended) return;
        if (stage != SmartRepairStage::Failed) stage = value;
    }
    SmartRepairStatus status() const {
        return {stage.load(), methods, generation, completedTiles.load(), totalTiles.load()};
    }
};

using SmartRepairJob = std::shared_ptr<SmartRepairControl>;

// What the latest smart repair selected and how it was filled, for the tool
// panel to show. Like the status above it is written by the engine and
// polled by the UI: `serial` moves whenever anything here changes.
struct SmartRepairReport {
    unsigned int serial = 0;

    // The object a Remove Object click picked, as a coverage grid over a
    // rectangle of the picture in spot coordinates: 255 on the object, 128
    // in the margin that is replaced with it, 0 outside. Empty when the
    // latest repair picked nothing.
    int clickX = -1, clickY = -1;                 // the entry's first click
    int x = 0, y = 0, width = 0, height = 0;      // rectangle, full-size pixels
    int gridWidth = 0, gridHeight = 0;
    std::shared_ptr<const std::vector<unsigned char>> grid;
    // Which of the selector's nested candidates was taken (0 smallest), and
    // whether a smaller or a larger one exists for the user to ask for.
    int candidate = -1;
    bool hasSmaller = false, hasLarger = false;

    enum class Fill { None, Standard, Wide, WideSkippedMemory, WideFailed };
    Fill fill = Fill::None;
    float neededGb = 0.f, freeGb = 0.f;           // for WideSkippedMemory
};

class SmartRepairReports {
    struct State {
        std::mutex mutex;
        SmartRepairReport report;
    };
    static State& state() {
        static State* const s = new State();
        return *s;
    }
public:
    static SmartRepairReport get() {
        State& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        return s.report;
    }
    static void setObject(const SmartRepairReport& object) {
        State& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        const unsigned int serial = s.report.serial + 1;
        const auto fill = s.report.fill;
        const float needed = s.report.neededGb, free = s.report.freeGb;
        s.report = object;
        s.report.serial = serial;
        s.report.fill = fill;
        s.report.neededGb = needed;
        s.report.freeGb = free;
    }
    static void clearObject() {
        SmartRepairReport none;
        setObject(none);
    }
    static void setFill(SmartRepairReport::Fill fill, float neededGb = 0.f, float freeGb = 0.f) {
        State& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        ++s.report.serial;
        s.report.fill = fill;
        s.report.neededGb = neededGb;
        s.report.freeGb = freeGb;
    }
};

}
