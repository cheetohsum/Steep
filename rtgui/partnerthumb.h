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

#include <gtkmm.h>

#include "cachemanager.h"
#include "thumbnail.h"

#include "rtengine/procparams.h"

namespace partnerthumb
{

// Render a small identification thumbnail for `path` via the cache.
// `neutral` renders with default params (full frame, no crop/rotation/tone,
// blend highlight recovery) — the framing the double-exposure engine feeds
// on; styled renders show the user's own edit of that file.
inline Glib::RefPtr<Gdk::Pixbuf> load(const Glib::ustring& path, int height, bool neutral)
{
    Thumbnail* thm = CacheManager::getInstance()->getEntry(path);

    if (!thm) {
        return {};
    }

    Glib::RefPtr<Gdk::Pixbuf> result;
    double scale = 1.0;
    rtengine::IImage8* img = nullptr;

    if (neutral) {
        rtengine::procparams::ProcParams neutralParams;
        neutralParams.toneCurve.hrenabled = true;
        neutralParams.toneCurve.method = "Blend";
        img = thm->processThumbImage(neutralParams, height, scale);
    } else {
        img = thm->processThumbImage(height, scale);
    }

    if (img) {
        if (img->getWidth() > 0 && img->getHeight() > 0) {
            const auto pb = Gdk::Pixbuf::create_from_data(
                img->getData(), Gdk::COLORSPACE_RGB, false, 8,
                img->getWidth(), img->getHeight(), 3 * img->getWidth());
            result = pb->copy(); // detach from the IImage8 buffer before deleting it
        }

        delete img;
    }

    thm->decreaseRef();
    return result;
}

} // namespace partnerthumb
