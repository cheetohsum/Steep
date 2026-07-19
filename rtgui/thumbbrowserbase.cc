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
#include <algorithm>

#include <glibmm/ustring.h>

#include "hidpi.h"
#include "inspector.h"
#include "multilangmgr.h"
#include "options.h"
#include "rtscalable.h"
#include "thumbbrowserbase.h"
#include "thumbbrowserentrybase.h"

#include "rtengine/rt_math.h"

using namespace std;

ThumbBrowserBase::ThumbBrowserBase ()
    : location(THLOC_FILEBROWSER), inspector(nullptr), isInspectorActive(false), eventTime(0), lastClicked(nullptr), anchor(nullptr), previewHeight(App::get().options().thumbSize), numOfCols(1), lastRowHeight(0), arrangement(TB_Horizontal)
{
    lastDeviceScale = 0;
    hscrollForceHidden = false;
    inW = -1;
    inH = -1;

    hscroll.set_orientation(Gtk::ORIENTATION_HORIZONTAL);
    vscroll.set_orientation(Gtk::ORIENTATION_VERTICAL);

    setExpandAlignProperties(&internal, true, true, Gtk::ALIGN_FILL, Gtk::ALIGN_FILL);
    setExpandAlignProperties(&hscroll, true, false, Gtk::ALIGN_FILL, Gtk::ALIGN_CENTER);
    setExpandAlignProperties(&vscroll, false, true, Gtk::ALIGN_CENTER, Gtk::ALIGN_FILL);
    attach (internal, 0, 0, 1, 1);
    attach (vscroll, 1, 0, 1, 1);
    attach (hscroll, 0, 1, 1, 1);

    internal.setParent (this);

    show_all ();

    vscroll.get_adjustment()->set_lower(0);
    hscroll.get_adjustment()->set_lower(0);
    vscroll.signal_value_changed().connect( sigc::mem_fun(*this, &ThumbBrowserBase::scrollChanged) );
    hscroll.signal_value_changed().connect( sigc::mem_fun(*this, &ThumbBrowserBase::scrollChanged) );

    internal.signal_size_allocate().connect( sigc::mem_fun(*this, &ThumbBrowserBase::internalAreaResized) );
}

void ThumbBrowserBase::scrollChanged ()
{
    for (auto* entry : visibleEntries_) {
        entry->setOffset((int)(hscroll.get_value()), (int)(vscroll.get_value()));
    }

    internal.setPosition ((int)(hscroll.get_value()), (int)(vscroll.get_value()));

    if (!internal.isDirty()) {
        internal.setDirty ();
        internal.queue_draw ();
    }
}

ThumbBrowserBase::ViewportRelation ThumbBrowserBase::viewportRelation_(const ThumbBrowserEntryBase* entry, int x, int y, int w, int h) const
{
    const int entryX = entry->getX();
    const int entryY = entry->getY();
    const int entryW = entry->getEffectiveWidth();
    const int entryH = entry->getEffectiveHeight();
    const int right = x + w;
    const int bottom = y + h;

    if (arrangement == TB_Horizontal) {
        if (entryX + entryW < x) {
            return VREL_BEFORE;
        }
        if (entryX > right) {
            return VREL_AFTER;
        }
        if (entryY + entryH < y || entryY > bottom) {
            return VREL_OUTSIDE;
        }
    } else {
        if (entryY + entryH < y) {
            return VREL_BEFORE;
        }
        if (entryY > bottom) {
            return VREL_AFTER;
        }
        if (entryX + entryW < x || entryX > right) {
            return VREL_OUTSIDE;
        }
    }

    return VREL_INSIDE;
}

std::size_t ThumbBrowserBase::firstViewportCandidate_(int x, int y) const
{
    const int viewportStart = arrangement == TB_Horizontal
        ? x - getScrollOffsetX()
        : y - getScrollOffsetY();

    const auto first = std::lower_bound(
        drawableEntries_.begin(),
        drawableEntries_.end(),
        viewportStart,
        [this](const ThumbBrowserEntryBase* entry, int value)
        {
            const int entryEnd = arrangement == TB_Horizontal
                ? entry->getStartX() + entry->getEffectiveWidth()
                : entry->getStartY() + entry->getEffectiveHeight();

            return entryEnd < value;
        });

    return static_cast<std::size_t>(first - drawableEntries_.begin());
}

void ThumbBrowserBase::rebuildDrawableEntries_()
{
    drawableEntries_.clear();
    drawableEntries_.reserve(fd.size());

    for (auto* entry : fd) {
        if (entry->drawable) {
            drawableEntries_.push_back(entry);
        }
    }
}

void ThumbBrowserBase::syncEntryOffset_(ThumbBrowserEntryBase* entry)
{
    entry->setOffset((int)(hscroll.get_value()), (int)(vscroll.get_value()));
}

void ThumbBrowserBase::scroll (int direction, double deltaX, double deltaY)
{
    double delta = 0.0;
    if (abs(deltaX) > abs(deltaY)) {
        delta = deltaX;
    } else {
        delta = deltaY;
    }
    if (direction == GDK_SCROLL_SMOOTH && delta == 0.0) {
        // sometimes this case happens. To avoid scrolling the wrong direction in this case, we just do nothing
        // This is probably no longer necessary now that coef is no longer quantized to +/-1.0 but why waste CPU cycles?
        return;
    }
    //GDK_SCROLL_SMOOTH can come in as many events with small deltas, don't quantize these to +/-1.0 so trackpads work well
    double coef;
    double scroll_unit;
    if (arrangement == TB_Vertical) {
        scroll_unit = vscroll.get_adjustment()->get_step_increment();
    } else {
        scroll_unit = hscroll.get_adjustment()->get_step_increment();
    }
    if(direction == GDK_SCROLL_SMOOTH) {
#ifdef GDK_WINDOWING_QUARTZ
        scroll_unit = 1.0;
#endif
        coef = delta;
    } else if (direction == GDK_SCROLL_DOWN) {
        coef = +1.0;
    } else {
        coef = -1.0;
    }

    // GUI already acquired when here
    if (direction == GDK_SCROLL_UP || direction == GDK_SCROLL_DOWN || direction == GDK_SCROLL_SMOOTH) {
        if (arrangement == TB_Vertical) {
            double currValue = vscroll.get_value();
            double newValue = rtengine::LIM<double>(currValue + coef * scroll_unit,
                                                    vscroll.get_adjustment()->get_lower (),
                                                    vscroll.get_adjustment()->get_upper());
            if (newValue != currValue) {
                vscroll.set_value (newValue);
            }
        } else {
            double currValue = hscroll.get_value();
            double newValue = rtengine::LIM<double>(currValue + coef * scroll_unit,
                                                    hscroll.get_adjustment()->get_lower(),
                                                    hscroll.get_adjustment()->get_upper());
            if (newValue != currValue) {
                hscroll.set_value (newValue);
            }
        }
    }
}

void ThumbBrowserBase::scrollPage (int direction)
{
    // GUI already acquired when here
    // GUI already acquired when here
    if (direction == GDK_SCROLL_UP || direction == GDK_SCROLL_DOWN) {
        if (arrangement == TB_Vertical) {
            double currValue = vscroll.get_value();
            double newValue = rtengine::LIM<double>(currValue + (direction == GDK_SCROLL_DOWN ? +1 : -1) * vscroll.get_adjustment()->get_page_increment(),
                                                    vscroll.get_adjustment()->get_lower(),
                                                    vscroll.get_adjustment()->get_upper());
            if (newValue != currValue) {
                vscroll.set_value (newValue);
            }
        } else {
            double currValue = hscroll.get_value();
            double newValue = rtengine::LIM<double>(currValue + (direction == GDK_SCROLL_DOWN ? +1 : -1) * hscroll.get_adjustment()->get_page_increment(),
                                                    hscroll.get_adjustment()->get_lower(),
                                                    hscroll.get_adjustment()->get_upper());
            if (newValue != currValue) {
                hscroll.set_value (newValue);
            }
        }
    }
}

namespace
{

typedef std::vector<ThumbBrowserEntryBase*> ThumbVector;
typedef ThumbVector::iterator ThumbIterator;

inline void clearSelection (ThumbVector& selected)
{
    for (ThumbIterator thumb = selected.begin (); thumb != selected.end (); ++thumb)
        (*thumb)->selected = false;

    selected.clear ();
}

inline void addToSelection (ThumbBrowserEntryBase* entry, ThumbVector& selected)
{
    if (entry->selected || entry->filtered)
        return;

    entry->selected = true;
    selected.push_back (entry);
}

inline void removeFromSelection (const ThumbIterator& iterator, ThumbVector& selected)
{
    (*iterator)->selected = false;
    selected.erase (iterator);
}

}

void ThumbBrowserBase::selectSingle (ThumbBrowserEntryBase* clicked)
{
    clearSelection(selected);
    anchor = clicked;

    if (clicked) {
        addToSelection(clicked, selected);
    }
}

void ThumbBrowserBase::selectRange (ThumbBrowserEntryBase* clicked, bool additional)
{
    if (!anchor) {
        anchor = clicked;
        if (selected.empty()) {
            addToSelection(clicked, selected);
            return;
        }
    }

    if (!additional || !lastClicked) {
        // Extend the current range w.r.t to first selected entry.
        ThumbIterator back = std::find(fd.begin(), fd.end(), clicked);
        ThumbIterator front = anchor == clicked ? back : std::find(fd.begin(), fd.end(), anchor);

        if (front > back) {
            std::swap(front, back);
        }

        clearSelection(selected);

        for (; front <= back && front != fd.end(); ++front) {
            addToSelection(*front, selected);
        }
    } else {
        // Add an additional range w.r.t. the last clicked entry.
        ThumbIterator last = std::find(fd.begin(), fd.end(), lastClicked);
        ThumbIterator current = std::find(fd.begin(), fd.end(), clicked);

        if (last > current) {
            std::swap(last, current);
        }

        for (; last <= current && last != fd.end(); ++last) {
            addToSelection(*last, selected);
        }
    }
}

void ThumbBrowserBase::selectSet (ThumbBrowserEntryBase* clicked)
{
    const ThumbIterator iterator = std::find(selected.begin(), selected.end(), clicked);

    if (iterator != selected.end()) {
        removeFromSelection(iterator, selected);
    } else {
        addToSelection(clicked, selected);
    }
    anchor = clicked;
}

static void scrollToEntry (double& h, double& v, int iw, int ih, ThumbBrowserEntryBase* entry)
{
    const int hMin = entry->getX();
    const int hMax = hMin + entry->getEffectiveWidth() - iw;
    const int vMin = entry->getY();
    const int vMax = vMin + entry->getEffectiveHeight() - ih;

    if (hMin < 0) {
        h += hMin;
    } else if (hMax > 0) {
        h += hMax;
    }

    if (vMin < 0) {
        v += vMin;
    } else if (vMax > 0) {
        v += vMax;
    }
}

void ThumbBrowserBase::selectPrev (int distance, bool enlarge)
{
    double h, v;
    getScrollPosition (h, v);

    {
        MYWRITERLOCK(l, entryRW);

        if (!selected.empty ()) {
            std::vector<ThumbBrowserEntryBase*>::iterator front = std::find (fd.begin (), fd.end (), selected.front ());
            std::vector<ThumbBrowserEntryBase*>::iterator back = std::find (fd.begin (), fd.end (), selected.back ());
            std::vector<ThumbBrowserEntryBase*>::iterator last = std::find (fd.begin (), fd.end (), lastClicked);

            if (front > back) {
                std::swap(front, back);
            }

            std::vector<ThumbBrowserEntryBase*>::iterator& curr = last == front ? front : back;

            // find next thumbnail at filtered distance before current
            for (; curr >= fd.begin (); --curr) {
                if (!(*curr)->filtered) {
                    if (distance-- == 0) {
                        // clear current selection
                        for (size_t i = 0; i < selected.size (); ++i) {
                            selected[i]->selected = false;
                            redrawNeeded (selected[i]);
                        }

                        selected.clear ();

                        // make sure the newly selected thumbnail is visible and make it current
                        scrollToEntry (h, v, internal.get_width (), internal.get_height (), *curr);
                        lastClicked = *curr;

                        // either enlarge current selection or set new selection
                        if(enlarge) {
                            // reverse direction if distance is too large
                            if(front > back) {
                                std::swap(front, back);
                            }

                            for (; front <= back; ++front) {
                                if (!(*front)->filtered) {
                                    (*front)->selected = true;
                                    redrawNeeded (*front);
                                    selected.push_back (*front);
                                }
                            }
                        } else {
                            (*curr)->selected = true;
                            redrawNeeded (*curr);
                            selected.push_back (*curr);
                        }

                        break;
                    }
                }
            }
        }

        MYWRITERLOCK_RELEASE(l);
        selectionChanged ();
    }

    setScrollPosition (h, v);
}

void ThumbBrowserBase::selectNext (int distance, bool enlarge)
{
    double h, v;
    getScrollPosition (h, v);

    {
        MYWRITERLOCK(l, entryRW);

        if (!selected.empty ()) {
            std::vector<ThumbBrowserEntryBase*>::iterator front = std::find (fd.begin (), fd.end (), selected.front ());
            std::vector<ThumbBrowserEntryBase*>::iterator back = std::find (fd.begin (), fd.end (), selected.back ());
            std::vector<ThumbBrowserEntryBase*>::iterator last = std::find (fd.begin (), fd.end (), lastClicked);

            if (front > back) {
                std::swap(front, back);
            }

            std::vector<ThumbBrowserEntryBase*>::iterator& curr = last == back ? back : front;

            // find next thumbnail at filtered distance after current
            for (; curr < fd.end (); ++curr) {
                if (!(*curr)->filtered) {
                    if (distance-- == 0) {
                        // clear current selection
                        for (size_t i = 0; i < selected.size (); ++i) {
                            selected[i]->selected = false;
                            redrawNeeded (selected[i]);
                        }

                        selected.clear ();

                        // make sure the newly selected thumbnail is visible and make it current
                        scrollToEntry (h, v, internal.get_width (), internal.get_height (), *curr);
                        lastClicked = *curr;

                        // either enlarge current selection or set new selection
                        if(enlarge) {
                            // reverse direction if distance is too large
                            if(front > back) {
                                std::swap(front, back);
                            }

                            for (; front <= back && front != fd.end(); ++front) {
                                if (!(*front)->filtered) {
                                    (*front)->selected = true;
                                    redrawNeeded (*front);
                                    selected.push_back (*front);
                                }
                            }
                        } else {
                            (*curr)->selected = true;
                            redrawNeeded (*curr);
                            selected.push_back (*curr);
                        }

                        break;
                    }
                }
            }
        }

        MYWRITERLOCK_RELEASE(l);
        selectionChanged ();
    }

    setScrollPosition (h, v);
}

void ThumbBrowserBase::selectFirst (bool enlarge)
{
    double h, v;
    getScrollPosition (h, v);

    {
        MYWRITERLOCK(l, entryRW);

        if (!fd.empty ()) {
            // find first unfiltered entry
            std::vector<ThumbBrowserEntryBase*>::iterator first = fd.begin ();

            for (; first < fd.end (); ++first) {
                if (!(*first)->filtered) {
                    break;
                }
            }

            scrollToEntry (h, v, internal.get_width (), internal.get_height (), *first);

            ThumbBrowserEntryBase* lastEntry = lastClicked;
            lastClicked = *first;

            if(selected.empty ()) {
                (*first)->selected = true;
                redrawNeeded (*first);
                selected.push_back (*first);
            } else {
                std::vector<ThumbBrowserEntryBase*>::iterator back = std::find (fd.begin (), fd.end (), lastEntry ? lastEntry : selected.back ());

                if (first > back) {
                    std::swap(first, back);
                }

                // clear current selection
                for (size_t i = 0; i < selected.size (); ++i) {
                    selected[i]->selected = false;
                    redrawNeeded (selected[i]);
                }

                selected.clear ();

                // either enlarge current selection or set new selection
                for (; first <= back; ++first) {
                    if (!(*first)->filtered) {
                        (*first)->selected = true;
                        redrawNeeded (*first);
                        selected.push_back (*first);
                    }

                    if (!enlarge) {
                        break;
                    }
                }
            }
        }

        MYWRITERLOCK_RELEASE(l);
        selectionChanged ();
    }

    setScrollPosition (h, v);
}

void ThumbBrowserBase::selectLast (bool enlarge)
{
    double h, v;
    getScrollPosition (h, v);

    {
        MYWRITERLOCK(l, entryRW);

        if (!fd.empty ()) {
            // find last unfiltered entry
            std::vector<ThumbBrowserEntryBase*>::iterator last = fd.end () - 1;

            for (; last >= fd.begin (); --last) {
                if (!(*last)->filtered) {
                    break;
                }
            }

            scrollToEntry (h, v, internal.get_width (), internal.get_height (), *last);

            ThumbBrowserEntryBase* lastEntry = lastClicked;
            lastClicked = *last;

            if(selected.empty()) {
                (*last)->selected = true;
                redrawNeeded (*last);
                selected.push_back (*last);
            } else {
                std::vector<ThumbBrowserEntryBase*>::iterator front = std::find (fd.begin (), fd.end (), lastEntry ? lastEntry : selected.front ());

                if (last < front) {
                    std::swap(last, front);
                }

                // clear current selection
                for (size_t i = 0; i < selected.size (); ++i) {
                    selected[i]->selected = false;
                    redrawNeeded (selected[i]);
                }

                selected.clear ();

                // either enlarge current selection or set new selection
                for (; front <= last; --last) {
                    if (!(*last)->filtered) {
                        (*last)->selected = true;
                        redrawNeeded (*last);
                        selected.push_back (*last);
                    }

                    if (!enlarge) {
                        break;
                    }
                }

                std::reverse(selected.begin (), selected.end ());
            }
        }

        MYWRITERLOCK_RELEASE(l);
        selectionChanged ();
    }

    setScrollPosition (h, v);
}

void ThumbBrowserBase::resizeThumbnailArea (int w, int h)
{

    inW = w;
    inH = h;

    if (hscroll.get_value() + internal.get_width() > inW) {
        hscroll.set_value (inW - internal.get_width());
    }

    if (vscroll.get_value() + internal.get_height() > inH) {
        vscroll.set_value (inH - internal.get_height());
    }

    configScrollBars ();
}

void ThumbBrowserBase::internalAreaResized (Gtk::Allocation& req)
{

    if (inW > 0 && inH > 0) {
        configScrollBars ();
        redraw ();
    }
}

void ThumbBrowserBase::onInternalAreaDraw()
{
    int deviceScale = RTScalable::getScaleForWidget(this);
    if (deviceScale == lastDeviceScale) return;

    lastDeviceScale = deviceScale;

    MYWRITERLOCK(l, entryRW);
    for (auto& entry : fd) {
        entry->onDeviceScaleChanged(deviceScale);
    }
}

void ThumbBrowserBase::configScrollBars ()
{

    // HOMBRE:DELETE ME?
    GThreadLock tLock; // Acquire the GUI

    if (inW > 0 && inH > 0) {
        int ih = internal.get_height();
        if (arrangement == TB_Horizontal) {
            auto ha = hscroll.get_adjustment();
            int iw = internal.get_width();
            ha->set_upper(inW);
            const int stepW = drawableEntries_.empty() ? 0 : drawableEntries_.front()->getEffectiveWidth();
            ha->set_step_increment(stepW);
            ha->set_page_increment(iw);
            ha->set_page_size(iw);
            if (iw >= inW || hscrollForceHidden) {
                hscroll.hide();
            } else {
                hscroll.show();
            }
        } else {
            hscroll.hide();
        }

        auto va = vscroll.get_adjustment();
        va->set_upper(inH);
        const int height = drawableEntries_.empty() ? 0 : drawableEntries_.front()->getEffectiveHeight();
        va->set_step_increment(height);
        va->set_page_increment(height == 0 ? ih : (ih / height) * height);
        va->set_page_size(ih);

        if (arrangement == TB_Horizontal) {
            // Filmstrip: single row, never show vertical scrollbar.
            // Don't set a height request on Internal — the parent container
            // (catalogPane) caps the filmstrip height. Setting a request here
            // would force the parent to grow beyond its cap in a Box layout.
            vscroll.hide();
        } else if (ih >= inH) {
            vscroll.hide();
        } else {
            vscroll.show();
        }
    }
}

void ThumbBrowserBase::arrangeFiles(ThumbBrowserEntryBase* entry, bool filterStateCurrent)
{

    if (fd.empty()) {
        // nothing to arrange
        clearDrawableEntries_();
        resizeThumbnailArea(0, 0);
        return;
    }
    if(entry && entry->filtered) {
        // a filtered entry was added, nothing to arrange, but has to be marked not drawable
        MYREADERLOCK(l, entryRW);
        entry->drawable = false;
        MYREADERLOCK_RELEASE(l);
        return;
    }

    MYREADERLOCK(l, entryRW);

    // GUI already locked by ::redraw, the only caller of this method for now.
    // We could lock it one more time, there's no harm excepted (negligible) speed penalty
    //GThreadLock lock;

    const bool fullRelayout = !entry;
    const bool buildDrawableEntriesInline = fullRelayout || arrangement == TB_Horizontal;
    if (buildDrawableEntriesInline) {
        drawableEntries_.clear();
        drawableEntries_.reserve(fd.size());
    }

    int rowHeight = 0;
    if (entry) {
        // we got the reference to the added entry, makes calculation of rowHeight O(1)
        lastRowHeight = rowHeight = std::max(lastRowHeight, entry->getMinimalHeight());
    } else {

        lastRowHeight = 0;
        for (auto* thumb : fd) {
            if (!filterStateCurrent) {
                thumb->filtered = !checkFilter(thumb);
            }

            if (thumb->filtered) {
                thumb->setPosition(-10000, -10000, thumb->getMinimalWidth(), 0);
                thumb->drawable = false;
                continue;
            }

            rowHeight = std::max(thumb->getMinimalHeight(), rowHeight);
            drawableEntries_.push_back(thumb);
        }
    }

    if (arrangement == TB_Horizontal) {
        numOfCols = 1;

        int currx = 0;

        if (fullRelayout) {
            for (auto* thumb : drawableEntries_) {
                const int maxw = thumb->getMinimalWidth();
                thumb->setPosition(currx, 0, maxw, rowHeight);
                thumb->drawable = true;
                currx += maxw;
            }
        } else for (unsigned int ct = 0; ct < fd.size(); ) {
            // skip filtered entries — position off-screen
            if (fd[ct]->filtered) {
                fd[ct]->setPosition(-10000, -10000, fd[ct]->getMinimalWidth(), rowHeight);
                fd[ct]->drawable = false;
                ++ct;
                continue;
            }

            const int maxw = fd[ct]->getMinimalWidth();
            fd[ct]->setPosition(currx, 0, maxw, rowHeight);
            fd[ct]->drawable = true;
            drawableEntries_.push_back(fd[ct]);
            currx += maxw;
            ++ct;
        }

        if (!buildDrawableEntriesInline) {
            rebuildDrawableEntries_();
        }
        MYREADERLOCK_RELEASE(l);
        // This will require a Writer access
        resizeThumbnailArea(currx, rowHeight);
    } else {
        const int availWidth = std::max(internal.get_width(), 1);
        int currx = 0;
        int curry = 0;
        int entriesInRow = 0;
        int maxEntriesInRow = 0;
        int maxRowWidth = 0;

        drawableEntries_.clear();
        drawableEntries_.reserve(fd.size());

        for (auto* thumb : fd) {
            if (thumb->filtered) {
                thumb->setPosition(-10000, -10000, thumb->getMinimalWidth(), rowHeight);
                thumb->drawable = false;
                continue;
            }

            const int itemWidth = std::max(thumb->getMinimalWidth(), 1);
            if (currx > 0 && currx + itemWidth > availWidth) {
                maxRowWidth = std::max(maxRowWidth, currx);
                maxEntriesInRow = std::max(maxEntriesInRow, entriesInRow);
                curry += rowHeight;
                currx = 0;
                entriesInRow = 0;
            }

            thumb->setPosition(currx, curry, itemWidth, rowHeight);
            thumb->drawable = true;
            drawableEntries_.push_back(thumb);
            currx += itemWidth;
            ++entriesInRow;
        }

        if (entriesInRow > 0) {
            maxRowWidth = std::max(maxRowWidth, currx);
            maxEntriesInRow = std::max(maxEntriesInRow, entriesInRow);
            curry += rowHeight;
        }

        numOfCols = std::max(maxEntriesInRow, 1);
        MYREADERLOCK_RELEASE(l);
        // This will require a Writer access
        resizeThumbnailArea(maxRowWidth, curry);
    }
}

void ThumbBrowserBase::disableInspector()
{
    if (inspector) {
        inspector->setActive(false);
    }
}

void ThumbBrowserBase::enableInspector()
{
    if (inspector) {
        inspector->setActive(true);
    }
}

bool ThumbBrowserBase::Internal::on_configure_event(GdkEventConfigure *configure_event)
{
    return true;
}

void ThumbBrowserBase::Internal::on_style_updated()
{
    style = get_style_context ();
    textn = style->get_color(Gtk::STATE_FLAG_NORMAL);
    texts = style->get_color(Gtk::STATE_FLAG_SELECTED);
    bgn = style->get_background_color(Gtk::STATE_FLAG_NORMAL);
    bgs = style->get_background_color(Gtk::STATE_FLAG_SELECTED);
}

void ThumbBrowserBase::Internal::on_realize()
{
    // Gtk signals automatically acquire the GUI (i.e. this method is enclosed by gdk_thread_enter and gdk_thread_leave)
    Cairo::FontOptions cfo;
    cfo.set_antialias (Cairo::ANTIALIAS_SUBPIXEL);
    get_pango_context()->set_cairo_font_options (cfo);

    Gtk::DrawingArea::on_realize();

    style = get_style_context ();
    textn = style->get_color(Gtk::STATE_FLAG_NORMAL);
    texts = style->get_color(Gtk::STATE_FLAG_SELECTED);
    bgn = style->get_background_color(Gtk::STATE_FLAG_NORMAL);
    bgs = style->get_background_color(Gtk::STATE_FLAG_SELECTED);

    set_can_focus(true);
    add_events(Gdk::EXPOSURE_MASK | Gdk::BUTTON_PRESS_MASK | Gdk::BUTTON_RELEASE_MASK | Gdk::POINTER_MOTION_MASK | Gdk::SCROLL_MASK | Gdk::SMOOTH_SCROLL_MASK | Gdk::KEY_PRESS_MASK);
    set_has_tooltip (true);
    signal_query_tooltip().connect( sigc::mem_fun(*this, &ThumbBrowserBase::Internal::on_query_tooltip) );
}

bool ThumbBrowserBase::Internal::on_query_tooltip (int x, int y, bool keyboard_tooltip, const Glib::RefPtr<Gtk::Tooltip>& tooltip)
{
    // Gtk signals automatically acquire the GUI (i.e. this method is enclosed by gdk_thread_enter and gdk_thread_leave)
    Glib::ustring ttip;
    bool useMarkup = false;
    {
        MYREADERLOCK(l, parent->entryRW);

        const int w = get_width();
        const int h = get_height();

        const std::size_t first = parent->firstViewportCandidate_(0, 0);
        for (size_t i = first; i < parent->drawableEntries_.size(); i++) {
            auto* entry = parent->drawableEntries_[i];

            const auto relation = parent->viewportRelation_(entry, 0, 0, w, h);
            if (relation == ThumbBrowserBase::VREL_BEFORE || relation == ThumbBrowserBase::VREL_OUTSIDE) {
                continue;
            }
            if (relation == ThumbBrowserBase::VREL_AFTER) {
                break;
            }

            parent->syncEntryOffset_(entry);
            if (entry->inside (x, y)) {
                std::tie(ttip, useMarkup) = entry->getToolTip (x, y);
                break;
            }
        }
    }

    if (!ttip.empty()) {
        if (useMarkup) {
            tooltip->set_markup(ttip);
        } else {
            tooltip->set_text(ttip);
        }
        return true;
    } else {
        return false;
    }
}

void ThumbBrowserBase::on_style_updated ()
{
    // GUI will be acquired by refreshThumbImages
    refreshThumbImages ();
}

ThumbBrowserBase::Internal::Internal () : ofsX(0), ofsY(0), parent(nullptr), dirty(true)
{
    set_name("FileCatalog");
}

void ThumbBrowserBase::Internal::setParent (ThumbBrowserBase* p)
{
    parent = p;
}

void ThumbBrowserBase::Internal::setPosition (int x, int y)
{
    ofsX = x;
    ofsY = y;
}

bool ThumbBrowserBase::Internal::on_key_press_event (GdkEventKey* event)
{
    // Gtk signals automatically acquire the GUI (i.e. this method is enclosed by gdk_thread_enter and gdk_thread_leave)
    return parent->keyPressed (event);
}

bool ThumbBrowserBase::Internal::on_button_press_event (GdkEventButton* event)
{
    // Gtk signals automatically acquire the GUI (i.e. this method is enclosed by gdk_thread_enter and gdk_thread_leave)
    grab_focus ();

    parent->eventTime = event->time;

    parent->buttonPressed ((int)event->x, (int)event->y, event->button, event->type, event->state, 0, 0, get_width(), get_height());
    Glib::RefPtr<Gdk::Window> window = get_window();

    GdkRectangle rect;
    rect.x = 0;
    rect.y = 0;
    rect.width = window->get_width();
    rect.height = window->get_height();

    gdk_window_invalidate_rect (window->gobj(), &rect, true);
    gdk_window_process_updates (window->gobj(), true);

    return true;
}

void ThumbBrowserBase::buttonPressed (int x, int y, int button, GdkEventType type, int state, int clx, int cly, int clw, int clh)
{
    // GUI already acquired

    ThumbBrowserEntryBase* fileDescr = nullptr;
    bool handled = false;

    {
        MYREADERLOCK(l, entryRW);

        const std::size_t first = firstViewportCandidate_(clx, cly);
        for (size_t i = first; i < drawableEntries_.size(); i++) {
            auto* entry = drawableEntries_[i];

            const auto relation = viewportRelation_(entry, clx, cly, clw, clh);
            if (relation == VREL_BEFORE || relation == VREL_OUTSIDE) {
                continue;
            }
            if (relation == VREL_AFTER) {
                break;
            }

            syncEntryOffset_(entry);
            if (entry->inside (x, y)) {
                fileDescr = entry;
            }

            const bool b = entry->pressNotify (button, type, state, x, y);
            handled = handled || b;
        }
    }

    if (handled || (fileDescr && fileDescr->processing)) {
        return;
    }

    {
        MYWRITERLOCK(l, entryRW);

        if (selected.size() == 1 && type == GDK_2BUTTON_PRESS && button == 1) {
            // Opening the editor reparents this browser into the filmstrip and
            // immediately asks it for the selected thumbnail. Do not carry the
            // writer lock into that callback: getSelectedThumbnail() takes a
            // reader lock, which otherwise deadlocks this GTK thread against
            // itself.
            ThumbBrowserEntryBase* const openEntry = selected[0];
            MYWRITERLOCK_RELEASE(l);
            doubleClicked(openEntry);
        } else if (button == 1 && type == GDK_BUTTON_PRESS) {
            if (fileDescr && (state & GDK_SHIFT_MASK))
                selectRange (fileDescr, state & GDK_CONTROL_MASK);
            else if (fileDescr && (state & GDK_CONTROL_MASK))
                selectSet (fileDescr);
            else
                selectSingle (fileDescr);

            lastClicked = fileDescr;
            MYWRITERLOCK_RELEASE(l);
            selectionChanged ();

            // In filmstrip mode, single-click also opens the image
            if (location == THLOC_EDITOR && fileDescr && selected.size() == 1) {
                doubleClicked (fileDescr);
            }
        } else if (fileDescr && button == 3 && type == GDK_BUTTON_PRESS) {
            if (!fileDescr->selected) {
                selectSingle (fileDescr);

                lastClicked = fileDescr;
                MYWRITERLOCK_RELEASE(l);
                selectionChanged ();
            }

            MYWRITERLOCK_RELEASE(l);
            rightClicked ();
        }
    } // end of MYWRITERLOCK(l, entryRW);

}

bool ThumbBrowserBase::Internal::on_draw(const ::Cairo::RefPtr< Cairo::Context> &cr)
{
    // Gtk signals automatically acquire the GUI (i.e. this method is enclosed by gdk_thread_enter and gdk_thread_leave)

    dirty = false;

    parent->onInternalAreaDraw();
    auto logical = hidpi::LogicalSize::forWidget(this);

    // draw thumbnails

    cr->set_antialias(Cairo::ANTIALIAS_NONE);
    cr->set_line_join(Cairo::LINE_JOIN_MITER);

    // Explicit background fill — macOS Quartz doesn't always honor CSS background-color on DrawingArea
    cr->set_source_rgb(0.145, 0.165, 0.196); // #252a32
    cr->paint();

    style->render_background(cr, 0., 0., logical.width, logical.height);

    bool thumbnailPrioritiesChanged = false;

    {
        MYWRITERLOCK(l, parent->entryRW);

        parent->previousVisibleEntries_.swap(parent->visibleEntries_);
        parent->visibleEntries_.clear();
        parent->visibleEntries_.reserve(parent->previousVisibleEntries_.size());
        parent->entriesToDraw_.clear();
        parent->entriesToDraw_.reserve(parent->previousVisibleEntries_.size());

        const std::size_t visibleGeneration = ++parent->visibleGenerationCounter_;
        const std::size_t first = parent->firstViewportCandidate_(0, 0);
        for (size_t i = first; i < parent->drawableEntries_.size() && !dirty; i++) { // if dirty meanwhile, cancel and wait for next redraw
            auto* entry = parent->drawableEntries_[i];

            const auto relation = parent->viewportRelation_(entry, 0, 0, logical.width, logical.height);
            if (relation == ThumbBrowserBase::VREL_BEFORE || relation == ThumbBrowserBase::VREL_OUTSIDE) {
                continue;
            }
            if (relation == ThumbBrowserBase::VREL_AFTER) {
                break;
            }

            parent->syncEntryOffset_(entry);
            if (!entry->updatepriority) {
                thumbnailPrioritiesChanged = true;
            }
            entry->updatepriority = true;
            entry->visibleGeneration = visibleGeneration;
            parent->visibleEntries_.push_back(entry);
            parent->entriesToDraw_.push_back(entry);
        }

        for (auto* entry : parent->previousVisibleEntries_) {
            if (entry->visibleGeneration != visibleGeneration && entry->updatepriority) {
                entry->updatepriority = false;
                thumbnailPrioritiesChanged = true;
            }
        }

        parent->previousVisibleEntries_.clear();
    }

    parent->visibleThumbnailRequests_.clear();
    parent->visibleThumbnailRequests_.reserve(parent->entriesToDraw_.size());
    for (auto* entry : parent->entriesToDraw_) {
        entry->appendQuickThumbnailJob(parent->visibleThumbnailRequests_);
    }

    for (auto* entry : parent->entriesToDraw_) {
        if (dirty) {
            break;
        }
        entry->draw(cr);
    }

    if (!parent->visibleThumbnailRequests_.empty()) {
        thumbImageUpdater->addBatch(parent->visibleThumbnailRequests_);
        parent->visibleThumbnailRequests_.clear();
    }
    parent->entriesToDraw_.clear();
    if (thumbnailPrioritiesChanged) {
        thumbImageUpdater->prioritiesChanged();
    }
    // Frame border removed for cleaner look

    return true;
}

Gtk::SizeRequestMode ThumbBrowserBase::Internal::get_request_mode_vfunc () const
{
    return Gtk::SIZE_REQUEST_CONSTANT_SIZE;
}

void ThumbBrowserBase::Internal::get_preferred_height_vfunc (int &minimum_height, int &natural_height) const
{
    if (parent && parent->arrangement == ThumbBrowserBase::TB_Horizontal) {
        // Filmstrip mode: report the entry height only. getEffectiveHeight()
        // adds the horizontal scrollbar's height, but the scrollbar lives in
        // its own grid row — including it here double-counted it and left a
        // scrollbar-sized band of padding below the thumbnails.
        int contentH = 0;
        {
            MYREADERLOCK(l, parent->entryRW);
            if (!parent->drawableEntries_.empty()) {
                contentH = parent->drawableEntries_.front()->getEffectiveHeight();
            }
        }
        if (contentH <= 0) {
            contentH = parent->getThumbnailHeight();
        }
        minimum_height = contentH;
        natural_height = contentH;
    } else {
        minimum_height = RTScalable::scalePixelSize(20);
        natural_height = RTScalable::scalePixelSize(80);
    }
}

void ThumbBrowserBase::Internal::get_preferred_width_vfunc (int &minimum_width, int &natural_width) const
{
    minimum_width = RTScalable::scalePixelSize(200);
    natural_width = RTScalable::scalePixelSize(1000);
}

void ThumbBrowserBase::Internal::get_preferred_height_for_width_vfunc (int width, int &minimum_height, int &natural_height) const
{
    get_preferred_height_vfunc(minimum_height, natural_height);
}

void ThumbBrowserBase::Internal::get_preferred_width_for_height_vfunc (int height, int &minimum_width, int &natural_width) const
{
    get_preferred_width_vfunc (minimum_width, natural_width);
}


bool ThumbBrowserBase::Internal::on_button_release_event (GdkEventButton* event)
{
    // Gtk signals automatically acquire the GUI (i.e. this method is enclosed by gdk_thread_enter and gdk_thread_leave)
    int w = get_width();
    int h = get_height();

    MYREADERLOCK(l, parent->entryRW);

    const std::size_t first = parent->firstViewportCandidate_(0, 0);
    for (size_t i = first; i < parent->drawableEntries_.size(); i++) {
        auto* entry = parent->drawableEntries_[i];

        const auto relation = parent->viewportRelation_(entry, 0, 0, w, h);
        if (relation == ThumbBrowserBase::VREL_BEFORE || relation == ThumbBrowserBase::VREL_OUTSIDE) {
            continue;
        }
        if (relation == ThumbBrowserBase::VREL_AFTER) {
            break;
        }

        parent->syncEntryOffset_(entry);
        ThumbBrowserEntryBase* tbe = entry;
        MYREADERLOCK_RELEASE(l);
        // This will require a Writer access...
        tbe->releaseNotify (event->button, event->type, event->state, (int)event->x, (int)event->y);
        MYREADERLOCK_ACQUIRE(l);
    }

    return true;
}

bool ThumbBrowserBase::Internal::on_motion_notify_event (GdkEventMotion* event)
{
    // Gtk signals automatically acquire the GUI (i.e. this method is enclosed by gdk_thread_enter and gdk_thread_leave)
    int w = get_width();
    int h = get_height();

    MYREADERLOCK(l, parent->entryRW);

    const std::size_t first = parent->firstViewportCandidate_(0, 0);
    for (size_t i = first; i < parent->drawableEntries_.size(); i++) {
        auto* entry = parent->drawableEntries_[i];

        const auto relation = parent->viewportRelation_(entry, 0, 0, w, h);
        if (relation == ThumbBrowserBase::VREL_BEFORE || relation == ThumbBrowserBase::VREL_OUTSIDE) {
            continue;
        }
        if (relation == ThumbBrowserBase::VREL_AFTER) {
            break;
        }

        parent->syncEntryOffset_(entry);
        entry->motionNotify ((int)event->x, (int)event->y);
    }

    return true;
}

bool ThumbBrowserBase::Internal::on_scroll_event (GdkEventScroll* event)
{
    // Gtk signals automatically acquire the GUI (i.e. this method is enclosed by gdk_thread_enter and gdk_thread_leave)
    parent->scroll (event->direction, event->delta_x, event->delta_y);
    return true;
}

void ThumbBrowserBase::resort ()
{
    {
        MYWRITERLOCK(l, entryRW);

        const auto& options = App::get().options();
        std::sort(
            fd.begin(),
            fd.end(),
            [&](const ThumbBrowserEntryBase* a, const ThumbBrowserEntryBase* b)
            {
                bool lt = a->compare(*b, options.sortMethod);
                return options.sortDescending ? !lt : lt;
            }
        );
        entriesOrderChanged_();
    }

    redraw ();
}

void ThumbBrowserBase::redraw (ThumbBrowserEntryBase* entry, bool filterStateCurrent)
{

    GThreadLock lock;
    if (redrawPending_) {
        redrawTimeout_.disconnect();
        redrawPending_ = false;
    }
    ThumbBrowserEntryBase* horizontalAnchor = nullptr;
    double horizontalAnchorOffset = 0.0;
    bool hasPendingInserts = false;
    if (!layoutPaused_) {
        {
            MyMutex::MyLock pendingLock(pendingMutex_);
            hasPendingInserts = !pendingInserts_.empty();
        }

        // Sorted batches can insert before the current viewport. Keep the
        // selected/leftmost visible thumbnail at the same screen coordinate so
        // background folder loading never makes the filmstrip jump under a
        // user's wheel or touchpad gesture.
        if (hasPendingInserts && arrangement == TB_Horizontal && !drawableEntries_.empty()) {
            const double viewportLeft = hscroll.get_value();
            const double viewportRight = viewportLeft + std::max(internal.get_width(), 1);

            MYREADERLOCK(entryLock, entryRW);
            auto isVisible = [viewportLeft, viewportRight](const ThumbBrowserEntryBase* thumb) {
                return !thumb->filtered
                    && thumb->getX() + thumb->getEffectiveWidth() > viewportLeft
                    && thumb->getX() < viewportRight;
            };

            if (lastClicked && isVisible(lastClicked)) {
                horizontalAnchor = lastClicked;
            } else {
                for (auto* selectedEntry : selected) {
                    if (isVisible(selectedEntry)) {
                        horizontalAnchor = selectedEntry;
                        break;
                    }
                }
            }

            if (!horizontalAnchor) {
                for (auto* visibleEntry : drawableEntries_) {
                    if (visibleEntry->getX() + visibleEntry->getEffectiveWidth() > viewportLeft) {
                        horizontalAnchor = visibleEntry;
                        break;
                    }
                }
            }

            if (horizontalAnchor) {
                horizontalAnchorOffset = horizontalAnchor->getX() - viewportLeft;
            }
        }

        flushPendingInserts_();
    }
    arrangeFiles(entry, filterStateCurrent);

    if (horizontalAnchor && !horizontalAnchor->filtered) {
        setScrollPosition(horizontalAnchor->getX() - horizontalAnchorOffset, vscroll.get_value());
    }

    if (arrangement == TB_Horizontal) {
        int allocated = internal.get_allocated_height();
        int content = getEffectiveHeight();
        if (content > 0 && content != allocated) {
            internal.queue_resize();
        }
    }
    queue_draw();
}

void ThumbBrowserBase::zoomChanged (bool zoomIn)
{

    int newHeight = 0;
    int optThumbSize = getThumbnailHeight();

    const auto& options = App::get().options();
    if (zoomIn)
        for (size_t i = 0; i < options.thumbnailZoomRatios.size(); i++) {
            newHeight = (int)(options.thumbnailZoomRatios[i] * getMaxThumbnailHeight());

            if (newHeight > optThumbSize) {
                break;
            }
        }
    else
        for (size_t i = options.thumbnailZoomRatios.size() - 1; i > 0; i--) {
            newHeight = (int)(options.thumbnailZoomRatios[i] * getMaxThumbnailHeight());

            if (newHeight < optThumbSize) {
                break;
            }
        }

    previewHeight = newHeight;

    saveThumbnailHeight(newHeight);

    {
        MYWRITERLOCK(l, entryRW);

        for (size_t i = 0; i < fd.size(); i++) {
            fd[i]->resize (previewHeight);
        }
    }

    redraw ();
}

void ThumbBrowserBase::setThumbnailHeight (int newHeight)
{
    previewHeight = newHeight;
    saveThumbnailHeight(newHeight);

    {
        MYWRITERLOCK(l, entryRW);

        for (size_t i = 0; i < fd.size(); i++) {
            fd[i]->resize (previewHeight);
        }
    }

    redraw ();
}

void ThumbBrowserBase::refreshThumbImages ()
{

    int previewHeight = getThumbnailHeight();
    {
        MYWRITERLOCK(l, entryRW);

        for (size_t i = 0; i < fd.size(); i++) {
            fd[i]->resize (previewHeight);
        }
    }

    redraw ();
}

void ThumbBrowserBase::refreshQuickThumbImages ()
{
    std::vector<ThumbBrowserEntryBase*> entries;
    std::vector<ThumbBrowserEntryBase*> fallbackEntries;
    std::vector<ThumbImageUpdater::Request> requests;

    {
        MYWRITERLOCK(l, entryRW);

        const int viewportWidth = internal.get_width();
        const int viewportHeight = internal.get_height();
        if (viewportWidth > 0 && viewportHeight > 0 && !drawableEntries_.empty()) {
            entries.reserve(std::min<std::size_t>(drawableEntries_.size(), 128));

            const std::size_t first = firstViewportCandidate_(0, 0);
            for (size_t i = first; i < drawableEntries_.size(); ++i) {
                auto* entry = drawableEntries_[i];

                const auto relation = viewportRelation_(entry, 0, 0, viewportWidth, viewportHeight);
                if (relation == VREL_BEFORE || relation == VREL_OUTSIDE) {
                    continue;
                }
                if (relation == VREL_AFTER) {
                    break;
                }

                entries.push_back(entry);
            }
        } else {
            entries.reserve(visibleEntries_.size());

            for (auto* entry : visibleEntries_) {
                if (entry->filtered) {
                    continue;
                }
                entries.push_back(entry);
            }
        }

        const std::size_t fallbackCount = std::min<std::size_t>(drawableEntries_.size(), 64);
        fallbackEntries.reserve(fallbackCount);

        for (size_t i = 0; i < fallbackCount; ++i) {
            auto* entry = drawableEntries_[i];
            if (entry->filtered) {
                continue;
            }
            fallbackEntries.push_back(entry);
        }
    }

    requests.reserve(std::max(entries.size(), fallbackEntries.size()));
    for (auto* entry : entries) {
        entry->appendQuickThumbnailJob(requests);
    }

    if (requests.empty()) {
        for (auto* entry : fallbackEntries) {
            entry->appendQuickThumbnailJob(requests);
        }
    }

    if (!requests.empty()) {
        thumbImageUpdater->addBatch(requests);
    }
}

void ThumbBrowserBase::clearVisibleEntries_()
{
    for (auto* entry : visibleEntries_) {
        entry->updatepriority = false;
    }
    visibleEntries_.clear();
    previousVisibleEntries_.clear();
    entriesToDraw_.clear();
    visibleThumbnailRequests_.clear();
}

void ThumbBrowserBase::clearDrawableEntries_()
{
    drawableEntries_.clear();
}

void ThumbBrowserBase::refreshEditedState (const std::set<Glib::ustring>& efiles)
{

    if (efiles.empty() && editedFiles.empty()) {
        return;
    }

    editedFiles = efiles;
    const bool hasEditedFiles = !editedFiles.empty();
    bool changed = false;

    {
        MYREADERLOCK(l, entryRW);

        for (size_t i = 0; i < fd.size(); i++) {
            const bool framed = hasEditedFiles && editedFiles.find (fd[i]->filename) != editedFiles.end();
            if (fd[i]->framed != framed) {
                fd[i]->framed = framed;
                changed = true;
            }
        }
    }

    if (changed) {
        queue_draw ();
    }
}

void ThumbBrowserBase::setArrangement (Arrangement a)
{

    arrangement = a;
    redraw ();
}

void ThumbBrowserBase::enableTabMode(bool enable)
{
    location = enable ? THLOC_EDITOR : THLOC_FILEBROWSER;
    arrangement = enable ? ThumbBrowserBase::TB_Horizontal : ThumbBrowserBase::TB_Vertical;

    if (enable) {
        // In filmstrip mode, never show the vertical scrollbar.
        vscroll.set_no_show_all(true);
        vscroll.hide();
        // Allow horizontal scrollbar to appear when thumbnails overflow
        hscrollForceHidden = false;
        hscroll.set_no_show_all(false);
        // Compact spacing — the scrollbar row will only take space when visible
        set_row_spacing(0);
        set_column_spacing(0);
    } else {
        vscroll.set_no_show_all(false);
        hscrollForceHidden = false;
        hscroll.set_no_show_all(false);
    }

    const unsigned int generation = ++tabModeGeneration_;
    if (applyTabModeEntryGeometry_(enable, generation)) {
        Glib::signal_timeout().connect(
            sigc::bind(
                sigc::mem_fun(*this, &ThumbBrowserBase::applyTabModeEntryGeometry_),
                enable,
                generation),
            10,
            G_PRIORITY_HIGH_IDLE);
    }
}

bool ThumbBrowserBase::applyTabModeEntryGeometry_(bool enable, unsigned int generation)
{
    if (generation != tabModeGeneration_) {
        return false;
    }

    {
        MyTryWriterLock lock(entryRW);
        if (!lock.owns_lock()) {
            return true;
        }

        for (auto* entry : fd) {
            entry->setMargins(enable ? 0 : 2, enable ? 0 : 2);
            entry->resize(getThumbnailHeight());
        }
    }

    // Always re-arrange with the new entry geometry. The browser direction
    // used to rely on FileCatalog::enableTabMode -> filterChanged() for its
    // redraw, but when this pass is deferred by a busy entry lock it runs
    // AFTER that redraw — the browser then kept filmstrip-sized cells and
    // wrapped rows far short of the window width.
    redraw();

    return false;
}

void ThumbBrowserBase::insertEntry (ThumbBrowserEntryBase* entry)
{
    // Queue entry for batch insertion instead of inserting one-by-one into sorted vector
    {
        MyMutex::MyLock lock(pendingMutex_);
        entry->onDeviceScaleChanged(lastDeviceScale);
        entry->setOffset((int)(hscroll.get_value()), (int)(vscroll.get_value()));
        pendingInserts_.push_back(entry);
    }

    schedulePendingInsertRedraw_();
}

void ThumbBrowserBase::insertEntries (const std::vector<ThumbBrowserEntryBase*>& entries)
{
    if (entries.empty()) {
        return;
    }

    {
        MyMutex::MyLock lock(pendingMutex_);
        pendingInserts_.reserve(pendingInserts_.size() + entries.size());
        const int xoffset = static_cast<int>(hscroll.get_value());
        const int yoffset = static_cast<int>(vscroll.get_value());

        for (auto* entry : entries) {
            entry->onDeviceScaleChanged(lastDeviceScale);
            entry->setOffset(xoffset, yoffset);
            pendingInserts_.push_back(entry);
        }
    }

    schedulePendingInsertRedraw_();
}

void ThumbBrowserBase::schedulePendingInsertRedraw_()
{
    // When layout is paused (during animations), just accumulate entries
    // without scheduling redraw — resumeLayout() will flush them all at once.
    if (layoutPaused_) {
        return;
    }

    // Schedule a debounced merge+redraw. Using a short timer instead of an
    // idle callback lets entries accumulate, dramatically reducing the number
    // of expensive arrangeFiles() calls when loading folders with thousands
    // of images. Filmstrip (horizontal) layout is trivially O(N) so we use
    // a shorter debounce; grid layout is more expensive so we keep 150ms.
    // PreviewLoader delivers cold results in small batches, preserving visible
    // progress without repeatedly laying out the entire folder.
    if (!redrawPending_) {
        redrawPending_ = true;
        const int debounceMs = fd.empty()
            ? 16
            : (arrangement == TB_Horizontal ? 50 : 150);
        redrawTimeout_ = Glib::signal_timeout().connect(
            sigc::mem_fun(*this, &ThumbBrowserBase::onRedrawIdle_),
            debounceMs,
            G_PRIORITY_DEFAULT_IDLE + 10
        );
    }
}

void ThumbBrowserBase::pauseLayout ()
{
    layoutPaused_ = true;
    // Cancel any pending redraw timer — entries will keep accumulating
    redrawTimeout_.disconnect();
    redrawPending_ = false;
}

void ThumbBrowserBase::resumeLayout ()
{
    layoutPaused_ = false;

    // If entries accumulated during the pause, flush them now
    bool hasPending;
    {
        MyMutex::MyLock lock(pendingMutex_);
        hasPending = !pendingInserts_.empty();
    }
    if (hasPending) {
        onRedrawIdle_();
    }
}

bool ThumbBrowserBase::onRedrawIdle_ ()
{
    redrawPending_ = false;
    redraw();
    return false;
}

void ThumbBrowserBase::flushPendingInserts_ ()
{
    // Collect all pending entries
    std::vector<ThumbBrowserEntryBase*> batch;
    {
        MyMutex::MyLock lock(pendingMutex_);
        batch.swap(pendingInserts_);
    }

    if (!batch.empty()) {
        MYWRITERLOCK(l, entryRW);

        const auto& options = App::get().options();
        auto cmp = [&](const ThumbBrowserEntryBase* a, const ThumbBrowserEntryBase* b) {
            return options.sortDescending
                ? b->compare(*a, options.sortMethod)
                : a->compare(*b, options.sortMethod);
        };

        if (batch.size() > 1 && !std::is_sorted(batch.begin(), batch.end(), cmp)) {
            std::sort(batch.begin(), batch.end(), cmp);
        }

        // Merge batch into the already-sorted fd vector in-place from the end.
        // This keeps the O(N+M) behavior while avoiding a full temporary copy of
        // the browser list on every thumbnail-load redraw.
        const std::size_t oldSize = fd.size();
        const std::size_t batchSize = batch.size();
        fd.resize(oldSize + batchSize);

        std::size_t ia = oldSize;
        std::size_t ib = batchSize;
        std::size_t out = fd.size();

        while (ia > 0 && ib > 0) {
            ThumbBrowserEntryBase* a = fd[ia - 1];
            ThumbBrowserEntryBase* b = batch[ib - 1];

            if (cmp(a, b)) {
                fd[--out] = b;
                --ib;
            } else {
                fd[--out] = a;
                --ia;
            }
        }

        while (ib > 0) {
            fd[--out] = batch[--ib];
        }

        entriesInserted_(batch);
        entriesOrderChanged_();
    }

}

void ThumbBrowserBase::getScrollPosition (double& h, double& v)
{
    h = hscroll.get_value ();
    v = vscroll.get_value ();
}

void ThumbBrowserBase::setScrollPosition (double h, double v)
{
    hscroll.set_value (h > hscroll.get_adjustment()->get_upper() ? hscroll.get_adjustment()->get_upper() : h);
    vscroll.set_value (v > vscroll.get_adjustment()->get_upper() ? vscroll.get_adjustment()->get_upper() : v);
}

int ThumbBrowserBase::getScrollOffsetX () const
{
    return -static_cast<int>(hscroll.get_value());
}

int ThumbBrowserBase::getScrollOffsetY () const
{
    return -static_cast<int>(vscroll.get_value());
}

// needed for auto-height in single tab
int ThumbBrowserBase::getEffectiveHeight()
{
    // Only include scrollbar height if it's actually visible
    int h = hscroll.get_visible() ? hscroll.get_height() : 0;

    MYREADERLOCK(l, entryRW);

    // Filtered items do not change in size, so take an arranged drawable entry.
    if (!drawableEntries_.empty()) {
        h += drawableEntries_.front()->getEffectiveHeight();
    }

    return h;
}

void ThumbBrowserBase::redrawNeeded (ThumbBrowserEntryBase* entry)
{

    // HOMBRE:DELETE ME?
    GThreadLock tLock; // Acquire the GUI

    // Unconditional: the viewport + dirty-coalescing guards could strand a
    // finished thumbnail (dirty set while its queue_draw was swallowed, e.g.
    // unmapped browser) with no repaint until the user clicked. GTK
    // coalesces queue_draw calls per frame, so this is cheap.
    internal.setDirty ();
    internal.queue_draw ();
}


