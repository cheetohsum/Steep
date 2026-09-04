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
 *
 *  Class created by Jean-Christophe FRISCH, aka 'Hombre'
 */
#pragma once

#include "threadutils.h"

#include <memory>
#include <vector>

#include <glibmm/refptr.h>
#include <glibmm/ustring.h>
#include <sigc++/signal.h>

namespace Gio
{
class Icon;
}

namespace Gtk
{

class Grid;
class Label;
class Menu;
class Button;
class ImageMenuItem;
class RadioButtonGroup;
class Widget;

}

typedef struct _GdkEventButton GdkEventButton;

class RTImage;

class PopUpCommon
{

public:
    typedef sigc::signal<void, int> type_signal_changed;
    typedef sigc::signal<void, int> type_signal_item_selected;
    /// Emitted with the entry index while the pointer rests on it in the open
    /// menu, and with -1 when the menu closes. Lets a caller preview an entry
    /// before committing to it; the -1 is the cue to put things back.
    typedef sigc::signal<void, int> type_signal_hovered;
    type_signal_changed signal_changed();
    type_signal_item_selected signal_item_selected();
    type_signal_hovered signal_hovered();
    Gtk::Grid* buttonGroup;    // this is the widget to be packed

    explicit PopUpCommon (Gtk::Button* button, const Glib::ustring& label = "");
    virtual ~PopUpCommon ();
    bool addEntry (const Glib::ustring& iconName, const Glib::ustring& label, Gtk::RadioButtonGroup* radioGroup = nullptr);
    bool insertEntry(int position, const Glib::ustring& iconName, const Glib::ustring& label, Gtk::RadioButtonGroup* radioGroup = nullptr);
    bool insertEntry(int position, const Glib::RefPtr<const Gio::Icon>& gIcon, const Glib::ustring& label, Gtk::RadioButtonGroup* radioGroup = nullptr);
    /// Sets the button image to show when there are no entries.
    void setEmptyImage(const Glib::ustring &fileName);
    int getEntryCount () const;
    bool setSelected (int entryNum);
    int  getSelected () const;
    void removeEntry(int position);
    void setButtonHint();
    void show ();
    void set_tooltip_text (const Glib::ustring &text);
    void setItemSensitivity (int i, bool isSensitive);
    /// Replaces one entry's text, for labels that carry live information —
    /// how much of the picture a class covers, say, which is only known once
    /// the picture has been looked at.
    void setEntryLabel (int i, const Glib::ustring& label);
    /// Replaces one entry's icon with a rendered tile — a thumbnail of what
    /// choosing it would select, which a fixed icon cannot show.
    void setEntryImage (int i, const Glib::RefPtr<Gdk::Pixbuf>& pixbuf);
    /// Leaves one entry out of the list without disturbing the numbering the
    /// caller stores. Never applied to the selected entry, which has to stay
    /// visible to be shown as selected.
    void setEntryVisible (int i, bool visible);
    void triggerShowMenu();    // Show the popup menu programmatically
    void hideArrowButton();    // Hide the dropdown arrow button
    void setShowSelectionLabel(bool show);  // Show selected entry's text inside the button

private:
    type_signal_changed messageChanged;
    type_signal_item_selected messageItemSelected;
    type_signal_hovered messageHovered;
    bool hoverSignalArmed_ = false;
    void entryHovered(Gtk::Widget* menuItem);

    Glib::ustring emptyImageFilename;
    std::vector<Glib::RefPtr<const Gio::Icon>> imageIcons;
    std::vector<Glib::ustring> imageIconNames;
    std::vector<const RTImage*> images;
    Glib::ustring buttonHint;
    RTImage* buttonImage;
    Gtk::Grid* imageContainer;
    std::unique_ptr<Gtk::Menu> menu;
    Gtk::Button* button;
    Gtk::Button* arrowButton;
    int selected;
    bool hasMenu;
    MyMutex entrySelectionMutex;

    Gtk::Label* selectionLabel_ = nullptr;

    void changeImage(int position);
    void changeImage(const Glib::ustring& iconName, const Glib::RefPtr<const Gio::Icon>& gIcon);
    void updateSelectionLabel();
    void entrySelected(Gtk::Widget* menuItem);
    bool insertEntryImpl(int position, const Glib::ustring& iconName, const Glib::RefPtr<const Gio::Icon>& gIcon, RTImage* image, const Glib::ustring& label, Gtk::RadioButtonGroup* radioGroup);
    void showMenu(GdkEventButton* event);

protected:
    virtual int posToIndex(int p) const { return p; }
    virtual int indexToPos(int i) const { return i; }

    void entrySelected (int i);

};

inline PopUpCommon::type_signal_changed PopUpCommon::signal_changed ()
{
    return messageChanged;
}

inline PopUpCommon::type_signal_hovered PopUpCommon::signal_hovered ()
{
    return messageHovered;
}

inline PopUpCommon::type_signal_item_selected PopUpCommon::signal_item_selected ()
{
    return messageItemSelected;
}

inline int PopUpCommon::getEntryCount () const
{
    return images.size();
}

inline int PopUpCommon::getSelected () const
{
    return posToIndex(selected);
}
