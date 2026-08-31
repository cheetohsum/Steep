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

#include <vector>

#include <cstdio>
#include <cstring>

#include <glib/gstdio.h>

#include "opthelper.h"

namespace rtengine
{

class ProgressListener;

struct IMFILE {
    int fd;
    ssize_t pos;
    ssize_t size;
    char* data;
    bool eof;
    rtengine::ProgressListener *plistener;
    double progress_range;
    ssize_t progress_next;
    ssize_t progress_current;
};

/*
  Functions for progress bar updates
  Note: progress bar is not intended to be exact, eg if you read same data over and over again progress
  will potentially reach 100% before you're finished.
 */
void imfile_set_plistener(IMFILE *f, rtengine::ProgressListener *plistener, double progress_range);
void imfile_update_progress(IMFILE *f);

IMFILE* fopen (const char* fname);
IMFILE* fopen (const char* fname, bool randomAccess);
// Pull [offset, offset+length) of a memory-mapped file into the page cache
// with large sequential reads, serialised across threads. Touching a mapped
// range through page faults costs one seek per 64 KB cluster whenever other
// workers interleave their own files on the same spindle; one sequential
// read per file is what a rotational disk wants. No-op for in-memory files.
void fprefetch (IMFILE* f, ssize_t offset, ssize_t length);
// Same idea before the file is even mapped: pull the first `bytes` of `fname`
// into the page cache so the header/EXIF parsers that follow (LibRaw identify,
// Exiv2 on its own handle) hit cache instead of seeking into the file.
void prefetchFileHead (const char* fname, ssize_t bytes);
// Read [offset, offset+length) of `fname` into `out` with the same
// serialisation as the prefetchers, so a parser that wants a byte range
// costs the disk one sequential run. Returns false if nothing was read.
bool readFileRange (const char* fname, ssize_t offset, ssize_t length, std::vector<unsigned char>& out);
IMFILE* gfopen (const char* fname);
// randomAccess=true opens the file without filesystem read-ahead. Use it for
// metadata-only passes that touch a few scattered structures: read-ahead
// turns those touches into megabytes of speculative disk traffic. Never use
// it for a full image decode, which reads the file front to back.
IMFILE* gfopen (const char* fname, bool randomAccess);
IMFILE* fopen (unsigned* buf, int size);
void fclose (IMFILE* f);
inline long ftell (IMFILE* f)
{
    return f->pos;
}

inline int feof (IMFILE* f)
{
    return f->eof;
}

inline void fseek (IMFILE* f, long p, int how)
{
    ssize_t fpos = f->pos;

    if (how == SEEK_SET) {
        f->pos = p;
    } else if (how == SEEK_CUR) {
        f->pos += p;
    } else if (how == SEEK_END) {
        if (p <= 0 && -p <= f->size) {
            f->pos = f->size + p;
        }
        return;
    }

    if (f->pos < 0  || f->pos > f->size) {
        f->pos = fpos;
    }
}

inline int fgetc (IMFILE* f)
{

    if (LIKELY(f->pos < f->size)) {
        if (f->plistener && ++f->progress_current >= f->progress_next) {
            imfile_update_progress(f);
        }

        return (unsigned char)f->data[f->pos++];
    }

    f->eof = true;
    return EOF;
}

inline int getc (IMFILE* f)
{

    return fgetc(f);
}

inline int fread (void* dst, size_t es, size_t count, IMFILE* f)
{

    size_t s = es * count;
    size_t avail = static_cast<size_t>(f->size) - static_cast<size_t>(f->pos);

    if (static_cast<ssize_t>(s) <= static_cast<ssize_t>(avail)) {
        memcpy (dst, f->data + f->pos, s);
        f->pos += s;

        if (f->plistener) {
            f->progress_current += s;

            if (f->progress_current >= f->progress_next) {
                imfile_update_progress(f);
            }
        }

        return count;
    } else {
        memcpy (dst, f->data + f->pos, avail);
        f->pos += avail;
        f->eof = true;
        return avail / es;
    }
}

inline unsigned char* fdata(int offset, IMFILE* f)
{
    return (unsigned char*)f->data + offset;
}

int fscanf (IMFILE* f, const char* s ...);
char* fgets (char* s, int n, IMFILE* f);

}
