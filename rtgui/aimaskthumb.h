/*
 *  This file is part of RawTherapee.
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

#include <functional>

#include <gdkmm/pixbuf.h>

namespace rtengine
{
struct PartnerClassView;
}

/** A class list is a list of questions about one picture -- where is the sky,
 *  where is the subject -- and a picture answers them better than a number
 *  can. These render a thumbnail of the frame with the class lit up in it,
 *  small enough to sit in a menu row.
 */
namespace aimaskthumb
{

/** A tile, and the two facts a list needs to decide whether to offer the class
 *  at all. "Not measured" and "measured, and there is none of it" look the
 *  same in a picture and must not be treated the same: only the second is a
 *  reason to leave the class out.
 */
struct Tile {
    Glib::RefPtr<Gdk::Pixbuf> image;
    bool measured = false;
    bool present = false;

    /// True when the picture has been looked at and holds none of this class.
    bool absent() const
    {
        return measured && !present;
    }
};

/// Per-class marker colour, indexed by rtengine::AISegClass. Out-of-range
/// indices get the neutral grey, so a caller need not range-check first.
const guint8* classColour(int classIndex);

/// @param sample  probability at normalized picture coordinates, 0..1 each
/// @param imW,imH the picture's proportions, so a portrait frame reads as one
/// @param maxW,maxH the largest tile the caller has room for
Glib::RefPtr<Gdk::Pixbuf> render(const std::function<float(double, double)>& sample,
                                 int classIndex, float threshold,
                                 int imW, int imH, int maxW, int maxH);

#ifdef RT_AI_MASKING
/// The same, reading the edited picture's own cached segmentation. Returns an
/// empty pointer when that picture has not been segmented yet -- the caller
/// shows what it always showed.
Glib::RefPtr<Gdk::Pixbuf> fromEditedImage(int classIndex, float threshold,
                                          int imW, int imH, int maxW, int maxH);

/// And for a double exposure's partner, whose tiles were kept when it was
/// segmented for its coverage.
Glib::RefPtr<Gdk::Pixbuf> fromPartnerView(const rtengine::PartnerClassView& view, int classIndex,
                                          float threshold, int maxW, int maxH);
#endif

}
