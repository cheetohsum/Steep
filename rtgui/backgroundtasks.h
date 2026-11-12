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

#include <deque>
#include <functional>
#include <future>
#include <mutex>

namespace steep
{

// Every std::thread that enters an OpenMP parallel region keeps its own
// worker team for as long as the process lives (MinGW's libgomp never frees
// it), so a detached thread per folder hover or per count pass leaves a
// full team of idle workers behind each time. These lanes are created once
// and reused, which bounds that to two small teams.
enum class Lane {
    // Two threads, OpenMP held to one: directory scans, counts, sidecar
    // reads, small thumbnails. Work that mostly waits on the disk.
    IO,
    // One thread, OpenMP held to four: auto-level / auto-cull style work
    // that decodes and analyses whole photos.
    CPU
};

// Queues `work` on the lane; FIFO within a lane. Never runs it on the calling
// thread. The work must marshal anything it does to widgets back through an
// idle callback, exactly as a detached thread would, and should check its
// own cancel/generation flag: a job may start long after it was queued.
void runBackground(std::function<void()> work, Lane lane = Lane::IO);

// The same, for owners that used to keep a joinable std::thread: the future
// becomes ready once `work` has returned (or thrown), so `wait()` stands in
// for `join()`. Waiting also waits for whatever is queued ahead on the lane.
std::future<void> runBackgroundJoinable(std::function<void()> work, Lane lane = Lane::IO);

// Runs its jobs one at a time, in order, on a lane, and never holds more
// than one of the lane's threads: between jobs it goes back to the end of
// the lane's queue, so a long backlog here cannot starve the lane's other
// users. Meant to live for the whole process.
class SerialQueue final
{
public:
    explicit SerialQueue(Lane lane = Lane::IO) : lane_(lane) {}

    void enqueue(std::function<void()> work);

private:
    void runOne();

    const Lane lane_;
    std::mutex mutex_;
    std::deque<std::function<void()>> queue_;
    bool scheduled_ = false;
};

}
