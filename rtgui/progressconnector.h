/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2004-2010 Gabor Horvath <hgabor@rawtherapee.com>
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
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#include <gtkmm.h>

#include <sigc++/sigc++.h>

#include "guiutils.h"
#include "multilangmgr.h"
#include "rtengine/rtengine.h"

class SharedProgressWorker final
{
public:
    static SharedProgressWorker& instance()
    {
        // This process-lifetime worker keeps OpenMP runtimes from retaining a
        // new worker team for every short-lived foreground image-load thread.
        static SharedProgressWorker* worker = new SharedProgressWorker();
        return *worker;
    }

    void enqueue(std::function<void()> work)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(std::move(work));
        }
        ready_.notify_one();
    }

private:
    SharedProgressWorker()
    {
        std::thread([this]() { run(); }).detach();
    }

    void run()
    {
        while (true) {
            std::function<void()> work;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [this]() { return !queue_.empty(); });
                work = std::move(queue_.front());
                queue_.pop_front();
            }
            work();
        }
    }

    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::function<void()>> queue_;
};

class PLDBridge final :
    public rtengine::ProgressListener
{
public:
    explicit PLDBridge(rtengine::ProgressListener* pb) :
        pl(pb)
    {
    }

    ~PLDBridge() override
    {
        idle_register_.destroy();
    }

    // ProgressListener interface
    void setProgress(double p) override
    {
        const bool endpoint = p <= 0.0 || p >= 1.0;
        const auto now = std::chrono::steady_clock::now();
        bool emit = false;
        bool scheduleFlush = false;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            const bool first = lastProgressTime_ == std::chrono::steady_clock::time_point();
            const double delta = std::abs(p - lastProgress_);
            const bool changed = first || delta > 0.000001;
            const bool movedEnough = first || delta >= 0.02;
            const bool waitedEnough = changed && now - lastProgressTime_ >= std::chrono::milliseconds(40);
            emit = (endpoint && changed) || movedEnough || waitedEnough;

            if (!emit) {
                return;
            }

            lastProgress_ = p;
            lastProgressTime_ = now;
            pendingProgress_ = p;
            pendingProgressValid_ = true;
            scheduleFlush = !uiFlushPending_;
            uiFlushPending_ = true;
        }

        if (scheduleFlush) {
            scheduleUIFlush();
        }
    }

    void setProgressStr(const Glib::ustring& str) override
    {
        bool scheduleFlush = false;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (str == lastProgressStrKey_) {
                return;
            }

            lastProgressStrKey_ = str;
            pendingProgressStr_ = str;
            pendingProgressStrValid_ = true;
            scheduleFlush = !uiFlushPending_;
            uiFlushPending_ = true;
        }

        if (scheduleFlush) {
            scheduleUIFlush();
        }
    }

    void setProgressState(bool inProcessing) override
    {
        bool scheduleFlush = false;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (lastProgressStateValid_ && lastProgressState_ == inProcessing) {
                return;
            }

            lastProgressState_ = inProcessing;
            lastProgressStateValid_ = true;
            pendingProgressState_ = inProcessing;
            pendingProgressStateValid_ = true;
            scheduleFlush = !uiFlushPending_;
            uiFlushPending_ = true;
        }

        if (scheduleFlush) {
            scheduleUIFlush();
        }
    }

    void error(const Glib::ustring& descr) override
    {
        GThreadLock lock;
        pl->error(descr);
    }

private:
    void scheduleUIFlush()
    {
        idle_register_.add(
            [this]() -> bool {
                flushUI();
                return false;
            },
            G_PRIORITY_DEFAULT_IDLE);
    }

    void flushUI()
    {
        double progress = 0.0;
        bool progressValid = false;
        Glib::ustring progressStr;
        bool progressStrValid = false;
        bool progressState = false;
        bool progressStateValid = false;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            progress = pendingProgress_;
            progressValid = pendingProgressValid_;
            pendingProgress_ = 0.0;
            pendingProgressValid_ = false;

            progressStr = pendingProgressStr_;
            progressStrValid = pendingProgressStrValid_;
            pendingProgressStr_.clear();
            pendingProgressStrValid_ = false;

            progressState = pendingProgressState_;
            progressStateValid = pendingProgressStateValid_;
            pendingProgressState_ = false;
            pendingProgressStateValid_ = false;

            uiFlushPending_ = false;
        }

        if (progressStrValid) {
            pl->setProgressStr(M(progressStr));
        }
        if (progressValid) {
            pl->setProgress(progress);
        }
        if (progressStateValid) {
            pl->setProgressState(progressState);
        }
    }

    rtengine::ProgressListener* const pl;
    IdleRegister idle_register_;
    std::mutex mutex_;
    double lastProgress_ = -1.0;
    std::chrono::steady_clock::time_point lastProgressTime_;
    Glib::ustring lastProgressStrKey_;
    bool lastProgressState_ = false;
    bool lastProgressStateValid_ = false;
    bool uiFlushPending_ = false;
    double pendingProgress_ = 0.0;
    bool pendingProgressValid_ = false;
    Glib::ustring pendingProgressStr_;
    bool pendingProgressStrValid_ = false;
    bool pendingProgressState_ = false;
    bool pendingProgressStateValid_ = false;
};

template<class T>
class ProgressConnector
{

    sigc::signal0<T> opStart;
    sigc::signal0<bool> opEnd;
    T retval;
    Glib::Thread *workThread;
    std::atomic<bool> started;

    static int emitEndSignalUI (void* data)
    {

        const sigc::signal0<bool>* lopEnd = reinterpret_cast<sigc::signal0<bool>*>(data);
        const int r = lopEnd->emit ();
        delete lopEnd;

        return r;
    }

    void workingThread ()
    {
        retval = opStart.emit ();
        // Use HIGH priority so the image-loaded callback runs before
        // queued directory-enumeration and thumbnail-loading idle callbacks,
        // which default to G_PRIORITY_DEFAULT_IDLE (200).
        gdk_threads_add_idle_full(G_PRIORITY_HIGH,
                                  ProgressConnector<T>::emitEndSignalUI,
                                  new sigc::signal0<bool>(opEnd), nullptr);
        workThread = nullptr;
        started.store(false, std::memory_order_release);
    }

public:

    ProgressConnector (): retval( 0 ), workThread( nullptr ), started(false) { }

    void startFunc (const sigc::slot0<T>& startHandler, const sigc::slot0<bool>& endHandler )
    {
        if (!started.exchange(true, std::memory_order_acq_rel)) {
            opStart.connect (startHandler);
            opEnd.connect (endHandler);
            workThread = Glib::Thread::create(sigc::mem_fun(*this, &ProgressConnector<T>::workingThread), 0, true, true, Glib::THREAD_PRIORITY_NORMAL);
        }
    }

    void startFuncPersistent (const sigc::slot0<T>& startHandler, const sigc::slot0<bool>& endHandler )
    {
        if (!started.exchange(true, std::memory_order_acq_rel)) {
            opStart.connect(startHandler);
            opEnd.connect(endHandler);
            SharedProgressWorker::instance().enqueue([this]() { workingThread(); });
        }
    }

    T returnValue()
    {
        return retval;
    }
};
