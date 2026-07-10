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
#include "filmpresets.h"

#include "eventmapper.h"

#include "rtengine/procparams.h"

using namespace rtengine;
using namespace rtengine::procparams;

namespace
{

int clampFilmValue(int value, int low, int high)
{
    return value < low ? low : (value > high ? high : value);
}

int wrapFilmHue(int value)
{
    value %= 360;
    return value < 0 ? value + 360 : value;
}

} // namespace

const Glib::ustring FilmPresets::TOOL_NAME = "filmpresets";

const FilmPresets::PresetInfo FilmPresets::presetList[] = {
    {"custom",          "TP_FILMPRESETS_CUSTOM"},
    {"heritage_gold",   "TP_FILMPRESETS_HERITAGE_GOLD"},
    {"porcelain_400",   "TP_FILMPRESETS_PORCELAIN_400"},
    {"vivid_chrome",    "TP_FILMPRESETS_VIVID_CHROME"},
    {"arctic",          "TP_FILMPRESETS_ARCTIC"},
    {"sovereign",       "TP_FILMPRESETS_SOVEREIGN"},
    {"golden_hour",     "TP_FILMPRESETS_GOLDEN_HOUR"},
    {"twilight_160",    "TP_FILMPRESETS_TWILIGHT_160"},
    {"nostalgia_200",   "TP_FILMPRESETS_NOSTALGIA_200"},
    {"desert_chrome",   "TP_FILMPRESETS_DESERT_CHROME"},
    {"street_800",      "TP_FILMPRESETS_STREET_800"},
    {"cinematic_500t",  "TP_FILMPRESETS_CINEMATIC_500T"},
    {"cinema_reveal_35", "TP_FILMPRESETS_CINEMA_REVEAL_35"},
    {"fade_bloom",      "TP_FILMPRESETS_FADE_BLOOM"},
    {"ember",           "TP_FILMPRESETS_EMBER"},
    {"silver_gelatin",  "TP_FILMPRESETS_SILVER_GELATIN"},
    {"analog_dream",    "TP_FILMPRESETS_ANALOG_DREAM"},
};

const int FilmPresets::numPresets = sizeof(presetList) / sizeof(presetList[0]);

FilmPresets::~FilmPresets()
{
    delete customDialog_;
}

FilmPresets::FilmPresets() :
    FoldableToolPanel(this, TOOL_NAME, M("TP_FILMPRESETS_LABEL"), false, true),
    activePresetIdx_(0),
    hoverPresetIdx_(-1),
    detailExpanded_(false),
    customDialog_(nullptr),
    dialogScrolled_(nullptr),
    dialogViewport_(nullptr)
{
    auto m = ProcEventMapper::getInstance();
    EvFilmPresetsEnabled      = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_FILMPRESETS_ENABLED");
    EvFilmPresetsPreset       = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_FILMPRESETS_PRESET");
    EvFilmPresetsStrength     = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_FILMPRESETS_STRENGTH");
    EvFilmPresetsModel        = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_FILMPRESETS_MODEL");
    EvFilmPresetsExposure     = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_FILMPRESETS_EXPOSURE");
    EvFilmPresetsPushPull     = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_FILMPRESETS_PUSHPULL");
    EvFilmPresetsProcess      = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_FILMPRESETS_PROCESS");
    EvFilmPresetsOutput       = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_FILMPRESETS_OUTPUT");
    EvFilmPresetsFormat       = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_FILMPRESETS_FORMAT");
    EvFilmPresetsContrast     = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_FILMPRESETS_CONTRAST");
    EvFilmPresetsSaturation   = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_FILMPRESETS_SATURATION");
    EvFilmPresetsWarmth       = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_FILMPRESETS_WARMTH");
    EvFilmPresetsTint         = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_FILMPRESETS_TINT");
    EvFilmPresetsFade         = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_FILMPRESETS_FADE");
    EvFilmPresetsRolloff      = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_FILMPRESETS_ROLLOFF");
    EvFilmPresetsShadowHue    = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_FILMPRESETS_SHADOWHUE");
    EvFilmPresetsShadowTint   = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_FILMPRESETS_SHADOWTINT");
    EvFilmPresetsHighlightHue = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_FILMPRESETS_HIGHLIGHTHUE");
    EvFilmPresetsHighlightTint= m->newEvent(LUMINANCECURVE, "HISTORY_MSG_FILMPRESETS_HIGHLIGHTTINT");
    EvFilmPresetsHalation     = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_FILMPRESETS_HALATION");
    EvFilmPresetsRedShift     = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_FILMPRESETS_REDSHIFT");
    EvFilmPresetsGreenShift   = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_FILMPRESETS_GREENSHIFT");
    EvFilmPresetsBlueShift    = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_FILMPRESETS_BLUESHIFT");
    EvFilmPresetsGrain        = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_FILMPRESETS_GRAIN");
    EvFilmPresetsVibrance     = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_FILMPRESETS_VIBRANCE");

    // --- Label + enable checkbox + preset dropdown on same row ---
    auto* headerLabel = Gtk::manage(new Gtk::Label(M("TP_FILMPRESETS_LABEL")));
    headerLabel->set_halign(Gtk::ALIGN_START);

    enableCheck_ = Gtk::manage(new Gtk::CheckButton());
    enableCheck_->set_active(false);
    enableConn_ = enableCheck_->signal_toggled().connect(
        sigc::mem_fun(*this, &FilmPresets::onEnableToggled));

    // --- Preset button + popover dropdown with hover-to-preview ---
    presetButton_ = Gtk::manage(new Gtk::Button(M("TP_FILMPRESETS_CUSTOM")));
    presetButton_->set_halign(Gtk::ALIGN_FILL);
    presetButton_->signal_clicked().connect([this]() {
        if (presetPopover_->is_visible()) {
            presetPopover_->popdown();
        } else {
            // Highlight the active preset when opening
            auto* row = presetListBox_->get_row_at_index(activePresetIdx_);
            if (row) {
                presetListBox_->select_row(*row);
            }
            presetPopover_->popup();
        }
    });

    presetPopover_ = Gtk::manage(new Gtk::Popover(*presetButton_));
    presetPopover_->set_position(Gtk::POS_BOTTOM);

    auto* scrolled = Gtk::manage(new Gtk::ScrolledWindow());
    scrolled->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    scrolled->set_max_content_height(420);
    scrolled->set_propagate_natural_height(true);

    presetListBox_ = Gtk::manage(new Gtk::ListBox());
    presetListBox_->set_selection_mode(Gtk::SELECTION_BROWSE);
    presetListBox_->set_activate_on_single_click(true);

    for (int i = 0; i < numPresets; ++i) {
        auto* row = Gtk::manage(new Gtk::ListBoxRow());
        auto* label = Gtk::manage(new Gtk::Label(M(presetList[i].langKey)));
        label->set_halign(Gtk::ALIGN_START);
        label->set_margin_start(6);
        label->set_margin_end(12);
        label->set_margin_top(3);
        label->set_margin_bottom(3);
        row->add(*label);
        presetListBox_->append(*row);
    }

    // Hover preview: track mouse position over the listbox rows
    presetListBox_->add_events(Gdk::POINTER_MOTION_MASK | Gdk::LEAVE_NOTIFY_MASK);
    presetListBox_->signal_motion_notify_event().connect([this](GdkEventMotion* ev) -> bool {
        auto* row = presetListBox_->get_row_at_y(static_cast<int>(ev->y));
        if (row) {
            int idx = row->get_index();
            if (idx != hoverPresetIdx_ && idx >= 0 && idx < numPresets) {
                onPresetHover(idx);
            }
        }
        return false;
    });

    presetListBox_->signal_leave_notify_event().connect([this](GdkEventCrossing* ev) -> bool {
        // Only revert when truly leaving the listbox (not entering a child widget)
        if (ev->detail != GDK_NOTIFY_INFERIOR) {
            onPresetLeave();
        }
        return false;
    });

    // Click to commit selection
    presetListBox_->signal_row_activated().connect([this](Gtk::ListBoxRow* row) {
        onPresetClick(row->get_index());
    });

    // Popover closed without clicking a row (e.g. click outside) — revert hover
    presetPopover_->signal_closed().connect([this]() {
        if (hoverPresetIdx_ >= 0) {
            onPresetLeave();
        }
    });

    scrolled->add(*presetListBox_);
    presetPopover_->add(*scrolled);
    presetPopover_->show_all_children();

    // Strength slider (always visible in summary)
    strength = Gtk::manage(new Adjuster(M("TP_FILMPRESETS_STRENGTH"), 0., 100., 1., 0.));
    strength->setAdjusterListener(this);
    strength->setLabel(Glib::ustring("\u25B8 ") + M("TP_FILMPRESETS_STRENGTH"));
    strength->setLabelClickCallback([this]() { toggleDetail(); });

    auto* headerRow = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 4));
    headerRow->pack_start(*headerLabel, Gtk::PACK_SHRINK, 0);
    headerRow->pack_start(*enableCheck_, Gtk::PACK_SHRINK, 0);
    headerRow->pack_start(*presetButton_, Gtk::PACK_EXPAND_WIDGET, 0);

    auto* summaryBox = getSummaryBox();
    summaryBox->pack_start(*headerRow, Gtk::PACK_SHRINK);
    summaryBox->pack_start(*strength);

    // Detail section
    detailContent_ = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));

    auto* labLabel = Gtk::manage(new Gtk::Label());
    labLabel->set_markup("<small>" + M("TP_FILMPRESETS_SECTION_LAB") + "</small>");
    labLabel->set_halign(Gtk::ALIGN_START);
    labLabel->get_style_context()->add_class("section-label");
    detailContent_->pack_start(*labLabel, Gtk::PACK_SHRINK, 4);

    auto* labGrid = Gtk::manage(new Gtk::Grid());
    labGrid->set_column_spacing(6);
    labGrid->set_row_spacing(3);
    labGrid->set_hexpand(true);

    modelCombo_ = Gtk::manage(new Gtk::ComboBoxText());
    modelCombo_->append("legacy", M("TP_FILMPRESETS_MODEL_LEGACY"));
    modelCombo_->append("v2", M("TP_FILMPRESETS_MODEL_V2"));
    processCombo_ = Gtk::manage(new Gtk::ComboBoxText());
    processCombo_->append("auto", M("TP_FILMPRESETS_PROCESS_AUTO"));
    processCombo_->append("c41", "C-41");
    processCombo_->append("e6", "E-6");
    processCombo_->append("ecn2", "ECN-2");
    processCombo_->append("bw", M("TP_FILMPRESETS_PROCESS_BW"));
    outputCombo_ = Gtk::manage(new Gtk::ComboBoxText());
    outputCombo_->append("scan", M("TP_FILMPRESETS_OUTPUT_SCAN"));
    outputCombo_->append("ra4", M("TP_FILMPRESETS_OUTPUT_RA4"));
    outputCombo_->append("projection", M("TP_FILMPRESETS_OUTPUT_PROJECTION"));
    outputCombo_->append("cinema", M("TP_FILMPRESETS_OUTPUT_CINEMA"));
    formatCombo_ = Gtk::manage(new Gtk::ComboBoxText());
    formatCombo_->append("35mm", "35 mm");
    formatCombo_->append("120", "120");
    formatCombo_->append("large", M("TP_FILMPRESETS_FORMAT_LARGE"));

    Gtk::ComboBoxText* labCombos[] = {modelCombo_, processCombo_, outputCombo_, formatCombo_};
    const char* labComboLabels[] = {
        "TP_FILMPRESETS_MODEL",
        "TP_FILMPRESETS_PROCESS",
        "TP_FILMPRESETS_OUTPUT",
        "TP_FILMPRESETS_FORMAT"
    };
    for (int row = 0; row < 4; ++row) {
        auto* label = Gtk::manage(new Gtk::Label(M(labComboLabels[row])));
        label->set_halign(Gtk::ALIGN_START);
        labCombos[row]->set_hexpand(true);
        labCombos[row]->set_halign(Gtk::ALIGN_FILL);
        labGrid->attach(*label, 0, row, 1, 1);
        labGrid->attach(*labCombos[row], 1, row, 1, 1);
    }
    detailContent_->pack_start(*labGrid, Gtk::PACK_SHRINK, 2);

    modelCombo_->signal_changed().connect([this]() { onLabOptionChanged(EvFilmPresetsModel, modelCombo_); });
    processCombo_->signal_changed().connect([this]() { onLabOptionChanged(EvFilmPresetsProcess, processCombo_); });
    outputCombo_->signal_changed().connect([this]() { onLabOptionChanged(EvFilmPresetsOutput, outputCombo_); });
    formatCombo_->signal_changed().connect([this]() { onLabOptionChanged(EvFilmPresetsFormat, formatCombo_); });

    exposureAdj_ = Gtk::manage(new Adjuster(M("TP_FILMPRESETS_EXPOSURE"), -4., 4., 0.1, 0.));
    pushPullAdj_ = Gtk::manage(new Adjuster(M("TP_FILMPRESETS_PUSHPULL"), -2., 3., 0.1, 0.));
    exposureAdj_->setAdjusterListener(this);
    pushPullAdj_->setAdjusterListener(this);
    detailContent_->pack_start(*exposureAdj_);
    detailContent_->pack_start(*pushPullAdj_);

    // -- Tone --
    auto* toneLabel = Gtk::manage(new Gtk::Label());
    toneLabel->set_markup("<small>" + M("TP_FILMPRESETS_SECTION_TONE") + "</small>");
    toneLabel->set_halign(Gtk::ALIGN_START);
    toneLabel->get_style_context()->add_class("section-label");
    detailContent_->pack_start(*toneLabel, Gtk::PACK_SHRINK, 4);

    contrast = Gtk::manage(new Adjuster(M("TP_FILMPRESETS_CONTRAST"), -100., 100., 1., 0.));
    fade = Gtk::manage(new Adjuster(M("TP_FILMPRESETS_FADE"), -100., 100., 1., 0.));
    rolloff = Gtk::manage(new Adjuster(M("TP_FILMPRESETS_ROLLOFF"), -100., 100., 1., 0.));
    contrast->setAdjusterListener(this);
    fade->setAdjusterListener(this);
    rolloff->setAdjusterListener(this);
    detailContent_->pack_start(*contrast);
    detailContent_->pack_start(*fade);
    detailContent_->pack_start(*rolloff);

    // -- Color --
    auto* colorLabel = Gtk::manage(new Gtk::Label());
    colorLabel->set_markup("<small>" + M("TP_FILMPRESETS_SECTION_COLOR") + "</small>");
    colorLabel->set_halign(Gtk::ALIGN_START);
    colorLabel->get_style_context()->add_class("section-label");
    detailContent_->pack_start(*colorLabel, Gtk::PACK_SHRINK, 4);

    warmth = Gtk::manage(new Adjuster(M("TP_FILMPRESETS_WARMTH"), -100., 100., 1., 0.));
    tintAdj = Gtk::manage(new Adjuster(M("TP_FILMPRESETS_TINT"), -100., 100., 1., 0.));
    saturation = Gtk::manage(new Adjuster(M("TP_FILMPRESETS_SATURATION"), -100., 100., 1., 0.));
    warmth->setAdjusterListener(this);
    tintAdj->setAdjusterListener(this);
    saturation->setAdjusterListener(this);
    detailContent_->pack_start(*warmth);
    detailContent_->pack_start(*tintAdj);
    detailContent_->pack_start(*saturation);

    // -- Tinting --
    auto* tintingLabel = Gtk::manage(new Gtk::Label());
    tintingLabel->set_markup("<small>" + M("TP_FILMPRESETS_SECTION_TINTING") + "</small>");
    tintingLabel->set_halign(Gtk::ALIGN_START);
    tintingLabel->get_style_context()->add_class("section-label");
    detailContent_->pack_start(*tintingLabel, Gtk::PACK_SHRINK, 4);

    shadowHue = Gtk::manage(new Adjuster(M("TP_FILMPRESETS_SHADOWHUE"), 0., 360., 1., 220.));
    shadowTintAdj = Gtk::manage(new Adjuster(M("TP_FILMPRESETS_SHADOWTINT"), -100., 100., 1., 0.));
    highlightHue = Gtk::manage(new Adjuster(M("TP_FILMPRESETS_HIGHLIGHTHUE"), 0., 360., 1., 40.));
    highlightTintAdj = Gtk::manage(new Adjuster(M("TP_FILMPRESETS_HIGHLIGHTTINT"), -100., 100., 1., 0.));
    shadowHue->setAdjusterListener(this);
    shadowTintAdj->setAdjusterListener(this);
    highlightHue->setAdjusterListener(this);
    highlightTintAdj->setAdjusterListener(this);
    detailContent_->pack_start(*shadowHue);
    detailContent_->pack_start(*shadowTintAdj);
    detailContent_->pack_start(*highlightHue);
    detailContent_->pack_start(*highlightTintAdj);

    // -- Channel Response --
    auto* channelLabel = Gtk::manage(new Gtk::Label());
    channelLabel->set_markup("<small>" + M("TP_FILMPRESETS_SECTION_CHANNEL") + "</small>");
    channelLabel->set_halign(Gtk::ALIGN_START);
    channelLabel->get_style_context()->add_class("section-label");
    detailContent_->pack_start(*channelLabel, Gtk::PACK_SHRINK, 4);

    redShift = Gtk::manage(new Adjuster(M("TP_FILMPRESETS_REDSHIFT"), -100., 100., 1., 0.));
    greenShift = Gtk::manage(new Adjuster(M("TP_FILMPRESETS_GREENSHIFT"), -100., 100., 1., 0.));
    blueShift = Gtk::manage(new Adjuster(M("TP_FILMPRESETS_BLUESHIFT"), -100., 100., 1., 0.));
    redShift->setAdjusterListener(this);
    greenShift->setAdjusterListener(this);
    blueShift->setAdjusterListener(this);
    detailContent_->pack_start(*redShift);
    detailContent_->pack_start(*greenShift);
    detailContent_->pack_start(*blueShift);

    // -- Film Character --
    auto* filmCharLabel = Gtk::manage(new Gtk::Label());
    filmCharLabel->set_markup("<small>" + M("TP_FILMPRESETS_SECTION_FILM") + "</small>");
    filmCharLabel->set_halign(Gtk::ALIGN_START);
    filmCharLabel->get_style_context()->add_class("section-label");
    detailContent_->pack_start(*filmCharLabel, Gtk::PACK_SHRINK, 4);

    grainAdj = Gtk::manage(new Adjuster(M("TP_FILMPRESETS_GRAIN"), -100., 100., 1., 0.));
    grainAdj->setAdjusterListener(this);
    detailContent_->pack_start(*grainAdj);

    vibranceAdj = Gtk::manage(new Adjuster(M("TP_FILMPRESETS_VIBRANCE"), -100., 100., 1., 0.));
    vibranceAdj->setAdjusterListener(this);
    detailContent_->pack_start(*vibranceAdj);

    // -- Special --
    auto* specialLabel = Gtk::manage(new Gtk::Label());
    specialLabel->set_markup("<small>" + M("TP_FILMPRESETS_SECTION_SPECIAL") + "</small>");
    specialLabel->set_halign(Gtk::ALIGN_START);
    specialLabel->get_style_context()->add_class("section-label");
    detailContent_->pack_start(*specialLabel, Gtk::PACK_SHRINK, 4);

    halationAdj = Gtk::manage(new Adjuster(M("TP_FILMPRESETS_HALATION"), -100., 100., 1., 0.));
    halationAdj->setAdjusterListener(this);
    detailContent_->pack_start(*halationAdj);

    detailContent_->show_all();

    detailRevealer_ = Gtk::manage(new Gtk::Revealer());
    detailRevealer_->set_transition_type(Gtk::REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    detailRevealer_->set_transition_duration(200);
    detailRevealer_->set_reveal_child(false);
    detailRevealer_->add(*detailContent_);
    detailRevealer_->show();
    summaryBox->pack_start(*detailRevealer_, Gtk::PACK_SHRINK);

    summaryBox->show_all();
}

void FilmPresets::toggleDetail()
{
    detailExpanded_ = !detailExpanded_;
    strength->setLabel(Glib::ustring(detailExpanded_ ? "\u25BE " : "\u25B8 ") + M("TP_FILMPRESETS_STRENGTH"));
    detailRevealer_->set_reveal_child(detailExpanded_);
}

void FilmPresets::updateButtonLabel()
{
    if (activePresetIdx_ >= 0 && activePresetIdx_ < numPresets) {
        presetButton_->set_label(M(presetList[activePresetIdx_].langKey));
    }
}

int FilmPresets::findPresetIndex(const Glib::ustring& id) const
{
    for (int i = 0; i < numPresets; ++i) {
        if (id == presetList[i].id) {
            return i;
        }
    }
    return 0; // custom
}

Glib::ustring FilmPresets::getPresetId(int index) const
{
    if (index >= 0 && index < numPresets) {
        return presetList[index].id;
    }
    return "custom";
}

void FilmPresets::onPresetHover(int idx)
{
    if (idx == hoverPresetIdx_) return;
    hoverPresetIdx_ = idx;

    // Debounce: fire preview after 120ms of stable hover
    hoverTimeout_.disconnect();
    hoverTimeout_ = Glib::signal_timeout().connect([this]() -> bool {
        if (hoverPresetIdx_ >= 0 && listener && getEnabled()) {
            // Fire panelChanged — write() will output the hovered preset
            listener->panelChanged(EvFilmPresetsPreset, M(presetList[hoverPresetIdx_].langKey));
        }
        return false;
    }, 120);
}

void FilmPresets::onPresetLeave()
{
    hoverTimeout_.disconnect();
    if (hoverPresetIdx_ >= 0) {
        hoverPresetIdx_ = -1;
        // Revert preview to the committed preset
        if (listener && getEnabled()) {
            listener->panelChanged(EvFilmPresetsPreset, M(presetList[activePresetIdx_].langKey));
        }
    }
}

void FilmPresets::openCustomDialog()
{
    if (!customDialog_) {
        Gtk::Window* toplevel = dynamic_cast<Gtk::Window*>(get_toplevel());

        customDialog_ = new Gtk::Dialog(
            M("TP_FILMPRESETS_CUSTOM"),
            false
        );
        if (toplevel) {
            customDialog_->set_transient_for(*toplevel);
        }
        customDialog_->set_default_size(550, 700);
        customDialog_->set_type_hint(Gdk::WINDOW_TYPE_HINT_DIALOG);

        dialogScrolled_ = Gtk::manage(new Gtk::ScrolledWindow());
        dialogScrolled_->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
        dialogViewport_ = Gtk::manage(new Gtk::Viewport(
            Glib::RefPtr<Gtk::Adjustment>(),
            Glib::RefPtr<Gtk::Adjustment>()));
        dialogViewport_->set_shadow_type(Gtk::SHADOW_NONE);
        dialogScrolled_->add(*dialogViewport_);

        auto* contentArea = customDialog_->get_content_area();
        contentArea->pack_start(*dialogScrolled_, Gtk::PACK_EXPAND_WIDGET);

        customDialog_->signal_delete_event().connect([this](GdkEventAny*) -> bool {
            restoreCustomControls();
            customDialog_->hide();
            return true;
        });
    }

    if (customDialog_->is_visible()) {
        customDialog_->present();
        return;
    }

    // Keep one explicit viewport attached for the dialog's lifetime. Letting
    // GtkScrolledWindow create an implicit viewport leaves that viewport
    // behind when the controls are moved back, corrupting the next reopen.
    if (detailContent_->get_parent()) {
        detailContent_->get_parent()->remove(*detailContent_);
    }
    dialogViewport_->add(*detailContent_);
    detailContent_->show_all();
    customDialog_->show_all();
    customDialog_->present();
}

void FilmPresets::restoreCustomControls()
{
    if (!dialogViewport_ || detailContent_->get_parent() != dialogViewport_) {
        return;
    }

    detailContent_->hide();
    dialogViewport_->remove();
    detailRevealer_->add(*detailContent_);
    detailContent_->show_all();
}

void FilmPresets::onPresetClick(int idx)
{
    hoverTimeout_.disconnect();
    hoverPresetIdx_ = -1;
    activePresetIdx_ = idx;
    updateButtonLabel();
    presetPopover_->popdown();
    modelCombo_->set_active_id("v2");

    if (idx == 0) {
        // Custom: open dialog instead of inline toggle
        openCustomDialog();
    } else {
        // Close custom dialog if visible
        if (customDialog_ && customDialog_->is_visible()) {
            restoreCustomControls();
            customDialog_->hide();
        }
    }

    autoEnable();
    if (listener && getEnabled()) {
        listener->panelChanged(EvFilmPresetsPreset, M(presetList[activePresetIdx_].langKey));
    }
}

void FilmPresets::onLabOptionChanged(ProcEvent event, Gtk::ComboBoxText* combo)
{
    updateLabControlSensitivity();
    if (!listener) {
        return;
    }

    autoEnable();
    if (getEnabled()) {
        listener->panelChanged(event, combo->get_active_text());
    }
}

void FilmPresets::updateLabControlSensitivity()
{
    const bool filmLab = modelCombo_->get_active_id() != "legacy";
    processCombo_->set_sensitive(filmLab);
    outputCombo_->set_sensitive(filmLab);
    formatCombo_->set_sensitive(filmLab);
    exposureAdj_->set_sensitive(filmLab);
    pushPullAdj_->set_sensitive(filmLab);
}

void FilmPresets::selectPreset(const Glib::ustring& presetId)
{
    activePresetIdx_ = findPresetIndex(presetId);
    updateButtonLabel();

    if (listener && getEnabled()) {
        listener->panelChanged(EvFilmPresetsPreset, M(presetList[activePresetIdx_].langKey));
    }
}

void FilmPresets::read(const ProcParams* pp, const ParamsEdited* pedited)
{
    disableListener();

    if (pedited) {
        exposureAdj_->setEditedState(pedited->filmPresets.exposure ? Edited : UnEdited);
        pushPullAdj_->setEditedState(pedited->filmPresets.pushPull ? Edited : UnEdited);
        strength->setEditedState(pedited->filmPresets.strength ? Edited : UnEdited);
        contrast->setEditedState(pedited->filmPresets.contrast ? Edited : UnEdited);
        saturation->setEditedState(pedited->filmPresets.saturation ? Edited : UnEdited);
        warmth->setEditedState(pedited->filmPresets.warmth ? Edited : UnEdited);
        tintAdj->setEditedState(pedited->filmPresets.tint ? Edited : UnEdited);
        fade->setEditedState(pedited->filmPresets.fade ? Edited : UnEdited);
        rolloff->setEditedState(pedited->filmPresets.rolloff ? Edited : UnEdited);
        shadowHue->setEditedState(pedited->filmPresets.shadowHue ? Edited : UnEdited);
        shadowTintAdj->setEditedState(pedited->filmPresets.shadowTint ? Edited : UnEdited);
        highlightHue->setEditedState(pedited->filmPresets.highlightHue ? Edited : UnEdited);
        highlightTintAdj->setEditedState(pedited->filmPresets.highlightTint ? Edited : UnEdited);
        halationAdj->setEditedState(pedited->filmPresets.halation ? Edited : UnEdited);
        redShift->setEditedState(pedited->filmPresets.redShift ? Edited : UnEdited);
        greenShift->setEditedState(pedited->filmPresets.greenShift ? Edited : UnEdited);
        blueShift->setEditedState(pedited->filmPresets.blueShift ? Edited : UnEdited);
        grainAdj->setEditedState(pedited->filmPresets.grain ? Edited : UnEdited);
        vibranceAdj->setEditedState(pedited->filmPresets.vibrance ? Edited : UnEdited);
        set_inconsistent(multiImage && !pedited->filmPresets.enabled);
    }

    setEnabled(pp->filmPresets.enabled);
    enableConn_.block(true);
    enableCheck_->set_active(pp->filmPresets.enabled);
    enableConn_.block(false);

    activePresetIdx_ = findPresetIndex(pp->filmPresets.preset);
    updateButtonLabel();

    modelCombo_->set_active_id(pp->filmPresets.modelVersion < 2 ? "legacy" : "v2");
    if (!processCombo_->set_active_id(pp->filmPresets.process)) {
        processCombo_->set_active_id("auto");
    }
    if (!outputCombo_->set_active_id(pp->filmPresets.output)) {
        outputCombo_->set_active_id("scan");
    }
    if (!formatCombo_->set_active_id(pp->filmPresets.format)) {
        formatCombo_->set_active_id("35mm");
    }
    exposureAdj_->setValue(pp->filmPresets.exposure);
    pushPullAdj_->setValue(pp->filmPresets.pushPull);
    updateLabControlSensitivity();

    strength->setValue(clampFilmValue(pp->filmPresets.strength, 0, 100));
    contrast->setValue(clampFilmValue(pp->filmPresets.contrast, -100, 100));
    saturation->setValue(clampFilmValue(pp->filmPresets.saturation, -100, 100));
    warmth->setValue(clampFilmValue(pp->filmPresets.warmth, -100, 100));
    tintAdj->setValue(clampFilmValue(pp->filmPresets.tint, -100, 100));
    fade->setValue(clampFilmValue(pp->filmPresets.fade, -100, 100));
    rolloff->setValue(clampFilmValue(pp->filmPresets.rolloff, -100, 100));
    shadowHue->setValue(wrapFilmHue(pp->filmPresets.shadowHue));
    shadowTintAdj->setValue(clampFilmValue(pp->filmPresets.shadowTint, -100, 100));
    highlightHue->setValue(wrapFilmHue(pp->filmPresets.highlightHue));
    highlightTintAdj->setValue(clampFilmValue(pp->filmPresets.highlightTint, -100, 100));
    halationAdj->setValue(clampFilmValue(pp->filmPresets.halation, -100, 100));
    redShift->setValue(clampFilmValue(pp->filmPresets.redShift, -100, 100));
    greenShift->setValue(clampFilmValue(pp->filmPresets.greenShift, -100, 100));
    blueShift->setValue(clampFilmValue(pp->filmPresets.blueShift, -100, 100));
    grainAdj->setValue(clampFilmValue(pp->filmPresets.grain, -100, 100));
    vibranceAdj->setValue(clampFilmValue(pp->filmPresets.vibrance, -100, 100));

    enableListener();
}

void FilmPresets::write(ProcParams* pp, ParamsEdited* pedited)
{
    pp->filmPresets.enabled = getEnabled();

    // During hover preview, output the hovered preset; otherwise the committed one
    int effectiveIdx = (hoverPresetIdx_ >= 0) ? hoverPresetIdx_ : activePresetIdx_;
    pp->filmPresets.preset = getPresetId(effectiveIdx);

    pp->filmPresets.modelVersion = modelCombo_->get_active_id() == "legacy" ? 1 : 2;
    pp->filmPresets.exposure = exposureAdj_->getValue();
    pp->filmPresets.pushPull = pushPullAdj_->getValue();
    pp->filmPresets.process = processCombo_->get_active_id();
    pp->filmPresets.output = outputCombo_->get_active_id();
    pp->filmPresets.format = formatCombo_->get_active_id();

    pp->filmPresets.strength = strength->getValue();
    pp->filmPresets.contrast = contrast->getValue();
    pp->filmPresets.saturation = saturation->getValue();
    pp->filmPresets.warmth = warmth->getValue();
    pp->filmPresets.tint = tintAdj->getValue();
    pp->filmPresets.fade = fade->getValue();
    pp->filmPresets.rolloff = rolloff->getValue();
    pp->filmPresets.shadowHue = shadowHue->getValue();
    pp->filmPresets.shadowTint = shadowTintAdj->getValue();
    pp->filmPresets.highlightHue = highlightHue->getValue();
    pp->filmPresets.highlightTint = highlightTintAdj->getValue();
    pp->filmPresets.halation = halationAdj->getValue();
    pp->filmPresets.redShift = redShift->getValue();
    pp->filmPresets.greenShift = greenShift->getValue();
    pp->filmPresets.blueShift = blueShift->getValue();
    pp->filmPresets.grain = grainAdj->getValue();
    pp->filmPresets.vibrance = vibranceAdj->getValue();

    if (pedited) {
        pedited->filmPresets.enabled = !get_inconsistent();
        pedited->filmPresets.preset = true;
        pedited->filmPresets.modelVersion = true;
        pedited->filmPresets.exposure = exposureAdj_->getEditedState();
        pedited->filmPresets.pushPull = pushPullAdj_->getEditedState();
        pedited->filmPresets.process = true;
        pedited->filmPresets.output = true;
        pedited->filmPresets.format = true;
        pedited->filmPresets.strength = strength->getEditedState();
        pedited->filmPresets.contrast = contrast->getEditedState();
        pedited->filmPresets.saturation = saturation->getEditedState();
        pedited->filmPresets.warmth = warmth->getEditedState();
        pedited->filmPresets.tint = tintAdj->getEditedState();
        pedited->filmPresets.fade = fade->getEditedState();
        pedited->filmPresets.rolloff = rolloff->getEditedState();
        pedited->filmPresets.shadowHue = shadowHue->getEditedState();
        pedited->filmPresets.shadowTint = shadowTintAdj->getEditedState();
        pedited->filmPresets.highlightHue = highlightHue->getEditedState();
        pedited->filmPresets.highlightTint = highlightTintAdj->getEditedState();
        pedited->filmPresets.halation = halationAdj->getEditedState();
        pedited->filmPresets.redShift = redShift->getEditedState();
        pedited->filmPresets.greenShift = greenShift->getEditedState();
        pedited->filmPresets.blueShift = blueShift->getEditedState();
        pedited->filmPresets.grain = grainAdj->getEditedState();
        pedited->filmPresets.vibrance = vibranceAdj->getEditedState();
    }
}

void FilmPresets::setDefaults(const ProcParams* defParams, const ParamsEdited* pedited)
{
    exposureAdj_->setDefault(defParams->filmPresets.exposure);
    pushPullAdj_->setDefault(defParams->filmPresets.pushPull);
    strength->setDefault(defParams->filmPresets.strength);
    contrast->setDefault(defParams->filmPresets.contrast);
    saturation->setDefault(defParams->filmPresets.saturation);
    warmth->setDefault(defParams->filmPresets.warmth);
    tintAdj->setDefault(defParams->filmPresets.tint);
    fade->setDefault(defParams->filmPresets.fade);
    rolloff->setDefault(defParams->filmPresets.rolloff);
    shadowHue->setDefault(defParams->filmPresets.shadowHue);
    shadowTintAdj->setDefault(defParams->filmPresets.shadowTint);
    highlightHue->setDefault(defParams->filmPresets.highlightHue);
    highlightTintAdj->setDefault(defParams->filmPresets.highlightTint);
    halationAdj->setDefault(defParams->filmPresets.halation);
    redShift->setDefault(defParams->filmPresets.redShift);
    greenShift->setDefault(defParams->filmPresets.greenShift);
    blueShift->setDefault(defParams->filmPresets.blueShift);
    grainAdj->setDefault(defParams->filmPresets.grain);
    vibranceAdj->setDefault(defParams->filmPresets.vibrance);

    if (pedited) {
        exposureAdj_->setDefaultEditedState(pedited->filmPresets.exposure ? Edited : UnEdited);
        pushPullAdj_->setDefaultEditedState(pedited->filmPresets.pushPull ? Edited : UnEdited);
        strength->setDefaultEditedState(pedited->filmPresets.strength ? Edited : UnEdited);
        contrast->setDefaultEditedState(pedited->filmPresets.contrast ? Edited : UnEdited);
        saturation->setDefaultEditedState(pedited->filmPresets.saturation ? Edited : UnEdited);
        warmth->setDefaultEditedState(pedited->filmPresets.warmth ? Edited : UnEdited);
        tintAdj->setDefaultEditedState(pedited->filmPresets.tint ? Edited : UnEdited);
        fade->setDefaultEditedState(pedited->filmPresets.fade ? Edited : UnEdited);
        rolloff->setDefaultEditedState(pedited->filmPresets.rolloff ? Edited : UnEdited);
        shadowHue->setDefaultEditedState(pedited->filmPresets.shadowHue ? Edited : UnEdited);
        shadowTintAdj->setDefaultEditedState(pedited->filmPresets.shadowTint ? Edited : UnEdited);
        highlightHue->setDefaultEditedState(pedited->filmPresets.highlightHue ? Edited : UnEdited);
        highlightTintAdj->setDefaultEditedState(pedited->filmPresets.highlightTint ? Edited : UnEdited);
        halationAdj->setDefaultEditedState(pedited->filmPresets.halation ? Edited : UnEdited);
        redShift->setDefaultEditedState(pedited->filmPresets.redShift ? Edited : UnEdited);
        greenShift->setDefaultEditedState(pedited->filmPresets.greenShift ? Edited : UnEdited);
        blueShift->setDefaultEditedState(pedited->filmPresets.blueShift ? Edited : UnEdited);
        grainAdj->setDefaultEditedState(pedited->filmPresets.grain ? Edited : UnEdited);
        vibranceAdj->setDefaultEditedState(pedited->filmPresets.vibrance ? Edited : UnEdited);
    } else {
        exposureAdj_->setDefaultEditedState(Irrelevant);
        pushPullAdj_->setDefaultEditedState(Irrelevant);
        strength->setDefaultEditedState(Irrelevant);
        contrast->setDefaultEditedState(Irrelevant);
        saturation->setDefaultEditedState(Irrelevant);
        warmth->setDefaultEditedState(Irrelevant);
        tintAdj->setDefaultEditedState(Irrelevant);
        fade->setDefaultEditedState(Irrelevant);
        rolloff->setDefaultEditedState(Irrelevant);
        shadowHue->setDefaultEditedState(Irrelevant);
        shadowTintAdj->setDefaultEditedState(Irrelevant);
        highlightHue->setDefaultEditedState(Irrelevant);
        highlightTintAdj->setDefaultEditedState(Irrelevant);
        halationAdj->setDefaultEditedState(Irrelevant);
        redShift->setDefaultEditedState(Irrelevant);
        greenShift->setDefaultEditedState(Irrelevant);
        blueShift->setDefaultEditedState(Irrelevant);
        grainAdj->setDefaultEditedState(Irrelevant);
        vibranceAdj->setDefaultEditedState(Irrelevant);
    }
}

void FilmPresets::adjusterChanged(Adjuster* a, double newval)
{
    autoEnable();

    // Cancel any active hover preview
    if (hoverPresetIdx_ >= 0) {
        hoverTimeout_.disconnect();
        hoverPresetIdx_ = -1;
    }

    if (listener && getEnabled()) {
        if (a == exposureAdj_) {
            listener->panelChanged(EvFilmPresetsExposure, a->getTextValue());
        } else if (a == pushPullAdj_) {
            listener->panelChanged(EvFilmPresetsPushPull, a->getTextValue());
        } else if (a == strength) {
            listener->panelChanged(EvFilmPresetsStrength, a->getTextValue());
        } else if (a == contrast) {
            listener->panelChanged(EvFilmPresetsContrast, a->getTextValue());
        } else if (a == saturation) {
            listener->panelChanged(EvFilmPresetsSaturation, a->getTextValue());
        } else if (a == warmth) {
            listener->panelChanged(EvFilmPresetsWarmth, a->getTextValue());
        } else if (a == tintAdj) {
            listener->panelChanged(EvFilmPresetsTint, a->getTextValue());
        } else if (a == fade) {
            listener->panelChanged(EvFilmPresetsFade, a->getTextValue());
        } else if (a == rolloff) {
            listener->panelChanged(EvFilmPresetsRolloff, a->getTextValue());
        } else if (a == shadowHue) {
            listener->panelChanged(EvFilmPresetsShadowHue, a->getTextValue());
        } else if (a == shadowTintAdj) {
            listener->panelChanged(EvFilmPresetsShadowTint, a->getTextValue());
        } else if (a == highlightHue) {
            listener->panelChanged(EvFilmPresetsHighlightHue, a->getTextValue());
        } else if (a == highlightTintAdj) {
            listener->panelChanged(EvFilmPresetsHighlightTint, a->getTextValue());
        } else if (a == halationAdj) {
            listener->panelChanged(EvFilmPresetsHalation, a->getTextValue());
        } else if (a == redShift) {
            listener->panelChanged(EvFilmPresetsRedShift, a->getTextValue());
        } else if (a == greenShift) {
            listener->panelChanged(EvFilmPresetsGreenShift, a->getTextValue());
        } else if (a == blueShift) {
            listener->panelChanged(EvFilmPresetsBlueShift, a->getTextValue());
        } else if (a == grainAdj) {
            listener->panelChanged(EvFilmPresetsGrain, a->getTextValue());
        } else if (a == vibranceAdj) {
            listener->panelChanged(EvFilmPresetsVibrance, a->getTextValue());
        }
    }
}

void FilmPresets::onEnableToggled()
{
    setEnabled(enableCheck_->get_active());
    enabledChanged();
}

void FilmPresets::enabledChanged()
{
    // Sync visible checkbox with internal enabled state
    enableConn_.block(true);
    enableCheck_->set_active(getEnabled());
    enableConn_.block(false);

    if (listener) {
        if (get_inconsistent()) {
            listener->panelChanged(EvFilmPresetsEnabled, M("GENERAL_UNCHANGED"));
        } else if (getEnabled()) {
            listener->panelChanged(EvFilmPresetsEnabled, M("GENERAL_ENABLED"));
        } else {
            listener->panelChanged(EvFilmPresetsEnabled, M("GENERAL_DISABLED"));
        }
    }
}

void FilmPresets::setBatchMode(bool batchMode)
{
    ToolPanel::setBatchMode(batchMode);

    strength->showEditedCB();
    exposureAdj_->showEditedCB();
    pushPullAdj_->showEditedCB();
    contrast->showEditedCB();
    saturation->showEditedCB();
    warmth->showEditedCB();
    tintAdj->showEditedCB();
    fade->showEditedCB();
    rolloff->showEditedCB();
    shadowHue->showEditedCB();
    shadowTintAdj->showEditedCB();
    highlightHue->showEditedCB();
    highlightTintAdj->showEditedCB();
    halationAdj->showEditedCB();
    redShift->showEditedCB();
    greenShift->showEditedCB();
    blueShift->showEditedCB();
    grainAdj->showEditedCB();
    vibranceAdj->showEditedCB();
}
