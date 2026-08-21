/*
 *  This file is part of RawTherapee.
 *
 *  Interactive-edit latency instrumentation.
 *
 *  Every parameter change handed to an ImProcCoordinator is stamped with a
 *  monotonically increasing "edit serial" and a submit timestamp. The serial
 *  travels with the pass through the engine, so a single slider tick can be
 *  followed from the GUI thread, through the preview and detail-crop
 *  pipelines, all the way to the moment the pixels are painted.
 *
 *  Enabled at runtime only:
 *    STEEP_EDIT_TRACE=1        stage timing lines on stdout
 *    STEEP_DUMP_CROP=<dir>     dump every published frame as a PPM
 *
 *  With both disabled the hooks are a single relaxed atomic load.
 */

#pragma once

#include <atomic>

namespace rtengine
{

namespace edittrace
{

/// True when STEEP_EDIT_TRACE names a non-empty, non-"0", non-"false" value.
bool enabled();

/// STEEP_EDIT_TRACE_VERBOSE: per-widget-event lines, far higher volume.
bool verbose();

/// Microseconds on a monotonic clock, zeroed at the first call in the process.
long long nowUs();

/// stdout trace line, prefixed and flushed. No-op unless enabled().
void logf(const char* fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 1, 2)))
#endif
    ;

/**
 * Stamp a parameter change as it is handed to the engine (GUI thread).
 *
 * @return the serial assigned to this edit, or 0 when tracing is disabled.
 */
unsigned long long noteSubmit();

/**
 * Submit timestamp of @p serial in microseconds, or -1 when the serial is
 * unknown (0, tracing disabled, or evicted from the ring).
 */
long long submitUs(unsigned long long serial);

/// Serial → milliseconds since it was submitted, or -1 if unknown.
double ageMs(unsigned long long serial);

/**
 * Publish the serial whose pixels are being handed to the GUI. The GUI side
 * reads it back with lastPublishedSerial() to close the loop, which is race
 * free enough for tracing: a single engine thread publishes.
 */
void notePublish(unsigned long long serial, unsigned long long newestSerial = 0);
unsigned long long lastPublishedSerial();
unsigned long long lastPublishedNewestSerial();

/// Directory named by STEEP_DUMP_CROP, or nullptr when unset.
const char* dumpDir();

/**
 * Append one RGB8 frame to the dump directory as <tag>-NNNN.ppm. Sequence
 * numbers are per-tag and start at 0. No-op when dumpDir() is null.
 */
void dumpFrame(const char* tag, const unsigned char* rgb, int width, int height, int rowStride);

}

}
