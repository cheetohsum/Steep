#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>

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

}
