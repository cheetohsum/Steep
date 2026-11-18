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
// over three sessions its real defects are known, and a new candidate whose
// neighbours caught light is held back: what is left looks like scene detail.
//
// Only clean frames teach it: at most kLearnMaxISO and at most
// kLearnMaxCandidates candidates. A sensor has thousands of mildly warm
// photosites that only spike above the noise at high gain; they recur from
// one ISO 3200+ frame to the next, so learning from those frames made them
// "known" and fixed them in every daylight frame too (an X-T4 reached 3942
// known photosites, of which ~180 show at ISO <= 800). Noisy frames still get
// the known fixes and their own single-frame candidates, they just don't
// teach. A file from before this rule (format 1) is set aside as
// <name>.txt.v1 and the camera is learned again.
//
// One small text file per camera body in the settings folder. Reprocessing a
// frame adds nothing (same time, same session), so the files only grow with
// genuinely new candidates; single-session entries are evicted oldest first
// past kMaxEntries. Thread-safe.

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
    static constexpr int kMatureSessions = 3;              // sessions before ambiguous new candidates wait
    static constexpr int kLearnMaxISO = 1600;              // noisier frames don't teach ...
    static constexpr size_t kLearnMaxCandidates = 300;     // ... nor frames full of candidates
    static constexpr size_t kMaxEntries = 20000;

    struct Verdict {
        std::vector<std::pair<int, int>> known; // photosites to fix in every frame
        bool mature = false;
        int sessions = 0;                       // sessions this camera has been seen in
        bool learned = false;                   // this frame taught the memory (a clean frame)
    };

    static StuckPixelMemory& instance();

    // Where the per-camera files live; empty keeps everything in memory.
    void setDirectory(const std::string& directory);

    // Remembers this frame's candidates (timestamp: capture time, seconds;
    // 0 = unknown, then nothing is learned from it; iso: 0 = unknown) and
    // returns what is known about the camera afterwards. Frames above
    // kLearnMaxISO or with more than kLearnMaxCandidates candidates teach
    // nothing.
    Verdict update(const std::string& cameraKey, std::int64_t timestamp, int iso, const std::vector<StuckPixelCandidate>& candidates);

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
        bool dirty = false; // set aside an old file: write the new one on the next lesson
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
