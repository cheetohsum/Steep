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
#include "relight.h"

#include <cmath>

#include "editwidgets.h"
#include "eventmapper.h"
#include "rtimage.h"

#include "rtengine/procparams.h"
#include "rtengine/rt_math.h"


using namespace rtengine;
using namespace rtengine::procparams;

namespace
{

enum GeometryIndex {
    LIGHT_CIRCLE,
};

}

const Glib::ustring Relight::TOOL_NAME = "relight";

Relight::Relight():
    FoldableToolPanel(this, TOOL_NAME, M("TP_RELIGHT_LABEL"), false, true),
    EditSubscriber(ET_OBJECTS),
    lastObject(-1)
{
    auto m = ProcEventMapper::getInstance();
    EvEnabled      = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_RELIGHT_ENABLED");
    EvIntensity    = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_RELIGHT_INTENSITY");
    EvPosition     = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_RELIGHT_POSITION");
    EvDistance     = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_RELIGHT_DISTANCE");
    EvReach        = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_RELIGHT_REACH");
    EvSubjectFocus = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_RELIGHT_SUBJECTFOCUS");
    EvBackground   = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_RELIGHT_BACKGROUND");
    EvShadows      = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_RELIGHT_SHADOWS");
    EvShape        = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_RELIGHT_SHAPE");
    EvWarmth       = m->newEvent(LUMINANCECURVE, "HISTORY_MSG_RELIGHT_WARMTH");

    const RelightParams defaults;
    // Brightness in stops; the parameter keeps fiftieths of a stop.
    intensity    = Gtk::manage(new Adjuster(M("TP_RELIGHT_INTENSITY"), 0., 4., 0.05, defaults.intensity / 50.0));
    lightX       = Gtk::manage(new Adjuster(M("TP_RELIGHT_LIGHTX"), -150., 150., 1., defaults.lightX));
    lightY       = Gtk::manage(new Adjuster(M("TP_RELIGHT_LIGHTY"), -150., 150., 1., defaults.lightY));
    distance     = Gtk::manage(new Adjuster(M("TP_RELIGHT_DISTANCE"), -100., 100., 1., defaults.distance));
    reach        = Gtk::manage(new Adjuster(M("TP_RELIGHT_REACH"), 5., 100., 1., defaults.reach));
    subjectFocus = Gtk::manage(new Adjuster(M("TP_RELIGHT_SUBJECTFOCUS"), 0., 100., 1., defaults.subjectFocus));
    background   = Gtk::manage(new Adjuster(M("TP_RELIGHT_BACKGROUND"), -100., 100., 1., defaults.background));
    shadows      = Gtk::manage(new Adjuster(M("TP_RELIGHT_SHADOWS"), 0., 100., 1., defaults.shadows));
    shape        = Gtk::manage(new Adjuster(M("TP_RELIGHT_SHAPE"), 0., 100., 1., defaults.shape));
    warmth       = Gtk::manage(new Adjuster(M("TP_RELIGHT_WARMTH"), -100., 100., 1., defaults.warmth));

    for (Adjuster* a : adjusters()) {
        a->setAdjusterListener(this);
    }

    warmth->setSliderGradient({
        GradientMilestone(0.0, 0.55, 0.70, 1.00),
        GradientMilestone(0.5, 0.85, 0.85, 0.85),
        GradientMilestone(1.0, 1.00, 0.70, 0.35)
    });
    background->setSliderGradient({
        GradientMilestone(0.0, 0.15, 0.15, 0.15),
        GradientMilestone(1.0, 0.90, 0.90, 0.90)
    });

    intensity->set_tooltip_text(M("TP_RELIGHT_INTENSITY_TOOLTIP"));
    distance->set_tooltip_text(M("TP_RELIGHT_DISTANCE_TOOLTIP"));
    reach->set_tooltip_text(M("TP_RELIGHT_REACH_TOOLTIP"));
    subjectFocus->set_tooltip_text(M("TP_RELIGHT_SUBJECTFOCUS_TOOLTIP"));
    background->set_tooltip_text(M("TP_RELIGHT_BACKGROUND_TOOLTIP"));
    shadows->set_tooltip_text(M("TP_RELIGHT_SHADOWS_TOOLTIP"));
    shape->set_tooltip_text(M("TP_RELIGHT_SHAPE_TOOLTIP"));

    edit = Gtk::manage(new Gtk::ToggleButton());
    edit->get_style_context()->add_class("independent");
    edit->set_image(*Gtk::manage(new RTImage("crosshair-adjust", Gtk::ICON_SIZE_BUTTON)));
    edit->set_label(M("TP_RELIGHT_PLACE"));
    edit->set_always_show_image(true);
    edit->set_tooltip_text(M("TP_RELIGHT_PLACE_TOOLTIP"));
    edit->set_halign(Gtk::ALIGN_START);
    editConn = edit->signal_toggled().connect(sigc::mem_fun(*this, &Relight::editToggled));


    const auto section = [this](const char* labelKey) {
        Gtk::Label* heading = Gtk::manage(new Gtk::Label(M(labelKey)));
        heading->set_halign(Gtk::ALIGN_START);
        heading->get_style_context()->add_class("tool-section-label");
        pack_start(*heading, Gtk::PACK_SHRINK);
    };

    get_style_context()->add_class("flush-tool");

    pack_start(*intensity);

    section("TP_RELIGHT_LIGHT");
    pack_start(*edit, Gtk::PACK_SHRINK);
    pack_start(*lightX);
    pack_start(*lightY);
    pack_start(*distance);
    pack_start(*reach);
    pack_start(*warmth);

    section("TP_RELIGHT_SCENE");
    pack_start(*subjectFocus);
    pack_start(*background);
    pack_start(*shadows);
    pack_start(*shape);

    // The light handle: a filled ball with a glow ring around it, and a
    // larger invisible twin to grab. Its size and opacity follow the light's
    // depth (see updateGeometry).
    Circle* visible = new Circle();
    visible->datum = Geometry::IMAGE;
    visible->radiusInImageSpace = false;
    visible->radius = 9;
    visible->filled = true;
    EditSubscriber::visibleGeometry.push_back(visible);

    Circle* glow = new Circle();
    glow->datum = Geometry::IMAGE;
    glow->radiusInImageSpace = false;
    glow->radius = 18;
    glow->filled = false;
    glow->innerLineWidth = 1.5f;
    EditSubscriber::visibleGeometry.push_back(glow);

    Circle* hit = new Circle();
    hit->datum = Geometry::IMAGE;
    hit->radiusInImageSpace = false;
    hit->radius = 16;
    hit->filled = true;
    EditSubscriber::mouseOverGeometry.push_back(hit);

    show_all();
}

Relight::~Relight()
{
    for (Geometry* g : visibleGeometry) {
        delete g;
    }

    for (Geometry* g : mouseOverGeometry) {
        delete g;
    }
}

std::array<Adjuster*, 10> Relight::adjusters() const
{
    return {{intensity, lightX, lightY, distance, reach, subjectFocus, background, shadows, shape, warmth}};
}

void Relight::read(const ProcParams* pp, const ParamsEdited* pedited)
{
    disableListener();

    const RelightParams& r = pp->relight;

    if (pedited) {
        const RelightParamsEdited& e = pedited->relight;
        intensity->setEditedState(e.intensity ? Edited : UnEdited);
        lightX->setEditedState(e.lightX ? Edited : UnEdited);
        lightY->setEditedState(e.lightY ? Edited : UnEdited);
        distance->setEditedState(e.distance ? Edited : UnEdited);
        reach->setEditedState(e.reach ? Edited : UnEdited);
        subjectFocus->setEditedState(e.subjectFocus ? Edited : UnEdited);
        background->setEditedState(e.background ? Edited : UnEdited);
        shadows->setEditedState(e.shadows ? Edited : UnEdited);
        shape->setEditedState(e.shape ? Edited : UnEdited);
        warmth->setEditedState(e.warmth ? Edited : UnEdited);
        set_inconsistent(multiImage && !e.enabled);
    }

    setEnabled(r.enabled);
    intensity->setValue(r.intensity / 50.0);
    lightX->setValue(r.lightX);
    lightY->setValue(r.lightY);
    distance->setValue(r.distance);
    reach->setValue(r.reach);
    subjectFocus->setValue(r.subjectFocus);
    background->setValue(r.background);
    shadows->setValue(r.shadows);
    shape->setValue(r.shape);
    warmth->setValue(r.warmth);

    updateGeometry();
    enableListener();
}

void Relight::write(ProcParams* pp, ParamsEdited* pedited)
{
    RelightParams& r = pp->relight;
    r.enabled = getEnabled();
    r.intensity = static_cast<int>(std::lround(intensity->getValue() * 50.0));
    r.lightX = lightX->getIntValue();
    r.lightY = lightY->getIntValue();
    r.distance = distance->getIntValue();
    r.reach = reach->getIntValue();
    r.subjectFocus = subjectFocus->getIntValue();
    r.background = background->getIntValue();
    r.shadows = shadows->getIntValue();
    r.shape = shape->getIntValue();
    r.warmth = warmth->getIntValue();

    if (pedited) {
        RelightParamsEdited& e = pedited->relight;
        e.enabled = !get_inconsistent();
        e.intensity = intensity->getEditedState();
        e.lightX = lightX->getEditedState();
        e.lightY = lightY->getEditedState();
        e.distance = distance->getEditedState();
        e.reach = reach->getEditedState();
        e.subjectFocus = subjectFocus->getEditedState();
        e.background = background->getEditedState();
        e.shadows = shadows->getEditedState();
        e.shape = shape->getEditedState();
        e.warmth = warmth->getEditedState();
    }
}

void Relight::setDefaults(const ProcParams* defParams, const ParamsEdited* pedited)
{
    const RelightParams& r = defParams->relight;
    intensity->setDefault(r.intensity / 50.0);
    lightX->setDefault(r.lightX);
    lightY->setDefault(r.lightY);
    distance->setDefault(r.distance);
    reach->setDefault(r.reach);
    subjectFocus->setDefault(r.subjectFocus);
    background->setDefault(r.background);
    shadows->setDefault(r.shadows);
    shape->setDefault(r.shape);
    warmth->setDefault(r.warmth);

    if (pedited) {
        const RelightParamsEdited& e = pedited->relight;
        intensity->setDefaultEditedState(e.intensity ? Edited : UnEdited);
        lightX->setDefaultEditedState(e.lightX ? Edited : UnEdited);
        lightY->setDefaultEditedState(e.lightY ? Edited : UnEdited);
        distance->setDefaultEditedState(e.distance ? Edited : UnEdited);
        reach->setDefaultEditedState(e.reach ? Edited : UnEdited);
        subjectFocus->setDefaultEditedState(e.subjectFocus ? Edited : UnEdited);
        background->setDefaultEditedState(e.background ? Edited : UnEdited);
        shadows->setDefaultEditedState(e.shadows ? Edited : UnEdited);
        shape->setDefaultEditedState(e.shape ? Edited : UnEdited);
        warmth->setDefaultEditedState(e.warmth ? Edited : UnEdited);
    } else {
        for (Adjuster* a : adjusters()) {
            a->setDefaultEditedState(Irrelevant);
        }
    }
}

void Relight::adjusterChanged(Adjuster* a, double newval)
{
    autoEnable();

    if (a == lightX || a == lightY || a == distance || a == intensity) {
        updateGeometry();
    }

    if (!listener || !getEnabled()) {
        return;
    }

    if (a == intensity) {
        listener->panelChanged(EvIntensity, a->getTextValue());
    } else if (a == lightX || a == lightY) {
        listener->panelChanged(EvPosition, Glib::ustring::compose("X=%1\nY=%2", lightX->getTextValue(), lightY->getTextValue()));
    } else if (a == distance) {
        listener->panelChanged(EvDistance, a->getTextValue());
    } else if (a == reach) {
        listener->panelChanged(EvReach, a->getTextValue());
    } else if (a == subjectFocus) {
        listener->panelChanged(EvSubjectFocus, a->getTextValue());
    } else if (a == background) {
        listener->panelChanged(EvBackground, a->getTextValue());
    } else if (a == shadows) {
        listener->panelChanged(EvShadows, a->getTextValue());
    } else if (a == shape) {
        listener->panelChanged(EvShape, a->getTextValue());
    } else if (a == warmth) {
        listener->panelChanged(EvWarmth, a->getTextValue());
    }
}

void Relight::enabledChanged()
{
    if (!listener) {
        return;
    }

    if (get_inconsistent()) {
        listener->panelChanged(EvEnabled, M("GENERAL_UNCHANGED"));
    } else if (getEnabled()) {
        listener->panelChanged(EvEnabled, M("GENERAL_ENABLED"));
    } else {
        listener->panelChanged(EvEnabled, M("GENERAL_DISABLED"));
    }
}

void Relight::setBatchMode(bool batchMode)
{
    ToolPanel::setBatchMode(batchMode);
    edit->hide();

    for (Adjuster* a : adjusters()) {
        a->showEditedCB();
    }
}

// ---- the light handle

void Relight::setEditProvider(EditDataProvider* provider)
{
    EditSubscriber::setEditProvider(provider);
}

void Relight::updateGeometry()
{
    EditDataProvider* provider = getEditProvider();

    if (!provider) {
        return;
    }

    int imW = 0;
    int imH = 0;
    provider->getImageSize(imW, imH);

    if (imW <= 0 || imH <= 0) {
        return;
    }

    const rtengine::Coord pos(
        int(std::lround(imW * (0.5 + lightX->getValue() / 200.))),
        int(std::lround(imH * (0.5 + lightY->getValue() / 200.))));
    auto* core = static_cast<Circle*>(visibleGeometry.at(LIGHT_CIRCLE));
    auto* glow = static_cast<Circle*>(visibleGeometry.at(LIGHT_CIRCLE + 1));
    core->center = pos;
    glow->center = pos;
    static_cast<Circle*>(mouseOverGeometry.at(LIGHT_CIRCLE))->center = pos;

    // Depth reads as size: toward the camera the ball swells, back toward
    // the subject it shrinks. Past the subject (positive depth) the light is
    // hidden behind it: the ball empties, the rings turn dashed and fade the
    // further back it goes -- an eclipse rather than a jump.
    const double depth = distance->getValue();            // -100 (camera) .. 100 (behind)
    const double brightness = intensity->getValue();      // EV
    const double size = depth <= 0 ? 1.0 + (-depth / 100.0) * 0.9 : 1.0 - (depth / 100.0) * 0.45;
    core->radius = std::max(3, int(std::lround(9.0 * size)));
    glow->radius = core->radius + std::max(4, int(std::lround((5.0 + brightness * 3.5) * size)));

    const bool behind = depth > 0;
    const double eclipse = behind ? std::min(1.0, depth / 60.0) : 0.0;
    core->filled = !behind;
    core->setDashed(behind);
    glow->setDashed(behind);
    core->opacity = static_cast<float>(100.0 - 55.0 * eclipse);
    glow->opacity = static_cast<float>((behind ? 70.0 : 85.0) - 45.0 * eclipse);
}

void Relight::editToggled()
{
    if (edit->get_active()) {
        updateGeometry();
        subscribe();
    } else {
        if (lastObject >= 0) {
            visibleGeometry.at(lastObject)->state = Geometry::NORMAL;
        }

        lastObject = -1;
        action = Action::NONE;
        unsubscribe();
    }
}

CursorShape Relight::getCursor(int objectID, int xPos, int yPos) const
{
    return objectID == LIGHT_CIRCLE ? CSMove2D : CSHandOpen;
}

bool Relight::mouseOver(int modifierKey)
{
    EditDataProvider* provider = getEditProvider();

    if (!provider || provider->getObject() == lastObject) {
        return false;
    }

    if (lastObject >= 0) {
        visibleGeometry.at(lastObject)->state = Geometry::NORMAL;
    }

    if (provider->getObject() >= 0) {
        visibleGeometry.at(provider->getObject())->state = Geometry::PRELIGHT;
    }

    lastObject = provider->getObject();
    return true;
}

bool Relight::button1Pressed(int modifierKey)
{
    EditDataProvider* provider = getEditProvider();

    if (lastObject < 0 || !provider || (modifierKey & GDK_CONTROL_MASK)) {
        return false;
    }

    int imW = 0;
    int imH = 0;
    provider->getImageSize(imW, imH);
    draggedLight.set(
        int(std::lround(imW * (0.5 + lightX->getValue() / 200.))),
        int(std::lround(imH * (0.5 + lightY->getValue() / 200.))));
    visibleGeometry.at(lastObject)->state = Geometry::DRAGGED;
    action = Action::DRAGGING;
    return false;
}

bool Relight::button1Released()
{
    if (lastObject >= 0) {
        visibleGeometry.at(lastObject)->state = Geometry::PRELIGHT;
    }

    action = Action::NONE;
    return true;
}

bool Relight::drag1(int modifierKey)
{
    EditDataProvider* provider = getEditProvider();

    if (!provider || lastObject != LIGHT_CIRCLE) {
        return false;
    }

    int imW = 0;
    int imH = 0;
    provider->getImageSize(imW, imH);

    if (imW <= 0 || imH <= 0) {
        return false;
    }

    // Unclamped: the light may stand outside the frame, as far as the
    // sliders reach.
    draggedLight += provider->deltaPrevImage;
    const int newX = rtengine::LIM(int(std::lround((double(draggedLight.x) / imW - 0.5) * 200.)), -150, 150);
    const int newY = rtengine::LIM(int(std::lround((double(draggedLight.y) / imH - 0.5) * 200.)), -150, 150);

    if (newX == lightX->getIntValue() && newY == lightY->getIntValue()) {
        return false;
    }

    disableListener();
    lightX->setValue(newX);
    lightY->setValue(newY);
    enableListener();
    updateGeometry();
    autoEnable();

    if (listener && getEnabled()) {
        listener->panelChanged(EvPosition, Glib::ustring::compose("X=%1\nY=%2", lightX->getTextValue(), lightY->getTextValue()));
    }

    return true;
}

void Relight::switchOffEditMode()
{
    if (edit->get_active()) {
        const bool wasBlocked = editConn.block(true);
        edit->set_active(false);

        if (!wasBlocked) {
            editConn.block(false);
        }
    }

    lastObject = -1;
    EditSubscriber::switchOffEditMode();
}
