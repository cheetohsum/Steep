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

#include <set>
#include <unordered_set>
#include <vector>

#include "exiffiltersettings.h"

class BrowserFilter
{
public:
    bool showRanked[6];
    bool showCLabeled[6];
    bool showPicked;
    bool showRejected;
    bool showUnflagged;
    bool showTrash;
    bool showNotTrash;
    bool showOriginal;
    bool showEdited[2];
    bool showRecentlySaved[2];

    bool exifFilterEnabled;
    bool matchEqual;
    ExifFilterSettings exifFilter;
    std::vector<std::string> vFilterStrings;
    std::unordered_set<std::string> albumWhitelist;

    // Filetype filter: if non-empty, only show entries whose uppercase
    // extension is in this set.  Empty = show all types.
    std::set<std::string> filetypeFilter;

    BrowserFilter ();
    bool isPassThrough () const;
};
