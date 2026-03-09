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
 *  GNU General Public License for more details
 *
 *  You should have received a copy of the GNU General Public License
 *  along with RawTherapee.  If not, see <https://www.gnu.org/licenses/>.
 */
#include <iomanip>
#include <cmath>

#include "blackwhite.h"

#include "guiutils.h"
#include "options.h"
#include "rtimage.h"
#include "widgets/curves/curveeditor.h"
#include "widgets/curves/curveeditorgroup.h"

#include "rtengine/color.h"
#include "rtengine/procparams.h"
#include "rtengine/utils.h"

using namespace rtengine;
using namespace rtengine::procparams;

const Glib::ustring BlackWhite::TOOL_NAME = "blackwhite";

BlackWhite::BlackWhite (): FoldableToolPanel(this, TOOL_NAME, M("TP_BWMIX_LABEL"), false, true)
{
    CurveListener::setMulti(true);

    nextredbw = 0.3333;
    nextgreenbw = 0.3333;
    nextbluebw = 0.3333;

    //----------- Method combobox ------------------------------

    Gtk::Box* metBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 4));
    setExpandAlignProperties(metBox, false, false, Gtk::ALIGN_START, Gtk::ALIGN_CENTER);
    metBox->set_vexpand(false);
    Gtk::Label* metLabel = Gtk::manage(new Gtk::Label(M("TP_BWMIX_MET")));
    metLabel->set_line_wrap(false);
    metLabel->set_valign(Gtk::ALIGN_CENTER);

	method = Gtk::manage (new MyComboBoxText ());
    method->append (M("GENERAL_DISABLED"));
    method->append (M("TP_BWMIX_MET_DESAT"));
    method->append (M("TP_BWMIX_MET_LUMEQUAL"));
    method->append (M("TP_BWMIX_MET_CHANMIX"));

    method->set_active (0);
    setExpandAlignProperties(method, false, false, Gtk::ALIGN_START, Gtk::ALIGN_CENTER);
    method->set_size_request(90, 18);
    // Center text vertically in combobox
    auto cells = method->get_cells();
    for (auto* cell : cells) {
        cell->set_alignment(0.0, 0.5);
        cell->set_padding(2, 0);
    }
    metBox->pack_start(*metLabel, false, false, 0);
    metBox->pack_start(*method, false, false, 0);
    getSummaryBox()->pack_start(*metBox, false, false, 0);

    getSummaryBox()->show_all();
    methodconn = method->signal_changed().connect ( sigc::mem_fun(*this, &BlackWhite::methodChanged) );

    //----------- Neutrals / Tone / Strength sliders --------------

    bwNeutrals = Gtk::manage(new Adjuster(M("TP_BWMIX_NEUTRALS"), -100, 100, 1, 0));
    bwNeutrals->setAdjusterListener(this);
    getSummaryBox()->pack_start(*bwNeutrals, Gtk::PACK_SHRINK, 0);

    bwTone = Gtk::manage(new Adjuster(M("TP_BWMIX_TONE"), -100, 100, 1, 0));
    bwTone->setAdjusterListener(this);
    getSummaryBox()->pack_start(*bwTone, Gtk::PACK_SHRINK, 0);

    bwStrength = Gtk::manage(new Adjuster(M("TP_BWMIX_STRENGTH"), 0, 100, 1, 100));
    bwStrength->setAdjusterListener(this);
    getSummaryBox()->pack_start(*bwStrength, Gtk::PACK_SHRINK, 0);

    //----------- Hidden widgets needed for read/write compatibility ------
    // These are created but never packed into any visible container.
    // They exist so that read()/write()/setDefaults() etc. don't crash.

    luminanceSep = nullptr;

    auto& options = App::get().mut_options();
    luminanceCEG = new CurveEditorGroup (options.lastBWCurvesDir, M("TP_BWMIX_CHANNEL"));
    luminanceCEG->setCurveListener (this);
    luminanceCurve = static_cast<FlatCurveEditor*>(luminanceCEG->addCurve(CT_Flat, M("TP_BWMIX_VAL")));
    luminanceCEG->curveListComplete();

    mixerFrame = nullptr;
    mixerVBox = Gtk::manage (new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
    autoHBox = Gtk::manage (new Gtk::Box ());
    autoch = Gtk::manage (new Gtk::ToggleButton (M("TP_BWMIX_AUTOCH")));
    autoconn = autoch->signal_toggled().connect( sigc::mem_fun(*this, &BlackWhite::autoch_toggled) );
    neutral = Gtk::manage (new Gtk::Button (M("TP_BWMIX_NEUTRAL")));
    neutralconn = neutral->signal_pressed().connect( sigc::mem_fun(*this, &BlackWhite::neutral_pressed) );

    settingHBox = Gtk::manage (new Gtk::Box ());
    setting = Gtk::manage (new MyComboBoxText ());
    setting->append (M("TP_BWMIX_SET_NORMCONTAST"));
    setting->append (M("TP_BWMIX_SET_HIGHCONTAST"));
    setting->append (M("TP_BWMIX_SET_LUMINANCE"));
    setting->append (M("TP_BWMIX_SET_LANDSCAPE"));
    setting->append (M("TP_BWMIX_SET_PORTRAIT"));
    setting->append (M("TP_BWMIX_SET_LOWSENSIT"));
    setting->append (M("TP_BWMIX_SET_HIGHSENSIT"));
    setting->append (M("TP_BWMIX_SET_PANCHRO"));
    setting->append (M("TP_BWMIX_SET_HYPERPANCHRO"));
    setting->append (M("TP_BWMIX_SET_ORTHOCHRO"));
    setting->append (M("TP_BWMIX_SET_RGBABS"));
    setting->append (M("TP_BWMIX_SET_RGBREL"));
    setting->append (M("TP_BWMIX_SET_ROYGCBPMABS"));
    setting->append (M("TP_BWMIX_SET_ROYGCBPMREL"));
    setting->append (M("TP_BWMIX_SET_INFRARED"));
    setting->set_active (11);
    settingconn = setting->signal_changed().connect ( sigc::mem_fun(*this, &BlackWhite::settingChanged) );

    RGBLabels = Gtk::manage(new Gtk::Label("---", Gtk::ALIGN_CENTER));

    enabledccSep = nullptr;
    enabledcc = Gtk::manage (new Gtk::CheckButton (M("TP_BWMIX_CC_ENABLED")));
    enabledcc->set_active (true);
    enaccconn = enabledcc->signal_toggled().connect( sigc::mem_fun(*this, &BlackWhite::enabledcc_toggled) );

    filterSep = nullptr;
    filterSep2 = nullptr;
    filterHBox = Gtk::manage (new Gtk::Box ());
    filter = Gtk::manage (new MyComboBoxText ());
    filter->append (M("TP_BWMIX_FILTER_NONE"));
    filter->append (M("TP_BWMIX_FILTER_RED"));
    filter->append (M("TP_BWMIX_FILTER_REDYELLOW"));
    filter->append (M("TP_BWMIX_FILTER_YELLOW"));
    filter->append (M("TP_BWMIX_FILTER_GREENYELLOW"));
    filter->append (M("TP_BWMIX_FILTER_GREEN"));
    filter->append (M("TP_BWMIX_FILTER_BLUEGREEN"));
    filter->append (M("TP_BWMIX_FILTER_BLUE"));
    filter->append (M("TP_BWMIX_FILTER_PURPLE"));
    filter->set_active (0);
    filterconn = filter->signal_changed().connect ( sigc::mem_fun(*this, &BlackWhite::filterChanged) );

    imgIcon[0] = Gtk::manage (new RTImage ("circle-red-small"));
    imgIcon[1] = Gtk::manage (new RTImage ("circle-orange-small"));
    imgIcon[2] = Gtk::manage (new RTImage ("circle-yellow-small"));
    imgIcon[3] = Gtk::manage (new RTImage ("circle-green-small"));
    imgIcon[4] = Gtk::manage (new RTImage ("circle-cyan-small"));
    imgIcon[5] = Gtk::manage (new RTImage ("circle-blue-small"));
    imgIcon[6] = Gtk::manage (new RTImage ("circle-purple-small"));
    imgIcon[7] = Gtk::manage (new RTImage ("circle-magenta-small"));
    imgIcon[8]  = Gtk::manage (new RTImage ("circle-empty-red-small"));
    imgIcon[9]  = Gtk::manage (new RTImage ("circle-empty-green-small"));
    imgIcon[10] = Gtk::manage (new RTImage ("circle-empty-blue-small"));

    mixerRed = Gtk::manage(new Adjuster ("", -100, 200, 1, 33, imgIcon[0]));
    mixerRed->setAdjusterListener (this);
    mixerGreen = Gtk::manage(new Adjuster ("", -100, 200, 1, 33, imgIcon[3]));
    mixerGreen->setAdjusterListener (this);
    mixerBlue = Gtk::manage(new Adjuster ("", -100, 200, 1, 33, imgIcon[5]));
    mixerBlue->setAdjusterListener (this);
    mixerOrange = Gtk::manage(new Adjuster ("", -100, 200, 1, 33, imgIcon[1]));
    mixerOrange->setAdjusterListener (this);
    mixerYellow = Gtk::manage(new Adjuster ("", -100, 200, 1, 33, imgIcon[2]));
    mixerYellow->setAdjusterListener (this);
    mixerCyan = Gtk::manage(new Adjuster ("", -100, 200, 1, 33, imgIcon[4]));
    mixerCyan->setAdjusterListener (this);
    mixerPurple = Gtk::manage(new Adjuster ("", -100, 200, 1, 33, imgIcon[6]));
    mixerPurple->setAdjusterListener (this);
    mixerMagenta = Gtk::manage(new Adjuster ("", -100, 200, 1, 33, imgIcon[7]));
    mixerMagenta->setAdjusterListener (this);

    algoHBox = Gtk::manage (new Gtk::Box ());
    alLabel = nullptr;
    algo = Gtk::manage (new MyComboBoxText ());
    algo->append (M("TP_BWMIX_ALGO_LI"));
    algo->append (M("TP_BWMIX_ALGO_SP"));
    algo->set_active (1);
    algoconn = algo->signal_changed().connect ( sigc::mem_fun(*this, &BlackWhite::algoChanged) );

    gammaFrame = nullptr;
    gammaRed = Gtk::manage(new Adjuster ("", -100, 100, 1, 0, imgIcon[8]));
    gammaRed->setAdjusterListener (this);
    gammaGreen = Gtk::manage(new Adjuster ("", -100, 100, 1, 0, imgIcon[9]));
    gammaGreen->setAdjusterListener (this);
    gammaBlue = Gtk::manage(new Adjuster ("", -100, 100, 1, 0, imgIcon[10]));
    gammaBlue->setAdjusterListener (this);

    beforeCurveMode = Gtk::manage (new MyComboBoxText ());
    beforeCurveMode->append (M("TP_BWMIX_TCMODE_STANDARD"));
    beforeCurveMode->append (M("TP_BWMIX_TCMODE_WEIGHTEDSTD"));
    beforeCurveMode->append (M("TP_BWMIX_TCMODE_FILMLIKE"));
    beforeCurveMode->append (M("TP_BWMIX_TCMODE_SATANDVALBLENDING"));
    beforeCurveMode->set_active (0);

    beforeCurveCEG = new CurveEditorGroup (options.lastBWCurvesDir, M("TP_BWMIX_CURVEEDITOR1"));
    beforeCurveCEG->setCurveListener (this);
    beforeCurve = static_cast<DiagonalCurveEditor*>(beforeCurveCEG->addCurve(CT_Diagonal, "", beforeCurveMode));
    beforeCurveCEG->curveListComplete();
    tcmodeconn = beforeCurveMode->signal_changed().connect( sigc::mem_fun(*this, &BlackWhite::curveMode1Changed), true );

    afterCurveCEG = new CurveEditorGroup (options.lastBWCurvesDir, M("TP_BWMIX_CURVEEDITOR2"));
    afterCurveCEG->setCurveListener (this);
    afterCurve = static_cast<DiagonalCurveEditor*>(afterCurveCEG->addCurve(CT_Diagonal, ""));
    afterCurveCEG->curveListComplete();

    show_all();

    disableListener();
    methodChanged();
    enableListener();
}
BlackWhite::~BlackWhite ()
{
    idle_register.destroy();

    delete luminanceCEG;
    delete beforeCurveCEG;
    delete afterCurveCEG;
}

void BlackWhite::BWChanged  (double redbw, double greenbw, double bluebw)
{
    nextredbw = redbw;
    nextgreenbw = greenbw;
    nextbluebw = bluebw;

    idle_register.add(
        [this]() -> bool
        {
            BWComputed_();
            return false;
        }
    );
}

bool BlackWhite::BWComputed_ ()
{

    disableListener ();
    mixerRed->setValue (nextredbw);
    adjusterChanged(mixerRed, mixerRed->getValue());
    mixerGreen->setValue (nextgreenbw);
    adjusterChanged(mixerGreen, mixerGreen->getValue());
    mixerBlue->setValue (nextbluebw);
    adjusterChanged(mixerBlue, mixerBlue->getValue());
    autoconn.block (true);
    autoch->set_active (true);
    autoconn.block (false);
    lastAuto = false;
    nextcount++;
    enableListener ();
    if (listener  && nextcount <= 1 ) {
        // activated only 1 time, but perhaps in some cases if we want that all is update pp3, auto, sliders : nextcount <= 2 but it cost time and result is identical
        listener->panelChanged (EvAutoch, M("GENERAL_ENABLED"));
    }

    updateRGBLabel();

    return false;
}

void BlackWhite::read (const ProcParams* pp, const ParamsEdited* pedited)
{

    disableListener ();
    methodconn.block(true);
    //autoconn.block (true);
    filterconn.block(true);
    settingconn.block(true);
    enaccconn.block (true);

    if (pedited && !pedited->blackwhite.setting) {
        setting->set_active (15);    // "Unchanged"
    } else if (pp->blackwhite.setting == "NormalContrast") {
        setting->set_active (0);
    } else if (pp->blackwhite.setting == "HighContrast") {
        setting->set_active (1);
    } else if (pp->blackwhite.setting == "Luminance") {
        setting->set_active (2);
    } else if (pp->blackwhite.setting == "Landscape") {
        setting->set_active (3);
    } else if (pp->blackwhite.setting == "Portrait") {
        setting->set_active (4);
    } else if (pp->blackwhite.setting == "LowSensitivity") {
        setting->set_active (5);
    } else if (pp->blackwhite.setting == "HighSensitivity") {
        setting->set_active (6);
    } else if (pp->blackwhite.setting == "Panchromatic") {
        setting->set_active (7);
    } else if (pp->blackwhite.setting == "HyperPanchromatic") {
        setting->set_active (8);
    } else if (pp->blackwhite.setting == "Orthochromatic") {
        setting->set_active (9);
    } else if (pp->blackwhite.setting == "RGB-Abs") {
        setting->set_active (10);
    } else if (pp->blackwhite.setting == "RGB-Rel") {
        setting->set_active (11);
    } else if (pp->blackwhite.setting == "ROYGCBPM-Abs") {
        setting->set_active (12);
    } else if (pp->blackwhite.setting == "ROYGCBPM-Rel") {
        setting->set_active (13);
    } else if (pp->blackwhite.setting == "InfraRed") {
        setting->set_active (14);
    }

    settingChanged();


    if (pedited && !pedited->blackwhite.method) {
        method->set_active (4);    // "Unchanged"
    } else if (pp->blackwhite.method == "Desaturation") {
        method->set_active (1);
    } else if (pp->blackwhite.method == "LumEqualizer") {
        method->set_active (2);
    } else if (pp->blackwhite.method == "ChannelMixer") {
        method->set_active (3);
    } else {
        method->set_active (1);    // Default to Desaturation
    }
    methodChanged();


    if (pedited && !pedited->blackwhite.filter) {
        filter->set_active (9);    // "Unchanged"
    } else if (pp->blackwhite.filter == "None") {
        filter->set_active (0);
    } else if (pp->blackwhite.filter == "Red") {
        filter->set_active (1);
    } else if (pp->blackwhite.filter == "Orange") {
        filter->set_active (2);
    } else if (pp->blackwhite.filter == "Yellow") {
        filter->set_active (3);
    } else if (pp->blackwhite.filter == "YellowGreen") {
        filter->set_active (4);
    } else if (pp->blackwhite.filter == "Green") {
        filter->set_active (5);
    } else if (pp->blackwhite.filter == "Cyan") {
        filter->set_active (6);
    } else if (pp->blackwhite.filter == "Blue") {
        filter->set_active (7);
    } else if (pp->blackwhite.filter == "Purple") {
        filter->set_active (8);
    }

    filterChanged();

    enabledcc->set_active (pp->blackwhite.enabledcc);
    lastEnabledcc = pp->blackwhite.enabledcc;
    // Enable state is controlled by method dropdown (Off = disabled)
    setEnabled(pp->blackwhite.enabled);

    mixerRed->setValue (pp->blackwhite.mixerRed);
    mixerGreen->setValue (pp->blackwhite.mixerGreen);
    mixerBlue->setValue (pp->blackwhite.mixerBlue);
    gammaRed->setValue (pp->blackwhite.gammaRed);
    gammaGreen->setValue (pp->blackwhite.gammaGreen);
    gammaBlue->setValue (pp->blackwhite.gammaBlue);
    bwNeutrals->setValue (pp->blackwhite.neutrals);
    bwTone->setValue (pp->blackwhite.tone);
    bwStrength->setValue (pp->blackwhite.strength);
    mixerOrange->setValue (pp->blackwhite.mixerOrange);
    mixerYellow->setValue (pp->blackwhite.mixerYellow);
    mixerCyan->setValue (pp->blackwhite.mixerCyan);
    mixerMagenta->setValue (pp->blackwhite.mixerMagenta);
    mixerPurple->setValue (pp->blackwhite.mixerPurple);
    luminanceCurve->setCurve (pp->blackwhite.luminanceCurve);
    beforeCurve->setCurve (pp->blackwhite.beforeCurve);
    beforeCurveMode->set_active(toUnderlying(pp->blackwhite.beforeCurveMode));
    afterCurve->setCurve (pp->blackwhite.afterCurve);
//  afterCurveMode->set_active(pp->blackwhite.afterCurveMode);

    autoch->set_active (pp->blackwhite.autoc);
    lastAuto = pp->blackwhite.autoc;

    algoconn.block(true);

    if (pedited && !pedited->blackwhite.algo) {
        algo->set_active (2);
    } else if (pp->blackwhite.algo == "LI") {
        algo->set_active (0);
    } else if (pp->blackwhite.algo == "SP") {
        algo->set_active (1);
    }

    algoconn.block(false);
    algoChanged();


    if (pedited) {
        luminanceCurve->setUnChanged (!pedited->blackwhite.luminanceCurve);
        beforeCurve->setUnChanged (!pedited->blackwhite.beforeCurve);
        afterCurve->setUnChanged (!pedited->blackwhite.afterCurve);
        autoch->set_inconsistent (!pedited->blackwhite.autoc);
        set_inconsistent (multiImage && !pedited->blackwhite.enabled);
        enabledcc->set_inconsistent  (!pedited->blackwhite.enabledcc);
        mixerRed->setEditedState (pedited->blackwhite.mixerRed ? Edited : UnEdited);
        mixerGreen->setEditedState (pedited->blackwhite.mixerGreen ? Edited : UnEdited);
        mixerBlue->setEditedState (pedited->blackwhite.mixerBlue ? Edited : UnEdited);
        gammaRed->setEditedState (pedited->blackwhite.gammaRed ? Edited : UnEdited);
        gammaGreen->setEditedState (pedited->blackwhite.gammaGreen ? Edited : UnEdited);
        gammaBlue->setEditedState (pedited->blackwhite.gammaBlue ? Edited : UnEdited);
        bwNeutrals->setEditedState (pedited->blackwhite.neutrals ? Edited : UnEdited);
        bwTone->setEditedState (pedited->blackwhite.tone ? Edited : UnEdited);
        bwStrength->setEditedState (pedited->blackwhite.strength ? Edited : UnEdited);
        mixerOrange->setEditedState (pedited->blackwhite.mixerOrange ? Edited : UnEdited);
        mixerYellow->setEditedState (pedited->blackwhite.mixerYellow ? Edited : UnEdited);
        mixerCyan->setEditedState (pedited->blackwhite.mixerCyan ? Edited : UnEdited);
        mixerMagenta->setEditedState (pedited->blackwhite.mixerMagenta ? Edited : UnEdited);
        mixerPurple->setEditedState (pedited->blackwhite.mixerPurple ? Edited : UnEdited);

        if (!pedited->blackwhite.beforeCurveMode) {
            beforeCurveMode->set_active(4); // "Unchanged"
        }

//      if (!pedited->blackwhite.afterCurveMode) {
//          afterCurveMode->set_active(1); // "Unchanged"
//      }
    }

    methodconn.block(false);
    filterconn.block(false);
    settingconn.block(false);
    //autoconn.block (false);
    enaccconn.block (false);

    updateRGBLabel();

    enableListener ();
}

void BlackWhite::write (ProcParams* pp, ParamsEdited* pedited)
{
    pp->blackwhite.enabled = getEnabled();
    pp->blackwhite.luminanceCurve = luminanceCurve->getCurve ();
    pp->blackwhite.autoc = autoch->get_active();
    pp->blackwhite.enabledcc = enabledcc->get_active ();
    pp->blackwhite.mixerRed = mixerRed->getValue ();
    pp->blackwhite.mixerGreen = mixerGreen->getValue ();
    pp->blackwhite.mixerBlue = mixerBlue->getValue ();
    pp->blackwhite.gammaRed = gammaRed->getValue ();
    pp->blackwhite.gammaGreen = gammaGreen->getValue ();
    pp->blackwhite.gammaBlue = gammaBlue->getValue ();
    pp->blackwhite.neutrals = bwNeutrals->getValue ();
    pp->blackwhite.tone = bwTone->getValue ();
    pp->blackwhite.strength = bwStrength->getValue ();
    pp->blackwhite.mixerOrange = mixerOrange->getValue ();
    pp->blackwhite.mixerYellow = mixerYellow->getValue ();
    pp->blackwhite.mixerCyan = mixerCyan->getValue ();
    pp->blackwhite.mixerMagenta = mixerMagenta->getValue ();
    pp->blackwhite.mixerPurple = mixerPurple->getValue ();
    pp->blackwhite.beforeCurve = beforeCurve->getCurve ();
    pp->blackwhite.afterCurve = afterCurve->getCurve ();

    int tcMode = beforeCurveMode->get_active_row_number();

    if      (tcMode == 0) {
        pp->blackwhite.beforeCurveMode = BlackWhiteParams::TcMode::STD_BW;
    } else if (tcMode == 1) {
        pp->blackwhite.beforeCurveMode = BlackWhiteParams::TcMode::WEIGHTEDSTD_BW;
    } else if (tcMode == 2) {
        pp->blackwhite.beforeCurveMode = BlackWhiteParams::TcMode::FILMLIKE_BW;
    } else if (tcMode == 3) {
        pp->blackwhite.beforeCurveMode = BlackWhiteParams::TcMode::SATANDVALBLENDING_BW;
    }

//  tcMode = afterCurveMode->get_active_row_number();
//  if      (tcMode == 0) pp->blackwhite.afterCurveMode = BlackWhiteParams::TCMode::STD_BW;
    //  else if (tcMode == 1) pp->blackwhite.afterCurveMode = BlackWhiteParams::TCMode::WEIGHTEDSTD;

    if (pedited) {
        pedited->blackwhite.enabled = true;
        pedited->blackwhite.luminanceCurve = !luminanceCurve->isUnChanged ();
        pedited->blackwhite.autoc = !autoch->get_inconsistent();
        pedited->blackwhite.enabledcc = !enabledcc->get_inconsistent();
        pedited->blackwhite.mixerRed = mixerRed->getEditedState ();
        pedited->blackwhite.mixerGreen = mixerGreen->getEditedState ();
        pedited->blackwhite.mixerBlue = mixerBlue->getEditedState ();
        pedited->blackwhite.gammaRed = gammaRed->getEditedState ();
        pedited->blackwhite.gammaGreen = gammaGreen->getEditedState ();
        pedited->blackwhite.gammaBlue = gammaBlue->getEditedState ();
        pedited->blackwhite.neutrals = bwNeutrals->getEditedState ();
        pedited->blackwhite.tone = bwTone->getEditedState ();
        pedited->blackwhite.strength = bwStrength->getEditedState ();
        pedited->blackwhite.filter = filter->get_active_text() != M("GENERAL_UNCHANGED");
        pedited->blackwhite.setting = setting->get_active_text() != M("GENERAL_UNCHANGED");
        pedited->blackwhite.method = method->get_active_text() != M("GENERAL_UNCHANGED");
        pedited->blackwhite.mixerOrange = mixerOrange->getEditedState ();
        pedited->blackwhite.mixerYellow = mixerYellow->getEditedState ();
        pedited->blackwhite.mixerCyan = mixerCyan->getEditedState ();
        pedited->blackwhite.mixerMagenta = mixerMagenta->getEditedState ();
        pedited->blackwhite.mixerPurple = mixerPurple->getEditedState ();
        pedited->blackwhite.beforeCurve = !beforeCurve->isUnChanged ();
        pedited->blackwhite.beforeCurveMode = beforeCurveMode->get_active_row_number() != 4;
        pedited->blackwhite.afterCurve = !afterCurve->isUnChanged ();
        pedited->blackwhite.algo          = algo->get_active_text() != M("GENERAL_UNCHANGED");

//      pedited->blackwhite.afterCurveMode = afterCurveMode->get_active_row_number() != 1;
    }

    if (method->get_active_row_number() == 1) {
        pp->blackwhite.method = "Desaturation";
    } else if (method->get_active_row_number() == 2) {
        pp->blackwhite.method = "LumEqualizer";
    } else if (method->get_active_row_number() == 3) {
        pp->blackwhite.method = "ChannelMixer";
    } else {
        // Default to Desaturation when method is "Disabled" but tool is enabled
        pp->blackwhite.method = "Desaturation";
    }

    if (algo->get_active_row_number() == 0) {
        pp->blackwhite.algo = "LI";
    } else if (algo->get_active_row_number() == 1) {
        pp->blackwhite.algo = "SP";
    }

    pp->blackwhite.setting = getSettingString();
    pp->blackwhite.filter = getFilterString();
}

void BlackWhite::algoChanged ()
{
    if (listener && (multiImage || getEnabled()) ) {
        listener->panelChanged (EvBWMethodalg, algo->get_active_text ());
    }
}

void BlackWhite::curveChanged (CurveEditor* ce)
{
    if (listener) {
        if (ce == beforeCurve) {
            listener->panelChanged (EvBWBeforeCurve, M("HISTORY_CUSTOMCURVE"));
        }

        if (ce == afterCurve) {
            listener->panelChanged (EvBWAfterCurve, M("HISTORY_CUSTOMCURVE"));
        }

        if (ce == luminanceCurve) {
            listener->panelChanged (EvBWLuminanceEqual, M("HISTORY_CUSTOMCURVE"));
        }
    }
}

void BlackWhite::curveMode1Changed ()
{
    if (listener) {
        Glib::signal_idle().connect (sigc::mem_fun(*this, &BlackWhite::curveMode1Changed_));
    }
}
bool BlackWhite::curveMode1Changed_ ()
{
    if (listener) {
        listener->panelChanged (EvBWBeforeCurveMode, escapeHtmlChars(beforeCurveMode->get_active_text()));
    }

    return false;
}
/*
void BlackWhite::curveMode1Changed2 () {
    if (listener)  Glib::signal_idle().connect (sigc::mem_fun(*this, &BlackWhite::curveMode1Changed2_));
}
bool BlackWhite::curveMode1Changed2_ () {
    if (listener) listener->panelChanged (EvBWAfterCurveMode, escapeHtmlChars(afterCurveMode->get_active_text()));
    return false;
}
*/
void BlackWhite::colorForValue (double valX, double valY, enum ColorCaller::ElemType elemType, int callerId, ColorCaller* caller)
{

    float r, g, b;

    if (elemType == ColorCaller::CCET_VERTICAL_BAR) {
        valY = 0.5f;
    }

    if (callerId == 1) {        // Hue = f(Hue)

        float h = float((valY - 0.5) * 2. + valX);

        if (h > 1.0f) {
            h -= 1.0f;
        } else if (h < 0.0f) {
            h += 1.0f;
        }

        Color::hsv2rgb01(h, 0.5f, 0.5f, r, g, b);
        caller->ccRed = double(r);
        caller->ccGreen = double(g);
        caller->ccBlue = double(b);
    } else if (callerId == 2) { // Saturation = f(Hue)
        Color::hsv2rgb01(float(valX), float(valY), 0.5f, r, g, b);
        caller->ccRed = double(r);
        caller->ccGreen = double(g);
        caller->ccBlue = double(b);
    } else if (callerId == 3) { // Value = f(Hue)
        Color::hsv2rgb01(float(valX), 0.5f, float(valY), r, g, b);
        caller->ccRed = double(r);
        caller->ccGreen = double(g);
        caller->ccBlue = double(b);
    } else {
        printf("Error: no curve displayed!\n");
    }
}


void BlackWhite::settingChanged ()
{

    if ( setting->get_active_row_number() == 10 || setting->get_active_row_number() == 11 ) {
        // RGB Channel Mixer
        showMixer(3);
        hideEnabledCC();
        showFilter();
    } else if ( setting->get_active_row_number() == 12 || setting->get_active_row_number() == 13 ) {
        // ROYGCBPM Channel Mixer
        showMixer(7);
        showEnabledCC();
        showFilter();
    } else if ( setting->get_active_row_number() == 14 ) {
        // Infrared
        filter->set_active (0);
        showMixer(3, false);
        hideEnabledCC();
        hideFilter();
    } else {
        // RGB Presets
        showMixer(3, false);
        hideEnabledCC();
        showFilter();
    }

    // Checking "listener" to avoid "autoch" getting toggled off because it has to change the sliders when toggling on
    if (listener) {
        if (multiImage && autoch->get_inconsistent()) {
            autoch->set_inconsistent (false);
        }

        autoconn.block(true);
        autoch->set_active (false);
        autoconn.block(false);
        lastAuto = false;
    }

    updateRGBLabel();

    if (listener && (multiImage || getEnabled())) {
        listener->panelChanged (EvBWsetting, setting->get_active_text ());
    }
}


void BlackWhite::filterChanged ()
{
    // Checking "listener" to avoid "autoch" getting toggled off because it has to change the sliders when toggling on
    if (listener) {
        if (multiImage && autoch->get_inconsistent()) {
            autoch->set_inconsistent (false);
        }

        autoconn.block(true);
        autoch->set_active (false);
        autoconn.block(false);
        lastAuto = false;
    }

    updateRGBLabel();

    if (listener && (multiImage || getEnabled())) {
        listener->panelChanged (EvBWfilter, filter->get_active_text ());
    }
}

void BlackWhite::methodChanged ()
{
    int row = method->get_active_row_number();

    if (row == 0) {
        setEnabled(false);
    } else {
        setEnabled(true);
    }

    if (listener) {
        listener->panelChanged (EvBWmethod, method->get_active_text ());
    }
}

void BlackWhite::setBWPreset (int methodRow, int filterRow)
{
    methodconn.block(true);
    filterconn.block(true);
    method->set_active(methodRow);
    filter->set_active(filterRow);
    methodconn.block(false);
    filterconn.block(false);
    methodChanged();
    if (methodRow > 0) {
        filterChanged();
    }
}

int BlackWhite::getBWPresetIndex () const
{
    if (method->get_active_row_number() == 0) return 0; // Off
    int m = method->get_active_row_number();
    int f = filter->get_active_row_number();
    // Filters only apply in ChannelMixer (row 3)
    if (m != 3 || f == 0) return 1; // plain B/W
    switch (f) {
        case 1: return 2; // Red
        case 2: return 3; // Orange
        case 3: return 4; // Yellow
        case 5: return 5; // Green
        case 7: return 6; // Blue
        default: return 1;
    }
}

void BlackWhite::enabledChanged ()
{
    // When enabled via checkbox and method is "Disabled", auto-select Desaturation
    if (getEnabled() && method->get_active_row_number() == 0) {
        methodconn.block(true);
        method->set_active(1); // Desaturation
        methodconn.block(false);
    }

    if (listener) {
        if (get_inconsistent()) {
            listener->panelChanged (EvBWChmixEnabled, M("GENERAL_UNCHANGED"));
        } else if (getEnabled()) {
            listener->panelChanged (EvBWChmixEnabled, M("GENERAL_ENABLED"));
        } else {
            listener->panelChanged (EvBWChmixEnabled, M("GENERAL_DISABLED"));
        }
    }
}

void BlackWhite::neutral_pressed ()
{
    // This method deselects auto chmixer and sets "neutral" values to params
    disableListener();

    if (multiImage && autoch->get_inconsistent()) {
        autoch->set_inconsistent (false);
    }

    autoconn.block(true);
    autoch->set_active (false);
    autoconn.block(false);
    lastAuto = false;

    int activeSetting = setting->get_active_row_number();

    if (activeSetting < 10 || activeSetting > 13) {
        setting->set_active (11);
    }

    filter->set_active (0);
    mixerRed->resetValue(false);
    mixerGreen->resetValue(false);
    mixerBlue->resetValue(false);
    mixerOrange->resetValue(false);
    mixerYellow->resetValue(false);
    mixerMagenta->resetValue(false);
    mixerPurple->resetValue(false);
    mixerCyan->resetValue(false);

    enableListener();

    updateRGBLabel();

    nextcount = 0;
    if(listener) {
        listener->panelChanged (EvNeutralBW, M("GENERAL_RESET"));
    }
}

void BlackWhite::enabledcc_toggled ()
{

    // toggling off the Complementary Colors does switch off the Auto button
    if (multiImage) {
        // multiple image editing (batch)
        if (enabledcc->get_inconsistent()) {
            enabledcc->set_inconsistent (false);  // set consistent
            enaccconn.block (true);
            enabledcc->set_active (false);  // ... and deactivated
            enaccconn.block (false);
        } else if (lastEnabledcc) {
            enabledcc->set_inconsistent (true);
        }

        lastEnabledcc = enabledcc->get_active ();
    }

    if (multiImage && autoch->get_inconsistent()) {
        autoch->set_inconsistent (false);
    }

    autoconn.block(true);
    autoch->set_active (false);
    autoconn.block(false);
    lastAuto = false;

    updateRGBLabel();

    if (listener) {
        if (enabledcc->get_inconsistent()) {
            listener->panelChanged (EvBWChmixEnabledLm, M("GENERAL_UNCHANGED"));
        } else if (enabledcc->get_active ()) {
            listener->panelChanged (EvBWChmixEnabledLm, M("GENERAL_ENABLED"));
        } else {
            listener->panelChanged (EvBWChmixEnabledLm, M("GENERAL_DISABLED"));
        }
    }
}


void BlackWhite::setDefaults (const ProcParams* defParams, const ParamsEdited* pedited)
{

    mixerRed->setDefault (defParams->blackwhite.mixerRed);
    mixerGreen->setDefault (defParams->blackwhite.mixerGreen);
    mixerBlue->setDefault (defParams->blackwhite.mixerBlue);
    gammaRed->setDefault (defParams->blackwhite.gammaRed);
    gammaGreen->setDefault (defParams->blackwhite.gammaGreen);
    gammaBlue->setDefault (defParams->blackwhite.gammaBlue);
    bwNeutrals->setDefault (defParams->blackwhite.neutrals);
    bwTone->setDefault (defParams->blackwhite.tone);
    bwStrength->setDefault (defParams->blackwhite.strength);
    mixerOrange->setDefault (defParams->blackwhite.mixerOrange);
    mixerYellow->setDefault (defParams->blackwhite.mixerYellow);
    mixerCyan->setDefault (defParams->blackwhite.mixerCyan);
    mixerMagenta->setDefault (defParams->blackwhite.mixerMagenta);
    mixerPurple->setDefault (defParams->blackwhite.mixerPurple);

    if (pedited) {
        mixerRed->setDefaultEditedState (pedited->blackwhite.mixerRed ? Edited : UnEdited);
        mixerGreen->setDefaultEditedState (pedited->blackwhite.mixerGreen ? Edited : UnEdited);
        mixerBlue->setDefaultEditedState (pedited->blackwhite.mixerBlue ? Edited : UnEdited);
        gammaRed->setDefaultEditedState (pedited->blackwhite.gammaRed ? Edited : UnEdited);
        gammaGreen->setDefaultEditedState (pedited->blackwhite.gammaGreen ? Edited : UnEdited);
        gammaBlue->setDefaultEditedState (pedited->blackwhite.gammaBlue ? Edited : UnEdited);
        bwNeutrals->setDefaultEditedState (pedited->blackwhite.neutrals ? Edited : UnEdited);
        bwTone->setDefaultEditedState (pedited->blackwhite.tone ? Edited : UnEdited);
        bwStrength->setDefaultEditedState (pedited->blackwhite.strength ? Edited : UnEdited);
        mixerOrange->setDefaultEditedState (pedited->blackwhite.mixerOrange ? Edited : UnEdited);
        mixerYellow->setDefaultEditedState (pedited->blackwhite.mixerYellow ? Edited : UnEdited);
        mixerCyan->setDefaultEditedState (pedited->blackwhite.mixerCyan ? Edited : UnEdited);
        mixerMagenta->setDefaultEditedState (pedited->blackwhite.mixerMagenta ? Edited : UnEdited);
        mixerPurple->setDefaultEditedState (pedited->blackwhite.mixerPurple ? Edited : UnEdited);
    } else {
        mixerRed->setDefaultEditedState (Irrelevant);
        mixerGreen->setDefaultEditedState (Irrelevant);
        mixerBlue->setDefaultEditedState (Irrelevant);
        gammaRed->setDefaultEditedState (Irrelevant);
        gammaGreen->setDefaultEditedState (Irrelevant);
        gammaBlue->setDefaultEditedState (Irrelevant);
        bwNeutrals->setDefaultEditedState (Irrelevant);
        bwTone->setDefaultEditedState (Irrelevant);
        bwStrength->setDefaultEditedState (Irrelevant);
        mixerOrange->setDefaultEditedState (Irrelevant);
        mixerYellow->setDefaultEditedState (Irrelevant);
        mixerCyan->setDefaultEditedState (Irrelevant);
        mixerMagenta->setDefaultEditedState (Irrelevant);
        mixerPurple->setDefaultEditedState (Irrelevant);
    }
}

void BlackWhite::autoch_toggled ()
{

    if (batchMode) {
        if (multiImage) {
            if (autoch->get_inconsistent()) {
                autoch->set_inconsistent (false);
                autoconn.block (true);
                autoch->set_active (false);
                autoconn.block (false);
            } else if (lastAuto) {
                autoch->set_inconsistent (true);
            }
        }

        lastAuto = autoch->get_active ();

        mixerRed->setEditedState (UnEdited);
        mixerGreen->setEditedState (UnEdited);
        mixerBlue->setEditedState (UnEdited);
        mixerOrange->setEditedState (UnEdited);
        mixerYellow->setEditedState (UnEdited);
        mixerPurple->setEditedState (UnEdited);
        mixerMagenta->setEditedState (UnEdited);
        mixerCyan->setEditedState (UnEdited);

        bool wasEnabled = disableListener();

        if (mixerRed->getAddMode()) {
            mixerRed->resetValue(true);
        }

        if (mixerGreen->getAddMode()) {
            mixerGreen->resetValue(true);
        }

        if (mixerBlue->getAddMode()) {
            mixerBlue->resetValue(true);
        }

        if (mixerOrange->getAddMode()) {
            mixerOrange->resetValue(true);
        }

        if (mixerYellow->getAddMode()) {
            mixerYellow->resetValue(true);
        }

        if (mixerMagenta->getAddMode()) {
            mixerMagenta->resetValue(true);
        }

        if (mixerPurple->getAddMode()) {
            mixerPurple->resetValue(true);
        }

        if (mixerCyan->getAddMode()) {
            mixerCyan->resetValue(true);
        }

        setting->set_active (11);
        filter->set_active (0);

        if (wasEnabled) {
            enableListener();
        }

        if (listener) {
            if (autoch->get_inconsistent()) {
                listener->panelChanged (EvAutoch, M("GENERAL_UNCHANGED"));
            } else if (autoch->get_active ()) {
                listener->panelChanged (EvAutoch, M("GENERAL_ENABLED"));
            } else {
                listener->panelChanged (EvAutoch, M("GENERAL_DISABLED"));
            }
        }
    } else {
        if (autoch->get_active()) {
            bool wasEnabled = disableListener();
            mixerRed->resetValue(false);
            mixerGreen->resetValue(false);
            mixerBlue->resetValue(false);
            mixerOrange->resetValue(false);
            mixerYellow->resetValue(false);
            mixerMagenta->resetValue(false);
            mixerPurple->resetValue(false);
            mixerCyan->resetValue(false);
            setting->set_active (11);
            filter->set_active (0);

            if (wasEnabled) {
                enableListener();
            }

            updateRGBLabel();

            if (listener) {
                listener->panelChanged (EvAutoch, M("GENERAL_ENABLED"));
            }
        } else {
            if (listener) {
                listener->panelChanged (EvAutoch, M("GENERAL_DISABLED"));
            }
        }
    }

}

void BlackWhite::adjusterChanged(Adjuster* a, double newval)
{
    autoEnable();
    // Checking "listener" to avoid "autoch" getting toggled off because it has to change the sliders when toggling on
    if (listener && (a == mixerRed || a == mixerGreen || a == mixerBlue || a == mixerOrange || a == mixerYellow || a == mixerMagenta || a == mixerPurple || a == mixerCyan) ) {
        if (multiImage && autoch->get_inconsistent()) {
            autoch->set_inconsistent (false);
        }

        nextcount = 0;
        autoconn.block(true);
        autoch->set_active (false);
        autoconn.block(false);
        lastAuto = false;
    }

    if (a == mixerRed || a == mixerGreen || a == mixerBlue
            || a == mixerOrange || a == mixerYellow || a == mixerCyan
            || a == mixerMagenta || a == mixerPurple)

    {
        updateRGBLabel();
    }

    if (listener  && (multiImage || getEnabled())) {
        Glib::ustring value = a->getTextValue();

        if (a == mixerRed) {
            listener->panelChanged (EvBWred, value );
        } else if (a == mixerGreen) {
            listener->panelChanged (EvBWgreen, value );
        } else if (a == mixerBlue) {
            listener->panelChanged (EvBWblue, value );
        } else if (a == gammaGreen) {
            listener->panelChanged (EvBWgreengam, value );
        } else if (a == gammaBlue) {
            listener->panelChanged (EvBWbluegam, value );
        } else if (a == gammaRed) {
            listener->panelChanged (EvBWredgam, value );
        } else if (a == mixerOrange) {
            listener->panelChanged (EvBWoran, value );
        } else if (a == mixerYellow) {
            listener->panelChanged (EvBWyell, value );
        } else if (a == mixerCyan) {
            listener->panelChanged (EvBWcyan, value );
        } else if (a == mixerMagenta) {
            listener->panelChanged (EvBWmag, value );
        } else if (a == mixerPurple) {
            listener->panelChanged (EvBWpur, value );
        } else if (a == bwNeutrals) {
            listener->panelChanged (EvBWNeutrals, value);
        } else if (a == bwTone) {
            listener->panelChanged (EvBWTone, value);
        } else if (a == bwStrength) {
            listener->panelChanged (EvBWStrength, value);
        }
    }
}

void BlackWhite::updateRGBLabel ()
{
    if (!batchMode) {
        float kcorrec = 1.f;
        float r, g, b;

        if (autoch->get_active()) {
            r = nextredbw;
            g = nextgreenbw;
            b = nextbluebw;
        } else {
            r = mixerRed->getValue();
            g = mixerGreen->getValue();
            b = mixerBlue->getValue();
        }

        double mixR, mixG, mixB;
        float filcor;
        Glib::ustring sSetting = getSettingString();
        Color::computeBWMixerConstants(sSetting, getFilterString(), getalgoString(), filcor, r, g, b,
                                       mixerOrange->getValue(), mixerYellow->getValue(), mixerCyan->getValue(), mixerPurple->getValue(), mixerMagenta->getValue(),
                                       autoch->get_active(), enabledcc->get_active(), kcorrec, mixR, mixG, mixB);

        if( filcor != 1.f) {
            r = kcorrec * r / (r + g + b);
            g = kcorrec * g / (r + g + b);
            b = kcorrec * b / (r + g + b);
        }

        RGBLabels->set_text(
            Glib::ustring::compose(M("TP_BWMIX_RGBLABEL"),
                                   Glib::ustring::format(std::fixed, std::setprecision(1), r * 100.f),
                                   Glib::ustring::format(std::fixed, std::setprecision(1), g * 100.f),
                                   Glib::ustring::format(std::fixed, std::setprecision(1), b * 100.f),
                                   Glib::ustring::format(std::fixed, std::setprecision(0), ceil(kcorrec * 100.f/*(r+g+b)*100.)*/)))
        );

        // We have to update the RGB sliders too if preset values has been chosen
        if (sSetting != "RGB-Abs" && sSetting != "RGB-Rel" && sSetting != "ROYGCBPM-Abs" && sSetting != "ROYGCBPM-Rel") {
            mixerRed->setValue(mixR);
            mixerGreen->setValue(mixG);
            mixerBlue->setValue(mixB);
        }
    }
}

void BlackWhite::setBatchMode (bool batchMode)
{
    ToolPanel::setBatchMode (batchMode);
    bwNeutrals->showEditedCB ();
    bwTone->showEditedCB ();
    bwStrength->showEditedCB ();
    method->append (M("GENERAL_UNCHANGED"));
    filter->append (M("GENERAL_UNCHANGED"));
    setting->append (M("GENERAL_UNCHANGED"));
    beforeCurveMode->append (M("GENERAL_UNCHANGED"));
    algo->append (M("GENERAL_UNCHANGED"));
}

void BlackWhite::autoOpenCurve ()
{
    luminanceCurve->openIfNonlinear();
    beforeCurve->openIfNonlinear();
    afterCurve->openIfNonlinear();
}
void BlackWhite::setEditProvider (EditDataProvider *provider)
{
    luminanceCurve->setEditProvider(provider);
    beforeCurve->setEditProvider(provider);
    afterCurve->setEditProvider(provider);
}

void BlackWhite::setAdjusterBehavior (bool bwadd, bool bwgadd)
{

    mixerRed->setAddMode(bwadd);
    mixerOrange->setAddMode(bwadd);
    mixerYellow->setAddMode(bwadd);
    mixerGreen->setAddMode(bwadd);
    mixerCyan->setAddMode(bwadd);
    mixerBlue->setAddMode(bwadd);
    mixerMagenta->setAddMode(bwadd);
    mixerPurple->setAddMode(bwadd);

    gammaRed->setAddMode(bwgadd);
    gammaGreen->setAddMode(bwgadd);
    gammaBlue->setAddMode(bwgadd);
}

void BlackWhite::trimValues (rtengine::procparams::ProcParams* pp)
{

    mixerRed->trimValue (pp->blackwhite.mixerRed);
    mixerGreen->trimValue (pp->blackwhite.mixerGreen);
    mixerBlue->trimValue (pp->blackwhite.mixerBlue);
    gammaRed->trimValue (pp->blackwhite.gammaRed);
    gammaGreen->trimValue (pp->blackwhite.gammaGreen);
    gammaBlue->trimValue (pp->blackwhite.gammaBlue);
    bwNeutrals->trimValue (pp->blackwhite.neutrals);
    bwTone->trimValue (pp->blackwhite.tone);
    bwStrength->trimValue (pp->blackwhite.strength);
    mixerOrange->trimValue (pp->blackwhite.mixerOrange);
    mixerYellow->trimValue (pp->blackwhite.mixerYellow);
    mixerCyan->trimValue (pp->blackwhite.mixerCyan);
    mixerMagenta->trimValue (pp->blackwhite.mixerMagenta);
    mixerPurple->trimValue (pp->blackwhite.mixerPurple);
}

void BlackWhite::showLuminance() {}
void BlackWhite::hideLuminance() {}
void BlackWhite::showFilter() {}
void BlackWhite::hideFilter() {}
void BlackWhite::showEnabledCC() {}
void BlackWhite::hideEnabledCC() {}
void BlackWhite::showMixer(int, bool) {}
void BlackWhite::hideMixer() {}
void BlackWhite::showGamma() {}
void BlackWhite::hideGamma() {}

Glib::ustring BlackWhite::getalgoString()
{
    Glib::ustring retVal;

    if (algo->get_active_row_number() == 0) {
        retVal = "LI";
    } else if (algo->get_active_row_number() == 1) {
        retVal = "SP";
    }

    return retVal;
}

Glib::ustring BlackWhite::getSettingString()
{
    Glib::ustring retVal;

    if (setting->get_active_row_number() == 0) {
        retVal = "NormalContrast";
    } else if (setting->get_active_row_number() == 1) {
        retVal = "HighContrast";
    } else if (setting->get_active_row_number() == 2) {
        retVal = "Luminance";
    } else if (setting->get_active_row_number() == 3) {
        retVal = "Landscape";
    } else if (setting->get_active_row_number() == 4) {
        retVal = "Portrait";
    } else if (setting->get_active_row_number() == 5) {
        retVal = "LowSensitivity";
    } else if (setting->get_active_row_number() == 6) {
        retVal = "HighSensitivity";
    } else if (setting->get_active_row_number() == 7) {
        retVal = "Panchromatic";
    } else if (setting->get_active_row_number() == 8) {
        retVal = "HyperPanchromatic";
    } else if (setting->get_active_row_number() == 9) {
        retVal = "Orthochromatic";
    } else if (setting->get_active_row_number() == 10) {
        retVal = "RGB-Abs";
    } else if (setting->get_active_row_number() == 11) {
        retVal = "RGB-Rel";
    } else if (setting->get_active_row_number() == 12) {
        retVal = "ROYGCBPM-Abs";
    } else if (setting->get_active_row_number() == 13) {
        retVal = "ROYGCBPM-Rel";
    } else if (setting->get_active_row_number() == 14) {
        retVal = "InfraRed";
    }

    return retVal;
}

Glib::ustring BlackWhite::getFilterString()
{
    Glib::ustring retVal;

    if (filter->get_active_row_number() == 0) {
        retVal = "None";
    } else if (filter->get_active_row_number() == 1) {
        retVal = "Red";
    } else if (filter->get_active_row_number() == 2) {
        retVal = "Orange";
    } else if (filter->get_active_row_number() == 3) {
        retVal = "Yellow";
    } else if (filter->get_active_row_number() == 4) {
        retVal = "YellowGreen";
    } else if (filter->get_active_row_number() == 5) {
        retVal = "Green";
    } else if (filter->get_active_row_number() == 6) {
        retVal = "Cyan";
    } else if (filter->get_active_row_number() == 7) {
        retVal = "Blue";
    } else if (filter->get_active_row_number() == 8) {
        retVal = "Purple";
    }

    return retVal;
}
