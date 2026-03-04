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
        .id = Tool::TEXTURE,
        .children = {},
    },
    {
        .id = Tool::CLARITY,
        .children = {},
    },
    {
        .id = Tool::GRAIN,
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
        .id = Tool::LENS_BLUR,
        .children = {},
    },
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
    texture             = Gtk::manage(new Texture());
    clarity             = Gtk::manage(new Clarity());
    grain               = Gtk::manage(new Grain());
    lensblur            = Gtk::manage(new LensBlur());
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
    locallabPanelContainer_ =
        Gtk::manage(new Gtk::Box(Gtk::Orientation::ORIENTATION_VERTICAL));
    locallabPanelSW->add(*locallabPanelContainer_);
    locallabPanelContainer_->pack_start(*locallabPanel, Gtk::PACK_SHRINK);
    locallabPanelContainer_->pack_start(*vbPanelEnd[2], Gtk::PACK_SHRINK);

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

    // Helper: make a flat icon button
    auto mkBtn = [](const char* icon, const char* tip) -> Gtk::Button* {
        Gtk::Button* b = Gtk::manage(new Gtk::Button());
        b->set_image(*Gtk::manage(new RTImage(icon, Gtk::ICON_SIZE_BUTTON)));
        b->set_relief(Gtk::RELIEF_NONE);
        b->set_tooltip_text(M(tip));
        return b;
    };

    // Helper: make a collapsible section (Clarity-style toggle)
    // Returns {headerRow, contentBox, label}
    struct CollapsibleSection { Gtk::Box* header; Gtk::Box* content; Gtk::Label* label; };
    auto mkCollapsible = [](const Glib::ustring& name) -> CollapsibleSection {
        Gtk::Box* headerRow = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 0));
        headerRow->set_margin_start(6);
        headerRow->set_margin_end(4);
        headerRow->set_margin_top(4);
        headerRow->set_margin_bottom(0);

        Gtk::Button* toggle = Gtk::manage(new Gtk::Button());
        toggle->set_relief(Gtk::RELIEF_NONE);
        Gtk::Label* label = Gtk::manage(new Gtk::Label());
        label->set_markup("\u25B8 <b>" + Glib::Markup::escape_text(name) + "</b>");
        toggle->add(*label);
        headerRow->pack_start(*toggle, Gtk::PACK_SHRINK);

        Gtk::Box* content = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
        content->set_no_show_all(true);

        toggle->signal_clicked().connect([content, label, name]() {
            bool expanded = content->get_visible();
            content->set_no_show_all(false);
            expanded ? content->hide() : content->show_all();
            content->set_no_show_all(true);
            label->set_markup(Glib::ustring(expanded ? "\u25B8 <b>" : "\u25BE <b>") + Glib::Markup::escape_text(name) + "</b>");
        });

        return {headerRow, content, label};
    };

    // Button row at top: RotL / RotR / FlipH / FlipV
    {
        Gtk::Box* btnRow = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 0));
        btnRow->set_margin_start(4);
        btnRow->set_margin_end(4);
        btnRow->set_margin_top(2);
        btnRow->set_margin_bottom(2);

        Gtk::Button* rotL = mkBtn("rotate-left-90", "TP_COARSETRAF_TOOLTIP_ROTLEFT");
        Gtk::Button* rotR = mkBtn("rotate-right-90", "TP_COARSETRAF_TOOLTIP_ROTRIGHT");
        Gtk::Button* flipH = mkBtn("flip-horizontal", "TP_COARSETRAF_TOOLTIP_HFLIP");
        Gtk::Button* flipV = mkBtn("flip-vertical", "TP_COARSETRAF_TOOLTIP_VFLIP");

        rotL->signal_pressed().connect([this]() { coarse->rotateLeft(); });
        rotR->signal_pressed().connect([this]() { coarse->rotateRight(); });
        flipH->signal_pressed().connect([this]() { coarse->toggleHFlip(); });
        flipV->signal_pressed().connect([this]() { coarse->toggleVFlip(); });

        btnRow->pack_start(*rotL, Gtk::PACK_SHRINK);
        btnRow->pack_start(*rotR, Gtk::PACK_SHRINK);
        btnRow->pack_start(*flipH, Gtk::PACK_SHRINK);
        btnRow->pack_start(*flipV, Gtk::PACK_SHRINK);

        transformPanel->pack_start(*btnRow, Gtk::PACK_SHRINK, 0);
    }

    transformPanel->pack_start(*Gtk::manage(new Gtk::Separator(Gtk::ORIENTATION_HORIZONTAL)), Gtk::PACK_SHRINK, 2);

    // --- Crop section (collapsible) ---
    {
        auto sec = mkCollapsible(M("TP_CROP_LABEL"));
        cropSectionContent_ = sec.content;
        cropSectionLabel_ = sec.label;

        // Add crop-select and reset buttons to header row
        sec.header->pack_end(*crop->getResetCropButton(), Gtk::PACK_SHRINK);
        sec.header->pack_end(*crop->getSelectCropButton(), Gtk::PACK_SHRINK);

        // Pack crop's expander into the collapsible content
        crop->setParent(sec.content);
        crop->setLevel(1);
        crop->setFlatMode(true);
        sec.content->pack_start(*crop->getExpander(), false, false);

        transformPanel->pack_start(*sec.header, Gtk::PACK_SHRINK);
        transformPanel->pack_start(*sec.content, Gtk::PACK_SHRINK);
    }

    // --- Rotate section (inline with straighten button) ---
    {
        Gtk::Box* rotateRow = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 0));

        rotate->setParent(rotateRow);
        rotate->setLevel(1);
        // rotate already has setFlatMode(true) in its constructor
        rotateRow->pack_start(*rotate->getExpander(), true, true);

        Gtk::Button* straighten = mkBtn("rotate-straighten-small", "TP_ROTATE_SELECTLINE");
        straighten->signal_pressed().connect([this]() { straightenRequested(); });
        straighten->set_margin_end(4);
        rotateRow->pack_end(*straighten, Gtk::PACK_SHRINK);

        transformPanel->pack_start(*rotateRow, Gtk::PACK_SHRINK);
    }

    // --- Perspective section (collapsible) ---
    {
        auto sec = mkCollapsible(M("TP_PERSPECTIVE_LABEL"));
        perspSectionContent_ = sec.content;
        perspSectionLabel_ = sec.label;

        Gtk::Button* perspSel = mkBtn("perspective-vertical-bottom", "TOOLBAR_TOOLTIP_PERSPECTIVE");
        perspSel->signal_pressed().connect([this]() { toolBar->setTool(TMPerspective); toolSelected(TMPerspective); });
        sec.header->pack_end(*perspSel, Gtk::PACK_SHRINK);

        perspective->setParent(sec.content);
        perspective->setLevel(1);
        perspective->setFlatMode(true);
        perspective->exposeAutoButtons();
        perspective->hideAdvancedSection();
        sec.content->pack_start(*perspective->getExpander(), false, false);

        transformPanel->pack_start(*sec.header, Gtk::PACK_SHRINK);
        transformPanel->pack_start(*sec.content, Gtk::PACK_SHRINK);
    }

    // --- Advanced section (collapsible) ---
    {
        auto sec = mkCollapsible(M("TP_TRANSFORM_ADVANCED"));
        advSectionContent_ = sec.content;
        advSectionLabel_ = sec.label;

        lensgeom->setFlatMode(true);
        lensgeom->hideMethodCombo();
        lensgeom->setParent(sec.content);
        lensgeom->setLevel(1);
        sec.content->pack_start(*lensgeom->getExpander(), false, false);

        distortion->setFlatMode(true);
        distortion->setParent(lensgeom->getSubToolsContainer());
        distortion->setLevel(2);
        lensgeom->getSubToolsContainer()->pack_start(*distortion->getExpander(), false, false);

        vignetting->setFlatMode(true);
        vignetting->setParent(lensgeom->getSubToolsContainer());
        vignetting->setLevel(2);
        lensgeom->getSubToolsContainer()->pack_start(*vignetting->getExpander(), false, false);

        transformPanel->pack_start(*sec.header, Gtk::PACK_SHRINK);
        transformPanel->pack_start(*sec.content, Gtk::PACK_SHRINK);
    }

    // Populate Selective panel (spot removal + locallab)
    // Simple bold section labels (not collapsible)
    Gtk::Label* spotLabel = Gtk::manage(new Gtk::Label());
    spotLabel->set_markup("<b>" + Glib::Markup::escape_text(M("TOOLGROUP_SPOT_REMOVAL")) + "</b>");
    spotLabel->set_halign(Gtk::ALIGN_START);
    spotLabel->set_margin_start(6);
    spotLabel->set_margin_top(4);
    spotLabel->set_margin_bottom(2);
    locallabPanel->pack_start(*spotLabel, Gtk::PACK_SHRINK);
    addPanel(locallabPanel, spot, 1);
    spot->setFlatMode(true);

    Gtk::Label* maskLabel = Gtk::manage(new Gtk::Label());
    maskLabel->set_markup("<b>" + Glib::Markup::escape_text(M("TOOLGROUP_MASKING")) + "</b>");
    maskLabel->set_halign(Gtk::ALIGN_START);
    maskLabel->set_margin_start(6);
    maskLabel->set_margin_top(4);
    maskLabel->set_margin_bottom(2);
    locallabPanel->pack_start(*maskLabel, Gtk::PACK_SHRINK);
    addPanel(locallabPanel, locallab, 1);
    locallab->setFlatMode(true);
    locallab->hideSettingsHeader();
    locallab->hideToolGroups();

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

    // Pack spot WB and color picker into WB's picker row
    if (whitebalance->getPickerRow() && toolBar->getWbTool()) {
        whitebalance->getPickerRow()->pack_start(*toolBar->getWbTool(), Gtk::PACK_SHRINK);
    }
    if (whitebalance->getPickerRow() && toolBar->getColPickerTool()) {
        whitebalance->getPickerRow()->pack_start(*toolBar->getColPickerTool(), Gtk::PACK_SHRINK);
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
        case Tool::TEXTURE:
            return Texture::TOOL_NAME;
        case Tool::CLARITY:
            return Clarity::TOOL_NAME;
        case Tool::GRAIN:
            return Grain::TOOL_NAME;
        case Tool::LENS_BLUR:
            return LensBlur::TOOL_NAME;
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

void ToolPanelCoordinator::bridgeGlobalToSpot(ProcParams* params)
{
    if (!maskModeActive_ || params->locallab.spots.empty()) return;

    const int idx = params->locallab.selspot;
    if (idx < 0 || idx >= (int)params->locallab.spots.size()) return;

    auto& spot = params->locallab.spots.at(idx);

    // Always bridge global tool values to the spot.
    // The widgets show spot values (loaded by loadSpotIntoGlobalTools),
    // so write() puts those values into global params. We copy them to the spot
    // and restore the original global params.

    // ToneCurve exposure → Exposure spot
    spot.expcomp = params->toneCurve.expcomp;
    spot.black = params->toneCurve.black;
    spot.hlcompr = params->toneCurve.hlcompr;
    spot.hlcomprthresh = params->toneCurve.hlcomprthresh;
    spot.shcompr = params->toneCurve.shcompr;
    // Enable expose sub-tool if any exposure value is non-default
    if (spot.expcomp != 0.0 || spot.black != 0 ||
        spot.hlcompr != 0 || spot.hlcomprthresh != 0 || spot.shcompr != 50) {
        spot.expexpose = true;
        spot.visiexpose = true;
    }

    // ToneCurve brightness/contrast/saturation → Color spot
    spot.lightness = params->toneCurve.brightness;
    spot.contrast = params->toneCurve.contrast;
    spot.chroma = params->toneCurve.saturation;
    if (spot.lightness != 0 || spot.contrast != 0 || spot.chroma != 0) {
        spot.expcolor = true;
        spot.visicolor = true;
    }

    // Vibrance → Vibrance spot
    spot.saturated = params->vibrance.saturated;
    spot.pastels = params->vibrance.pastels;
    spot.psthreshold = params->vibrance.psthreshold;
    spot.protectskins = params->vibrance.protectskins;
    spot.avoidcolorshift = params->vibrance.avoidcolorshift;
    spot.pastsattog = params->vibrance.pastsattog;
    if (spot.pastels != 0 || spot.saturated != 0) {
        spot.expvibrance = true;
        spot.visivibrance = true;
    }

    // Sharpening → Sharp spot
    spot.sharamount = params->sharpening.amount;
    spot.sharradius = params->sharpening.radius;
    spot.sharcontrast = static_cast<int>(params->sharpening.contrast);
    if (spot.sharamount != 0) {
        spot.expsharp = true;
        spot.visisharp = true;
    }

    // ShadowsHighlights → Shadow spot
    spot.highlights = params->sh.highlights;
    spot.shadows = params->sh.shadows;
    spot.h_tonalwidth = params->sh.htonalwidth;
    spot.s_tonalwidth = params->sh.stonalwidth;
    if (spot.highlights != 0 || spot.shadows != 0) {
        spot.expshadhigh = true;
        spot.visishadhigh = true;
    }

    // Restore global params to saved state (no global effect)
    params->toneCurve = savedToneCurve_;
    params->vibrance = savedVibrance_;
    params->sharpening = savedSharpening_;
    params->sh = savedSH_;
}

void ToolPanelCoordinator::loadSpotIntoGlobalTools()
{
    if (!ipc) return;

    ProcParams* params = ipc->beginUpdateParams();
    if (params->locallab.spots.empty()) {
        ipc->endUpdateParams(0);
        return;
    }

    const int idx = params->locallab.selspot;
    if (idx < 0 || idx >= (int)params->locallab.spots.size()) {
        ipc->endUpdateParams(0);
        return;
    }

    const auto& spot = params->locallab.spots.at(idx);

    // Map spot values to global fields in a temp copy
    ProcParams tempParams = *params;
    tempParams.toneCurve.expcomp = spot.expcomp;
    tempParams.toneCurve.black = spot.black;
    tempParams.toneCurve.hlcompr = spot.hlcompr;
    tempParams.toneCurve.hlcomprthresh = spot.hlcomprthresh;
    tempParams.toneCurve.shcompr = spot.shcompr;
    tempParams.toneCurve.brightness = spot.lightness;
    tempParams.toneCurve.contrast = spot.contrast;
    tempParams.toneCurve.saturation = spot.chroma;

    tempParams.vibrance.pastels = spot.pastels;
    tempParams.vibrance.saturated = spot.saturated;
    tempParams.vibrance.psthreshold = spot.psthreshold;
    tempParams.vibrance.protectskins = spot.protectskins;
    tempParams.vibrance.avoidcolorshift = spot.avoidcolorshift;
    tempParams.vibrance.pastsattog = spot.pastsattog;

    tempParams.sharpening.amount = spot.sharamount;
    tempParams.sharpening.radius = spot.sharradius;
    tempParams.sharpening.contrast = spot.sharcontrast;

    tempParams.sh.highlights = spot.highlights;
    tempParams.sh.shadows = spot.shadows;
    tempParams.sh.htonalwidth = spot.h_tonalwidth;
    tempParams.sh.stonalwidth = spot.s_tonalwidth;

    ipc->endUpdateParams(0);  // release lock, no reprocess

    // Update widget display (listener disabled to prevent recursive panelChanged)
    toneCurve->disableListener();
    toneCurve->read(&tempParams);
    toneCurve->enableListener();

    vibrance->disableListener();
    vibrance->read(&tempParams);
    vibrance->enableListener();

    sharpening->disableListener();
    sharpening->read(&tempParams);
    sharpening->enableListener();

    shadowshighlights->disableListener();
    shadowshighlights->read(&tempParams);
    shadowshighlights->enableListener();
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

    // Reparent global ToolGroups between editPanel and locallabPanel
    if (mode == EditorMode::MASK && prevMode != EditorMode::MASK) {
        editPanel->remove(*lightGroup);
        editPanel->remove(*colorGroup);
        editPanel->remove(*detailGroup);
        editPanel->remove(*effectsGroup);
        editPanel->remove(*calibrationGroup);
        locallabPanel->pack_start(*lightGroup, Gtk::PACK_SHRINK);
        locallabPanel->pack_start(*colorGroup, Gtk::PACK_SHRINK);
        locallabPanel->pack_start(*detailGroup, Gtk::PACK_SHRINK);
        locallabPanel->pack_start(*effectsGroup, Gtk::PACK_SHRINK);
        locallabPanel->pack_start(*calibrationGroup, Gtk::PACK_SHRINK);
    }
    if (prevMode == EditorMode::MASK && mode != EditorMode::MASK) {
        locallabPanel->remove(*lightGroup);
        locallabPanel->remove(*colorGroup);
        locallabPanel->remove(*detailGroup);
        locallabPanel->remove(*effectsGroup);
        locallabPanel->remove(*calibrationGroup);
        editPanel->pack_start(*lightGroup, Gtk::PACK_SHRINK);
        editPanel->pack_start(*colorGroup, Gtk::PACK_SHRINK);
        editPanel->pack_start(*detailGroup, Gtk::PACK_SHRINK);
        editPanel->pack_start(*effectsGroup, Gtk::PACK_SHRINK);
        editPanel->pack_start(*calibrationGroup, Gtk::PACK_SHRINK);
    }

    // Handle Locallab subscription/unsubscription
    if (photoLoadedOnce) {
        if (mode == EditorMode::MASK) {
            toolBar->blockEditDeactivation();
            locallab->subscribe();

            // Auto-enable locallab when entering mask mode (required for overlay rendering)
            if (!locallab->getEnabled()) {
                locallab->setEnabled(true);
                locallab->enabledChanged();
            }

            // Fire mask visibility so the engine shows the overlay
            if (ipc) {
                const Locallab::llMaskVisibility mv = locallab->getMaskVisibility();
                ipc->setLocallabMaskVisibility(mv.previewDeltaE, mv.showMaskOverlay,
                    mv.colorMask, mv.colorMaskinv, mv.expMask, mv.expMaskinv,
                    mv.SHMask, mv.SHMaskinv, mv.vibMask, mv.softMask,
                    mv.blMask, mv.tmMask, mv.retiMask, mv.sharMask,
                    mv.lcMask, mv.cbMask, mv.logMask, mv.maskMask, mv.cieMask);
                // Trigger reprocess so the mask overlay renders immediately
                panelChanged(rtengine::EvlocallabshowmaskMethod, "");
            }

            // Save global params and load spot values into global tools
            maskModeActive_ = true;
            if (ipc) {
                ProcParams* p = ipc->beginUpdateParams();
                savedToneCurve_ = p->toneCurve;
                savedVibrance_ = p->vibrance;
                savedSharpening_ = p->sharpening;
                savedSH_ = p->sh;
                ipc->endUpdateParams(0);
            }
            loadSpotIntoGlobalTools();
        }

        if (prevMode == EditorMode::MASK && mode != EditorMode::MASK) {
            // Restore original global params and re-read tools
            maskModeActive_ = false;
            if (ipc) {
                ProcParams* p = ipc->beginUpdateParams();
                p->toneCurve = savedToneCurve_;
                p->vibrance = savedVibrance_;
                p->sharpening = savedSharpening_;
                p->sh = savedSH_;
                ipc->endUpdateParams(rtengine::RefreshMapper::getInstance()->getAction(
                    rtengine::EvlocallabshowmaskMethod));

                ProcParams restored;
                ProcParams* current = ipc->beginUpdateParams();
                restored = *current;
                ipc->endUpdateParams(0);

                toneCurve->disableListener();
                toneCurve->read(&restored);
                toneCurve->enableListener();
                vibrance->disableListener();
                vibrance->read(&restored);
                vibrance->enableListener();
                sharpening->disableListener();
                sharpening->read(&restored);
                sharpening->enableListener();
                shadowshighlights->disableListener();
                shadowshighlights->read(&restored);
                shadowshighlights->enableListener();
            }

            toolBar->blockEditDeactivation(false);
            locallab->unsubscribe();
            // Clear mask overlay when leaving mask mode (non-zero = inactive)
            if (ipc) {
                ipc->setLocallabMaskVisibility(false, false,
                    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1);
                panelChanged(rtengine::EvlocallabshowmaskMethod, "");
            }
        }
    }

    prevMode = mode;
}

void ToolPanelCoordinator::populateEditPanel()
{
    // Exposure & Tone group: B&W toggle + summary sliders + RGB Curves
    {
        // B&W toggle button
        bwToggle_ = Gtk::manage(new Gtk::ToggleButton("B&&W"));
        bwToggle_->set_relief(Gtk::RELIEF_NONE);
        bwToggle_->set_can_focus(false);
        bwToggle_->set_halign(Gtk::ALIGN_START);
        bwToggle_->set_margin_start(4);
        bwToggle_->set_margin_top(2);
        bwToggle_->set_margin_bottom(2);
        auto bwCss = Gtk::CssProvider::create();
        bwCss->load_from_data(
            "button { font-size: 9px; font-weight: bold; padding: 2px 8px;"
            "  min-height: 0; border-radius: 3px;"
            "  background: transparent; background-image: none;"
            "  border: 1px solid alpha(@theme_fg_color, 0.3); }"
            "button:checked { background-color: alpha(@theme_fg_color, 0.15);"
            "  border-color: alpha(@theme_fg_color, 0.5); }"
            "button:hover { background-color: rgba(130,170,230,0.22); }");
        bwToggle_->get_style_context()->add_provider(
            bwCss, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 200);
        lightGroup->getContentBox()->pack_start(*bwToggle_, Gtk::PACK_SHRINK);
        bwConn_ = bwToggle_->signal_toggled().connect([this]() {
            blackwhite->setEnabled(bwToggle_->get_active());
            blackwhite->enabledChanged();
        });
    }
    addPanel(lightGroup->getContentBox(), toneCurve, 1);
    toneCurve->setFlatMode(true);
    toneCurve->collapseDetail();
    addPanel(lightGroup->getContentBox(), shadowshighlights, 1);
    shadowshighlights->setFlatMode(true);
    shadowshighlights->collapseDetail();
    addPanel(lightGroup->getContentBox(), rgbcurves, 1);
    rgbcurves->setFlatMode(true);

    // Color group: WB, Vibrance (below tint), HSV Eq, Color Grading, Point Color
    addPanel(colorGroup->getContentBox(), whitebalance, 1);
    whitebalance->setFlatMode(true);
    whitebalance->collapseDetail();
    addPanel(colorGroup->getContentBox(), vibrance, 1);
    vibrance->setFlatMode(true);
    vibrance->collapseDetail();
    addPanel(colorGroup->getContentBox(), hsvequalizer, 1);
    hsvequalizer->setFlatMode(true);
    hsvequalizer->collapseDetail();
    addPanel(colorGroup->getContentBox(), colorgrading, 1);
    colorgrading->setFlatMode(true);
    colorgrading->collapseDetail();
    addPanel(colorGroup->getContentBox(), pointcolor, 1);
    pointcolor->setFlatMode(true);

    // Detail group: Sharpening, Local Contrast, Noise Reduction, AI Denoise, Dehaze
    addPanel(detailGroup->getContentBox(), sharpening, 1);
    sharpening->setFlatMode(true);
    sharpening->collapseDetail();
    addPanel(detailGroup->getContentBox(), dirpyrdenoise, 1);
    dirpyrdenoise->setFlatMode(true);
    addPanel(detailGroup->getContentBox(), aidenoise, 1);
    aidenoise->setFlatMode(true);
    addPanel(detailGroup->getContentBox(), dehaze, 1);
    dehaze->setFlatMode(true);
    dehaze->collapseDetail();

    // Effects group: Texture, Clarity, Grain, PC Vignette, Gradient, Film Simulation
    addPanel(effectsGroup->getContentBox(), texture, 1);
    texture->setFlatMode(true);
    texture->collapseDetail();
    addPanel(effectsGroup->getContentBox(), clarity, 1);
    clarity->setFlatMode(true);
    clarity->collapseDetail();
    addPanel(effectsGroup->getContentBox(), grain, 1);
    grain->setFlatMode(true);
    grain->collapseDetail();
    addPanel(effectsGroup->getContentBox(), pcvignette, 1);
    pcvignette->setFlatMode(true);
    pcvignette->collapseDetail();
    addPanel(effectsGroup->getContentBox(), gradient, 1);
    gradient->setFlatMode(true);
    gradient->collapseDetail();
    addPanel(effectsGroup->getContentBox(), filmSimulation, 1);
    filmSimulation->setFlatMode(true);
    filmSimulation->collapseDetail();

    // Advanced group: Niche/legacy tools (hidden from main panel)
    addPanel(advancedGroup->getContentBox(), blackwhite, 1);
    blackwhite->setFlatMode(true);
    addPanel(advancedGroup->getContentBox(), toneEqualizer, 1);
    toneEqualizer->setFlatMode(true);
    addPanel(advancedGroup->getContentBox(), epd, 1);
    epd->setFlatMode(true);
    addPanel(advancedGroup->getContentBox(), dirpyrequalizer, 1);
    dirpyrequalizer->setFlatMode(true);
    addPanel(advancedGroup->getContentBox(), chmixer, 1);
    chmixer->setFlatMode(true);
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

    // Camera group: Lens tools only
    addPanel(calibrationGroup->getContentBox(), defringe, 1);
    defringe->setFlatMode(true);
    addPanel(calibrationGroup->getContentBox(), lensProf, 1);
    lensProf->setFlatMode(true);
    addPanel(calibrationGroup->getContentBox(), lensblur, 1);
    lensblur->setFlatMode(true);

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

void ToolPanelCoordinator::turnOffMaskOverlay(bool /*forceRedraw*/)
{
    if (!ipc) return;

    hoverMaskApplied_ = false;
    pendingHoverState_ = false;
    hoverMaskDebounce_.disconnect();
    hoverMaskWatchdog_.disconnect();

    locallab->setHoverMaskOverlay(false);
    ipc->setLocallabMaskVisibility(false, false,
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1);

    // Trigger reprocess to clear overlay from dcrop
    ipc->beginUpdateParams();
    ipc->endUpdateParams(AUTOEXP);
}

void ToolPanelCoordinator::hoverMaskChanged(bool hover, bool forceRedraw)
{
    if (!ipc) return;

    // Cancel any pending debounce
    hoverMaskDebounce_.disconnect();
    pendingHoverState_ = hover;

    if (!hover) {
        // Turn off immediately — never debounce "off"
        turnOffMaskOverlay(forceRedraw);
        return;
    }

    // Skip if already on
    if (hoverMaskApplied_) return;

    // Debounce "on" to avoid expensive reprocessing on rapid mouse movement
    hoverMaskDebounce_ = Glib::signal_timeout().connect([this]() {
        if (!ipc || !pendingHoverState_) return false;

        hoverMaskApplied_ = true;
        locallab->setHoverMaskOverlay(true);
        const Locallab::llMaskVisibility mv = locallab->getMaskVisibility();
        ipc->setLocallabMaskVisibility(mv.previewDeltaE, mv.showMaskOverlay,
            mv.colorMask, mv.colorMaskinv, mv.expMask, mv.expMaskinv,
            mv.SHMask, mv.SHMaskinv, mv.vibMask, mv.softMask,
            mv.blMask, mv.tmMask, mv.retiMask, mv.sharMask,
            mv.lcMask, mv.cbMask, mv.logMask, mv.maskMask, mv.cieMask);
        ipc->beginUpdateParams();
        ipc->endUpdateParams(AUTOEXP);

        // Start watchdog: periodically check actual pointer position
        // This catches cases where leave events are missed (common on WSLg/X11)
        hoverMaskWatchdog_ = Glib::signal_timeout().connect([this]() {
            if (!hoverMaskApplied_) {
                return false;  // already off
            }
            if (!locallab->isPointerOverMaskList()) {
                turnOffMaskOverlay();
                return false;  // stop watchdog
            }
            return true;  // keep checking
        }, 300);

        return false;  // one-shot debounce
    }, 100);
}

void ToolPanelCoordinator::applyHoverMask()
{
    // Unused — logic inlined into hoverMaskChanged
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

    // Bridge global tool values to selected locallab spot in mask mode
    bridgeGlobalToSpot(params);

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
    } else if (event == rtengine::EvLocallabSpotCreated || event == rtengine::EvLocallabSpotSelected ||
            event == rtengine::EvLocallabSpotSelectedWithMask ||
            event == rtengine::EvLocallabSpotDeleted /*|| event == rtengine::Evlocallabshowreset*/ ||
            event == rtengine::EvlocallabToolRemovedWithRefresh) {
        locallab->resetMaskVisibility();
        hoverMaskApplied_ = false;
        hoverMaskDebounce_.disconnect();
        hoverMaskWatchdog_.disconnect();
        ipc->setLocallabMaskVisibility(false, false, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1);
    }

    ipc->endUpdateParams(changeFlags);    // starts the IPC processing

    hasChanged = true;

    for (auto paramcListener : paramcListeners) {
        paramcListener->procParamsChanged(params, event, descr);
    }

    // When spot changes in mask mode, reload spot values into global tools
    if (maskModeActive_ &&
        (event == rtengine::EvLocallabSpotSelectedWithMask ||
         event == rtengine::EvLocallabSpotCreated)) {
        loadSpotIntoGlobalTools();
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

    // Sync B&W toggle with blackwhite tool state
    if (bwToggle_) {
        bwConn_.block();
        bwToggle_->set_active(blackwhite->getEnabled());
        bwConn_.unblock();
    }

    if (event == rtengine::EvPhotoLoaded || event == rtengine::EvProfileChanged || event == rtengine::EvHistoryBrowsed || event == rtengine::EvCTRotate) {
        // updating the "on preview" geometry
        gradient->updateGeometry(params->gradient.centerX, params->gradient.centerY, params->gradient.feather, params->gradient.degree, fw, fh);
    }

    // Reset Locallab mask visibility — overlay off by default (hover-driven)
    locallab->resetMaskVisibility();
    hoverMaskApplied_ = false;
    hoverMaskDebounce_.disconnect();
    ipc->setLocallabMaskVisibility(false, false, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1);

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
        Tool::HSV_EQUALIZER, Tool::POINT_COLOR, Tool::TEXTURE, Tool::CLARITY,
        Tool::GRAIN, Tool::FILM_SIMULATION, Tool::SOFT_LIGHT,
        Tool::DEHAZE, Tool::SENSOR_BAYER, Tool::SENSOR_XTRANS,
        Tool::BAYER_PROCESS, Tool::XTRANS_PROCESS, Tool::BAYER_PREPROCESS,
        Tool::PREPROCESS, Tool::DARKFRAME_TOOL, Tool::FLATFIELD_TOOL,
        Tool::RAW_CA_CORRECTION, Tool::RAW_EXPOSURE, Tool::PREPROCESS_WB,
        Tool::BAYER_RAW_EXPOSURE, Tool::XTRANS_RAW_EXPOSURE, Tool::FATTAL,
        Tool::FILM_NEGATIVE, Tool::PD_SHARPENING,
    };

    for (const auto& tool : allTools) {
        // Spot and Locallab are always visible on the Mask page
        if (tool == Tool::SPOT || tool == Tool::LOCALLAB) continue;

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
        perspective->setDragEditMode(false);
    }
}

void ToolPanelCoordinator::expandTransformSection(Gtk::Box* content, Gtk::Label* label, const Glib::ustring& name)
{
    if (content && !content->get_visible()) {
        content->set_no_show_all(false);
        content->show_all();
        content->set_no_show_all(true);
        if (label) {
            label->set_markup("\u25BE <b>" + Glib::Markup::escape_text(name) + "</b>");
        }
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
            expandTransformSection(cropSectionContent_, cropSectionLabel_, M("TP_CROP_LABEL"));
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
            modeButtonBar->setActiveMode(EditorMode::CROPPING);
            modeChanged(EditorMode::CROPPING);
            break;
        }

        case TMPerspective: {
            toolBar->blockEditDeactivation(false);
            perspective->setDragEditMode(true);
            perspective->setExpanded(true);
            expandTransformSection(perspSectionContent_, perspSectionLabel_, M("TP_PERSPECTIVE_LABEL"));
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

void ToolPanelCoordinator::setLevelingGridCallback(std::function<void(bool)> cb)
{
    rotate->setLevelingGridCallback(std::move(cb));
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
        case Tool::TEXTURE:
            return texture;
        case Tool::CLARITY:
            return clarity;
        case Tool::GRAIN:
            return grain;
        case Tool::LENS_BLUR:
            return lensblur;
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
