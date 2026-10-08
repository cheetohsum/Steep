#pragma once

// Per-camera memory of stuck photosites (see stuckpixels.h).
//
// A defective photosite sits at the same sensor coordinates in every frame a
// camera takes; a small coloured light that one frame cannot tell from it
// does not. So each frame's candidates are remembered per camera body, and a
// photosite seen in two separate shooting sessions -- 12 hours apart, so a
// tripod burst, bracket or timelapse of the same scene counts once -- is
// known to be stuck. Known photosites are fixed in every frame, including
// faint ones the single-frame test would miss. Once a camera has been seen
// over three sessions its real defects are known, and a new candidate must
// pass the strict (sensitivity 0) test to be fixed: what is left looks like
// scene detail.
//
// One small text file per camera body in the settings folder. Reprocessing a
// frame adds nothing (same time, same session), so the files only grow with
// genuinely new candidates; each frame contributes at most its strongest
// kMaxPerFrame, and single-session entries are evicted oldest first past
// kMaxEntries. Thread-safe.

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace rtengine {

struct StuckPixelCandidate {
    int row, col;
    float strength; // deviation over the sensor noise
};

class StuckPixelMemory
{
public:
    static constexpr std::int64_t kSessionGap = 12 * 3600; // seconds
    static constexpr int kKnownSessions = 2;               // sessions before a photosite counts as stuck
    static constexpr int kMatureSessions = 3;              // sessions before new candidates must be strict
    static constexpr size_t kMaxPerFrame = 300;
    static constexpr size_t kMaxEntries = 20000;

    struct Verdict {
        std::vector<std::pair<int, int>> known; // photosites to fix in every frame
        bool mature = false;
        int sessions = 0;                       // sessions this camera has been seen in
    };

    static StuckPixelMemory& instance();

    // Where the per-camera files live; empty keeps everything in memory.
    void setDirectory(const std::string& directory);

    // Remembers this frame's candidates (timestamp: capture time, seconds;
    // 0 = unknown, then nothing is learned from it) and returns what is known
    // about the camera afterwards.
    Verdict update(const std::string& cameraKey, std::int64_t timestamp, std::vector<StuckPixelCandidate> candidates);

    // What is known, without learning anything.
    Verdict query(const std::string& cameraKey);

    // For tests: forget everything in memory (files are left alone).
    void clearCache();

private:
    struct Entry {
        std::int64_t sessions[kKnownSessions] = {};
        int sessionCount = 0;
        std::uint32_t lastTouch = 0; // update counter when last seen, for eviction
    };
    struct Camera {
        bool loaded = false;
        std::vector<std::int64_t> sessions; // session anchors seen for the camera (capped)
        std::map<std::pair<int, int>, Entry> pixels;
        std::uint32_t touch = 0;
    };

    Camera& camera(const std::string& key);
    void load(const std::string& key, Camera& cam);
    void save(const std::string& key, const Camera& cam) const;
    std::string pathFor(const std::string& key) const;
    static bool addSession(std::vector<std::int64_t>& anchors, std::int64_t t, size_t cap);
    static Verdict verdictFor(const Camera& cam);

    std::mutex mutex_;
    std::string directory_;
    std::map<std::string, Camera> cameras_;
};

} // namespace rtengine
