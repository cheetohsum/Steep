/*
 *  This file is part of RawTherapee.
 *
 *  RawTherapee is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */
#include "steepperflog.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

namespace
{

std::mutex fileBrowserPerfLogMutex;

bool fileBrowserPerfLogEnabled()
{
    static const bool enabled = std::getenv("STEEP_FILESEL_LOG") != nullptr;
    return enabled;
}

}

void fileBrowserPerfLog(const char* fmt, ...)
{
    if (!fileBrowserPerfLogEnabled()) {
        return;
    }

    std::lock_guard<std::mutex> lock(fileBrowserPerfLogMutex);
    const char* const home = std::getenv("USERPROFILE");
    const std::string path = home ? std::string(home) + "\steep-fileSel.log" : "steep-fileSel.log";

    FILE* const f = std::fopen(path.c_str(), "ab");
    if (!f) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    std::vfprintf(f, fmt, args);
    va_end(args);
    std::fclose(f);
}
