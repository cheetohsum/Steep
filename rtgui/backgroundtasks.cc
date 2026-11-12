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
#include "backgroundtasks.h"

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace
{

class TaskLane final
{
public:
    TaskLane(int threads, int ompThreads)
    {
        for (int i = 0; i < threads; ++i) {
            std::thread([this, ompThreads]() { run(ompThreads); }).detach();
        }
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
    void run(int ompThreads)
    {
#ifdef _OPENMP
        // Per-thread setting: it sizes the one team this thread will keep.
        omp_set_num_threads(ompThreads);
#else
        (void)ompThreads;
#endif

        while (true) {
            std::function<void()> work;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [this]() { return !queue_.empty(); });
                work = std::move(queue_.front());
                queue_.pop_front();
            }

            // A job that throws must not take the lane (and with it the
            // process: an exception leaving a thread calls terminate) down.
            try {
                work();
            } catch (...) {
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::function<void()>> queue_;
};

}

namespace steep
{

void runBackground(std::function<void()> work, Lane lane)
{
    if (!work) {
        return;
    }

    // Leaked on purpose, like SharedProgressWorker: the threads outlive
    // static destruction, so the queues they wait on must too.
    if (lane == Lane::CPU) {
        static TaskLane* const cpu = new TaskLane(1, 4);
        cpu->enqueue(std::move(work));
    } else {
        static TaskLane* const io = new TaskLane(2, 1);
        io->enqueue(std::move(work));
    }
}

std::future<void> runBackgroundJoinable(std::function<void()> work, Lane lane)
{
    auto done = std::make_shared<std::promise<void>>();
    std::future<void> finished = done->get_future();

    runBackground([work = std::move(work), done]() {
        try {
            if (work) {
                work();
            }
        } catch (...) {
        }
        done->set_value();
    }, lane);

    return finished;
}

void SerialQueue::enqueue(std::function<void()> work)
{
    if (!work) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(work));
        if (scheduled_) {
            return;
        }
        scheduled_ = true;
    }

    runBackground([this]() { runOne(); }, lane_);
}

void SerialQueue::runOne()
{
    std::function<void()> work;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        work = std::move(queue_.front());
        queue_.pop_front();
    }

    try {
        work();
    } catch (...) {
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            scheduled_ = false;
            return;
        }
    }

    runBackground([this]() { runOne(); }, lane_);
}

}
