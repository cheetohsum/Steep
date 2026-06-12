/*
 *  This file is part of RawTherapee.
 *
 *  RawTherapee is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */
#pragma once

#include <string>

void noteRawLoadForegroundActivity(const std::string& fname = std::string());
bool isRawLoadForegroundQuietForMs(int quietMs);
int rawLoadForegroundQuietRetryMs(int quietMs, int minRetryMs);
void setRawLoadEditorActivity(const std::string& fname, bool active);
