#pragma once

#include <mutex>

namespace rtengine
{

// Claiming the worker and handing off its last request must be atomic together.
class CropUpdateState
{
public:
    bool request()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_ = true;
        if (running_) {
            return false;
        }
        running_ = true;
        return true;
    }

    bool takePending()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_) {
            pending_ = false;
            return true;
        }
        running_ = false;
        return false;
    }

    void cancel()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_ = running_ = false;
    }

private:
    std::mutex mutex_;
    bool pending_ = false;
    bool running_ = false;
};

}
