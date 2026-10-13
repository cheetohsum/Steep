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
#include <string>

#include <glibmm/ustring.h>
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
    // Standing preference: drop rejected photos from browser/filmstrip even
    // when no explicit pick filter is active. Overridden by the rejects view
    // and by explicitly filtering for rejected.
    bool hideRejects;
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

    std::set<std::string> tagFilter;
    bool tagMatchAll = false;
    bool tagUntagged = false;

    BrowserFilter ();
    bool isPassThrough () const;
};

// The toggles both filter bars share, carried across a view switch so
// the browser and the editor filmstrip always filter alike.
struct FilterToggles {
    bool unranked = false;
    bool rank[5] = {false, false, false, false, false};
    int rankAnchor = 0;
    bool rankExact = false;
    bool unlabeled = false;
    bool label[5] = {false, false, false, false, false};
    bool edited[2] = {false, false};
    bool saved[2] = {false, false};
    bool picked = false;
    bool unflagged = false;
    bool rejected = false;
    Glib::ustring search;
    std::set<std::string> tags; // PhotoTags::Tags
    bool tagsAll = false;
    bool untagged = false;
};
