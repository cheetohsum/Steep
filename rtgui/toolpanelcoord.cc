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
 *  along with RawTherapee.  If not, see <https://www.gnu.org/licenses/>.
 */
#include <iostream>

#include "multilangmgr.h"
#include "toolpanelcoord.h"
#include "metadatapanel.h"
#include "options.h"
#include "rtimage.h"

#include "rtengine/imagesource.h"
#include "rtengine/dfmanager.h"
#include "rtengine/ffmanager.h"
#include "rtengine/improcfun.h"
#include "rtengine/perspectivecorrection.h"
#include "rtengine/procevents.h"
#include "rtengine/refreshmap.h"

using namespace rtengine::procparams;

using Tool = ToolPanelCoordinator::Tool;
using ToolTree = ToolPanelCoordinator::ToolTree;

const std::vector<ToolTree> EXPOSURE_PANEL_TOOLS = {
    {
        .id = Tool::TONE_CURVE,
        .children = {},
    },
    {
        .id = Tool::SHADOWS_HIGHLIGHTS,
        .children = {},
    },
    {
        .id = Tool::TONE_EQUALIZER,
        .children = {},
    },
    {
        .id = Tool::EPD,
        .children = {},
    },
    {
        .id = Tool::FATTAL,
        .children = {},
    },
    {
        .id = Tool::PC_VIGNETTE,
        .children = {},
    },
    {
        .id = Tool::GRADIENT,
        .children = {},
    },
    {
        .id = Tool::L_CURVE,
        .children = {},
    },
};

const std::vector<ToolTree> DETAILS_PANEL_TOOLS = {
    {
        .id = Tool::SPOT,
        .children = {},
    },
    {
        .id = Tool::SHARPENING_TOOL,
        .children = {},
    },
    {
        .id = Tool::LOCAL_CONTRAST,
        .children = {},
    },
    {
        .id = Tool::SHARPEN_EDGE,
        .children = {},
    },
    {
        .id = Tool::SHARPEN_MICRO,
        .children = {},
    },
    {
        .id = Tool::IMPULSE_DENOISE,
        .children = {},
    },
    {
        .id = Tool::DIR_PYR_DENOISE,
        .children = {},
    },
    {
        .id = Tool::AI_DENOISE,
        .children = {},
    },
    {
        .id = Tool::DEFRINGE_TOOL,
        .children = {},
    },
    {
        .id = Tool::DIR_PYR_EQUALIZER,
        .children = {},
    },
    {
        .id = Tool::DEHAZE,
        .children = {},
    },
};

const std::vector<ToolTree> COLOR_PANEL_TOOLS = {
    {
        .id = Tool::WHITE_BALANCE,
        .children = {},
    },
    {
        .id = Tool::COMPRESSGAMUT_TOOL,
    },
    {
        .id = Tool::VIBRANCE,
        .children = {},
    },
    {
        .id = Tool::CH_MIXER,
        .children = {},
    },
    {
        .id = Tool::BLACK_WHITE,
        .children = {},
    },
    {
        .id = Tool::HSV_EQUALIZER,
        .children = {},
    },
    {
        .id = Tool::POINT_COLOR,
        .children = {},
    },
    {
        .id = Tool::FILM_SIMULATION,
        .children = {},
    },
    {
        .id = Tool::FILM_NEGATIVE,
        .children = {},
    },
    {
        .id = Tool::SOFT_LIGHT,
        .children = {},
    },
    {
        .id = Tool::RGB_CURVES,
        .children = {},
    },
    {
        .id = Tool::COLOR_TONING,
        .children = {},
    },
    {
        .id = Tool::COLOR_GRADING,
        .children = {},
    },
    {
        .id = Tool::ICM,
        .children = {},
    },
};

const std::vector<ToolTree> ADVANCED_PANEL_TOOLS = {
    {
        .id = Tool::RETINEX_TOOL,
        .children = {},
    },
    {
        .id = Tool::COLOR_APPEARANCE,
        .children = {},
    },
    {
        .id = Tool::WAVELET,
        .children = {},
    },
};

const std::vector<ToolTree> LOCALLAB_PANEL_TOOLS = {
    {
        .id = Tool::LOCALLAB,
        .children = {},
    },
};

const std::vector<ToolTree> TRANSFORM_PANEL_TOOLS = {
    {
        .id = Tool::CROP_TOOL,
        .children = {},
    },
    {
        .id = Tool::RESIZE_TOOL,
        .children = {
            {
                .id = Tool::PR_SHARPENING,
                .children = {},
            },
            {
                .id = Tool::FRAMING,
                .children = {},
            },
        },
    },
    {
        .id = Tool::LENS_GEOM,
        .children = {
            {
                .id = Tool::ROTATE,
                .children = {},
            },
            {
                .id = Tool::PERSPECTIVE,
                .children = {},
            },
            {
                .id = Tool::LENS_PROF,
                .children = {},
            },
            {
                .id = Tool::DISTORTION,
                .children = {},
            },
            {
                .id = Tool::CA_CORRECTION,
                .children = {},
            },
            {
                .id = Tool::VIGNETTING,
                .children = {},
            },
        },
    },
};

const std::vector<ToolTree> RAW_PANEL_TOOLS = {
    {
        .id = Tool::PD_SHARPENING,
        .children = {},
    },

    {
        .id = Tool::SENSOR_BAYER,
        .children = {
            {
                {
                    .id = Tool::BAYER_PROCESS,
                    .children = {},
                },
                {
                    .id = Tool::BAYER_RAW_EXPOSURE,
                    .children = {},
                },
                {
                    .id = Tool::BAYER_PREPROCESS,
                    .children = {},
                },
                {
                    .id = Tool::RAW_CA_CORRECTION,
                    .children = {},
                },
            },
        },
    },
    {
        .id = Tool::SENSOR_XTRANS,
        .children = {
            {
                {
                    .id = Tool::XTRANS_PROCESS,
                    .children = {},
                },
                {
                    .id = Tool::XTRANS_RAW_EXPOSURE,
                    .children = {},
                },
            },
        },
    },
    {
        .id = Tool::RAW_EXPOSURE,
        .children = {},
    },
    {
        .id = Tool::PREPROCESS_WB,
        .children = {},
    },
    {
        .id = Tool::PREPROCESS,
        .children = {},
    },
    {
        .id = Tool::DARKFRAME_TOOL,
        .children = {},
    },
    {
        .id = Tool::FLATFIELD_TOOL,
        .children = {},
    },
};

const ToolPanelCoordinator::ToolLayout PANEL_TOOLS = {
    {
        ToolPanelCoordinator::Panel::EXPOSURE,
        EXPOSURE_PANEL_TOOLS
    },
    {
        ToolPanelCoordinator::Panel::DETAILS,
        DETAILS_PANEL_TOOLS
    },
    {
        ToolPanelCoordinator::Panel::COLOR,
        COLOR_PANEL_TOOLS
    },
    {
        ToolPanelCoordinator::Panel::ADVANCED,
        ADVANCED_PANEL_TOOLS
    },
    {
        ToolPanelCoordinator::Panel::LOCALLAB,
        LOCALLAB_PANEL_TOOLS
    },
    {
        ToolPanelCoordinator::Panel::TRANSFORM_PANEL,
        TRANSFORM_PANEL_TOOLS
    },
    {
        ToolPanelCoordinator::Panel::RAW,
        RAW_PANEL_TOOLS
    },
};

std::unordered_map<std::string, Tool> ToolPanelCoordinator::toolNamesReverseMap;

ToolPanelCoordinator::ToolPanelCoordinator (bool batch) : ipc (nullptr), favoritePanelSW(nullptr), hasChanged (false), batch(batch), editDataProvider (nullptr), photoLoadedOnce(false), ornamentSurface(new RTSurface("ornament1.svg")), prevMode(EditorMode::EDIT)
{

    colorPickerRow_ = nullptr;

    // Legacy panel pointers (no longer used as separate panels)
    favoritePanel   = nullptr;
    exposurePanel   = nullptr;
    detailsPanel    = nullptr;
    colorPanel      = nullptr;
    rawPanel        = nullptr;
    advancedPanel   = nullptr;

    // Active panels
    transformPanel  = Gtk::manage (new ToolVBox ());
    locallabPanel   = Gtk::manage(new ToolVBox());

    coarse              = Gtk::manage (new CoarsePanel ());
    toneCurve           = Gtk::manage (new ToneCurve ());
    shadowshighlights   = Gtk::manage (new ShadowsHighlights ());
    toneEqualizer       = Gtk::manage (new ToneEqualizer ());
    impulsedenoise      = Gtk::manage (new ImpulseDenoise ());
    aidenoise           = Gtk::manage (new AIDenoise ());
    defringe            = Gtk::manage (new Defringe ());
    spot                = Gtk::manage (new Spot ());
    dirpyrdenoise       = Gtk::manage (new DirPyrDenoise ());
    epd                 = Gtk::manage (new EdgePreservingDecompositionUI ());
    sharpening          = Gtk::manage (new Sharpening ());
    localContrast       = Gtk::manage(new LocalContrast());
    sharpenEdge         = Gtk::manage(new SharpenEdge());
    sharpenMicro        = Gtk::manage(new SharpenMicro());
    lcurve              = Gtk::manage(new LCurve());
    rgbcurves           = Gtk::manage(new RGBCurves());
    colortoning         = Gtk::manage(new ColorToning());
    colorgrading        = Gtk::manage(new ColorGrading());
    lensgeom            = Gtk::manage(new LensGeometry());
    lensProf            = Gtk::manage(new LensProfilePanel());
    distortion          = Gtk::manage(new Distortion());
    rotate              = Gtk::manage(new Rotate());
    vibrance            = Gtk::manage(new Vibrance());
    colorappearance     = Gtk::manage(new ColorAppearance());
    whitebalance        = Gtk::manage(new WhiteBalance());
    compressgamut       = Gtk::manage (new Compressgamut ());
    vignetting          = Gtk::manage(new Vignetting());
    retinex             = Gtk::manage(new Retinex());
    gradient            = Gtk::manage(new Gradient());
    locallab            = Gtk::manage(new Locallab());
    pcvignette          = Gtk::manage(new PCVignette());
    perspective         = Gtk::manage(new PerspCorrection());
    cacorrection        = Gtk::manage(new CACorrection());
    chmixer             = Gtk::manage(new ChMixer());
    blackwhite          = Gtk::manage(new BlackWhite());
    resize              = Gtk::manage(new Resize());
    prsharpening        = Gtk::manage(new PrSharpening());
    framing             = Gtk::manage(new Framing());
    crop                = Gtk::manage(new Crop());
    icm                 = Gtk::manage(new ICMPanel());
    metadata            = Gtk::manage(new MetaDataPanel());
    wavelet             = Gtk::manage(new Wavelet());
    dirpyrequalizer     = Gtk::manage(new DirPyrEqualizer());
    hsvequalizer        = Gtk::manage(new HSVEqualizer());
    pointcolor          = Gtk::manage(new PointColor());
    filmSimulation      = Gtk::manage(new FilmSimulation());
    softlight           = Gtk::manage(new SoftLight());
    dehaze              = Gtk::manage(new Dehaze());
    sensorbayer         = Gtk::manage(new SensorBayer());
    sensorxtrans        = Gtk::manage(new SensorXTrans());
    bayerprocess        = Gtk::manage(new BayerProcess());
    xtransprocess       = Gtk::manage(new XTransProcess());
    bayerpreprocess     = Gtk::manage(new BayerPreProcess());
    preprocess          = Gtk::manage(new PreProcess());
    darkframe           = Gtk::manage(new DarkFrame());
    flatfield           = Gtk::manage(new FlatField());
    rawcacorrection     = Gtk::manage(new RAWCACorr());
    rawexposure         = Gtk::manage(new RAWExposure());
    preprocessWB        = Gtk::manage (new PreprocessWB ());
    bayerrawexposure    = Gtk::manage(new BayerRAWExposure());
    xtransrawexposure   = Gtk::manage(new XTransRAWExposure());
    fattal              = Gtk::manage(new FattalToneMapping());
    filmNegative        = Gtk::manage (new FilmNegative());
    pdSharpening        = Gtk::manage (new PdSharpening());
    // So Demosaic, Line noise filter, Green Equilibration, Ca-Correction (garder le nom de section identique!) and Black-Level will be moved in a "Bayer sensor" tool,
    // and a separate Demosaic and Black Level tool will be created in an "X-Trans sensor" tool

    // X-Trans demozaic methods: "3-pass (best), 1-pass (medium), fast"
    // Mettre  jour les profils fournis pour inclure les nouvelles section Raw, notamment pour "Default High ISO"
    // Valeurs par dfaut:
    //     Best -> low ISO
    //     Medium -> High ISO

    for (const auto &panel_tool_layout : getDefaultToolLayout()) {
        const auto &panel_tools = panel_tool_layout.second;
        std::vector<const ToolTree *> unprocessed_tools(panel_tools.size());

        // Start with the root tools for every panel.
        std::transform(
            panel_tools.begin(),
            panel_tools.end(),
            unprocessed_tools.begin(),
            [](const ToolTree &tool_tree) { return &tool_tree; });

        // Process each tool.
        while (!unprocessed_tools.empty()) {
            // Pop from stack of unprocessed.
            const ToolTree *cur_tool = unprocessed_tools.back();
            unprocessed_tools.pop_back();
            // Add tool to list of expanders and tool panels.
            FoldableToolPanel *const tool_panel = getFoldableToolPanel(*cur_tool);
            expList.push_back(tool_panel->getExpander());
            toolPanels.push_back(tool_panel);
            expanderToToolPanelMap[tool_panel->getExpander()] = tool_panel;
            toolToDefaultToolTreeMap[cur_tool->id] = cur_tool;
            // Show all now, since they won't be attached to a parent.
            tool_panel->getExpander()->show_all();
            // Add children to unprocessed.
            for (const auto &child_tool : cur_tool->children) {
                unprocessed_tools.push_back(&child_tool);
            }
        }
    }

    toolPanels.push_back (coarse);
    toolPanels.push_back(metadata);

    // Create the hidden notebook (kept for legacy compatibility)
    toolPanelNotebook = new Gtk::Notebook();
    toolPanelNotebook->set_name("ToolPanelNotebook");

    // Legacy scrolled window pointers set to nullptr (no longer used)
    favoritePanelSW.reset(nullptr);
    exposurePanelSW    = nullptr;
    detailsPanelSW     = nullptr;
    colorPanelSW       = nullptr;
    rawPanelSW         = nullptr;
    advancedPanelSW    = nullptr;

    // Mode-based scrolled windows (no horizontal scroll - content must fit)
    // propagate_natural_height so sidebar shrinks vertically when tools are collapsed
    editPanelSW        = Gtk::manage (new MyScrolledWindow ());
    transformPanelSW   = Gtk::manage (new MyScrolledWindow ());
    locallabPanelSW    = Gtk::manage (new MyScrolledWindow ());
    editPanelSW->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    transformPanelSW->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    locallabPanelSW->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    editPanelSW->set_propagate_natural_height(true);
    transformPanelSW->set_propagate_natural_height(true);
    locallabPanelSW->set_propagate_natural_height(true);

    // Create edit panel with ToolGroup sections
    editPanel    = Gtk::manage (new ToolVBox ());

    lightGroup       = Gtk::manage(new ToolGroup(M("TOOLGROUP_LIGHT")));
    colorGroup       = Gtk::manage(new ToolGroup(M("TOOLGROUP_COLOR")));
    detailGroup      = Gtk::manage(new ToolGroup(M("TOOLGROUP_DETAIL")));
    effectsGroup     = Gtk::manage(new ToolGroup(M("TOOLGROUP_EFFECTS")));
    advancedGroup    = Gtk::manage(new ToolGroup(M("TOOLGROUP_ADVANCED")));
    calibrationGroup = Gtk::manage(new ToolGroup(M("TOOLGROUP_CALIBRATION")));

    // load panel endings (hidden — ornament removed)
    for (int i = 0; i < 6; i++) {
        vbPanelEnd[i] = Gtk::manage (new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
        imgPanelEnd[i] = Gtk::manage (new Gtk::Image (ornamentSurface->get()));
        vbPanelEnd[i]->get_style_context()->add_class("PanelEnding");
        vbPanelEnd[i]->pack_start(*imgPanelEnd[i], Gtk::PACK_SHRINK);
        vbPanelEnd[i]->set_no_show_all(true);
        vbPanelEnd[i]->hide();
    }
    const auto& options = App::get().options();
    updateVScrollbars(options.hideTPVScrollbar);

    // Build the Edit panel with grouped tool sections
    editPanel->pack_start(*lightGroup, Gtk::PACK_SHRINK);
    editPanel->pack_start(*colorGroup, Gtk::PACK_SHRINK);
    editPanel->pack_start(*detailGroup, Gtk::PACK_SHRINK);
    editPanel->pack_start(*effectsGroup, Gtk::PACK_SHRINK);
    editPanel->pack_start(*advancedGroup, Gtk::PACK_SHRINK);
    editPanel->pack_start(*calibrationGroup, Gtk::PACK_SHRINK);

    // Metadata is now inside the Advanced group

    // Edit panel scrolled window
    Gtk::Box *editPanelContainer =
        Gtk::manage(new Gtk::Box(Gtk::Orientation::ORIENTATION_VERTICAL));
    editPanelSW->add(*editPanelContainer);
    editPanelContainer->pack_start(*editPanel, Gtk::PACK_SHRINK);
    editPanelContainer->pack_start(*vbPanelEnd[0], Gtk::PACK_SHRINK);

    // Transform/Crop panel scrolled window
    Gtk::Box *transformPanelContainer =
        Gtk::manage(new Gtk::Box(Gtk::Orientation::ORIENTATION_VERTICAL));
    transformPanelSW->add(*transformPanelContainer);
    transformPanelContainer->pack_start(*transformPanel, Gtk::PACK_SHRINK);
    transformPanelContainer->pack_start(*vbPanelEnd[1], Gtk::PACK_SHRINK);

    // Locallab/Mask panel scrolled window
    Gtk::Box *locallabPanelContainer =
        Gtk::manage(new Gtk::Box(Gtk::Orientation::ORIENTATION_VERTICAL));
    locallabPanelSW->add(*locallabPanelContainer);
    locallabPanelContainer->pack_start(*locallabPanel, Gtk::PACK_SHRINK);
    locallabPanelContainer->pack_start(*vbPanelEnd[2], Gtk::PACK_SHRINK);

    // Create ModeButtonBar
    modeButtonBar = Gtk::manage(new ModeButtonBar());

    // Create mode stack — homogeneous=false so sidebar sizes to current mode's content
    modeStack = Gtk::manage(new Gtk::Stack());
    modeStack->set_hhomogeneous(false);
    modeStack->set_vhomogeneous(false);
    modeStack->set_transition_type(Gtk::STACK_TRANSITION_TYPE_CROSSFADE);
    modeStack->set_transition_duration(150);

    // Presets page is added by EditorPanel via presetListPanel->getWidget()
    modeStack->add(*editPanelSW, "edit");
    modeStack->add(*transformPanelSW, "crop");
    modeStack->add(*locallabPanelSW, "mask");

    // Populate Edit panel with tools
    populateEditPanel();

    // Button bar at top of Crop page
    {
        Gtk::Box* btnRow = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 0));
        btnRow->set_margin_start(4);
        btnRow->set_margin_end(4);
        btnRow->set_margin_top(2);
        btnRow->set_margin_bottom(2);

        auto mkBtn = [](const char* icon, const char* tip) -> Gtk::Button* {
            Gtk::Button* b = Gtk::manage(new Gtk::Button());
            b->set_image(*Gtk::manage(new RTImage(icon, Gtk::ICON_SIZE_BUTTON)));
            b->set_relief(Gtk::RELIEF_NONE);
            b->set_tooltip_text(M(tip));
            return b;
        };

        Gtk::Button* rotL = mkBtn("rotate-left-90", "TP_COARSETRAF_TOOLTIP_ROTLEFT");
        Gtk::Button* rotR = mkBtn("rotate-right-90", "TP_COARSETRAF_TOOLTIP_ROTRIGHT");
        Gtk::Button* flipH = mkBtn("flip-horizontal", "TP_COARSETRAF_TOOLTIP_HFLIP");
        Gtk::Button* flipV = mkBtn("flip-vertical", "TP_COARSETRAF_TOOLTIP_VFLIP");
        Gtk::Button* straighten = mkBtn("rotate-straighten-small", "TP_ROTATE_SELECTLINE");
        Gtk::Button* cropSel = mkBtn("crop-small", "TP_CROP_SELECTCROP");
        Gtk::Button* perspSel = mkBtn("perspective-vertical-bottom", "TOOLBAR_TOOLTIP_PERSPECTIVE");

        rotL->signal_pressed().connect([this]() { coarse->rotateLeft(); });
        rotR->signal_pressed().connect([this]() { coarse->rotateRight(); });
        flipH->signal_pressed().connect([this]() { coarse->toggleHFlip(); });
        flipV->signal_pressed().connect([this]() { coarse->toggleVFlip(); });
        straighten->signal_pressed().connect([this]() { straightenRequested(); });
        cropSel->signal_pressed().connect([this]() { cropSelectRequested(); });
        perspSel->signal_pressed().connect([this]() { toolBar->setTool(TMPerspective); toolSelected(TMPerspective); });

        btnRow->pack_start(*rotL, Gtk::PACK_SHRINK);
        btnRow->pack_start(*rotR, Gtk::PACK_SHRINK);
        btnRow->pack_start(*flipH, Gtk::PACK_SHRINK);
        btnRow->pack_start(*flipV, Gtk::PACK_SHRINK);
        btnRow->pack_start(*Gtk::manage(new Gtk::Separator(Gtk::ORIENTATION_VERTICAL)), Gtk::PACK_SHRINK, 4);
        btnRow->pack_start(*straighten, Gtk::PACK_SHRINK);
        btnRow->pack_start(*cropSel, Gtk::PACK_SHRINK);
        btnRow->pack_start(*perspSel, Gtk::PACK_SHRINK);

        transformPanel->pack_start(*btnRow, Gtk::PACK_SHRINK, 0);
    }

    // Populate Transform panel — all tools flat (no headers/power buttons)
    addPanel(transformPanel, crop, 1);
    crop->setFlatMode(true);
    addPanel(transformPanel, rotate, 1);
    // rotate already has setFlatMode(true) in its constructor
    addPanel(transformPanel, perspective, 1);
    perspective->setFlatMode(true);
    perspective->exposeAutoButtons();
    perspective->hideAdvancedSection();
    addPanel(transformPanel, lensgeom, 1);
    lensgeom->setFlatMode(true);
    lensgeom->hideMethodCombo();
    addPanel(lensgeom->getSubToolsContainer(), lensProf, 2);
    lensProf->setFlatMode(true);
    addPanel(lensgeom->getSubToolsContainer(), distortion, 2);
    distortion->setFlatMode(true);
    addPanel(lensgeom->getSubToolsContainer(), cacorrection, 2);
    cacorrection->setFlatMode(true);
    addPanel(lensgeom->getSubToolsContainer(), vignetting, 2);
    vignetting->setFlatMode(true);

    // Populate Selective panel (spot removal + locallab)
    addPanel(locallabPanel, spot, 1);
    addPanel(locallabPanel, locallab, 1);

    // Show all after populating panels
    modeStack->show_all();

    // Collapse groups AFTER show_all (show_all overrides hide)
    advancedGroup->setExpanded(false);
    calibrationGroup->setExpanded(false);
    effectsGroup->setExpanded(false);

    // Collapse Color Appearance and Wavelet by default
    colorappearance->setExpanded(false);
    wavelet->setExpanded(false);


    // Hide mask/selective button in batch mode
    if (batch) {
        modeButtonBar->setModeVisible(EditorMode::MASK, false);
    }

    // Connect mode change signal
    modeconn = modeButtonBar->signal_mode_changed().connect(
        sigc::mem_fun(*this, &ToolPanelCoordinator::modeChanged));

    // Set Edit as the default visible panel (must be after show_all and signal connection)
    modeStack->set_visible_child("edit");
    modeButtonBar->setActiveMode(EditorMode::EDIT);

    for (auto toolPanel : toolPanels) {
        toolPanel->setListener(this);
    }

    whitebalance->setWBProvider(this);
    whitebalance->setSpotWBListener(this);
    pointcolor->setPointColorPickListener(this);
    darkframe->setDFProvider(this);
    flatfield->setFFProvider(this);
    lensgeom->setLensGeomListener(this);
    rotate->setLensGeomListener(this);
    perspective->setLensGeomListener(this);
    perspective->setPerspCorrectionPanelListener(this);
    distortion->setLensGeomListener(this);
    crop->setCropPanelListener(this);
    icm->setICMPanelListener(this);
    filmNegative->setFilmNegProvider(this);

    toolBar = new ToolBar();
    toolBar->setToolBarListener(this);
    toolBar->hideCropTools();

    // Pack spot WB and color picker into the Color group's picker row
    if (colorPickerRow_ && toolBar->getWbTool()) {
        colorPickerRow_->pack_start(*toolBar->getWbTool(), Gtk::PACK_SHRINK);
    }
    if (colorPickerRow_ && toolBar->getColPickerTool()) {
        colorPickerRow_->pack_start(*toolBar->getColPickerTool(), Gtk::PACK_SHRINK);
    }
}

const ToolPanelCoordinator::ToolLayout &ToolPanelCoordinator::getDefaultToolLayout()
{
    return PANEL_TOOLS;
}

Tool ToolPanelCoordinator::getToolFromName(const std::string &name)
{
    if (toolNamesReverseMap.empty()) {
        // Create the name to tool mapping.

        const auto panels = ToolPanelCoordinator::getDefaultToolLayout();
        std::vector<const ToolPanelCoordinator::ToolTree *> unprocessed_tool_trees;

        // Get the root tools from each panel.
        for (const auto &panel_tools : panels) {
            for (const auto &tool : panel_tools.second) {
                unprocessed_tool_trees.push_back(&tool);
            }
        }

        // Process all the tools, including their children.
        while (unprocessed_tool_trees.size() > 0) {
            const ToolPanelCoordinator::ToolTree *tool_tree =
                unprocessed_tool_trees.back();
            unprocessed_tool_trees.pop_back();
            toolNamesReverseMap[getToolName(tool_tree->id)] = tool_tree->id;
            for (const auto &child_tree : tool_tree->children) {
                unprocessed_tool_trees.push_back(&child_tree);
            }
        }
    }

    return toolNamesReverseMap.at(name);
}

std::string ToolPanelCoordinator::getToolName(Tool tool)
{
    switch (tool) {
        case Tool::TONE_CURVE:
            return ToneCurve::TOOL_NAME;
        case Tool::SHADOWS_HIGHLIGHTS:
            return ShadowsHighlights::TOOL_NAME;
        case Tool::TONE_EQUALIZER:
            return ToneEqualizer::TOOL_NAME;
        case Tool::IMPULSE_DENOISE:
            return ImpulseDenoise::TOOL_NAME;
        case Tool::AI_DENOISE:
            return AIDenoise::TOOL_NAME;
        case Tool::DEFRINGE_TOOL:
            return Defringe::TOOL_NAME;
        case Tool::COMPRESSGAMUT_TOOL:
            return Compressgamut::TOOL_NAME;
        case Tool::SPOT:
            return Spot::TOOL_NAME;
        case Tool::DIR_PYR_DENOISE:
            return DirPyrDenoise::TOOL_NAME;
        case Tool::EPD:
            return EdgePreservingDecompositionUI::TOOL_NAME;
        case Tool::SHARPENING_TOOL:
            return Sharpening::TOOL_NAME;
        case Tool::LOCAL_CONTRAST:
            return LocalContrast::TOOL_NAME;
        case Tool::SHARPEN_EDGE:
            return SharpenEdge::TOOL_NAME;
        case Tool::SHARPEN_MICRO:
            return SharpenMicro::TOOL_NAME;
        case Tool::L_CURVE:
            return LCurve::TOOL_NAME;
        case Tool::RGB_CURVES:
            return RGBCurves::TOOL_NAME;
        case Tool::COLOR_TONING:
            return ColorToning::TOOL_NAME;
        case Tool::COLOR_GRADING:
            return ColorGrading::TOOL_NAME;
        case Tool::LENS_GEOM:
            return LensGeometry::TOOL_NAME;
        case Tool::LENS_PROF:
            return LensProfilePanel::TOOL_NAME;
        case Tool::DISTORTION:
            return Distortion::TOOL_NAME;
        case Tool::ROTATE:
            return Rotate::TOOL_NAME;
        case Tool::VIBRANCE:
            return Vibrance::TOOL_NAME;
        case Tool::COLOR_APPEARANCE:
            return ColorAppearance::TOOL_NAME;
        case Tool::WHITE_BALANCE:
            return WhiteBalance::TOOL_NAME;
        case Tool::VIGNETTING:
            return Vignetting::TOOL_NAME;
        case Tool::RETINEX_TOOL:
            return Retinex::TOOL_NAME;
        case Tool::GRADIENT:
            return Gradient::TOOL_NAME;
        case Tool::LOCALLAB:
            return Locallab::TOOL_NAME;
        case Tool::PC_VIGNETTE:
            return PCVignette::TOOL_NAME;
        case Tool::PERSPECTIVE:
            return PerspCorrection::TOOL_NAME;
        case Tool::CA_CORRECTION:
            return CACorrection::TOOL_NAME;
        case Tool::CH_MIXER:
            return ChMixer::TOOL_NAME;
        case Tool::BLACK_WHITE:
            return BlackWhite::TOOL_NAME;
        case Tool::RESIZE_TOOL:
            return Resize::TOOL_NAME;
        case Tool::PR_SHARPENING:
            return PrSharpening::TOOL_NAME;
        case Tool::FRAMING:
            return Framing::TOOL_NAME;
        case Tool::CROP_TOOL:
            return Crop::TOOL_NAME;
        case Tool::ICM:
            return ICMPanel::TOOL_NAME;
        case Tool::WAVELET:
            return Wavelet::TOOL_NAME;
        case Tool::DIR_PYR_EQUALIZER:
            return DirPyrEqualizer::TOOL_NAME;
        case Tool::HSV_EQUALIZER:
            return HSVEqualizer::TOOL_NAME;
        case Tool::POINT_COLOR:
            return PointColor::TOOL_NAME;
        case Tool::FILM_SIMULATION:
            return FilmSimulation::TOOL_NAME;
        case Tool::SOFT_LIGHT:
            return SoftLight::TOOL_NAME;
        case Tool::DEHAZE:
            return Dehaze::TOOL_NAME;
        case Tool::SENSOR_BAYER:
            return SensorBayer::TOOL_NAME;
        case Tool::SENSOR_XTRANS:
            return SensorXTrans::TOOL_NAME;
        case Tool::BAYER_PROCESS:
            return BayerProcess::TOOL_NAME;
        case Tool::XTRANS_PROCESS:
            return XTransProcess::TOOL_NAME;
        case Tool::BAYER_PREPROCESS:
            return BayerPreProcess::TOOL_NAME;
        case Tool::PREPROCESS:
            return PreProcess::TOOL_NAME;
        case Tool::DARKFRAME_TOOL:
            return DarkFrame::TOOL_NAME;
        case Tool::FLATFIELD_TOOL:
            return FlatField::TOOL_NAME;
        case Tool::RAW_CA_CORRECTION:
            return RAWCACorr::TOOL_NAME;
        case Tool::RAW_EXPOSURE:
            return RAWExposure::TOOL_NAME;
        case Tool::PREPROCESS_WB:
            return PreprocessWB::TOOL_NAME;
        case Tool::BAYER_RAW_EXPOSURE:
            return BayerRAWExposure::TOOL_NAME;
        case Tool::XTRANS_RAW_EXPOSURE:
            return XTransRAWExposure::TOOL_NAME;
        case Tool::FATTAL:
            return FattalToneMapping::TOOL_NAME;
        case Tool::FILM_NEGATIVE:
            return FilmNegative::TOOL_NAME;
        case Tool::PD_SHARPENING:
            return PdSharpening::TOOL_NAME;
    };
    assert(false);
    return "";
};

bool ToolPanelCoordinator::isFavoritable(Tool tool)
{
    switch (tool) {
        case Tool::PR_SHARPENING:
            return false;
        default:
            return true;
    }
}

void ToolPanelCoordinator::notebookPageChanged(Gtk::Widget* page, guint page_num)
{
    // Legacy function - no longer used (mode switching handled by modeChanged)
}

void ToolPanelCoordinator::modeChanged(EditorMode mode)
{
    // Switch the visible stack child
    switch (mode) {
        case EditorMode::PRESETS:
            modeStack->set_visible_child("presets");
            break;
        case EditorMode::EDIT:
            modeStack->set_visible_child("edit");
            break;
        case EditorMode::CROPPING:
            modeStack->set_visible_child("crop");
            break;
        case EditorMode::MASK:
            modeStack->set_visible_child("mask");
            break;
    }

    // Handle Locallab subscription/unsubscription
    if (photoLoadedOnce) {
        if (mode == EditorMode::MASK) {
            toolBar->blockEditDeactivation();
            locallab->subscribe();
            locallab->setExpanded(true);
        }

        if (prevMode == EditorMode::MASK) {
            toolBar->blockEditDeactivation(false);
            locallab->unsubscribe();
        }
    }

    prevMode = mode;
}

void ToolPanelCoordinator::populateEditPanel()
{
    // Light group: Tone Curve, Shadows/Highlights, Tone Equalizer, EPD
    addPanel(lightGroup->getContentBox(), blackwhite, 1);
    blackwhite->setFlatMode(true);
    addPanel(lightGroup->getContentBox(), toneCurve, 1);
    toneCurve->setFlatMode(true);
    addPanel(lightGroup->getContentBox(), shadowshighlights, 1);
    shadowshighlights->setFlatMode(true);
    addPanel(lightGroup->getContentBox(), toneEqualizer, 1);
    toneEqualizer->setFlatMode(true);
    addPanel(lightGroup->getContentBox(), epd, 1);
    epd->setFlatMode(true);

    // Color group (LR-aligned): White Balance, Vibrance, Color Mixer (HSV Eq),
    //                           Point Color, Color Grading
    // Spot WB and Color Picker buttons will be added after toolBar is created
    colorPickerRow_ = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 2));
    colorPickerRow_->set_margin_start(4);
    colorPickerRow_->set_margin_bottom(2);
    colorGroup->getContentBox()->pack_start(*colorPickerRow_, Gtk::PACK_SHRINK);
    addPanel(colorGroup->getContentBox(), whitebalance, 1);
    whitebalance->setFlatMode(true);
    addPanel(colorGroup->getContentBox(), vibrance, 1);
    vibrance->setFlatMode(true);
    addPanel(colorGroup->getContentBox(), hsvequalizer, 1);
    hsvequalizer->setFlatMode(true);
    addPanel(colorGroup->getContentBox(), pointcolor, 1);
    pointcolor->setFlatMode(true);
    addPanel(colorGroup->getContentBox(), colorgrading, 1);
    colorgrading->setFlatMode(true);

    // Detail group: Sharpening, Local Contrast, Denoise, AI Denoise, Defringe, Dehaze
    addPanel(detailGroup->getContentBox(), sharpening, 1);
    sharpening->setFlatMode(true);
    addPanel(detailGroup->getContentBox(), localContrast, 1);
    localContrast->setFlatMode(true);
    addPanel(detailGroup->getContentBox(), dirpyrdenoise, 1);
    dirpyrdenoise->setFlatMode(true);
    addPanel(detailGroup->getContentBox(), aidenoise, 1);
    aidenoise->setFlatMode(true);
    addPanel(detailGroup->getContentBox(), defringe, 1);
    defringe->setFlatMode(true);
    addPanel(detailGroup->getContentBox(), dirpyrequalizer, 1);
    dirpyrequalizer->setFlatMode(true);
    addPanel(detailGroup->getContentBox(), dehaze, 1);
    dehaze->setFlatMode(true);

    // Effects group: PC Vignette, Gradient, Film Simulation, Film Negative, L Curve
    addPanel(effectsGroup->getContentBox(), pcvignette, 1);
    pcvignette->setFlatMode(true);
    addPanel(effectsGroup->getContentBox(), gradient, 1);
    gradient->setFlatMode(true);
    addPanel(effectsGroup->getContentBox(), filmSimulation, 1);
    filmSimulation->setFlatMode(true);
    addPanel(effectsGroup->getContentBox(), filmNegative, 1);
    filmNegative->setFlatMode(true);
    addPanel(effectsGroup->getContentBox(), lcurve, 1);
    lcurve->setFlatMode(true);

    // Advanced group: Moved color tools + existing advanced tools
    addPanel(advancedGroup->getContentBox(), chmixer, 1);
    chmixer->setFlatMode(true);
    // B&W moved to lightGroup
    addPanel(advancedGroup->getContentBox(), rgbcurves, 1);
    rgbcurves->setFlatMode(true);
    addPanel(advancedGroup->getContentBox(), colortoning, 1);
    colortoning->setFlatMode(true);
    addPanel(advancedGroup->getContentBox(), softlight, 1);
    softlight->setFlatMode(true);
    addPanel(advancedGroup->getContentBox(), fattal, 1);
    fattal->setFlatMode(true);
    addPanel(advancedGroup->getContentBox(), compressgamut, 1);
    compressgamut->setFlatMode(true);
    addPanel(advancedGroup->getContentBox(), sharpenMicro, 1);
    sharpenMicro->setFlatMode(true);
    addPanel(advancedGroup->getContentBox(), impulsedenoise, 1);
    impulsedenoise->setFlatMode(true);
    addPanel(advancedGroup->getContentBox(), sharpenEdge, 1);
    sharpenEdge->setFlatMode(true);
    addPanel(advancedGroup->getContentBox(), retinex, 1);
    retinex->setFlatMode(true);
    addPanel(advancedGroup->getContentBox(), colorappearance, 1);
    colorappearance->setFlatMode(true);
    addPanel(advancedGroup->getContentBox(), wavelet, 1);
    wavelet->setFlatMode(true);
    advancedGroup->getContentBox()->pack_start(*metadata, Gtk::PACK_SHRINK);

    // Calibration group: ICM, Raw Exposure, Bayer/XTrans process, Preprocess, Dark Frame, Flat Field, Raw CA, PD Sharpening
    addPanel(calibrationGroup->getContentBox(), icm, 1);
    icm->setFlatMode(true);
    addPanel(calibrationGroup->getContentBox(), rawexposure, 1);
    rawexposure->setFlatMode(true);
    addPanel(calibrationGroup->getContentBox(), sensorbayer, 1);
    sensorbayer->setFlatMode(true);
    addPanel(sensorbayer->getSubToolsContainer(), bayerprocess, 2);
    addPanel(sensorbayer->getSubToolsContainer(), bayerrawexposure, 2);
    addPanel(sensorbayer->getSubToolsContainer(), bayerpreprocess, 2);
    addPanel(sensorbayer->getSubToolsContainer(), rawcacorrection, 2);
    addPanel(calibrationGroup->getContentBox(), sensorxtrans, 1);
    sensorxtrans->setFlatMode(true);
    addPanel(sensorxtrans->getSubToolsContainer(), xtransprocess, 2);
    addPanel(sensorxtrans->getSubToolsContainer(), xtransrawexposure, 2);
    addPanel(calibrationGroup->getContentBox(), preprocessWB, 1);
    preprocessWB->setFlatMode(true);
    addPanel(calibrationGroup->getContentBox(), preprocess, 1);
    preprocess->setFlatMode(true);
    addPanel(calibrationGroup->getContentBox(), darkframe, 1);
    darkframe->setFlatMode(true);
    addPanel(calibrationGroup->getContentBox(), flatfield, 1);
    flatfield->setFlatMode(true);
    addPanel(calibrationGroup->getContentBox(), pdSharpening, 1);
    pdSharpening->setFlatMode(true);

}

void ToolPanelCoordinator::updateFavoritesPanel(
    const std::vector<Glib::ustring> &favoritesNames,
    bool cloneFavoriteTools)
{
    // Legacy function for notebook-based favorites (no longer used)
    return;
    std::unordered_set<Tool, ScopedEnumHash> favorites_set;
    std::vector<std::reference_wrapper<const ToolTree>> favorites_tool_tree;

    for (const auto &tool_name : favoritesNames) {
        Tool tool;
        try {
            tool = getToolFromName(tool_name.raw());
        } catch (const std::out_of_range &e) {
            if (rtengine::settings->verbose) {
                std::cerr
                    << "Unrecognized favorite tool \"" << tool_name << "\""
                    << std::endl;
            }
            continue;
        }
        if (isFavoritable(tool)) {
        favorites_set.insert(tool);
        favorites_tool_tree.push_back(
            std::ref(*(toolToDefaultToolTreeMap.at(tool))));
        }
    }

    updateToolPanel(
        favoritePanel, favorites_tool_tree, 1, favorites_set, cloneFavoriteTools);
}

void ToolPanelCoordinator::updatePanelTools(
    Gtk::Widget *page,
    const std::vector<Glib::ustring> &favorites,
    bool cloneFavoriteTools)
{
    // Legacy function for notebook-based panel management (no longer used)
    return;

    if (page == favoritePanelSW.get()) {
        updateFavoritesPanel(favorites, cloneFavoriteTools);
        return;
    }

    ToolVBox *panel = nullptr;
    const std::vector<ToolTree> *default_panel_tools = nullptr;
    if (page == exposurePanelSW) {
        panel = exposurePanel;
        default_panel_tools = &EXPOSURE_PANEL_TOOLS;
    } else if (page == detailsPanelSW) {
        panel = detailsPanel;
        default_panel_tools = &DETAILS_PANEL_TOOLS;
    } else if (page == colorPanelSW) {
        panel = colorPanel;
        default_panel_tools = &COLOR_PANEL_TOOLS;
    } else if (page == transformPanelSW) {
        panel = transformPanel;
        default_panel_tools = &TRANSFORM_PANEL_TOOLS;
    } else if (page == rawPanelSW) {
        panel = rawPanel;
        default_panel_tools = &RAW_PANEL_TOOLS;
    } else if (page == advancedPanelSW) {
        panel = advancedPanel;
        default_panel_tools = &ADVANCED_PANEL_TOOLS;
    } else if (page == locallabPanelSW) {
        panel = locallabPanel;
        default_panel_tools = &LOCALLAB_PANEL_TOOLS;
    } else {
        return;
    }
    assert(panel && default_panel_tools);

    std::unordered_set<Tool, ScopedEnumHash> favoriteTools;
    for (const auto &tool_name : favorites) {
        Tool tool;
        try {
            tool = getToolFromName(tool_name.raw());
        } catch (const std::out_of_range &e) {
            if (rtengine::settings->verbose) {
                std::cerr
                    << "Unrecognized favorite tool \"" << tool_name << "\""
                    << std::endl;
            }
            continue;
        }
        if (isFavoritable(tool)) {
            favoriteTools.insert(tool);
        }
    }

    updateToolPanel(panel, *default_panel_tools, 1, favoriteTools, cloneFavoriteTools);
}

template <typename T>
typename std::enable_if<std::is_convertible<T, const ToolTree>::value, void>::type
ToolPanelCoordinator::updateToolPanel(
    Gtk::Box *panelBox,
    const std::vector<T> &children,
    int level,
    const std::unordered_set<Tool, ScopedEnumHash> &favorites,
    bool cloneFavoriteTools)
{
    const bool is_favorite_panel = panelBox == favoritePanel;
    const bool skip_favorites = !cloneFavoriteTools && !is_favorite_panel;
    const std::vector<Gtk::Widget *> old_tool_panels = panelBox->get_children();
    auto old_widgets_iter = old_tool_panels.begin();
    auto new_tool_trees_iter = children.begin();

    // Indicates if this tool should not be added. Favorite tools are skipped
    // if they are sub-tools within the favorites panel, or if tool cloning is
    // off and they are not within the favorites panel.
    const auto should_skip_tool =
        [this, skip_favorites, &favorites](const ToolTree &tool_tree) {
            return (skip_favorites && favorites.count(tool_tree.id)) ||
                   (batch && tool_tree.id == Tool::LOCALLAB);
        };

    // Keep tools that are already correct.
    while (
        old_widgets_iter != old_tool_panels.end() &&
        new_tool_trees_iter != children.end()) {
        if (should_skip_tool(*new_tool_trees_iter)) {
            ++new_tool_trees_iter;
            continue;
        }
        if (*old_widgets_iter !=
            getFoldableToolPanel(*new_tool_trees_iter)->getExpander()) {
            break;
        }
        ++new_tool_trees_iter;
        ++old_widgets_iter;
    }

    // Remove incorrect tools.
    for (auto iter = old_tool_panels.end(); iter != old_widgets_iter;) {
        --iter;
        FoldableToolPanel *old_tool_panel = expanderToToolPanelMap.at(*iter);
        assert(*iter == old_tool_panel->getExpander());
        panelBox->remove(**iter);
        old_tool_panel->setParent(nullptr);
    }

    // Add correct tools.
    for (; new_tool_trees_iter != children.end(); new_tool_trees_iter++) {
        if (should_skip_tool(*new_tool_trees_iter)) {
            continue;
        }
        FoldableToolPanel *tool_panel =
            getFoldableToolPanel(*new_tool_trees_iter);
        const bool reparent = tool_panel->getParent();
        if (reparent) {
            tool_panel->getParent()->remove(*tool_panel->getExpander());
        }
        addPanel(panelBox, tool_panel, level);
        if (!reparent) {
            // If attaching for the first time, update the widget sizes.
            tool_panel->getExpander()->check_resize();
        }
    }

    // Update the child tools.
    for (const ToolTree &tool_tree : children) {
        const FoldableToolPanel *tool_panel = getFoldableToolPanel(tool_tree);
        updateToolPanel(
            tool_panel->getSubToolsContainer(),
            tool_tree.children,
            level + 1,
            favorites,
            cloneFavoriteTools && !is_favorite_panel);
    }
}

void ToolPanelCoordinator::addPanel(Gtk::Box* where, FoldableToolPanel* panel, int level)
{

    panel->setParent(where);
    panel->setLevel(level);
    where->pack_start(*panel->getExpander(), false, false);
}

ToolPanelCoordinator::~ToolPanelCoordinator ()
{
    idle_register.destroy();

    closeImage();

    // Block mode change signal to prevent callbacks during destruction
    modeconn.block(true);

    // Foldable tool panels manage (Gtk::manage) their expanders. Each expander
    // will only be automatically deleted if attached to a parent and the parent
    // is deleted.  This is a hack in lieu of a potentially tedious refactoring
    // of FoldableToolPanel.
    std::unique_ptr<Gtk::Box> hidden_tool_panel_parent(new Gtk::Box());
    for (const auto expander : expList) {
        if (!expander->get_parent()) {
            hidden_tool_panel_parent->add(*expander);
        }
    }

    delete toolPanelNotebook;
    delete toolBar;
}

void ToolPanelCoordinator::imageTypeChanged(bool isRaw, bool isBayer, bool isXtrans, bool isMono, bool isGainMapSupported)
{
    if (isRaw) {
        if (isBayer) {
            idle_register.add(
                [this, isGainMapSupported]() -> bool
                {
                    calibrationGroup->set_sensitive(true);
                    sensorxtrans->FoldableToolPanel::hide();
                    xtransprocess->FoldableToolPanel::hide();
                    bayerrawexposure->FoldableToolPanel::show();
                    xtransrawexposure->FoldableToolPanel::hide();
                    sensorbayer->FoldableToolPanel::show();
                    bayerprocess->FoldableToolPanel::show();
                    bayerpreprocess->FoldableToolPanel::show();
                    rawcacorrection->FoldableToolPanel::show();
                    preprocessWB->FoldableToolPanel::show();
                    preprocess->FoldableToolPanel::show();
                    flatfield->FoldableToolPanel::show();
                    flatfield->setGainMap(isGainMapSupported);
                    pdSharpening->FoldableToolPanel::show();
                    retinex->FoldableToolPanel::setGrayedOut(false);
                    return false;
                }
            );
        } else if (isXtrans) {
            idle_register.add(
                [this, isGainMapSupported]() -> bool
                {
                    calibrationGroup->set_sensitive(true);
                    sensorxtrans->FoldableToolPanel::show();
                    xtransprocess->FoldableToolPanel::show();
                    xtransrawexposure->FoldableToolPanel::show();
                    bayerrawexposure->FoldableToolPanel::hide();
                    sensorbayer->FoldableToolPanel::hide();
                    bayerprocess->FoldableToolPanel::hide();
                    bayerpreprocess->FoldableToolPanel::hide();
                    rawcacorrection->FoldableToolPanel::hide();
                    preprocessWB->FoldableToolPanel::show();
                    preprocess->FoldableToolPanel::show();
                    flatfield->FoldableToolPanel::show();
                    flatfield->setGainMap(isGainMapSupported);
                    pdSharpening->FoldableToolPanel::show();
                    retinex->FoldableToolPanel::setGrayedOut(false);
                    return false;
                }
            );
        } else if (isMono) {
            idle_register.add(
                [this, isGainMapSupported]() -> bool
                {
                    calibrationGroup->set_sensitive(true);
                    sensorbayer->FoldableToolPanel::hide();
                    bayerprocess->FoldableToolPanel::hide();
                    bayerpreprocess->FoldableToolPanel::hide();
                    rawcacorrection->FoldableToolPanel::hide();
                    sensorxtrans->FoldableToolPanel::hide();
                    xtransprocess->FoldableToolPanel::hide();
                    xtransrawexposure->FoldableToolPanel::hide();
                    preprocessWB->FoldableToolPanel::hide();
                    preprocess->FoldableToolPanel::hide();
                    flatfield->FoldableToolPanel::show();
                    flatfield->setGainMap(isGainMapSupported);
                    pdSharpening->FoldableToolPanel::show();
                    retinex->FoldableToolPanel::setGrayedOut(false);
                    return false;
                }
            );
        } else {
            idle_register.add(
                [this]() -> bool
                {
                    calibrationGroup->set_sensitive(true);
                    sensorbayer->FoldableToolPanel::hide();
                    bayerprocess->FoldableToolPanel::hide();
                    bayerpreprocess->FoldableToolPanel::hide();
                    rawcacorrection->FoldableToolPanel::hide();
                    sensorxtrans->FoldableToolPanel::hide();
                    xtransprocess->FoldableToolPanel::hide();
                    xtransrawexposure->FoldableToolPanel::hide();
                    preprocessWB->FoldableToolPanel::hide();
                    preprocess->FoldableToolPanel::hide();
                    flatfield->FoldableToolPanel::hide();
                    pdSharpening->FoldableToolPanel::hide();
                    retinex->FoldableToolPanel::setGrayedOut(false);
                    return false;
                }
            );
        }
    } else {
        idle_register.add(
            [this]() -> bool
            {
                calibrationGroup->set_sensitive(false);
                sensorbayer->FoldableToolPanel::hide();
                bayerprocess->FoldableToolPanel::hide();
                bayerpreprocess->FoldableToolPanel::hide();
                rawcacorrection->FoldableToolPanel::hide();
                sensorxtrans->FoldableToolPanel::hide();
                xtransprocess->FoldableToolPanel::hide();
                xtransrawexposure->FoldableToolPanel::hide();
                preprocessWB->FoldableToolPanel::hide();
                preprocess->FoldableToolPanel::hide();
                flatfield->FoldableToolPanel::hide();
                pdSharpening->FoldableToolPanel::hide();
                retinex->FoldableToolPanel::setGrayedOut(true);
                return false;
            }
        );
    }

}

void ToolPanelCoordinator::setTweakOperator (rtengine::TweakOperator *tOperator)
{
    if (ipc && tOperator) {
        ipc->setTweakOperator(tOperator);
    }
}

void ToolPanelCoordinator::unsetTweakOperator (rtengine::TweakOperator *tOperator)
{
    if (ipc && tOperator) {
        ipc->unsetTweakOperator(tOperator);
    }
}

void ToolPanelCoordinator::refreshPreview (const rtengine::ProcEvent& event)
{
    if (!ipc) {
        return;
    }

    ProcParams* params = ipc->beginUpdateParams ();
    for (auto toolPanel : toolPanels) {
        toolPanel->write (params);
    }

    ipc->endUpdateParams (event);   // starts the IPC processing
}

void ToolPanelCoordinator::panelChanged(const rtengine::ProcEvent& event, const Glib::ustring& descr)
{
    if (!ipc) {
        return;
    }

    int changeFlags = rtengine::RefreshMapper::getInstance()->getAction(event);

    ProcParams* params = ipc->beginUpdateParams();

    for (auto toolPanel : toolPanels) {
        toolPanel->write(params);
    }

    // Compensate rotation on flip
    if (event == rtengine::EvCTHFlip || event == rtengine::EvCTVFlip) {
        if (fabs(params->rotate.degree) > 0.001) {
            params->rotate.degree *= -1;
            changeFlags |= rtengine::RefreshMapper::getInstance()->getAction(rtengine::EvROTDegree);
            rotate->read(params);
        }
    }

    int tr = TR_NONE;

    if (params->coarse.rotate == 90) {
        tr = TR_R90;
    } else if (params->coarse.rotate == 180) {
        tr = TR_R180;
    } else if (params->coarse.rotate == 270) {
        tr = TR_R270;
    }

    // Update "on preview" geometry
    if (event == rtengine::EvPhotoLoaded || event == rtengine::EvProfileChanged || event == rtengine::EvHistoryBrowsed || event == rtengine::EvCTRotate) {
        // updating the "on preview" geometry
        int fw, fh;
        ipc->getInitialImage()->getImageSource()->getFullSize(fw, fh, tr);
        gradient->updateGeometry(params->gradient.centerX, params->gradient.centerY, params->gradient.feather, params->gradient.degree, fw, fh);
    }

    // some transformations make the crop change for convenience
    if (event == rtengine::EvCTHFlip) {
        crop->hFlipCrop();
        crop->write(params);
    } else if (event == rtengine::EvCTVFlip) {
        crop->vFlipCrop();
        crop->write(params);
    } else if (event == rtengine::EvCTRotate) {
        crop->rotateCrop(params->coarse.rotate, params->coarse.hflip, params->coarse.vflip);
        crop->write(params);
        resize->update(params->crop.enabled, params->crop.w, params->crop.h, ipc->getFullWidth(), ipc->getFullHeight());
        resize->write(params);
        framing->update(ipc->getFullWidth(), ipc->getFullHeight());
        framing->write(params);
    } else if (event == rtengine::EvCrop) {
        resize->update(params->crop.enabled, params->crop.w, params->crop.h);
        resize->write(params);
        framing->update(ipc->getFullWidth(), ipc->getFullHeight());
        framing->write(params);
    }

    /*
     * Manage Locallab mask visibility:
     * - Mask preview is updated when choosing a mask preview method
     * - Mask preview is also updated when modifying (to avoid hiding a potentially visible mask combobox):
     *   - Color&Light invers
     *   - Exposure inversex
     *   - Shadow Highlight inverssh
     *   - Soft Light softMethod
     * - Mask preview is stopped when creating, deleting or selecting a spot
     * - Mask preview is also stopped when removing a spot or resetting all mask visibility
     */
    if (event == rtengine::EvlocallabshowmaskMethod) {
        const Locallab::llMaskVisibility maskStruc = locallab->getMaskVisibility();
        ipc->setLocallabMaskVisibility(maskStruc.previewDeltaE, maskStruc.showMaskOverlay, maskStruc.colorMask, maskStruc.colorMaskinv, maskStruc.expMask, maskStruc.expMaskinv,
                maskStruc.SHMask, maskStruc.SHMaskinv, maskStruc.vibMask, maskStruc.softMask,
                maskStruc.blMask, maskStruc.tmMask, maskStruc.retiMask, maskStruc.sharMask,
                maskStruc.lcMask, maskStruc.cbMask, maskStruc.logMask, maskStruc.maskMask, maskStruc.cieMask);
    } else if (event == rtengine::EvLocallabSpotCreated || event == rtengine::EvLocallabSpotSelectedWithMask ||
            event == rtengine::EvLocallabSpotDeleted /*|| event == rtengine::Evlocallabshowreset*/ ||
            event == rtengine::EvlocallabToolRemovedWithRefresh) {
        locallab->resetMaskVisibility();
        ipc->setLocallabMaskVisibility(false, false, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    }

    ipc->endUpdateParams(changeFlags);    // starts the IPC processing

    hasChanged = true;

    for (auto paramcListener : paramcListeners) {
        paramcListener->procParamsChanged(params, event, descr);
    }

    // Locallab spot curves are set visible if at least one photo has been loaded (to avoid
    // segfault) and locallab panel is active
    // When a new photo is loaded, Locallab spot curves need to be set visible again
const auto func =
    [this]() -> bool
    {
        if (photoLoadedOnce && modeButtonBar->getActiveMode() == EditorMode::MASK) {
            locallab->subscribe();
       }

        return false;
    };

if (event == rtengine::EvPhotoLoaded) {
    idle_register.add(func);
}

    photoLoadedOnce = true;

}

void ToolPanelCoordinator::profileChange(
    const PartialProfile* nparams,
    const rtengine::ProcEvent& event,
    const Glib::ustring& descr,
    const ParamsEdited* paramsEdited,
    bool fromLastSave
)
{
    int fw, fh, tr;

    if (!ipc) {
        return;
    }

    ProcParams *params = ipc->beginUpdateParams();
    ProcParams *mergedParams = new ProcParams();

    // Copy the current params as default values for the fusion
    *mergedParams = *params;

    // Reset IPTC values when switching procparams from the History
    if (event == rtengine::EvHistoryBrowsed) {
        mergedParams->metadata.iptc.clear();
        mergedParams->metadata.exif.clear();
    }

    // And apply the partial profile nparams to mergedParams
    nparams->applyTo(mergedParams, fromLastSave);

    // Derive the effective changes, if it's a profile change, to prevent slow RAW rerendering if not necessary
    bool filterRawRefresh = false;

    if (event != rtengine::EvPhotoLoaded) {
        ParamsEdited pe(true);
        std::vector<rtengine::procparams::ProcParams> lParams(2);
        lParams[0] = *params;
        lParams[1] = *mergedParams;
        pe.initFrom(lParams);

        filterRawRefresh = pe.raw.isUnchanged() && pe.lensProf.isUnchanged() && pe.retinex.isUnchanged() && pe.pdsharpening.isUnchanged();
    }

    *params = *mergedParams;
    delete mergedParams;

    tr = TR_NONE;

    if (params->coarse.rotate == 90) {
        tr = TR_R90;
    } else if (params->coarse.rotate == 180) {
        tr = TR_R180;
    } else if (params->coarse.rotate == 270) {
        tr = TR_R270;
    }

    // trimming overflowing cropped area
    ipc->getInitialImage()->getImageSource()->getFullSize(fw, fh, tr);
    crop->trim(params, fw, fh);

    // updating the GUI with updated values
    for (auto toolPanel : toolPanels) {
        toolPanel->read(params);

        if (event == rtengine::EvPhotoLoaded || event == rtengine::EvProfileChanged) {
            toolPanel->autoOpenCurve();

            // For Locallab, reset tool expanders visibility only when a photo or profile is loaded
            locallab->openAllTools();
        }
    }

    if (event == rtengine::EvPhotoLoaded || event == rtengine::EvProfileChanged || event == rtengine::EvHistoryBrowsed || event == rtengine::EvCTRotate) {
        // updating the "on preview" geometry
        gradient->updateGeometry(params->gradient.centerX, params->gradient.centerY, params->gradient.feather, params->gradient.degree, fw, fh);
    }

    // Reset Locallab mask visibility
    locallab->resetMaskVisibility();
    ipc->setLocallabMaskVisibility(false, false, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);

    // start the IPC processing
    if (filterRawRefresh) {
        ipc->endUpdateParams(rtengine::RefreshMapper::getInstance()->getAction(event) & ALLNORAW);
    } else {
        ipc->endUpdateParams(event);
    }

    hasChanged = event != rtengine::EvProfileChangeNotification;

    for (auto paramcListener : paramcListeners) {
        paramcListener->procParamsChanged(params, event, descr);
    }

    // Locallab spot curves are set visible if at least one photo has been loaded (to avoid
    // segfault) and locallab panel is active
    // When a new photo is loaded, Locallab spot curves need to be set visible again
const auto func =
    [this]() -> bool
    {
        if (photoLoadedOnce && modeButtonBar->getActiveMode() == EditorMode::MASK) {
            locallab->subscribe();
        }

        return false;
    };

if (event == rtengine::EvPhotoLoaded) {
    idle_register.add(func);
}

    photoLoadedOnce = true;
}

void ToolPanelCoordinator::setDefaults(const ProcParams* defparams)
{
    if (defparams) {
        for (auto toolPanel : toolPanels) {
            toolPanel->setDefaults(defparams);
        }
    }
}

CropGUIListener* ToolPanelCoordinator::getCropGUIListener()
{

    return crop;
}

void ToolPanelCoordinator::initImage(rtengine::StagedImageProcessor* ipc_, bool raw)
{

    ipc = ipc_;
    toneCurve->disableListener();
    toneCurve->enableAll();
    toneCurve->enableListener();

    if (ipc) {
        const rtengine::FramesMetaData* pMetaData = ipc->getInitialImage()->getMetaData();
        metadata->setImageData(pMetaData);

        ipc->setAutoExpListener(toneCurve);
        ipc->setAutoCamListener(colorappearance);
        ipc->setAutoBlackListener(bayerrawexposure);
        ipc->setAutoBlackxListener(xtransrawexposure);
        ipc->setAutoBWListener(blackwhite);
        ipc->setFrameCountListener(bayerprocess);
        ipc->setFlatFieldAutoClipListener (flatfield);
        ipc->setBayerAutoContrastListener (bayerprocess);
        ipc->setXtransAutoContrastListener (xtransprocess);
        ipc->setpdSharpenAutoContrastListener (pdSharpening);
        ipc->setpdSharpenAutoRadiusListener (pdSharpening);
        ipc->setAutoWBListener(whitebalance);
        ipc->setAutoColorTonListener(colortoning);
        ipc->setAutoprimListener(icm);
        ipc->setAutoChromaListener(dirpyrdenoise);
        ipc->setWaveletListener(wavelet);
        ipc->setRetinexListener(retinex);
        ipc->setSizeListener(crop);
        ipc->setSizeListener(resize);
        ipc->setLocallabListener(locallab);
        ipc->setImageTypeListener(this);
        ipc->setFilmNegListener(filmNegative);
        ipc->setCompgamutListener(compressgamut);
        flatfield->setShortcutPath(Glib::path_get_dirname(ipc->getInitialImage()->getFileName()));
        aidenoise->setImagePath(ipc->getInitialImage()->getFileName());
        aidenoise->setImProcCoordinator(ipc);

        icm->setRawMeta(raw, (const rtengine::FramesData*)pMetaData);
        lensProf->setRawMeta(raw, pMetaData);
        perspective->setMetadata(pMetaData);
    }


    toneCurve->setRaw(raw);
    hasChanged = true;
}


void ToolPanelCoordinator::closeImage()
{
    // Just disconnect — don't call stopProcessing() here.
    // The caller (EditorPanel::close) defers the blocking join
    // and destruction to a background thread.
    ipc = nullptr;
    aidenoise->setImProcCoordinator(nullptr);
}

void ToolPanelCoordinator::closeAllTools()
{
    const auto& options = App::get().options();
    for (size_t i = 0; i < options.tpOpen.size(); ++i) {
        if (i < expList.size()) {
            expList[i]->set_expanded(false);
        }
    }
}

void ToolPanelCoordinator::openAllTools()
{
    const auto& options = App::get().options();
    for (size_t i = 0; i < options.tpOpen.size(); ++i) {
        if (i < expList.size()) {
            expList[i]->set_expanded(true);
        }
    }
}

void ToolPanelCoordinator::updateToolState()
{
    const auto& options = App::get().options();
    if (options.tpOpen.empty()) {
        for (auto expander : expList) {
            expander->set_expanded(false);
        }

        wavelet->updateToolState({});
        retinex->updateToolState({});

        return;
    }

    for (size_t i = 0; i < options.tpOpen.size(); ++i) {
        if (i < expList.size()) {
            expList[i]->set_expanded(options.tpOpen[i]);
        }
    }

    if (options.tpOpen.size() > expList.size()) {
        const size_t sizeWavelet = options.tpOpen.size() - expList.size();

        std::vector<int> temp;

        for (size_t i = 0; i < sizeWavelet; ++i) {
            temp.push_back(options.tpOpen[i + expList.size()]);
        }

        wavelet->updateToolState(temp);
        retinex->updateToolState(temp);
    }
}

void ToolPanelCoordinator::readOptions()
{

    crop->readOptions();

    // Apply UI complexity filtering
    applyUIComplexity(App::get().options().uiComplexity);
}

void ToolPanelCoordinator::writeOptions()
{

    crop->writeOptions();

    auto& options = App::get().mut_options();
    if (options.autoSaveTpOpen) {
        writeToolExpandedStatus(options.tpOpen);
    }
}


void ToolPanelCoordinator::writeToolExpandedStatus(std::vector<int> &tpOpen)
{
    tpOpen.clear();

    for (size_t i = 0; i < expList.size(); i++) {
        tpOpen.push_back(expList.at(i)->get_expanded());
    }

    wavelet->writeOptions(tpOpen);
    retinex->writeOptions(tpOpen);

}


void ToolPanelCoordinator::updateShowtooltipVisibility (bool showtooltip)
{
    locallab->updateShowtooltipVisibility(showtooltip);
}


void ToolPanelCoordinator::spotWBselected(int x, int y, Thumbnail* thm)
{
    if (!ipc) {
        return;
    }

//    toolBar->setTool (TOOL_HAND);
    int rect = whitebalance->getSize();
    int ww = ipc->getFullWidth();
    int hh = ipc->getFullHeight();

    if (x - rect > 0 && y - rect > 0 && x + rect < ww && y + rect < hh) {
        double temp;
        double green;
        ipc->getSpotWB(x, y, rect, temp, green);
        whitebalance->setWB(temp, green);
    }
}

void ToolPanelCoordinator::pointColorSelected(int x, int y, Thumbnail* thm)
{
    if (!ipc) {
        return;
    }

    int ww = ipc->getFullWidth();
    int hh = ipc->getFullHeight();

    if (x > 3 && y > 3 && x + 3 < ww && y + 3 < hh) {
        float h, s, v;
        ipc->getSpotHSV(x, y, 3, h, s, v);
        pointcolor->addTargetFromPick(h, s, v);
    }

    toolBar->setTool(TMHand);
}

void ToolPanelCoordinator::sharpMaskSelected(bool sharpMask)
{
    if (!ipc) {
        return;
    }

    ipc->beginUpdateParams();
    ipc->endUpdateParams (ipc->setSharpMask(sharpMask));
}

int ToolPanelCoordinator::getSpotWBRectSize() const
{
    return whitebalance->getSize();
}

void ToolPanelCoordinator::cropSelectionReady()
{
    toolBar->setTool (TMHand);

    if (!ipc) {
        return;
    }
}

void ToolPanelCoordinator::rotateSelectionReady(double rotate_deg, Thumbnail* thm)
{
    toolBar->setTool (TMHand);

    if (!ipc) {
        return;
    }

    if (rotate_deg != 0.0) {
        rotate->straighten (rotate_deg);
    }
}

ToolBar* ToolPanelCoordinator::getToolBar() const
{
    return toolBar;
}

CropGUIListener* ToolPanelCoordinator::startCropEditing(Thumbnail* thm)
{
    return crop;
}

void ToolPanelCoordinator::autoCropRequested()
{

    if (!ipc) {
        return;
    }

    int x1, y1, x2, y2, w, h;
    ipc->getAutoCrop(crop->getRatio(), x1, y1, w, h);
    x2 = x1 + w - 1;
    y2 = y1 + h - 1;
    crop->cropInit(x1, y1, w, h);
    crop->cropResized(x1, y1, x2, y2);
    crop->cropManipReady();
}

const rtengine::RawImage* ToolPanelCoordinator::getDF()
{
    if (!ipc) {
        return nullptr;
    }

    const rtengine::FramesMetaData *imd = ipc->getInitialImage()->getMetaData();

    if (imd) {
        int iso = imd->getISOSpeed();
        double shutter = imd->getShutterSpeed();
        std::string maker(imd->getMake());
        std::string model(imd->getModel());
        time_t timestamp = imd->getDateTimeAsTS();

        return rtengine::DFManager::getInstance().searchDarkFrame(maker, model, iso, shutter, timestamp);
    }

    return nullptr;
}

rtengine::RawImage* ToolPanelCoordinator::getFF()
{
    if (!ipc) {
        return nullptr;
    }

    const rtengine::FramesMetaData *imd = ipc->getInitialImage()->getMetaData();

    if (imd) {
        // int iso = imd->getISOSpeed();              temporarily removed because unused
        // double shutter = imd->getShutterSpeed();   temporarily removed because unused
        double aperture = imd->getFNumber();
        double focallength = imd->getFocalLen();
        std::string maker(imd->getMake());
        std::string model(imd->getModel());
        std::string lens(imd->getLens());
        time_t timestamp = imd->getDateTimeAsTS();

        return rtengine::ffm.searchFlatField(maker, model, lens, focallength, aperture, timestamp);
    }

    return nullptr;
}

Glib::ustring ToolPanelCoordinator::GetCurrentImageFilePath()
{
    if (!ipc) {
        return "";
    }

    return ipc->getInitialImage()->getFileName();
}

void ToolPanelCoordinator::straightenRequested()
{

    if (!ipc) {
        return;
    }

    toolBar->setTool(TMStraighten);
}

void ToolPanelCoordinator::autoPerspRequested (bool corr_pitch, bool corr_yaw, double& rot, double& pitch, double& yaw, const std::vector<rtengine::ControlLine> *lines)
{
    if (!(ipc && (corr_pitch || corr_yaw))) {
        return;
    }

    rtengine::ImageSource *src = dynamic_cast<rtengine::ImageSource *>(ipc->getInitialImage());
    if (!src) {
        return;
    }

    rtengine::procparams::ProcParams params;
    ipc->getParams(&params);

    // If focal length or crop factor are undetermined, use the defaults.
    if (params.perspective.camera_focal_length <= 0) {
        params.perspective.camera_focal_length =
            PerspectiveParams::DEFAULT_CAMERA_FOCAL_LENGTH;
    }
    if (params.perspective.camera_crop_factor <= 0) {
        params.perspective.camera_crop_factor =
            PerspectiveParams::DEFAULT_CAMERA_CROP_FACTOR;
    }

    auto res = rtengine::PerspectiveCorrection::autocompute(src, corr_pitch, corr_yaw, &params, src->getMetaData(), lines);
    rot = res.angle;
    pitch = res.pitch;
    yaw = res.yaw;
}

double ToolPanelCoordinator::autoDistorRequested()
{
    if (!ipc) {
        return 0.0;
    }

    return rtengine::ImProcFunctions::getAutoDistor(ipc->getInitialImage()->getFileName(), 400);
}

void ToolPanelCoordinator::spotWBRequested(int size)
{

    if (!ipc) {
        return;
    }

    toolBar->setTool(TMSpotWB);
}

void ToolPanelCoordinator::pointColorPickRequested()
{
    if (!ipc) {
        return;
    }

    toolBar->setTool(TMPointColorPick);
}

void ToolPanelCoordinator::cropSelectRequested()
{

    if (!ipc) {
        return;
    }

    toolBar->setTool(TMCropSelect);
}

void ToolPanelCoordinator::controlLineEditModeChanged(bool active)
{
    if (!ipc) {
        return;
    }

    if (active) {
        toolBar->setTool(TMPerspective);
    }
}

void ToolPanelCoordinator::saveInputICCReference(const Glib::ustring& fname, bool apply_wb)
{
    if (ipc) {
        ipc->saveInputICCReference(fname, apply_wb);
    }
}

void ToolPanelCoordinator::updateCurveBackgroundHistogram(
    const LUTu& histToneCurve,
    const LUTu& histLCurve,
    const LUTu& histCCurve,
    const LUTu& histLCAM,
    const LUTu& histCCAM,
    const LUTu& histRed,
    const LUTu& histGreen,
    const LUTu& histBlue,
    const LUTu& histLuma,
    const LUTu& histLRETI
)
{
    colorappearance->updateCurveBackgroundHistogram(histToneCurve, histLCurve, histCCurve, histLCAM,  histCCAM, histRed, histGreen, histBlue, histLuma, histLRETI);
    toneCurve->updateCurveBackgroundHistogram(histToneCurve, histLCurve, histCCurve,histLCAM,  histCCAM, histRed, histGreen, histBlue, histLuma, histLRETI);
    lcurve->updateCurveBackgroundHistogram(histToneCurve, histLCurve, histCCurve, histLCAM, histCCAM, histRed, histGreen, histBlue, histLuma, histLRETI);
    rgbcurves->updateCurveBackgroundHistogram(histToneCurve, histLCurve, histCCurve, histLCAM, histCCAM, histRed, histGreen, histBlue, histLuma, histLRETI);
    retinex->updateCurveBackgroundHistogram(histToneCurve, histLCurve, histCCurve, histLCAM, histCCAM, histRed, histGreen, histBlue, histLuma, histLRETI);
}

void ToolPanelCoordinator::foldAllButOne(Gtk::Box* parent, FoldableToolPanel* openedSection)
{

    for (auto toolPanel : toolPanels) {
        if (toolPanel->getParent() != nullptr) {
            ToolPanel* currentTP = toolPanel;

            if (currentTP->getParent() == parent) {
                // Section in the same tab, we unfold it if it's not the one that has been clicked
                if (currentTP != openedSection) {
                    currentTP->setExpanded(false);
                } else {
                    if (!currentTP->getExpanded()) {
                        currentTP->setExpanded(true);
                    }
                }
            }
        }
    }

    // Auto-scroll to the opened section so it's visible
    if (openedSection && openedSection->getExpander()) {
        MyExpander* exp = openedSection->getExpander();
        // Find the parent ScrolledWindow and scroll to show the expander
        Gtk::Widget* widget = exp;
        while (widget) {
            Gtk::ScrolledWindow* sw = dynamic_cast<Gtk::ScrolledWindow*>(widget->get_parent());
            if (sw) {
                // Use idle callback to scroll after layout is complete
                Glib::signal_idle().connect_once([exp, sw]() {
                    int x, y;
                    if (exp->translate_coordinates(*sw, 0, 0, x, y)) {
                        auto vadj = sw->get_vadjustment();
                        if (vadj) {
                            vadj->set_value(vadj->get_value() + y);
                        }
                    }
                });
                break;
            }
            widget = widget->get_parent();
        }
    }
}

void ToolPanelCoordinator::applyUIComplexity(int complexityLevel)
{
    // Define which tools are visible at each complexity level.
    // Beginner: essential tools only (~12)
    // Standard: most tools except advanced (Retinex, ColorAppearance, Wavelet) and Locallab
    // Expert: all tools (default)

    static const std::unordered_set<Tool, ScopedEnumHash> beginnerTools = {
        Tool::WHITE_BALANCE,
        Tool::TONE_CURVE,
        Tool::SHADOWS_HIGHLIGHTS,
        Tool::VIBRANCE,
        Tool::SHARPENING_TOOL,
        Tool::DIR_PYR_DENOISE,
        Tool::CROP_TOOL,
        Tool::RESIZE_TOOL,
        Tool::LENS_PROF,
        Tool::FILM_SIMULATION,
        Tool::DEHAZE,
        Tool::FATTAL,
    };

    static const std::unordered_set<Tool, ScopedEnumHash> advancedOnlyTools = {
        Tool::RETINEX_TOOL,
        Tool::COLOR_APPEARANCE,
        Tool::WAVELET,
        Tool::LOCALLAB,
        Tool::DIR_PYR_EQUALIZER,
        Tool::EPD,
        Tool::COMPRESSGAMUT_TOOL,
    };

    // All Tool enum values for iteration
    static const std::vector<Tool> allTools = {
        Tool::TONE_CURVE, Tool::SHADOWS_HIGHLIGHTS, Tool::TONE_EQUALIZER,
        Tool::IMPULSE_DENOISE, Tool::DEFRINGE_TOOL, Tool::COMPRESSGAMUT_TOOL,
        Tool::SPOT, Tool::DIR_PYR_DENOISE, Tool::EPD,
        Tool::SHARPENING_TOOL, Tool::LOCAL_CONTRAST, Tool::SHARPEN_EDGE,
        Tool::SHARPEN_MICRO, Tool::L_CURVE, Tool::RGB_CURVES,
        Tool::COLOR_TONING, Tool::COLOR_GRADING, Tool::LENS_GEOM, Tool::LENS_PROF,
        Tool::DISTORTION, Tool::ROTATE, Tool::VIBRANCE,
        Tool::COLOR_APPEARANCE, Tool::WHITE_BALANCE, Tool::VIGNETTING,
        Tool::RETINEX_TOOL, Tool::GRADIENT, Tool::LOCALLAB,
        Tool::PC_VIGNETTE, Tool::PERSPECTIVE, Tool::CA_CORRECTION,
        Tool::CH_MIXER, Tool::BLACK_WHITE, Tool::RESIZE_TOOL,
        Tool::PR_SHARPENING, Tool::FRAMING, Tool::CROP_TOOL,
        Tool::ICM, Tool::WAVELET, Tool::DIR_PYR_EQUALIZER,
        Tool::HSV_EQUALIZER, Tool::POINT_COLOR, Tool::FILM_SIMULATION, Tool::SOFT_LIGHT,
        Tool::DEHAZE, Tool::SENSOR_BAYER, Tool::SENSOR_XTRANS,
        Tool::BAYER_PROCESS, Tool::XTRANS_PROCESS, Tool::BAYER_PREPROCESS,
        Tool::PREPROCESS, Tool::DARKFRAME_TOOL, Tool::FLATFIELD_TOOL,
        Tool::RAW_CA_CORRECTION, Tool::RAW_EXPOSURE, Tool::PREPROCESS_WB,
        Tool::BAYER_RAW_EXPOSURE, Tool::XTRANS_RAW_EXPOSURE, Tool::FATTAL,
        Tool::FILM_NEGATIVE, Tool::PD_SHARPENING,
    };

    for (const auto& tool : allTools) {
        FoldableToolPanel* panel = getFoldableToolPanel(tool);
        if (!panel) continue;

        bool visible = true;

        if (complexityLevel == Options::UI_BEGINNER) {
            visible = beginnerTools.count(tool) > 0;
        } else if (complexityLevel == Options::UI_STANDARD) {
            visible = advancedOnlyTools.count(tool) == 0;
        }
        // UI_EXPERT: all visible

        if (visible) {
            panel->show();
        } else {
            panel->hide();
        }
    }
}

void ToolPanelCoordinator::updateToolLocations(
    const std::vector<Glib::ustring> &favorites, bool cloneFavoriteTools)
{
    // Update favorite tool panels list (kept for legacy compatibility)
    favoritesToolPanels.clear();
    for (const auto &favorite_name : favorites) {
        Tool tool;
        try {
            tool = getToolFromName(favorite_name.raw());
        } catch (const std::out_of_range &e) {
            if (rtengine::settings->verbose) {
                std::cerr
                    << "Unrecognized favorite tool \"" << favorite_name << "\""
                    << std::endl;
            }
            continue;
        }
        if (isFavoritable(tool)) {
            favoritesToolPanels.push_back(getFoldableToolPanel(tool));
        }
    }
}

bool ToolPanelCoordinator::handleShortcutKey(GdkEventKey* event)
{
    bool alt = event->state & GDK_MOD1_MASK;

    if (alt) {
        switch (event->keyval) {
            case GDK_KEY_p:
                modeButtonBar->setActiveMode(EditorMode::PRESETS);
                return true;

            case GDK_KEY_e:
                modeButtonBar->setActiveMode(EditorMode::EDIT);
                return true;

            case GDK_KEY_t:
            case GDK_KEY_c:
                modeButtonBar->setActiveMode(EditorMode::CROPPING);
                return true;

            case GDK_KEY_r:
            case GDK_KEY_o:
                modeButtonBar->setActiveMode(EditorMode::MASK);
                return true;
        }
    }

    return false;
}

void ToolPanelCoordinator::updateVScrollbars(bool hide)
{
    GThreadLock lock; // All GUI access from idle_add callbacks or separate thread HAVE to be protected
    Gtk::PolicyType policy = hide ? Gtk::POLICY_NEVER : Gtk::POLICY_AUTOMATIC;
    editPanelSW->set_policy         (Gtk::POLICY_NEVER, policy);
    transformPanelSW->set_policy    (Gtk::POLICY_NEVER, policy);
    locallabPanelSW->set_policy     (Gtk::POLICY_NEVER, policy);

    for (auto currExp : expList) {
        currExp->updateVScrollbars(hide);
    }
}

void ToolPanelCoordinator::updateTPVScrollbar(bool hide)
{
    updateVScrollbars(hide);
}

void ToolPanelCoordinator::toolDeselected(ToolMode tool)
{
    if (tool == TMPerspective) {
        perspective->requestApplyControlLines();
    }
}

void ToolPanelCoordinator::toolSelected(ToolMode tool)
{
    GThreadLock lock; // All GUI access from idle_add callbacks or separate thread HAVE to be protected

    // Block mode signal to prevent recursive callbacks
    modeconn.block(true);

    switch (tool) {
        case TMCropSelect: {
            toolBar->blockEditDeactivation(false);
            crop->setExpanded(true);
            modeButtonBar->setActiveMode(EditorMode::CROPPING);
            modeChanged(EditorMode::CROPPING);
            break;
        }

        case TMSpotWB: {
            toolBar->blockEditDeactivation(false);
            whitebalance->setExpanded(true);
            modeButtonBar->setActiveMode(EditorMode::EDIT);
            modeChanged(EditorMode::EDIT);
            break;
        }

        case TMStraighten: {
            toolBar->blockEditDeactivation(false);
            rotate->setExpanded(true);
            lensgeom->setExpanded(true);
            modeButtonBar->setActiveMode(EditorMode::CROPPING);
            modeChanged(EditorMode::CROPPING);
            break;
        }

        case TMPerspective: {
            toolBar->blockEditDeactivation(false);
            perspective->setControlLineEditMode(true);
            perspective->setExpanded(true);
            lensgeom->setExpanded(true);
            modeButtonBar->setActiveMode(EditorMode::CROPPING);
            modeChanged(EditorMode::CROPPING);
            break;
        }

        default:
            break;
    }

    modeconn.block(false);
}

void ToolPanelCoordinator::editModeSwitchedOff()
{
    if (editDataProvider) {
        editDataProvider->switchOffEditMode();
    }
}

void ToolPanelCoordinator::dirSelected(const Glib::ustring& dirname, const Glib::ustring& openfile)
{

    flatfield->setShortcutPath(dirname);
}

void ToolPanelCoordinator::setEditProvider(EditDataProvider *provider)
{
    editDataProvider = provider;

    for (size_t i = 0; i < toolPanels.size(); i++) {
        toolPanels.at(i)->setEditProvider(provider);
    }
}

bool ToolPanelCoordinator::getFilmNegativeSpot(rtengine::Coord spot, int spotSize, RGB &refInput, RGB &refOutput)
{
    return ipc && ipc->getFilmNegativeSpot(spot.x, spot.y, spotSize, refInput, refOutput);
}


void ToolPanelCoordinator::setProgressListener(rtengine::ProgressListener *pl)
{
    metadata->setProgressListener(pl);
}

FoldableToolPanel *ToolPanelCoordinator::getFoldableToolPanel(Tool tool) const
{
    switch (tool) {
        case Tool::TONE_CURVE:
            return toneCurve;
        case Tool::SHADOWS_HIGHLIGHTS:
            return shadowshighlights;
        case Tool::TONE_EQUALIZER:
            return toneEqualizer;
        case Tool::IMPULSE_DENOISE:
            return impulsedenoise;
        case Tool::AI_DENOISE:
            return aidenoise;
        case Tool::DEFRINGE_TOOL:
            return defringe;
        case Tool::COMPRESSGAMUT_TOOL:
            return compressgamut;
        case Tool::SPOT:
            return spot;
        case Tool::DIR_PYR_DENOISE:
            return dirpyrdenoise;
        case Tool::EPD:
            return epd;
        case Tool::SHARPENING_TOOL:
            return sharpening;
        case Tool::LOCAL_CONTRAST:
            return localContrast;
        case Tool::SHARPEN_EDGE:
            return sharpenEdge;
        case Tool::SHARPEN_MICRO:
            return sharpenMicro;
        case Tool::L_CURVE:
            return lcurve;
        case Tool::RGB_CURVES:
            return rgbcurves;
        case Tool::COLOR_TONING:
            return colortoning;
        case Tool::COLOR_GRADING:
            return colorgrading;
        case Tool::LENS_GEOM:
            return lensgeom;
        case Tool::LENS_PROF:
            return lensProf;
        case Tool::DISTORTION:
            return distortion;
        case Tool::ROTATE:
            return rotate;
        case Tool::VIBRANCE:
            return vibrance;
        case Tool::COLOR_APPEARANCE:
            return colorappearance;
        case Tool::WHITE_BALANCE:
            return whitebalance;
        case Tool::VIGNETTING:
            return vignetting;
        case Tool::RETINEX_TOOL:
            return retinex;
        case Tool::GRADIENT:
            return gradient;
        case Tool::LOCALLAB:
            return locallab;
        case Tool::PC_VIGNETTE:
            return pcvignette;
        case Tool::PERSPECTIVE:
            return perspective;
        case Tool::CA_CORRECTION:
            return cacorrection;
        case Tool::CH_MIXER:
            return chmixer;
        case Tool::BLACK_WHITE:
            return blackwhite;
        case Tool::RESIZE_TOOL:
            return resize;
        case Tool::PR_SHARPENING:
            return prsharpening;
        case Tool::FRAMING:
            return framing;
        case Tool::CROP_TOOL:
            return crop;
        case Tool::ICM:
            return icm;
        case Tool::WAVELET:
            return wavelet;
        case Tool::DIR_PYR_EQUALIZER:
            return dirpyrequalizer;
        case Tool::HSV_EQUALIZER:
            return hsvequalizer;
        case Tool::POINT_COLOR:
            return pointcolor;
        case Tool::FILM_SIMULATION:
            return filmSimulation;
        case Tool::SOFT_LIGHT:
            return softlight;
        case Tool::DEHAZE:
            return dehaze;
        case Tool::SENSOR_BAYER:
            return sensorbayer;
        case Tool::SENSOR_XTRANS:
            return sensorxtrans;
        case Tool::BAYER_PROCESS:
            return bayerprocess;
        case Tool::XTRANS_PROCESS:
            return xtransprocess;
        case Tool::BAYER_PREPROCESS:
            return bayerpreprocess;
        case Tool::PREPROCESS:
            return preprocess;
        case Tool::DARKFRAME_TOOL:
            return darkframe;
        case Tool::FLATFIELD_TOOL:
            return flatfield;
        case Tool::RAW_CA_CORRECTION:
            return rawcacorrection;
        case Tool::RAW_EXPOSURE:
            return rawexposure;
        case Tool::PREPROCESS_WB:
            return preprocessWB;
        case Tool::BAYER_RAW_EXPOSURE:
            return bayerrawexposure;
        case Tool::XTRANS_RAW_EXPOSURE:
            return xtransrawexposure;
        case Tool::FATTAL:
            return fattal;
        case Tool::FILM_NEGATIVE:
            return filmNegative;
        case Tool::PD_SHARPENING:
            return pdSharpening;
    };
    assert(false);
    return nullptr;
}

FoldableToolPanel *ToolPanelCoordinator::getFoldableToolPanel(const ToolTree &toolTree) const
{
    return getFoldableToolPanel(toolTree.id);
}
