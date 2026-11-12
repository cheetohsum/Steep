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
#include "myfile.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <cstdarg>
#include <cstring>
#include <cstdio>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "rtengine.h"
#ifdef _WIN32
#include "rtengine/leanwindows.h"
#include <winioctl.h>
#endif
// get mmap() sorted out
#ifdef MYFILE_MMAP

#ifdef _WIN32

#include "rtengine/leanwindows.h"
#include <fcntl.h>

// dummy values
#define MAP_PRIVATE 1
#define PROT_READ 1
// Windows only, see mmap() below.
#define RT_PROT_COPY_ON_WRITE 2
#define MAP_FAILED (void *)-1

#ifdef __GNUC__ // silence warning
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

void* mmap(void *start, size_t length, int prot, int flags, int fd, off_t offset)
{
    // A copy-on-write view is charged against the commit limit for its whole
    // length the moment it is mapped, written to or not: every open RAW cost
    // its file size in commit. Readers get a plain read-only view, which
    // costs none.
    const bool copyOnWrite = prot & RT_PROT_COPY_ON_WRITE;
    HANDLE handle = CreateFileMapping((HANDLE)_get_osfhandle(fd), NULL, copyOnWrite ? PAGE_WRITECOPY : PAGE_READONLY, 0, 0, NULL);

    if (handle != NULL) {
        start = MapViewOfFile(handle, copyOnWrite ? FILE_MAP_COPY : FILE_MAP_READ, 0, offset, length);
        CloseHandle(handle);
        return start;
    }

    return MAP_FAILED;
}

int munmap(void *start, size_t length)
{
    UnmapViewOfFile(start);
    return 0;
}
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

#else // _WIN32

#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#endif // _WIN32
#endif // MYFILE_MMAP

#ifdef MYFILE_MMAP

rtengine::IMFILE* rtengine::fopen (const char* fname)
{
    return fopen(fname, false);
}

rtengine::IMFILE* rtengine::fopen (const char* fname, bool randomAccess)
{
    int fd;

#ifdef _WIN32

    fd = -1;
    // First convert UTF8 to UTF16, then use Windows function to open the file and convert back to file descriptor.
    std::unique_ptr<wchar_t, GFreeFunc> wfname (reinterpret_cast<wchar_t*>(g_utf8_to_utf16 (fname, -1, NULL, NULL, NULL)), g_free);

    HANDLE hFile = CreateFileW (wfname.get (), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                                randomAccess ? FILE_FLAG_RANDOM_ACCESS : FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        fd = _open_osfhandle((intptr_t)hFile, 0);
    }

#else

    fd = ::g_open (fname, O_RDONLY);

#endif

    if ( fd < 0 ) {
        return nullptr;
    }

    struct stat stat_buffer;

    if ( fstat(fd, &stat_buffer) < 0 ) {
        printf("no stat\n");
        close (fd);
        return nullptr;
    }

    if (stat_buffer.st_size <= 0) {
        close(fd);
        return nullptr;
    }

    int prot = PROT_READ;
#ifdef _WIN32
    {
        // The one writer through the mapping: fuji_fill_buffer() in
        // fujicompressed.cc zero-fills a byte at its read position when a
        // compressed strip runs past its declared size. Fuji files keep the
        // copy-on-write view until that pads into a buffer of its own.
        const std::size_t length = std::strlen(fname);
        if (length >= 4 && !g_ascii_strcasecmp(fname + length - 4, ".raf")) {
            prot |= RT_PROT_COPY_ON_WRITE;
        }
    }
#endif

    void* data = mmap(nullptr, stat_buffer.st_size, prot, MAP_PRIVATE, fd, 0);

    if ( data == MAP_FAILED ) {
        printf("no mmap %s\n", fname);
        close(fd);
        return nullptr;
    }

    IMFILE* mf = new IMFILE;

    memset(mf, 0, sizeof(*mf));
    mf->fd = fd;
    mf->pos = 0;
    mf->size = stat_buffer.st_size;
    mf->data = (char*)data;
    mf->eof = false;

    return mf;
}

rtengine::IMFILE* rtengine::gfopen (const char* fname)
{
    return fopen(fname, false);
}

rtengine::IMFILE* rtengine::gfopen (const char* fname, bool randomAccess)
{
    return fopen(fname, randomAccess);
}

namespace
{

// One stream at a time: the point is to hand the disk a sequential run per
// file instead of interleaved clusters from every worker.
std::mutex& prefetchMutex()
{
    static std::mutex instance;
    return instance;
}

}

namespace
{

// A RAW's embedded preview is read twice while its thumbnail is generated:
// once by the metadata parser and once by the preview decoder, microseconds
// apart on two threads. On a rotational disk that doubles the cost of a cold
// folder load, so hold the last few ranges in memory and serve the second
// reader from there. Deliberately tiny and short-lived: entries survive only
// until a handful of other files push them out.
class RangeCache
{
public:
    static RangeCache& get()
    {
        static RangeCache instance;
        return instance;
    }

    bool take(const std::string& key, std::vector<unsigned char>& out)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = entries_.find(key);

        if (it == entries_.end()) {
            return false;
        }

        out = std::move(it->second);
        bytes_ -= out.size();
        entries_.erase(it);
        order_.remove(key);
        return true;
    }

    void put(const std::string& key, const std::vector<unsigned char>& data)
    {
        if (data.size() < kMinBytes || data.size() > kMaxBytes) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        if (entries_.count(key)) {
            return;
        }

        while (!order_.empty() && (bytes_ + data.size() > kMaxBytes || entries_.size() >= kMaxEntries)) {
            const auto oldest = order_.front();
            order_.pop_front();
            const auto it = entries_.find(oldest);

            if (it != entries_.end()) {
                bytes_ -= it->second.size();
                entries_.erase(it);
            }
        }

        bytes_ += data.size();
        entries_.emplace(key, data);
        order_.push_back(key);
    }

private:
    static constexpr size_t kMinBytes = 256 * 1024;
    static constexpr size_t kMaxBytes = 96u * 1024 * 1024;
    static constexpr size_t kMaxEntries = 16;

    std::mutex mutex_;
    std::map<std::string, std::vector<unsigned char>> entries_;
    std::list<std::string> order_;
    size_t bytes_ = 0;
};

std::string rangeKey(const char* fname, ssize_t offset, ssize_t length)
{
    return std::string(fname) + '|' + std::to_string(offset) + '|' + std::to_string(length);
}

#ifdef _WIN32
// 1 = rotational (or unknown: network, removable, query failed), 0 = no seek
// penalty (SSD/NVMe). Queried once per drive letter.
bool driveIncursSeekPenalty(wchar_t letter)
{
    static std::mutex cacheMutex;
    static std::map<wchar_t, bool> cache;

    std::lock_guard<std::mutex> lock(cacheMutex);
    const auto it = cache.find(letter);

    if (it != cache.end()) {
        return it->second;
    }

    bool seekPenalty = true;
    const wchar_t root[] = { letter, L':', L'\\', 0 };

    if (GetDriveTypeW(root) == DRIVE_FIXED) {
        const wchar_t volume[] = { L'\\', L'\\', L'.', L'\\', letter, L':', 0 };
        HANDLE h = CreateFileW(volume, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);

        if (h != INVALID_HANDLE_VALUE) {
            STORAGE_PROPERTY_QUERY query;
            memset(&query, 0, sizeof(query));
            query.PropertyId = StorageDeviceSeekPenaltyProperty;
            query.QueryType = PropertyStandardQuery;
            DEVICE_SEEK_PENALTY_DESCRIPTOR desc;
            memset(&desc, 0, sizeof(desc));
            DWORD returned = 0;

            if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &desc, sizeof(desc), &returned, NULL)
                    && returned >= sizeof(desc)) {
                seekPenalty = desc.IncursSeekPenalty;
            }

            CloseHandle(h);
        }
    }

    cache.emplace(letter, seekPenalty);
    return seekPenalty;
}
#endif

// The lock a preview read runs under. A spinning disk gets one stream at a
// time per drive (sequential runs instead of interleaved seeks — the reason
// this lock exists). A drive without seek penalty gets a lock per file:
// different files read in parallel, while the two readers of the same RAW
// preview still serialize so the second is served from RangeCache.
std::mutex& readLockFor(const char* fname)
{
    static std::mutex perDrive[27];
    static std::mutex perFile[32];

#ifdef _WIN32
    const char c = fname[0];
    const bool hasLetter = ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) && fname[1] == ':';

    if (hasLetter) {
        const int index = (c | 0x20) - 'a';

        if (!driveIncursSeekPenalty(static_cast<wchar_t>(L'A' + index))) {
            return perFile[std::hash<std::string>()(fname) % 32];
        }

        return perDrive[index];
    }

    return perDrive[26];
#else
    (void)perFile;
    (void)fname;
    return perDrive[26];
#endif
}

}

bool rtengine::readFileRange (const char* fname, ssize_t offset, ssize_t length, std::vector<unsigned char>& out)
{
    out.clear();

    if (!fname || offset < 0 || length <= 0) {
        return false;
    }

    const std::string key = rangeKey(fname, offset, length);

    // Whoever asks second gets the bytes without touching the disk. The entry
    // is removed on read: exactly two consumers per file, and holding it any
    // longer would just waste memory during a folder load.
    static const bool trace = g_getenv("RT_RANGE_TRACE") != nullptr;

    const auto served = [&](const char* how) {
        if (trace) {
            std::fprintf(stderr, "RT_RANGE %s off=%lld len=%lld file=%s\n",
                how, (long long)offset, (long long)length, fname);
        }
    };

    if (RangeCache::get().take(key, out)) {
        served("hit ");
        return !out.empty();
    }

    // The two readers of a RAW preview start at the same moment on different
    // threads, so an unlocked lookup misses in both and the range gets read
    // twice. Re-check once this thread owns the read lock: by then the other
    // one has finished and deposited its bytes.
    std::lock_guard<std::mutex> lock(readLockFor(fname));

    if (RangeCache::get().take(key, out)) {
        served("hit2");
        return !out.empty();
    }

    served("miss");
    out.resize(static_cast<size_t>(length));
    ssize_t done = 0;

#ifdef _WIN32
    std::unique_ptr<wchar_t, GFreeFunc> wfname (reinterpret_cast<wchar_t*>(g_utf8_to_utf16 (fname, -1, NULL, NULL, NULL)), g_free);
    HANDLE hFile = wfname
        ? CreateFileW (wfname.get (), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)
        : INVALID_HANDLE_VALUE;

    if (hFile == INVALID_HANDLE_VALUE) {
        out.clear();
        return false;
    }

    while (done < length) {
        const DWORD want = static_cast<DWORD>(std::min<ssize_t>(4 * 1024 * 1024, length - done));
        OVERLAPPED ov;
        memset(&ov, 0, sizeof(ov));
        const ULONGLONG at = static_cast<ULONGLONG>(offset + done);
        ov.Offset = static_cast<DWORD>(at & 0xffffffffu);
        ov.OffsetHigh = static_cast<DWORD>(at >> 32);
        DWORD got = 0;

        if (!ReadFile(hFile, out.data() + done, want, &got, &ov) || got == 0) {
            break;
        }

        done += got;
    }

    CloseHandle(hFile);
#else
    const int fd = ::g_open (fname, O_RDONLY);

    if (fd < 0) {
        out.clear();
        return false;
    }

    while (done < length) {
        const ssize_t got = ::pread(fd, out.data() + done, std::min<ssize_t>(4 * 1024 * 1024, length - done), offset + done);

        if (got <= 0) {
            break;
        }

        done += got;
    }

    close(fd);
#endif

    out.resize(static_cast<size_t>(done));

    if (done == length) {
        RangeCache::get().put(key, out);
    }

    return done > 0;
}

bool rtengine::fileIncursSeekPenalty (const char* fname)
{
#ifdef _WIN32
    if (!fname) {
        return true;
    }

    const char c = fname[0];

    if (((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) && fname[1] == ':') {
        return driveIncursSeekPenalty(static_cast<wchar_t>(L'A' + ((c | 0x20) - 'a')));
    }

    return true;
#else
    (void)fname;
    return false;
#endif
}

namespace
{

// Reads [0, bytes) of `fname` (the whole file when bytes < 0) and throws the
// data away, leaving it in the OS cache. Each 4 MB chunk is read under the
// drive's read lock, so on a spinning disk this interleaves with preview
// reads at chunk granularity instead of page-fault granularity, and other
// readers are never held off for a whole file. False when cancelled.
bool readSequentially(const char* fname, ssize_t bytes, const std::function<bool()>& cancelled)
{
    if (!fname || bytes == 0) {
        return true;
    }

    constexpr ssize_t chunk = 4 * 1024 * 1024;
    std::vector<char> buffer(static_cast<size_t>(bytes > 0 ? std::min(bytes, chunk) : chunk));
    bool finished = true;

#ifdef _WIN32
    std::unique_ptr<wchar_t, GFreeFunc> wfname (reinterpret_cast<wchar_t*>(g_utf8_to_utf16 (fname, -1, NULL, NULL, NULL)), g_free);

    if (!wfname) {
        return true;
    }

    HANDLE hFile = CreateFileW (wfname.get (), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    if (hFile == INVALID_HANDLE_VALUE) {
        return true;
    }

    for (ssize_t done = 0; bytes < 0 || done < bytes;) {
        if (cancelled && cancelled()) {
            finished = false;
            break;
        }

        const ssize_t want = bytes < 0 ? chunk : std::min(chunk, bytes - done);
        DWORD got = 0;
        bool ok;
        {
            std::lock_guard<std::mutex> lock(readLockFor(fname));
            ok = ReadFile(hFile, buffer.data(), static_cast<DWORD>(want), &got, NULL);
        }

        if (!ok || got == 0) {
            break;
        }

        done += got;
    }

    CloseHandle(hFile);
#else
    const int fd = ::g_open (fname, O_RDONLY);

    if (fd < 0) {
        return true;
    }

    for (ssize_t done = 0; bytes < 0 || done < bytes;) {
        if (cancelled && cancelled()) {
            finished = false;
            break;
        }

        const ssize_t want = bytes < 0 ? chunk : std::min(chunk, bytes - done);
        ssize_t got;
        {
            std::lock_guard<std::mutex> lock(readLockFor(fname));
            got = ::read(fd, buffer.data(), want);
        }

        if (got <= 0) {
            break;
        }

        done += got;
    }

    close(fd);
#endif
    return finished;
}

// One low-priority thread that pulls the files the user is likely to open
// next into the OS cache. A new request replaces the old one, and the file
// being read is abandoned at the next chunk boundary.
class CacheWarmer
{
public:
    static CacheWarmer& get()
    {
        // Leaked on purpose: the thread may still be parked at exit.
        static CacheWarmer* instance = new CacheWarmer();
        return *instance;
    }

    void request(std::vector<std::string> files, std::function<bool()> ready)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);

            // The same neighbourhood is often requested several times per
            // open; restarting would throw away the read in progress.
            std::vector<std::string> remaining;
            if (!inFlight_.empty()) {
                remaining.push_back(inFlight_);
            }
            remaining.insert(remaining.end(), files_.begin(), files_.end());

            if (remaining == files) {
                ready_ = std::move(ready);
                return;
            }

            files_ = std::move(files);
            ready_ = std::move(ready);
            ++generation_;

            static const bool trace = g_getenv("RT_RANGE_TRACE") != nullptr;
            if (trace) {
                std::fprintf(stderr, "RT_WARM t=%lld request n=%d first=%s\n",
                    (long long)(g_get_monotonic_time() / 1000), (int)files_.size(),
                    files_.empty() ? "-" : files_.front().c_str());
            }

            if (!started_) {
                started_ = true;
                std::thread(&CacheWarmer::run, this).detach();
            }
        }
        cv_.notify_all();
    }

private:
    void run()
    {
#ifdef _WIN32
        // Not THREAD_MODE_BACKGROUND_BEGIN: besides I/O priority it lowers
        // the memory priority of every page this thread reads, and those are
        // the first the editor's allocations evict -- warming for nothing.
        // Yielding to foreground loads is the ready() callback's job.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
        std::unique_lock<std::mutex> lock(mutex_);

        while (true) {
            cv_.wait(lock, [this] { return !files_.empty(); });

            const unsigned generation = generation_;
            const auto stale = [this, generation] { return generation_.load() != generation; };

            // Wait for the foreground to go quiet; a newer request restarts.
            while (!stale() && ready_ && !ready_()) {
                cv_.wait_for(lock, std::chrono::milliseconds(100));
            }

            if (stale() || files_.empty()) {
                continue;
            }

            const std::string file = files_.front();
            files_.erase(files_.begin());
            inFlight_ = file;
            lock.unlock();
            static const bool trace = g_getenv("RT_RANGE_TRACE") != nullptr;
            const auto start = std::chrono::steady_clock::now();
            const bool finished = readSequentially(file.c_str(), -1, [&]() {
                return stale() || !readyUnlocked();
            });
            if (trace) {
                std::fprintf(stderr, "RT_WARM t=%lld %s ms=%lld file=%s\n",
                    (long long)(g_get_monotonic_time() / 1000), finished ? "done" : "stop",
                    (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count(),
                    file.c_str());
            }
            lock.lock();
            inFlight_.clear();

            // Interrupted by foreground work rather than a new request: pick
            // it up again later; the chunks already read are cached.
            if (!finished && !stale()) {
                files_.insert(files_.begin(), file);
            }
        }
    }

    // ready_ is only replaced together with a generation bump, after which
    // the reader stops at its next chunk anyway; copying it under the lock
    // keeps the callable alive while it runs.
    bool readyUnlocked()
    {
        std::function<bool()> ready;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ready = ready_;
        }
        return !ready || ready();
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::string> files_;
    std::function<bool()> ready_;
    std::string inFlight_;
    std::atomic<unsigned> generation_{0};
    bool started_ = false;
};

}

void rtengine::prefetchFileHead (const char* fname, ssize_t bytes)
{
    if (bytes > 0) {
        readSequentially(fname, bytes, {});
    }
}

bool rtengine::prefetchFile (const char* fname, const std::function<bool()>& cancelled)
{
    return readSequentially(fname, -1, cancelled);
}

void rtengine::warmFileCache (std::vector<std::string> fnames, std::function<bool()> ready)
{
    CacheWarmer::get().request(std::move(fnames), std::move(ready));
}

void rtengine::fprefetch (IMFILE* f, ssize_t offset, ssize_t length)
{
    if (!f || f->fd < 0 || !f->data || offset < 0 || length <= 0 || offset >= f->size) {
        return;
    }

    length = std::min<ssize_t>(length, f->size - offset);

    std::lock_guard<std::mutex> lock(prefetchMutex());

    constexpr ssize_t chunk = 4 * 1024 * 1024;
    std::vector<char> buffer(static_cast<size_t>(std::min<ssize_t>(chunk, length)));

#ifdef _WIN32
    HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(f->fd));

    if (handle == INVALID_HANDLE_VALUE) {
        return;
    }

    for (ssize_t done = 0; done < length;) {
        const DWORD want = static_cast<DWORD>(std::min<ssize_t>(chunk, length - done));
        OVERLAPPED ov;
        memset(&ov, 0, sizeof(ov));
        const ULONGLONG at = static_cast<ULONGLONG>(offset + done);
        ov.Offset = static_cast<DWORD>(at & 0xffffffffu);
        ov.OffsetHigh = static_cast<DWORD>(at >> 32);
        DWORD got = 0;

        if (!ReadFile(handle, buffer.data(), want, &got, &ov) || got == 0) {
            break;
        }

        done += got;
    }
#else
    for (ssize_t done = 0; done < length;) {
        const ssize_t got = ::pread(f->fd, buffer.data(), std::min<ssize_t>(chunk, length - done), offset + done);

        if (got <= 0) {
            break;
        }

        done += got;
    }
#endif
}
#else

void rtengine::fprefetch (IMFILE*, ssize_t, ssize_t)
{
}

void rtengine::prefetchFileHead (const char*, ssize_t)
{
}

bool rtengine::prefetchFile (const char*, const std::function<bool()>&)
{
    return true;
}

bool rtengine::fileIncursSeekPenalty (const char*)
{
    return false;
}

void rtengine::warmFileCache (std::vector<std::string>, std::function<bool()>)
{
}

bool rtengine::readFileRange (const char* fname, ssize_t offset, ssize_t length, std::vector<unsigned char>& out)
{
    out.clear();

    if (!fname || offset < 0 || length <= 0) {
        return false;
    }

    FILE* f = g_fopen(fname, "rb");

    if (!f) {
        return false;
    }

    out.resize(static_cast<size_t>(length));
    size_t got = 0;

    if (fseeko(f, offset, SEEK_SET) == 0) {
        got = std::fread(out.data(), 1, out.size(), f);
    }

    std::fclose(f);
    out.resize(got);
    return got > 0;
}

rtengine::IMFILE* rtengine::fopen (const char* fname, bool)
{
    return fopen(fname);
}

rtengine::IMFILE* rtengine::fopen (const char* fname)
{

    FILE* f = g_fopen (fname, "rb");

    if (!f) {
        return NULL;
    }

    IMFILE* mf = new IMFILE;
    memset(mf, 0, sizeof(*mf));
    fseek (f, 0, SEEK_END);
    mf->size = ftell (f);
    mf->data = new char [mf->size];
    fseek (f, 0, SEEK_SET);
    fread (mf->data, 1, mf->size, f);
    fclose (f);
    mf->pos = 0;
    mf->eof = false;

    return mf;
}

rtengine::IMFILE* rtengine::gfopen (const char* fname, bool)
{
    return gfopen(fname);
}

rtengine::IMFILE* rtengine::gfopen (const char* fname)
{

    FILE* f = g_fopen (fname, "rb");

    if (!f) {
        return NULL;
    }

    IMFILE* mf = new IMFILE;
    memset(mf, 0, sizeof(*mf));
    fseek (f, 0, SEEK_END);
    mf->size = ftell (f);
    mf->data = new char [mf->size];
    fseek (f, 0, SEEK_SET);
    fread (mf->data, 1, mf->size, f);
    fclose (f);
    mf->pos = 0;
    mf->eof = false;

    return mf;
}
#endif //MYFILE_MMAP

rtengine::IMFILE* rtengine::fopen (unsigned* buf, int size)
{

    IMFILE* mf = new IMFILE;
    memset(mf, 0, sizeof(*mf));
    mf->fd = -1;
    mf->size = size;
    mf->data = new char [mf->size];
    memcpy ((void*)mf->data, buf, size);
    mf->pos = 0;
    mf->eof = false;
    return mf;
}

void rtengine::fclose (IMFILE* f)
{
#ifdef MYFILE_MMAP

    if ( f->fd == -1 ) {
        delete [] f->data;
    } else {
        munmap((void*)f->data, f->size);
        close(f->fd);
    }

#else
    delete [] f->data;
#endif
    delete f;
}

int rtengine::fscanf (IMFILE* f, const char* s ...)
{
    // fscanf not easily wrapped since we have no terminating \0 at end
    // of file data and vsscanf() won't tell us how many characters that
    // were parsed. However, only dcraw.cc code use it and only for "%f" and
    // "%d", so we make a dummy fscanf here just to support dcraw case.
    char buf[51], *endptr = nullptr;
    int copy_sz = f->size - f->pos;

    if (copy_sz >= static_cast<int>(sizeof(buf))) {
        copy_sz = sizeof(buf) - 1;
    }

    memcpy(buf, &f->data[f->pos], copy_sz);
    buf[copy_sz] = '\0';
    va_list ap;
    va_start (ap, s);

    if (strcmp(s, "%d") == 0) {
        int i = strtol(buf, &endptr, 10);

        if (endptr == buf) {
            va_end (ap);
            return 0;
        }

        int *pi = va_arg(ap, int*);
        *pi = i;
    } else if (strcmp(s, "%f") == 0) {
        float f = strtof(buf, &endptr);

        if (endptr == buf) {
            va_end (ap);
            return 0;
        }

        float *pf = va_arg(ap, float*);
        *pf = f;
    }

    va_end (ap);
    f->pos += endptr - buf;
    return 1;
}


char* rtengine::fgets (char* s, int n, IMFILE* f)
{

    if (f->pos >= f->size) {
        f->eof = true;
        return nullptr;
    }

    int i = 0;

    do {
        s[i++] = f->data[f->pos++];
    } while (i < n && f->pos < f->size);

    return s;
}

void rtengine::imfile_set_plistener(IMFILE *f, rtengine::ProgressListener *plistener, double progress_range)
{
    f->plistener = plistener;
    f->progress_range = progress_range;
    f->progress_next = f->size / 10 + 1;
    f->progress_current = 0;
}

void rtengine::imfile_update_progress(IMFILE *f)
{
    if (!f->plistener || f->progress_current < f->progress_next) {
        return;
    }

    do {
        f->progress_next += f->size / 10 + 1;
    } while (f->progress_next < f->progress_current);

    double p = (double)f->progress_current / f->size;

    if (p > 1.0) {
        /* this can happen if same bytes are read over and over again. Progress bar is not intended
           to be exact, just give some progress indication for normal raw file access patterns */
        p = 1.0;
    }

    f->plistener->setProgress(p * f->progress_range);
}
