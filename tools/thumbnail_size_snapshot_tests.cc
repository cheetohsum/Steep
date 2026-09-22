#include "thumbnailsizesnapshot.h"

#include <atomic>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

static void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

int main()
{
    try {
        ThumbnailSizeSnapshot size;
        auto check = [&](int sourceW, int sourceH, int targetH, int cap, int wantW, int wantH) {
            size.publish(sourceW, sourceH);
            int w = 0, h = targetH;
            size.fit(w, h, cap);
            require(w == wantW && h == wantH, "aspect/size limit mismatch");
        };
        check(6000, 4000, 160, 800, 240, 160);
        check(4000, 6000, 150, 800, 100, 150);
        check(12000, 1000, 250, 800, 800, 66);
        check(1000, 1000, 0, 800, 1, 1);
        size.publish(100, 100, 1.5f);
        size.publish(0, 0);
        size.publish(-1, 10);
        size.publish(0, 0, std::numeric_limits<float>::infinity());
        int w = 0, h = 160;
        size.fit(w, h, 800);
        require(w == 240 && h == 160, "invalid worker state replaced last valid dimensions");

        std::atomic<bool> done{false};
        std::thread writer([&]() {
            for (int i = 0; i < 200000; ++i) size.publish(i % 2 ? 6000 : 3000, 4000);
            done.store(true);
        });
        bool coherent = true;
        do {
            h = 160;
            size.fit(w, h, 800);
            coherent &= h == 160 && (w == 240 || w == 120);
        } while (!done.load());
        writer.join();
        require(coherent, "layout observed partial geometry");
        std::cout << "PASS portrait/landscape, width caps, invalid dimensions and concurrent layout reads\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
