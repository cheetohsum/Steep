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

#include <array>

#include <gtkmm.h>

#include "widgets/basic/lwbuttonset.h"

class FileBrowserEntry;
class RTSurface;

class FileThumbnailButtonSet :
    public LWButtonSet
{

    static bool iconsLoaded;

public:
    static std::shared_ptr<RTSurface> rankIcon;
    static std::shared_ptr<RTSurface> gRankIcon;
    static std::shared_ptr<RTSurface> unRankIcon;
    static std::shared_ptr<RTSurface> trashIcon;
    static std::shared_ptr<RTSurface> unTrashIcon;
    static std::shared_ptr<RTSurface> processIcon;

    static std::array<std::shared_ptr<RTSurface>, 6> colorLabelIcon;
    static std::shared_ptr<RTSurface> pickIcon;
    static std::shared_ptr<RTSurface> rejectIcon;

    static Glib::ustring processToolTip;
    static Glib::ustring unrankToolTip;
    static Glib::ustring trashToolTip;
    static Glib::ustring untrashToolTip;
    static Glib::ustring colorLabelToolTip;
    static std::array<Glib::ustring, 5> rankToolTip;

    static void ensureIconsLoaded ();

    explicit FileThumbnailButtonSet (FileBrowserEntry* myEntry);
    void    arrangeButtons (int x, int y, int w, int h) override;
    void    setRank (int stars);
    void    setColorLabel (int colorlabel);

    // Returns true if buttons have meaningful content to show
    bool    shouldShow () const override { return currentRank_ > 0 || currentColorLabel_ > 0; }

private:
    int currentRank_ = 0;
    int currentColorLabel_ = 0;
};
