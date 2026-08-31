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

#include "editbuffer.h"
#include "editcallbacks.h"
#include "imagearea.h"
#include "spot.h"
#include "rtimage.h"
#include <iomanip>
#include "rtengine/rt_math.h"
#include "guiutils.h"
#include "eventmapper.h"
#include "rtengine/refreshmap.h"
#ifdef RT_AI_MASKING
#include "rtengine/aiinpainting.h"
#endif

using namespace rtengine;
using namespace rtengine::procparams;

namespace
{

enum GeometryIndex {
    MO_TARGET_DISK,
    MO_SOURCE_DISC,
    MO_TARGET_CIRCLE,
    MO_SOURCE_CIRCLE,
    MO_TARGET_FEATHER_CIRCLE,
    MO_SOURCE_FEATHER_CIRCLE,
    MO_OBJECT_COUNT,

    VISIBLE_SOURCE_ICON = 0,
    VISIBLE_SOURCE_FEATHER_CIRCLE,
    VISIBLE_LINK,
    VISIBLE_SOURCE_CIRCLE,
    VISIBLE_TARGET_FEATHER_CIRCLE,
    VISIBLE_TARGET_CIRCLE,
    VISIBLE_CURSOR_PREVIEW,
    VISIBLE_STROKE_PREVIEW,
    VISIBLE_OBJECT_COUNT
};

}

// ---- Phase 3: SpotSizePreview implementation ----

Spot::SpotSizePreview::SpotSizePreview()
{
    set_size_request(24, 24);
}

void Spot::SpotSizePreview::setValue(int radius)
{
    currentRadius = radius;
    queue_draw();
}

bool Spot::SpotSizePreview::on_draw(const Cairo::RefPtr<Cairo::Context>& cr)
{
    int w = get_allocated_width();
    int h = get_allocated_height();

    // Map radius [1,400] -> diameter [4px, min(w,h)-2]
    int maxDia = std::min(w, h) - 2;
    int minDia = 4;
    double t = double(currentRadius - 1) / double(400 - 1);
    int dia = int(minDia + t * (maxDia - minDia) + 0.5);
    dia = std::max(minDia, std::min(maxDia, dia));

    cr->set_source_rgba(0.7, 0.7, 0.7, 0.8);
    cr->arc(w / 2.0, h / 2.0, dia / 2.0, 0, 2.0 * M_PI);
    cr->fill();

    return true;
}

// ---- BrushCursorRing implementation ----

void BrushCursorRing::drawInnerGeometry (Cairo::RefPtr<Cairo::Context> &cr, ObjectMOBuffer *objectBuffer, EditCoordSystem &coordSystem)
{
    Circle::drawInnerGeometry (cr, objectBuffer, coordSystem);

    if (!(flags & F_VISIBLE) || busyPhase < 0.f) {
        return;
    }

    // Rotating arc riding just outside the brush ring. Brush mode hides the
    // system pointer, so this is what tells the user the repair is still
    // being computed rather than finished-and-unchanged.
    rtengine::Coord centre = center;
    double radius_ = radiusInImageSpace ? coordSystem.scaleValueToCanvas (double (radius)) : double (radius);

    if (datum == IMAGE) {
        coordSystem.imageCoordToScreen (center.x, center.y, centre.x, centre.y);
    } else if (datum == CLICKED_POINT) {
        centre += objectBuffer->getDataProvider()->posScreen;
    } else if (datum == CURSOR) {
        centre += objectBuffer->getDataProvider()->posScreen + objectBuffer->getDataProvider()->deltaScreen;
    }

    // Keep the spinner legible whatever the brush size / zoom.
    const double arcRadius = rtengine::max (radius_ + 6., 11.);
    const double start = double (busyPhase) * 2. * rtengine::RT_PI;
    const double sweep = 0.62 * rtengine::RT_PI;

    cr->save();
    cr->unset_dash();
    cr->set_line_cap (Cairo::LINE_CAP_ROUND);

    // Dark backing so the arc reads on light and dark photos alike.
    cr->set_line_width (4.);
    cr->set_source_rgba (0., 0., 0., 0.55);
    cr->begin_new_path();
    cr->arc (centre.x + 0.5, centre.y + 0.5, arcRadius, start, start + sweep);
    cr->stroke();

    cr->set_line_width (2.);
    cr->set_source_rgba (1.0, 0.78, 0.25, 0.95);
    cr->begin_new_path();
    cr->arc (centre.x + 0.5, centre.y + 0.5, arcRadius, start, start + sweep);
    cr->stroke();
    cr->restore();
}

// ---- BrushAreaOverlay implementation ----

void BrushAreaOverlay::drawOuterGeometry (Cairo::RefPtr<Cairo::Context> &cr, ObjectMOBuffer *objectBuffer, EditCoordSystem &coordSystem)
{
    // Everything is drawn in the inner pass — the outer dark halo of stock
    // geometry would double the apparent border width of the overlay.
}

void BrushAreaOverlay::drawInnerGeometry (Cairo::RefPtr<Cairo::Context> &cr, ObjectMOBuffer *objectBuffer, EditCoordSystem &coordSystem)
{
    if (!(flags & F_VISIBLE) || points.empty()) {
        return;
    }

    double diameter = lineWidthInImageSpace ? coordSystem.scaleValueToCanvas (double (innerLineWidth)) : double (innerLineWidth);
    diameter = rtengine::max (diameter, 2.);
    const double borderWidth = 1.5; // screen-space ring around the whole area

    RGBColor color = innerLineColor;

    const auto buildPath = [&]() {
        cr->begin_new_path();
        rtengine::Coord currPos;

        for (unsigned int i = 0; i < points.size(); ++i) {
            currPos = points.at (i);

            if (datum == IMAGE) {
                coordSystem.imageCoordToScreen (points.at (i).x, points.at (i).y, currPos.x, currPos.y);
            } else if (datum == CLICKED_POINT) {
                currPos += objectBuffer->getDataProvider()->posScreen;
            } else if (datum == CURSOR) {
                currPos += objectBuffer->getDataProvider()->posScreen + objectBuffer->getDataProvider()->deltaScreen;
            }

            if (!i) {
                cr->move_to (currPos.x + 0.5, currPos.y + 0.5);
            } else {
                cr->line_to (currPos.x + 0.5, currPos.y + 0.5);
            }
        }

        if (points.size() == 1) {
            // A degenerate segment so round caps render a single dab as a disc.
            cr->rel_line_to (0.01, 0.);
        }
    };

    cr->save();
    cr->push_group();
    cr->set_line_cap (Cairo::LINE_CAP_ROUND);
    cr->set_line_join (Cairo::LINE_JOIN_ROUND);

    // Pass 1: opaque stroke, wider by the border — its visible remainder
    // after pass 2 is exactly the outline of the swept area.
    buildPath();
    cr->set_source_rgba (color.getR(), color.getG(), color.getB(), 0.9);
    cr->set_line_width (diameter + 2. * borderWidth);
    cr->stroke();

    // Pass 2: interior at brush width, SOURCE operator so it *replaces* the
    // opaque pass inside the area, leaving a translucent fill + border ring.
    buildPath();
    cr->set_operator (Cairo::OPERATOR_SOURCE);
    cr->set_source_rgba (color.getR(), color.getG(), color.getB(), 0.22);
    cr->set_line_width (diameter);
    cr->stroke();

    cr->pop_group_to_source();
    cr->set_operator (Cairo::OPERATOR_OVER);
    cr->paint();
    cr->restore();
}

void BrushAreaOverlay::drawToMOChannel (Cairo::RefPtr<Cairo::Context> &cr, unsigned short id, ObjectMOBuffer *objectBuffer, EditCoordSystem &coordSystem)
{
    // Not hoverable — purely a visual overlay.
}

// ---- Main Spot implementation ----

const Glib::ustring Spot::TOOL_NAME = "spot";

Spot::Spot() :
    FoldableToolPanel(this, TOOL_NAME, M ("TP_SPOT_LABEL"), true, true),
    EditSubscriber(ET_OBJECTS),
    draggedSide(DraggedSide::NONE),
    lastObject(-1),
    activeSpot(-1),
    sourceIcon("spot-normal", "spot-active", "spot-prelight", "", "", Geometry::DP_CENTERCENTER),
    editedCheckBox(nullptr)
{
    countLabel = Gtk::manage (new Gtk::Label (""));
    countLabel->set_no_show_all(true);

    edit = Gtk::manage (new Gtk::ToggleButton());
    edit->add (*Gtk::manage (new RTImage ("spot-eraser")));
    editConn = edit->signal_toggled().connect ( sigc::mem_fun (*this, &Spot::editToggled) );
    edit->set_tooltip_text(M("TP_SPOT_HINT"));

    reset = Gtk::manage (new Gtk::Button ());
    reset->add (*Gtk::manage (new RTImage ("undo-small")));
    reset->set_relief (Gtk::RELIEF_NONE);
    reset->set_border_width (0);
    reset->signal_clicked().connect ( sigc::mem_fun (*this, &Spot::resetPressed) );

    spotSize = Gtk::manage(new Adjuster("", SpotParams::minRadius, SpotParams::maxRadius, 1, 25));
    spotSize->setAdjusterListener(this);

    // Phase 1: Method toggle buttons (icon-only toolbar)
    methodBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 0));
    methodBox->get_style_context()->add_class("linked");

    auto makeMethodButton = [&](const Glib::ustring& iconName, const Glib::ustring& tooltip) -> Gtk::ToggleButton* {
        Gtk::ToggleButton* btn = Gtk::manage(new Gtk::ToggleButton());
        btn->add(*Gtk::manage(new RTImage(iconName)));
        btn->set_tooltip_text(tooltip);
        btn->set_relief(Gtk::RELIEF_NONE);
        return btn;
    };

    btnClone = makeMethodButton("spot-clone", M("TP_SPOT_METHOD_CLONE"));
    btnHeal = makeMethodButton("spot-heal", M("TP_SPOT_METHOD_HEAL"));
    btnErase = makeMethodButton("spot-erase", M("TP_SPOT_METHOD_ERASE"));
    btnRedEye = makeMethodButton("spot-redeye", M("TP_SPOT_METHOD_REDEYE"));

    // No button active initially — clicking any button activates that mode + enters edit
    cloneConn = btnClone->signal_toggled().connect(sigc::bind(sigc::mem_fun(*this, &Spot::onMethodButtonToggled), btnClone, 0));
    healConn = btnHeal->signal_toggled().connect(sigc::bind(sigc::mem_fun(*this, &Spot::onMethodButtonToggled), btnHeal, 1));
    eraseConn = btnErase->signal_toggled().connect(sigc::bind(sigc::mem_fun(*this, &Spot::onMethodButtonToggled), btnErase, 2));
    redeyeConn = btnRedEye->signal_toggled().connect(sigc::bind(sigc::mem_fun(*this, &Spot::onMethodButtonToggled), btnRedEye, 3));

    methodBox->pack_start(*btnClone, false, false, 0);
    methodBox->pack_start(*btnHeal, false, false, 0);
    methodBox->pack_start(*btnErase, false, false, 0);
    methodBox->pack_start(*btnRedEye, false, false, 0);

    // Phase 3: Size preview widget
    sizePreview = Gtk::manage(new SpotSizePreview());

    // AI placeholder section (collapsible)
    aiSection = Gtk::manage(new AdvancedSection(M("TP_SPOT_AI_SECTION")));
    Gtk::Box* aiContent = aiSection->getContentBox();

    // 2×2 grid of equal-width tool buttons; each per-tool reset "↺" floats
    // inside its button's right edge instead of taking a slot of its own.
    Gtk::Grid* aiGrid = Gtk::manage(new Gtk::Grid());
    aiGrid->set_column_homogeneous(true);
    aiGrid->set_column_spacing(4);
    aiGrid->set_row_spacing(4);
    aiGrid->set_hexpand(true);

    const auto embedReset = [](Gtk::Button* toolBtn, Gtk::Button* resetBtn) -> Gtk::Widget* {
        Gtk::Overlay* overlay = Gtk::manage(new Gtk::Overlay());
        toolBtn->set_hexpand(true);
        overlay->add(*toolBtn);
        resetBtn->get_style_context()->add_class("smart-tool-reset");
        resetBtn->set_halign(Gtk::ALIGN_END);
        resetBtn->set_valign(Gtk::ALIGN_CENTER);
        resetBtn->set_margin_end(3);
        overlay->add_overlay(*resetBtn);
        return overlay;
    };

    // Remove Object is live: a fifth method toggle driving the AI_REMOVE
    // brush (paint over the object, LaMa fills it). Available only when the
    // inpainting engine loaded its model at startup.
    btnAIRemove = Gtk::manage(new Gtk::ToggleButton(M("TP_SPOT_AI_OBJECT")));
    btnAIRemove->set_relief(Gtk::RELIEF_NONE);
#ifdef RT_AI_MASKING
    if (rtengine::getAIInpaintingEngine().isInitialized()) {
        btnAIRemove->set_tooltip_text(M("TP_SPOT_AI_OBJECT_TIP"));
        aiRemoveConn = btnAIRemove->signal_toggled().connect(
            sigc::bind(sigc::mem_fun(*this, &Spot::onMethodButtonToggled), btnAIRemove, 4));
    } else
#endif
    {
        btnAIRemove->set_sensitive(false);
        btnAIRemove->set_tooltip_text(M("TP_SPOT_AI_UNAVAILABLE"));
    }
    // Per-tool reset: appears once the tool has been applied.
    btnAIRemoveReset = Gtk::manage(new Gtk::Button("↺"));
    btnAIRemoveReset->set_relief(Gtk::RELIEF_NONE);
    btnAIRemoveReset->set_tooltip_text(M("TP_SPOT_AI_RESET_TIP"));
    btnAIRemoveReset->set_no_show_all(true);
    btnAIRemoveReset->signal_clicked().connect(sigc::bind(
        sigc::mem_fun(*this, &Spot::resetEntriesOfMethod), SpotMethod::AI_REMOVE));
    aiGrid->attach(*embedReset(btnAIRemove, btnAIRemoveReset), 0, 0, 1, 1);

    // Remove Reflections: a glare-reduction brush — no model needed.
    btnAIReflect = Gtk::manage(new Gtk::ToggleButton(M("TP_SPOT_AI_REFLECTION")));
    btnAIReflect->set_relief(Gtk::RELIEF_NONE);
    btnAIReflect->set_tooltip_text(M("TP_SPOT_AI_REFLECTION_TIP"));
    aiReflectConn = btnAIReflect->signal_toggled().connect(
        sigc::bind(sigc::mem_fun(*this, &Spot::onMethodButtonToggled), btnAIReflect, 6));
    btnAIReflectReset = Gtk::manage(new Gtk::Button("↺"));
    btnAIReflectReset->set_relief(Gtk::RELIEF_NONE);
    btnAIReflectReset->set_tooltip_text(M("TP_SPOT_AI_RESET_TIP"));
    btnAIReflectReset->set_no_show_all(true);
    btnAIReflectReset->signal_clicked().connect(sigc::bind(
        sigc::mem_fun(*this, &Spot::resetEntriesOfMethod), SpotMethod::AI_REFLECT));
    aiGrid->attach(*embedReset(btnAIReflect, btnAIReflectReset), 1, 0, 1, 1);
    // Remove Dust: one press scans the photo and heals every speck found.
    // No AI model involved — enabled once the coordinator provides a detector.
    btnAIDust = Gtk::manage(new Gtk::Button(M("TP_SPOT_AI_DUST")));
    btnAIDust->set_relief(Gtk::RELIEF_NONE);
    btnAIDust->set_sensitive(false);
    btnAIDust->set_tooltip_text(M("TP_SPOT_AI_DUST_TIP"));
    btnAIDust->signal_clicked().connect(sigc::mem_fun(*this, &Spot::onRemoveDustPressed));
    btnAIDustReset = Gtk::manage(new Gtk::Button("↺"));
    btnAIDustReset->set_relief(Gtk::RELIEF_NONE);
    btnAIDustReset->set_tooltip_text(M("TP_SPOT_AI_RESET_TIP"));
    btnAIDustReset->set_no_show_all(true);
    btnAIDustReset->signal_clicked().connect(sigc::bind(
        sigc::mem_fun(*this, &Spot::resetEntriesOfMethod), SpotMethod::AI_DUST));
    aiGrid->attach(*embedReset(btnAIDust, btnAIDustReset), 0, 1, 1, 1);

    // Generative Fill: same LaMa engine as Remove Object, tuned for large
    // areas — bigger context and native-resolution inference.
    btnAIFill = Gtk::manage(new Gtk::ToggleButton(M("TP_SPOT_AI_GENERATIVE")));
    btnAIFill->set_relief(Gtk::RELIEF_NONE);
#ifdef RT_AI_MASKING
    if (rtengine::getAIInpaintingEngine().isInitialized()) {
        btnAIFill->set_tooltip_text(M("TP_SPOT_AI_GENERATIVE_TIP"));
        aiFillConn = btnAIFill->signal_toggled().connect(
            sigc::bind(sigc::mem_fun(*this, &Spot::onMethodButtonToggled), btnAIFill, 7));
    } else
#endif
    {
        btnAIFill->set_sensitive(false);
        btnAIFill->set_tooltip_text(M("TP_SPOT_AI_UNAVAILABLE"));
    }
    btnAIFillReset = Gtk::manage(new Gtk::Button("↺"));
    btnAIFillReset->set_relief(Gtk::RELIEF_NONE);
    btnAIFillReset->set_tooltip_text(M("TP_SPOT_AI_RESET_TIP"));
    btnAIFillReset->set_no_show_all(true);
    btnAIFillReset->signal_clicked().connect(sigc::bind(
        sigc::mem_fun(*this, &Spot::resetEntriesOfMethod), SpotMethod::AI_FILL));
    aiGrid->attach(*embedReset(btnAIFill, btnAIFillReset), 1, 1, 1, 1);

    aiContent->pack_start(*aiGrid, false, false, 0);
    // Cancels the flat-mode tool content indent (see widgets.css) so the
    // section header lines up with the group headers around it.
    aiSection->get_style_context()->add_class("smart-tools");
    aiSection->setExpanded(false);
    aiSection->setOnToggled([this](bool expanded) {
        if (!expanded) {
            deselectSmartTools();
        }
    });

    // Layout: [method buttons] ... [reset] on one row
    labelBox = Gtk::manage (new Gtk::Box());
    labelBox->set_spacing (2);
    labelBox->pack_start (*methodBox, false, false, 0);
    labelBox->pack_end (*reset, false, false, 0);
    pack_start (*labelBox);

    // Size row: [size preview] [slider]
    Gtk::Box* sizeBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 4));
    sizeBox->pack_start(*sizePreview, false, false, 0);
    sizeBox->pack_start(*spotSize, true, true, 0);
    pack_start(*sizeBox);

    pack_start (*aiSection, Gtk::PACK_SHRINK);

    sourceIcon.datum = Geometry::IMAGE;
    sourceIcon.setActive (false);
    sourceIcon.state = Geometry::ACTIVE;
    sourceCircle.datum = Geometry::IMAGE;
    sourceCircle.setActive (false);
    sourceCircle.radiusInImageSpace = true;
    sourceCircle.setDashed(true);
    sourceMODisc.datum = Geometry::IMAGE;
    sourceMODisc.setActive (false);
    sourceMODisc.radiusInImageSpace = true;
    sourceMODisc.filled = true;
    sourceMODisc.innerLineWidth = 0.;
    targetCircle.datum = Geometry::IMAGE;
    targetCircle.setActive (false);
    targetCircle.radiusInImageSpace = true;
    targetMODisc.datum = Geometry::IMAGE;
    targetMODisc.setActive (false);
    targetMODisc.radiusInImageSpace = true;
    targetMODisc.filled = true;
    targetMODisc.innerLineWidth = 0.;
    sourceFeatherCircle.datum = Geometry::IMAGE;
    sourceFeatherCircle.setActive (false);
    sourceFeatherCircle.radiusInImageSpace = true;
    sourceFeatherCircle.setDashed(true);
    sourceFeatherCircle.innerLineWidth = 0.7;
    targetFeatherCircle.datum = Geometry::IMAGE;
    targetFeatherCircle.setActive (false);
    targetFeatherCircle.radiusInImageSpace = true;
    targetFeatherCircle.innerLineWidth = 0.7;
    link.datum = Geometry::IMAGE;
    link.setActive (false);

    // Phase 2: cursor preview circle
    cursorPreviewCircle.datum = Geometry::IMAGE;
    cursorPreviewCircle.radiusInImageSpace = true;
    cursorPreviewCircle.setDashed(true);
    // A hairline was invisible at fit zoom; the brush cursor must read clearly.
    cursorPreviewCircle.innerLineWidth = 1.5;
    cursorPreviewCircle.setInnerLineColor(1.0, 0.78, 0.25);
    cursorPreviewCircle.setActive(false);

    // Phase 4: stroke preview line — width matches brush diameter in image space
    strokePreviewLine.datum = Geometry::IMAGE;
    strokePreviewLine.setActive(false);
    strokePreviewLine.lineWidthInImageSpace = true;
    strokePreviewLine.innerLineWidth = float(SpotParams::minRadius * 2);
    // Painted stroke reads as a bright amber ribbon while dragging.
    strokePreviewLine.setInnerLineColor(1.0, 0.78, 0.25);

    auto m = ProcEventMapper::getInstance();
    EvSpotEnabled = m->newEvent(ALLNORAW, "HISTORY_MSG_SPOT");
    EvSpotEnabledOPA = m->newEvent(SPOTADJUST, "HISTORY_MSG_SPOT");
    EvSpotEntry = m->newEvent(SPOTADJUST, "HISTORY_MSG_SPOT_ENTRY");
    EvSpotEntryOPA = m->newEvent(SPOTADJUST, "HISTORY_MSG_SPOT_ENTRY");
    EvSpotMethod = m->newEvent(SPOTADJUST, "HISTORY_MSG_SPOT_METHOD");

    // Build the base geometry now — read() skips createGeometry when the
    // entry count is unchanged, so with zero spots the cursor preview and
    // stroke ribbon otherwise stay unregistered until the first entry lands
    // (the "brush only shows after you click once" bug).
    createGeometry();

    show_all();
}

Spot::~Spot()
{
    strokeLingerConn_.disconnect();
    busyAnimConn_.disconnect();
    busyTimeoutConn_.disconnect();

    // delete all dynamically allocated geometry
    if (EditSubscriber::visibleGeometry.size()) {
        for (size_t i = 0; i < EditSubscriber::visibleGeometry.size() - VISIBLE_OBJECT_COUNT; ++i) { // static visible geometry at the end of the list
            delete EditSubscriber::visibleGeometry.at (i);
        }
    }

    // We do not delete the mouseOverGeometry, because the referenced objects are either
    // shared with visibleGeometry or instantiated by the class's ctor
}

// Phase 1: Method button helpers

void Spot::queueCanvasRedraw()
{
    if (EditDataProvider* provider = getEditProvider()) {
        // Same EditDataProvider -> ImageArea narrowing ControlSpotPanel uses
        // to reach the preview widget.
        static_cast<ImageArea*>(provider)->queue_draw();
    }
}

void Spot::startBusyIndicator()
{
    if (busyAnimConn_.connected()) {
        return;
    }

    busyPhase_ = 0.f;
    cursorPreviewCircle.busyPhase = 0.f;
    busyAnimConn_ = Glib::signal_timeout().connect([this]() -> bool {
        busyPhase_ += 0.05f;
        if (busyPhase_ >= 1.f) {
            busyPhase_ -= 1.f;
        }
        cursorPreviewCircle.busyPhase = busyPhase_;
        queueCanvasRedraw();
        return true;
        // ~14fps: smooth enough for a rotating arc, and each tick repaints
        // the canvas while the inference is already saturating the CPU.
    }, 70);
}

void Spot::stopBusyIndicator()
{
    busyAnimConn_.disconnect();
    busyTimeoutConn_.disconnect();

    if (cursorPreviewCircle.busyPhase >= 0.f) {
        cursorPreviewCircle.busyPhase = -1.f;
        queueCanvasRedraw();
    }
}

void Spot::finishSmartResult()
{
    if (!awaitingSmartResult_) {
        return;
    }

    awaitingSmartResult_ = false;
    busySeenActive_ = false;
    stopBusyIndicator();

    strokeLingerConn_.disconnect();
    strokeLingerActive_ = false;
    strokePreviewLine.setActive(false);
    strokePreviewLine.points.clear();
    updateGeometry();
    queueCanvasRedraw();
}

void Spot::setProcessingActive(bool active)
{
    if (!awaitingSmartResult_) {
        return;
    }

    if (active) {
        busySeenActive_ = true;
        startBusyIndicator();
        return;
    }

    if (!busySeenActive_) {
        // Idle report from the render that was already running when the
        // stroke was committed — ours has not started yet.
        return;
    }

    finishSmartResult();
}

void Spot::deselectSmartTools()
{
    // Collapsing the section puts the brush away. Leaving a tool armed
    // behind a collapsed header left the canvas in brush mode with no
    // visible control explaining why.
    Gtk::ToggleButton* armed = nullptr;

    if (btnAIRemove && btnAIRemove->get_active()) {
        armed = btnAIRemove;
    } else if (btnAIReflect && btnAIReflect->get_active()) {
        armed = btnAIReflect;
    } else if (btnAIFill && btnAIFill->get_active()) {
        armed = btnAIFill;
    }

    if (armed) {
        // Untoggling runs onMethodButtonToggled's "already active" branch,
        // which leaves edit mode and drops the tweak operator.
        armed->set_active(false);
    }
}

bool Spot::isBrushMethod() const
{
    const SpotMethod m = static_cast<SpotMethod>(getActiveMethod());
    return m == SpotMethod::ERASE || m == SpotMethod::AI_REMOVE
        || m == SpotMethod::AI_REFLECT || m == SpotMethod::AI_FILL;
}

int Spot::getActiveMethod() const
{
    if (btnClone->get_active()) return 0;
    if (btnHeal->get_active()) return 1;
    if (btnErase->get_active()) return 2;
    if (btnRedEye->get_active()) return 3;
    if (btnAIRemove && btnAIRemove->get_active()) return 4;
    if (btnAIReflect && btnAIReflect->get_active()) return 6;
    if (btnAIFill && btnAIFill->get_active()) return 7;
    return 0;
}

void Spot::setActiveMethod(int index)
{
    blockMethodButtons(true);
    btnClone->set_active(index == 0);
    btnHeal->set_active(index == 1);
    btnErase->set_active(index == 2);
    btnRedEye->set_active(index == 3);
    if (btnAIRemove) {
        btnAIRemove->set_active(index == 4);
    }
    if (btnAIReflect) {
        btnAIReflect->set_active(index == 6);
    }
    if (btnAIFill) {
        btnAIFill->set_active(index == 7);
    }
    blockMethodButtons(false);
}

void Spot::blockMethodButtons(bool block)
{
    if (block) {
        cloneConn.block();
        healConn.block();
        eraseConn.block();
        redeyeConn.block();
        aiRemoveConn.block();
        aiReflectConn.block();
        aiFillConn.block();
    } else {
        cloneConn.unblock();
        healConn.unblock();
        eraseConn.unblock();
        redeyeConn.unblock();
        aiRemoveConn.unblock();
        aiReflectConn.unblock();
        aiFillConn.unblock();
    }
    blockMethodSignal = block;
}

void Spot::onMethodButtonToggled(Gtk::ToggleButton* button, int methodIndex)
{
    if (blockMethodSignal) return;

    if (!button->get_active()) {
        // User clicked the already-active button → toggle edit mode OFF
        if (listener && edit->get_active()) {
            editConn.block(true);
            edit->set_active(false);
            editConn.block(false);
            releaseEdit();
            unsubscribe();
            listener->unsetTweakOperator(this);
            listener->refreshPreview(EvSpotEnabled);
        }
        return;
    }

    // Deactivate all others
    blockMethodButtons(true);
    if (button != btnClone) btnClone->set_active(false);
    if (button != btnHeal) btnHeal->set_active(false);
    if (button != btnErase) btnErase->set_active(false);
    if (button != btnRedEye) btnRedEye->set_active(false);
    if (btnAIRemove && button != btnAIRemove) btnAIRemove->set_active(false);
    if (btnAIReflect && button != btnAIReflect) btnAIReflect->set_active(false);
    if (btnAIFill && button != btnAIFill) btnAIFill->set_active(false);
    blockMethodButtons(false);

    // Auto-activate edit mode
    if (listener && !edit->get_active()) {
        editConn.block(true);
        edit->set_active(true);
        editConn.block(false);

        // Auto-enable the tool if it's currently disabled
        if (!getEnabled()) {
            setEnabled(true);
            enabledChanged();
        }

        listener->setTweakOperator(this);
        listener->refreshPreview(EvSpotEnabledOPA);
        subscribe();
    }

    methodChanged();
}

// Phase 3: AdjusterListener
void Spot::adjusterChanged(Adjuster* a, double newval)
{
    if (a == spotSize) {
        sizePreview->setValue(int(newval));

        // Update cursor preview circle radius if active
        if (cursorPreviewCircle.isVisible()) {
            cursorPreviewCircle.radius = int(newval);
        }
    }
}

void Spot::read (const ProcParams* pp, const ParamsEdited* pedited)
{
    disableListener ();

    size_t oldSize = spots.size();
    spots = pp->spot.entries;

    if (pedited) {
        set_inconsistent (multiImage && !pedited->spot.enabled);
    }

    setEnabled (pp->spot.enabled);
    lastEnabled = pp->spot.enabled;
    activeSpot = -1;
    lastObject = -1;

    if (batchMode) {
        editedCheckBox->set_label(Glib::ustring::compose (M ("TP_SPOT_COUNTLABEL"), spots.size()));
    }
    else {
        if (spots.size() != oldSize) {
            createGeometry();
        }

        updateGeometry();
    }

    updateSmartToolIndicators ();

    enableListener ();
}

void Spot::write (ProcParams* pp, ParamsEdited* pedited)
{
    pp->spot.enabled = getEnabled();
    pp->spot.entries = spots;

    if (pedited) {
        pedited->spot.enabled = !get_inconsistent();
        pedited->spot.entries = editedCheckBox->get_active();
    }
}

void Spot::updateSmartToolIndicators()
{
    int counts[4] = {0, 0, 0, 0}; // remove, dust, reflect, fill
    for (const auto& entry : spots) {
        switch (entry.method) {
            case SpotMethod::AI_REMOVE:  ++counts[0]; break;
            case SpotMethod::AI_DUST:    ++counts[1]; break;
            case SpotMethod::AI_REFLECT: ++counts[2]; break;
            case SpotMethod::AI_FILL:    ++counts[3]; break;
            default: break;
        }
    }

    const auto apply = [](Gtk::Button* button, Gtk::Button* resetButton,
                          const Glib::ustring& labelKey, int count) {
        if (button) {
            button->set_label(count > 0 ? M(labelKey) + " ✓" : M(labelKey));
        }
        if (resetButton) {
            resetButton->set_visible(count > 0);
        }
    };

    apply(btnAIRemove, btnAIRemoveReset, "TP_SPOT_AI_OBJECT", counts[0]);
    apply(btnAIDust, btnAIDustReset, "TP_SPOT_AI_DUST", counts[1]);
    apply(btnAIReflect, btnAIReflectReset, "TP_SPOT_AI_REFLECTION", counts[2]);
    apply(btnAIFill, btnAIFillReset, "TP_SPOT_AI_GENERATIVE", counts[3]);
}

void Spot::resetEntriesOfMethod(SpotMethod method)
{
    bool removed = false;
    for (auto it = spots.begin(); it != spots.end();) {
        if (it->method == method) {
            it = spots.erase(it);
            removed = true;
        } else {
            ++it;
        }
    }

    if (removed) {
        EditSubscriber::action = EditSubscriber::Action::NONE;
        activeSpot = -1;
        lastObject = -1;

        if (!batchMode) {
            createGeometry();
            updateGeometry();
        }

        updateSmartToolIndicators();

        if (listener) {
            listener->panelChanged(edit->get_active() ? EvSpotEntryOPA : EvSpotEntry,
                                   Glib::ustring::compose(M("TP_SPOT_COUNTLABEL"), spots.size()));
        }
    }
}

void Spot::onRemoveDustPressed()
{
    if (!dustDetector_) {
        return;
    }

    const auto candidates = dustDetector_(40);

    int added = 0;
    for (const auto& candidate : candidates) {
        bool overlaps = false;
        for (const auto& existing : spots) {
            const int dx = existing.targetPos.x - candidate.targetPos.x;
            const int dy = existing.targetPos.y - candidate.targetPos.y;
            const int lim = existing.radius + candidate.radius;
            if (dx * dx + dy * dy < lim * lim) {
                overlaps = true;
                break;
            }
        }
        if (!overlaps) {
            spots.push_back(candidate);
            ++added;
        }
    }

    if (added == 0) {
        return;
    }

    if (!getEnabled()) {
        setEnabled(true);
        enabledChanged();
    }

    activeSpot = -1;
    lastObject = -1;

    if (!batchMode) {
        createGeometry();
        updateGeometry();
    }

    updateSmartToolIndicators();

    if (listener) {
        listener->panelChanged(edit->get_active() ? EvSpotEntryOPA : EvSpotEntry,
                               Glib::ustring::compose(M("TP_SPOT_COUNTLABEL"), spots.size()));
    }
}

void Spot::resetPressed()
{
    if (batchMode) {
        // no need to handle the Geometry in batch mode, since point editing is disabled
        spots.clear();
        editedConn.block (true);
        editedCheckBox->set_active (true);
        editedConn.block (false);

        editedCheckBox->set_label(Glib::ustring::compose (M ("TP_SPOT_COUNTLABEL"), spots.size()));

        if (listener) {
            listener->panelChanged (EvSpotEntry, Glib::ustring::compose (M ("TP_SPOT_COUNTLABEL"), 0));
        }
    } else {
        if (!spots.empty()) {
            EditSubscriber::action = EditSubscriber::Action::NONE;
            spots.clear();
            activeSpot = -1;
            lastObject = -1;
            createGeometry();
            updateGeometry();
            updateSmartToolIndicators();

            if (listener) {
                listener->panelChanged (edit->get_active() ? EvSpotEntryOPA : EvSpotEntry, Glib::ustring::compose (M ("TP_SPOT_COUNTLABEL"), 0));
            }
        }
    }
}

/**
 * Release anything that's currently being dragged.
 */
void Spot::releaseEdit()
{
    Geometry *loGeom = getVisibleGeometryFromMO (lastObject);

    EditSubscriber::action = EditSubscriber::Action::NONE;

    if (!loGeom) {
        return;
    }

    loGeom->state = Geometry::NORMAL;
    sourceIcon.state = Geometry::NORMAL;
    draggedSide = DraggedSide::NONE;

    // Reset stroke dragging state
    isStrokeDragging = false;
    strokeLingerConn_.disconnect();
    strokeLingerActive_ = false;
    strokePreviewLine.setActive(false);
    strokePreviewLine.points.clear();
    currentStrokePoints.clear();

    updateGeometry();
}

void Spot::setBatchMode (bool batchMode)
{
    ToolPanel::setBatchMode (batchMode);

    if (batchMode) {
        removeIfThere (labelBox, edit, false);

        if (!editedCheckBox) {
            removeIfThere (labelBox, countLabel, false);
            countLabel = nullptr;
            editedCheckBox = Gtk::manage (new Gtk::CheckButton (Glib::ustring::compose (M ("TP_SPOT_COUNTLABEL"), 0)));
            labelBox->pack_start (*editedCheckBox, Gtk::PACK_SHRINK, 2);
            labelBox->reorder_child (*editedCheckBox, 0);
            editedConn = editedCheckBox->signal_toggled().connect ( sigc::mem_fun (*this, &Spot::editedToggled) );
            editedCheckBox->show();
        }
    }
}

void Spot::editedToggled ()
{
    if (listener) {
        listener->panelChanged (EvSpotEntry, !editedCheckBox->get_active() ? M ("GENERAL_UNCHANGED") : Glib::ustring::compose (M ("TP_SPOT_COUNTLABEL"), spots.size()));
    }
}

void Spot::enabledChanged ()
{
    if (listener) {
        if (get_inconsistent()) {
            listener->panelChanged (edit->get_active() ? EvSpotEnabledOPA : EvSpotEnabled, M ("GENERAL_UNCHANGED"));
        } else if (getEnabled()) {
            listener->panelChanged (edit->get_active() ? EvSpotEnabledOPA : EvSpotEnabled, M ("GENERAL_ENABLED"));
        } else {
            listener->panelChanged (edit->get_active() ? EvSpotEnabledOPA : EvSpotEnabled, M ("GENERAL_DISABLED"));
        }
    }
}

void Spot::setEditProvider (EditDataProvider* provider)
{
    EditSubscriber::setEditProvider (provider);
}

void Spot::editToggled ()
{
    if (listener) {
        if (edit->get_active()) {
            listener->setTweakOperator(this);
            listener->refreshPreview(EvSpotEnabledOPA); // reprocess the preview w/o creating History entry
            subscribe();
        } else {
            releaseEdit();
            unsubscribe();
            listener->unsetTweakOperator(this);
            listener->refreshPreview(EvSpotEnabled); // reprocess the preview w/o creating History entry
        }
    }
}

void Spot::methodChanged()
{
    if (activeSpot > -1) {
        SpotMethod newMethod = static_cast<SpotMethod>(getActiveMethod());
        if (spots.at(activeSpot).method != newMethod) {
            spots.at(activeSpot).method = newMethod;

            // Everything except Clone/Heal is sourceless: source = target.
            if (newMethod != SpotMethod::CLONE && newMethod != SpotMethod::HEAL) {
                spots.at(activeSpot).sourcePos = spots.at(activeSpot).targetPos;
            }

            // The stroke paths (AI fill, glare) only process stroke entries;
            // give a converted circle spot a one-point stroke.
            if ((newMethod == SpotMethod::AI_REMOVE || newMethod == SpotMethod::AI_REFLECT
                    || newMethod == SpotMethod::AI_FILL)
                    && spots.at(activeSpot).strokePoints.empty()) {
                spots.at(activeSpot).strokePoints.push_back(spots.at(activeSpot).targetPos);
            }

            updateGeometry();

            if (listener) {
                listener->panelChanged(EvSpotEntryOPA, M("TP_SPOT_ENTRYCHANGED"));
            }
        }
    }
}

Geometry* Spot::getVisibleGeometryFromMO (int MOID)
{
    if (MOID == -1) {
        return nullptr;
    }

    if (MOID == MO_TARGET_DISK) {
        return getActiveSpotIcon();
    }

    if (MOID == MO_SOURCE_DISC) {
        return &sourceIcon;
    }

    if (MOID > MO_OBJECT_COUNT) {
        return EditSubscriber::visibleGeometry.at(MOID - MO_OBJECT_COUNT);
    }

    return EditSubscriber::mouseOverGeometry.at (MOID);
}

void Spot::createGeometry ()
{
    int nbrEntry = spots.size();

    if (!batchMode) {
        // countLabel removed from UI
    }

    // delete all dynamically allocated geometry
    if (EditSubscriber::visibleGeometry.size() > VISIBLE_OBJECT_COUNT)
        for (size_t i = 0; i < EditSubscriber::visibleGeometry.size() - VISIBLE_OBJECT_COUNT; ++i) { // static visible geometry at the end of the list
            delete EditSubscriber::visibleGeometry.at (i);
        }

    // mouse over geometry starts with the static geometry, then the spot's icon geometry
    EditSubscriber::mouseOverGeometry.resize (MO_OBJECT_COUNT + nbrEntry);
    // visible geometry starts with the spot's icon geometry, then the static geometry
    EditSubscriber::visibleGeometry.resize (nbrEntry + VISIBLE_OBJECT_COUNT);

    size_t i = 0, j = 0;
    assert(i == MO_TARGET_DISK);
    EditSubscriber::mouseOverGeometry.at (i++) = &targetMODisc;        // MO_OBJECT_COUNT + 0
    assert(i == MO_SOURCE_DISC);
    EditSubscriber::mouseOverGeometry.at (i++) = &sourceMODisc;        // MO_OBJECT_COUNT + 1
    assert(i == MO_TARGET_CIRCLE);
    EditSubscriber::mouseOverGeometry.at (i++) = &targetCircle;        // MO_OBJECT_COUNT + 2
    assert(i == MO_SOURCE_CIRCLE);
    EditSubscriber::mouseOverGeometry.at (i++) = &sourceCircle;        // MO_OBJECT_COUNT + 3
    assert(i == MO_TARGET_FEATHER_CIRCLE);
    EditSubscriber::mouseOverGeometry.at (i++) = &targetFeatherCircle; // MO_OBJECT_COUNT + 4
    assert(i == MO_SOURCE_FEATHER_CIRCLE);
    EditSubscriber::mouseOverGeometry.at (i++) = &sourceFeatherCircle; // MO_OBJECT_COUNT + 5

    // recreate all spots geometry
    std::shared_ptr<RTSurface> normalImg   = sourceIcon.getNormalImg();
    std::shared_ptr<RTSurface> prelightImg = sourceIcon.getPrelightImg();
    std::shared_ptr<RTSurface> activeImg   = sourceIcon.getActiveImg();

    for (; j < EditSubscriber::visibleGeometry.size() - VISIBLE_OBJECT_COUNT; ++i, ++j) {
        EditSubscriber::mouseOverGeometry.at (i) = EditSubscriber::visibleGeometry.at (j) = new OPIcon (normalImg, activeImg, prelightImg, nullptr, nullptr, Geometry::DP_CENTERCENTER);
        EditSubscriber::visibleGeometry.at (j)->setActive (true);
        EditSubscriber::visibleGeometry.at (j)->datum = Geometry::IMAGE;
        EditSubscriber::visibleGeometry.at (j)->state = Geometry::NORMAL;
    }

    int visibleOffset = j;
    assert(j - visibleOffset == VISIBLE_SOURCE_ICON);
    EditSubscriber::visibleGeometry.at (j++) = &sourceIcon;          // VISIBLE_OBJECT_COUNT + 0
    assert(j - visibleOffset == VISIBLE_SOURCE_FEATHER_CIRCLE);
    EditSubscriber::visibleGeometry.at (j++) = &sourceFeatherCircle; // VISIBLE_OBJECT_COUNT + 1
    assert(j - visibleOffset == VISIBLE_LINK);
    EditSubscriber::visibleGeometry.at (j++) = &link;                // VISIBLE_OBJECT_COUNT + 2
    assert(j - visibleOffset == VISIBLE_SOURCE_CIRCLE);
    EditSubscriber::visibleGeometry.at (j++) = &sourceCircle;        // VISIBLE_OBJECT_COUNT + 3
    assert(j - visibleOffset == VISIBLE_TARGET_FEATHER_CIRCLE);
    EditSubscriber::visibleGeometry.at (j++) = &targetFeatherCircle; // VISIBLE_OBJECT_COUNT + 4
    assert(j - visibleOffset == VISIBLE_TARGET_CIRCLE);
    EditSubscriber::visibleGeometry.at (j++) = &targetCircle;        // VISIBLE_OBJECT_COUNT + 5
    assert(j - visibleOffset == VISIBLE_CURSOR_PREVIEW);
    EditSubscriber::visibleGeometry.at (j++) = &cursorPreviewCircle; // VISIBLE_OBJECT_COUNT + 6
    assert(j - visibleOffset == VISIBLE_STROKE_PREVIEW);
    EditSubscriber::visibleGeometry.at (j++) = &strokePreviewLine;   // VISIBLE_OBJECT_COUNT + 7
    static_cast<void>(visibleOffset);
}

void Spot::updateGeometry()
{
    EditDataProvider* dataProvider = getEditProvider();

    if (dataProvider) {
        int imW, imH;
        dataProvider->getImageSize (imW, imH);

        if (activeSpot > -1) {
            bool hideSource = (spots.at(activeSpot).method != SpotMethod::CLONE
                               && spots.at(activeSpot).method != SpotMethod::HEAL);

            // Target point circle
            targetCircle.center = spots.at (activeSpot).targetPos;
            targetCircle.radius = spots.at (activeSpot).radius;
            targetCircle.setActive (true);

            // Target point Mouse Over disc
            targetMODisc.center = targetCircle.center;
            targetMODisc.radius = targetCircle.radius;
            targetMODisc.setActive (true);

            // Source point Icon
            sourceIcon.position = spots.at (activeSpot).sourcePos;
            sourceIcon.setActive (!hideSource);

            // Source point circle
            sourceCircle.center = spots.at (activeSpot).sourcePos;
            sourceCircle.radius = spots.at (activeSpot).radius;
            sourceCircle.setActive (!hideSource);

            // Source point Mouse Over disc
            sourceMODisc.center = sourceCircle.center;
            sourceMODisc.radius = sourceCircle.radius;
            sourceMODisc.setActive (!hideSource);

            // Target point feather circle
            targetFeatherCircle.center = spots.at (activeSpot).targetPos;
            targetFeatherCircle.radius = float (spots.at (activeSpot).radius) * (1.f + spots.at (activeSpot).feather);
            targetFeatherCircle.radiusInImageSpace = true;
            targetFeatherCircle.setActive (true);

            // Source point feather circle
            sourceFeatherCircle.center = spots.at (activeSpot).sourcePos;
            sourceFeatherCircle.radius = targetFeatherCircle.radius;
            sourceFeatherCircle.setActive (!hideSource);

            // Link line
            if (!hideSource) {
                PolarCoord p;
                p = targetCircle.center - sourceCircle.center;

                if (p.radius > sourceCircle.radius + targetCircle.radius) {
                    PolarCoord p2 (sourceCircle.radius, p.angle);
                    Coord p3;
                    p3 = p2;
                    link.begin = sourceCircle.center + p3;
                    p2.set (targetCircle.radius, p.angle + 180);
                    p3 = p2;
                    link.end = targetCircle.center + p3;
                    link.setActive (draggedSide == DraggedSide::NONE);
                } else {
                    link.setActive (false);
                }
            } else {
                link.setActive (false);
            }

            sourceCircle.setVisible(!hideSource && draggedSide != DraggedSide::SOURCE);
            targetCircle.setVisible(draggedSide != DraggedSide::TARGET);

            // Phase 4: show stroke preview for active stroke spot
            if (spots.at(activeSpot).isStroke()) {
                strokePreviewLine.points.clear();
                for (const auto& pt : spots.at(activeSpot).strokePoints) {
                    strokePreviewLine.points.push_back(pt);
                }
                strokePreviewLine.innerLineWidth = float(spots.at(activeSpot).radius * 2);
                strokePreviewLine.setActive(true);
            } else if (!isStrokeDragging && !strokeLingerActive_) {
                strokePreviewLine.setActive(false);
            }

            // Hide cursor preview when a spot is active — but never in
            // brush mode, where it is standing in for the hidden pointer.
            if (!isBrushMethod()) {
                cursorPreviewCircle.setActive(false);
            }
        } else {
            targetCircle.state = Geometry::NORMAL;
            sourceCircle.state = Geometry::NORMAL;
            targetFeatherCircle.state = Geometry::NORMAL;
            sourceFeatherCircle.state = Geometry::NORMAL;

            targetCircle.setActive (false);
            targetMODisc.setActive (false);
            sourceIcon.setActive (false);
            sourceCircle.setActive (false);
            sourceMODisc.setActive (false);
            targetFeatherCircle.setActive (false);
            sourceFeatherCircle.setActive (false);
            link.setActive (false);

            if (!isStrokeDragging && !strokeLingerActive_) {
                strokePreviewLine.setActive(false);
            }
        }

        for (size_t i = 0; i < spots.size(); ++i) {
            // Target point icon
            OPIcon* geom = static_cast<OPIcon*> (EditSubscriber::visibleGeometry.at (i));
            geom->position = spots.at (i).targetPos;
            geom->setActive (true);

            if (int (i) == activeSpot) {
                geom->setHoverable (false);
            }
        }
    }
}

OPIcon *Spot::getActiveSpotIcon()
{
    if (activeSpot > -1) {
        return static_cast<OPIcon*> (EditSubscriber::visibleGeometry.at (activeSpot));
    }

    return nullptr;
}

void Spot::addNewEntry()
{
    EditDataProvider* editProvider = getEditProvider();
    // we create a new entry
    SpotEntry se;
    se.radius = spotSize->getIntValue();
    se.targetPos = editProvider->posImage;
    se.sourcePos = se.targetPos;
    se.method = static_cast<SpotMethod>(getActiveMethod());
    spots.push_back (se); // this make a copy of se ...
    activeSpot = spots.size() - 1;
    lastObject = MO_SOURCE_DISC;

    createGeometry();
    updateGeometry();
    EditSubscriber::visibleGeometry.at (activeSpot)->state = Geometry::ACTIVE;
    sourceIcon.state = Geometry::DRAGGED;

    if (listener) {
        listener->panelChanged (EvSpotEntryOPA, M ("TP_SPOT_ENTRYCHANGED"));
    }
}

void Spot::deleteSelectedEntry()
{
    // delete the activeSpot
    if (activeSpot > -1) {
        std::vector<rtengine::procparams::SpotEntry>::iterator i = spots.begin();

        for (int j = 0; j < activeSpot; ++j) {
            ++i;
        }

        spots.erase (i);
    }

    lastObject = -1;
    activeSpot = -1;

    createGeometry();
    updateGeometry();
    updateSmartToolIndicators();

    if (listener) {
        listener->panelChanged (EvSpotEntry, M ("TP_SPOT_ENTRYCHANGED"));
    }
}

CursorShape Spot::getCursor (int objectID, int xPos, int yPos) const
{
    // Brush methods own the pointer: the amber ring IS the cursor, so the
    // OS one stays hidden and the spot-handle resize arrows never appear.
    // Painting across an older stroke's handles used to flash those arrows
    // and blink the ring out, which read as a second, jumpy cursor.
    if (isBrushMethod()) {
        return CSEmpty;
    }

    const EditDataProvider* editProvider = getEditProvider();
    if (editProvider && activeSpot > -1) {
        if (draggedSide != DraggedSide::NONE) {
            return CSEmpty;
        }

        if (objectID == MO_TARGET_DISK || objectID == MO_SOURCE_DISC) {
            return CSMove2D;
        }
        if (objectID >= MO_TARGET_CIRCLE && objectID <= MO_SOURCE_FEATHER_CIRCLE) {
            Coord delta(Coord(xPos, yPos) - ((objectID == MO_SOURCE_CIRCLE || objectID == MO_SOURCE_FEATHER_CIRCLE) ? spots.at(activeSpot).sourcePos : spots.at(activeSpot).targetPos));
            PolarCoord polarPos(delta);
            if (polarPos.angle < 0.) {
                polarPos.angle += 180.;
            }
            if (polarPos.angle < 22.5 || polarPos.angle >= 157.5) {
                return CSMove1DH;
            }
            if (polarPos.angle < 67.5) {
                return CSResizeBottomRight;
            }
            if (polarPos.angle < 112.5) {
                return CSMove1DV;
            }
            return CSResizeBottomLeft;
        }
    }

    // While the brush circle marks the position (or a stroke is being
    // painted), the OS pointer on top of it is just clutter — hide it.
    // const_cast: Geometry::isVisible() is a non-const getter.
    if (isStrokeDragging || const_cast<Spot*>(this)->cursorPreviewCircle.isVisible()) {
        return CSEmpty;
    }
    return CSCrosshair;
}

bool Spot::mouseOver (int modifierKey)
{
    EditDataProvider* editProvider = getEditProvider();

    if (editProvider && editProvider->getObject() != lastObject) {
        if (lastObject > -1) {
            if (EditSubscriber::mouseOverGeometry.at (lastObject) == &targetMODisc) {
                getVisibleGeometryFromMO (lastObject)->state = Geometry::ACTIVE;
            } else {
                getVisibleGeometryFromMO (lastObject)->state = Geometry::NORMAL;
            }

            sourceIcon.state = Geometry::ACTIVE;
        }

        if (editProvider->getObject() > -1) {
            getVisibleGeometryFromMO (editProvider->getObject())->state = Geometry::PRELIGHT;

            if (editProvider->getObject() >= MO_OBJECT_COUNT) {
                // a Spot is being edited
                int oldActiveSpot = activeSpot;
                activeSpot = editProvider->getObject() - MO_OBJECT_COUNT;

                if (activeSpot != oldActiveSpot) {
                    if (oldActiveSpot > -1) {
                        EditSubscriber::visibleGeometry.at (oldActiveSpot)->state = Geometry::NORMAL;
                        EditSubscriber::mouseOverGeometry.at (oldActiveSpot + MO_OBJECT_COUNT)->state = Geometry::NORMAL;
                    }

                    EditSubscriber::visibleGeometry.at (activeSpot)->state = Geometry::PRELIGHT;
                    EditSubscriber::mouseOverGeometry.at (activeSpot + MO_OBJECT_COUNT)->state = Geometry::PRELIGHT;

                    // Sync method buttons to reflect this spot's method
                    setActiveMethod(static_cast<int>(spots.at(activeSpot).method));
                }
            }
        }

        lastObject = editProvider->getObject();

        if (lastObject > -1 && EditSubscriber::mouseOverGeometry.at (lastObject) == getActiveSpotIcon()) {
            lastObject = MO_TARGET_DISK;
        }

        // Phase 2: Show/hide cursor preview circle. In brush mode it stands
        // in for the hidden OS pointer, so it must keep following the mouse
        // over existing spots' handles and mid-stroke too.
        if (isBrushMethod() || (lastObject == -1 && activeSpot == -1 && !isStrokeDragging)) {
            cursorPreviewCircle.center = editProvider->posImage;
            cursorPreviewCircle.radius = spotSize->getIntValue();
            cursorPreviewCircle.setActive(true);
        } else {
            cursorPreviewCircle.setActive(false);
        }

        updateGeometry();
        return true;
    }

    // Phase 2: Update cursor preview position even when object hasn't changed,
    // but only if the position actually moved (avoid redraw loop)
    if (editProvider && (isBrushMethod() || (lastObject == -1 && activeSpot == -1 && !isStrokeDragging))) {
        rtengine::Coord newPos = editProvider->posImage;
        if (newPos != cursorPreviewCircle.center) {
            cursorPreviewCircle.center = newPos;
            cursorPreviewCircle.radius = spotSize->getIntValue();
            cursorPreviewCircle.setActive(true);
            return true;  // Request redraw, but don't call updateGeometry (no state change)
        }
    }

    return false;
}

// Create a new Target and Source point or start the drag of a Target point under the cursor
bool Spot::button1Pressed (int modifierKey)
{
    EditDataProvider* editProvider = getEditProvider();

    if (editProvider) {
        // Hide cursor preview during interaction — except in brush mode,
        // where it is the pointer and must not blink out on mouse-down.
        if (!isBrushMethod()) {
            cursorPreviewCircle.setActive(false);
        }

        // Interact with existing spot (drag target or source)
        if (lastObject > -1) {
            draggedSide = lastObject == MO_TARGET_DISK ? DraggedSide::TARGET : lastObject == MO_SOURCE_DISC ? DraggedSide::SOURCE : DraggedSide::NONE;
            getVisibleGeometryFromMO (lastObject)->state = Geometry::DRAGGED;
            EditSubscriber::action = EditSubscriber::Action::DRAGGING;
            return true;
        }

        // Clicking empty area — check bounds
        int imW, imH;
        const auto startPos = editProvider->posImage;
        editProvider->getImageSize(imW, imH);
        if (startPos.x < 0 || startPos.y < 0 || startPos.x > imW || startPos.y > imH) {
            return false; // Outside of image area.
        }

        SpotMethod currentMethod = static_cast<SpotMethod>(getActiveMethod());

        if (currentMethod == SpotMethod::ERASE || currentMethod == SpotMethod::AI_REMOVE
                || currentMethod == SpotMethod::AI_REFLECT || currentMethod == SpotMethod::AI_FILL) {
            // Brush methods: start stroke dragging — drag for freeform, single click for a dab
            strokeLingerConn_.disconnect();
            strokeLingerActive_ = false;
            isStrokeDragging = true;
            currentStrokePoints.clear();
            currentStrokePoints.push_back(startPos);
            strokePreviewLine.points.clear();
            strokePreviewLine.points.push_back(startPos);
            strokePreviewLine.innerLineWidth = float(spotSize->getIntValue() * 2);
            strokePreviewLine.setActive(true);
            EditSubscriber::action = EditSubscriber::Action::DRAGGING;
            return true;
        }

        if (currentMethod == SpotMethod::REDEYE) {
            // RedEye: click to place, no source needed
            draggedSide = DraggedSide::NONE;
            addNewEntry();
            activeSpot = -1;
            lastObject = -1;
            updateGeometry();
            EditSubscriber::action = EditSubscriber::Action::PICKING;
            return true;
        }

        if (currentMethod == SpotMethod::CLONE || currentMethod == SpotMethod::HEAL) {
            // Clone/Heal: click to place target, drag to set source
            draggedSide = DraggedSide::SOURCE;
            addNewEntry();
            EditSubscriber::action = EditSubscriber::Action::DRAGGING;
            return true;
        }
    }

    return false;
}

// End the drag of a Target point
bool Spot::button1Released()
{
    // Phase 4: Handle stroke release
    if (isStrokeDragging) {
        isStrokeDragging = false;

        const int activeMethod = getActiveMethod();
        const SpotMethod strokeMethod =
            activeMethod == 4 ? SpotMethod::AI_REMOVE
            : activeMethod == 6 ? SpotMethod::AI_REFLECT
            : activeMethod == 7 ? SpotMethod::AI_FILL
            : SpotMethod::ERASE;

        // A drag and a single-click dab commit through one path. They used
        // to differ: the dab created its entry silently, with no overlay
        // and no spinner, which read as "the tool did not fire".
        if (!currentStrokePoints.empty()) {
            SpotEntry se;
            se.radius = spotSize->getIntValue();
            se.method = strokeMethod;

            // Compute bounding box center as targetPos
            int minX = INT_MAX, minY = INT_MAX, maxX = INT_MIN, maxY = INT_MIN;
            for (const auto& pt : currentStrokePoints) {
                minX = std::min(minX, pt.x);
                minY = std::min(minY, pt.y);
                maxX = std::max(maxX, pt.x);
                maxY = std::max(maxY, pt.y);
            }
            se.targetPos.set((minX + maxX) / 2, (minY + maxY) / 2);
            se.sourcePos = se.targetPos;

            // Erase keeps its classic circle-spot behaviour for a lone
            // click; the smart tools need the point list to reach their
            // stroke path at any length.
            if (currentStrokePoints.size() >= 2 || strokeMethod != SpotMethod::ERASE) {
                se.strokePoints = currentStrokePoints;
            }

            spots.push_back(se);
            activeSpot = -1;
            lastObject = -1;

            // Keep the translucent area overlay on screen while the repair
            // renders — the fill is see-through, so it marks where the
            // result lands without hiding it.
            strokePreviewLine.setActive(true);
            strokeLingerActive_ = true;
            strokeLingerConn_.disconnect();

            if (strokeMethod != SpotMethod::ERASE) {
                // A smart tool can take many seconds. Hold the overlay and
                // spin the cursor until the result actually lands (see
                // setProcessingActive) rather than for a guessed interval;
                // the long backstop only covers a missed completion signal.
                awaitingSmartResult_ = true;
                busySeenActive_ = false;
                startBusyIndicator();
                busyTimeoutConn_.disconnect();
                busyTimeoutConn_ = Glib::signal_timeout().connect([this]() -> bool {
                    finishSmartResult();
                    return false;
                }, 120000);
            } else {
                strokeLingerConn_ = Glib::signal_timeout().connect([this]() -> bool {
                    strokeLingerActive_ = false;
                    strokePreviewLine.setActive(false);
                    strokePreviewLine.points.clear();
                    return false;
                }, 4000);
            }

            createGeometry();
            updateGeometry();

            if (listener) {
                listener->panelChanged(edit->get_active() ? EvSpotEntryOPA : EvSpotEntry, M("TP_SPOT_ENTRYCHANGED"));
            }
        }

        currentStrokePoints.clear();
        if (!strokeLingerActive_) {
            strokePreviewLine.setActive(false);
            strokePreviewLine.points.clear();
        }
        EditSubscriber::action = EditSubscriber::Action::NONE;
        updateSmartToolIndicators();
        return true;
    }

    Geometry *loGeom = getVisibleGeometryFromMO (lastObject);

    if (!loGeom) {
        EditSubscriber::action = EditSubscriber::Action::NONE;
        return false;
    }

    loGeom->state = Geometry::PRELIGHT;
    EditSubscriber::action = EditSubscriber::Action::NONE;
    draggedSide = DraggedSide::NONE;
    updateGeometry();
    return true;
}

// Delete a point
bool Spot::button2Pressed (int modifierKey)
{
    EditDataProvider* editProvider = getEditProvider();

    if (!editProvider || lastObject == -1 || activeSpot == -1) {
        return false;
    }

    if (! (modifierKey & (GDK_SHIFT_MASK | GDK_SHIFT_MASK))) {
        EditSubscriber::action = EditSubscriber::Action::PICKING;
    }

    return false;
}

// Create a new Target and Source point or start the drag of a Target point under the cursor
bool Spot::button3Pressed (int modifierKey)
{
    EditDataProvider* editProvider = getEditProvider();

    if (!editProvider || lastObject == -1 || activeSpot == -1) {
        return false;
    }

    if ((modifierKey & GDK_CONTROL_MASK) && (EditSubscriber::mouseOverGeometry.at (lastObject) == &targetMODisc || lastObject >= MO_OBJECT_COUNT)) {
        lastObject = MO_SOURCE_DISC;
        sourceIcon.state = Geometry::DRAGGED;
        EditSubscriber::action = EditSubscriber::Action::DRAGGING;
        draggedSide = DraggedSide::SOURCE;
        return true;
    } else if (! (modifierKey & (GDK_SHIFT_MASK | GDK_SHIFT_MASK))) {
        EditSubscriber::action = EditSubscriber::Action::PICKING;
        return true;
    }

    return false;
}

bool Spot::button3Released()
{
    Geometry *loGeom = getVisibleGeometryFromMO (lastObject);

    if (!loGeom) {
        EditSubscriber::action = EditSubscriber::Action::NONE;
        return false;
    }

    lastObject = -1;
    sourceIcon.state = Geometry::ACTIVE;
    draggedSide = DraggedSide::NONE;
    updateGeometry();
    EditSubscriber::action = EditSubscriber::Action::NONE;
    return true;
}

bool Spot::drag1 (int modifierKey)
{
    // Phase 4: Handle stroke dragging
    if (isStrokeDragging) {
        EditDataProvider* editProvider = getEditProvider();
        if (!editProvider) return false;

        // posImage is the initial click position; deltaImage gives the total offset during drag
        rtengine::Coord pos = editProvider->posImage + editProvider->deltaImage;

        // Throttle: only add point if far enough from last
        if (!currentStrokePoints.empty()) {
            const auto& last = currentStrokePoints.back();
            int dx = pos.x - last.x;
            int dy = pos.y - last.y;
            int minSpacing = std::max(spotSize->getIntValue() / 4, 2);
            if (dx * dx + dy * dy < minSpacing * minSpacing) {
                return true; // Too close, skip
            }
        }

        currentStrokePoints.push_back(pos);
        strokePreviewLine.points.push_back(pos);
        // Keep the ring under the pointer while painting (the OS cursor is
        // hidden, so this is the only thing marking where the brush is).
        cursorPreviewCircle.center = pos;
        cursorPreviewCircle.radius = spotSize->getIntValue();
        cursorPreviewCircle.setActive(true);
        updateGeometry();
        return true;
    }

    if (EditSubscriber::action != EditSubscriber::Action::DRAGGING) {
        return false;
    }

    EditDataProvider *editProvider = getEditProvider();
    int imW, imH;
    editProvider->getImageSize (imW, imH);
    bool modified = false;

    Geometry *loGeom = EditSubscriber::mouseOverGeometry.at (lastObject);

    if (loGeom == &sourceMODisc) {
        rtengine::Coord currPos = spots.at (activeSpot).sourcePos;
        spots.at (activeSpot).sourcePos += editProvider->deltaPrevImage;
        spots.at (activeSpot).sourcePos.clip (imW, imH);

        if (spots.at (activeSpot).sourcePos != currPos) {
            modified = true;
        }

        EditSubscriber::mouseOverGeometry.at (activeSpot + MO_OBJECT_COUNT)->state = Geometry::DRAGGED;
    } else if (loGeom == &targetMODisc || lastObject >= MO_OBJECT_COUNT) {
        rtengine::Coord currPos = spots.at (activeSpot).targetPos;
        spots.at (activeSpot).targetPos += editProvider->deltaPrevImage;
        spots.at (activeSpot).targetPos.clip (imW, imH);

        if (spots.at (activeSpot).targetPos != currPos) {
            modified = true;
        }
    } else if (loGeom == &sourceCircle) {
        int lastRadius = spots.at (activeSpot).radius;
        rtengine::Coord currPos = editProvider->posImage + editProvider->deltaImage;
        rtengine::PolarCoord currPolar (currPos - spots.at (activeSpot).sourcePos);
        spots.at (activeSpot).radius = LIM<int> (int (currPolar.radius), SpotParams::minRadius, SpotParams::maxRadius);

        if (spots.at (activeSpot).radius != lastRadius) {
            modified = true;
        }
    } else if (loGeom == &targetCircle) {
        int lastRadius = spots.at (activeSpot).radius;
        rtengine::Coord currPos = editProvider->posImage + editProvider->deltaImage;
        rtengine::PolarCoord currPolar (currPos - spots.at (activeSpot).targetPos);
        spots.at (activeSpot).radius = LIM<int> (int (currPolar.radius), SpotParams::minRadius, SpotParams::maxRadius);

        if (spots.at (activeSpot).radius != lastRadius) {
            modified = true;
        }
    } else if (loGeom == &sourceFeatherCircle) {
        float currFeather = spots.at (activeSpot).feather;
        rtengine::Coord currPos = editProvider->posImage + editProvider->deltaImage;
        rtengine::PolarCoord currPolar (currPos - spots.at (activeSpot).sourcePos);
        spots.at (activeSpot).feather = LIM01<float> ((currPolar.radius - double (spots.at (activeSpot).radius)) / double (spots.at (activeSpot).radius));

        if (spots.at (activeSpot).feather != currFeather) {
            modified = true;
        }
    } else if (loGeom == &targetFeatherCircle) {
        float currFeather = spots.at (activeSpot).feather;
        rtengine::Coord currPos = editProvider->posImage + editProvider->deltaImage;
        rtengine::PolarCoord currPolar (currPos - spots.at (activeSpot).targetPos);
        spots.at (activeSpot).feather = LIM01<float> ((currPolar.radius - double (spots.at (activeSpot).radius)) / double (spots.at (activeSpot).radius));

        if (spots.at (activeSpot).feather != currFeather) {
            modified = true;
        }
    }

    if (listener && modified) {
        updateGeometry();
        listener->panelChanged (EvSpotEntry, M ("TP_SPOT_ENTRYCHANGED"));
    }

    return modified;
}

bool Spot::drag3 (int modifierKey)
{
    if (EditSubscriber::action != EditSubscriber::Action::DRAGGING) {
        return false;
    }

    EditDataProvider *editProvider = getEditProvider();
    int imW, imH;
    editProvider->getImageSize (imW, imH);
    bool modified = false;

    Geometry *loGeom = EditSubscriber::mouseOverGeometry.at (lastObject);

    if (loGeom == &sourceMODisc) {
        rtengine::Coord currPos = spots.at (activeSpot).sourcePos;
        spots.at (activeSpot).sourcePos += editProvider->deltaPrevImage;
        spots.at (activeSpot).sourcePos.clip (imW, imH);

        if (spots.at (activeSpot).sourcePos != currPos) {
            modified = true;
        }
    }

    if (listener) {
        updateGeometry();
        listener->panelChanged (EvSpotEntry, M ("TP_SPOT_ENTRYCHANGED"));
    }

    return modified;
}

bool Spot::pick2 (bool picked)
{
    return pick3 (picked);
}

bool Spot::pick3 (bool picked)
{
    EditDataProvider* editProvider = getEditProvider();

    if (!picked) {
        if (editProvider->getObject() != lastObject) {
            return false;
        }
    }

    // Object is picked, we delete it
    deleteSelectedEntry();
    EditSubscriber::action = EditSubscriber::Action::NONE;
    updateGeometry();
    return true;
}


void Spot::switchOffEditMode ()
{
    if (edit->get_active()) {
        // switching off the hidden edit toggle
        bool wasBlocked = editConn.block (true);
        edit->set_active (false);

        if (!wasBlocked) {
            editConn.block (false);
        }
    }

    // Visually deactivate all method buttons
    blockMethodButtons(true);
    btnClone->set_active(false);
    btnHeal->set_active(false);
    btnErase->set_active(false);
    btnRedEye->set_active(false);
    blockMethodButtons(false);

    EditSubscriber::switchOffEditMode();  // disconnect
    listener->unsetTweakOperator(this);
    listener->refreshPreview(EvSpotEnabled); // reprocess the preview w/o creating History entry
}


void Spot::tweakParams(procparams::ProcParams& pparams)
{
    // Only pixel-MOVING tools are disabled while editing: spots live in
    // pre-transform image coordinates, so the preview must show the
    // untransformed, uncropped frame for clicks to land where the repair
    // lands. Appearance-only tools (film sim, dehaze, sharpening, local
    // adjustments, ...) stay enabled — the old blanket disables made
    // entering the brush visibly strip the user's edits while painting.
    pparams.lensProf = LensProfParams();
    pparams.cacorrection = CACorrParams();
    pparams.distortion = DistortionParams();
    pparams.rotate = RotateParams();
    pparams.perspective = PerspectiveParams();
    pparams.crop.enabled = false;
    pparams.toneCurve.histmatching = false;
}
