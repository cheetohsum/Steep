/*
 *  This file is part of RawTherapee.
 *
 *  RawTherapee is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */
#pragma once

// Diagnostic trace shared by the file browser and the Auto Edit pipeline.
// Enabled by setting STEEP_FILESEL_LOG; writes to %USERPROFILE%\steep-fileSel.log.
// Both live behind one sink so an Auto Edit trace and the browser timings it
// came from interleave in a single file, which is how they get read.
// No printf format attribute: the existing call sites use C99 %zu/%lld, which
// MinGW's default -Wformat checks against msvcrt semantics and rejects.
void fileBrowserPerfLog(const char* fmt, ...);
