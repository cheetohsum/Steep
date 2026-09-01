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

/**
 * Fetches the AI inpainting model on demand.
 *
 * The model is around 200 MB, which is why it is not simply shipped: baking it
 * into every download would take the AppImage from 146 MB to roughly 340 MB for
 * a feature many people will not touch. So builds may be produced without it,
 * and this offers to fetch it the first time somebody wants Remove Object.
 *
 * The transfer is handed to the platform's own curl rather than done in
 * process. Doing it in process would mean HTTPS, and the vendored httplib is
 * built without TLS while neither the AppImage nor the macOS bundle ships
 * glib-networking -- so the in-process route means adding a TLS stack to three
 * bundles and hoping it works inside each. curl is part of macOS, part of
 * Windows since 10/1803, and present on essentially every desktop Linux. Where
 * it is genuinely absent the dialog says so and gives the manual path.
 */
class ModelDownloader
{
public:
    /// True when this build knows a URL to fetch from. Nothing is offered
    /// otherwise; a build with no URL simply does without the feature.
    static bool isConfigured();

    /// Where the model is looked for and downloaded to. Inside the user's own
    /// settings directory, because the install tree is read-only for a
    /// system-wide install. Mirrors the search order in rtengine/init.cc.
    static Glib::ustring modelPath();

    /// True once the file is on disk at modelPath().
    static bool isPresent();

    /**
     * Run the download behind a modal progress dialog.
     *
     * Returns true only when the model finished downloading AND the engine
     * accepted it, so the caller can enable the tools straight away with no
     * restart. Reports its own errors.
     */
    static bool runDialog(Gtk::Window& parent);
};
