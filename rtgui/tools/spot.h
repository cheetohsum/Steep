/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2019 Jean-Christophe FRISCH <natureh.510@gmail.com>
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
 *  along with RawTherapee.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _SPOT_H_
#define _SPOT_H_

#include "editwidgets.h"
#include "guiutils.h"
#include "toolpanel.h"
#include "widgets/basic/adjuster.h"

#include "rtengine/procparams.h"
#include "rtengine/tweakoperator.h"

#include <gtkmm.h>

/**
 * @brief Let the user create/edit/delete points for Spot Removal tool
 */

class Spot : public ToolParamBlock, public FoldableToolPanel, public AdjusterListener, public rtengine::TweakOperator, public EditSubscriber
{

private:
    enum class DraggedSide {
        NONE,
        SOURCE,
        TARGET
    };

    DraggedSide draggedSide;       // tells which of source or target is being dragged
    int lastObject;                // current object that is hovered
    int activeSpot;                // currently active spot, being edited
    std::vector<rtengine::procparams::SpotEntry> spots; // list of edited spots
    OPIcon sourceIcon;             // to show the source location
    Circle sourceCircle;           // to show and change the Source radius
    Circle sourceMODisc;           // to change the Source position
    Circle targetCircle;           // to show and change the Target radius
    Circle targetMODisc;           // to change the Target position
    Circle sourceFeatherCircle;    // to show the Feather radius at the Source position
    Circle targetFeatherCircle;    // to show the Feather radius at the Target position
    Line link;                     // to show the link between the Source and Target position

    // Phase 2: cursor preview circle
    Circle cursorPreviewCircle;

    // Phase 4: stroke preview
    Polyline strokePreviewLine;
    bool isStrokeDragging = false;
    std::vector<rtengine::Coord> currentStrokePoints;

    OPIcon *getActiveSpotIcon ();
    void updateGeometry ();
    void createGeometry ();
    void addNewEntry ();
    void deleteSelectedEntry ();
    void resetPressed ();
    void releaseEdit();
    void methodChanged ();

    // Phase 1: method toggle buttons
    int getActiveMethod() const;
    void setActiveMethod(int index);
    void blockMethodButtons(bool block);
    void onMethodButtonToggled(Gtk::ToggleButton* button, int methodIndex);

    // Phase 3: dynamic size preview widget
    class SpotSizePreview : public Gtk::DrawingArea {
    public:
        SpotSizePreview();
        void setValue(int radius);
    protected:
        bool on_draw(const Cairo::RefPtr<Cairo::Context>& cr) override;
    private:
        int currentRadius = 25;
    };

protected:
    Gtk::Box* labelBox;
    Gtk::CheckButton* editedCheckBox;
    Gtk::Label* countLabel;
    Gtk::ToggleButton* edit;
    Gtk::Button* reset;
    Adjuster* spotSize;

    // Phase 1: icon toggle buttons replacing MyComboBoxText
    Gtk::ToggleButton* btnClone;
    Gtk::ToggleButton* btnHeal;
    Gtk::ToggleButton* btnErase;
    Gtk::ToggleButton* btnRedEye;
    Gtk::Box* methodBox;
    bool blockMethodSignal = false;
    sigc::connection cloneConn, healConn, eraseConn, redeyeConn;

    // Phase 3: size preview
    SpotSizePreview* sizePreview;

    AdvancedSection* aiSection;
    sigc::connection editConn, editedConn;

    void editToggled ();
    void editedToggled ();
    Geometry* getVisibleGeometryFromMO (int MOID);

public:
    static const Glib::ustring TOOL_NAME;

    Spot ();
    ~Spot ();

    void read (const rtengine::procparams::ProcParams* pp, const ParamsEdited* pedited = nullptr) override;
    void write (rtengine::procparams::ProcParams* pp, ParamsEdited* pedited = nullptr) override;

    void enabledChanged () override;

    void setEditProvider (EditDataProvider* provider) override;

    void setBatchMode (bool batchMode) override;

    // AdjusterListener interface
    void adjusterChanged (Adjuster* a, double newval) override;

    // EditSubscriber interface
    CursorShape getCursor (int objectID, int xPos, int yPos) const override;
    bool mouseOver (int modifierKey) override;
    bool button1Pressed (int modifierKey) override;
    bool button1Released () override;
    bool button2Pressed (int modifierKey) override;
    bool button3Pressed (int modifierKey) override;
    bool button3Released () override;
    bool drag1 (int modifierKey) override;
    bool drag3 (int modifierKey) override;
    bool pick2 (bool picked) override;
    bool pick3 (bool picked) override;
    void switchOffEditMode () override;

    //TweakOperator interface
    void tweakParams(rtengine::procparams::ProcParams& pparams) override;

    rtengine::ProcEvent EvSpotEnabled;
    rtengine::ProcEvent EvSpotEnabledOPA; // used to toggle-on the Spot 'On Preview Adjustment' mode
    rtengine::ProcEvent EvSpotEntry;
    rtengine::ProcEvent EvSpotEntryOPA;
    rtengine::ProcEvent EvSpotMethod;
};

#endif
