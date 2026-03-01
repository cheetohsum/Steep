/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2024 RawTherapee team
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

#include <vector>

#include <gtkmm.h>

#include "toolpanel.h"
#include "widgets/basic/adjuster.h"
#include "rtengine/procparams.h"

class PointColorPickListener
{
public:
    virtual ~PointColorPickListener() = default;
    virtual void pointColorPickRequested() = 0;
};

class PointColor final :
    public ToolParamBlock,
    public FoldableToolPanel,
    public AdjusterListener
{
public:
    static const Glib::ustring TOOL_NAME;

    PointColor();

    void read(const rtengine::procparams::ProcParams* pp, const ParamsEdited* pedited = nullptr) override;
    void write(rtengine::procparams::ProcParams* pp, ParamsEdited* pedited = nullptr) override;
    void setDefaults(const rtengine::procparams::ProcParams* defParams, const ParamsEdited* pedited = nullptr) override;
    void setBatchMode(bool batchMode) override;
    void trimValues(rtengine::procparams::ProcParams* pp) override;

    void adjusterChanged(Adjuster* a, double newval) override;
    void enabledChanged() override;

    void addTargetFromPick(float h, float s, float v);

    void setPointColorPickListener(PointColorPickListener* l) { pickListener = l; }

private:
    void updateColorSwatch();
    void updateTargetList();
    void loadTargetToControls(int idx);
    void saveControlsToTarget(int idx);
    void onAddTarget();
    void onRemoveTarget();
    void onPickTarget();
    void onTargetSelected();
    void setControlsSensitive(bool sensitive);
    static Glib::ustring hueToColorName(double hue01);

    // Target list model
    class TargetColumns : public Gtk::TreeModel::ColumnRecord {
    public:
        TargetColumns() { add(index); add(label); add(hue); }
        Gtk::TreeModelColumn<int> index;
        Gtk::TreeModelColumn<Glib::ustring> label;
        Gtk::TreeModelColumn<double> hue;
    };

    TargetColumns targetColumns;
    Glib::RefPtr<Gtk::ListStore> targetStore;
    Gtk::TreeView* targetList;
    Gtk::ScrolledWindow* targetScroll;

    // Toolbar buttons
    Gtk::Button* pickButton;
    Gtk::Button* addButton;
    Gtk::Button* removeButton;
    Gtk::Box* toolbarBox;

    // Per-target controls
    Adjuster* centerHueAdj;
    Adjuster* hueShiftAdj;
    Adjuster* saturationAdj;
    Adjuster* luminanceAdj;
    Adjuster* rangeAdj;

    // State
    std::vector<rtengine::procparams::PointColorTarget> targets;
    int activeTarget;
    PointColorPickListener* pickListener;
    bool internalUpdate;  // suppress adjuster events during load
};
