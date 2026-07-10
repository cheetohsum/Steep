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

#include "toolpanel.h"
#include "widgets/basic/adjuster.h"
#include "guiutils.h"

#include <gtkmm.h>

class FilmPresets final : public ToolParamBlock, public AdjusterListener, public FoldableToolPanel
{
private:
    // Enable checkbox (visible in flat mode since header is hidden)
    Gtk::CheckButton* enableCheck_;
    sigc::connection enableConn_;

    // Preset selector: button + popover + listbox (hover-to-preview)
    Gtk::Button* presetButton_;
    Gtk::Popover* presetPopover_;
    Gtk::ListBox* presetListBox_;
    int activePresetIdx_;       // Committed selection
    int hoverPresetIdx_;        // Currently hovered (-1 = none)
    sigc::connection hoverTimeout_;

    Adjuster* strength;
    Gtk::ComboBoxText* modelCombo_;
    Gtk::ComboBoxText* processCombo_;
    Gtk::ComboBoxText* outputCombo_;
    Gtk::ComboBoxText* formatCombo_;
    Adjuster* exposureAdj_;
    Adjuster* pushPullAdj_;
    Adjuster* contrast;
    Adjuster* saturation;
    Adjuster* warmth;
    Adjuster* tintAdj;
    Adjuster* fade;
    Adjuster* rolloff;
    Adjuster* shadowHue;
    Adjuster* shadowTintAdj;
    Adjuster* highlightHue;
    Adjuster* highlightTintAdj;
    Adjuster* halationAdj;
    Adjuster* redShift;
    Adjuster* greenShift;
    Adjuster* blueShift;
    Adjuster* grainAdj;
    Adjuster* vibranceAdj;

    Gtk::Box* detailContent_;
    Gtk::Revealer* detailRevealer_;
    bool detailExpanded_;

    Gtk::Dialog* customDialog_;
    Gtk::ScrolledWindow* dialogScrolled_;
    void openCustomDialog();

    rtengine::ProcEvent EvFilmPresetsEnabled;
    rtengine::ProcEvent EvFilmPresetsPreset;
    rtengine::ProcEvent EvFilmPresetsStrength;
    rtengine::ProcEvent EvFilmPresetsModel;
    rtengine::ProcEvent EvFilmPresetsExposure;
    rtengine::ProcEvent EvFilmPresetsPushPull;
    rtengine::ProcEvent EvFilmPresetsProcess;
    rtengine::ProcEvent EvFilmPresetsOutput;
    rtengine::ProcEvent EvFilmPresetsFormat;
    rtengine::ProcEvent EvFilmPresetsContrast;
    rtengine::ProcEvent EvFilmPresetsSaturation;
    rtengine::ProcEvent EvFilmPresetsWarmth;
    rtengine::ProcEvent EvFilmPresetsTint;
    rtengine::ProcEvent EvFilmPresetsFade;
    rtengine::ProcEvent EvFilmPresetsRolloff;
    rtengine::ProcEvent EvFilmPresetsShadowHue;
    rtengine::ProcEvent EvFilmPresetsShadowTint;
    rtengine::ProcEvent EvFilmPresetsHighlightHue;
    rtengine::ProcEvent EvFilmPresetsHighlightTint;
    rtengine::ProcEvent EvFilmPresetsHalation;
    rtengine::ProcEvent EvFilmPresetsRedShift;
    rtengine::ProcEvent EvFilmPresetsGreenShift;
    rtengine::ProcEvent EvFilmPresetsBlueShift;
    rtengine::ProcEvent EvFilmPresetsGrain;
    rtengine::ProcEvent EvFilmPresetsVibrance;

    void toggleDetail();
    void updateButtonLabel();
    void onEnableToggled();
    void onPresetHover(int idx);
    void onPresetLeave();
    void onPresetClick(int idx);
    void onLabOptionChanged(rtengine::ProcEvent event, Gtk::ComboBoxText* combo);
    void updateLabControlSensitivity();

    // Preset IDs and display names
    struct PresetInfo {
        const char* id;
        const char* langKey;
    };
    static const PresetInfo presetList[];
    static const int numPresets;

    int findPresetIndex(const Glib::ustring& id) const;
    Glib::ustring getPresetId(int index) const;

public:
    static const Glib::ustring TOOL_NAME;

    FilmPresets();
    ~FilmPresets() override;

    void read(const rtengine::procparams::ProcParams* pp, const ParamsEdited* pedited = nullptr) override;
    void write(rtengine::procparams::ProcParams* pp, ParamsEdited* pedited = nullptr) override;
    void setDefaults(const rtengine::procparams::ProcParams* defParams, const ParamsEdited* pedited = nullptr) override;
    void setBatchMode(bool batchMode) override;

    void adjusterChanged(Adjuster* a, double newval) override;
    void enabledChanged() override;

    void selectPreset(const Glib::ustring& presetId);
    Adjuster* getStrengthSlider() { return strength; }
};
