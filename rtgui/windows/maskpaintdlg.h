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
#include <memory>
#include <vector>

#include <gtkmm.h>

#include "rtengine/maskpaint.h"

class MaskPaintCanvas;

namespace rtengine
{
class PartnerMask;
}

// Hand-correcting an automatic mask. A segmentation is a good guess and no
// more: this is where the user says "not that bit" and "this bit too", with a
// brush, over the picture the mask was found in.
//
// The dialog knows nothing about where the mask came from — the caller hands
// it a picture and the automatic mask that goes with it, both covering the
// same frame, and gets strokes back. That is what lets the same editor serve
// the double exposure tool and the masking tab.
class MaskPaintDlg final : public Gtk::Dialog
{
public:
    // The automatic mask underneath the strokes: `values` holds width*height
    // samples in 0..1, row by row, over the whole frame. May be empty, in
    // which case the strokes are shown over the picture alone.
    struct AutoMask {
        std::vector<float> values;
        int width = 0;
        int height = 0;

        bool valid() const
        {
            return width > 0 && height > 0
                   && values.size() == static_cast<size_t>(width) * height;
        }
    };

    MaskPaintDlg(Gtk::Window* parent,
                 const Glib::ustring& title,
                 const Glib::RefPtr<Gdk::Pixbuf>& picture,
                 const AutoMask& automatic,
                 const rtengine::MaskPaint& initial);
    ~MaskPaintDlg() override;

    rtengine::MaskPaint getResult() const;

private:
    void onBrushChanged();
    void onStrokesChanged();
    void undo();
    void clearAll();
    void updateCounts();

    MaskPaintCanvas* canvas_;
    Gtk::Scale* sizeScale_;
    Gtk::Scale* hardnessScale_;
    Gtk::Scale* strengthScale_;
    Gtk::RadioButton* includeBtn_;
    Gtk::RadioButton* excludeBtn_;
    Gtk::CheckButton* showMask_;
    Gtk::Button* undoBtn_;
    Gtk::Button* clearBtn_;
    Gtk::Label* countLabel_;
};

namespace maskpaint
{

// Opens the editor over the picture at `imagePath`, rendered in the engine's
// own framing so the strokes land where the mask does. Returns true when the
// user accepted, with `paint` updated.
bool refine(Gtk::Window* parent, const Glib::ustring& imagePath,
            const MaskPaintDlg::AutoMask& automatic, rtengine::MaskPaint& paint);

#ifdef RT_AI_MASKING
// The double exposure partner's segmented mask, ready to show underneath.
MaskPaintDlg::AutoMask fromPartner(const std::shared_ptr<const rtengine::PartnerMask>& mask);
#endif

} // namespace maskpaint
