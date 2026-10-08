#include "rtengine/cropupdatestate.h"
#include "rtengine/previewquality.h"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>

void check(bool ok)
{
    if (!ok) {
        std::cerr << "Crop update handoff failed\n";
        std::exit(1);
    }
}

int main()
{
    check(rtengine::previewThreadCount(0) == 1);
    check(rtengine::previewThreadCount(1) == 1);
    check(rtengine::previewThreadCount(2) == 2);
    check(rtengine::previewThreadCount(4) == 4);
    check(rtengine::previewThreadCount(16) == 4);
    check(rtengine::previewThreadCount(128) == 4);
    rtengine::CropUpdateState state;
    check(state.request());
    check(!state.request());
    check(state.takePending());
    check(!state.request());
    check(state.takePending());
    check(!state.takePending());
    check(state.request());
    state.cancel();
    check(state.request());
    check(state.takePending());
    check(!state.takePending());

    // Race the final worker check against a new viewport request. Either
    // the existing worker takes it or the producer claims a new worker.
    for (int i = 0; i < 4000; ++i) {
        check(state.request());
        check(state.takePending());
        std::atomic<bool> ready{false};
        bool newWorker = false;
        std::thread producer([&]() {
            while (!ready.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            newWorker = state.request();
        });
        ready.store(true, std::memory_order_release);
        const bool oldWorkerTookRequest = state.takePending();
        producer.join();
        check(oldWorkerTookRequest != newWorker);
        if (newWorker) {
            check(state.takePending());
        }
        check(!state.takePending());
    }
    std::cout << "Crop update coalescing and 4000 concurrent handoffs passed\n";
}
