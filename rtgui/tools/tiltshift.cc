/** -*- C++ -*-
 *
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
#include "tiltshift.h"

#include "editwidgets.h"
#include "rtimage.h"
#include "eventmapper.h"

#include "rtengine/procparams.h"
#include "rtengine/rt_math.h"

using namespace rtengine;
using namespace rtengine::procparams;

namespace
{

enum GeometryIndex {
    CENTER_LINE,        // Main horizontal line at focus center
    FOCUS_LINE_TOP,     // Top edge of focus band
    FOCUS_LINE_BOTTOM,  // Bottom edge of focus band
    FEATHER_LINE_TOP,   // Top feather boundary
    FEATHER_LINE_BOTTOM,// Bottom feather boundary
    CENTER_CIRCLE,      // Draggable center point
};

}

const Glib::ustring TiltShift::TOOL_NAME = "tiltshift";

TiltShift::TiltShift(): FoldableToolPanel(this, TOOL_NAME, M("TP_TILTSHIFT_LABEL"), false, true),
    EditSubscriber(ET_OBJECTS), lastObject(-1),
    detailExpanded_(false), draggedFeatherOffset(0.),
    dragRotating_(false), dragStartAngle_(0.)
{
    auto m = ProcEventMapper::getInstance();
    EvTiltShiftEnabled = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_TILTSHIFT_ENABLED");
    EvTiltShiftAmount = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_TILTSHIFT_AMOUNT");
    EvTiltShiftFocusPos = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_TILTSHIFT_FOCUSPOS");
    EvTiltShiftFocusWidth = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_TILTSHIFT_FOCUSWIDTH");
    EvTiltShiftFeather = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_TILTSHIFT_FEATHER");
    EvTiltShiftAngle = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_TILTSHIFT_ANGLE");

    // Label row: "Tilt-Shift" label on the left, edit toggle button on the right
    editHBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 0));
    auto *titleLabel = Gtk::manage(new Gtk::Label(M("TP_TILTSHIFT_LABEL")));
    titleLabel->set_halign(Gtk::ALIGN_START);
    editHBox->pack_start(*titleLabel, Gtk::PACK_EXPAND_WIDGET, 0);

    edit = Gtk::manage(new Gtk::ToggleButton());
    edit->get_style_context()->add_class("independent");
    edit->add(*Gtk::manage(new RTImage("crosshair-adjust", Gtk::ICON_SIZE_BUTTON)));
    edit->set_tooltip_text(M("EDIT_OBJECT_TOOLTIP"));
    editConn = edit->signal_toggled().connect(sigc::mem_fun(*this, &TiltShift::editToggled));
    editHBox->pack_end(*edit, Gtk::PACK_SHRINK, 0);

    amount = Gtk::manage(new Adjuster(M("TP_TILTSHIFT_AMOUNT"), 0., 100., 1., 0.));
    focusPos = Gtk::manage(new Adjuster(M("TP_TILTSHIFT_FOCUSPOS"), 0., 100., 1., 50.));
    focusWidth = Gtk::manage(new Adjuster(M("TP_TILTSHIFT_FOCUSWIDTH"), 0., 100., 1., 20.));
    feather = Gtk::manage(new Adjuster(M("TP_TILTSHIFT_FEATHER"), 0., 100., 1., 50.));
    angle = Gtk::manage(new Adjuster(M("TP_TILTSHIFT_ANGLE"), -45., 45., 1., 0.));

    amount->setAdjusterListener(this);
    focusPos->setAdjusterListener(this);
    focusWidth->setAdjusterListener(this);
    feather->setAdjusterListener(this);
    angle->setAdjusterListener(this);

    // Gradient: cool blue tilt-shift feel
    amount->setSliderGradient({
        GradientMilestone(0.0, 0.30, 0.40, 0.55),
        GradientMilestone(1.0, 0.50, 0.65, 0.80)
    });

    // Label-as-toggle: click the slider label to expand/collapse detail
    amount->setLabel(Glib::ustring("\u25B8 ") + M("TP_TILTSHIFT_AMOUNT"));
    amount->setLabelClickCallback([this]() { toggleDetail(); });

    auto *summaryBox = getSummaryBox();
    summaryBox->pack_start(*editHBox, Gtk::PACK_SHRINK, 0);
    summaryBox->pack_start(*amount);

    detailContent_ = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
    detailContent_->pack_start(*focusPos);
    detailContent_->pack_start(*focusWidth);
    detailContent_->pack_start(*feather);
    detailContent_->pack_start(*angle);
    detailContent_->show_all();

    detailRevealer_ = Gtk::manage(new Gtk::Revealer());
    detailRevealer_->set_transition_type(Gtk::REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    detailRevealer_->set_transition_duration(200);
    detailRevealer_->set_reveal_child(false);
    detailRevealer_->add(*detailContent_);
    detailRevealer_->show();
    summaryBox->pack_start(*detailRevealer_, Gtk::PACK_SHRINK);

    summaryBox->show_all();

    // --- Editing geometry (lines + circle on the image preview) ---
    // We draw: center line, top/bottom focus edges, top/bottom feather edges, center circle
    // Visible geometry
    for (int i = 0; i < 5; i++) {
        Line *line = new Line();
        line->innerLineWidth = (i == 0) ? 2 : 1;
        line->datum = Geometry::IMAGE;
        EditSubscriber::visibleGeometry.push_back(line);
    }
    Circle *vc = new Circle();
    vc->datum = Geometry::IMAGE;
    vc->radiusInImageSpace = false;
    vc->radius = 6;
    vc->filled = true;
    EditSubscriber::visibleGeometry.push_back(vc);

    // MouseOver geometry (larger hit areas)
    for (int i = 0; i < 5; i++) {
        Line *line = new Line();
        line->innerLineWidth = (i == 0) ? 2 : 1;
        line->datum = Geometry::IMAGE;
        EditSubscriber::mouseOverGeometry.push_back(line);
    }
    Circle *mc = new Circle();
    mc->datum = Geometry::IMAGE;
    mc->radiusInImageSpace = false;
    mc->radius = 12;
    mc->filled = true;
    EditSubscriber::mouseOverGeometry.push_back(mc);

    show_all();
}

TiltShift::~TiltShift()
{
    for (auto *g : visibleGeometry) {
        delete g;
    }
    for (auto *g : mouseOverGeometry) {
        delete g;
    }
}

void TiltShift::toggleDetail()
{
    detailExpanded_ = !detailExpanded_;
    amount->setLabel(Glib::ustring(detailExpanded_ ? "\u25BE " : "\u25B8 ") + M("TP_TILTSHIFT_AMOUNT"));
    detailRevealer_->set_reveal_child(detailExpanded_);
}

void TiltShift::updateGeometry(int focusPosVal, int focusWidthVal, int featherVal, int angleVal, int fullWidth, int fullHeight)
{
    EditDataProvider *dataProvider = getEditProvider();
    if (!dataProvider) {
        return;
    }

    int imW = 0, imH = 0;
    if (fullWidth != -1 && fullHeight != -1) {
        imW = fullWidth;
        imH = fullHeight;
    } else {
        dataProvider->getImageSize(imW, imH);
        if (!imW || !imH) {
            return;
        }
    }

    // Convert params to pixel coords
    double centerY = (focusPosVal / 100.0) * imH;
    double halfBand = (focusWidthVal / 100.0) * imH * 0.5;
    double featherPx = (featherVal / 100.0) * imH * 0.3;
    double angleDeg = angleVal;

    double centerX = imW / 2.0;

    // Helper: create a line across the image at a given Y offset from center, with angle
    auto setLine = [&](Geometry *geom, double yPos) {
        Line *line = static_cast<Line*>(geom);
        double tanA = std::tan(angleDeg * rtengine::RT_PI / 180.0);
        // Left edge: x=0, y = yPos + (0 - centerX) * tanA
        // Right edge: x=imW, y = yPos + (imW - centerX) * tanA
        double yLeft = yPos + (0 - centerX) * tanA;
        double yRight = yPos + (imW - centerX) * tanA;
        line->begin = rtengine::Coord(0, int(yLeft));
        line->end = rtengine::Coord(imW, int(yRight));
    };

    auto setCircle = [&](Geometry *geom) {
        Circle *circle = static_cast<Circle*>(geom);
        circle->center = rtengine::Coord(int(centerX), int(centerY));
    };

    // Center line
    setLine(visibleGeometry.at(CENTER_LINE), centerY);
    setLine(mouseOverGeometry.at(CENTER_LINE), centerY);

    // Focus band edges
    setLine(visibleGeometry.at(FOCUS_LINE_TOP), centerY - halfBand);
    setLine(mouseOverGeometry.at(FOCUS_LINE_TOP), centerY - halfBand);
    setLine(visibleGeometry.at(FOCUS_LINE_BOTTOM), centerY + halfBand);
    setLine(mouseOverGeometry.at(FOCUS_LINE_BOTTOM), centerY + halfBand);

    // Feather boundaries
    setLine(visibleGeometry.at(FEATHER_LINE_TOP), centerY - halfBand - featherPx);
    setLine(mouseOverGeometry.at(FEATHER_LINE_TOP), centerY - halfBand - featherPx);
    setLine(visibleGeometry.at(FEATHER_LINE_BOTTOM), centerY + halfBand + featherPx);
    setLine(mouseOverGeometry.at(FEATHER_LINE_BOTTOM), centerY + halfBand + featherPx);

    // Center circle
    setCircle(visibleGeometry.at(CENTER_CIRCLE));
    setCircle(mouseOverGeometry.at(CENTER_CIRCLE));
}

void TiltShift::read(const ProcParams *pp, const ParamsEdited *pedited)
{
    disableListener();

    if (pedited) {
        amount->setEditedState(pedited->tiltShift.amount ? Edited : UnEdited);
        focusPos->setEditedState(pedited->tiltShift.focusPos ? Edited : UnEdited);
        focusWidth->setEditedState(pedited->tiltShift.focusWidth ? Edited : UnEdited);
        feather->setEditedState(pedited->tiltShift.feather ? Edited : UnEdited);
        angle->setEditedState(pedited->tiltShift.angle ? Edited : UnEdited);
        set_inconsistent(multiImage && !pedited->tiltShift.enabled);
    }

    setEnabled(pp->tiltShift.enabled);
    amount->setValue(pp->tiltShift.amount);
    focusPos->setValue(pp->tiltShift.focusPos);
    focusWidth->setValue(pp->tiltShift.focusWidth);
    feather->setValue(pp->tiltShift.feather);
    angle->setValue(pp->tiltShift.angle);

    updateGeometry(pp->tiltShift.focusPos, pp->tiltShift.focusWidth, pp->tiltShift.feather, pp->tiltShift.angle);

    enableListener();
}

void TiltShift::write(ProcParams *pp, ParamsEdited *pedited)
{
    pp->tiltShift.amount = amount->getValue();
    pp->tiltShift.focusPos = focusPos->getValue();
    pp->tiltShift.focusWidth = focusWidth->getValue();
    pp->tiltShift.feather = feather->getValue();
    pp->tiltShift.angle = angle->getValue();
    pp->tiltShift.enabled = getEnabled();

    if (pedited) {
        pedited->tiltShift.amount = amount->getEditedState();
        pedited->tiltShift.focusPos = focusPos->getEditedState();
        pedited->tiltShift.focusWidth = focusWidth->getEditedState();
        pedited->tiltShift.feather = feather->getEditedState();
        pedited->tiltShift.angle = angle->getEditedState();
        pedited->tiltShift.enabled = !get_inconsistent();
    }
}

void TiltShift::setDefaults(const ProcParams *defParams, const ParamsEdited *pedited)
{
    amount->setDefault(defParams->tiltShift.amount);
    focusPos->setDefault(defParams->tiltShift.focusPos);
    focusWidth->setDefault(defParams->tiltShift.focusWidth);
    feather->setDefault(defParams->tiltShift.feather);
    angle->setDefault(defParams->tiltShift.angle);

    if (pedited) {
        amount->setDefaultEditedState(pedited->tiltShift.amount ? Edited : UnEdited);
        focusPos->setDefaultEditedState(pedited->tiltShift.focusPos ? Edited : UnEdited);
        focusWidth->setDefaultEditedState(pedited->tiltShift.focusWidth ? Edited : UnEdited);
        feather->setDefaultEditedState(pedited->tiltShift.feather ? Edited : UnEdited);
        angle->setDefaultEditedState(pedited->tiltShift.angle ? Edited : UnEdited);
    } else {
        amount->setDefaultEditedState(Irrelevant);
        focusPos->setDefaultEditedState(Irrelevant);
        focusWidth->setDefaultEditedState(Irrelevant);
        feather->setDefaultEditedState(Irrelevant);
        angle->setDefaultEditedState(Irrelevant);
    }
}

void TiltShift::adjusterChanged(Adjuster* a, double newval)
{
    autoEnable();
    updateGeometry(int(focusPos->getValue()), int(focusWidth->getValue()), int(feather->getValue()), int(angle->getValue()));

    if (listener && getEnabled()) {
        if (a == amount) {
            listener->panelChanged(EvTiltShiftAmount, a->getTextValue());
        } else if (a == focusPos) {
            listener->panelChanged(EvTiltShiftFocusPos, a->getTextValue());
        } else if (a == focusWidth) {
            listener->panelChanged(EvTiltShiftFocusWidth, a->getTextValue());
        } else if (a == feather) {
            listener->panelChanged(EvTiltShiftFeather, a->getTextValue());
        } else if (a == angle) {
            listener->panelChanged(EvTiltShiftAngle, a->getTextValue());
        }
    }
}

void TiltShift::enabledChanged()
{
    if (listener) {
        if (get_inconsistent()) {
            listener->panelChanged(EvTiltShiftEnabled, M("GENERAL_UNCHANGED"));
        } else if (getEnabled()) {
            listener->panelChanged(EvTiltShiftEnabled, M("GENERAL_ENABLED"));
        } else {
            listener->panelChanged(EvTiltShiftEnabled, M("GENERAL_DISABLED"));
        }
    }
}

void TiltShift::setBatchMode(bool batchMode)
{
    editConn.disconnect();
    removeIfThere(this, editHBox, false);
    ToolPanel::setBatchMode(batchMode);

    amount->showEditedCB();
    focusPos->showEditedCB();
    focusWidth->showEditedCB();
    feather->showEditedCB();
    angle->showEditedCB();
}

void TiltShift::setEditProvider(EditDataProvider *provider)
{
    EditSubscriber::setEditProvider(provider);
}

void TiltShift::editToggled()
{
    if (edit->get_active()) {
        subscribe();
    } else {
        releaseEdit();
        unsubscribe();
    }
}

void TiltShift::releaseEdit()
{
    if (lastObject >= 0) {
        EditSubscriber::visibleGeometry.at(lastObject)->state = Geometry::NORMAL;
    }
    action = Action::NONE;
}

void TiltShift::switchOffEditMode()
{
    if (edit->get_active()) {
        bool wasBlocked = editConn.block(true);
        edit->set_active(false);
        if (!wasBlocked) {
            editConn.block(false);
        }
    }
    EditSubscriber::switchOffEditMode();
}

CursorShape TiltShift::getCursor(int objectID, int xPos, int yPos) const
{
    if (objectID == CENTER_CIRCLE) {
        return CSMove2D;
    }
    if (objectID >= CENTER_LINE && objectID <= FEATHER_LINE_BOTTOM) {
        // Near line endpoints (left/right 25%) → rotate cursor
        EditDataProvider *dp = getEditProvider();
        if (dp) {
            int imW = 0, imH = 0;
            dp->getImageSize(imW, imH);
            if (imW > 0) {
                double relX = double(xPos) / imW;
                if (relX < 0.25 || relX > 0.75) {
                    return CSMoveRotate;
                }
            }
        }
        if (objectID == CENTER_LINE) {
            return CSMove2D;
        }
        return CSMove1DV;
    }
    return CSHandOpen;
}

bool TiltShift::mouseOver(int modifierKey)
{
    EditDataProvider *editProvider = getEditProvider();

    if (editProvider && editProvider->getObject() != lastObject) {
        if (lastObject > -1) {
            EditSubscriber::visibleGeometry.at(lastObject)->state = Geometry::NORMAL;
        }

        if (editProvider->getObject() > -1) {
            EditSubscriber::visibleGeometry.at(editProvider->getObject())->state = Geometry::PRELIGHT;
        }

        lastObject = editProvider->getObject();
        return true;
    }

    return false;
}

bool TiltShift::button1Pressed(int modifierKey)
{
    if (lastObject < 0) {
        return false;
    }

    EditDataProvider *provider = getEditProvider();

    if (!(modifierKey & GDK_CONTROL_MASK)) {
        int imW, imH;
        provider->getImageSize(imW, imH);
        double centerY = (focusPos->getValue() / 100.0) * imH;
        draggedCenter.set(imW / 2, int(centerY));

        // Detect rotation: clicking near the left/right 25% of the image on any line
        double relX = double(provider->posImage.x) / imW;
        dragRotating_ = (lastObject != CENTER_CIRCLE) && (relX < 0.25 || relX > 0.75);
        dragStartAngle_ = angle->getValue();

        if (!dragRotating_ && (lastObject == FEATHER_LINE_TOP || lastObject == FEATHER_LINE_BOTTOM)) {
            // Record offset between mouse and feather line for smooth dragging
            double halfBand = (focusWidth->getValue() / 100.0) * imH * 0.5;
            double featherPx = (feather->getValue() / 100.0) * imH * 0.3;
            double lineY;
            if (lastObject == FEATHER_LINE_TOP) {
                lineY = centerY - halfBand - featherPx;
            } else {
                lineY = centerY + halfBand + featherPx;
            }
            draggedFeatherOffset = provider->posImage.y - lineY;
        }

        EditSubscriber::action = EditSubscriber::Action::DRAGGING;
        return false;
    } else {
        EditSubscriber::visibleGeometry.at(lastObject)->state = Geometry::NORMAL;
        lastObject = -1;
        return true;
    }
}

bool TiltShift::button1Released()
{
    EditSubscriber::action = EditSubscriber::Action::NONE;
    return true;
}

bool TiltShift::drag1(int modifierKey)
{
    EditDataProvider *provider = getEditProvider();
    int imW, imH;
    provider->getImageSize(imW, imH);

    // Rotation mode: compute angle from mouse position relative to center
    if (dragRotating_) {
        double centerYpx = (focusPos->getValue() / 100.0) * imH;
        double centerXpx = imW / 2.0;
        double mouseX = provider->posImage.x + provider->deltaImage.x;
        double mouseY = provider->posImage.y + provider->deltaImage.y;
        double dx = mouseX - centerXpx;
        double dy = mouseY - centerYpx;
        // atan2 gives angle from horizontal; we want angle of the focus band
        double newAngle = std::atan2(dy, dx) * 180.0 / rtengine::RT_PI;
        newAngle = rtengine::LIM(newAngle, -45.0, 45.0);
        int intAngle = int(newAngle);
        if (intAngle != angle->getIntValue()) {
            angle->setValue(intAngle);
            updateGeometry(focusPos->getIntValue(), focusWidth->getIntValue(), feather->getIntValue(), angle->getIntValue());
            if (listener) {
                listener->panelChanged(EvTiltShiftAngle, angle->getTextValue());
            }
            return true;
        }
        return false;
    }

    if (lastObject == CENTER_CIRCLE || lastObject == CENTER_LINE) {
        // Drag to move focus position vertically
        double newY = provider->posImage.y + provider->deltaImage.y;
        int newFocusPos = int((newY / imH) * 100.0 + 0.5);
        newFocusPos = rtengine::LIM(newFocusPos, 0, 100);

        if (newFocusPos != focusPos->getIntValue()) {
            focusPos->setValue(newFocusPos);
            updateGeometry(focusPos->getIntValue(), focusWidth->getIntValue(), feather->getIntValue(), angle->getIntValue());
            if (listener) {
                listener->panelChanged(EvTiltShiftFocusPos, focusPos->getTextValue());
            }
            return true;
        }
    } else if (lastObject == FOCUS_LINE_TOP || lastObject == FOCUS_LINE_BOTTOM) {
        // Drag to change focus width
        double centerY = (focusPos->getValue() / 100.0) * imH;
        double mouseY = provider->posImage.y + provider->deltaImage.y;
        double dist = std::abs(mouseY - centerY);
        // dist is half the band in pixels; convert to % of imH
        int newWidth = int((dist / (imH * 0.5)) * 100.0 + 0.5);
        newWidth = rtengine::LIM(newWidth, 0, 100);

        if (newWidth != focusWidth->getIntValue()) {
            focusWidth->setValue(newWidth);
            updateGeometry(focusPos->getIntValue(), focusWidth->getIntValue(), feather->getIntValue(), angle->getIntValue());
            if (listener) {
                listener->panelChanged(EvTiltShiftFocusWidth, focusWidth->getTextValue());
            }
            return true;
        }
    } else if (lastObject == FEATHER_LINE_TOP || lastObject == FEATHER_LINE_BOTTOM) {
        // Drag to change feather
        double centerY = (focusPos->getValue() / 100.0) * imH;
        double halfBand = (focusWidth->getValue() / 100.0) * imH * 0.5;
        double mouseY = provider->posImage.y + provider->deltaImage.y - draggedFeatherOffset;
        double edgeY;
        if (lastObject == FEATHER_LINE_TOP) {
            edgeY = centerY - halfBand;
            double featherPx = edgeY - mouseY;
            int newFeather = int((featherPx / (imH * 0.3)) * 100.0 + 0.5);
            newFeather = rtengine::LIM(newFeather, 0, 100);
            if (newFeather != feather->getIntValue()) {
                feather->setValue(newFeather);
                updateGeometry(focusPos->getIntValue(), focusWidth->getIntValue(), feather->getIntValue(), angle->getIntValue());
                if (listener) {
                    listener->panelChanged(EvTiltShiftFeather, feather->getTextValue());
                }
                return true;
            }
        } else {
            edgeY = centerY + halfBand;
            double featherPx = mouseY - edgeY;
            int newFeather = int((featherPx / (imH * 0.3)) * 100.0 + 0.5);
            newFeather = rtengine::LIM(newFeather, 0, 100);
            if (newFeather != feather->getIntValue()) {
                feather->setValue(newFeather);
                updateGeometry(focusPos->getIntValue(), focusWidth->getIntValue(), feather->getIntValue(), angle->getIntValue());
                if (listener) {
                    listener->panelChanged(EvTiltShiftFeather, feather->getTextValue());
                }
                return true;
            }
        }
    }

    return false;
}
