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
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

#include "rtengine.h"

namespace rtengine
{

/**
 * A second decode of the photo an editor just opened, prepared at full
 * quality (ImageSource::precomputeFullQuality) on a background thread while
 * the editor shows its fast first frame. The editor's settled pass adopts its
 * demosaic instead of computing one itself, so full quality arrives once the
 * twin is done rather than one whole demosaic after the first frame.
 *
 * Shared between the loader thread and the ImProcCoordinator; whichever side
 * lets go last releases the image.
 */
class FullQualityTwin
{
public:
    enum class Stage {
        Queued,
        Decoding,
        Precomputing,
        Done
    };

    ~FullQualityTwin()
    {
        release();
    }

    // Loader side.
    void setStage(Stage stage)
    {
        stage_.store(stage, std::memory_order_release);
    }

    // The loader decodes at once but holds the demosaic until the editor's
    // first frame is out (or timeoutMs passes): sharing the cores earlier
    // only delays that frame. False when the twin was abandoned meanwhile.
    bool waitForPrecomputeRelease(int timeoutMs)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this]() {
            return precomputeReleased_ || abandoned_->load(std::memory_order_acquire);
        });
        return !abandoned_->load(std::memory_order_acquire);
    }

    // Engine side, after each finished pass.
    void releasePrecompute()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (precomputeReleased_) {
                return;
            }
            precomputeReleased_ = true;
        }
        cv_.notify_all();
    }

    // Hands over one reference to img (null when the load gave up).
    void deliver(InitialImage* img)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!abandoned_->load(std::memory_order_acquire)) {
                image_ = img;
                img = nullptr;
            }
            stage_.store(Stage::Done, std::memory_order_release);
        }
        if (img) {
            img->decreaseRef();
        }
        cv_.notify_all();
    }

    // Set once nobody will take the twin any more; the loader polls it.
    const std::shared_ptr<std::atomic<bool>>& abandonedFlag() const
    {
        return abandoned_;
    }

    // Engine side. Waits up to timeoutMs, but only while the twin is already
    // demosaicing -- a twin still queued or decoding would cost the caller
    // as much as doing the work itself. Returns the image with one reference
    // or null; the slot is closed either way.
    InitialImage* take(int timeoutMs)
    {
        InitialImage* img = nullptr;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (stage_.load(std::memory_order_acquire) == Stage::Precomputing) {
                cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this]() {
                    return stage_.load(std::memory_order_acquire) == Stage::Done;
                });
            }
            img = image_;
            image_ = nullptr;
            abandoned_->store(true, std::memory_order_release);
        }
        return img;
    }

    void abandon()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            abandoned_->store(true, std::memory_order_release);
        }
        cv_.notify_all();
        release();
    }

private:
    void release()
    {
        InitialImage* img = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            img = image_;
            image_ = nullptr;
        }
        if (img) {
            img->decreaseRef();
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    InitialImage* image_ = nullptr;
    bool precomputeReleased_ = false;
    std::atomic<Stage> stage_{Stage::Queued};
    const std::shared_ptr<std::atomic<bool>> abandoned_ = std::make_shared<std::atomic<bool>>(false);
};

}
