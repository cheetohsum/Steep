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

#ifdef RT_AI_MASKING

#include "locallabaimask.h"
#include "eventmapper.h"
#include "options.h"
#include "rtengine/procparams.h"

using namespace rtengine;
using namespace procparams;

LocallabAIMask::LocallabAIMask():
    LocallabTool(this, M("TP_LOCALLAB_AIMASK_TOOLNAME"), M("TP_LOCALLAB_AIMASK"), false, false),

    aiMaskClassCombo(Gtk::manage(new MyComboBoxText())),
    aiMaskShapeOpCombo(Gtk::manage(new MyComboBoxText())),
    aiMaskThreshold(Gtk::manage(new Adjuster(M("TP_LOCALLAB_AIMASK_TOLERANCE"), 0.0, 100.0, 1.0, 70.0))),
    aiMaskFeather(Gtk::manage(new Adjuster(M("TP_LOCALLAB_AIMASK_FEATHER"), 0.0, 100.0, 1.0, 35.0))),
    aiMaskBlur(Gtk::manage(new Adjuster(M("TP_LOCALLAB_AIMASK_BLUR"), 0.0, 50.0, 0.1, 0.0))),
    aiMaskInvert(Gtk::manage(new Gtk::CheckButton(M("TP_LOCALLAB_AIMASK_INVERT")))),
    aiMaskOpacity(Gtk::manage(new Adjuster(M("TP_LOCALLAB_AIMASK_OPACITY"), 0.0, 1.0, 0.01, 1.0))),
    aiMaskRefineRadius(Gtk::manage(new Adjuster(M("TP_LOCALLAB_AIMASK_REFINE_RADIUS"), 1, 32, 1, 8))),
    aiMaskRefineEps(Gtk::manage(new Adjuster(M("TP_LOCALLAB_AIMASK_REFINE_EPS"), 0.001, 0.5, 0.001, 0.01)))
{
    set_orientation(Gtk::ORIENTATION_VERTICAL);

    auto m = ProcEventMapper::getInstance();
    EvlocallabAIMask = m->newEvent(AUTOEXP, "HISTORY_MSG_LOCAL_AIMASK");

    const LocallabParams::LocallabSpot defSpot;

    // Class selector
    Gtk::Frame* const classFrame = Gtk::manage(new Gtk::Frame(M("TP_LOCALLAB_AIMASK_CLASS")));
    Gtk::Box* const classBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));

    aiMaskClassCombo->append(M("TP_LOCALLAB_AIMASK_CLASS_BACKGROUND"));
    aiMaskClassCombo->append(M("TP_LOCALLAB_AIMASK_CLASS_PERSON"));
    aiMaskClassCombo->append(M("TP_LOCALLAB_AIMASK_CLASS_SKY"));
    aiMaskClassCombo->append(M("TP_LOCALLAB_AIMASK_CLASS_VEGETATION"));
    aiMaskClassCombo->append(M("TP_LOCALLAB_AIMASK_CLASS_BUILDING"));
    aiMaskClassCombo->append(M("TP_LOCALLAB_AIMASK_CLASS_VEHICLE"));
    aiMaskClassCombo->append(M("TP_LOCALLAB_AIMASK_CLASS_ANIMAL"));
    aiMaskClassCombo->append(M("TP_LOCALLAB_AIMASK_CLASS_FOREGROUND"));
    // Composed pseudo-classes (AISegClass::SUBJECT / NOT_SUBJECT).
    aiMaskClassCombo->append(M("TP_LOCALLAB_AIMASK_CLASS_SUBJECT"));
    aiMaskClassCombo->append(M("TP_LOCALLAB_AIMASK_CLASS_NOTSUBJECT"));
    aiMaskClassCombo->set_active(defSpot.aiMaskClass);
    aiMaskClassConn = aiMaskClassCombo->signal_changed().connect(
        sigc::mem_fun(*this, &LocallabAIMask::aiMaskClassChanged));
    classBox->pack_start(*aiMaskClassCombo);
    classFrame->add(*classBox);
    pack_start(*classFrame);

    // Threshold & feathering
    aiMaskThreshold->setAdjusterListener(this);
    pack_start(*aiMaskThreshold);

    aiMaskFeather->setAdjusterListener(this);
    pack_start(*aiMaskFeather);

    aiMaskBlur->setAdjusterListener(this);
    pack_start(*aiMaskBlur);

    // Invert
    aiMaskInvertConn = aiMaskInvert->signal_toggled().connect(
        sigc::mem_fun(*this, &LocallabAIMask::aiMaskInvertChanged));
    pack_start(*aiMaskInvert);

    // Opacity (blend with geometric mask)
    aiMaskOpacity->setAdjusterListener(this);
    pack_start(*aiMaskOpacity);

    // Shape combine: how the spot's own ellipse/rectangle/gradient interacts
    // with the AI mask — opacity blend, union, or cut-out.
    Gtk::Box* const shapeOpBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 4));
    Gtk::Label* const shapeOpLabel = Gtk::manage(new Gtk::Label(M("TP_LOCALLAB_AIMASK_SHAPEOP") + ":"));
    shapeOpLabel->set_halign(Gtk::ALIGN_START);
    shapeOpBox->pack_start(*shapeOpLabel, Gtk::PACK_SHRINK);
    aiMaskShapeOpCombo->append(M("TP_LOCALLAB_AIMASK_SHAPEOP_BLEND"));
    aiMaskShapeOpCombo->append(M("TP_LOCALLAB_AIMASK_SHAPEOP_ADD"));
    aiMaskShapeOpCombo->append(M("TP_LOCALLAB_AIMASK_SHAPEOP_SUB"));
    aiMaskShapeOpCombo->set_active(0);
    aiMaskShapeOpConn = aiMaskShapeOpCombo->signal_changed().connect(
        sigc::mem_fun(*this, &LocallabAIMask::aiMaskShapeOpChanged));
    shapeOpBox->pack_start(*aiMaskShapeOpCombo);
    pack_start(*shapeOpBox);

    // Refinement
    Gtk::Frame* const refineFrame = Gtk::manage(new Gtk::Frame(M("TP_LOCALLAB_AIMASK_REFINE")));
    Gtk::Box* const refineBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));

    aiMaskRefineRadius->setAdjusterListener(this);
    refineBox->pack_start(*aiMaskRefineRadius);

    aiMaskRefineEps->setAdjusterListener(this);
    refineBox->pack_start(*aiMaskRefineEps);

    refineFrame->add(*refineBox);
    pack_start(*refineFrame);
}

LocallabAIMask::~LocallabAIMask()
{
}

bool LocallabAIMask::isMaskViewActive()
{
    return false;
}

void LocallabAIMask::resetMaskView()
{
}

void LocallabAIMask::getMaskView(int &colorMask, int &colorMaskinv, int &expMask, int &expMaskinv,
                                  int &shMask, int &shMaskinv, int &vibMask, int &softMask,
                                  int &blMask, int &tmMask, int &retiMask, int &sharMask,
                                  int &lcMask, int &cbMask, int &logMask, int &maskMask,
                                  int &cieMask)
{
    // AI masks use hoverMaskOverlay_ (via showMaskOverlay) for overlay display
    // rather than the combo-based mask preview system. Nothing to set here.
}

Gtk::ToggleButton* LocallabAIMask::getPreviewDeltaEButton() const
{
    return nullptr;
}

sigc::connection* LocallabAIMask::getPreviewDeltaEButtonConnection()
{
    return nullptr;
}

void LocallabAIMask::updateAdviceTooltips(const bool showTooltips)
{
    if (showTooltips) {
        aiMaskClassCombo->set_tooltip_text(M("TP_LOCALLAB_AIMASK_CLASS_TOOLTIP"));
        aiMaskThreshold->set_tooltip_text(M("TP_LOCALLAB_AIMASK_TOLERANCE_TOOLTIP"));
        aiMaskOpacity->set_tooltip_text(M("TP_LOCALLAB_AIMASK_OPACITY_TOOLTIP"));
        aiMaskShapeOpCombo->set_tooltip_text(M("TP_LOCALLAB_AIMASK_SHAPEOP_TOOLTIP"));
    } else {
        aiMaskClassCombo->set_tooltip_text("");
        aiMaskThreshold->set_tooltip_text("");
        aiMaskOpacity->set_tooltip_text("");
        aiMaskShapeOpCombo->set_tooltip_text("");
    }
}

void LocallabAIMask::disableListener()
{
    LocallabTool::disableListener();
    aiMaskClassConn.block(true);
    aiMaskInvertConn.block(true);
    aiMaskShapeOpConn.block(true);
}

void LocallabAIMask::enableListener()
{
    LocallabTool::enableListener();
    aiMaskClassConn.block(false);
    aiMaskInvertConn.block(false);
    aiMaskShapeOpConn.block(false);
}

void LocallabAIMask::read(const rtengine::procparams::ProcParams* pp, const ParamsEdited* pedited)
{
    disableListener();

    const int index = pp->locallab.selspot;

    if (index < (int)pp->locallab.spots.size()) {
        const LocallabParams::LocallabSpot& spot = pp->locallab.spots.at(index);

        exp->set_visible(true);
        exp->setEnabled(spot.expaimask);

        aiMaskClassCombo->set_active(spot.aiMaskClass);
        aiMaskThreshold->setValue(100.0 * (1.0 - spot.aiMaskThreshold));
        aiMaskFeather->setValue(spot.aiMaskFeather);
        aiMaskBlur->setValue(spot.aiMaskBlur);
        aiMaskInvert->set_active(spot.aiMaskInvert);
        aiMaskOpacity->setValue(spot.aiMaskOpacity);
        aiMaskShapeOpCombo->set_active(rtengine::LIM(spot.aiMaskShapeOp, 0, 2));
        aiMaskRefineRadius->setValue((double)spot.aiMaskRefineRadius);
        aiMaskRefineEps->setValue(spot.aiMaskRefineEps);
    }

    enableListener();
}

void LocallabAIMask::write(rtengine::procparams::ProcParams* pp, ParamsEdited* pedited)
{
    const int index = pp->locallab.selspot;

    if (index < (int)pp->locallab.spots.size()) {
        LocallabParams::LocallabSpot& spot = pp->locallab.spots.at(index);

        spot.visiaimask = true;
        spot.expaimask = exp->getEnabled();
        spot.useAIMask = exp->getEnabled();

        spot.aiMaskClass = aiMaskClassCombo->get_active_row_number();
        spot.aiMaskThreshold = rtengine::LIM(
            1.0 - aiMaskThreshold->getValue() / 100.0, 0.0, 1.0);
        spot.aiMaskFeather = aiMaskFeather->getValue();
        spot.aiMaskBlur = aiMaskBlur->getValue();
        spot.aiMaskInvert = aiMaskInvert->get_active();
        spot.aiMaskOpacity = aiMaskOpacity->getValue();
        spot.aiMaskShapeOp = rtengine::LIM(aiMaskShapeOpCombo->get_active_row_number(), 0, 2);
        spot.aiMaskRefineRadius = (int)aiMaskRefineRadius->getValue();
        spot.aiMaskRefineEps = aiMaskRefineEps->getValue();
    }

    // Note: No need to manage pedited as batch mode is deactivated for Locallab
}

void LocallabAIMask::setDefaults(const rtengine::procparams::ProcParams* defParams, const ParamsEdited* pedited)
{
    const int index = defParams->locallab.selspot;

    if (index < (int)defParams->locallab.spots.size()) {
        const LocallabParams::LocallabSpot& defSpot = defParams->locallab.spots.at(index);

        aiMaskThreshold->setDefault(100.0 * (1.0 - defSpot.aiMaskThreshold));
        aiMaskFeather->setDefault(defSpot.aiMaskFeather);
        aiMaskBlur->setDefault(defSpot.aiMaskBlur);
        aiMaskOpacity->setDefault(defSpot.aiMaskOpacity);
        aiMaskRefineRadius->setDefault((double)defSpot.aiMaskRefineRadius);
        aiMaskRefineEps->setDefault(defSpot.aiMaskRefineEps);
    }
}

void LocallabAIMask::adjusterChanged(Adjuster* a, double newval)
{
    if (isLocActivated && exp->getEnabled()) {
        if (listener) {
            if (a == aiMaskThreshold) {
                listener->panelChanged(EvlocallabAIMask, aiMaskThreshold->getTextValue());
            } else if (a == aiMaskFeather) {
                listener->panelChanged(EvlocallabAIMask, aiMaskFeather->getTextValue());
            } else if (a == aiMaskBlur) {
                listener->panelChanged(EvlocallabAIMask, aiMaskBlur->getTextValue());
            } else if (a == aiMaskOpacity) {
                listener->panelChanged(EvlocallabAIMask, aiMaskOpacity->getTextValue());
            } else if (a == aiMaskRefineRadius) {
                listener->panelChanged(EvlocallabAIMask, aiMaskRefineRadius->getTextValue());
            } else if (a == aiMaskRefineEps) {
                listener->panelChanged(EvlocallabAIMask, aiMaskRefineEps->getTextValue());
            }
        }
    }
}

void LocallabAIMask::complexityModeChanged()
{
}

void LocallabAIMask::enabledChanged()
{
    if (isLocActivated) {
        if (listener) {
            if (exp->getEnabled()) {
                listener->panelChanged(EvlocallabAIMask, M("GENERAL_ENABLED"));
            } else {
                listener->panelChanged(EvlocallabAIMask, M("GENERAL_DISABLED"));
            }
        }
    }
}

void LocallabAIMask::convertParamToNormal()
{
}

void LocallabAIMask::convertParamToSimple()
{
}

void LocallabAIMask::updateGUIToMode(const modeType complexity)
{
}

void LocallabAIMask::aiMaskClassChanged()
{
    if (isLocActivated && exp->getEnabled()) {
        if (listener) {
            listener->panelChanged(EvlocallabAIMask,
                                   aiMaskClassCombo->get_active_text());
        }
    }
}

void LocallabAIMask::aiMaskShapeOpChanged()
{
    if (isLocActivated && exp->getEnabled()) {
        if (listener) {
            listener->panelChanged(EvlocallabAIMask,
                                   aiMaskShapeOpCombo->get_active_text());
        }
    }
}

void LocallabAIMask::aiMaskInvertChanged()
{
    if (isLocActivated && exp->getEnabled()) {
        if (listener) {
            listener->panelChanged(EvlocallabAIMask,
                                   aiMaskInvert->get_active() ? M("TP_LOCALLAB_AIMASK_INVERTED") : M("TP_LOCALLAB_AIMASK_NORMAL"));
        }
    }
}

#endif // RT_AI_MASKING
