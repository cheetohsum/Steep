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
 *  along with RawTherapee.  If not, see <http://www.gnu.org/licenses/>.
 *  2018 Pierre Cabrera <pierre.cab@gmail.com>
 */

#include "rtengine/rt_math.h"
#include "controlspotpanel.h"
#include "editwidgets.h"
#include "options.h"
#include "rtengine/procparams.h"
#include "rtimage.h"
#include "eventmapper.h"
#include "imagearea.h"
#include "cropwindow.h"
#ifdef RT_AI_MASKING
#include "rtengine/aimaskcache.h"
#endif

#include <cmath>

using namespace rtengine;
using namespace procparams;

//-----------------------------------------------------------------------------
// ControlSpotPanel
//-----------------------------------------------------------------------------

ControlSpotPanel::ControlSpotPanel():
    EditSubscriber(ET_OBJECTS),
    FoldableToolPanel(this, "controlspotpanel", M("TP_LOCALLAB_SETTINGS")),

    scrolledwindow_(Gtk::manage(new Gtk::ScrolledWindow())),
    treeview_(Gtk::manage(new Gtk::TreeView())),

    button_add_(Gtk::manage(new Gtk::Button(M("TP_LOCALLAB_BUTTON_ADD")))),
    button_delete_(Gtk::manage(new Gtk::Button(M("TP_LOCALLAB_BUTTON_DEL")))),
    button_duplicate_(Gtk::manage(new Gtk::Button(M("TP_LOCALLAB_BUTTON_DUPL")))),

    button_rename_(Gtk::manage(new Gtk::Button(M("TP_LOCALLAB_BUTTON_REN")))),
    button_visibility_(Gtk::manage(new Gtk::Button(M("TP_LOCALLAB_BUTTON_VIS")))),

    prevMethod_(Gtk::manage(new MyComboBoxText())),
    shape_(Gtk::manage(new PopUpButton())),
    spotMethod_(Gtk::manage(new MyComboBoxText())),
    shapeMethod_(Gtk::manage(new MyComboBoxText())),
    qualityMethod_(Gtk::manage(new MyComboBoxText())),
    //complexMethod_(Gtk::manage(new MyComboBoxText())),
    wavMethod_(Gtk::manage(new MyComboBoxText())),
    avoidgamutMethod_(Gtk::manage(new MyComboBoxText())),
    maskType_(Gtk::manage(new PopUpButton())),
    aiMaskClass_(Gtk::manage(new PopUpButton())),
    aiMaskTolerance_(Gtk::manage(new Adjuster(M("TP_LOCALLAB_AIMASK_TOLERANCE"), 0, 100, 1, 70))),
    maskBlendMode_(Gtk::manage(new PopUpButton())),

    sensiexclu_(Gtk::manage(new Adjuster(M("TP_LOCALLAB_SENSIEXCLU"), 0, 100, 1, 12))),
    structexclu_(Gtk::manage(new Adjuster(M("TP_LOCALLAB_STRUCCOL"), 0, 100, 1, 0))),
    locX_(Gtk::manage(new Adjuster(M("TP_LOCAL_WIDTH"), 2, 3000, 1, 150))),
    locXL_(Gtk::manage(new Adjuster(M("TP_LOCAL_WIDTH_L"), 2, 3000, 1, 150))),
    locY_(Gtk::manage(new Adjuster(M("TP_LOCAL_HEIGHT"), 2, 3000, 1, 150))),
    locYT_(Gtk::manage(new Adjuster(M("TP_LOCAL_HEIGHT_T"), 2, 3000, 1, 150))),
    centerX_(Gtk::manage(new Adjuster(M("TP_LOCALLAB_CENTER_X"), -1000, 1000, 1, 0))),
    centerY_(Gtk::manage(new Adjuster(M("TP_LOCALLAB_CENTER_Y"), -1000, 1000, 1, 0))),
    circrad_(Gtk::manage(new Adjuster(M("TP_LOCALLAB_CIRCRADIUS"), 1.5, 150., 0.5, 18.))),
    transit_(Gtk::manage(new Adjuster(M("TP_LOCALLAB_TRANSITVALUE"), 0.5, 100., 0.1, 60., Gtk::manage(new RTImage("feather"))))),
    transitweak_(Gtk::manage(new Adjuster(M("TP_LOCALLAB_TRANSITWEAK"), 0.5, 25.0, 0.1, 1.0))),
    transitgrad_(Gtk::manage(new Adjuster(M("TP_LOCALLAB_TRANSITGRAD"), -1.0, 1.0, 0.01, 0.0))),
    gradangle_(Gtk::manage(new Adjuster(M("TP_LOCALLAB_GRADANGLE"), -180., 180., 0.5, 0.))),
    feather_(Gtk::manage(new Adjuster(M("TP_LOCALLAB_FEATVALUE_MASK"), 10., 100., 0.1, 25.))),
    struc_(Gtk::manage(new Adjuster(M("TP_LOCALLAB_THRES"), 1.0, 12.0, 0.1, 4.0))),
    thresh_(Gtk::manage(new Adjuster(M("TP_LOCALLAB_THRESDELTAE"), 0.0, 15.0, 0.1, 2.0))),
    iter_(Gtk::manage(new Adjuster(M("TP_LOCALLAB_PROXI"), 0.2, 10.0, 0.1, 2.0))),
   // balan_(Gtk::manage(new Adjuster(M("TP_LOCALLAB_BALAN"), 0.05, 2.5, 0.05, 1.0, Gtk::manage(new RTImage("rawtherapee-logo-16")),  Gtk::manage(new RTImage("circle-white-small"))))),
    balan_(Gtk::manage(new Adjuster(M("TP_LOCALLAB_BALAN"), 0.05, 2.5, 0.05, 1.0, Gtk::manage(new RTImage("circle-yellow-small")),  Gtk::manage(new RTImage("circle-white-small"))))),
    balanh_(Gtk::manage(new Adjuster(M("TP_LOCALLAB_BALANH"), 0.2, 2.5, 0.1, 1.0, Gtk::manage(new RTImage("circle-multicolor-small")), Gtk::manage(new RTImage("circle-red-green-small"))))),
    colorde_(Gtk::manage(new Adjuster(M("TP_LOCALLAB_COLORDE"), -15, 15, 2, 5, Gtk::manage(new RTImage("circle-blue-yellow-small")), Gtk::manage(new RTImage("circle-gray-green-small"))))),
    colorscope_(Gtk::manage(new Adjuster(M("TP_LOCALLAB_COLORSCOPE"), 0., 100.0, 1., 30.))),
    avoidrad_(Gtk::manage(new Adjuster(M("TP_LOCALLAB_AVOIDRAD"), 0., 30.0, 0.1, 0.))),
    scopemask_(Gtk::manage(new Adjuster(M("TP_LOCALLAB_SCOPEMASK"), 0, 100, 1, 60))),
    denoichmask_(Gtk::manage(new Adjuster(M("TP_LOCALLAB_DENOIMASK"), 0., 100., 0.5, 0))),
    lumask_(Gtk::manage(new Adjuster(M("TP_LOCALLAB_LUMASK"), -50, 30, 1, 10, Gtk::manage(new RTImage("circle-yellow-small")), Gtk::manage(new RTImage("circle-gray-small")) ))),

    polyFeather_(Gtk::manage(new Adjuster(M("TP_LOCALLAB_POLY_FEATHER"), 0, 100, 1, 5, Gtk::manage(new RTImage("feather"))))),
    polySnapTol_(Gtk::manage(new Adjuster(M("TP_LOCALLAB_POLY_SNAP_TOL"), 0, 50, 1, 10))),
    polyLegLen_(Gtk::manage(new Adjuster(M("TP_LOCALLAB_POLY_LEG_LEN"), 0.5, 20, 0.5, 3))),

    hishow_(Gtk::manage(new Gtk::CheckButton(M("TP_LOCALLAB_PREVSHOW")))),
    activ_(Gtk::manage(new Gtk::CheckButton(M("TP_LOCALLAB_ACTIVSPOT")))),
    avoidneg_(Gtk::manage(new Gtk::CheckButton(M("TP_LOCALLAB_AVOIDNEG")))),
    blwh_(Gtk::manage(new Gtk::CheckButton(M("TP_LOCALLAB_BLWH")))),
    recurs_(Gtk::manage(new Gtk::CheckButton(M("TP_LOCALLAB_RECURS")))),
    laplac_(Gtk::manage(new Gtk::CheckButton(M("TP_LOCALLAB_LAPLACC")))),
    deltae_(Gtk::manage(new Gtk::CheckButton(M("TP_LOCALLAB_DELTAEC")))),
    shortc_(Gtk::manage(new Gtk::CheckButton(M("TP_LOCALLAB_SHORTC")))),
    //savrest_(Gtk::manage(new Gtk::CheckButton(M("TP_LOCALLAB_SAVREST")))),

    expTransGrad_(Gtk::manage(new MyExpander(false, M("TP_LOCALLAB_TRANSIT")))),
    expShapeDetect_(Gtk::manage(new MyExpander(false, M("TP_LOCALLAB_ARTIF")))),
    expSpecCases_(Gtk::manage(new MyExpander(false, M("TP_LOCALLAB_SPECCASE")))),
    expMaskMerge_(Gtk::manage(new MyExpander(false, M("TP_LOCALLAB_MASFRAME")))),
    expAdvanced_(Gtk::manage(new MyExpander(false, M("TP_LOCALLAB_SPOT_ADVANCED")))),

    preview_(Gtk::manage(new Gtk::ToggleButton(M("TP_LOCALLAB_PREVIEW")))),
    ctboxshape(Gtk::manage(new Gtk::Box())),
    ctboxactivmethod(Gtk::manage(new Gtk::Box())),
    ctboxspotmethod(Gtk::manage(new Gtk::Box())),    
    ctboxshapemethod(Gtk::manage(new Gtk::Box())),
    ctboxgamut(Gtk::manage(new Gtk::Box())),
    ctboxmasktype(Gtk::manage(new Gtk::Box())),
    ctboxaiclass(Gtk::manage(new Gtk::Box())),
    artifBox2(Gtk::manage(new ToolParamBlock())),

    controlPanelListener(nullptr),
    lastObject_(-1),
    nbSpotChanged_(false),
    selSpotChanged_(false),
    nameChanged_(false),
    visibilityChanged_(false),
    eventType(None),
    excluFrame(Gtk::manage(new Gtk::Frame(M("TP_LOCALLAB_EXCLUF")))),
    maskPrevActive(false)
{
    auto m = ProcEventMapper::getInstance();
    EvLocallabavoidgamutMethod = m->newEvent(AUTOEXP, "HISTORY_MSG_LOCAL_GAMUTMUNSEL");
    EvLocallabavoidnegative =  m->newEvent(AUTOEXP, "HISTORY_MSG_LOCAL_AVOIDNEGATIVE");
    const bool showtooltip = App::get().options().showtooltip;

//    pack_start(*hishow_);

    Gtk::Box* const ctboxprevmethod = Gtk::manage(new Gtk::Box());
    prevMethod_->append(M("TP_LOCALLAB_PREVHIDE"));
    prevMethod_->append(M("TP_LOCALLAB_PREVSHOW"));
    prevMethod_->set_active(0);
    prevMethodconn_ = prevMethod_->signal_changed().connect(
                          sigc::mem_fun(
                              *this, &ControlSpotPanel::prevMethodChanged));

//    ctboxprevmethod->pack_start(*prevMethod_);
    pack_start(*ctboxprevmethod);


    // Connect all button signals
    buttondeleteconn_ = button_delete_->signal_clicked().connect(
                            sigc::mem_fun(*this, &ControlSpotPanel::on_button_delete));
    buttonduplicateconn_ = button_duplicate_->signal_clicked().connect(
                               sigc::mem_fun(*this, &ControlSpotPanel::on_button_duplicate));
    buttonrenameconn_ = button_rename_->signal_clicked().connect(
                            sigc::mem_fun(*this, &ControlSpotPanel::on_button_rename));
    buttonvisibilityconn_ = button_visibility_->signal_button_release_event().connect(
                                sigc::mem_fun(*this, &ControlSpotPanel::on_button_visibility));

    // Keep unused buttons parented in a hidden box (Gtk::manage requires a parent)
    Gtk::Box* const hiddenBox = Gtk::manage(new Gtk::Box());
    hiddenBox->set_no_show_all(true);
    hiddenBox->hide();
    hiddenBox->pack_start(*button_delete_);
    hiddenBox->pack_start(*button_duplicate_);
    hiddenBox->pack_start(*button_rename_);
    hiddenBox->pack_start(*button_visibility_);
    pack_start(*hiddenBox);

    // One compact Add Mask button with a menu for every primary mask type.
    if (button_add_->get_child()) {
        button_add_->remove();
    }
    auto* addLabel = Gtk::manage(new Gtk::Label("+"));
    addLabel->set_halign(Gtk::ALIGN_CENTER);
    addLabel->set_valign(Gtk::ALIGN_CENTER);
    button_add_->add(*addLabel);
    button_add_->set_relief(Gtk::RELIEF_NONE);
    button_add_->set_can_focus(false);
    button_add_->set_tooltip_text(M("TP_LOCALLAB_BUTTON_ADD_MASK"));
    button_add_->set_size_request(24, 22);
    button_add_->get_style_context()->add_class("mask-add-button");

    addMaskMenu_ = new Gtk::Menu();
    const auto createMaskMenuItem = [](const char* icon, const Glib::ustring& label) {
        auto* box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 4));
        auto* img = Gtk::manage(new RTImage(icon));
        auto* lbl = Gtk::manage(new Gtk::Label(label));
        lbl->set_halign(Gtk::ALIGN_START);
        box->pack_start(*img, Gtk::PACK_SHRINK);
        box->pack_start(*lbl, Gtk::PACK_EXPAND_WIDGET);
        auto* mi = Gtk::manage(new Gtk::MenuItem());
        mi->add(*box);
        return mi;
    };

    auto* aiMenuItem = createMaskMenuItem("mask-ai", M("TP_LOCALLAB_MASKTYPE_AI"));
    auto* aiClassMenu = Gtk::manage(new Gtk::Menu());
    const char* aiClassKeys[] = {
        "TP_LOCALLAB_AIMASK_CLASS_BACKGROUND",
        "TP_LOCALLAB_AIMASK_CLASS_PERSON",
        "TP_LOCALLAB_AIMASK_CLASS_SKY",
        "TP_LOCALLAB_AIMASK_CLASS_VEGETATION",
        "TP_LOCALLAB_AIMASK_CLASS_BUILDING",
        "TP_LOCALLAB_AIMASK_CLASS_VEHICLE",
        "TP_LOCALLAB_AIMASK_CLASS_ANIMAL",
        "TP_LOCALLAB_AIMASK_CLASS_FOREGROUND"
    };
    const char* aiClassIcons[] = {
        "mask-class-background", "mask-class-person", "mask-class-sky",
        "mask-class-vegetation", "mask-class-building", "mask-class-vehicle",
        "mask-class-animal", "mask-class-foreground"
    };
    for (int i = 0; i < 8; ++i) {
        auto* item = createMaskMenuItem(aiClassIcons[i], M(aiClassKeys[i]));
        item->signal_activate().connect(
            sigc::bind(sigc::mem_fun(*this, &ControlSpotPanel::on_ai_mask_selected), i));
        aiClassMenu->append(*item);
    }
    aiMenuItem->set_submenu(*aiClassMenu);
    addMaskMenu_->append(*aiMenuItem);

    const auto addShapeMenuItem = [this, &createMaskMenuItem](
        const char* icon, const Glib::ustring& label, int shape) {
        auto* mi = createMaskMenuItem(icon, label);
        mi->signal_activate().connect([this, shape]() {
            on_mask_shape_selected(shape);
        });
        addMaskMenu_->append(*mi);
    };
    addShapeMenuItem("shape-ellipse", M("TP_LOCALLAB_ELI"), 0);
    addShapeMenuItem("shape-rectangle", M("TP_LOCALLAB_RECT"), 1);
    addShapeMenuItem("shape-gradient", M("TP_LOCALLAB_GRAD"), 2);
    addMaskMenu_->show_all();

    buttonaddconn_ = button_add_->signal_clicked().connect([this]() {
        addMaskMenu_->popup_at_widget(button_add_,
            Gdk::GRAVITY_SOUTH_WEST, Gdk::GRAVITY_NORTH_WEST, nullptr);
    });

    // Right-click context menu for mask rows
    contextMenu_ = new Gtk::Menu();
    {
        auto* mi = Gtk::manage(new Gtk::MenuItem(M("TP_LOCALLAB_BUTTON_REN")));
        mi->signal_activate().connect(sigc::mem_fun(*this, &ControlSpotPanel::on_button_rename));
        contextMenu_->append(*mi);
    }
    {
        auto* mi = Gtk::manage(new Gtk::MenuItem(M("TP_LOCALLAB_BUTTON_DUPL")));
        mi->signal_activate().connect(sigc::mem_fun(*this, &ControlSpotPanel::on_button_duplicate));
        contextMenu_->append(*mi);
    }
    {
        auto* blendItem = Gtk::manage(new Gtk::MenuItem(M("TP_LOCALLAB_MASK_BLEND_MODE")));
        auto* blendMenu = Gtk::manage(new Gtk::Menu());
        Gtk::RadioMenuItem::Group blendGroup;
        const char* blendKeys[] = {
            "TP_LOCALLAB_MASK_BLEND_NORMAL",
            "TP_LOCALLAB_MASK_BLEND_DARKEN",
            "TP_LOCALLAB_MASK_BLEND_LIGHTEN",
            "TP_LOCALLAB_MASK_BLEND_LUMINOSITY",
            "TP_LOCALLAB_MASK_BLEND_COLOR"
        };
        for (int i = 0; i < 5; ++i) {
            auto* item = Gtk::manage(new Gtk::RadioMenuItem(blendGroup, M(blendKeys[i])));
            blendModeMenuItems_[i] = item;
            item->signal_toggled().connect([this, item, i]() {
                if (item->get_active() && !updatingBlendModeMenu_) {
                    setMaskBlendMode(i);
                }
            });
            blendMenu->append(*item);
        }
        blendItem->set_submenu(*blendMenu);
        contextMenu_->append(*blendItem);
    }
    {
        auto* mi = Gtk::manage(new Gtk::MenuItem(M("TP_LOCALLAB_BUTTON_DEL")));
        mi->signal_activate().connect(sigc::mem_fun(*this, &ControlSpotPanel::on_button_delete));
        contextMenu_->append(*mi);
    }
    contextMenu_->show_all();

    treemodel_ = Gtk::ListStore::create(spots_);
    treeview_->set_model(treemodel_);
    treeview_->set_reorderable(true);
    rowsReorderedConn_ = treemodel_->signal_rows_reordered().connect(
        sigc::mem_fun(*this, &ControlSpotPanel::onRowsReordered));
    treeview_->set_name("MaskTreeView");
    treeviewconn_ = treeview_->get_selection()->signal_changed().connect(
                        sigc::mem_fun(
                            *this, &ControlSpotPanel::controlspotChanged));
    treeview_->set_grid_lines(Gtk::TREE_VIEW_GRID_LINES_NONE);
    treeview_->set_headers_visible(false);

    // Disable search to prevent hijacking keyboard shortcuts #5265
    treeview_->set_enable_search(false);
    treeview_->signal_key_press_event().connect(
        sigc::mem_fun(
            *this, &ControlSpotPanel::blockTreeviewSearch), false);

    // Handle right-click (context menu), left-click on visibility column, and Ctrl+click
    treeview_->signal_button_press_event().connect(
        sigc::mem_fun(
            *this, &ControlSpotPanel::onSpotSelectionEvent), false);

    // Sidebar hover: show mask overlay when mouse is over a selected row
    treeview_->add_events(Gdk::POINTER_MOTION_MASK | Gdk::LEAVE_NOTIFY_MASK);
    treeview_->signal_motion_notify_event().connect(
        sigc::mem_fun(*this, &ControlSpotPanel::onTreeviewMotion), false);
    treeview_->signal_leave_notify_event().connect(
        sigc::mem_fun(*this, &ControlSpotPanel::onTreeviewLeave), false);

    // Preview column — small colored square for AI masks
    auto* detailsCell = Gtk::manage(new Gtk::CellRendererText());
    detailsCell->property_xalign() = 0.5f;
    detailsCell->property_ypad() = 11;
    int cols_count = treeview_->append_column("", *detailsCell);
    auto col = treeview_->get_column(cols_count - 1);
    if (col) {
        col->set_expand(false);
        col->set_fixed_width(22);
        col->set_cell_data_func(
            *detailsCell, sigc::mem_fun(
                *this, &ControlSpotPanel::render_details_toggle));
    }

    auto* previewCell = Gtk::manage(new Gtk::CellRendererPixbuf());
    previewCell->property_ypad() = 11;
    cols_count = treeview_->append_column("", *previewCell);
    col = treeview_->get_column(cols_count - 1);

    if (col) {
        col->set_expand(false);
        col->set_cell_data_func(
            *previewCell, sigc::mem_fun(
                *this, &ControlSpotPanel::render_preview));
    }

    // Name column
    auto cell = Gtk::manage(new Gtk::CellRendererText());
    cell->property_ellipsize() = Pango::ELLIPSIZE_END;
    cell->property_ypad() = 11;
    cols_count = treeview_->append_column("", *cell);
    col = treeview_->get_column(cols_count - 1);

    if (col) {
        col->set_expand(true);
        col->set_cell_data_func(
            *cell, sigc::mem_fun(
                *this, &ControlSpotPanel::render_name));
    }

    // Visibility column — eye icon, toggled via left-click
    auto* pixCell = Gtk::manage(new Gtk::CellRendererPixbuf());
    pixCell->property_stock_size() = Gtk::ICON_SIZE_MENU;
    pixCell->property_xpad() = 4;
    pixCell->property_ypad() = 11;
    cols_count = treeview_->append_column("", *pixCell);
    col = treeview_->get_column(cols_count - 1);

    if (col) {
        col->set_expand(false);
        col->set_cell_data_func(
            *pixCell, sigc::mem_fun(
                *this, &ControlSpotPanel::render_isvisible));
    }

    scrolledwindow_->add(*treeview_);
    scrolledwindow_->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    scrolledwindow_->set_shadow_type(Gtk::SHADOW_NONE);
    // Start compact — grows as masks are added, caps at 300px then scrolls
    scrolledwindow_->set_min_content_height(40);
    scrolledwindow_->set_max_content_height(300);
    scrolledwindow_->set_propagate_natural_height(true);
    scrolledwindow_->set_margin_end(4);
    pack_start(*scrolledwindow_, Gtk::PACK_SHRINK);

    // hishow_ and activ_ are configured here but packed into advancedBox below
   // Gtk::Box* const ctboxactivmethod = Gtk::manage(new Gtk::Box());
    ctboxactivmethod->pack_start(*activ_);

    shape_->addEntry("shape-ellipse", M("TP_LOCALLAB_ELI"));
    shape_->addEntry("shape-rectangle", M("TP_LOCALLAB_RECT"));
    shape_->addEntry("shape-gradient", M("TP_LOCALLAB_GRAD"));
    shape_->addEntry("shape-polygon", M("TP_LOCALLAB_POLY"));
    shape_->setSelected(0);
    shape_->show();
    shapeconn_ = shape_->signal_changed().connect(
                     sigc::mem_fun(
                         *this, &ControlSpotPanel::shapeChanged));
    shape_->hideArrowButton();
    shape_->signal_clicked().connect([this]() { shape_->triggerShowMenu(); });
    shape_->buttonGroup->set_hexpand(false);
    shape_->buttonGroup->set_halign(Gtk::ALIGN_START);
    ctboxshape->pack_start(*shape_->buttonGroup, Gtk::PACK_SHRINK);
    shape_->setShowSelectionLabel(true);
    // ctboxshape packed into maskDetailBox_ below
    if (showtooltip) {
        shape_->set_tooltip_text(M("TP_LOCALLAB_SHAPE_TOOLTIP"));
    }

    gradangle_->setAdjusterListener(this);
    if (showtooltip) {
        gradangle_->set_tooltip_text(M("TP_LOCALLAB_GRADANGLE_TOOLTIP"));
    }
    // gradangle_ packed into advancedBox below

    // Polygon (Lasso) controls — draw button + vertex count inline with shape row
    {
        polyDrawBtn_ = Gtk::manage(new Gtk::ToggleButton());
        polyDrawBtn_->set_image(*Gtk::manage(new RTImage("poly-draw")));
        polyDrawBtn_->set_tooltip_text(M("TP_LOCALLAB_POLY_DRAW"));
        polyDrawConn_ = polyDrawBtn_->signal_toggled().connect(
            sigc::mem_fun(*this, &ControlSpotPanel::polyDrawClicked));

        polyVertexLabel_ = Gtk::manage(new Gtk::Label("0"));
        polyVertexLabel_->set_margin_start(2);
    }

    // Polygon sliders box (feather, snap tolerance, leg length)
    polyBox_ = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 2));
    {
        polyFeather_->setAdjusterListener(this);
        polySnapTol_->setAdjusterListener(this);
        polyLegLen_->setAdjusterListener(this);

        polyBox_->pack_start(*polyFeather_, Gtk::PACK_SHRINK);
        polyBox_->pack_start(*polySnapTol_, Gtk::PACK_SHRINK);
        polyBox_->pack_start(*polyLegLen_, Gtk::PACK_SHRINK);
    }
    polyBox_->set_no_show_all(true);
    // polyBox_ packed into maskDetailBox_ below

    Gtk::Label* const labelspotmethod = Gtk::manage(new Gtk::Label(M("TP_LOCALLAB_EXCLUTYPE") + ":"));
    labelspotmethod->set_ellipsize(Pango::ELLIPSIZE_END);
    labelspotmethod->set_max_width_chars(12);
    ctboxspotmethod->pack_start(*labelspotmethod, Gtk::PACK_SHRINK, 2);

    if (showtooltip) {
        ctboxspotmethod->set_tooltip_markup(M("TP_LOCALLAB_EXCLUTYPE_TOOLTIP"));
    }

    spotMethod_->append(M("TP_LOCALLAB_EXNORM"));
    spotMethod_->append(M("TP_LOCALLAB_EXECLU"));
    spotMethod_->append(M("TP_LOCALLAB_EXFULL"));
    spotMethod_->append(M("TP_LOCALLAB_EXMAIN"));//new choice Global
    spotMethod_->set_active(0);
    spotMethodconn_ = spotMethod_->signal_changed().connect(
                          sigc::mem_fun(
                              *this, &ControlSpotPanel::spotMethodChanged));
    ctboxspotmethod->pack_start(*spotMethod_);
    // ctboxspotmethod packed into advancedBox below


    excluFrame->set_label_align(0.025, 0.5);

    if (showtooltip) {
        excluFrame->set_tooltip_text(M("TP_LOCALLAB_EXCLUF_TOOLTIP"));
    }

    ToolParamBlock* const excluBox = Gtk::manage(new ToolParamBlock());

    if (showtooltip) {
        sensiexclu_->set_tooltip_text(M("TP_LOCALLAB_SENSIEXCLU_TOOLTIP"));
    }

    sensiexclu_->setAdjusterListener(this);
    structexclu_->setAdjusterListener(this);
    structexclu_->setLogScale(10, 0);

    excluBox->pack_start(*sensiexclu_);
    excluBox->pack_start(*structexclu_);
    excluFrame->add(*excluBox);
    // excluFrame packed into advancedBox below


    Gtk::Label* const labelshapemethod = Gtk::manage(new Gtk::Label(M("TP_LOCALLAB_STYPE") + ":"));
    ctboxshapemethod->pack_start(*labelshapemethod, Gtk::PACK_SHRINK, 4);

    if (showtooltip) {
        ctboxshapemethod->set_tooltip_markup(M("TP_LOCALLAB_STYPE_TOOLTIP"));
    }

    shapeMethod_->append(M("TP_LOCALLAB_IND"));
    shapeMethod_->append(M("TP_LOCALLAB_SYM"));
    shapeMethod_->append(M("TP_LOCALLAB_INDSL"));
    shapeMethod_->append(M("TP_LOCALLAB_SYMSL"));
    shapeMethod_->set_active(0);
    shapeMethodconn_ = shapeMethod_->signal_changed().connect(
                           sigc::mem_fun(
                               *this, &ControlSpotPanel::shapeMethodChanged));
    ctboxshapemethod->pack_start(*shapeMethod_);
//    pack_start(*ctboxshapemethod);

    // locX_, locXL_, locY_, locYT_, centerX_, centerY_ packed into advancedBox below
    locX_->setAdjusterListener(this);
    locXL_->setAdjusterListener(this);
    locY_->setAdjusterListener(this);
    locYT_->setAdjusterListener(this);
    centerX_->setAdjusterListener(this);
    centerY_->setAdjusterListener(this);

    // circrad_ packed into maskDetailBox_ below
    circrad_->setAdjusterListener(this);

    if (showtooltip) {
        circrad_->set_tooltip_text(M("TP_LOCALLAB_CIRCRAD_TOOLTIP"));
    }

    // transit_ (Mask feathering) packed into maskDetailBox_ below
    transit_->setAdjusterListener(this);

    if (showtooltip) {
        transit_->set_tooltip_text(M("TP_LOCALLAB_TRANSIT_TOOLTIP"));
    }

    // Mask Type combo (Normal / AI Mask)
    maskType_->addEntry("mask-normal", M("TP_LOCALLAB_MASKTYPE_NORMAL"));
    maskType_->addEntry("mask-ai", M("TP_LOCALLAB_MASKTYPE_AI"));
    maskType_->setSelected(0);
    maskType_->show();
    maskTypeConn_ = maskType_->signal_changed().connect(
                        sigc::mem_fun(
                            *this, &ControlSpotPanel::maskTypeChanged));
    maskType_->hideArrowButton();
    maskType_->signal_clicked().connect([this]() { maskType_->triggerShowMenu(); });
    maskType_->buttonGroup->set_hexpand(false);
    maskType_->buttonGroup->set_halign(Gtk::ALIGN_START);
    ctboxmasktype->pack_start(*maskType_->buttonGroup, Gtk::PACK_SHRINK);
    maskType_->setShowSelectionLabel(true);

    // AI Class combo
    aiMaskClass_->addEntry("mask-class-background", M("TP_LOCALLAB_AIMASK_CLASS_BACKGROUND"));
    aiMaskClass_->addEntry("mask-class-person", M("TP_LOCALLAB_AIMASK_CLASS_PERSON"));
    aiMaskClass_->addEntry("mask-class-sky", M("TP_LOCALLAB_AIMASK_CLASS_SKY"));
    aiMaskClass_->addEntry("mask-class-vegetation", M("TP_LOCALLAB_AIMASK_CLASS_VEGETATION"));
    aiMaskClass_->addEntry("mask-class-building", M("TP_LOCALLAB_AIMASK_CLASS_BUILDING"));
    aiMaskClass_->addEntry("mask-class-vehicle", M("TP_LOCALLAB_AIMASK_CLASS_VEHICLE"));
    aiMaskClass_->addEntry("mask-class-animal", M("TP_LOCALLAB_AIMASK_CLASS_ANIMAL"));
    aiMaskClass_->addEntry("mask-class-foreground", M("TP_LOCALLAB_AIMASK_CLASS_FOREGROUND"));
    aiMaskClass_->setSelected(2); // Default: Sky
    aiMaskClass_->show();
    aiMaskClassConn_ = aiMaskClass_->signal_changed().connect(
                           sigc::mem_fun(
                               *this, &ControlSpotPanel::aiMaskClassChanged));
    aiMaskClass_->hideArrowButton();
    aiMaskClass_->signal_clicked().connect([this]() { aiMaskClass_->triggerShowMenu(); });
    aiMaskClass_->buttonGroup->set_hexpand(false);
    aiMaskClass_->buttonGroup->set_halign(Gtk::ALIGN_START);
    ctboxaiclass->pack_start(*aiMaskClass_->buttonGroup, Gtk::PACK_SHRINK);
    aiMaskClass_->setShowSelectionLabel(true);
    if (showtooltip) {
        aiMaskClass_->set_tooltip_text(M("TP_LOCALLAB_AIMASK_CLASS_TOOLTIP"));
        aiMaskTolerance_->set_tooltip_text(M("TP_LOCALLAB_AIMASK_TOLERANCE_TOOLTIP"));
    }
    aiMaskTolerance_->setAdjusterListener(this);

    maskBlendMode_->addEntry("blend-normal", M("TP_LOCALLAB_MASK_BLEND_NORMAL"));
    maskBlendMode_->addEntry("blend-darken", M("TP_LOCALLAB_MASK_BLEND_DARKEN"));
    maskBlendMode_->addEntry("blend-lighten", M("TP_LOCALLAB_MASK_BLEND_LIGHTEN"));
    maskBlendMode_->addEntry("blend-luminosity", M("TP_LOCALLAB_MASK_BLEND_LUMINOSITY"));
    maskBlendMode_->addEntry("blend-color", M("TP_LOCALLAB_MASK_BLEND_COLOR"));
    maskBlendMode_->setSelected(0);
    maskBlendMode_->hideArrowButton();
    maskBlendMode_->signal_clicked().connect([this]() { maskBlendMode_->triggerShowMenu(); });
    maskBlendMode_->buttonGroup->set_hexpand(false);
    maskBlendMode_->buttonGroup->set_halign(Gtk::ALIGN_START);
    maskBlendMode_->setShowSelectionLabel(true);
    maskBlendModeConn_ = maskBlendMode_->signal_changed().connect(
        sigc::mem_fun(*this, &ControlSpotPanel::maskBlendModeChanged));
    if (showtooltip) {
        maskBlendMode_->set_tooltip_text(M("TP_LOCALLAB_MASK_BLEND_TOOLTIP"));
    }

    // Quality method (not packed at top level, used internally)
    Gtk::Box* const ctboxqualitymethod = Gtk::manage(new Gtk::Box());
    Gtk::Label* const labelqualitymethod = Gtk::manage(new Gtk::Label(M("TP_LOCALLAB_QUAL_METHOD") + ":"));
    ctboxqualitymethod->pack_start(*labelqualitymethod, Gtk::PACK_SHRINK, 4);

    if (showtooltip) {
        ctboxqualitymethod->set_tooltip_markup(M("TP_LOCALLAB_METHOD_TOOLTIP"));
    }

    qualityMethod_->append(M("TP_LOCALLAB_ENH"));
    qualityMethod_->append(M("TP_LOCALLAB_ENHDEN"));
    qualityMethod_->set_active(1);
    qualityMethodconn_ = qualityMethod_->signal_changed().connect(
                             sigc::mem_fun(
                                 *this, &ControlSpotPanel::qualityMethodChanged));
    ctboxqualitymethod->pack_start(*qualityMethod_);

    // --- Advanced section (collapsed by default) ---
    ToolParamBlock* const advancedBox = Gtk::manage(new ToolParamBlock());

    // Moved from top level: mask settings that are less commonly changed
    advancedBox->pack_start(*hishow_);
    advancedBox->pack_start(*ctboxactivmethod);
    advancedBox->pack_start(*gradangle_);
    advancedBox->pack_start(*ctboxspotmethod);
    advancedBox->pack_start(*excluFrame);
    advancedBox->pack_start(*locX_);
    advancedBox->pack_start(*locXL_);
    advancedBox->pack_start(*locY_);
    advancedBox->pack_start(*locYT_);
    advancedBox->pack_start(*centerX_);
    advancedBox->pack_start(*centerY_);

    // Transition Gradient sliders (Decay and Symmetry)
    if (showtooltip) {
        expTransGrad_->set_tooltip_text(M("TP_LOCALLAB_TRANSIT_TOOLTIP"));
        transitweak_->set_tooltip_text(M("TP_LOCALLAB_TRANSITWEAK_TOOLTIP"));
        feather_->set_tooltip_text(M("TP_LOCALLAB_FEATH_TOOLTIP"));
        transitgrad_->set_tooltip_text(M("TP_LOCALLAB_TRANSITGRAD_TOOLTIP"));
        scopemask_->set_tooltip_text(M("TP_LOCALLAB_SCOPEMASK_TOOLTIP"));
        denoichmask_->set_tooltip_text(M("TP_LOCALLAB_DENOIMASK_TOOLTIP"));
    }

    transitweak_->setAdjusterListener(this);
    transitgrad_->setAdjusterListener(this);
    feather_->setAdjusterListener(this);
    scopemask_->setAdjusterListener(this);
    denoichmask_->setAdjusterListener(this);

    advancedBox->pack_start(*transitweak_);
    advancedBox->pack_start(*transitgrad_);

    // ΔE Shape detection expander
    if (showtooltip) {
        expShapeDetect_->set_tooltip_text(M("TP_LOCALLAB_ARTIF_TOOLTIP"));
    }

    ToolParamBlock* const artifBox = Gtk::manage(new ToolParamBlock());
    struc_->setAdjusterListener(this);
    thresh_->setAdjusterListener(this);
    iter_->setAdjusterListener(this);
    balan_->setAdjusterListener(this);
    balanh_->setAdjusterListener(this);
    colorde_->setAdjusterListener(this);
    colorscope_->setAdjusterListener(this);
    avoidrad_->setAdjusterListener(this);

    preview_->set_active(false);
    previewConn_ = preview_->signal_clicked().connect(
                       sigc::mem_fun(
                           *this, &ControlSpotPanel::previewChanged));

    if (showtooltip) {
        balan_->set_tooltip_text(M("TP_LOCALLAB_BALAN_TOOLTIP"));
        balanh_->set_tooltip_text(M("TP_LOCALLAB_BALAN_TOOLTIP"));
        colorde_->set_tooltip_text(M("TP_LOCALLAB_COLORDE_TOOLTIP"));
        colorscope_->set_tooltip_text(M("TP_LOCALLAB_COLORSCOPE_TOOLTIP"));
        preview_->set_tooltip_text(M("TP_LOCALLAB_COLORDEPREV_TOOLTIP"));
    }

//    artifBox->pack_start(*struc_);
    artifBox->pack_start(*thresh_);
    artifBox->pack_start(*iter_);
    artifBox->pack_start(*balan_);
    artifBox->pack_start(*balanh_);
    artifBox->pack_start(*colorde_);
    expShapeDetect_->add(*artifBox, false);
    advancedBox->pack_start(*expShapeDetect_, false, false);

    // Preview ΔE button + colorscope
    artifBox2->pack_start(*preview_);
    artifBox2->pack_start(*colorscope_);//unused with contrlspotpanel since 17 / 01 : 2024 but data used in color, vibrance, sh
    colorscope_->hide();
    advancedBox->pack_start(*artifBox2);

    // Specific cases expander
    ToolParamBlock* const specCaseBox = Gtk::manage(new ToolParamBlock());

    hishowconn_  = hishow_->signal_toggled().connect(
                      sigc::mem_fun(*this, &ControlSpotPanel::hishowChanged));

    activConn_  = activ_->signal_toggled().connect(
                     sigc::mem_fun(*this, &ControlSpotPanel::activChanged));

    Gtk::Label* const labelgamut = Gtk::manage(new Gtk::Label(M("TP_LOCALLAB_AVOID") + ":"));
    ctboxgamut->pack_start(*labelgamut, Gtk::PACK_SHRINK, 4);
    avoidgamutMethod_->append(M("TP_LOCALLAB_GAMUTNON"));
    avoidgamutMethod_->append(M("TP_LOCALLAB_GAMUTLABRELA"));
    avoidgamutMethod_->append(M("TP_LOCALLAB_GAMUTXYZABSO"));
    avoidgamutMethod_->append(M("TP_LOCALLAB_GAMUTXYZRELA"));
    avoidgamutMethod_->append(M("TP_LOCALLAB_GAMUTMUNSELL"));
    avoidgamutMethod_->set_active(4);
    avoidgamutconn_ = avoidgamutMethod_->signal_changed().connect(
                     sigc::mem_fun(
                         *this, &ControlSpotPanel::avoidgamutMethodChanged));
    ctboxgamut->pack_start(*avoidgamutMethod_);
    if (showtooltip) {
        ctboxgamut->set_tooltip_text(M("TP_LOCALLAB_AVOIDCOLORSHIFT_TOOLTIP"));
    }

    Gtk::Frame* const avFrame = Gtk::manage(new Gtk::Frame());
    ToolParamBlock* const avbox = Gtk::manage(new ToolParamBlock());
    avFrame->set_label_align(0.025, 0.5);
    avbox->pack_start(*ctboxgamut);
    avbox->pack_start(*avoidrad_);
    avFrame->add(*avbox);
    specCaseBox->pack_start(*avFrame);

    avoidnegConn_  = avoidneg_->signal_toggled().connect(
                     sigc::mem_fun(*this, &ControlSpotPanel::avoidnegChanged));

    blwhConn_  = blwh_->signal_toggled().connect(
                     sigc::mem_fun(*this, &ControlSpotPanel::blwhChanged));

    if (showtooltip) {
        blwh_->set_tooltip_text(M("TP_LOCALLAB_BLWH_TOOLTIP"));
    }

    specCaseBox->pack_start(*blwh_);
    specCaseBox->pack_start(*avoidneg_);

    recursConn_  = recurs_->signal_toggled().connect(
                       sigc::mem_fun(*this, &ControlSpotPanel::recursChanged));

    if (showtooltip) {
        recurs_->set_tooltip_text(M("TP_LOCALLAB_RECURS_TOOLTIP"));
    }

    specCaseBox->pack_start(*recurs_);
    specCaseBox->pack_start(*ctboxshapemethod);

    Gtk::Box* const ctboxwavmethod = Gtk::manage(new Gtk::Box());
    Gtk::Label* const labelwavmethod = Gtk::manage(new Gtk::Label(M("TP_WAVELET_DAUBLOCAL") + ":"));
    ctboxwavmethod->pack_start(*labelwavmethod, Gtk::PACK_SHRINK, 4);

    if (showtooltip) {
        ctboxwavmethod->set_tooltip_markup(M("TP_WAVELET_DAUB_TOOLTIP"));
    }

    wavMethod_->append(M("TP_WAVELET_DAUB2"));
    wavMethod_->append(M("TP_WAVELET_DAUB4"));
    wavMethod_->append(M("TP_WAVELET_DAUB6"));
    wavMethod_->append(M("TP_WAVELET_DAUB10"));
    wavMethod_->append(M("TP_WAVELET_DAUB14"));
    wavMethod_->append(M("TP_WAVELET_DAUB20"));
    wavMethod_->set_active(1);
    wavMethodconn_ = wavMethod_->signal_changed().connect(
                         sigc::mem_fun(
                             *this, &ControlSpotPanel::wavMethodChanged));
    ctboxwavmethod->pack_start(*wavMethod_);
    specCaseBox->pack_start(*ctboxwavmethod);

    expSpecCases_->add(*specCaseBox, false);
    advancedBox->pack_start(*expSpecCases_, false, false);

    // Mask and Merge expander
    if (showtooltip) {
        expMaskMerge_->set_tooltip_text(M("TP_LOCALLAB_MASFRAME_TOOLTIP"));
    }

    ToolParamBlock* const maskBox = Gtk::manage(new ToolParamBlock());
    laplacConn_  = laplac_->signal_toggled().connect(
                       sigc::mem_fun(*this, &ControlSpotPanel::laplacChanged));
    deltaeConn_  = deltae_->signal_toggled().connect(
                       sigc::mem_fun(*this, &ControlSpotPanel::deltaeChanged));
    shortcConn_  = shortc_->signal_toggled().connect(
                       sigc::mem_fun(*this, &ControlSpotPanel::shortcChanged));

    if (showtooltip) {
        shortc_->set_tooltip_text(M("TP_LOCALLAB_SHORTCMASK_TOOLTIP"));
    }

    lumask_->setAdjusterListener(this);

    if (showtooltip) {
        lumask_->set_tooltip_text(M("TP_LOCALLAB_LUMASK_TOOLTIP"));
        laplac_->set_tooltip_text(M("TP_LOCALLAB_LAP_MASK_TOOLTIP"));
    }

//    maskBox->pack_start(*laplac_);
    maskBox->pack_start(*deltae_);
    maskBox->pack_start(*scopemask_);
    maskBox->pack_start(*denoichmask_);
    maskBox->pack_start(*feather_);
    // maskBox->pack_start(*shortc_);
    maskBox->pack_start(*lumask_);
    // maskBox->pack_start(*savrest_);
    expMaskMerge_->add(*maskBox, false);
    advancedBox->pack_start(*expMaskMerge_, false, false);

    // Pack the Advanced expander (collapsed by default)
    expAdvanced_->add(*advancedBox, false);
    expAdvanced_->setLevel(2);

    // Mask-specific controls are revealed by the chevron on the selected row.
    maskDetailExpanded_ = false;

    maskDetailBox_ = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 4));
    shapeTypeRow_ = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 8));
    shapeTypeRow_->set_margin_top(6);
    shapeTypeRow_->pack_start(*ctboxshape, Gtk::PACK_SHRINK);
    shapeTypeRow_->pack_start(*ctboxmasktype, Gtk::PACK_SHRINK);

    // Polygon draw control. Point count remains internal to keep the row quiet.
    polyDrawBtn_->set_no_show_all(true);
    shapeTypeRow_->pack_start(*polyDrawBtn_, Gtk::PACK_SHRINK);

    polyVertexIcon_ = Gtk::manage(new RTImage("poly-vertices"));
    polyVertexIcon_->set_no_show_all(true);
    polyVertexIcon_->set_margin_start(4);
    shapeTypeRow_->pack_start(*polyVertexIcon_, Gtk::PACK_SHRINK);

    polyVertexLabel_->set_no_show_all(true);
    shapeTypeRow_->pack_start(*polyVertexLabel_, Gtk::PACK_SHRINK);

    maskDetailBox_->pack_start(*shapeTypeRow_, Gtk::PACK_SHRINK);
    maskDetailBox_->pack_start(*ctboxaiclass, Gtk::PACK_SHRINK);
    maskDetailBox_->pack_start(*aiMaskTolerance_, Gtk::PACK_SHRINK);
    maskDetailBox_->pack_start(*maskBlendMode_->buttonGroup, Gtk::PACK_SHRINK);
    maskDetailBox_->pack_start(*polyBox_, Gtk::PACK_SHRINK);
    maskDetailBox_->pack_start(*circrad_, Gtk::PACK_SHRINK);
    maskDetailBox_->pack_start(*transit_, Gtk::PACK_SHRINK);

    maskRevealer_ = Gtk::manage(new Gtk::Revealer());
    maskRevealer_->set_transition_type(Gtk::REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    maskRevealer_->set_transition_duration(140);
    maskRevealer_->set_reveal_child(false);
    maskRevealer_->add(*maskDetailBox_);
    maskRevealer_->set_no_show_all(true);
    maskRevealer_->hide();
    pack_start(*maskRevealer_, Gtk::PACK_SHRINK);

    // Grey out mask controls until a mask/spot is added
    setMaskControlsSensitive(false);

    Gtk::Separator *separatormet = Gtk::manage(new Gtk::Separator(Gtk::ORIENTATION_HORIZONTAL));
    pack_start(*separatormet, Gtk::PACK_SHRINK, 2);

    //Gtk::Box* const ctboxcomplexmethod = Gtk::manage(new Gtk::Box());

    //if (showtooltip) {
    //    ctboxcomplexmethod->set_tooltip_markup(M("TP_LOCALLAB_COMPLEXMETHOD_TOOLTIP"));
    //}

    //Gtk::Label* const labelcomplexmethod = Gtk::manage(new Gtk::Label(M("TP_LOCALLAB_COMPLEX_METHOD") + ":"));
    //ctboxcomplexmethod->pack_start(*labelcomplexmethod, Gtk::PACK_SHRINK, 4);

    //if (showtooltip) {
    //    complexMethod_->set_tooltip_markup(M("TP_LOCALLAB_COMPLEX_TOOLTIP"));
    //}

    //complexMethod_->append(M("TP_LOCALLAB_SIM"));
    //complexMethod_->append(M("TP_LOCALLAB_MED"));
    //complexMethod_->append(M("TP_LOCALLAB_ALL"));
    //complexMethod_->set_active(1);
    //complexMethodconn_ = complexMethod_->signal_changed().connect(
    //                         sigc::mem_fun(
    //                             *this, &ControlSpotPanel::complexMethodChanged));
    //ctboxcomplexmethod->pack_start(*complexMethod_);
    // pack_start(*ctboxcomplexmethod);
/*
    Gtk::Box* const ctboxwavmethod = Gtk::manage(new Gtk::Box());
    Gtk::Label* const labelwavmethod = Gtk::manage(new Gtk::Label(M("TP_WAVELET_DAUBLOCAL") + ":"));
    ctboxwavmethod->pack_start(*labelwavmethod, Gtk::PACK_SHRINK, 4);

    if (showtooltip) {
        ctboxwavmethod->set_tooltip_markup(M("TP_WAVELET_DAUB_TOOLTIP"));
    }

    wavMethod_->append(M("TP_WAVELET_DAUB2"));
    wavMethod_->append(M("TP_WAVELET_DAUB4"));
    wavMethod_->append(M("TP_WAVELET_DAUB6"));
    wavMethod_->append(M("TP_WAVELET_DAUB10"));
    wavMethod_->append(M("TP_WAVELET_DAUB14"));
    wavMethod_->set_active(1);
    wavMethodconn_ = wavMethod_->signal_changed().connect(
                         sigc::mem_fun(
                             *this, &ControlSpotPanel::wavMethodChanged));
    ctboxwavmethod->pack_start(*wavMethod_);
    pack_start(*ctboxwavmethod);
*/
    set_name("MaskingPanel");
    set_margin_end(6);  // Prevent content clipping at right edge
    show_all();
    maskRevealer_->hide();
    // Hide poly-specific and AI-specific widgets initially
    polyDrawBtn_->hide();
    polyVertexIcon_->hide();
    polyVertexLabel_->hide();
    polyBox_->hide();
    ctboxaiclass->hide();
    // Define row background color
    // Mouseovered spot (opaque orange)
    colorMouseover.set_red(1.);
    colorMouseover.set_green(100. / 255.);
    colorMouseover.set_blue(0.);
    colorMouseover.set_alpha(1.);

    colorMouseovertext.set_red(0.6);
    colorMouseovertext.set_green(100. / 255.);
    colorMouseovertext.set_blue(0.);
    colorMouseovertext.set_alpha(0.5);

    // Nominal spot (transparent black)
    colorNominal.set_red(0.);
    colorNominal.set_green(0.);
    colorNominal.set_blue(0.);
    colorNominal.set_alpha(0.);
}

ControlSpotPanel::~ControlSpotPanel()
{
    previewRefresh_.disconnect();
    aiPreviewRefresh_.disconnect();

    // visibleGeometry
    for (auto i = EditSubscriber::visibleGeometry.begin(); i != EditSubscriber::visibleGeometry.end(); ++i) {
        delete *i;
    }

    // mouseOverGeometry
    for (auto i = EditSubscriber::mouseOverGeometry.begin(); i != EditSubscriber::mouseOverGeometry.end(); ++i) {
        delete *i;
    }
}

void ControlSpotPanel::setMaskDetailExpanded(bool expanded)
{
    const bool hasSelection =
        treeview_->get_selection()->count_selected_rows() > 0;
    maskDetailExpanded_ = hasSelection && expanded;
    maskRevealer_->set_visible(hasSelection);
    if (maskDetailExpanded_) {
        maskDetailBox_->show_all();
        maskBlendMode_->buttonGroup->show();

        if (maskType_->getSelected() == 1) {
            ctboxaiclass->show();
            aiMaskTolerance_->show();
        } else {
            ctboxaiclass->hide();
            aiMaskTolerance_->hide();
        }

        if (shape_->getSelected() == 3) {
            polyBox_->show();
            polyBox_->show_all_children();
            polyDrawBtn_->show();
            circrad_->hide();
            transit_->hide();
        } else {
            polyBox_->hide();
            polyDrawBtn_->hide();
            polyVertexIcon_->hide();
            polyVertexLabel_->hide();
            circrad_->show();
            transit_->show();
        }
    }
    maskRevealer_->set_reveal_child(maskDetailExpanded_);
}

void ControlSpotPanel::setMaskControlsSensitive(bool sensitive)
{
    shapeTypeRow_->set_sensitive(sensitive);
    circrad_->set_sensitive(sensitive);
    transit_->set_sensitive(sensitive);
    polyBox_->set_sensitive(sensitive);
    ctboxaiclass->set_sensitive(sensitive);
    aiMaskTolerance_->set_sensitive(sensitive);
    maskBlendMode_->buttonGroup->set_sensitive(sensitive);
}

void ControlSpotPanel::queueMaskPreviewRefresh()
{
    if (!previewRefresh_.connected()) {
        previewRefresh_ = Glib::signal_timeout().connect([this]() {
            treeview_->queue_draw();
            return false;
        }, 16);
    }
}

void ControlSpotPanel::startAIPreviewRefresh()
{
    aiPreviewRefresh_.disconnect();
    aiPreviewAttempts_ = 0;
    aiPreviewRefresh_ = Glib::signal_timeout().connect([this]() {
        treeview_->queue_draw();
        return ++aiPreviewAttempts_ < 20;
    }, 120);
}

bool ControlSpotPanel::onTreeviewMotion(GdkEventMotion* event)
{
    if (eyePinned_) {
        return false;
    }

    Gtk::TreeModel::Path path;
    if (!treeview_->get_path_at_pos(
            static_cast<int>(event->x), static_cast<int>(event->y), path)) {
        if (sidebarHoverActive_) {
            hoveredSpotIndex_ = -1;
            sidebarHoverActive_ = false;
            for (auto& row : treemodel_->children()) {
                row[spots_.mouseover] = false;
            }
            treeview_->queue_draw();
            if (controlPanelListener) {
                controlPanelListener->spotHovered(false, false, -1);
            }
        }
        return false;
    }

    const int hovered = !path.empty() ? path[0] : -1;
    if (hovered >= 0 && hovered != hoveredSpotIndex_) {
        hoveredSpotIndex_ = hovered;
        sidebarHoverActive_ = true;
        int index = 0;
        for (auto& row : treemodel_->children()) {
            row[spots_.mouseover] = index++ == hovered;
        }
        treeview_->queue_draw();
        if (controlPanelListener) {
            controlPanelListener->spotHovered(true, false, hovered);
        }
    }
    return false;
}

bool ControlSpotPanel::onTreeviewLeave(GdkEventCrossing* /*event*/)
{
    hoveredSpotIndex_ = -1;
    for (auto& row : treemodel_->children()) {
        row[spots_.mouseover] = false;
    }
    treeview_->queue_draw();
    // Only hide overlay on leave if not eye-pinned
    if (!eyePinned_ && sidebarHoverActive_) {
        sidebarHoverActive_ = false;
        if (controlPanelListener) {
            controlPanelListener->spotHovered(false, false, -1);
        }
    }
    return false;
}

bool ControlSpotPanel::isPointerOverTreeview() const
{
    // Use the toplevel window to get pointer position, then check if it's
    // within the treeview's screen allocation. This avoids bin_window
    // coordinate issues on Windows where get_device_position can return
    // stale values during redraws.
    auto toplevel = treeview_->get_toplevel();
    if (!toplevel) return false;

    auto gdkWin = toplevel->get_window();
    if (!gdkWin) return false;

    auto display = treeview_->get_display();
    auto seat = display->get_default_seat();
    auto device = seat->get_pointer();
    int px, py;
    Gdk::ModifierType mask;
    gdkWin->get_device_position(device, px, py, mask);

    // Translate treeview allocation to toplevel coordinates
    int wx, wy;
    treeview_->translate_coordinates(*dynamic_cast<Gtk::Widget*>(toplevel), 0, 0, wx, wy);

    const int w = treeview_->get_allocated_width();
    const int h = treeview_->get_allocated_height();

    return px >= wx && py >= wy && px < wx + w && py < wy + h;
}

void ControlSpotPanel::resetSidebarHover()
{
    sidebarHoverActive_ = false;
}

void ControlSpotPanel::setEditProvider(EditDataProvider* provider)
{
    EditSubscriber::setEditProvider(provider);
}

void ControlSpotPanel::render_details_toggle(
    Gtk::CellRenderer* cell, const Gtk::TreeModel::iterator& iter)
{
    const auto row = *iter;
    auto* text = static_cast<Gtk::CellRendererText*>(cell);
    text->property_text() = row[spots_.detailsExpanded]
        ? "\xe2\x96\xbe"
        : "\xe2\x96\xb8";
}

void ControlSpotPanel::render_preview(
    Gtk::CellRenderer* cell, const Gtk::TreeModel::iterator& iter)
{
    auto row = *iter;
    Gtk::CellRendererPixbuf *cp = static_cast<Gtk::CellRendererPixbuf *>(cell);

    const int shp    = row[spots_.shape];      // 0=ellipse, 1=rect, 2=gradient, 3=polygon
    const int maskT  = row[spots_.maskType];
    const int cls    = row[spots_.aiMaskClass];
    const float aiThreshold = static_cast<float>(row[spots_.aiMaskThreshold]);
    const double rX  = (double)row[spots_.locX]  / 3000.0;
    const double rXL = (double)row[spots_.locXL] / 3000.0;
    const double rY  = (double)row[spots_.locY]  / 3000.0;
    const double rYT = (double)row[spots_.locYT] / 3000.0;
    const double cxOff = (double)row[spots_.centerX] / 2000.0;
    const double cyOff = (double)row[spots_.centerY] / 2000.0;
    const double feather = std::max((double)row[spots_.transit] / 100.0, 0.05);
    const double angle = (double)row[spots_.gradangle] * RT_PI_180;

    // Polygon vertices (for shape==3)
    const std::vector<int> polyPts = row[spots_.polyMaskPoints];
    const double polyFeather = (double)row[spots_.polyMaskFeather];

    // AI class colors
    static const guint8 classColors[][3] = {
        {128, 128, 128}, {220,  80,  80}, { 80, 140, 220}, { 80, 180,  80},
        {200, 150,  60}, {160,  80, 200}, {220, 180,  50}, { 80, 200, 200},
    };

    // Heat colors for normal masks (warm orange/yellow)
    static const guint8 heatHi[3]  = {255, 170,  40};
    static const guint8 heatLo[3]  = { 60,  90, 160};

    const int W = 28, H = 18;
    auto pixbuf = Gdk::Pixbuf::create(Gdk::COLORSPACE_RGB, true, 8, W, H);
    pixbuf->fill(0x00000000);
    guint8* pixels = pixbuf->get_pixels();
    const int rowstride = pixbuf->get_rowstride();

    const double cx = std::max(0.2, std::min(0.8, 0.5 + cxOff * 0.35));
    const double cy = std::max(0.2, std::min(0.8, 0.5 + cyOff * 0.35));

    // For polygon shape, build normalized vertex list and compute bounding box
    std::vector<std::pair<double, double>> polyVerts;
    double polyMinX = 1e9, polyMinY = 1e9, polyMaxX = -1e9, polyMaxY = -1e9;
    if (shp == 3 && polyPts.size() >= 6) {
        // Get image size for normalization
        int imW = 1, imH = 1;
        EditDataProvider* prov = getEditProvider();
        if (prov) prov->getImageSize(imW, imH);
        if (imW < 1) imW = 1;
        if (imH < 1) imH = 1;

        polyVerts.reserve(polyPts.size() / 2);
        for (size_t i = 0; i + 1 < polyPts.size(); i += 2) {
            double vx = (double)polyPts[i] / (double)imW;
            double vy = (double)polyPts[i + 1] / (double)imH;
            polyVerts.emplace_back(vx, vy);
            polyMinX = std::min(polyMinX, vx);
            polyMinY = std::min(polyMinY, vy);
            polyMaxX = std::max(polyMaxX, vx);
            polyMaxY = std::max(polyMaxY, vy);
        }

        const double polyW = std::max(polyMaxX - polyMinX, 1e-6);
        const double polyH = std::max(polyMaxY - polyMinY, 1e-6);
        const double scale = 0.78 / std::max(polyW, polyH);
        const double sourceCx = (polyMinX + polyMaxX) * 0.5;
        const double sourceCy = (polyMinY + polyMaxY) * 0.5;
        for (auto& vertex : polyVerts) {
            vertex.first = 0.5 + (vertex.first - sourceCx) * scale;
            vertex.second = 0.5 + (vertex.second - sourceCy) * scale;
        }
    }

    // For AI masks, sample from the actual cached segmentation mask
#ifdef RT_AI_MASKING
    bool aiRendered = false;
    if (maskT == 1) {
        const int ci = std::min(std::max(cls, 0), 7);
        const AIMaskSnapshot snapshot = AIMaskCache::getInstance().getMaskSnapshot(
            static_cast<AISegClass>(ci));
        if (snapshot) {
            for (int py = 0; py < H; py++) {
                for (int px = 0; px < W; px++) {
                    const int my = std::min(
                        static_cast<int>((double)py / std::max(H - 1, 1) * (snapshot.height - 1)),
                        snapshot.height - 1);
                    const int mx = std::min(
                        static_cast<int>((double)px / std::max(W - 1, 1) * (snapshot.width - 1)),
                        snapshot.width - 1);
                    const float prob = (*snapshot.mask)[my][mx];
                    constexpr float transitionWidth = 0.21f;
                    const float strength = std::max(0.f, std::min(
                        1.f,
                        (prob - aiThreshold + transitionWidth * 0.5f) /
                            transitionWidth));
                    if (strength <= 0.001f) {
                        continue;
                    }

                    guint8* p = pixels + py * rowstride + px * 4;
                    p[0] = classColors[ci][0];
                    p[1] = classColors[ci][1];
                    p[2] = classColors[ci][2];
                    p[3] = static_cast<guint8>(strength * 200);
                }
            }
            aiRendered = true;
        }
        if (!aiRendered) {
            // No cached AI mask yet or lock busy — dim class-colored placeholder
            for (int py = 1; py < H - 1; py++) {
                for (int px = 1; px < W - 1; px++) {
                    guint8* p = pixels + py * rowstride + px * 4;
                    p[0] = classColors[ci][0];
                    p[1] = classColors[ci][1];
                    p[2] = classColors[ci][2];
                    p[3] = 60;
                }
            }
            aiRendered = true;
        }
    }
    if (!aiRendered)
#endif
    // Normal geometric mask rendering
    {
    for (int py = 0; py < H; py++) {
        for (int px = 0; px < W; px++) {
            double x = (double)px / (W - 1); // 0..1
            double y = (double)py / (H - 1);

            double strength = 0.0;

            if (shp == 3) {
                // Polygon: point-in-polygon test with feathering
                if (polyVerts.size() >= 3) {
                    // Ray casting PiP test
                    bool inside = false;
                    const int n = static_cast<int>(polyVerts.size());
                    for (int i = 0, j = n - 1; i < n; j = i++) {
                        const double xi = polyVerts[i].first, yi = polyVerts[i].second;
                        const double xj = polyVerts[j].first, yj = polyVerts[j].second;
                        if (((yi > y) != (yj > y)) &&
                            (x < (xj - xi) * (y - yi) / (yj - yi) + xi)) {
                            inside = !inside;
                        }
                    }

                    if (inside) {
                        strength = 1.0;
                    }

                    // Apply feathering near edges
                    if (polyFeather > 0.0) {
                        // Compute normalized feather distance
                        int imW = 1;
                        EditDataProvider* prov = getEditProvider();
                        if (prov) { int h; prov->getImageSize(imW, h); }
                        if (imW < 1) imW = 1;
                        double fNorm = polyFeather / (double)imW;

                        // Distance to nearest polygon edge
                        double minDist = 1e9;
                        for (int i = 0, j = n - 1; i < n; j = i++) {
                            double ax = polyVerts[j].first, ay = polyVerts[j].second;
                            double bx = polyVerts[i].first, by = polyVerts[i].second;
                            double dx = bx - ax, dy = by - ay;
                            double lenSq = dx * dx + dy * dy;
                            double t = (lenSq < 1e-12) ? 0.0 :
                                std::max(0.0, std::min(1.0, ((x - ax) * dx + (y - ay) * dy) / lenSq));
                            double ex = x - (ax + t * dx);
                            double ey = y - (ay + t * dy);
                            minDist = std::min(minDist, std::sqrt(ex * ex + ey * ey));
                        }

                        if (inside && minDist < fNorm) {
                            strength = minDist / fNorm;
                        } else if (!inside && minDist < fNorm) {
                            strength = 1.0 - minDist / fNorm;
                        }
                    }
                }
            } else {
                double dist = 0.0;

                if (shp == 2) {
                    // Gradient: linear along angle direction from center
                    double dx = x - cx;
                    double dy = y - cy;
                    dist = std::abs(dx * std::sin(angle) - dy * std::cos(angle));
                    dist /= std::max(feather, 0.05);
                } else {
                    // Ellipse or Rectangle
                    const double maxExtent = std::max(
                        std::max(rX, rXL), std::max(rY, rYT));
                    double previewScale = 1.0;
                    if (maxExtent > 0.0) {
                        previewScale = std::max(1.0, 0.24 / maxExtent);
                        previewScale = std::min(previewScale, 0.42 / maxExtent);
                    }
                    double extR = std::max(rX * previewScale, 0.01);
                    double extL = std::max(rXL * previewScale, 0.01);
                    double extB = std::max(rY * previewScale, 0.01);
                    double extT = std::max(rYT * previewScale, 0.01);

                    double dx = x - cx;
                    double dy = y - cy;
                    double nx = dx / (dx >= 0 ? extR : extL);
                    double ny = dy / (dy >= 0 ? extB : extT);

                    if (shp == 0) {
                        dist = std::sqrt(nx * nx + ny * ny);
                    } else {
                        dist = std::max(std::abs(nx), std::abs(ny));
                    }
                }

                // Compute strength with feathering
                if (dist <= 1.0) {
                    strength = 1.0;
                } else if (dist <= 1.0 + feather) {
                    strength = 1.0 - (dist - 1.0) / feather;
                } else {
                    strength = 0.0;
                }
            }

            if (strength <= 0.001) continue;

            guint8* p = pixels + py * rowstride + px * 4;

            // Normal mask: heat gradient (lo→hi by strength)
            double s = strength;
            p[0] = (guint8)(heatLo[0] + s * (heatHi[0] - heatLo[0]));
            p[1] = (guint8)(heatLo[1] + s * (heatHi[1] - heatLo[1]));
            p[2] = (guint8)(heatLo[2] + s * (heatHi[2] - heatLo[2]));
            p[3] = (guint8)(strength * 200);
        }
    }
    } // end normal geometric mask rendering

    // Subtle 1px border for definition
    for (int px = 0; px < W; px++) {
        for (int py : {0, H - 1}) {
            guint8* p = pixels + py * rowstride + px * 4;
            p[0] = 80; p[1] = 85; p[2] = 95; p[3] = 100;
        }
    }
    for (int py = 0; py < H; py++) {
        for (int px : {0, W - 1}) {
            guint8* p = pixels + py * rowstride + px * 4;
            p[0] = 80; p[1] = 85; p[2] = 95; p[3] = 100;
        }
    }

    cp->property_pixbuf() = pixbuf;
}

void ControlSpotPanel::render_name(
    Gtk::CellRenderer* cell, const Gtk::TreeModel::iterator& iter)
{
    auto row = *iter;
    Gtk::CellRendererText *ct = static_cast<Gtk::CellRendererText *>(cell);

    // Render cell text with larger font to match mask dropdown label
    ct->property_markup() = "<span size=\"large\">" +
        Glib::Markup::escape_text(Glib::ustring(row[spots_.name])) + "</span>";

    // Render cell background color
    if (row[spots_.mouseover]) {
        ct->property_background_rgba() = colorMouseovertext;
    } else {
        ct->property_background_rgba() = colorNominal;
    }
}

void ControlSpotPanel::render_isvisible(
    Gtk::CellRenderer* cell, const Gtk::TreeModel::iterator& iter)
{
    auto row = *iter;
    Gtk::CellRendererPixbuf *cp = static_cast<Gtk::CellRendererPixbuf *>(cell);

    // Render eye icon based on visibility
    if (row[spots_.isvisible]) {
        cp->property_icon_name() = "eye-open";
    } else {
        cp->property_icon_name() = "eye-closed";
    }
}

void ControlSpotPanel::on_button_add()
{

    if (!listener) {
        return;
    }

    // Raise event
    nbSpotChanged_ = true;
    selSpotChanged_ = true;
    eventType = SpotCreation;
    listener->panelChanged(EvLocallabSpotCreated, "-");
}

void ControlSpotPanel::on_mask_shape_selected(int shape)
{
    pendingShape_ = std::max(0, std::min(shape, 2));
    on_button_add();
}

void ControlSpotPanel::on_ai_mask_selected(int classIndex)
{
    if (!listener) {
        return;
    }

    pendingAIClass_ = classIndex;
    nbSpotChanged_ = true;
    selSpotChanged_ = true;
    eventType = SpotCreationAI;
    listener->panelChanged(EvLocallabSpotCreated, "-");

    startAIPreviewRefresh();
}

void ControlSpotPanel::on_button_delete()
{
    // printf("on_button_delete\n");

    if (!listener) {
        return;
    }

    // Raise event
    const int selIndex = getSelectedSpot();

    if (selIndex == -1) { // No selected spot to remove
        return;
    }

    nbSpotChanged_ = true;
    selSpotChanged_ = true;
    eventType = SpotDeletion;
    const std::unique_ptr<SpotRow> delSpotRow = getSpot(selIndex);
    listener->panelChanged(EvLocallabSpotDeleted, delSpotRow->name);
}

void ControlSpotPanel::on_button_duplicate()
{
    // printf("on_button_duplicate\n");

    if (!listener) {
        return;
    }

    // Raise event
    const int selIndex = getSelectedSpot();

    if (selIndex == -1) { // No selected spot to duplicate
        return;
    }

    nbSpotChanged_ = true;
    selSpotChanged_ = true;
    eventType = SpotDuplication;
    const std::unique_ptr<SpotRow> duplSpotRow = getSpot(selIndex);
    listener->panelChanged(EvLocallabSpotCreated, M("TP_LOCALLAB_EV_DUPL") + " "
                           + duplSpotRow->name);
}

void ControlSpotPanel::on_button_rename()
{
    // printf("on_button_rename\n");

    if (!listener) {
        return;
    }

    // Get actual control spot name
    const auto s = treeview_->get_selection();

    if (!s->count_selected_rows()) {
        return;
    }

    const auto iter = s->get_selected();
    const Gtk::TreeModel::Row row = *iter;
    const Glib::ustring actualname = row[spots_.name];

    // Launch windows to update spot name
    RenameDialog d(actualname,
                   static_cast<Gtk::Window &>(*get_toplevel()));
    int status = d.run();

    // Update actual name and raise event
    if (status == RenameDialog::OkButton) {
        const Glib::ustring newname = d.get_new_name();

        if (newname != actualname) { // Event is only raised if name is updated
            nameChanged_ = true;
            row[spots_.name] = newname;
            row[spots_.nameAutomatic] = false;
            treeview_->columns_autosize();
            listener->panelChanged(EvLocallabSpotName, newname);
            if (controlPanelListener) {
                controlPanelListener->spotNameChanged(newname);
            }
        }
    }
}

bool ControlSpotPanel::isAutomaticMaskName(const Glib::ustring& name) const
{
    const char* automaticNameKeys[] = {
        "TP_LOCALLAB_MASK_NAME_ELLIPSE", "TP_LOCALLAB_MASK_NAME_RECTANGLE",
        "TP_LOCALLAB_MASK_NAME_GRADIENT", "TP_LOCALLAB_MASK_NAME_LASSO",
        "TP_LOCALLAB_MASK_NAME_BACKGROUND", "TP_LOCALLAB_MASK_NAME_PERSON",
        "TP_LOCALLAB_MASK_NAME_SKY", "TP_LOCALLAB_MASK_NAME_VEGETATION",
        "TP_LOCALLAB_MASK_NAME_BUILDING", "TP_LOCALLAB_MASK_NAME_VEHICLE",
        "TP_LOCALLAB_MASK_NAME_ANIMAL", "TP_LOCALLAB_MASK_NAME_FOREGROUND"
    };
    for (const char* key : automaticNameKeys) {
        if (name == M(key)) {
            return true;
        }
    }

    // Profiles created in another locale or an older build retain their
    // original generated English name.
    static const char* legacyAutomaticNames[] = {
        "Ellipse Mask", "Rectangle Mask", "Gradient Mask", "Lasso Mask",
        "Background Mask", "Person Mask", "Sky Mask", "Vegetation Mask",
        "Building Mask", "Vehicle Mask", "Animal Mask", "Foreground Mask"
    };
    for (const char* automaticName : legacyAutomaticNames) {
        if (name == automaticName) {
            return true;
        }
    }

    const std::string raw = name.raw();
    if (raw.rfind("Mask ", 0) != 0 || raw.size() <= 5) {
        return false;
    }
    for (size_t i = 5; i < raw.size(); ++i) {
        if (raw[i] < '0' || raw[i] > '9') {
            return false;
        }
    }
    return true;
}

void ControlSpotPanel::updateAutomaticMaskName(Gtk::TreeModel::Row& row)
{
    if (!row[spots_.nameAutomatic]) {
        return;
    }

    static const char* shapeNameKeys[] = {
        "TP_LOCALLAB_MASK_NAME_ELLIPSE", "TP_LOCALLAB_MASK_NAME_RECTANGLE",
        "TP_LOCALLAB_MASK_NAME_GRADIENT", "TP_LOCALLAB_MASK_NAME_LASSO"
    };
    static const char* aiNameKeys[] = {
        "TP_LOCALLAB_MASK_NAME_BACKGROUND", "TP_LOCALLAB_MASK_NAME_PERSON",
        "TP_LOCALLAB_MASK_NAME_SKY", "TP_LOCALLAB_MASK_NAME_VEGETATION",
        "TP_LOCALLAB_MASK_NAME_BUILDING", "TP_LOCALLAB_MASK_NAME_VEHICLE",
        "TP_LOCALLAB_MASK_NAME_ANIMAL", "TP_LOCALLAB_MASK_NAME_FOREGROUND"
    };

    const int type = row[spots_.maskType];
    const int index = type == 1
        ? rtengine::LIM(static_cast<int>(row[spots_.aiMaskClass]), 0, 7)
        : rtengine::LIM(static_cast<int>(row[spots_.shape]), 0, 3);
    const Glib::ustring newName = M(type == 1 ? aiNameKeys[index] : shapeNameKeys[index]);
    if (row[spots_.name] == newName) {
        return;
    }

    row[spots_.name] = newName;
    nameChanged_ = true;
    treeview_->columns_autosize();
    if (controlPanelListener) {
        controlPanelListener->spotNameChanged(newName);
    }
}

bool ControlSpotPanel::on_button_visibility(GdkEventButton* event)
{
    // printf("on_button_visibility\n");

    if (!listener) {
        return true;
    }

    // Get selected control spot
    const auto s = treeview_->get_selection();

    if (!s->count_selected_rows()) {
        return true;
    }

    const auto iter = s->get_selected();
    const Gtk::TreeModel::Row row = *iter;

    const int ctrl = event->state & GDK_CONTROL_MASK;

    if (event->button == 1) { // Left click on button
        if (ctrl) { // Ctrl+click case: all spots are shown/hidden
            // Get visibility of selected spot
            const bool selVisibility = row[spots_.isvisible];

            // Update visibility of all spot
            const Gtk::TreeModel::Children children = treemodel_->children();

            for (auto i = children.begin(); i != children.end(); i++) {
                Gtk::TreeModel::Row r = *i;
                r[spots_.isvisible] = !selVisibility;
                updateControlSpotCurve(r);
            }

            // Raise event
            visibilityChanged_ = true;
            eventType = SpotAllVisibilityChanged;

            if (!selVisibility) {
                listener->panelChanged(EvLocallabSpotVisibility, M("TP_LOCALLAB_EV_VIS_ALL"));
            } else {
                listener->panelChanged(EvLocallabSpotVisibility, M("TP_LOCALLAB_EV_NVIS_ALL"));
            }

            return true;
        } else { // Click case: only selected spot is shown/hidden
            // Update visibility for selected spot only
            row[spots_.isvisible] = !row[spots_.isvisible];
            updateControlSpotCurve(row);

            // Raise event
            visibilityChanged_ = true;
            const std::unique_ptr<SpotRow> spotRow = getSpot(getSelectedSpot());

            if (row[spots_.isvisible]) {
                listener->panelChanged(EvLocallabSpotVisibility, M("TP_LOCALLAB_EV_VIS") + " (" + spotRow->name + ")");
            } else {
                listener->panelChanged(EvLocallabSpotVisibility, M("TP_LOCALLAB_EV_NVIS") + " (" + spotRow->name + ")");
            }

            return true;
        }
    }

    return false;
}


bool ControlSpotPanel::blockTreeviewSearch(GdkEventKey* event)
{
    // printf("blockTreeviewSearch\n");

    if (event->state & Gdk::CONTROL_MASK) { // Ctrl
        if (event->keyval == GDK_KEY_f || event->keyval == GDK_KEY_F) {
            // No action is performed to avoid activating treeview search
            return true;
        }
    }

    // Otherwise key action is transferred to treeview widget
    return false;
}

bool ControlSpotPanel::onSpotSelectionEvent(GdkEventButton* event)
{
    if (event->state & Gdk::CONTROL_MASK) { // Ctrl
        // No action is performed to avoid a situation where no spot is selected
        return true;
    }

    // Right-click: show context menu
    if (event->button == 3) {
        Gtk::TreeModel::Path path;
        if (treeview_->get_path_at_pos(static_cast<int>(event->x), static_cast<int>(event->y), path)) {
            treeview_->get_selection()->select(path);
            const auto iter = treemodel_->get_iter(path);
            if (iter) {
                const int mode = rtengine::LIM(
                    static_cast<int>((*iter)[spots_.maskBlendMode]), 0, 4);
                updatingBlendModeMenu_ = true;
                blendModeMenuItems_[mode]->set_active(true);
                updatingBlendModeMenu_ = false;
            }
            contextMenu_->popup(event->button, event->time);
            return true;
        }
        return false;
    }

    // Left-click on visibility column: toggle pinned mask overlay
    if (event->button == 1) {
        Gtk::TreeModel::Path path;
        Gtk::TreeViewColumn* column = nullptr;
        int cell_x, cell_y;

        if (treeview_->get_path_at_pos(static_cast<int>(event->x), static_cast<int>(event->y), path, column, cell_x, cell_y)) {
            auto iter = treemodel_->get_iter(path);
            if (column == treeview_->get_column(0) && iter) {
                Gtk::TreeModel::Row clickedRow = *iter;
                const bool expand = !clickedRow[spots_.detailsExpanded];
                for (auto& row : treemodel_->children()) {
                    row[spots_.detailsExpanded] = false;
                }
                clickedRow[spots_.detailsExpanded] = expand;
                treeview_->get_selection()->select(path);
                setMaskControlsSensitive(true);
                setMaskDetailExpanded(expand);
                treeview_->queue_draw();
                return true;
            }

            if (column == treeview_->get_column(3)) {
                if (iter) {
                    // Select the clicked row's spot
                    treeview_->get_selection()->select(path);

                    // Toggle pinned mask overlay
                    eyePinned_ = !eyePinned_;
                    sidebarHoverActive_ = eyePinned_;

                    // Update eye icon and geometry visibility
                    Gtk::TreeModel::Row row = *iter;
                    row[spots_.isvisible] = eyePinned_;
                    updateControlSpotCurve(row);

                    if (controlPanelListener) {
                        // forceRedraw=true ensures canvas repaints to show/hide geometry
                        controlPanelListener->spotHovered(
                            eyePinned_, true, eyePinned_ ? path[0] : -1);
                    }

                    return true;
                }
            }

            // Selecting a mask by its preview/name keeps settings collapsed;
            // the row chevron is the only control that opens them.
            for (auto& row : treemodel_->children()) {
                row[spots_.detailsExpanded] = false;
            }
            setMaskDetailExpanded(false);
            treeview_->queue_draw();
        }
    }

    // Otherwise selection action is transferred to treeview widget
    return false;
}

void ControlSpotPanel::load_ControlSpot_param()
{
    // printf("load_ControlSpot_param\n");

    // Get selected control spot
    const auto s = treeview_->get_selection();

    if (!s->count_selected_rows()) {
        return;
    }

    const auto iter = s->get_selected();
    const Gtk::TreeModel::Row row = *iter;

    // Load param in selected control spot
    prevMethod_->set_active(row[spots_.prevMethod]);
    shape_->setSelected(row[spots_.shape]);
    spotMethod_->set_active(row[spots_.spotMethod]);
    sensiexclu_->setValue((double)row[spots_.sensiexclu]);
    structexclu_->setValue((double)row[spots_.structexclu]);
    shapeMethod_->set_active(row[spots_.shapeMethod]);
    locX_->setValue((double)row[spots_.locX]);
    locXL_->setValue((double)row[spots_.locXL]);
    locY_->setValue((double)row[spots_.locY]);
    locYT_->setValue((double)row[spots_.locYT]);
    centerX_->setValue((double)row[spots_.centerX]);
    centerY_->setValue((double)row[spots_.centerY]);
    circrad_->setValue((double)row[spots_.circrad]);
    qualityMethod_->set_active(row[spots_.qualityMethod]);
    transit_->setValue((double)row[spots_.transit]);
    transitweak_->setValue((double)row[spots_.transitweak]);
    transitgrad_->setValue((double)row[spots_.transitgrad]);
    gradangle_->setValue((double)row[spots_.gradangle]);
    feather_->setValue((double)row[spots_.feather]);
    struc_->setValue((double)row[spots_.struc]);
    thresh_->setValue((double)row[spots_.thresh]);

    iter_->setValue((double)row[spots_.iter]);    
    balan_->setValue((double)row[spots_.balan]);
    balanh_->setValue((double)row[spots_.balanh]);
    colorde_->setValue((double)row[spots_.colorde]);

    
    colorscope_->setValue((double)row[spots_.colorscope]);
    avoidrad_->setValue((double)row[spots_.avoidrad]);
    hishow_->set_active(row[spots_.hishow]);
    activ_->set_active(row[spots_.activ]);
    avoidneg_->set_active(row[spots_.avoidneg]);
    blwh_->set_active(row[spots_.blwh]);
    recurs_->set_active(row[spots_.recurs]);
   // laplac_->set_active(row[spots_.laplac]);
    laplac_->set_active(true);
    deltae_->set_active(row[spots_.deltae]);
    scopemask_->setValue((double)row[spots_.scopemask]);
    denoichmask_->setValue(row[spots_.denoichmask]);
    shortc_->set_active(row[spots_.shortc]);
    lumask_->setValue((double)row[spots_.lumask]);
    //savrest_->set_active(row[spots_.savrest]);
    //complexMethod_->set_active(row[spots_.complexMethod]);
    wavMethod_->set_active(row[spots_.wavMethod]);
	avoidgamutMethod_->set_active(row[spots_.avoidgamutMethod]);
    maskType_->setSelected(row[spots_.maskType]);
    aiMaskClass_->setSelected(row[spots_.aiMaskClass]);
    aiMaskTolerance_->setValue(
        100.0 * (1.0 - static_cast<double>(row[spots_.aiMaskThreshold])));
    maskBlendModeConn_.block(true);
    maskBlendMode_->setSelected(row[spots_.maskBlendMode]);
    maskBlendModeConn_.block(false);
    ctboxaiclass->set_visible(row[spots_.maskType] == 1);
    aiMaskTolerance_->set_visible(row[spots_.maskType] == 1);

    // Show/hide controls based on shape
    if (shape_->getSelected() == 2) { // Gradient
        gradangle_->show();
        locX_->hide();
        locXL_->hide();
        locY_->hide();
        locYT_->hide();
        circrad_->show();
        transit_->show();
        polyBox_->hide();
        polyDrawBtn_->hide();
        polyVertexIcon_->hide();
        polyVertexLabel_->hide();
    } else if (shape_->getSelected() == 3) { // Polygon (Lasso)
        gradangle_->hide();
        locX_->hide();
        locXL_->hide();
        locY_->hide();
        locYT_->hide();
        circrad_->hide();
        transit_->hide();
        polyBox_->show();
        polyBox_->show_all_children();
        polyDrawBtn_->show();
        const std::vector<int> pts = row[spots_.polyMaskPoints];
        const int nVerts = static_cast<int>(pts.size()) / 2;
        polyVertexLabel_->set_text(Glib::ustring::compose("%1", nVerts));
        polyFeather_->setValue(row[spots_.polyMaskFeather]);
        polySnapTol_->setValue(row[spots_.polyMaskSnapTolerance]);
        polyLegLen_->setValue(row[spots_.polyMaskLegLength]);
        // Auto-activate draw mode
        if (!polyDrawing_) {
            polyDrawConn_.block(true);
            polyDrawBtn_->set_active(true);
            polyDrawConn_.block(false);
            polyDrawing_ = true;
            polyTempPoints_.clear();
        }
    } else { // Ellipse or Rectangle
        gradangle_->hide();
        circrad_->show();
        transit_->show();
        locX_->show();
        locXL_->show();
        locY_->show();
        locYT_->show();
        polyBox_->hide();
        polyDrawBtn_->hide();
        polyVertexIcon_->hide();
        polyVertexLabel_->hide();
    }

    setMaskControlsSensitive(true);
    setMaskDetailExpanded(row[spots_.detailsExpanded]);
}

void ControlSpotPanel::controlspotChanged()
{

    if (!listener) {
        return;
    }

    // Reset hover so overlay re-triggers for newly selected spot
    sidebarHoverActive_ = false;

    // Raise event
    const int selIndex = getSelectedSpot();

    if (selIndex == -1) { // No selected spot
        setMaskControlsSensitive(false);
        setMaskDetailExpanded(false);
        return;
    }

    selSpotChanged_ = true;
    eventType = SpotSelection;
    const std::unique_ptr<SpotRow> spotRow = getSpot(selIndex);

    setMaskControlsSensitive(true);
    const auto selected = treeview_->get_selection()->get_selected();
    setMaskDetailExpanded(selected && (*selected)[spots_.detailsExpanded]);
    // Image area shall be regenerated if mask or deltaE preview was active when switching spot
    if (maskPrevActive || preview_->get_active()) {
        listener->panelChanged(EvLocallabSpotSelectedWithMask, spotRow->name);
    } else {
        listener->panelChanged(EvLocallabSpotSelected, spotRow->name);
    }
}

void ControlSpotPanel::shapeChanged(int /*index*/)
{
    // Get selected control spot
    const auto s = treeview_->get_selection();

    if (!s->count_selected_rows()) {
        return;
    }

    const auto iter = s->get_selected();
    Gtk::TreeModel::Row row = *iter;

    const int prevShape = row[spots_.shape];
    row[spots_.shape] = shape_->getSelected();
    updateAutomaticMaskName(row);

    // When switching to Gradient or Polygon, save current dimensions and expand bounding box
    if ((shape_->getSelected() == 2 || shape_->getSelected() == 3) && prevShape != 2 && prevShape != 3) {
        savedLocX_  = (double)row[spots_.locX];
        savedLocXL_ = (double)row[spots_.locXL];
        savedLocY_  = (double)row[spots_.locY];
        savedLocYT_ = (double)row[spots_.locYT];
        savedTransit_ = row[spots_.transit];
        hasSavedDims_ = true;

        disableParamlistener(true);
        locX_->setValue(3000.);
        row[spots_.locX] = locX_->getIntValue();
        locXL_->setValue(3000.);
        row[spots_.locXL] = locXL_->getIntValue();
        locY_->setValue(3000.);
        row[spots_.locY] = locY_->getIntValue();
        locYT_->setValue(3000.);
        row[spots_.locYT] = locYT_->getIntValue();
        transit_->setValue(100.);
        row[spots_.transit] = transit_->getValue();
        disableParamlistener(false);
    }
    // When switching back from Gradient/Polygon, restore saved dimensions
    else if ((prevShape == 2 || prevShape == 3) && shape_->getSelected() != 2 && shape_->getSelected() != 3 && hasSavedDims_) {
        disableParamlistener(true);
        locX_->setValue(savedLocX_);
        row[spots_.locX] = locX_->getIntValue();
        locXL_->setValue(savedLocXL_);
        row[spots_.locXL] = locXL_->getIntValue();
        locY_->setValue(savedLocY_);
        row[spots_.locY] = locY_->getIntValue();
        locYT_->setValue(savedLocYT_);
        row[spots_.locYT] = locYT_->getIntValue();
        transit_->setValue(savedTransit_);
        row[spots_.transit] = transit_->getValue();
        disableParamlistener(false);
        hasSavedDims_ = false;
    }

    // Cancel polygon drawing if switching away from polygon
    if (shape_->getSelected() != 3 && (polyDrawing_ || polyDragging_)) {
        polyDrawing_ = false;
        polyDragging_ = false;
        polyTempPoints_.clear();
        polyDrawConn_.block(true);
        polyDrawBtn_->set_active(false);
        polyDrawConn_.block(false);
    }

    updateControlSpotCurve(row);

    // Show/hide controls based on shape
    if (shape_->getSelected() == 2) { // Gradient
        gradangle_->show();
        locX_->hide();
        locXL_->hide();
        locY_->hide();
        locYT_->hide();
        circrad_->show();
        transit_->show();
        polyBox_->hide();
        polyDrawBtn_->hide();
        polyVertexIcon_->hide();
        polyVertexLabel_->hide();
    } else if (shape_->getSelected() == 3) { // Polygon (Lasso)
        gradangle_->hide();
        locX_->hide();
        locXL_->hide();
        locY_->hide();
        locYT_->hide();
        circrad_->hide();
        transit_->hide();
        polyBox_->show();
        polyBox_->show_all_children();
        polyDrawBtn_->show();
        // Update vertex count label
        const std::vector<int> pts = row[spots_.polyMaskPoints];
        const int nVerts = static_cast<int>(pts.size()) / 2;
        polyVertexLabel_->set_text(Glib::ustring::compose("%1", nVerts));
        polyFeather_->setValue(row[spots_.polyMaskFeather]);
        polySnapTol_->setValue(row[spots_.polyMaskSnapTolerance]);
        polyLegLen_->setValue(row[spots_.polyMaskLegLength]);
        // Auto-activate draw mode when switching to Lasso
        if (!polyDrawing_) {
            polyDrawConn_.block(true);
            polyDrawBtn_->set_active(true);
            polyDrawConn_.block(false);
            polyDrawing_ = true;
            polyTempPoints_.clear();
        }
    } else { // Ellipse or Rectangle
        gradangle_->hide();
        circrad_->show();
        transit_->show();
        locX_->show();
        locXL_->show();
        locY_->show();
        locYT_->show();
        polyBox_->hide();
        polyDrawBtn_->hide();
        polyVertexIcon_->hide();
        polyVertexLabel_->hide();
    }

    // Raise event
    if (listener) {
        const char* shapeKeys[] = {"TP_LOCALLAB_ELI", "TP_LOCALLAB_RECT", "TP_LOCALLAB_GRAD", "TP_LOCALLAB_POLY"};
        const int sel = shape_->getSelected();
        listener->panelChanged(EvLocallabSpotShape, sel >= 0 && sel < 4 ? M(shapeKeys[sel]) : "");
    }
}

void ControlSpotPanel::polyDrawClicked()
{
    if (polyDrawBtn_->get_active()) {
        // Enter polygon drawing mode
        polyDrawing_ = true;
        polyDragging_ = false;
        polyTempPoints_.clear();
    } else {
        // Exit drawing mode
        polyDrawing_ = false;
        polyDragging_ = false;

        const auto s = treeview_->get_selection();
        if (s->count_selected_rows() && polyTempPoints_.size() >= 3) {
            const auto iter = s->get_selected();
            Gtk::TreeModel::Row row = *iter;

            std::vector<int> pts;
            pts.reserve(polyTempPoints_.size() * 2);
            for (const auto& p : polyTempPoints_) {
                pts.push_back(p.x);
                pts.push_back(p.y);
            }
            row[spots_.polyMaskPoints] = pts;
            polyVertexLabel_->set_text(Glib::ustring::compose("%1", polyTempPoints_.size()));

            updateControlSpotCurve(row);

            if (listener) {
                listener->panelChanged(EvLocallabSpotShape, M("TP_LOCALLAB_POLY"));
            }
        }
        polyTempPoints_.clear();
    }
}

void ControlSpotPanel::prevMethodChanged()
{
    // printf("prevMethodChanged\n");

    // Get selected control spot
    const auto s = treeview_->get_selection();

    if (!s->count_selected_rows()) {
        return;
    }

    const auto iter = s->get_selected();
    Gtk::TreeModel::Row row = *iter;

    row[spots_.prevMethod] = prevMethod_->get_active_row_number();
/*
    // Update Control Spot GUI according to spotMethod_ combobox state (to be compliant with updateParamVisibility function)
    if (multiImage && prevMethod_->get_active_text() == M("GENERAL_UNCHANGED")) {
        expTransGrad_->show();
        expShapeDetect_->show();
        expSpecCases_->show();
        expMaskMerge_->show();
        circrad_->show();
        ctboxshape->show();
    } else if (prevMethod_->get_active_row_number() == 0) { // Normal case
        expTransGrad_->hide();
        expShapeDetect_->hide();
        expSpecCases_->hide();
        expMaskMerge_->hide();
        circrad_->hide();
        ctboxshape->hide();
        shapeMethod_->set_active(0);

    } else { // Excluding case
        expTransGrad_->show();
        expShapeDetect_->show();
        expSpecCases_->show();
        expMaskMerge_->show();
        circrad_->show();
        ctboxshape->show();
    }
*/
    // Raise event
    if (listener) {
//        listener->panelChanged(EvLocallabSpotprevMethod, prevMethod_->get_active_text());
    }
}



void ControlSpotPanel::spotMethodChanged()
{
    //01 2024 take into account new problems linked to Global spotmethod
    // Get selected control spot
    const auto s = treeview_->get_selection();

    if (!s->count_selected_rows()) {
        return;
    }

    const auto iter = s->get_selected();
    Gtk::TreeModel::Row row = *iter;

    const int oldSpotMethod = row[spots_.spotMethod];
    row[spots_.spotMethod] = spotMethod_->get_active_row_number();
    //ctboxspotmethod->show();
    hishow_->show();
    ctboxshape->show();
    artifBox2->show();
    colorscope_->hide();

    // Update Control Spot GUI according to spotMethod_ combobox state (to be compliant with updateParamVisibility function)
    if (multiImage && spotMethod_->get_active_text() == M("GENERAL_UNCHANGED")) {
        excluFrame->show();

    } else if (spotMethod_->get_active_row_number() == 0) { // Normal case
        excluFrame->hide();
        // Reset spot shape only if previous spotMethod is Full image or Global
        if (oldSpotMethod == 2  || oldSpotMethod == 3) {
            disableParamlistener(true);
            locX_->setValue(150.);
            row[spots_.locX] = locX_->getIntValue();
            locXL_->setValue(150.);
            row[spots_.locXL] = locXL_->getIntValue();
            locY_->setValue(150.);
            row[spots_.locY] = locY_->getIntValue();
            locYT_->setValue(150.);
            row[spots_.locYT] = locYT_->getIntValue();
            shape_->setSelected(0);
            row[spots_.shape] = shape_->getSelected();
            transit_->setValue(60.);
            row[spots_.transit] = transit_->getValue();
            disableParamlistener(false);
            updateControlSpotCurve(row);
        }
    } else if (spotMethod_->get_active_row_number() == 1) { // Excluding case
        excluFrame->show();

        // Reset spot shape only if previous spotMethod is Full image or Global
        if (oldSpotMethod == 2 || oldSpotMethod == 3) {
            disableParamlistener(true);
            locX_->setValue(150.);
            row[spots_.locX] = locX_->getIntValue();
            locXL_->setValue(150.);
            row[spots_.locXL] = locXL_->getIntValue();
            locY_->setValue(150.);
            row[spots_.locY] = locY_->getIntValue();
            locYT_->setValue(150.);
            row[spots_.locYT] = locYT_->getIntValue();
            shape_->setSelected(0);
            row[spots_.shape] = shape_->getSelected();
            transit_->setValue(60.);
            row[spots_.transit] = transit_->getValue();
            disableParamlistener(false);
            updateControlSpotCurve(row);
        }
    } else if (spotMethod_->get_active_row_number() == 2  || spotMethod_->get_active_row_number() == 3) { // Full image or Global case
        excluFrame->hide();
        shape_->setSelected(0);

        locX_->setValue(3000.);
        row[spots_.locX] = locX_->getIntValue();
        locXL_->setValue(3000.);
        row[spots_.locXL] = locXL_->getIntValue();
        locY_->setValue(3000.);
        row[spots_.locY] = locY_->getIntValue();
        locYT_->setValue(3000.);
        row[spots_.locYT] = locYT_->getIntValue();
        shape_->setSelected(1);
        row[spots_.shape] = shape_->getSelected();
        transit_->setValue(100.);
        row[spots_.transit] = transit_->getValue();
        
        if(spotMethod_->get_active_row_number() == 3) { //global
            ctboxshape->hide();
            artifBox2->hide();
            hishow_->hide();
            expTransGrad_->hide();
            expShapeDetect_->hide();
            expSpecCases_->hide();
            expMaskMerge_->hide();
            circrad_->hide();
            ctboxshape->hide();
        } else {
            ctboxshape->show();
            circrad_->show();    
            artifBox2->show();
            colorscope_->hide();
           
            hishow_->show();
            if(hishow_->get_active()) {
                expTransGrad_->show();
                expShapeDetect_->show();
                expSpecCases_->show();
                expMaskMerge_->show();
            }
        }
    }

    // Raise event
    if (listener) {
        listener->panelChanged(EvLocallabSpotSpotMethod, spotMethod_->get_active_text());
    }
}

void ControlSpotPanel::avoidgamutMethodChanged()
{

    // Get selected control spot
    const auto s = treeview_->get_selection();

    if (!s->count_selected_rows()) {
        return;
    }
    const int meth = avoidgamutMethod_->get_active_row_number();
    avoidrad_->show();

    if(meth == 2 || meth == 3 || meth == 4) {
        avoidrad_->hide();
    }

    const auto iter = s->get_selected();
    Gtk::TreeModel::Row row = *iter;

    row[spots_.avoidgamutMethod] = avoidgamutMethod_->get_active_row_number();

    // Raise event
    if (listener) {
        listener->panelChanged(EvLocallabavoidgamutMethod, avoidgamutMethod_->get_active_text());
    }

}

void ControlSpotPanel::shapeMethodChanged()
{
    // printf("shapeMethodChanged\n");

    const int method = shapeMethod_->get_active_row_number();

    // Get selected control spot
    const auto s = treeview_->get_selection();

    if (!s->count_selected_rows()) {
        return;
    }

    const auto iter = s->get_selected();
    Gtk::TreeModel::Row row = *iter;

    if (!batchMode && (method == 1 || method == 3)) { // Symmetrical cases
        disableParamlistener(true);
        locXL_->setValue(locX_->getValue());
        locYT_->setValue(locY_->getValue());
        disableParamlistener(false);

        row[spots_.shapeMethod] = shapeMethod_->get_active_row_number();
        row[spots_.locXL] = locX_->getIntValue();
        row[spots_.locYT] = locY_->getIntValue();

        updateControlSpotCurve(row);
    } else { // In batch mode, sliders are always independent
        row[spots_.shapeMethod] = shapeMethod_->get_active_row_number();
    }

    // Update Control Spot GUI according to shapeMethod_ combobox state (to be compliant with updateParamVisibility function)
    if (!batchMode) {
        if (method == 1 || method == 3) { // Symmetrical cases
            locXL_->hide();
            locYT_->hide();

            if (method == 1) { // 1 = Symmetrical (mouse)
                locX_->hide();
                locY_->hide();
                centerX_->hide();
                centerY_->hide();
            } else { // 3 = Symmetrical (mouse + sliders)
                locX_->show();
                locY_->show();
                centerX_->show();
                centerY_->show();
            }
        } else { // Independent cases
            if (method == 0) { // 0 = Independent (mouse)
                locX_->hide();
                locXL_->hide();
                locY_->hide();
                locYT_->hide();
                centerX_->hide();
                centerY_->hide();
            } else { // 2 = Independent (mouse + sliders)
                locX_->show();
                locXL_->show();
                locY_->show();
                locYT_->show();
                centerX_->show();
                centerY_->show();
            }
        }
    } else { // In batch mode, sliders are necessary shown
        locX_->show();
        locXL_->show();
        locY_->show();
        locYT_->show();
        centerX_->show();
        centerY_->show();
    }

    // Raise event
    if (listener) {
        listener->panelChanged(EvLocallabSpotShapeMethod, shapeMethod_->get_active_text());
    }
}

void ControlSpotPanel::qualityMethodChanged()
{
    // printf("qualityMethodChanged\n");

    // Get selected control spot
    const auto s = treeview_->get_selection();

    if (!s->count_selected_rows()) {
        return;
    }

    const auto iter = s->get_selected();
    Gtk::TreeModel::Row row = *iter;

    row[spots_.qualityMethod] = qualityMethod_->get_active_row_number();

    // Raise event
    if (listener) {
        listener->panelChanged(EvLocallabSpotQualityMethod, qualityMethod_->get_active_text());
    }
}

//void ControlSpotPanel::complexMethodChanged()
//{
//    // printf("qualityMethodChanged\n");
//
//    // Get selected control spot
//    const auto s = treeview_->get_selection();
//
//    if (!s->count_selected_rows()) {
//        return;
//    }
//
//    const auto iter = s->get_selected();
//    Gtk::TreeModel::Row row = *iter;
//
//    row[spots_.complexMethod] = complexMethod_->get_active_row_number();
//
//    if (multiImage && complexMethod_->get_active_text() == M("GENERAL_UNCHANGED")) {
//        // excluFrame->show();
//    } else if (complexMethod_->get_active_row_number() == 0) { //sim
//        // excluFrame->hide();
//    } else if (complexMethod_->get_active_row_number() == 1) { // mod
//        // excluFrame->show();
//    } else if (complexMethod_->get_active_row_number() == 2) { // all
//        // excluFrame->show();
//    }
//
//    // Raise event
//    if (listener) {
//        listener->panelChanged(EvLocallabSpotcomplexMethod, complexMethod_->get_active_text());
//    }
//}

void ControlSpotPanel::wavMethodChanged()
{
    // printf("qualityMethodChanged\n");

    // Get selected control spot
    const auto s = treeview_->get_selection();

    if (!s->count_selected_rows()) {
        return;
    }

    const auto iter = s->get_selected();
    Gtk::TreeModel::Row row = *iter;

    row[spots_.wavMethod] = wavMethod_->get_active_row_number();

    // Raise event
    if (listener) {
        listener->panelChanged(EvLocallabSpotwavMethod, wavMethod_->get_active_text());
    }
}

void ControlSpotPanel::maskTypeChanged(int /*index*/)
{
    const auto s = treeview_->get_selection();

    if (!s->count_selected_rows()) {
        return;
    }

    const auto iter = s->get_selected();
    Gtk::TreeModel::Row row = *iter;

    row[spots_.maskType] = maskType_->getSelected();

    // Show/hide AI class based on mask type
    if (maskType_->getSelected() == 1) {
        ctboxaiclass->show();
        aiMaskTolerance_->show();
        // Set spotMethod to "full" for engine compatibility, but do NOT
        // trigger spotMethodChanged() which would expand loc to 3000 and
        // lose the user's geometric boundary. The AI mask + geometric
        // boundary work together: effect only applies where BOTH the AI
        // detects the class AND the pixel is within the spot boundary.
        row[spots_.spotMethod] = 2; // "full"
        row[spots_.shape] = 1; // "RECT"
        shape_->setSelected(1);
        spotMethodconn_.block(true);
        spotMethod_->set_active(2);
        spotMethodconn_.block(false);
    } else {
        ctboxaiclass->hide();
        aiMaskTolerance_->hide();
    }
    updateAutomaticMaskName(row);

    if (listener) {
        // Use EvLocallabSpotShape (not EvLocallabSpotSelectedWithMask) so the
        // hover mask overlay is NOT killed by panelChanged — this is a parameter
        // change, not a spot selection change.
        listener->panelChanged(EvLocallabSpotShape,
            maskType_->getSelected() == 1 ? M("TP_LOCALLAB_MASKTYPE_AI") : M("TP_LOCALLAB_MASKTYPE_NORMAL"));
    }
}

void ControlSpotPanel::aiMaskClassChanged(int /*index*/)
{
    const auto s = treeview_->get_selection();

    if (!s->count_selected_rows()) {
        return;
    }

    const auto iter = s->get_selected();
    Gtk::TreeModel::Row row = *iter;

    row[spots_.aiMaskClass] = aiMaskClass_->getSelected();
    updateAutomaticMaskName(row);

    if (listener) {
        const char* aiClassKeys[] = {
            "TP_LOCALLAB_AIMASK_CLASS_BACKGROUND", "TP_LOCALLAB_AIMASK_CLASS_PERSON",
            "TP_LOCALLAB_AIMASK_CLASS_SKY", "TP_LOCALLAB_AIMASK_CLASS_VEGETATION",
            "TP_LOCALLAB_AIMASK_CLASS_BUILDING", "TP_LOCALLAB_AIMASK_CLASS_VEHICLE",
            "TP_LOCALLAB_AIMASK_CLASS_ANIMAL", "TP_LOCALLAB_AIMASK_CLASS_FOREGROUND"
        };
        const int sel = aiMaskClass_->getSelected();
        // Use EvLocallabSpotShape (not EvLocallabSpotSelectedWithMask) so the
        // hover mask overlay is NOT killed — this is a parameter change.
        listener->panelChanged(EvLocallabSpotShape,
            sel >= 0 && sel < 8 ? M(aiClassKeys[sel]) : "");
    }

    startAIPreviewRefresh();
}

void ControlSpotPanel::maskBlendModeChanged(int /*index*/)
{
    setMaskBlendMode(maskBlendMode_->getSelected());
}

void ControlSpotPanel::setMaskBlendMode(int mode)
{
    const auto selection = treeview_->get_selection();
    if (!selection->count_selected_rows()) {
        return;
    }

    mode = rtengine::LIM(mode, 0, 4);
    Gtk::TreeModel::Row row = *selection->get_selected();
    if (static_cast<int>(row[spots_.maskBlendMode]) == mode
            && maskBlendMode_->getSelected() == mode) {
        return;
    }

    row[spots_.maskBlendMode] = mode;
    maskBlendModeConn_.block(true);
    maskBlendMode_->setSelected(mode);
    maskBlendModeConn_.block(false);
    updatingBlendModeMenu_ = true;
    blendModeMenuItems_[mode]->set_active(true);
    updatingBlendModeMenu_ = false;

    if (listener) {
        static const char* blendKeys[] = {
            "TP_LOCALLAB_MASK_BLEND_NORMAL", "TP_LOCALLAB_MASK_BLEND_DARKEN",
            "TP_LOCALLAB_MASK_BLEND_LIGHTEN", "TP_LOCALLAB_MASK_BLEND_LUMINOSITY",
            "TP_LOCALLAB_MASK_BLEND_COLOR"
        };
        listener->panelChanged(EvLocallabSpotShape, M(blendKeys[mode]));
    }
}

void ControlSpotPanel::onRowsReordered(
    const Gtk::TreeModel::Path& path,
    const Gtk::TreeModel::iterator& /*iter*/,
    int* newOrder)
{
    if (!path.empty() || !newOrder || !listener) {
        return;
    }

    const int count = static_cast<int>(treemodel_->children().size());
    reorderMap_.assign(newOrder, newOrder + count);
    if (count < 2) {
        return;
    }

    eventType = SpotReorder;
    selSpotChanged_ = true;
    listener->panelChanged(EvLocallabSpotShape, M("TP_LOCALLAB_MASK_REORDERED"));
}

void ControlSpotPanel::updateParamVisibility()
{
    // printf("updateParamVisibility\n");

    // Update Control Spot GUI according to shapeMethod_ combobox state (to be compliant with shapeMethodChanged function)
    const int method = shapeMethod_->get_active_row_number();
    const int meth = avoidgamutMethod_->get_active_row_number();

    if (!batchMode) {
        if (method == 1 || method == 3) { // Symmetrical cases
            locXL_->hide();
            locYT_->hide();

            if (method == 1) { // 1 = Symmetrical (mouse)
                locX_->hide();
                locY_->hide();
                centerX_->hide();
                centerY_->hide();
            } else { // 3 = Symmetrical (mouse + sliders)
                locX_->show();
                locY_->show();
                centerX_->show();
                centerY_->show();
            }
        } else { // Independent cases
            if (method == 0) { // 0 = Independent (mouse)
                locX_->hide();
                locXL_->hide();
                locY_->hide();
                locYT_->hide();
                centerX_->hide();
                centerY_->hide();
            } else { // 2 = Independent (mouse + sliders)
                locX_->show();
                locXL_->show();
                locY_->show();
                locYT_->show();
                centerX_->show();
                centerY_->show();
            }
        }
    } else { // In batch mode, sliders are necessary shown
        locX_->show();
        locXL_->show();
        locY_->show();
        locYT_->show();
        centerX_->show();
        centerY_->show();
    }

    if(meth == 1) {
        avoidrad_->show();
    } else {
        avoidrad_->hide();
}
   // ctboxspotmethod->show();
    hishow_->show();
    artifBox2->show();
    ctboxshape->show();
    colorscope_->hide();
    
    // Update Control Spot GUI according to spotMethod_ combobox state (to be compliant with spotMethodChanged function)
    if (multiImage && spotMethod_->get_active_text() == M("GENERAL_UNCHANGED")) {
        excluFrame->show();
    } else if (spotMethod_->get_active_row_number() == 0) { // Normal case
        excluFrame->hide();
    } else if (spotMethod_->get_active_row_number() == 1) { // Excluding case
        excluFrame->show();
    } else if (spotMethod_->get_active_row_number() == 2 || spotMethod_->get_active_row_number() == 3) {//full image or global
        excluFrame->hide();
        
        if(spotMethod_->get_active_row_number() == 3) {        
            artifBox2->hide();
            hishow_->hide();
            hishow_->set_active(false);           
            ctboxshape->hide();
            circrad_->hide();
            expTransGrad_->hide();
            expShapeDetect_->hide();
            expSpecCases_->hide();
            expMaskMerge_->hide();

        } else {
            artifBox2->show();
            colorscope_->hide();
            hishow_->show();
            ctboxshape->show();
            circrad_->show();
            if(hishow_->get_active()) {
                expTransGrad_->show();
                expShapeDetect_->show();
                expSpecCases_->show();
                expMaskMerge_->show();
            }
            
        }    

    }

/*
    if (multiImage && prevMethod_->get_active_text() == M("GENERAL_UNCHANGED")) {
        expTransGrad_->show();
        expShapeDetect_->show();
        expSpecCases_->show();
        expMaskMerge_->show();
        circrad_->show();
        ctboxshape->show();
    } else if (prevMethod_->get_active_row_number() == 0) { // Normal case
    */
    //ctboxshape->show();
   // artifBox2->show();
   
    if (!hishow_->get_active()  || spotMethod_->get_active_row_number() == 3) { // Normal case or Global 
        expTransGrad_->hide();
        expShapeDetect_->hide();
        expSpecCases_->hide();
        expMaskMerge_->hide();
        circrad_->hide();
        ctboxshape->hide();
        if(spotMethod_->get_active_row_number() == 3) {
            artifBox2->hide();
            hishow_->hide();
            ctboxshape->hide();
            circrad_->hide();
            expTransGrad_->hide();
            expShapeDetect_->hide();
            expSpecCases_->hide();
            expMaskMerge_->hide();
            
        } else {
            hishow_->show();
            artifBox2->show();
            colorscope_->hide();
            hishow_->show();
            ctboxshape->show();
            circrad_->show();
        }
        
    } else { // Excluding case
        expTransGrad_->show();
        expShapeDetect_->show();
        expSpecCases_->show();
        expMaskMerge_->show();
        circrad_->show();
        ctboxshape->show();
        hishow_->show();
        
    }


}

void ControlSpotPanel::adjusterChanged(Adjuster* a, double newval)
{
    // printf("adjusterChanged\n");

    const int method = shapeMethod_->get_active_row_number();

    // Get selected control spot
    const auto s = treeview_->get_selection();

    if (!s->count_selected_rows()) {
        return;
    }

    const auto iter = s->get_selected();
    Gtk::TreeModel::Row row = *iter;

    if (a == sensiexclu_) {
        row[spots_.sensiexclu] = sensiexclu_->getIntValue();

        if (listener) {
            listener->panelChanged(EvLocallabSpotSensiexclu, sensiexclu_->getTextValue());
        }
    }

    if (a == structexclu_) {
        row[spots_.structexclu] = structexclu_->getIntValue();

        if (listener) {
            listener->panelChanged(Evlocallabstructexlu, structexclu_->getTextValue());
        }
    }

    if (a == locX_) {
        row[spots_.locX] = locX_->getIntValue();

        if (!batchMode && (method == 1 || method == 3)) { // Symmetrical cases (in batch mode, sliders are always independent)
            disableParamlistener(true);
            locXL_->setValue(locX_->getValue());
            disableParamlistener(false);
            row[spots_.locXL] = locXL_->getIntValue();
        }

        updateControlSpotCurve(row);

        if (listener) {
            listener->panelChanged(EvLocallabSpotLocX, locX_->getTextValue());
        }
    }

    if (a == locXL_) {
        row[spots_.locXL] = locXL_->getIntValue();

        if (!batchMode && (method == 1 || method == 3)) { // Symmetrical cases (in batch mode, sliders are always independent)
            disableParamlistener(true);
            locX_->setValue(locXL_->getValue());
            disableParamlistener(false);
            row[spots_.locX] = locX_->getIntValue();
        }

        updateControlSpotCurve(row);

        if (listener) {
            listener->panelChanged(EvLocallabSpotLocXL, locXL_->getTextValue());
        }
    }

    if (a == locY_) {
        row[spots_.locY] = locY_->getIntValue();

        if (!batchMode && (method == 1 || method == 3)) { // Symmetrical cases (in batch mode, sliders are always independent)
            disableParamlistener(true);
            locYT_->setValue(locY_->getValue());
            disableParamlistener(false);
            row[spots_.locYT] = locYT_->getIntValue();
        }

        updateControlSpotCurve(row);

        if (listener) {
            listener->panelChanged(EvLocallabSpotLocY, locY_->getTextValue());
        }
    }

    if (a == locYT_) {
        row[spots_.locYT] = locYT_->getIntValue();

        if (!batchMode && (method == 1 || method == 3)) { // Symmetrical cases (in batch mode, sliders are always independent)
            disableParamlistener(true);
            locY_->setValue(locYT_->getValue());
            disableParamlistener(false);
            row[spots_.locY] = locY_->getIntValue();
        }

        updateControlSpotCurve(row);

        if (listener) {
            listener->panelChanged(EvLocallabSpotLocYT, locYT_->getTextValue());
        }
    }

    if (a == centerX_ || a == centerY_) {
        row[spots_.centerX] = centerX_->getIntValue();
        row[spots_.centerY] = centerY_->getIntValue();

        updateControlSpotCurve(row);

        if (listener) {
            listener->panelChanged(EvLocallabSpotCenter, "X=" + centerX_->getTextValue() + ", Y=" + centerY_->getTextValue());
        }
    }

    if (a == circrad_) {
        row[spots_.circrad] = circrad_->getValue();

        updateControlSpotCurve(row);

        if (listener) {
            listener->panelChanged(EvLocallabSpotCircrad, circrad_->getTextValue());
        }
    }

    if (a == transit_) {
        row[spots_.transit] = transit_->getValue();

        if (listener) {
            listener->panelChanged(EvLocallabSpotTransit, transit_->getTextValue());
        }
    }

    if (a == transitweak_) {
        row[spots_.transitweak] = transitweak_->getValue();

        if (listener) {
            listener->panelChanged(EvLocallabSpotTransitweak, transitweak_->getTextValue());
        }
    }

    if (a == transitgrad_) {
        row[spots_.transitgrad] = transitgrad_->getValue();

        if (listener) {
            listener->panelChanged(EvLocallabSpotTransitgrad, transitgrad_->getTextValue());
        }
    }

    if (a == gradangle_) {
        row[spots_.gradangle] = gradangle_->getValue();

        updateControlSpotCurve(row);

        if (listener) {
            listener->panelChanged(EvLocallabSpotGradAngle, gradangle_->getTextValue());
        }
    }

    if (a == feather_) {
        row[spots_.feather] = feather_->getValue();

        if (listener) {
            listener->panelChanged(EvLocallabSpotfeather, feather_->getTextValue());
        }
    }

    if (a == struc_) {
        row[spots_.struc] = struc_->getValue();

        if (listener) {
            listener->panelChanged(EvLocallabSpotStruc, struc_->getTextValue());
        }
    }

    if (a == thresh_) {
        row[spots_.thresh] = thresh_->getValue();

        if (listener) {
            listener->panelChanged(EvLocallabSpotThresh, thresh_->getTextValue());
        }
    }

    if (a == iter_) {
        row[spots_.iter] = iter_->getValue();

        if (listener) {
            listener->panelChanged(EvLocallabSpotIter, iter_->getTextValue());
        }
    }

    if (a == balan_) {
        row[spots_.balan] = balan_->getValue();

        if (listener) {
            listener->panelChanged(EvLocallabSpotbalan, balan_->getTextValue());
        }
    }

    if (a == balanh_) {
        row[spots_.balanh] = balanh_->getValue();

        if (listener) {
            listener->panelChanged(EvLocallabSpotbalanh, balanh_->getTextValue());
        }
    }

    if (a == colorde_) {
        row[spots_.colorde] = colorde_->getValue();

        if (listener) {
            listener->panelChanged(EvLocallabSpotcolorde, colorde_->getTextValue());
        }
    }

    if (a == colorscope_) {
        row[spots_.colorscope] = colorscope_->getValue();

        if (listener) {
            listener->panelChanged(EvLocallabSpotcolorscope, colorscope_->getTextValue());
        }
    }

    if (a == avoidrad_) {
        row[spots_.avoidrad] = avoidrad_->getValue();

        if (listener) {
            listener->panelChanged(EvLocallabSpotavoidrad, avoidrad_->getTextValue());
        }
    }

    if (a == scopemask_) {
        row[spots_.scopemask] = scopemask_->getIntValue();

        if (listener) {
            listener->panelChanged(EvLocallabSpotscopemask, scopemask_->getTextValue());
        }
    }

    if (a == denoichmask_) {
        row[spots_.denoichmask] = denoichmask_->getValue();

        if (listener) {
            listener->panelChanged(EvLocallabSpotdenoichmask, denoichmask_->getTextValue());
        }
    }

    if (a == lumask_) {
        row[spots_.lumask] = lumask_->getIntValue();

        if (listener) {
            listener->panelChanged(EvLocallabSpotlumask, lumask_->getTextValue());
        }
    }

    if (a == aiMaskTolerance_) {
        row[spots_.aiMaskThreshold] = rtengine::LIM(1.0 - newval / 100.0, 0.0, 1.0);

        if (listener) {
            listener->panelChanged(EvLocallabSpotShape,
                aiMaskTolerance_->getTextValue());
        }
        queueMaskPreviewRefresh();
    }

    if (a == polyFeather_) {
        row[spots_.polyMaskFeather] = polyFeather_->getValue();

        if (listener) {
            listener->panelChanged(EvLocallabSpotShape, polyFeather_->getTextValue());
        }
    }

    if (a == polySnapTol_) {
        row[spots_.polyMaskSnapTolerance] = polySnapTol_->getValue();
        // No engine event needed — tolerance is only used during drawing
    }

    if (a == polyLegLen_) {
        row[spots_.polyMaskLegLength] = polyLegLen_->getValue();
        // No engine event needed — leg length is only used during drawing
    }
}

void ControlSpotPanel::hishowChanged()
{
    // printf("avoidChanged\n");

    // Get selected control spot
    const auto s = treeview_->get_selection();

    if (!s->count_selected_rows()) {
        return;
    }

    const auto iter = s->get_selected();
    Gtk::TreeModel::Row row = *iter;
    row[spots_.hishow] = hishow_->get_active();


    ctboxshape->show();

    if (!hishow_->get_active()  || spotMethod_->get_active_row_number() == 3) { // Normal case or Global
        expTransGrad_->hide();
        expShapeDetect_->hide();
        expSpecCases_->hide();
        expMaskMerge_->hide();
        circrad_->hide();
        ctboxshape->hide();
        shapeMethod_->set_active(0);
        if(spotMethod_->get_active_row_number() == 3) {
            hishow_->hide();
            hishow_->set_active(false);           
            circrad_->hide();
            expTransGrad_->hide();
            expShapeDetect_->hide();
            expSpecCases_->hide();
            expMaskMerge_->hide();
            
        } else {
            hishow_->show();
            circrad_->show();
            
        }

    } else { // Excluding case
        expTransGrad_->show();
        expShapeDetect_->show();
        expSpecCases_->show();
        expMaskMerge_->show();
        circrad_->show();
        ctboxshape->show();
        hishow_->show();
        
   }

    // Raise event
    if (listener) {
        if (hishow_->get_active()) {
            listener->panelChanged(Evlocallabhishow, M("GENERAL_ENABLED"));
        } else {
            listener->panelChanged(Evlocallabhishow, M("GENERAL_DISABLED"));
        }
    }
}


void ControlSpotPanel::activChanged()
{
    // printf("activChanged\n");

    // Get selected control spot
    const auto s = treeview_->get_selection();

    if (!s->count_selected_rows()) {
        return;
    }

    const auto iter = s->get_selected();
    Gtk::TreeModel::Row row = *iter;
    row[spots_.activ] = activ_->get_active();

    // Raise event
    if (listener) {
        if (activ_->get_active()) {
            listener->panelChanged(Evlocallabactiv, M("GENERAL_ENABLED"));
        } else {
            listener->panelChanged(Evlocallabactiv, M("GENERAL_DISABLED"));
        }
    }
}

void ControlSpotPanel::avoidnegChanged()
{

    // Get selected control spot
    const auto s = treeview_->get_selection();

    if (!s->count_selected_rows()) {
        return;
    }

    const auto iter = s->get_selected();
    Gtk::TreeModel::Row row = *iter;
    row[spots_.avoidneg] = avoidneg_->get_active();

    // Raise event
    if (listener) {
        if (avoidneg_->get_active()) {
            listener->panelChanged(EvLocallabavoidnegative, M("GENERAL_ENABLED"));
        } else {
            listener->panelChanged(EvLocallabavoidnegative, M("GENERAL_DISABLED"));
        }
    }
}



void ControlSpotPanel::blwhChanged()
{
    // printf("blwhChanged\n");

    // Get selected control spot
    const auto s = treeview_->get_selection();

    if (!s->count_selected_rows()) {
        return;
    }

    const auto iter = s->get_selected();
    Gtk::TreeModel::Row row = *iter;
    row[spots_.blwh] = blwh_->get_active();

    // Raise event
    if (listener) {
        if (blwh_->get_active()) {
            listener->panelChanged(Evlocallabblwh, M("GENERAL_ENABLED"));
        } else {
            listener->panelChanged(Evlocallabblwh, M("GENERAL_DISABLED"));
        }
    }
}

void ControlSpotPanel::recursChanged()
{
    // printf("recursChanged\n");

    // Get selected control spot
    const auto s = treeview_->get_selection();

    if (!s->count_selected_rows()) {
        return;
    }

    const auto iter = s->get_selected();
    Gtk::TreeModel::Row row = *iter;
    row[spots_.recurs] = recurs_->get_active();

    // Raise event
    if (listener) {
        if (recurs_->get_active()) {
            listener->panelChanged(Evlocallabrecurs, M("GENERAL_ENABLED"));
        } else {
            listener->panelChanged(Evlocallabrecurs, M("GENERAL_DISABLED"));
        }
    }
}


void ControlSpotPanel::laplacChanged()
{
    // Get selected control spot
    const auto s = treeview_->get_selection();

    if (!s->count_selected_rows()) {
        return;
    }

    const auto iter = s->get_selected();
    Gtk::TreeModel::Row row = *iter;
    row[spots_.laplac] = laplac_->get_active();

    // Raise event
    if (listener) {
        if (laplac_->get_active()) {
            listener->panelChanged(Evlocallablaplac, M("GENERAL_ENABLED"));
        } else {
            listener->panelChanged(Evlocallablaplac, M("GENERAL_DISABLED"));
        }
    }
}


void ControlSpotPanel::deltaeChanged()
{
    // Get selected control spot
    const auto s = treeview_->get_selection();

    if (!s->count_selected_rows()) {
        return;
    }

    const auto iter = s->get_selected();
    Gtk::TreeModel::Row row = *iter;
    row[spots_.deltae] = deltae_->get_active();

    // Raise event
    if (listener) {
        if (deltae_->get_active()) {
            listener->panelChanged(Evlocallabdeltae, M("GENERAL_ENABLED"));
        } else {
            listener->panelChanged(Evlocallabdeltae, M("GENERAL_DISABLED"));
        }
    }
}

void ControlSpotPanel::shortcChanged()
{
    // Get selected control spot
    const auto s = treeview_->get_selection();

    if (!s->count_selected_rows()) {
        return;
    }

    const auto iter = s->get_selected();
    Gtk::TreeModel::Row row = *iter;
    row[spots_.shortc] = shortc_->get_active();

    // Raise event
    if (listener) {
        if (shortc_->get_active()) {
            listener->panelChanged(Evlocallabshortc, M("GENERAL_ENABLED"));
        } else {
            listener->panelChanged(Evlocallabshortc, M("GENERAL_DISABLED"));
        }
    }
}

//void ControlSpotPanel::savrestChanged()
//{
//    // Get selected control spot
//    const auto s = treeview_->get_selection();
//
//    if (!s->count_selected_rows()) {
//        return;
//    }
//
//    const auto iter = s->get_selected();
//    Gtk::TreeModel::Row row = *iter;
//    row[spots_.savrest] = savrest_->get_active();
//
//    // Raise event
//    if (listener) {
//        if (savrest_->get_active()) {
//            listener->panelChanged(Evlocallabsavrest, M("GENERAL_ENABLED"));
//        } else {
//            listener->panelChanged(Evlocallabsavrest, M("GENERAL_DISABLED"));
//        }
//    }
//}

void ControlSpotPanel::previewChanged()
{
    // If deltaE preview is activated, deactivate all other tool mask preview
    if (controlPanelListener) {
        controlPanelListener->resetToolMaskView();
    }

    if (listener) {
        listener->panelChanged(EvlocallabshowmaskMethod, "");
    }
}

void ControlSpotPanel::disableParamlistener(bool cond)
{
    // printf("disableParamlistener: %d\n", cond);

    treeviewconn_.block(cond);
    buttonaddconn_.block(cond);
    buttondeleteconn_.block(cond);
    buttonduplicateconn_.block(cond);
    buttonrenameconn_.block(cond);
    buttonvisibilityconn_.block(cond);
    prevMethodconn_.block(cond);
    shapeconn_.block(cond);
    spotMethodconn_.block(cond);
    sensiexclu_->block(cond);
    structexclu_->block(cond);
    shapeMethodconn_.block(cond);
    locX_->block(cond);
    locXL_->block(cond);
    locY_->block(cond);
    locYT_->block(cond);
    centerX_->block(cond);
    centerY_->block(cond);
    circrad_->block(cond);
    qualityMethodconn_.block(cond);
    transit_->block(cond);
    transitweak_->block(cond);
    transitgrad_->block(cond);
    gradangle_->block(cond);
    feather_->block(cond);
    struc_->block(cond);
    thresh_->block(cond);
    iter_->block(cond);
    balan_->block(cond);
    balanh_->block(cond);
    colorde_->block(cond);
    colorscope_->block(cond);
    avoidrad_->block(cond);
    hishowconn_.block(cond);
    activConn_.block(cond);
    avoidnegConn_.block(cond);
    blwhConn_.block(cond);
    recursConn_.block(cond);
    laplacConn_.block(cond);
    deltaeConn_.block(cond);
    scopemask_->block(cond);
    denoichmask_->block(cond);
    shortcConn_.block(cond);
    lumask_->block(cond);
    //savrestConn_.block(cond);
    //complexMethodconn_.block(cond);
    wavMethodconn_.block(cond);
	avoidgamutconn_.block(cond);
    maskTypeConn_.block(cond);
    aiMaskClassConn_.block(cond);
    aiMaskTolerance_->block(cond);
    maskBlendModeConn_.block(cond);

}

void ControlSpotPanel::setParamEditable(bool cond)
{
    // printf("setParamEditable: %d\n", cond);

    prevMethod_->set_sensitive(cond);
    shape_->set_sensitive(cond);
    spotMethod_->set_sensitive(cond);
    sensiexclu_->set_sensitive(cond);
    structexclu_->set_sensitive(cond);
    shapeMethod_->set_sensitive(cond);
    locX_->set_sensitive(cond);
    locXL_->set_sensitive(cond);
    locY_->set_sensitive(cond);
    locYT_->set_sensitive(cond);
    centerX_->set_sensitive(cond);
    centerY_->set_sensitive(cond);
    circrad_->set_sensitive(cond);
    qualityMethod_->set_sensitive(cond);
    transit_->set_sensitive(cond);
    transitweak_->set_sensitive(cond);
    transitgrad_->set_sensitive(cond);
    gradangle_->set_sensitive(cond);
    feather_->set_sensitive(cond);
    struc_->set_sensitive(cond);
    thresh_->set_sensitive(cond);
    iter_->set_sensitive(cond);
    balan_->set_sensitive(cond);
    balanh_->set_sensitive(cond);
    colorde_->set_sensitive(cond);
    colorscope_->set_sensitive(cond);
    avoidrad_->set_sensitive(cond);
    hishow_->set_sensitive(cond);
    activ_->set_sensitive(cond);
    avoidneg_->set_sensitive(cond);
    blwh_->set_sensitive(cond);
    recurs_->set_sensitive(cond);
    laplac_->set_sensitive(cond);
    deltae_->set_sensitive(cond);
    scopemask_->set_sensitive(cond);
    denoichmask_->set_sensitive(cond);
    shortc_->set_sensitive(cond);
    lumask_->set_sensitive(cond);
    //savrest_->set_sensitive(cond);
    //complexMethod_->set_sensitive(cond);
    wavMethod_->set_sensitive(cond);
    preview_->set_sensitive(cond);
    avoidgamutMethod_->set_sensitive(cond);

    if (!cond) {
        // Reset complex parameters visibility to default state
        expTransGrad_->hide();
        expShapeDetect_->hide();
        expSpecCases_->hide();
        expMaskMerge_->hide();
        circrad_->hide();
        ctboxshape->hide();
        excluFrame->hide();
//        ctboxshapemethod->hide();
        locX_->hide();
        locXL_->hide();
        locY_->hide();
        locYT_->hide();
        centerX_->hide();
        centerY_->hide();
    }
}

void ControlSpotPanel::setDefaultExpanderVisibility()
{
    expAdvanced_->set_expanded(false);
    expTransGrad_->set_expanded(false);
    expShapeDetect_->set_expanded(false);
    expSpecCases_->set_expanded(false);
    expMaskMerge_->set_expanded(false);
}

void ControlSpotPanel::addControlSpotCurve(Gtk::TreeModel::Row& row)
{
    // printf("addControlSpotCurve\n");

    if (row[spots_.curveid] > 0) { // Row has already an associated curve
        return;
    }

    // Creation of visibleGeometry
    Circle* cirX;
    cirX = new Circle();
    cirX->radius = 4.;
    cirX->filled = true;
    cirX->datum = Geometry::IMAGE;
    Circle* cirXL;
    cirXL = new Circle();
    cirXL->radius = 4.;
    cirXL->filled = true;
    cirXL->datum = Geometry::IMAGE;
    Circle* cirY;
    cirY = new Circle();
    cirY->radius = 4.;
    cirY->filled = true;
    cirY->datum = Geometry::IMAGE;
    Circle* cirYT;
    cirYT = new Circle();
    cirYT->radius = 4.;
    cirYT->filled = true;
    cirYT->datum = Geometry::IMAGE;
    Circle* centerCircle;
    centerCircle = new Circle();
    centerCircle->datum = Geometry::IMAGE;
    centerCircle->radiusInImageSpace = true;
    Ellipse* shape_ellipse;
    shape_ellipse = new Ellipse();
    shape_ellipse->datum = Geometry::IMAGE;
    shape_ellipse->radiusInImageSpace = true;
    EditRectangle* shape_rectangle;
    shape_rectangle = new EditRectangle();
    shape_rectangle->datum = Geometry::IMAGE;
    EditSubscriber::visibleGeometry.push_back(centerCircle); // (curveid - 1) * GEOM_PER_SPOT
    EditSubscriber::visibleGeometry.push_back(shape_ellipse); // (curveid - 1) * GEOM_PER_SPOT + 1
    EditSubscriber::visibleGeometry.push_back(shape_rectangle); // (curveid - 1) * GEOM_PER_SPOT + 2
    EditSubscriber::visibleGeometry.push_back(cirX); // (curveid - 1) * GEOM_PER_SPOT + 3
    EditSubscriber::visibleGeometry.push_back(cirXL); // (curveid - 1) * GEOM_PER_SPOT + 4
    EditSubscriber::visibleGeometry.push_back(cirY); // (curveid - 1) * GEOM_PER_SPOT + 5
    EditSubscriber::visibleGeometry.push_back(cirYT); // (curveid - 1) * GEOM_PER_SPOT + 6

    // Gradient geometry (visible)
    Line* gradLine1 = new Line();
    gradLine1->datum = Geometry::IMAGE;
    gradLine1->innerLineWidth = 2;
    Line* gradLine2 = new Line();
    gradLine2->datum = Geometry::IMAGE;
    gradLine2->innerLineWidth = 2;
    Circle* gradAngleCircle = new Circle();
    gradAngleCircle->datum = Geometry::IMAGE;
    gradAngleCircle->radiusInImageSpace = false;
    gradAngleCircle->radius = 6;
    gradAngleCircle->filled = true;
    EditSubscriber::visibleGeometry.push_back(gradLine1);       // (curveid - 1) * GEOM_PER_SPOT + 7
    EditSubscriber::visibleGeometry.push_back(gradLine2);       // (curveid - 1) * GEOM_PER_SPOT + 8
    EditSubscriber::visibleGeometry.push_back(gradAngleCircle); // (curveid - 1) * GEOM_PER_SPOT + 9

    // Polygon (Lasso) geometry (visible)
    Polyline* polyShape = new Polyline();
    polyShape->datum = Geometry::IMAGE;
    polyShape->filled = false;
    EditSubscriber::visibleGeometry.push_back(polyShape);       // (curveid - 1) * GEOM_PER_SPOT + 10

    // Creation of mouseOverGeometry
    cirX = new Circle();
    cirX->radius = 4.;
    cirX->filled = true;
    cirX->datum = Geometry::IMAGE;
    cirXL = new Circle();
    cirXL->radius = 4.;
    cirXL->filled = true;
    cirXL->datum = Geometry::IMAGE;
    cirY = new Circle();
    cirY->radius = 4.;
    cirY->filled = true;
    cirY->datum = Geometry::IMAGE;
    cirYT = new Circle();
    cirYT->radius = 4.;
    cirYT->filled = true;
    cirYT->datum = Geometry::IMAGE;
    centerCircle = new Circle();
    centerCircle->filled = true;
    centerCircle->datum = Geometry::IMAGE;
    centerCircle->radiusInImageSpace = true;
    shape_ellipse = new Ellipse();
    shape_ellipse->datum = Geometry::IMAGE;
    shape_ellipse->radiusInImageSpace = true;
    shape_rectangle = new EditRectangle();
    shape_rectangle->datum = Geometry::IMAGE;
    EditSubscriber::mouseOverGeometry.push_back(centerCircle);  // (curveid - 1) * GEOM_PER_SPOT
    EditSubscriber::mouseOverGeometry.push_back(shape_ellipse);  // (curveid - 1) * GEOM_PER_SPOT + 1
    EditSubscriber::mouseOverGeometry.push_back(shape_rectangle);  // (curveid - 1) * GEOM_PER_SPOT + 2
    EditSubscriber::mouseOverGeometry.push_back(cirX);  // (curveid - 1) * GEOM_PER_SPOT + 3
    EditSubscriber::mouseOverGeometry.push_back(cirXL);  // (curveid - 1) * GEOM_PER_SPOT + 4
    EditSubscriber::mouseOverGeometry.push_back(cirY);  // (curveid - 1) * GEOM_PER_SPOT + 5
    EditSubscriber::mouseOverGeometry.push_back(cirYT);  // (curveid - 1) * GEOM_PER_SPOT + 6

    // Gradient geometry (mouseOver) — wide hit area for easy grabbing
    gradLine1 = new Line();
    gradLine1->datum = Geometry::IMAGE;
    gradLine1->innerLineWidth = 30;
    gradLine2 = new Line();
    gradLine2->datum = Geometry::IMAGE;
    gradLine2->innerLineWidth = 30;
    gradAngleCircle = new Circle();
    gradAngleCircle->datum = Geometry::IMAGE;
    gradAngleCircle->radiusInImageSpace = false;
    gradAngleCircle->radius = 12; // Larger for easier grabbing
    gradAngleCircle->filled = true;
    EditSubscriber::mouseOverGeometry.push_back(gradLine1);       // (curveid - 1) * GEOM_PER_SPOT + 7
    EditSubscriber::mouseOverGeometry.push_back(gradLine2);       // (curveid - 1) * GEOM_PER_SPOT + 8
    EditSubscriber::mouseOverGeometry.push_back(gradAngleCircle); // (curveid - 1) * GEOM_PER_SPOT + 9

    // Polygon (Lasso) geometry (mouseOver) — same as visible, wide hit area
    polyShape = new Polyline();
    polyShape->datum = Geometry::IMAGE;
    polyShape->filled = true; // Filled for hit detection
    EditSubscriber::mouseOverGeometry.push_back(polyShape);       // (curveid - 1) * GEOM_PER_SPOT + 10

    row[spots_.curveid] = EditSubscriber::visibleGeometry.size() / GEOM_PER_SPOT;
}

void ControlSpotPanel::updateControlSpotCurve(const Gtk::TreeModel::Row& row)
{
    const int curveid_ = row[spots_.curveid];
    EditDataProvider* const dataProvider = getEditProvider();

    // printf("updateControlSpotCurve: %d\n", curveid_);

    if (curveid_ == 0 || !dataProvider) { // Row has no associated curve or there is no EditProvider
        return;
    }

    int imW = 0;
    int imH = 0;
    dataProvider->getImageSize(imW, imH);

    if (!imW || !imH) { // No image loaded
        return;
    }

    const int centerX_ = row[spots_.centerX];
    const int centerY_ = row[spots_.centerY];
    const int circrad_ = row[spots_.circrad];
    const int locX_ = row[spots_.locX];
    const int locXL_ = row[spots_.locXL];
    const int locY_ = row[spots_.locY];
    const int locYT_ = row[spots_.locYT];
    const int shape_ = row[spots_.shape];
    const bool isvisible_ = row[spots_.isvisible];

    const int decayX = (double)locX_ * (double)imW / 2000.;
    const int decayXL = (double)locXL_ * (double)imW / 2000.;
    const int decayY = (double)locY_ * (double)imH / 2000.;
    const int decayYT = (double)locYT_ * (double)imH / 2000.;
    const rtengine::Coord origin((double)imW / 2. + (double)centerX_ * (double)imW / 2000., (double)imH / 2. + (double)centerY_ * (double)imH / 2000.);

    const auto updateSelectionCircle = [&](Geometry * geometry, const int offsetX, const int offsetY) {
        const auto cir = static_cast<Circle*>(geometry);
        cir->center.x = origin.x + offsetX;
        cir->center.y = origin.y + offsetY;
    };

    const auto updateCenterCircle = [&](Geometry * geometry) {
        const auto circle = static_cast<Circle*>(geometry);
        circle->center = origin;
        circle->radius = circrad_;
    };

    const auto updateEllipse = [&](Geometry * geometry) {
        const auto ellipse = static_cast<Ellipse*>(geometry);
        ellipse->center = origin;
        ellipse->radX = decayX;
        ellipse->radXL = decayXL;
        ellipse->radY = decayY;
        ellipse->radYT = decayYT;
    };

    const auto updateRectangle = [&](Geometry * geometry) {
        const auto rectangle = static_cast<EditRectangle*>(geometry);
        rectangle->bottomRight.x = origin.x + decayX;
        rectangle->bottomRight.y = origin.y + decayY;
        rectangle->topLeft.x = origin.x - decayXL;
        rectangle->topLeft.y = origin.y - decayYT;
    };

    const double gradangle_ = row[spots_.gradangle];
    const double transit_val = row[spots_.transit];
    const auto updateGradientLine = [&](Geometry* geom1, Geometry* geom2, Geometry* handle) {
        const float theta = gradangle_ * rtengine::RT_PI_F / 180.f;
        const float sinT = std::sin(theta);
        const float cosT = std::cos(theta);
        // Perpendicular direction to gradient
        const float perpX = cosT;
        const float perpY = sinT;
        // Max span for drawing the lines — cap to image size
        const float maxExtent = std::max((float)imW, (float)imH);
        const float span = maxExtent;
        // Transition band position along gradient direction
        // Cap to image extent from center (matching engine calcTransitiongrad)
        const float imgMaxX = std::max((float)origin.x, (float)(imW - origin.x));
        const float imgMaxY = std::max((float)origin.y, (float)(imH - origin.y));
        const float maxX = std::min(std::max((float)decayX, (float)decayXL), imgMaxX);
        const float maxY = std::min(std::max((float)decayY, (float)decayYT), imgMaxY);
        const float maxProj = maxX * std::abs(sinT) + maxY * std::abs(cosT);
        // Position lines at actual transition boundaries using transit value
        const float ach = static_cast<float>(transit_val) / 100.f;
        const float bandHalf = maxProj * ach;

        auto* l1 = static_cast<Line*>(geom1);
        l1->begin.set(origin.x + sinT * bandHalf - perpX * span,
                      origin.y - cosT * bandHalf - perpY * span);
        l1->end.set(origin.x + sinT * bandHalf + perpX * span,
                    origin.y - cosT * bandHalf + perpY * span);

        auto* l2 = static_cast<Line*>(geom2);
        l2->begin.set(origin.x - sinT * bandHalf - perpX * span,
                      origin.y + cosT * bandHalf - perpY * span);
        l2->end.set(origin.x - sinT * bandHalf + perpX * span,
                    origin.y + cosT * bandHalf + perpY * span);

        auto* h = static_cast<Circle*>(handle);
        h->center.set(origin.x + sinT * bandHalf, origin.y - cosT * bandHalf);
    };

    const int base = (curveid_ - 1) * GEOM_PER_SPOT;

    updateCenterCircle(visibleGeometry.at(base));
    updateCenterCircle(mouseOverGeometry.at(base));

    updateEllipse(visibleGeometry.at(base + 1));
    updateEllipse(mouseOverGeometry.at(base + 1));

    updateRectangle(visibleGeometry.at(base + 2));
    updateRectangle(mouseOverGeometry.at(base + 2));

    updateSelectionCircle(visibleGeometry.at(base + 3), decayX, 0.);
    updateSelectionCircle(mouseOverGeometry.at(base + 3), decayX, 0.);

    updateSelectionCircle(visibleGeometry.at(base + 4), -decayXL, 0.);
    updateSelectionCircle(mouseOverGeometry.at(base + 4), -decayXL, 0.);

    updateSelectionCircle(visibleGeometry.at(base + 5), 0., decayY);
    updateSelectionCircle(mouseOverGeometry.at(base + 5), 0., decayY);

    updateSelectionCircle(visibleGeometry.at(base + 6), 0., -decayYT);
    updateSelectionCircle(mouseOverGeometry.at(base + 6), 0., -decayYT);

    // Update gradient lines
    updateGradientLine(visibleGeometry.at(base + 7), visibleGeometry.at(base + 8), visibleGeometry.at(base + 9));
    updateGradientLine(mouseOverGeometry.at(base + 7), mouseOverGeometry.at(base + 8), mouseOverGeometry.at(base + 9));

    // Update polygon outline from stored vertices
    const auto updatePolygon = [&](Geometry* geom, const std::vector<int>& pts) {
        auto* poly = static_cast<Polyline*>(geom);
        poly->points.clear();
        for (size_t i = 0; i + 1 < pts.size(); i += 2) {
            poly->points.push_back(rtengine::Coord(pts[i], pts[i + 1]));
        }
        // Close the polygon by connecting last point to first
        if (poly->points.size() >= 3) {
            poly->points.push_back(poly->points.front());
        }
    };

    const std::vector<int> polyPts = row[spots_.polyMaskPoints];
    updatePolygon(visibleGeometry.at(base + 10), polyPts);
    updatePolygon(mouseOverGeometry.at(base + 10), polyPts);

    // Helper to set active state for a range of geometry indices
    const auto setGeomActive = [&](int idx, bool vis, bool mo) {
        EditSubscriber::visibleGeometry.at(base + idx)->setActive(vis);
        EditSubscriber::mouseOverGeometry.at(base + idx)->setActive(mo);
    };

    // Update shape visibility according to shape type and spot visibility
    if (isvisible_) {
        setGeomActive(0, true, true); // centerCircle always visible

        if (shape_ == 0) { // Ellipse
            setGeomActive(1, true, true);   // ellipse
            setGeomActive(2, false, false); // rectangle
            for (int i = 3; i <= 6; i++) setGeomActive(i, true, true); // handles
            for (int i = 7; i <= 9; i++) setGeomActive(i, false, false); // gradient
            setGeomActive(10, false, false); // polygon
        } else if (shape_ == 1) { // Rectangle
            setGeomActive(1, false, false);
            setGeomActive(2, true, true);
            for (int i = 3; i <= 6; i++) setGeomActive(i, true, true);
            for (int i = 7; i <= 9; i++) setGeomActive(i, false, false);
            setGeomActive(10, false, false);
        } else if (shape_ == 3) { // Polygon (Lasso)
            setGeomActive(1, false, false);
            setGeomActive(2, false, false);
            for (int i = 3; i <= 6; i++) setGeomActive(i, false, false);
            for (int i = 7; i <= 9; i++) setGeomActive(i, false, false);
            setGeomActive(10, true, true);
        } else { // 2 = Gradient
            setGeomActive(1, false, false);
            setGeomActive(2, false, false);
            for (int i = 3; i <= 6; i++) setGeomActive(i, false, false);
            for (int i = 7; i <= 9; i++) setGeomActive(i, true, true);
            setGeomActive(10, false, false);
        }
    } else {
        for (int i = 0; i < GEOM_PER_SPOT; i++) {
            EditSubscriber::visibleGeometry.at(base + i)->setActive(false);
            EditSubscriber::mouseOverGeometry.at(base + i)->setActive(false);
        }
    }

    queueMaskPreviewRefresh();
}

void ControlSpotPanel::deleteControlSpotCurve(Gtk::TreeModel::Row& row)
{
    const int curveid_ = row[spots_.curveid];

    // printf("deleteControlSpotCurve: %d\n", curveid_);

    if (curveid_ == 0) { // Row has no associated curve
        return;
    }

    // visibleGeometry
    for (int i = GEOM_PER_SPOT - 1; i >= 0; i--) {
        delete *(EditSubscriber::visibleGeometry.begin() + (curveid_ - 1) * GEOM_PER_SPOT + i);
        EditSubscriber::visibleGeometry.erase(EditSubscriber::visibleGeometry.begin() + (curveid_ - 1) * GEOM_PER_SPOT + i);
    }

    // mouseOverGeometry
    for (int i = GEOM_PER_SPOT - 1; i >= 0; i--) {
        delete *(EditSubscriber::mouseOverGeometry.begin() + (curveid_ - 1) * GEOM_PER_SPOT + i);
        EditSubscriber::mouseOverGeometry.erase(EditSubscriber::mouseOverGeometry.begin() + (curveid_ - 1) * GEOM_PER_SPOT + i);
    }

    row[spots_.curveid] = 0; // Reset associated curve id

    // Reordering curve id
    const Gtk::TreeModel::Children children = treemodel_->children();

    for (auto iter = children.begin(); iter != children.end(); iter++) {
        Gtk::TreeModel::Row r = *iter;

        if (r[spots_.curveid] > curveid_) {
            r[spots_.curveid] = r[spots_.curveid] - 1;
        }
    }
}

void ControlSpotPanel::updateCurveOpacity(const Gtk::TreeModel::Row& selectedRow)
{
    const int curveid_ = selectedRow[spots_.curveid];

    // printf("updateCurveOpacity: %d\n", curveid_);

    if (curveid_ == 0) { // Row has no associated curve
        return;
    }

    for (int it_ = 0; it_ < (int) EditSubscriber::visibleGeometry.size(); it_++) {
        if ((it_ < ((curveid_ - 1) * GEOM_PER_SPOT)) || (it_ > ((curveid_ - 1) * GEOM_PER_SPOT) + GEOM_PER_SPOT - 1)) { // it_ does not belong to selected curve
            EditSubscriber::visibleGeometry.at(it_)->opacity = 25.;
        } else {
            EditSubscriber::visibleGeometry.at(it_)->opacity = 75.;
        }
    }
}

CursorShape ControlSpotPanel::getCursor(int objectID, int xPos, int yPos) const
{
    // printf("Object ID: %d\n", objectID);

    // When there is no control spot (i.e. no selected row), objectID can unexpectedly be different from -1 and produced not desired behavior
    const auto s = treeview_->get_selection();

    if (!s->count_selected_rows()) {
        return CSHandOpen;
    }

    const int rem_ = objectID % GEOM_PER_SPOT;

    switch (rem_) {
        case (0): // centerCircle: (curveid_ - 1) * GEOM_PER_SPOT
            return CSMove2D;

        case (1): // shape_ellipse: (curveid_ - 1) * GEOM_PER_SPOT + 1
            return CSMove2D;

        case (2): // shape_rectangle: (curveid_ - 1) * GEOM_PER_SPOT + 2
            return CSMove2D;

        case (3): // cirX: (curveid_ - 1) * GEOM_PER_SPOT + 3
            return CSMove1DH;

        case (4): // cirXL: (curveid_ - 1) * GEOM_PER_SPOT + 4
            return CSMove1DH;

        case (5): // cirY: (curveid_ - 1) * GEOM_PER_SPOT + 5
            return CSMove1DV;

        case (6): // cirYT: (curveid_ - 1) * GEOM_PER_SPOT + 6
            return CSMove1DV;

        case (7): // gradLine1: drag to adjust gradient width
        case (8): // gradLine2: drag to adjust gradient width
            return CSResizeHeight;

        case (9): // gradAngleHandle: drag to rotate gradient
            return CSMove2D;

        default:
            return CSHandOpen;
    }
}

bool ControlSpotPanel::mouseOver(int modifierKey)
{
    EditDataProvider* editProvider_ = getEditProvider();
    const auto s = treeview_->get_selection();

    if (!editProvider_ || !s->count_selected_rows()) { // When there is no control spot (i.e. no selected row), objectID can unexpectedly be different from -1 and produced not desired behavior
        return false;
    }

    // Get selected row
    const auto selIter = s->get_selected();
    const Gtk::TreeModel::Row selRow = *selIter;

    const int object_ = editProvider_->object;

    if (object_ != lastObject_) {
        if (object_ == -1) {
            // Reset mouseOver preview for visibleGeometry
            for (size_t it_ = 0; it_ < EditSubscriber::visibleGeometry.size(); it_++) {
                EditSubscriber::visibleGeometry.at(it_)->state = Geometry::NORMAL;
            }

            // Reset mouseOver preview for TreeView
            const Gtk::TreeModel::Children children = treemodel_->children();

            for (auto iter = children.begin(); iter != children.end(); iter++) {
                Gtk::TreeModel::Row row = *iter;
                row[spots_.mouseover] = false;
            }

            // Notify listener that mouse left spot area
            if (controlPanelListener) {
                controlPanelListener->spotHovered(false, false, -1);
            }

            // Actualize lastObject_
            lastObject_ = object_;
            return false;
        }

        const int curveId_ = object_ / GEOM_PER_SPOT + 1;
        const int rem = object_ % GEOM_PER_SPOT;

        // Notify listener that mouse entered a spot (selected spot only)
        if (controlPanelListener) {
            const bool isSelectedSpot = (selRow[spots_.curveid] == curveId_);
            controlPanelListener->spotHovered(
                isSelectedSpot, false, isSelectedSpot ? getSelectedSpot() : -1);
        }

        // Manage mouseOver preview for TreeView
        const Gtk::TreeModel::Children children = treemodel_->children();

        for (auto iter = children.begin(); iter != children.end(); iter++) {
            Gtk::TreeModel::Row row = *iter;

            if (row[spots_.curveid] == curveId_ && *row != *selRow) {
                row[spots_.mouseover] = true;
            } else {
                row[spots_.mouseover] = false;
            }
        }

        for (int it_ = 0; it_ < (int) EditSubscriber::visibleGeometry.size(); it_++) {
            if ((it_ < ((curveId_ - 1) * GEOM_PER_SPOT)) || (it_ > ((curveId_ - 1) * GEOM_PER_SPOT) + GEOM_PER_SPOT - 1)) { // it_ does not belong to cursor pointed curve
                EditSubscriber::visibleGeometry.at(it_)->state = Geometry::NORMAL;
            }
        }

        const int method = shapeMethod_->get_active_row_number();

        // Circle, Arcellipses and Rectangle
        if (rem >= 0 && rem < 3) {
            EditSubscriber::visibleGeometry.at((curveId_ - 1) * GEOM_PER_SPOT)->state = Geometry::PRELIGHT;
            EditSubscriber::visibleGeometry.at((curveId_ - 1) * GEOM_PER_SPOT + 1)->state = Geometry::PRELIGHT;
            EditSubscriber::visibleGeometry.at((curveId_ - 1) * GEOM_PER_SPOT + 2)->state = Geometry::PRELIGHT;
            EditSubscriber::visibleGeometry.at((curveId_ - 1) * GEOM_PER_SPOT + 3)->state = Geometry::PRELIGHT;
            EditSubscriber::visibleGeometry.at((curveId_ - 1) * GEOM_PER_SPOT + 4)->state = Geometry::PRELIGHT;
            EditSubscriber::visibleGeometry.at((curveId_ - 1) * GEOM_PER_SPOT + 5)->state = Geometry::PRELIGHT;
            EditSubscriber::visibleGeometry.at((curveId_ - 1) * GEOM_PER_SPOT + 6)->state = Geometry::PRELIGHT;
        } else {
            EditSubscriber::visibleGeometry.at((curveId_ - 1) * GEOM_PER_SPOT)->state = Geometry::NORMAL;
            EditSubscriber::visibleGeometry.at((curveId_ - 1) * GEOM_PER_SPOT + 2)->state = Geometry::NORMAL;
            EditSubscriber::visibleGeometry.at((curveId_ - 1) * GEOM_PER_SPOT + 3)->state = Geometry::NORMAL;
            EditSubscriber::visibleGeometry.at((curveId_ - 1) * GEOM_PER_SPOT + 4)->state = Geometry::NORMAL;
            EditSubscriber::visibleGeometry.at((curveId_ - 1) * GEOM_PER_SPOT + 4)->state = Geometry::NORMAL;
            EditSubscriber::visibleGeometry.at((curveId_ - 1) * GEOM_PER_SPOT + 5)->state = Geometry::NORMAL;
            EditSubscriber::visibleGeometry.at((curveId_ - 1) * GEOM_PER_SPOT + 6)->state = Geometry::NORMAL;
        }

        // cirX
        if (rem == 3) {
            EditSubscriber::visibleGeometry.at((curveId_ - 1) * GEOM_PER_SPOT + 3)->state = Geometry::PRELIGHT;

            if (method == 1 || method == 3) { // Symmetrical cases
                EditSubscriber::visibleGeometry.at((curveId_ - 1) * GEOM_PER_SPOT + 4)->state = Geometry::PRELIGHT;
            }
        }

        // cirXL
        if (rem == 4) {
            EditSubscriber::visibleGeometry.at((curveId_ - 1) * GEOM_PER_SPOT + 4)->state = Geometry::PRELIGHT;

            if (method == 1 || method == 3) { // Symmetrical cases
                EditSubscriber::visibleGeometry.at((curveId_ - 1) * GEOM_PER_SPOT + 3)->state = Geometry::PRELIGHT;
            }
        }

        // cirY
        if (rem == 5) {
            EditSubscriber::visibleGeometry.at((curveId_ - 1) * GEOM_PER_SPOT + 5)->state = Geometry::PRELIGHT;

            if (method == 1 || method == 3) { // Symmetrical cases
                EditSubscriber::visibleGeometry.at((curveId_ - 1) * GEOM_PER_SPOT + 6)->state = Geometry::PRELIGHT;
            }
        }

        // cirYT
        if (rem == 6) {
            EditSubscriber::visibleGeometry.at((curveId_ - 1) * GEOM_PER_SPOT + 6)->state = Geometry::PRELIGHT;

            if (method == 1 || method == 3) { // Symmetrical cases
                EditSubscriber::visibleGeometry.at((curveId_ - 1) * GEOM_PER_SPOT + 5)->state = Geometry::PRELIGHT;
            }
        }

        // Gradient geometry (lines and angle handle)
        if (rem >= 7 && rem <= 9) {
            EditSubscriber::visibleGeometry.at((curveId_ - 1) * GEOM_PER_SPOT + 7)->state = Geometry::PRELIGHT;
            EditSubscriber::visibleGeometry.at((curveId_ - 1) * GEOM_PER_SPOT + 8)->state = Geometry::PRELIGHT;
            EditSubscriber::visibleGeometry.at((curveId_ - 1) * GEOM_PER_SPOT + 9)->state = Geometry::PRELIGHT;
        }

        lastObject_ = object_;
        return true;
    }

    return false;
}

//-----------------------------------------------------------------------------
// Magnetic snap: snap a point to the nearest strong edge in the preview
//-----------------------------------------------------------------------------
rtengine::Coord ControlSpotPanel::magneticSnap(int imgX, int imgY)
{
    EditDataProvider* provider = getEditProvider();
    if (!provider) return {imgX, imgY};

    // Access preview pixbuf through ImageArea → CropWindow → CropHandler
    auto* imageArea = static_cast<ImageArea*>(provider);
    CropWindow* mainCrop = imageArea->getMainCropWindow();
    if (!mainCrop) return {imgX, imgY};

    auto pixbuf = mainCrop->cropHandler.cropPixbuf;
    if (!pixbuf) return {imgX, imgY};

    const int pbW = pixbuf->get_width();
    const int pbH = pixbuf->get_height();
    const guint8* data = pixbuf->get_pixels();
    const int rowstride = pixbuf->get_rowstride();

    // Convert image coords to crop image (pixbuf) coords
    int cx, cy;
    mainCrop->imageCoordToCropImage(imgX, imgY, cx, cy);

    // Get scale factor: image pixels per preview pixel
    const auto cropSz = mainCrop->cropHandler.getSize();
    if (cropSz.width <= 0 || cropSz.height <= 0) return {imgX, imgY};
    const float scaleX = static_cast<float>(cropSz.width) / static_cast<float>(pbW);
    const float scaleY = static_cast<float>(cropSz.height) / static_cast<float>(pbH);

    // Get tolerance from the selected spot
    int radius = 10;
    const auto s = treeview_->get_selection();
    if (s->count_selected_rows()) {
        const auto iter = s->get_selected();
        Gtk::TreeModel::Row row = *iter;
        radius = static_cast<int>(row[spots_.polyMaskSnapTolerance]);
    }
    if (radius <= 0) return {imgX, imgY}; // snapping disabled
    float bestMag = 0.f;
    int bestDx = 0, bestDy = 0;

    // Luminance helper
    auto lum = [&](int x, int y) -> float {
        const guint8* p = data + rowstride * y + x * 3;
        return 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
    };

    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            if (dx * dx + dy * dy > radius * radius) continue;

            const int px = cx + dx;
            const int py = cy + dy;

            // Need 1px border for Sobel kernel
            if (px < 1 || px >= pbW - 1 || py < 1 || py >= pbH - 1) continue;

            // Sobel gradient magnitude
            const float gx = -lum(px-1,py-1) + lum(px+1,py-1)
                         - 2*lum(px-1,py)   + 2*lum(px+1,py)
                         -   lum(px-1,py+1) + lum(px+1,py+1);
            const float gy = -lum(px-1,py-1) - 2*lum(px,py-1) - lum(px+1,py-1)
                         +   lum(px-1,py+1) + 2*lum(px,py+1) + lum(px+1,py+1);
            const float mag = gx * gx + gy * gy;

            if (mag > bestMag) {
                bestMag = mag;
                bestDx = dx;
                bestDy = dy;
            }
        }
    }

    // Only snap if the edge is strong enough (threshold avoids snapping in flat areas)
    if (bestMag < 500.f) return {imgX, imgY};

    // Convert preview offset back to image coords
    return {imgX + static_cast<int>(bestDx * scaleX),
            imgY + static_cast<int>(bestDy * scaleY)};
}

//-----------------------------------------------------------------------------
// Ramer-Douglas-Peucker polygon simplification
//-----------------------------------------------------------------------------
void ControlSpotPanel::simplifyPolygon(std::vector<rtengine::Coord>& pts, double epsilon)
{
    if (pts.size() < 3) return;

    // Find the point farthest from the line between first and last
    const auto perpDist = [](const Coord& pt, const Coord& a, const Coord& b) -> double {
        const double dx = b.x - a.x;
        const double dy = b.y - a.y;
        const double lenSq = dx * dx + dy * dy;
        if (lenSq < 1e-10) {
            const double ex = pt.x - a.x;
            const double ey = pt.y - a.y;
            return std::sqrt(ex * ex + ey * ey);
        }
        return std::abs(dy * pt.x - dx * pt.y + b.x * a.y - b.y * a.x) / std::sqrt(lenSq);
    };

    double maxDist = 0;
    size_t maxIdx = 0;
    for (size_t i = 1; i + 1 < pts.size(); i++) {
        const double d = perpDist(pts[i], pts.front(), pts.back());
        if (d > maxDist) {
            maxDist = d;
            maxIdx = i;
        }
    }

    if (maxDist > epsilon) {
        // Recursively simplify both halves
        std::vector<Coord> left(pts.begin(), pts.begin() + maxIdx + 1);
        std::vector<Coord> right(pts.begin() + maxIdx, pts.end());
        simplifyPolygon(left, epsilon);
        simplifyPolygon(right, epsilon);

        pts.clear();
        pts.insert(pts.end(), left.begin(), left.end() - 1);
        pts.insert(pts.end(), right.begin(), right.end());
    } else {
        // All intermediate points are within tolerance — keep only endpoints
        Coord first = pts.front();
        Coord last = pts.back();
        pts.clear();
        pts.push_back(first);
        pts.push_back(last);
    }
}

bool ControlSpotPanel::button1Pressed(int modifierKey)
{
    EditDataProvider *provider = getEditProvider();
    const auto s = treeview_->get_selection();

    // Handle polygon freehand drawing mode — start a drag gesture
    if (polyDrawing_ && provider && s->count_selected_rows()) {
        const int px = static_cast<int>(provider->posImage.x + provider->deltaImage.x);
        const int py = static_cast<int>(provider->posImage.y + provider->deltaImage.y);

        // Snap first point to nearest edge
        const auto snapped = magneticSnap(px, py);

        polyTempPoints_.clear();
        polyTempPoints_.push_back(snapped);
        polyDragging_ = true;

        // Update polyline geometry
        const auto iter_sel = s->get_selected();
        Gtk::TreeModel::Row row = *iter_sel;
        const int curveid_ = row[spots_.curveid];
        if (curveid_ > 0) {
            const int base = (curveid_ - 1) * GEOM_PER_SPOT;
            auto* visPoly = static_cast<Polyline*>(visibleGeometry.at(base + 10));
            visPoly->points.clear();
            visPoly->points.push_back(snapped);
            mouseOverGeometry.at(base + 10)->setActive(true);
            visibleGeometry.at(base + 10)->setActive(true);
        }

        EditSubscriber::action = EditSubscriber::Action::DRAGGING;
        return true;
    }

    if (!provider || lastObject_ == -1 || !s->count_selected_rows()) {
        return false;
    }

    // Select associated control spot
    const int curveId_ = lastObject_ / GEOM_PER_SPOT + 1;
    Gtk::TreeModel::Children children = treemodel_->children();

    for (auto iter = children.begin(); iter != children.end(); iter++) {
        const Gtk::TreeModel::Row r = *iter;

        if (r[spots_.curveid] == curveId_) {
            treeview_->set_cursor(treemodel_->get_path(r));
            break;
        }
    }

    lastCoord_.set(provider->posImage.x + provider->deltaImage.x, provider->posImage.y + provider->deltaImage.y);
    EditSubscriber::action = EditSubscriber::Action::DRAGGING;
    return true;
}

bool ControlSpotPanel::button1Released()
{
    // Handle polygon freehand draw release — close and store the polygon
    if (polyDragging_ && polyDrawing_) {
        polyDragging_ = false;
        EditSubscriber::action = EditSubscriber::Action::NONE;

        const auto s = treeview_->get_selection();
        if (s->count_selected_rows() && polyTempPoints_.size() >= 3) {
            // Simplify the polygon to reduce point count
            simplifyPolygon(polyTempPoints_, 3.0);

            const auto iter = s->get_selected();
            Gtk::TreeModel::Row row = *iter;

            std::vector<int> pts;
            pts.reserve(polyTempPoints_.size() * 2);
            for (const auto& p : polyTempPoints_) {
                pts.push_back(p.x);
                pts.push_back(p.y);
            }
            row[spots_.polyMaskPoints] = pts;
            polyVertexLabel_->set_text(Glib::ustring::compose("%1", polyTempPoints_.size()));

            updateControlSpotCurve(row);

            if (listener) {
                listener->panelChanged(EvLocallabSpotShape, M("TP_LOCALLAB_POLY"));
            }
        }
        polyTempPoints_.clear();

        // Deactivate draw mode after completing a polygon
        polyDrawing_ = false;
        polyDrawConn_.block(true);
        polyDrawBtn_->set_active(false);
        polyDrawConn_.block(false);

        return true;
    }

    EditSubscriber::action = EditSubscriber::Action::NONE;
    return true;
}

bool ControlSpotPanel::drag1(int modifierKey)
{
    EditDataProvider *provider = getEditProvider();
    const auto s = treeview_->get_selection();

    // Handle polygon freehand drawing — add points along the drag path
    if (polyDragging_ && polyDrawing_ && provider && s->count_selected_rows()) {
        const int px = static_cast<int>(provider->posImage.x + provider->deltaImage.x);
        const int py = static_cast<int>(provider->posImage.y + provider->deltaImage.y);

        // Only add point if far enough from the last one (configurable leg length)
        const double legLen = polyLegLen_->getValue();
        if (!polyTempPoints_.empty()) {
            const auto& last = polyTempPoints_.back();
            const double dx = px - last.x;
            const double dy = py - last.y;
            if (dx * dx + dy * dy < legLen * legLen) {
                return false; // too close, skip
            }
        }

        // Magnetic snap to nearest edge
        const auto snapped = magneticSnap(px, py);
        polyTempPoints_.push_back(snapped);

        // Update the visible polyline
        const auto iter_sel = s->get_selected();
        Gtk::TreeModel::Row row = *iter_sel;
        const int curveid_ = row[spots_.curveid];
        if (curveid_ > 0) {
            const int base = (curveid_ - 1) * GEOM_PER_SPOT;
            auto* visPoly = static_cast<Polyline*>(visibleGeometry.at(base + 10));
            visPoly->points.push_back(snapped);
        }

        return true;
    }

    if (!provider || lastObject_ == -1 || !s->count_selected_rows()) {
        return false;
    }

    const auto iter = s->get_selected();
    Gtk::TreeModel::Row row = *iter;

    int imW, imH;
    provider->getImageSize(imW, imH);
    const int rem = lastObject_ % GEOM_PER_SPOT;
    const int method = shapeMethod_->get_active_row_number();
    Coord newCoord = Coord(provider->posImage.x + provider->deltaImage.x, provider->posImage.y + provider->deltaImage.y);

    // Circle, Ellipses and Rectangle
    if (rem >= 0 && rem < 3) {
        double deltaX = (double (newCoord.x) - double (lastCoord_.x)) * 2000. / double (imW);
        double deltaY = (double (newCoord.y) - double (lastCoord_.y)) * 2000. / double (imH);
        centerX_->setValue(centerX_->getValue() + deltaX);
        centerY_->setValue(centerY_->getValue() + deltaY);
        row[spots_.centerX] = centerX_->getIntValue();
        row[spots_.centerY] = centerY_->getIntValue();

        updateControlSpotCurve(row);

        if (listener) {
            listener->panelChanged(EvLocallabSpotCenter, "X=" + centerX_->getTextValue() + ", Y=" + centerY_->getTextValue());
        }
    }

    // cirX
    if (rem == 3) {
        double deltaX = (double (newCoord.x) - double (lastCoord_.x)) * 2000. / double (imW);
        locX_->setValue(locX_->getValue() + deltaX);
        row[spots_.locX] = locX_->getIntValue();

        if (method == 1 || method == 3) { // Symmetrical cases
            disableParamlistener(true);
            locXL_->setValue(locX_->getValue());
            disableParamlistener(false);
            row[spots_.locXL] = locXL_->getIntValue();
        }

        updateControlSpotCurve(row);

        if (listener) {
            listener->panelChanged(EvLocallabSpotLocX, locX_->getTextValue());
        }
    }

    // cirXL
    if (rem == 4) {
        double deltaXL = (double (lastCoord_.x) - double (newCoord.x)) * 2000. / double (imW);
        locXL_->setValue(locXL_->getValue() + deltaXL);
        row[spots_.locXL] = locXL_->getIntValue();

        if (method == 1 || method == 3) { // Symmetrical cases
            disableParamlistener(true);
            locX_->setValue(locXL_->getValue());
            disableParamlistener(false);
            row[spots_.locX] = locX_->getIntValue();
        }

        updateControlSpotCurve(row);

        if (listener) {
            listener->panelChanged(EvLocallabSpotLocXL, locXL_->getTextValue());
        }
    }

    // cirY
    if (rem == 5) {
        double deltaY = (double (newCoord.y) - double (lastCoord_.y)) * 2000. / double (imH);
        locY_->setValue(locY_->getValue() + deltaY);
        row[spots_.locY] = locY_->getIntValue();

        if (method == 1 || method == 3) { // Symmetrical cases
            disableParamlistener(true);
            locYT_->setValue(locY_->getValue());
            disableParamlistener(false);
            row[spots_.locYT] = locYT_->getIntValue();
        }

        updateControlSpotCurve(row);

        if (listener) {
            listener->panelChanged(EvLocallabSpotLocY, locY_->getTextValue());
        }
    }

    // cirYT
    if (rem == 6) {
        double deltaYT = (double (lastCoord_.y) - double (newCoord.y)) * 2000. / double (imH);
        locYT_->setValue(locYT_->getValue() + deltaYT);
        row[spots_.locYT] = locYT_->getIntValue();

        if (method == 1 || method == 3) { // Symmetrical cases
            disableParamlistener(true);
            locY_->setValue(locYT_->getValue());
            disableParamlistener(false);
            row[spots_.locY] = locY_->getIntValue();
        }

        updateControlSpotCurve(row);

        if (listener) {
            listener->panelChanged(EvLocallabSpotLocYT, locYT_->getTextValue());
        }
    }

    // Gradient line drag — adjust transit (width of gradient transition)
    if (rem == 7 || rem == 8) {
        const double cx = (double)imW / 2. + centerX_->getValue() * (double)imW / 2000.;
        const double cy = (double)imH / 2. + centerY_->getValue() * (double)imH / 2000.;
        const double theta = gradangle_->getValue() * rtengine::RT_PI / 180.0;
        const double sinT = std::sin(theta);
        const double cosT = std::cos(theta);
        // Project mouse position onto gradient direction from center
        const double dx = (double)newCoord.x - cx;
        const double dy = (double)newCoord.y - cy;
        const double proj = std::abs(dx * sinT - dy * cosT);
        // Compute maxProj (matching engine and visualization)
        const double imgMaxX = std::max(cx, (double)imW - cx);
        const double imgMaxY = std::max(cy, (double)imH - cy);
        const double maxProj = imgMaxX * std::abs(sinT) + imgMaxY * std::abs(cosT);
        // Convert distance to transit value: proj = maxProj * (transit/100)
        if (maxProj > 0.001) {
            double newTransit = (proj / maxProj) * 100.0;
            newTransit = rtengine::LIM(newTransit, 0.5, 100.0);
            transit_->setValue(newTransit);
            row[spots_.transit] = transit_->getValue();
        }

        updateControlSpotCurve(row);

        if (listener) {
            listener->panelChanged(EvLocallabSpotTransit, transit_->getTextValue());
        }
    }

    // Gradient angle handle drag
    if (rem == 9) {
        // Compute center position in image coords
        const double cx = (double)imW / 2. + centerX_->getValue() * (double)imW / 2000.;
        const double cy = (double)imH / 2. + centerY_->getValue() * (double)imH / 2000.;
        // Compute angle from center to current mouse position
        const double dx = (double)newCoord.x - cx;
        const double dy = cy - (double)newCoord.y; // Y is inverted in image coords
        double angle = std::atan2(dx, dy) * 180.0 / rtengine::RT_PI;
        // Clamp to -180..180
        if (angle > 180.0) {
            angle -= 360.0;
        } else if (angle < -180.0) {
            angle += 360.0;
        }
        gradangle_->setValue(angle);
        row[spots_.gradangle] = gradangle_->getValue();

        updateControlSpotCurve(row);

        if (listener) {
            listener->panelChanged(EvLocallabSpotGradAngle, gradangle_->getTextValue());
        }
    }

    lastCoord_.set(newCoord.x, newCoord.y);
    return true;
}

int ControlSpotPanel::getEventType()
{
    const int tmp = eventType;
    eventType = None; // Re-initialization at "None" if event type gotten
    return tmp;
}

std::unique_ptr<ControlSpotPanel::SpotRow> ControlSpotPanel::getSpot(const int index)
{
    // TODO: Return an std::optional<ControlSpotPanel::SpotRow> after upgrading
    // to C++17.

    // printf("getSpot: %d\n", index);

    MyMutex::MyLock lock(mTreeview);

    std::unique_ptr<SpotRow> r(new SpotRow());

    int i = -1;

    for (auto &row : treemodel_->children()) {
        i++;

        if (i == index) {
            r->name = row[spots_.name];
            r->isvisible = row[spots_.isvisible];
            r->prevMethod = row[spots_.prevMethod];
            r->shape = row[spots_.shape];
            r->spotMethod = row[spots_.spotMethod];
//           r->mergeMethod = row[spots_.mergeMethod];
            r->sensiexclu = row[spots_.sensiexclu];
            r->structexclu = row[spots_.structexclu];
            r->struc = row[spots_.struc];
            r->shapeMethod = row[spots_.shapeMethod];
            r->locX = row[spots_.locX];
            r->locXL = row[spots_.locXL];
            r->locY = row[spots_.locY];
            r->locYT = row[spots_.locYT];
            r->centerX = row[spots_.centerX];
            r->centerY = row[spots_.centerY];
            r->circrad = row[spots_.circrad];
            r->qualityMethod = row[spots_.qualityMethod];
            r->complexMethod = row[spots_.complexMethod];
            r->transit = row[spots_.transit];
            r->feather = row[spots_.feather];
            r->thresh = row[spots_.thresh];
            r->iter = row[spots_.iter];
            r->balan = row[spots_.balan];
            r->balanh = row[spots_.balanh];
            r->colorde = row[spots_.colorde];
            r->colorscope = row[spots_.colorscope];
            r->avoidrad = row[spots_.avoidrad];
            r->transitweak = row[spots_.transitweak];
            r->transitgrad = row[spots_.transitgrad];
            r->gradangle = row[spots_.gradangle];
            r->scopemask = row[spots_.scopemask];
            r->denoichmask = row[spots_.denoichmask];
            r->lumask = row[spots_.lumask];
            r->hishow = row[spots_.hishow];
            r->activ = row[spots_.activ];
            r->avoidneg = row[spots_.avoidneg];
            r->blwh = row[spots_.blwh];
            r->recurs = row[spots_.recurs];
            r->laplac = row[spots_.laplac];
            r->deltae = row[spots_.deltae];
            r->shortc = row[spots_.shortc];
            //r->savrest = row[spots_.savrest];
            r->wavMethod = row[spots_.wavMethod];
            r->avoidgamutMethod = row[spots_.avoidgamutMethod];
            r->maskType = row[spots_.maskType];
            r->aiMaskClass = row[spots_.aiMaskClass];
            r->aiMaskThreshold = row[spots_.aiMaskThreshold];
            r->maskBlendMode = row[spots_.maskBlendMode];
            r->polyMaskPoints = row[spots_.polyMaskPoints];
            r->polyMaskFeather = row[spots_.polyMaskFeather];
            r->polyMaskSnapTolerance = row[spots_.polyMaskSnapTolerance];
            r->polyMaskLegLength = row[spots_.polyMaskLegLength];

            return r;
        }
    }

    return nullptr;
}

int ControlSpotPanel::getSpotNumber()
{
    // printf("getSpotNumber\n");

    return (int)treemodel_->children().size();
}

int ControlSpotPanel::getSelectedSpot()
{
    // printf("getSelectedSpot\n");

    MyMutex::MyLock lock(mTreeview);

    const auto s = treeview_->get_selection();

    // Check if treeview has row, otherwise return 0
    if (!s->count_selected_rows()) {
        return -1;
    }

    const auto selRow = s->get_selected();

    // Get selected spot index
    int index = -1;

    for (auto i : treemodel_->children()) {
        index++;

        if (selRow == i) {
            return index;
        }
    }

    return -1;
}

bool ControlSpotPanel::setSelectedSpot(const int index)
{
    MyMutex::MyLock lock(mTreeview);

    int i = -1;

    for (auto &row : treemodel_->children()) {
        i++;

        if (i == index) {
            disableParamlistener(true);

            treeview_->set_cursor(treemodel_->get_path(row));
            load_ControlSpot_param();
            updateParamVisibility();
            updateCurveOpacity(row);

            disableParamlistener(false);

            return true;
        }
    }

    return false;
}

bool ControlSpotPanel::isDeltaEPrevActive()
{
    return (preview_->get_active());
}

void ControlSpotPanel::resetDeltaEPreview()
{
    previewConn_.block(true);
    preview_->set_active(false);
    previewConn_.block(false);
}

void ControlSpotPanel::addControlSpot(const SpotRow &newSpot)
{
    // printf("addControlSpot: %d\n", newSpot.name);

    MyMutex::MyLock lock(mTreeview);

    disableParamlistener(true);
    for (auto& existingRow : treemodel_->children()) {
        existingRow[spots_.detailsExpanded] = false;
    }
    Gtk::TreeModel::Row row = *(treemodel_->append());
    row[spots_.mouseover] = false;
    row[spots_.detailsExpanded] = false;
    row[spots_.name] = newSpot.name;
    row[spots_.nameAutomatic] = isAutomaticMaskName(newSpot.name);
    row[spots_.isvisible] = newSpot.isvisible;
    row[spots_.curveid] = 0; // No associated curve
    row[spots_.prevMethod] = newSpot.prevMethod;
    row[spots_.shape] = newSpot.shape;
    row[spots_.spotMethod] = newSpot.spotMethod;
    row[spots_.sensiexclu] = newSpot.sensiexclu;
    row[spots_.structexclu] = newSpot.structexclu;
    row[spots_.shapeMethod] = newSpot.shapeMethod;
    row[spots_.locX] = newSpot.locX;
    row[spots_.locXL] = newSpot.locXL;
    row[spots_.locY] = newSpot.locY;
    row[spots_.locYT] = newSpot.locYT;
    row[spots_.centerX] = newSpot.centerX;
    row[spots_.centerY] = newSpot.centerY;
    row[spots_.circrad] = newSpot.circrad;
    row[spots_.qualityMethod] = newSpot.qualityMethod;
    row[spots_.transit] = newSpot.transit;
    row[spots_.transitweak] = newSpot.transitweak;
    row[spots_.transitgrad] = newSpot.transitgrad;
    row[spots_.gradangle] = newSpot.gradangle;
    row[spots_.feather] = newSpot.feather;
    row[spots_.struc] = newSpot.struc;
    row[spots_.thresh] = newSpot.thresh;
    row[spots_.iter] = newSpot.iter;
    row[spots_.balan] = newSpot.balan;
    row[spots_.balanh] = newSpot.balanh;
    row[spots_.colorde] = newSpot.colorde;
    row[spots_.colorscope] = newSpot.colorscope;
    row[spots_.avoidrad] = newSpot.avoidrad;
    row[spots_.hishow] = newSpot.hishow;
    row[spots_.activ] = newSpot.activ;
    row[spots_.avoidneg] = newSpot.avoidneg;
    row[spots_.blwh] = newSpot.blwh;
    row[spots_.recurs] = newSpot.recurs;
    row[spots_.laplac] = newSpot.laplac;
    row[spots_.deltae] = newSpot.deltae;
    row[spots_.scopemask] = newSpot.scopemask;
    row[spots_.denoichmask] = newSpot.denoichmask;
    row[spots_.shortc] = newSpot.shortc;
    row[spots_.lumask] = newSpot.lumask;
    //row[spots_.savrest] = newSpot.savrest;
    row[spots_.complexMethod] = newSpot.complexMethod;
    row[spots_.wavMethod] = newSpot.wavMethod;
    row[spots_.avoidgamutMethod] = newSpot.avoidgamutMethod;
    row[spots_.maskType] = newSpot.maskType;
    row[spots_.aiMaskClass] = newSpot.aiMaskClass;
    row[spots_.aiMaskThreshold] = newSpot.aiMaskThreshold;
    row[spots_.maskBlendMode] = newSpot.maskBlendMode;
    row[spots_.polyMaskPoints] = newSpot.polyMaskPoints;
    row[spots_.polyMaskFeather] = newSpot.polyMaskFeather;
    row[spots_.polyMaskSnapTolerance] = newSpot.polyMaskSnapTolerance;
    row[spots_.polyMaskLegLength] = newSpot.polyMaskLegLength;
    updateParamVisibility();
    disableParamlistener(false);

    // Enable mask controls now that a spot exists
    setMaskControlsSensitive(true);

    // Add associated control spot curve
    addControlSpotCurve(row);
    updateControlSpotCurve(row);
}

void ControlSpotPanel::deleteControlSpot(const int index)
{
    // printf("deleteControlSpot: %d\n", index);

    MyMutex::MyLock lock(mTreeview);

    disableParamlistener(true);

    int i = -1;

    for (auto iter : treemodel_->children()) {
        i++;

        if (i == index) {
            Gtk::TreeModel::Row row = *iter;
            deleteControlSpotCurve(row);
            treemodel_->erase(*row);
            break;
        }
    }

    disableParamlistener(false);

    // Grey out mask controls when no spots remain
    if (treemodel_->children().empty()) {
        setMaskControlsSensitive(false);
        setMaskDetailExpanded(false);
    }
}

//new function linked to Global and options 
void ControlSpotPanel::updateguiset(int spottype, bool iscolor, bool issh, bool isvib, bool isexpos, bool issoft, bool isblur, bool istom, bool isret, bool issharp, bool iscont, bool iscbdl, bool islog, bool ismas, bool isci)
{
    {  //with this function we can 1) activate Settings SpotMethod
        // also if need GUI for mask ,  todo...
        idle_register.add(
        [this, spottype, iscolor, issh , isvib, isexpos, issoft, isblur, istom, isret, issharp, iscont, iscbdl, islog, ismas, isci]() -> bool {
            GThreadLock lock; // All GUI access from idle_add callbacks or separate thread HAVE to be protected

            // Update GUI fullimage or main
            disableListener();
            if(spottype >= 2  && App::get().options().spotmet >= 2) {//optimize update
                spotMethodChanged();
            }
            
            if((iscolor || issh || isvib || isexpos || istom || iscont || islog || ismas || isci)
                && !issharp && !issoft && !isret && !isblur  & !iscbdl) {
                preview_->hide();               
            } else if (issoft || isblur || isret || issharp || iscbdl) {
                preview_->show(); 
            }
            enableListener();

        return false;
        }
        );
    }
   
}
//new function linked to change scope
void ControlSpotPanel::updateguiscopeset(int scope)
{
    {  //with this function we can disabled old values scope 
        idle_register.add(
        [this, scope]() -> bool {
            GThreadLock lock; // All GUI access from idle_add callbacks or separate thread HAVE to be protected

            disableListener();
            colorscope_->setValue(scope);
            adjusterChanged(colorscope_, 0.);
            enableListener();

        return false;
        }
        );
    }
   
}


void ControlSpotPanel::setDefaults(const rtengine::procparams::ProcParams * defParams, const ParamsEdited * pedited)
{
    const int index = defParams->locallab.selspot;

    if (index < (int)defParams->locallab.spots.size()) {
        const LocallabParams::LocallabSpot defSpot = defParams->locallab.spots.at(index);

        // Set default values for adjuster widgets
        sensiexclu_->setDefault((double)defSpot.sensiexclu);
        structexclu_->setDefault((double)defSpot.structexclu);
        locX_->setDefault((double)defSpot.loc.at(0));
        locXL_->setDefault((double)defSpot.loc.at(1));
        locY_->setDefault((double)defSpot.loc.at(2));
        locYT_->setDefault((double)defSpot.loc.at(3));
        centerX_->setDefault((double)defSpot.centerX);
        centerY_->setDefault((double)defSpot.centerY);
        circrad_->setDefault((double)defSpot.circrad);
        transit_->setDefault(defSpot.transit);
        transitweak_->setDefault(defSpot.transitweak);
        transitgrad_->setDefault(defSpot.transitgrad);
        gradangle_->setDefault(defSpot.gradangle);
        feather_->setDefault(defSpot.feather);
        struc_->setDefault(defSpot.struc);
        thresh_->setDefault(defSpot.thresh);
        iter_->setDefault(defSpot.iter);
        balan_->setDefault(defSpot.balan);
        balanh_->setDefault(defSpot.balanh);
        colorde_->setDefault(defSpot.colorde);
        colorscope_->setDefault(defSpot.colorscope);
        avoidrad_->setDefault(defSpot.avoidrad);
        scopemask_->setDefault((double)defSpot.scopemask);
        denoichmask_->setDefault((double)defSpot.denoichmask);
        lumask_->setDefault((double)defSpot.lumask);
    }

    // Note: No need to manage pedited as batch mode is deactivated for Locallab
}

//-----------------------------------------------------------------------------
// ControlSpots
//-----------------------------------------------------------------------------

ControlSpotPanel::ControlSpots::ControlSpots()
{
    add(mouseover);
    add(detailsExpanded);
    add(name);
    add(nameAutomatic);
    add(isvisible);
    add(curveid);
    add(prevMethod);
    add(shape);
    add(spotMethod);
    add(sensiexclu);
    add(structexclu);
    add(shapeMethod);
    add(locX);
    add(locXL);
    add(locYT);
    add(locY);
    add(centerX);
    add(centerY);
    add(circrad);
    add(qualityMethod);
    add(transit);
    add(transitweak);
    add(transitgrad);
    add(gradangle);
    add(feather);
    add(struc);
    add(thresh);
    add(iter);
    add(balan);
    add(balanh);
    add(colorde);
    add(colorscope);
    add(avoidrad);
    add(hishow);
    add(activ);
    add(avoidneg);
    add(blwh);
    add(recurs);
    add(laplac);
    add(deltae);
    add(scopemask);
    add(denoichmask);
    add(shortc);
    add(lumask);
    //add(savrest);
    add(complexMethod);
    add(wavMethod);
	add(avoidgamutMethod);
    add(maskType);
    add(aiMaskClass);
    add(aiMaskThreshold);
    add(maskBlendMode);
    add(polyMaskPoints);
    add(polyMaskFeather);
    add(polyMaskSnapTolerance);
    add(polyMaskLegLength);
}

//-----------------------------------------------------------------------------
// RenameDialog
//-----------------------------------------------------------------------------

ControlSpotPanel::RenameDialog::RenameDialog(const Glib::ustring &actualname, Gtk::Window &parent):
    Gtk::Dialog(M("TP_LOCALLAB_REN_DIALOG_NAME"), parent),

    newname_(Gtk::manage(new Gtk::Entry()))
{
    // Entry widget
    Gtk::Box* const hb = Gtk::manage(new Gtk::Box());
    hb->pack_start(*Gtk::manage(new Gtk::Label(M("TP_LOCALLAB_REN_DIALOG_LAB"))), false, false, 4);
    newname_->set_text(actualname);
    hb->pack_start(*newname_);
    get_content_area()->pack_start(*hb, Gtk::PACK_SHRINK, 4);

    // OK/CANCEL buttons
    add_button(M("GENERAL_OK"), OkButton);
    add_button(M("GENERAL_CANCEL"), CancelButton);

    // Set OK button as default one when pressing enter
    newname_->set_activates_default();
    set_default_response(OkButton);

    show_all_children();
}

Glib::ustring ControlSpotPanel::RenameDialog::get_new_name()
{
    return newname_->get_text();
}
