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
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace rtengine
{

/**
 * Frees AI model sessions nobody has used for a while. On DirectML a loaded
 * session holds its weights and working buffers in GPU and shared system
 * memory -- gigabytes for BiRefNet at 1024 px -- long after the photo it was
 * loaded for has been segmented. Each engine registers a callback that
 * releases its session when idle past kIdleMs (and reloads it on the next
 * use); one background thread calls them every few seconds.
 */
class AIIdleReaper
{
public:
    // Long enough that moving between photos does not reload a model per
    // photo; short enough that a session is gone soon after the last mask.
    static constexpr long long kIdleMs = 60000;

    static long long nowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // The callback runs on the reaper thread and must not block for long:
    // try-lock the engine, release if idle (or at once when forced), return.
    // Returns the engine's id, for releaseNow().
    static int watch(std::function<void(bool force)> release)
    {
        State& state = instance();
        std::lock_guard<std::mutex> lock(state.mutex);
        state.callbacks.push_back(std::move(release));
        const int id = static_cast<int>(state.callbacks.size()) - 1;
        if (!state.started) {
            state.started = true;
            // Engines that are static objects are destroyed at exit; from
            // then on the reaper leaves them alone.
            std::atexit([]() { instance().exiting = true; });
            // Detached and never joined: it may be mid-release at exit, when
            // static destructors must not wait on it.
            std::thread([&state]() {
                while (true) {
                    std::this_thread::sleep_for(std::chrono::seconds(5));
                    std::vector<std::function<void(bool)>> callbacks;
                    {
                        std::lock_guard<std::mutex> lock(state.mutex);
                        callbacks = state.callbacks;
                    }
                    for (const auto& callback : callbacks) {
                        if (state.exiting) {
                            return;
                        }
                        callback(false);
                    }
                }
            }).detach();
        }
        return id;
    }

    // Out of memory: frees every model not in use right now, on the calling
    // thread. `except` is the caller's own engine (its run lock is held, and
    // try-locking a mutex one already holds is undefined).
    static void releaseNow(int except = -1)
    {
        State& state = instance();
        std::vector<std::function<void(bool)>> callbacks;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            callbacks = state.callbacks;
        }
        for (int i = 0; i < static_cast<int>(callbacks.size()); ++i) {
            if (i != except && !state.exiting) {
                callbacks[i](true);
            }
        }
    }

    // Whether an inference error is the device or the process running out
    // of memory (DirectML reports E_OUTOFMEMORY, 8007000E), which freeing
    // other models and trying again can cure.
    static bool outOfMemory(const char* message)
    {
        return message && (std::strstr(message, "8007000E") || std::strstr(message, "8007000e")
                           || std::strstr(message, "Not enough memory") || std::strstr(message, "out of memory")
                           || std::strstr(message, "bad_alloc") || std::strstr(message, "Failed to allocate"));
    }

private:
    struct State {
        std::mutex mutex;
        std::vector<std::function<void(bool)>> callbacks;
        bool started = false;
        std::atomic<bool> exiting{false};
    };

    static State& instance()
    {
        static State* const state = new State();   // never destroyed, see watch()
        return *state;
    }
};

}
