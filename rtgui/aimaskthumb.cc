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
#include "aimaskthumb.h"

#include <algorithm>
#include <cmath>

#ifdef RT_AI_MASKING
#include "rtengine/aimaskcache.h"
#include "rtengine/partnermaskstore.h"
#endif

namespace
{

// Model classes 0-7, then the composed SUBJECT and NOT_SUBJECT pseudo-classes.
// The same colours the mask list swatches use, so one class means one colour
// wherever it appears.
const guint8 CLASS_COLOURS[][3] = {
    {128, 128, 128}, {220,  80,  80}, { 80, 140, 220}, { 80, 180,  80},
    {200, 150,  60}, {160,  80, 200}, {220, 180,  50}, { 80, 200, 200},
    {240, 130,  90}, {110, 120, 150},
};

constexpr int CLASS_COUNT = static_cast<int>(sizeof(CLASS_COLOURS) / sizeof(CLASS_COLOURS[0]));

}

namespace aimaskthumb
{

const guint8* classColour(int classIndex)
{
    return CLASS_COLOURS[classIndex >= 0 && classIndex < CLASS_COUNT ? classIndex : 0];
}

Glib::RefPtr<Gdk::Pixbuf> render(const std::function<float(double, double)>& sample,
                                 int classIndex, float threshold,
                                 int imW, int imH, int maxW, int maxH)
{
    if (!sample || imW <= 0 || imH <= 0 || maxW <= 0 || maxH <= 0) {
        return Glib::RefPtr<Gdk::Pixbuf>();
    }

    // The tile takes the picture's own proportions: a fixed landscape chip
    // shows a portrait photo's mask lying on its side, which is a poor way to
    // answer "where in the frame is this?".
    const double fit = std::min(static_cast<double>(maxW) / imW, static_cast<double>(maxH) / imH);
    const int w = std::max(6, static_cast<int>(std::lround(imW * fit)));
    const int h = std::max(6, static_cast<int>(std::lround(imH * fit)));

    auto pixbuf = Gdk::Pixbuf::create(Gdk::COLORSPACE_RGB, true, 8, w, h);
    guint8* pixels = pixbuf->get_pixels();
    const int rowstride = pixbuf->get_rowstride();
    const guint8* colour = classColour(classIndex);

    for (int y = 0; y < h; ++y) {
        guint8* row = pixels + y * rowstride;
        const double v = (y + 0.5) / h;

        for (int x = 0; x < w; ++x) {
            const double u = (x + 0.5) / w;
            const float probability = sample(u, v);

            // A soft band around the threshold rather than a hard cut, so a
            // feathered selection still looks feathered at this size.
            constexpr float band = 0.21f;
            const float strength = std::max(0.f, std::min(1.f,
                                            (probability - threshold + band * 0.5f) / band));

            guint8* px = row + x * 4;

            // An unselected frame is drawn, not left transparent: an empty
            // tile has to read as "this class is not in the picture" rather
            // than as "nothing was measured".
            px[0] = static_cast<guint8>(26 + (colour[0] - 26) * strength);
            px[1] = static_cast<guint8>(28 + (colour[1] - 28) * strength);
            px[2] = static_cast<guint8>(32 + (colour[2] - 32) * strength);
            px[3] = static_cast<guint8>(150 + 105 * strength);
        }
    }

    return pixbuf;
}

#ifdef RT_AI_MASKING
Glib::RefPtr<Gdk::Pixbuf> fromEditedImage(int classIndex, float threshold,
                                          int imW, int imH, int maxW, int maxH)
{
    const rtengine::AIMaskSnapshot snapshot = rtengine::AIMaskCache::getInstance().getMaskSnapshot(
                static_cast<rtengine::AISegClass>(classIndex));

    if (!snapshot || snapshot.width <= 0 || snapshot.height <= 0) {
        return Glib::RefPtr<Gdk::Pixbuf>();
    }

    const auto& mask = *snapshot.mask;
    const int mw = snapshot.width;
    const int mh = snapshot.height;

    return render([&mask, mw, mh](double u, double v) {
        const int x = std::min(mw - 1, std::max(0, static_cast<int>(u * mw)));
        const int y = std::min(mh - 1, std::max(0, static_cast<int>(v * mh)));
        return mask[y][x];
    }, classIndex, threshold, imW, imH, maxW, maxH);
}

Glib::RefPtr<Gdk::Pixbuf> fromPartnerView(const rtengine::PartnerClassView& view, int classIndex,
                                          float threshold, int maxW, int maxH)
{
    if (classIndex < 0 || classIndex >= static_cast<int>(view.thumbs.size())
            || view.thumbWidth <= 0 || view.thumbHeight <= 0) {
        return Glib::RefPtr<Gdk::Pixbuf>();
    }

    const std::vector<unsigned char>& tile = view.thumbs[classIndex];

    if (tile.size() != static_cast<size_t>(view.thumbWidth) * view.thumbHeight) {
        return Glib::RefPtr<Gdk::Pixbuf>();
    }

    const int w = view.thumbWidth;
    const int h = view.thumbHeight;

    return render([&tile, w, h](double u, double v) {
        const int x = std::min(w - 1, std::max(0, static_cast<int>(u * w)));
        const int y = std::min(h - 1, std::max(0, static_cast<int>(v * h)));
        return tile[static_cast<size_t>(y) * w + x] / 255.f;
    }, classIndex, threshold, w, h, maxW, maxH);
}
#endif

}
