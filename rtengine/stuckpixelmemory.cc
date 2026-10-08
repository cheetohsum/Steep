#include "stuckpixelmemory.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

#include <glib.h>
#include <glib/gstdio.h>

namespace rtengine {

// C++14: constants bound by reference (std::min, std::max) need a definition.
constexpr std::int64_t StuckPixelMemory::kSessionGap;
constexpr int StuckPixelMemory::kKnownSessions;
constexpr int StuckPixelMemory::kMatureSessions;
constexpr size_t StuckPixelMemory::kMaxPerFrame;
constexpr size_t StuckPixelMemory::kMaxEntries;

namespace {

constexpr size_t kMaxCameraSessions = 256;
constexpr const char* kHeader = "steep-stuckpixels 1";

bool sameSession(std::int64_t a, std::int64_t b)
{
    return std::llabs(static_cast<long long>(a - b)) < StuckPixelMemory::kSessionGap;
}

} // namespace

StuckPixelMemory& StuckPixelMemory::instance()
{
    static StuckPixelMemory memory;
    return memory;
}

void StuckPixelMemory::setDirectory(const std::string& directory)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (directory != directory_) {
        directory_ = directory;
        cameras_.clear();
    }
}

void StuckPixelMemory::clearCache()
{
    std::lock_guard<std::mutex> lock(mutex_);
    cameras_.clear();
}

bool StuckPixelMemory::addSession(std::vector<std::int64_t>& anchors, std::int64_t t, size_t cap)
{
    for (std::int64_t a : anchors) {
        if (sameSession(a, t)) {
            return false;
        }
    }
    if (anchors.size() < cap) {
        anchors.push_back(t);
    }
    return true;
}

StuckPixelMemory::Verdict StuckPixelMemory::verdictFor(const Camera& cam)
{
    Verdict v;
    v.sessions = static_cast<int>(cam.sessions.size());
    v.mature = v.sessions >= kMatureSessions;
    for (const auto& p : cam.pixels) {
        if (p.second.sessionCount >= kKnownSessions) {
            v.known.push_back(p.first);
        }
    }
    return v;
}

StuckPixelMemory::Camera& StuckPixelMemory::camera(const std::string& key)
{
    Camera& cam = cameras_[key];
    if (!cam.loaded) {
        load(key, cam);
        cam.loaded = true;
    }
    return cam;
}

StuckPixelMemory::Verdict StuckPixelMemory::query(const std::string& cameraKey)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return verdictFor(camera(cameraKey));
}

StuckPixelMemory::Verdict StuckPixelMemory::update(const std::string& cameraKey, std::int64_t timestamp, std::vector<StuckPixelCandidate> candidates)
{
    std::lock_guard<std::mutex> lock(mutex_);
    Camera& cam = camera(cameraKey);
    if (timestamp <= 0) {
        return verdictFor(cam); // no capture time: sessions can't be told apart, so learn nothing
    }

    bool changed = addSession(cam.sessions, timestamp, kMaxCameraSessions);
    ++cam.touch;

    // The strongest candidates are the likeliest defects; a noise-dominated
    // frame (ISO 25600: ~1800 candidates, none of them repeatable) must not
    // flood the file.
    std::sort(candidates.begin(), candidates.end(),
              [](const StuckPixelCandidate& a, const StuckPixelCandidate& b) { return a.strength > b.strength; });
    if (candidates.size() > kMaxPerFrame) {
        candidates.resize(kMaxPerFrame);
    }

    for (const StuckPixelCandidate& c : candidates) {
        const auto inserted = cam.pixels.emplace(std::make_pair(c.row, c.col), Entry());
        Entry& e = inserted.first->second;
        e.lastTouch = cam.touch;
        bool newSession = true;
        for (int i = 0; i < e.sessionCount; ++i) {
            if (sameSession(e.sessions[i], timestamp)) {
                newSession = false;
            }
        }
        if (newSession && e.sessionCount < kKnownSessions) {
            e.sessions[e.sessionCount++] = timestamp;
            changed = true;
        }
    }

    if (cam.pixels.size() > kMaxEntries) {
        // Forget single-session sightings, oldest first.
        std::vector<std::pair<std::uint32_t, std::pair<int, int>>> single;
        for (const auto& p : cam.pixels) {
            if (p.second.sessionCount < kKnownSessions) {
                single.emplace_back(p.second.lastTouch, p.first);
            }
        }
        std::sort(single.begin(), single.end());
        const size_t target = kMaxEntries * 3 / 4;
        for (const auto& s : single) {
            if (cam.pixels.size() <= target) {
                break;
            }
            cam.pixels.erase(s.second);
        }
        changed = true;
    }

    if (changed) {
        save(cameraKey, cam);
    }
    return verdictFor(cam);
}

std::string StuckPixelMemory::pathFor(const std::string& key) const
{
    // A short, stable name: the key's first word or two for people, a hash
    // of the whole key (serial numbers can be long) for uniqueness, so the
    // path stays well inside Windows' 260-character limit.
    std::uint64_t hash = 1469598103934665603ull; // FNV-1a
    for (unsigned char ch : key) {
        hash = (hash ^ ch) * 1099511628211ull;
    }
    std::string name;
    for (char ch : key) {
        if (name.size() >= 24) {
            break;
        }
        name += (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-') ? ch : '_';
    }
    char tail[24];
    std::snprintf(tail, sizeof tail, "_%016llx.txt", static_cast<unsigned long long>(hash));
    return directory_ + G_DIR_SEPARATOR_S + name + tail;
}

void StuckPixelMemory::load(const std::string& key, Camera& cam)
{
    if (directory_.empty()) {
        return;
    }
    FILE* f = g_fopen(pathFor(key).c_str(), "r");
    if (!f) {
        return;
    }
    char line[256];
    bool ok = std::fgets(line, sizeof line, f) && std::string(line).compare(0, std::string(kHeader).size(), kHeader) == 0;
    while (ok && std::fgets(line, sizeof line, f)) {
        long long a = 0, t0 = 0, t1 = 0;
        int row = 0, col = 0, count = 0;
        unsigned touch = 0;
        if (std::sscanf(line, "session %lld", &a) == 1) {
            if (cam.sessions.size() < kMaxCameraSessions) {
                cam.sessions.push_back(a);
            }
        } else if (std::sscanf(line, "touch %u", &touch) == 1) {
            cam.touch = touch;
        } else if (std::sscanf(line, "px %d %d %d %lld %lld %u", &row, &col, &count, &t0, &t1, &touch) == 6) {
            Entry& e = cam.pixels[std::make_pair(row, col)];
            e.sessionCount = std::max(0, std::min(count, kKnownSessions));
            e.sessions[0] = t0;
            e.sessions[1] = t1;
            e.lastTouch = touch;
        }
    }
    std::fclose(f);
}

void StuckPixelMemory::save(const std::string& key, const Camera& cam) const
{
    if (directory_.empty()) {
        return;
    }
    g_mkdir_with_parents(directory_.c_str(), 0755);
    const std::string path = pathFor(key);
    const std::string tmp = path + ".tmp";
    FILE* f = g_fopen(tmp.c_str(), "w");
    if (!f) {
        return;
    }
    std::fprintf(f, "%s\ncamera %s\ntouch %u\n", kHeader, key.c_str(), cam.touch);
    for (std::int64_t s : cam.sessions) {
        std::fprintf(f, "session %lld\n", static_cast<long long>(s));
    }
    for (const auto& p : cam.pixels) {
        std::fprintf(f, "px %d %d %d %lld %lld %u\n", p.first.first, p.first.second, p.second.sessionCount,
                     static_cast<long long>(p.second.sessions[0]), static_cast<long long>(p.second.sessions[1]), p.second.lastTouch);
    }
    const bool written = std::fclose(f) == 0;
    if (written) {
        g_remove(path.c_str());
        g_rename(tmp.c_str(), path.c_str());
    } else {
        g_remove(tmp.c_str());
    }
}

} // namespace rtengine
