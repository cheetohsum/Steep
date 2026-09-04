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

#include "aimaskthumb.h"

#include <gtkmm.h>
#include <sigc++/sigc++.h>

#include <functional>
#include <memory>
#include <vector>

namespace steepui
{
class PopupMenu;
}

/** Mask creation chip strip ("Smart Masks").
 *
 *  One "AI Mask ▾" dropdown chip holding every segmentation class, plus a
 *  chip per normal mask shape (Ellipse, Rectangle, Gradient). Everything
 *  routes through the same creation paths as the add-mask menu. Lives in
 *  the Masking group's persistent box so it stays usable while the group
 *  is collapsed.
 *
 *  Construct only when the segmentation engine is available; emits the
 *  rtengine::AISegClass index or the ControlSpotPanel shape code.
 */
class SmartMaskBar final : public Gtk::Box
{
public:
    SmartMaskBar();
    ~SmartMaskBar() override;

    sigc::signal<void, int>& signalClassRequested()
    {
        return classRequested_;
    }

    sigc::signal<void, int>& signalShapeRequested()
    {
        return shapeRequested_;
    }

    /// Fired when the user arms click-to-select (pick a class off the photo).
    sigc::signal<void>& signalPickRequested()
    {
        return pickRequested_;
    }

    /** Tile callback: given an AISegClass index, render a thumbnail of the
     *  frame with that class lit up in it, or return an empty pointer when
     *  the picture has not been segmented yet. Queried each time the AI
     *  dropdown opens. A class list asks where things are, and a picture
     *  answers that better than a percentage does. */
    void setThumbProvider(std::function<aimaskthumb::Tile(int)> provider)
    {
        thumbProvider_ = std::move(provider);
    }

private:
    struct AIMenuEntry {
        Gtk::MenuItem* item = nullptr;
        Gtk::Image* thumb = nullptr;
        Glib::ustring baseLabel;
        int classIndex = 0;
    };

    Gtk::Button* makeChip(Gtk::FlowBox* flow, const Glib::ustring& icon,
                          const Glib::ustring& label, const Glib::ustring& tooltip);
    void refreshAIMenuThumbs();

    sigc::signal<void, int> classRequested_;
    sigc::signal<void, int> shapeRequested_;
    sigc::signal<void> pickRequested_;
    std::function<aimaskthumb::Tile(int)> thumbProvider_;
    std::vector<AIMenuEntry> aiMenuEntries_;
    std::unique_ptr<steepui::PopupMenu> aiMenu_;
};
