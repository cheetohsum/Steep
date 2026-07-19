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
#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>

#include "rtengine/rt_math.h"

#include "autoedit.h"
#include "imagearea.h"
#include "multilangmgr.h"
#include "rawloadactivity.h"
#include "toolpanelcoord.h"
#include "metadatapanel.h"
#include "options.h"
#include "rtimage.h"
#include "thumbnail.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>

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

namespace
{

constexpr unsigned int kQuickPreviewHighDetailDelayMs = 650;

bool toolPanelEditLogEnabled()
{
    static const bool enabled = std::getenv("STEEP_FILESEL_LOG") != nullptr;
    return enabled;
}

void toolPanelEditLog(const char* fmt, ...)
{
    if (!toolPanelEditLogEnabled()) {
        return;
    }

    static std::mutex logMutex;
    std::lock_guard<std::mutex> lock(logMutex);

    static FILE* f = nullptr;
    if (!f) {
        const char* home = std::getenv("USERPROFILE");
        if (!home) {
            home = std::getenv("HOME");
        }
        const std::string path = home ? std::string(home) + "\\steep-fileSel.log" : "steep-fileSel.log";
        f = std::fopen(path.c_str(), "a");
    }
    if (!f) {
        return;
    }

    std::fprintf(f, "[edit] ");
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(f, fmt, ap);
    va_end(ap);
    std::fflush(f);
}

long long toolPanelDurationMs(const std::chrono::steady_clock::time_point& start)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start)
        .count();
}

int sidebarPageWidth()
{
    // Match the usable width of the fixed editor sidebar: the outer panel is
    // clamped in editorpanel.cc and this page leaves room for the scrollbar.
    constexpr int minPanelWidth = 300;
    constexpr int maxPanelWidth = 340;
    constexpr int scrollbarGutter = 18;

    const int panelWidth =
        std::min(std::max(App::get().options().toolPanelWidth, minPanelWidth), maxPanelWidth);

    return std::max(282, panelWidth - scrollbarGutter);
}

class SidebarPageBox final : public Gtk::Box
{
public:
    SidebarPageBox() :
        Gtk::Box(Gtk::ORIENTATION_VERTICAL)
    {
        set_size_request(sidebarPageWidth(), -1);
        set_hexpand(false);
        set_halign(Gtk::ALIGN_START);
    }

    void get_preferred_width_vfunc(int& minimum_width, int& natural_width) const override
    {
        Gtk::Box::get_preferred_width_vfunc(minimum_width, natural_width);
        const int width = sidebarPageWidth();
        minimum_width = std::min(minimum_width, width);
        natural_width = width;
    }
};

void configureSidebarPage(Gtk::Widget* widget)
{
    if (!widget) {
        return;
    }

    widget->set_size_request(sidebarPageWidth(), -1);
    widget->set_hexpand(false);
    widget->set_halign(Gtk::ALIGN_START);
}

}

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
        .id = Tool::TILT_SHIFT,
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
        .id = Tool::FILM_PRESETS,
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

ToolPanelCoordinator::ToolPanelCoordinator (bool batch) : ipc (nullptr), favoritePanelSW(nullptr), hasChanged (false), batch(batch), editDataProvider (nullptr), imageArea_(nullptr), photoLoadedOnce(false), ornamentSurface(new RTSurface("ornament1.svg")), prevMode(EditorMode::EDIT)
{
    quickAutoEditPool_.reset(new Glib::ThreadPool(1, true));
    quickAutoEditGeneration_ = std::make_shared<std::atomic<unsigned>>(0);
    colorPickerRow_ = nullptr;

    whitebalance = nullptr;
    vignetting = nullptr;
    gradient = nullptr;
    locallab = nullptr;
    retinex = nullptr;
    pcvignette = nullptr;
    lensgeom = nullptr;
    lensProf = nullptr;
    rotate = nullptr;
    distortion = nullptr;
    perspective = nullptr;
    cacorrection = nullptr;
    colorappearance = nullptr;
    vibrance = nullptr;
    chmixer = nullptr;
    blackwhite = nullptr;
    resize = nullptr;
    framing = nullptr;
    prsharpening = nullptr;
    icm = nullptr;
    crop = nullptr;
    toneCurve = nullptr;
    shadowshighlights = nullptr;
    toneEqualizer = nullptr;
    localContrast = nullptr;
    texture = nullptr;
    clarity = nullptr;
    grain = nullptr;
    tiltshift = nullptr;
    lensblur = nullptr;
    spot = nullptr;
    defringe = nullptr;
    compressgamut = nullptr;
    impulsedenoise = nullptr;
    aidenoise = nullptr;
    dirpyrdenoise = nullptr;
    epd = nullptr;
    sharpening = nullptr;
    sharpenEdge = nullptr;
    sharpenMicro = nullptr;
    lcurve = nullptr;
    rgbcurves = nullptr;
    colortoning = nullptr;
    colorgrading = nullptr;
    wavelet = nullptr;
    dirpyrequalizer = nullptr;
    hsvequalizer = nullptr;
    pointcolor = nullptr;
    softlight = nullptr;
    dehaze = nullptr;
    filmSimulation = nullptr;
    filmPresets = nullptr;
    sensorbayer = nullptr;
    sensorxtrans = nullptr;
    bayerprocess = nullptr;
    xtransprocess = nullptr;
    bayerpreprocess = nullptr;
    preprocess = nullptr;
    darkframe = nullptr;
    flatfield = nullptr;
    rawcacorrection = nullptr;
    rawexposure = nullptr;
    preprocessWB = nullptr;
    bayerrawexposure = nullptr;
    xtransrawexposure = nullptr;
    fattal = nullptr;
    metadata = nullptr;
    filmNegative = nullptr;
    pdSharpening = nullptr;
    toolBar = nullptr;
    toolPanelNotebook = nullptr;
    modeButtonBar = nullptr;
    modeStack = nullptr;

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
    if (batch) {
        toolPanels.push_back(coarse);

        toolBar = new ToolBar();
        toolBar->setToolBarListener(this);
        toolBar->hideCropTools();

        toolPanelNotebook = new Gtk::Notebook();
        toolPanelNotebook->set_name("ToolPanelNotebook");

        favoritePanelSW.reset(nullptr);
        exposurePanelSW    = nullptr;
        detailsPanelSW     = nullptr;
        colorPanelSW       = nullptr;
        rawPanelSW         = nullptr;
        advancedPanelSW    = nullptr;

        editPanelSW        = Gtk::manage(new MyScrolledWindow());
        transformPanelSW   = nullptr;
        locallabPanelSW    = nullptr;
        locallabPanelContainer_ = nullptr;
        editPanelSW->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
        editPanelSW->set_propagate_natural_height(true);

        auto* batchPanel = Gtk::manage(new SidebarPageBox());
        batchPanel->set_margin_start(8);
        batchPanel->set_margin_end(8);
        batchPanel->set_margin_top(8);
        batchPanel->set_margin_bottom(8);
        editPanelSW->add(*batchPanel);

        modeButtonBar = Gtk::manage(new ModeButtonBar());
        modeButtonBar->setModeVisible(EditorMode::PRESETS, false);
        modeButtonBar->setModeVisible(EditorMode::CROPPING, false);
        modeButtonBar->setModeVisible(EditorMode::MASK, false);

        modeStack = Gtk::manage(new Gtk::Stack());
        modeStack->set_hhomogeneous(false);
        modeStack->set_vhomogeneous(false);
        modeStack->add(*editPanelSW, "edit");
        modeStack->show_all();
        modeStack->set_visible_child("edit");

        modeconn = modeButtonBar->signal_mode_changed().connect(
            sigc::mem_fun(*this, &ToolPanelCoordinator::modeChanged));
        modeButtonBar->setActiveMode(EditorMode::EDIT);

        for (auto* toolPanel : toolPanels) {
            toolPanel->setListener(this);
        }
        return;
    }
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
    tiltshift           = Gtk::manage(new TiltShift());
    lensblur            = Gtk::manage(new LensBlur());
    filmSimulation      = Gtk::manage(new FilmSimulation());
    filmPresets         = Gtk::manage(new FilmPresets());
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
            if (!tool_panel) {
                continue;
            }
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

    // Mode-based scrolled windows
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
    bwGroup          = Gtk::manage(new ToolGroup(M("TOOLGROUP_BW")));
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
    editPanel->pack_start(*bwGroup, Gtk::PACK_SHRINK);
    editPanel->pack_start(*colorGroup, Gtk::PACK_SHRINK);
    editPanel->pack_start(*detailGroup, Gtk::PACK_SHRINK);
    editPanel->pack_start(*effectsGroup, Gtk::PACK_SHRINK);
    editPanel->pack_start(*calibrationGroup, Gtk::PACK_SHRINK);

    // Metadata is now inside the Advanced group

    // Edit panel scrolled window
    Gtk::Box *editPanelContainer =
        Gtk::manage(new SidebarPageBox());
    configureSidebarPage(editPanel);
    editPanelSW->add(*editPanelContainer);
    editPanelContainer->pack_start(*editPanel, Gtk::PACK_SHRINK);
    editPanelContainer->pack_start(*vbPanelEnd[0], Gtk::PACK_SHRINK);

    // Transform/Crop panel scrolled window
    Gtk::Box *transformPanelContainer =
        Gtk::manage(new SidebarPageBox());
    configureSidebarPage(transformPanel);
    transformPanelSW->add(*transformPanelContainer);
    transformPanelContainer->pack_start(*transformPanel, Gtk::PACK_SHRINK);
    transformPanelContainer->pack_start(*vbPanelEnd[1], Gtk::PACK_SHRINK);

    // Locallab/Mask panel scrolled window
    locallabPanelContainer_ =
        Gtk::manage(new SidebarPageBox());
    configureSidebarPage(locallabPanel);
    locallabPanelSW->add(*locallabPanelContainer_);
    locallabPanelContainer_->pack_start(*locallabPanel, Gtk::PACK_SHRINK);
    locallabPanelContainer_->pack_start(*vbPanelEnd[2], Gtk::PACK_SHRINK);

    // Create ModeButtonBar
    modeButtonBar = Gtk::manage(new ModeButtonBar());

    // Create mode stack — homogeneous=false so sidebar sizes to current mode's content
    modeStack = Gtk::manage(new Gtk::Stack());
    modeStack->set_hhomogeneous(false);
    modeStack->set_vhomogeneous(false);
    modeStack->set_transition_type(Gtk::STACK_TRANSITION_TYPE_SLIDE_LEFT);
    modeStack->set_transition_duration(200);

    // Presets page is added by EditorPanel via presetListPanel->getWidget()
    modeStack->add(*editPanelSW, "edit");
    modeStack->add(*transformPanelSW, "crop");
    modeStack->add(*locallabPanelSW, "mask");

    // Create spot + masking groups (needed by populateEditPanel's reset callbacks)
    spotGroup = Gtk::manage(new ToolGroup(M("TOOLGROUP_SPOT_REMOVAL")));
    maskingGroup = Gtk::manage(new ToolGroup(M("TOOLGROUP_MASKING")));

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
        headerRow->set_margin_end(8);
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

        // Add straighten, crop-select and reset buttons to header row (after label)
        Gtk::Button* straighten = mkBtn("rotate-straighten-small", "TP_ROTATE_SELECTLINE");
        straighten->signal_pressed().connect([this]() { straightenRequested(); });
        sec.header->pack_start(*straighten, Gtk::PACK_SHRINK);
        sec.header->pack_start(*crop->getSelectCropButton(), Gtk::PACK_SHRINK);
        sec.header->pack_start(*crop->getResetCropButton(), Gtk::PACK_SHRINK);

        // Reset X button for crop section
        cropResetBtn_ = Gtk::manage(new Gtk::Button());
        cropResetBtn_->set_relief(Gtk::RELIEF_NONE);
        cropResetBtn_->set_can_focus(false);
        cropResetBtn_->set_tooltip_text("Reset to defaults");
        auto* cropResetLabel = Gtk::manage(new Gtk::Label());
        cropResetLabel->set_use_markup(true);
        cropResetLabel->set_markup("<small>\xc3\x97</small>");
        cropResetBtn_->add(*cropResetLabel);
        cropResetBtn_->set_no_show_all(true);
        cropResetBtn_->set_name("ToolGroupReset");
        {
            auto css = Gtk::CssProvider::create();
            css->load_from_data(
                "#ToolGroupReset { padding: 1px 3px; margin: 0 2px; min-height: 12px; min-width: 12px; border: none; background: none; background-image: none; box-shadow: none; }"
                "#ToolGroupReset:hover { background-color: rgba(200,80,80,0.3); border-radius: 3px; }"
                "#ToolGroupReset label { font-size: 9px; color: #aaaaaa; min-height: 0; padding: 0; margin: 0; }"
                "#ToolGroupReset:hover label { color: #ffffff; }"
            );
            cropResetBtn_->get_style_context()->add_provider(css, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 200);
        }
        cropResetBtn_->signal_clicked().connect([this]() {
            rtengine::procparams::ProcParams dp;
            crop->disableListener();
            crop->read(&dp);
            crop->enableListener();
            rotate->disableListener();
            rotate->read(&dp);
            rotate->enableListener();
            suppressResetUpdate_ = true;
            panelChanged(rtengine::EvProfileChanged, M("GENERAL_CHANGED"));
            suppressResetUpdate_ = false;
            cropResetBtn_->set_visible(false);
        });
        sec.header->pack_end(*cropResetBtn_, Gtk::PACK_SHRINK, 0);

        // Pack crop's expander into the collapsible content
        crop->setParent(sec.content);
        crop->setLevel(1);
        crop->setFlatMode(true);
        sec.content->pack_start(*crop->getExpander(), false, false);

        // Ratio row always visible below crop header
        Gtk::Widget* ratioRow = crop->getRatioRow();
        ratioRow->set_margin_start(10);
        ratioRow->set_margin_end(4);
        ratioRow->show_all();

        transformPanel->pack_start(*sec.header, Gtk::PACK_SHRINK);
        transformPanel->pack_start(*ratioRow, Gtk::PACK_SHRINK);
        transformPanel->pack_start(*sec.content, Gtk::PACK_SHRINK);
    }

    // --- Rotate section (inline) ---
    {
        rotate->setParent(transformPanel);
        rotate->setLevel(1);
        // rotate already has setFlatMode(true) in its constructor
        transformPanel->pack_start(*rotate->getExpander(), false, false);
    }

    // --- Perspective section (collapsible) ---
    {
        auto sec = mkCollapsible(M("TP_PERSPECTIVE_LABEL"));
        perspSectionContent_ = sec.content;
        perspSectionLabel_ = sec.label;

        Gtk::Button* perspSel = mkBtn("perspective-vertical-bottom", "TOOLBAR_TOOLTIP_PERSPECTIVE");
        perspSel->signal_pressed().connect([this]() {
            if (toolBar->getTool() == TMPerspective) {
                toolBar->setTool(TMHand);
                toolDeselected(TMPerspective);
            } else {
                toolBar->setTool(TMPerspective);
                toolSelected(TMPerspective);
            }
        });
        sec.header->pack_start(*perspSel, Gtk::PACK_SHRINK);

        Gtk::Button* perspGridSel = mkBtn("perspective-grid", "TP_PERSPECTIVE_GRID_TOOLTIP");
        perspGridSel->signal_pressed().connect([this]() {
            if (toolBar->getTool() == TMPerspectiveGrid) {
                toolBar->setTool(TMHand);
                toolDeselected(TMPerspectiveGrid);
            } else {
                toolBar->setTool(TMPerspectiveGrid);
                toolSelected(TMPerspectiveGrid);
            }
        });
        sec.header->pack_start(*perspGridSel, Gtk::PACK_SHRINK);

        // Reset X button for perspective section
        perspResetBtn_ = Gtk::manage(new Gtk::Button());
        perspResetBtn_->set_relief(Gtk::RELIEF_NONE);
        perspResetBtn_->set_can_focus(false);
        perspResetBtn_->set_tooltip_text("Reset to defaults");
        auto* perspResetLabel = Gtk::manage(new Gtk::Label());
        perspResetLabel->set_use_markup(true);
        perspResetLabel->set_markup("<small>\xc3\x97</small>");
        perspResetBtn_->add(*perspResetLabel);
        perspResetBtn_->set_no_show_all(true);
        perspResetBtn_->set_name("ToolGroupReset");
        {
            auto css = Gtk::CssProvider::create();
            css->load_from_data(
                "#ToolGroupReset { padding: 1px 3px; margin: 0 2px; min-height: 12px; min-width: 12px; border: none; background: none; background-image: none; box-shadow: none; }"
                "#ToolGroupReset:hover { background-color: rgba(200,80,80,0.3); border-radius: 3px; }"
                "#ToolGroupReset label { font-size: 9px; color: #aaaaaa; min-height: 0; padding: 0; margin: 0; }"
                "#ToolGroupReset:hover label { color: #ffffff; }"
            );
            perspResetBtn_->get_style_context()->add_provider(css, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 200);
        }
        perspResetBtn_->signal_clicked().connect([this]() {
            rtengine::procparams::ProcParams dp;
            perspective->disableListener();
            perspective->read(&dp);
            perspective->enableListener();
            suppressResetUpdate_ = true;
            panelChanged(rtengine::EvProfileChanged, M("GENERAL_CHANGED"));
            suppressResetUpdate_ = false;
            perspResetBtn_->set_visible(false);
        });
        sec.header->pack_end(*perspResetBtn_, Gtk::PACK_SHRINK, 0);

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
    // spotGroup and maskingGroup already created before populateEditPanel()
    locallabPanel->pack_start(*spotGroup, Gtk::PACK_SHRINK);
    addPanel(spotGroup->getContentBox(), spot, 1);
    spot->setFlatMode(true);

    if (locallab) {
        locallabPanel->pack_start(*maskingGroup, Gtk::PACK_SHRINK);
        maskingGroup->addHeaderWidget(*locallab->getAddMaskButton());
        addPanel(maskingGroup->getContentBox(), locallab, 1);
        locallab->setFlatMode(true);
        locallab->hideSettingsHeader();
        locallab->hideToolGroups();
    } else {
        modeButtonBar->setModeVisible(EditorMode::MASK, false);
    }

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
    if (batch || !locallab) {
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
        case Tool::TILT_SHIFT:
            return TiltShift::TOOL_NAME;
        case Tool::LENS_BLUR:
            return LensBlur::TOOL_NAME;
        case Tool::FILM_PRESETS:
            return FilmPresets::TOOL_NAME;
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

bool ToolPanelCoordinator::bridgeGlobalToSpot(ProcParams* params, const rtengine::ProcEvent& event)
{
    if (params->locallab.spots.empty()) return false;

    // Never bridge for internal mask visibility events — these aren't user edits.
    using namespace rtengine;
    const int id = event;
    if (id == EvlocallabshowmaskMethod) return false;

    // Bridge when in mask mode OR when any locallab spot has gradient shape
    bool hasGradient = false;
    int gradIdx = -1;
    // Scan all spots for gradient shape regardless of locallab enabled state
    for (int i = 0; i < (int)params->locallab.spots.size(); ++i) {
        if (params->locallab.spots.at(i).shape == "GRAD") {
            hasGradient = true;
            gradIdx = i;
            break;
        }
    }

    if (!maskModeActive_ && !hasGradient) return false;

    // Auto-enable locallab when bridging to a gradient spot
    if (hasGradient && !params->locallab.enabled) {
        params->locallab.enabled = true;
    }

    // Use the gradient spot if available, otherwise use selected spot
    const int idx = hasGradient ? gradIdx : params->locallab.selspot;
    if (idx < 0 || idx >= (int)params->locallab.spots.size()) return false;

    auto& spot = params->locallab.spots.at(idx);

    if (hasGradient) {
        // Gradient mode: copy ALL bridgeable global values to the gradient spot
        // and zero globals. Must ALWAYS return true because globals are zeroed —
        // locallab MUST reprocess to compensate via the gradient.
        spot.expcomp = params->toneCurve.expcomp;
        spot.black = params->toneCurve.black;
        spot.hlcompr = params->toneCurve.hlcompr;
        spot.hlcomprthresh = params->toneCurve.hlcomprthresh;
        spot.shcompr = params->toneCurve.shcompr;
        if (spot.expcomp != 0.0 || spot.black != 0 ||
            spot.hlcompr != 0 || spot.hlcomprthresh != 0 || spot.shcompr != 50) {
            spot.expexpose = true;
            spot.visiexpose = true;
        }

        spot.lightness = params->toneCurve.brightness;
        spot.contrast = params->toneCurve.contrast;
        spot.chroma = params->toneCurve.saturation;
        if (spot.lightness != 0 || spot.contrast != 0 || spot.chroma != 0) {
            spot.expcolor = true;
            spot.visicolor = true;
        }

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

        spot.sharamount = params->sharpening.amount;
        spot.sharradius = params->sharpening.radius;
        spot.sharcontrast = static_cast<int>(params->sharpening.contrast);
        if (spot.sharamount != 0) {
            spot.expsharp = true;
            spot.visisharp = true;
        }

        spot.highlights = params->sh.highlights;
        spot.shadows = params->sh.shadows;
        spot.h_tonalwidth = params->sh.htonalwidth;
        spot.s_tonalwidth = params->sh.stonalwidth;
        if (spot.highlights != 0 || spot.shadows != 0) {
            spot.expshadhigh = true;
            spot.visishadhigh = true;
        }

        // Zero ALL bridgeable global params so the effect only applies through
        // the gradient, not uniformly across the entire image.
        params->toneCurve.expcomp = 0.0;
        params->toneCurve.black = 0;
        params->toneCurve.hlcompr = 0;
        params->toneCurve.hlcomprthresh = 0;
        params->toneCurve.shcompr = 50;
        params->toneCurve.brightness = 0;
        params->toneCurve.contrast = 0;
        params->toneCurve.saturation = 0;
        params->sh.highlights = 0;
        params->sh.shadows = 0;

        return true;
    }

    // Mask mode: event-specific bridging
    bool bridged = false;

    if (id == EvExpComp || id == EvBlack || id == EvHLCompr || id == EvHLComprThreshold || id == EvSHCompr) {
        spot.expcomp = params->toneCurve.expcomp;
        spot.black = params->toneCurve.black;
        spot.hlcompr = params->toneCurve.hlcompr;
        spot.hlcomprthresh = params->toneCurve.hlcomprthresh;
        spot.shcompr = params->toneCurve.shcompr;
        if (spot.expcomp != 0.0 || spot.black != 0 ||
            spot.hlcompr != 0 || spot.hlcomprthresh != 0 || spot.shcompr != 50) {
            spot.expexpose = true;
            spot.visiexpose = true;
        }
        bridged = true;
    }

    if (id == EvBrightness || id == EvContrast || id == EvSaturation) {
        spot.lightness = params->toneCurve.brightness;
        spot.contrast = params->toneCurve.contrast;
        spot.chroma = params->toneCurve.saturation;
        if (spot.lightness != 0 || spot.contrast != 0 || spot.chroma != 0) {
            spot.expcolor = true;
            spot.visicolor = true;
        }
        bridged = true;
    }

    if (id == EvVibrancePastels || id == EvVibranceSaturated) {
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
        bridged = true;
    }

    if (id == EvShrAmount || id == EvShrRadius) {
        spot.sharamount = params->sharpening.amount;
        spot.sharradius = params->sharpening.radius;
        spot.sharcontrast = static_cast<int>(params->sharpening.contrast);
        if (spot.sharamount != 0) {
            spot.expsharp = true;
            spot.visisharp = true;
        }
        bridged = true;
    }

    if (id == EvSHHighlights || id == EvSHShadows) {
        spot.highlights = params->sh.highlights;
        spot.shadows = params->sh.shadows;
        spot.h_tonalwidth = params->sh.htonalwidth;
        spot.s_tonalwidth = params->sh.stonalwidth;
        if (spot.highlights != 0 || spot.shadows != 0) {
            spot.expshadhigh = true;
            spot.visishadhigh = true;
        }
        bridged = true;
    }

    if (id == EvBWChmixEnabled || id == EvBWmethod) {
        // B&W is active when enabled OR when the method is changed to anything
        // other than its default. Use enabled flag from the write() call.
        spot.blwh = params->blackwhite.enabled;
        // B&W desaturation runs inside the color processing block,
        // so the color tool must be enabled for the engine to reach it.
        if (spot.blwh) {
            spot.expcolor = true;
        }
        bridged = true;
    }

    if (maskModeActive_) {
        params->toneCurve = savedToneCurve_;
        params->vibrance = savedVibrance_;
        params->sharpening = savedSharpening_;
        params->sh = savedSH_;
        params->blackwhite = savedBlackWhite_;
    }

    return bridged;
}

void ToolPanelCoordinator::loadSpotIntoGlobalTools()
{
    if (!ipc || !locallab) return;

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

    tempParams.blackwhite.enabled = spot.blwh;

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

    blackwhite->disableListener();
    blackwhite->read(&tempParams);
    blackwhite->enableListener();
}

void ToolPanelCoordinator::modeChanged(EditorMode mode)
{
    // Direction-aware slide transition
    bool goingRight = static_cast<int>(mode) > static_cast<int>(prevMode);
    modeStack->set_transition_type(
        goingRight ? Gtk::STACK_TRANSITION_TYPE_SLIDE_LEFT
                   : Gtk::STACK_TRANSITION_TYPE_SLIDE_RIGHT);

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
            // Auto-expand masking group (the primary tool on this pane)
            maskingGroup->setExpanded(true);
            break;
    }

    // Deselect active perspective/crop tools when switching away from CROPPING tab
    if (mode != EditorMode::CROPPING && toolBar) {
        ToolMode cur = toolBar->getTool();
        if (cur == TMPerspective || cur == TMPerspectiveGrid ||
            cur == TMCropSelect || cur == TMStraighten) {
            toolDeselected(cur);
            toolBar->setTool(TMHand);
        }
    }

    // Crop preview mode: show full image when on crop tab, cropped view otherwise
    if (imageArea_) {
        imageArea_->setCropPreviewMode(mode == EditorMode::CROPPING);
    }

    // Reparent global ToolGroups between editPanel and locallabPanel
    if (mode == EditorMode::MASK && prevMode != EditorMode::MASK) {
        ++editGroupRestoreGeneration_;
        editGroupCollapseConn_.disconnect();
        editPanel->remove(*lightGroup);
        editPanel->remove(*colorGroup);
        editPanel->remove(*detailGroup);
        editPanel->remove(*bwGroup);
        editPanel->remove(*effectsGroup);
        editPanel->remove(*calibrationGroup);
        locallabPanel->pack_start(*lightGroup, Gtk::PACK_SHRINK);
        locallabPanel->pack_start(*bwGroup, Gtk::PACK_SHRINK);
        locallabPanel->pack_start(*colorGroup, Gtk::PACK_SHRINK);
        locallabPanel->pack_start(*detailGroup, Gtk::PACK_SHRINK);
        locallabPanel->pack_start(*effectsGroup, Gtk::PACK_SHRINK);
        locallabPanel->pack_start(*calibrationGroup, Gtk::PACK_SHRINK);
        // Start all reparented groups collapsed on the Mask pane
        lightGroup->setExpanded(false);
        bwGroup->setExpanded(false);
        colorGroup->setExpanded(false);
        detailGroup->setExpanded(false);
        effectsGroup->setExpanded(false);
        calibrationGroup->setExpanded(false);
    }
    if (prevMode == EditorMode::MASK && mode != EditorMode::MASK) {
        locallabPanel->remove(*lightGroup);
        locallabPanel->remove(*colorGroup);
        locallabPanel->remove(*detailGroup);
        locallabPanel->remove(*bwGroup);
        locallabPanel->remove(*effectsGroup);
        locallabPanel->remove(*calibrationGroup);
        editPanel->pack_start(*lightGroup, Gtk::PACK_SHRINK);
        editPanel->pack_start(*bwGroup, Gtk::PACK_SHRINK);
        editPanel->pack_start(*colorGroup, Gtk::PACK_SHRINK);
        editPanel->pack_start(*detailGroup, Gtk::PACK_SHRINK);
        editPanel->pack_start(*effectsGroup, Gtk::PACK_SHRINK);
        editPanel->pack_start(*calibrationGroup, Gtk::PACK_SHRINK);
        if (quickEditBar_) {
            editPanel->reorder_child(*quickEditBar_, 0);
        }
    }

    // Handle Locallab subscription/unsubscription
    if (photoLoadedOnce && locallab) {
        if (mode == EditorMode::MASK) {
            toolBar->blockEditDeactivation();
            locallab->subscribe();

            // Auto-enable locallab when entering mask mode (required for overlay rendering)
            if (!locallab->getEnabled()) {
                locallab->setEnabled(true);
                locallab->enabledChanged();
            }

            // Save global params BEFORE any panelChanged call, which could
            // zero them via bridgeGlobalToSpot when a gradient spot exists.
            if (ipc) {
                ProcParams* p = ipc->beginUpdateParams();
                savedToneCurve_ = p->toneCurve;
                savedVibrance_ = p->vibrance;
                savedSharpening_ = p->sharpening;
                savedSH_ = p->sh;
                savedBlackWhite_ = p->blackwhite;
                ipc->endUpdateParams(0);
            }
            maskModeActive_ = true;

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
                p->blackwhite = savedBlackWhite_;
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
                blackwhite->disableListener();
                blackwhite->read(&restored);
                blackwhite->enableListener();
            }

            toolBar->blockEditDeactivation(false);
            locallab->unsubscribe();
            // Clear mask overlay when leaving mask mode (all zeros = no mask shown).
            // Skip locallab tool writes so the stale locallab tool widgets don't
            // overwrite bridged spot values (expcomp, lightness, etc.).
            if (ipc) {
                ipc->setLocallabMaskVisibility(false, false,
                    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
                locallab->setSkipToolWrites(true);
                panelChanged(rtengine::EvlocallabshowmaskMethod, "");
                locallab->setSkipToolWrites(false);
            }
        }
    }

    // Reparenting and the mask-overlay refresh both schedule GTK layout work.
    // Always return to a clean collapsed Edit pane, then enforce that state on
    // idle so a stack/map transition cannot reveal every group behind us.
    if (prevMode == EditorMode::MASK && mode != EditorMode::MASK) {
        const unsigned generation = ++editGroupRestoreGeneration_;
        auto collapseEditGroups = [this]() {
            ToolGroup* groups[] = {
                lightGroup, bwGroup, colorGroup, detailGroup, effectsGroup, calibrationGroup
            };
            for (auto* group : groups) {
                group->setExpanded(false);
            }
        };
        collapseEditGroups();
        idle_register.add([this, generation]() -> bool {
            if (generation == editGroupRestoreGeneration_ && prevMode != EditorMode::MASK) {
                ToolGroup* groups[] = {
                    lightGroup, bwGroup, colorGroup, detailGroup, effectsGroup, calibrationGroup
                };
                for (auto* group : groups) {
                    group->setExpanded(false);
                }
            }
            return false;
        });
        editGroupCollapseConn_.disconnect();
        editGroupCollapseConn_ = Glib::signal_timeout().connect([this, generation]() -> bool {
            if (generation == editGroupRestoreGeneration_ && prevMode != EditorMode::MASK) {
                ToolGroup* groups[] = {
                    lightGroup, bwGroup, colorGroup, detailGroup, effectsGroup, calibrationGroup
                };
                for (auto* group : groups) {
                    group->setExpanded(false);
                }
            }
            return false;
        }, 400);
    }

    prevMode = mode;
}

void ToolPanelCoordinator::populateEditPanel()
{
    buildQuickEditBar();

    // --- Exposure preview strip ---
    exposureStrip_ = Gtk::manage(new PreviewStrip());
    exposureStrip_->setParamModifier([](rtengine::procparams::ProcParams& pp, double t) {
        if (std::abs(t) < 0.001) return;
        double absT = std::min(std::abs(t), 1.0);
        double f = (1.0 - std::cos(absT * M_PI)) / 2.0;

        // Always modify global params — the bridge handles routing to gradient spots.
        pp.sh.enabled = true;
        if (t < 0) {
            pp.toneCurve.expcomp -= 1.35 * f;
            pp.toneCurve.brightness -= static_cast<int>(std::lround(18 * f));
            pp.toneCurve.contrast += static_cast<int>(std::lround(12 * f));
            pp.toneCurve.black += static_cast<int>(std::lround(260 * f));
            pp.toneCurve.hlcompr += static_cast<int>(std::lround(100 * f));
            pp.sh.shadows = std::min(pp.sh.shadows + static_cast<int>(std::lround(24 * f)), 100);
            pp.sh.highlights = std::min(pp.sh.highlights + static_cast<int>(std::lround(35 * f)), 100);
        } else {
            pp.toneCurve.expcomp += 1.0 * f;
            pp.toneCurve.brightness += static_cast<int>(std::lround(12 * f));
            pp.toneCurve.contrast += static_cast<int>(std::lround(8 * f));
            pp.toneCurve.black -= static_cast<int>(std::lround(220 * f));
            pp.toneCurve.hlcompr += static_cast<int>(std::lround(140 * f));
            pp.sh.shadows = std::min(pp.sh.shadows + static_cast<int>(std::lround(18 * f)), 100);
            pp.sh.highlights = std::min(pp.sh.highlights + static_cast<int>(std::lround(45 * f)), 100);
        }
        pp.toneCurve.brightness = std::max(-100, std::min(100, pp.toneCurve.brightness));
        pp.toneCurve.contrast = std::max(-100, std::min(100, pp.toneCurve.contrast));
        pp.toneCurve.black = std::max(-16384, std::min(16384, pp.toneCurve.black));
        pp.toneCurve.hlcompr = std::max(0, std::min(500, pp.toneCurve.hlcompr));
    });
    exposureStrip_->setDragCallback([this](const rtengine::procparams::ProcParams& pp, double) {
        // Always set global sliders — the bridge handles routing to gradient spots.
        toneCurve->disableListener();
        toneCurve->getExpcompSlider()->setValue(pp.toneCurve.expcomp);
        toneCurve->getBrightnessSlider()->setValue(pp.toneCurve.brightness);
        toneCurve->getContrastSlider()->setValue(pp.toneCurve.contrast);
        toneCurve->getBlackSlider()->setValue(pp.toneCurve.black * 100.0 / 16384.0);
        toneCurve->getHlcomprSlider()->setValue(-pp.toneCurve.hlcompr / 5.0);
        toneCurve->enableListener();
        shadowshighlights->disableListener();
        shadowshighlights->setEnabled(pp.sh.enabled);
        shadowshighlights->getHighlightsSlider()->setValue(pp.sh.highlights);
        shadowshighlights->getShadowsSlider()->setValue(pp.sh.shadows);
        shadowshighlights->enableListener();
        suppressResetUpdate_ = true;
        panelChangedFromPreviewStrip(exposureStrip_, rtengine::EvExpComp, M("GENERAL_CHANGED"));
        suppressResetUpdate_ = false;
        lightGroup->setResetVisible(true);
    });
    lightGroup->getPersistentBox()->pack_start(*exposureStrip_, Gtk::PACK_SHRINK);

    addPanel(lightGroup->getContentBox(), toneCurve, 1);
    toneCurve->setFlatMode(true);
    toneCurve->collapseDetail();
    addPanel(lightGroup->getContentBox(), shadowshighlights, 1);
    shadowshighlights->setFlatMode(true);
    shadowshighlights->collapseDetail();
    addPanel(lightGroup->getContentBox(), rgbcurves, 1);
    rgbcurves->setFlatMode(true);
    // Scale curves down ~30% by adding horizontal margins
    rgbcurves->getExpander()->set_margin_start(8);
    rgbcurves->getExpander()->set_margin_end(8);

    // --- Color preview strip ---
    colorStrip_ = Gtk::manage(new PreviewStrip());
    colorStrip_->setParamModifier([](rtengine::procparams::ProcParams& pp, double t) {

        if (std::abs(t) < 0.001) return;
        double absT = std::min(std::abs(t), 1.0);
        double f = (1.0 - std::cos(absT * M_PI)) / 2.0;
        pp.vibrance.enabled = true;
        if (t < 0) {
            // Cool/desaturated: lower temperature, reduce vibrance + saturation, shift tint
            pp.wb.temperature = std::max(1500, static_cast<int>(std::lround(pp.wb.temperature * (1.0 - 0.20 * f))));
            pp.wb.green += 0.015 * f;
            pp.vibrance.pastels = std::max(-100, pp.vibrance.pastels - static_cast<int>(std::lround(30 * f)));
            pp.vibrance.saturated = std::max(-100, pp.vibrance.saturated - static_cast<int>(std::lround(18 * f)));
            pp.toneCurve.saturation = std::max(-100, pp.toneCurve.saturation - static_cast<int>(std::lround(14 * f)));
        } else {
            // Warm/vibrant: higher temperature, boost vibrance + saturation, warm tint
            pp.wb.temperature = std::min(25000, static_cast<int>(std::lround(pp.wb.temperature * (1.0 + 0.22 * f))));
            pp.wb.green -= 0.012 * f;
            pp.vibrance.pastels = std::min(100, pp.vibrance.pastels + static_cast<int>(std::lround(35 * f)));
            pp.vibrance.saturated = std::min(100, pp.vibrance.saturated + static_cast<int>(std::lround(20 * f)));
            pp.toneCurve.saturation = std::min(100, pp.toneCurve.saturation + static_cast<int>(std::lround(16 * f)));
        }
    });
    colorStrip_->setDragCallback([this](const rtengine::procparams::ProcParams& pp, double) {
        whitebalance->disableListener();
        whitebalance->getTempSlider()->setValue(pp.wb.temperature);
        whitebalance->getGreenSlider()->setValue(pp.wb.green);
        whitebalance->enableListener();
        vibrance->disableListener();
        vibrance->setEnabled(pp.vibrance.enabled);
        vibrance->getVibranceSlider()->setValue(pp.vibrance.pastels);
        vibrance->getSaturationSlider()->setValue(pp.vibrance.saturated);
        vibrance->enableListener();
        toneCurve->disableListener();
        toneCurve->getSaturationSlider()->setValue(pp.toneCurve.saturation);
        toneCurve->enableListener();
        suppressResetUpdate_ = true;
        panelChangedFromPreviewStrip(colorStrip_, rtengine::EvExpComp, M("GENERAL_CHANGED"));
        suppressResetUpdate_ = false;
        colorGroup->setResetVisible(true);
    });
    colorGroup->getPersistentBox()->pack_start(*colorStrip_, Gtk::PACK_SHRINK);

    // Color group: WB (with Vibrance+Saturation under Tint), HSV Eq, Color Grading, Point Color
    addPanel(colorGroup->getContentBox(), whitebalance, 1);
    whitebalance->setFlatMode(true);
    whitebalance->collapseDetail();

    // Move Vibrance and Saturation sliders from Vibrance tool into WB summary (after Tint)
    {
        Adjuster* vibSlider = vibrance->getVibranceSlider();
        Adjuster* satSlider = vibrance->getSaturationSlider();
        vibrance->getSummaryBox()->remove(*vibSlider);
        vibrance->getSummaryBox()->remove(*satSlider);
        whitebalance->getSummaryBox()->pack_start(*vibSlider, Gtk::PACK_SHRINK, 0);
        whitebalance->getSummaryBox()->pack_start(*satSlider, Gtk::PACK_SHRINK, 0);
    }

    addPanel(colorGroup->getContentBox(), vibrance, 1);
    vibrance->setFlatMode(true);
    vibrance->collapseDetail();
    // --- Color tool pagination (orb dots + stack) ---
    colorDotBlock_ = false;
    colorDotActive_ = 0;

    // Dot navigation bar (centered horizontal box)
    auto* dotBar = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 0));
    dotBar->set_halign(Gtk::ALIGN_CENTER);
    dotBar->set_margin_top(0);
    dotBar->set_margin_bottom(2);
    dotBar->set_name("PaginationBar");

    const char* tooltips[] = {"Color Mixer", "Grading", "Point Color"};
    const char* icons[] = {"page-colormixer", "page-grading", "page-pointcolor"};
    for (int i = 0; i < 3; i++) {
        colorDots_[i] = Gtk::manage(new Gtk::ToggleButton());
        colorDots_[i]->set_image(*Gtk::manage(new RTImage(icons[i], Gtk::ICON_SIZE_MENU)));
        colorDots_[i]->set_always_show_image(true);
        colorDots_[i]->get_style_context()->add_class("PaginationDot");
        colorDots_[i]->set_tooltip_text(tooltips[i]);
        colorDots_[i]->signal_toggled().connect([this, i]() {
            if (colorDotBlock_) return;
            if (colorDots_[i]->get_active()) {
                colorDotBlock_ = true;
                for (int j = 0; j < 3; j++) {
                    if (j != i) colorDots_[j]->set_active(false);
                }
                colorToolStack_->set_transition_type(
                    i > colorDotActive_
                        ? Gtk::STACK_TRANSITION_TYPE_SLIDE_LEFT
                        : Gtk::STACK_TRANSITION_TYPE_SLIDE_RIGHT);
                colorDotActive_ = i;
                const char* names[] = {"mixer", "grading", "pointcolor"};
                colorToolStack_->set_visible_child(names[i]);
                colorDotBlock_ = false;
            } else {
                // Don't allow deactivating active dot
                colorDotBlock_ = true;
                colorDots_[i]->set_active(true);
                colorDotBlock_ = false;
            }
        });
        dotBar->pack_start(*colorDots_[i], Gtk::PACK_SHRINK, 3);
    }

    // Activate first dot
    colorDotBlock_ = true;
    colorDots_[0]->set_active(true);
    colorDotBlock_ = false;

    colorGroup->getContentBox()->pack_start(*dotBar, Gtk::PACK_SHRINK);

    // Stack for the three tool pages
    colorToolStack_ = Gtk::manage(new Gtk::Stack());
    colorToolStack_->set_name("ColorToolStack");
    colorToolStack_->set_transition_type(Gtk::STACK_TRANSITION_TYPE_SLIDE_LEFT);
    colorToolStack_->set_transition_duration(200);
    colorToolStack_->set_hhomogeneous(false);
    colorToolStack_->set_vhomogeneous(false);

    // Add each tool as a stack page
    hsvequalizer->setFlatMode(true);
    hsvequalizer->collapseDetail();
    colorToolStack_->add(*hsvequalizer->getExpander(), "mixer");

    colorgrading->setFlatMode(true);
    colorgrading->collapseDetail();
    colorToolStack_->add(*colorgrading->getExpander(), "grading");

    pointcolor->setFlatMode(true);
    colorToolStack_->add(*pointcolor->getExpander(), "pointcolor");

    colorGroup->getContentBox()->pack_start(*colorToolStack_, Gtk::PACK_SHRINK);

    // Hide section labels and force content visible
    // (the pagination dots replace the clickable "▸ Label" headers)
    auto prepareToolPage = [](FoldableToolPanel* tool) {
        auto* box = tool->getSummaryBox();
        if (!box) return;
        auto children = box->get_children();
        // First child is the EventBox wrapping the section label — permanently hide it
        if (!children.empty()) {
            children[0]->set_no_show_all(true);
            children[0]->hide();
        }
        // Show all remaining children (toolContent_, advancedSection, etc.)
        for (size_t i = 1; i < children.size(); i++) {
            children[i]->set_no_show_all(false);
            children[i]->show_all();
        }
    };
    prepareToolPage(hsvequalizer);
    prepareToolPage(colorgrading);
    prepareToolPage(pointcolor);

    // --- Detail preview strip ---
    // Modifies sharpening, noise reduction (luma/chroma), and dehaze
    detailStrip_ = Gtk::manage(new PreviewStrip());
    detailStrip_->setParamModifier([](rtengine::procparams::ProcParams& pp, double t) {

        if (std::abs(t) < 0.001) return;
        double absT = std::min(std::abs(t), 1.0);
        double f = (1.0 - std::cos(absT * M_PI)) / 2.0;
        if (t < 0) {
            // Soft/smooth: reduce sharpening, boost denoise, reduce dehaze
            pp.sharpening.amount = std::max(0, pp.sharpening.amount - static_cast<int>(std::lround(120 * f)));
            pp.dirpyrDenoise.luma = std::min(100.0, pp.dirpyrDenoise.luma + 30.0 * f);
            pp.dirpyrDenoise.chroma = std::min(100.0, pp.dirpyrDenoise.chroma + 25.0 * f);
            pp.dirpyrDenoise.enabled = true;
            pp.dehaze.strength = std::max(0, pp.dehaze.strength - static_cast<int>(std::lround(15 * f)));
        } else {
            // Crisp/detailed: boost sharpening and dehaze
            pp.sharpening.amount = std::min(1000, pp.sharpening.amount + static_cast<int>(std::lround(180 * f)));
            pp.sharpening.enabled = true;
            pp.dehaze.strength = std::min(100, pp.dehaze.strength + static_cast<int>(std::lround(20 * f)));
            pp.dehaze.enabled = true;
        }
    });
    detailStrip_->setDragCallback([this](const rtengine::procparams::ProcParams& pp, double) {
        sharpening->disableListener();
        sharpening->setEnabled(pp.sharpening.enabled);
        sharpening->getAmountSlider()->setValue(pp.sharpening.amount);
        sharpening->enableListener();
        dirpyrdenoise->disableListener();
        dirpyrdenoise->setEnabled(pp.dirpyrDenoise.enabled);
        dirpyrdenoise->getLumaSlider()->setValue(pp.dirpyrDenoise.luma);
        dirpyrdenoise->getChromaSlider()->setValue(pp.dirpyrDenoise.chroma);
        dirpyrdenoise->enableListener();
        dehaze->disableListener();
        dehaze->setEnabled(pp.dehaze.enabled);
        dehaze->getStrengthSlider()->setValue(pp.dehaze.strength);
        dehaze->enableListener();
        suppressResetUpdate_ = true;
        panelChangedFromPreviewStrip(detailStrip_, rtengine::EvExpComp, M("GENERAL_CHANGED"));
        suppressResetUpdate_ = false;
        detailGroup->setResetVisible(true);
    });
    detailGroup->getPersistentBox()->pack_start(*detailStrip_, Gtk::PACK_SHRINK);

    // Detail group: Sharpening, Local Contrast, Noise Reduction, AI Denoise, Dehaze
    addPanel(detailGroup->getContentBox(), sharpening, 1);
    sharpening->setFlatMode(true);
    sharpening->collapseDetail();
    addPanel(detailGroup->getContentBox(), dirpyrdenoise, 1);
    dirpyrdenoise->setFlatMode(true);
    dirpyrdenoise->collapseDetail();
    addPanel(detailGroup->getContentBox(), dehaze, 1);
    dehaze->setFlatMode(true);
    dehaze->collapseDetail();
    addPanel(detailGroup->getContentBox(), aidenoise, 1);
    aidenoise->setFlatMode(true);
    aidenoise->collapseDetail();

    // --- Effects preview strip ---
    effectsStrip_ = Gtk::manage(new PreviewStrip());
    effectsStrip_->setParamModifier([](rtengine::procparams::ProcParams& pp, double t) {

        if (std::abs(t) < 0.001) return;
        double absT = std::min(std::abs(t), 1.0);
        double f = (1.0 - std::cos(absT * M_PI)) / 2.0;
        pp.texture.enabled = true;
        pp.clarity.enabled = true;
        if (t < 0) {
            // Gentle/clean: soften fine texture and midtone bite while reducing existing artifacts.
            pp.texture.amount = std::max(-100.0, pp.texture.amount - 28.0 * f);
            pp.clarity.amount = std::max(-100.0, pp.clarity.amount - 24.0 * f);
            pp.grain.strength = std::max(0, pp.grain.strength - static_cast<int>(std::lround(30 * f)));
            pp.pcvignette.strength *= 1.0 - 0.70 * f;
        } else {
            // Textured/cinematic: restrained texture, grain, clarity, and edge falloff.
            pp.texture.amount = std::min(100.0, pp.texture.amount + 22.0 * f);
            pp.clarity.amount = std::min(100.0, pp.clarity.amount + 26.0 * f);
            pp.grain.strength = std::min(100, pp.grain.strength + static_cast<int>(std::lround(35 * f)));
            pp.grain.enabled = true;
            pp.pcvignette.strength = std::min(6.0, pp.pcvignette.strength + 1.8 * f);
            pp.pcvignette.enabled = true;
        }
    });
    effectsStrip_->setDragCallback([this](const rtengine::procparams::ProcParams& pp, double) {
        texture->disableListener();
        texture->setEnabled(pp.texture.enabled);
        texture->getAmountSlider()->setValue(pp.texture.amount);
        texture->enableListener();
        grain->disableListener();
        grain->setEnabled(pp.grain.enabled);
        grain->getStrengthSlider()->setValue(pp.grain.strength);
        grain->enableListener();
        pcvignette->disableListener();
        pcvignette->setEnabled(pp.pcvignette.enabled);
        pcvignette->getStrengthSlider()->setValue(pp.pcvignette.strength);
        pcvignette->enableListener();
        clarity->disableListener();
        clarity->setEnabled(pp.clarity.enabled);
        clarity->getAmountSlider()->setValue(pp.clarity.amount);
        clarity->enableListener();
        suppressResetUpdate_ = true;
        panelChangedFromPreviewStrip(effectsStrip_, rtengine::EvExpComp, M("GENERAL_CHANGED"));
        suppressResetUpdate_ = false;
        effectsGroup->setResetVisible(true);
    });
    effectsGroup->getPersistentBox()->pack_start(*effectsStrip_, Gtk::PACK_SHRINK);

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
    addPanel(effectsGroup->getContentBox(), tiltshift, 1);
    tiltshift->setFlatMode(true);
    tiltshift->collapseDetail();
    addPanel(effectsGroup->getContentBox(), filmPresets, 1);
    filmPresets->setFlatMode(true);
    filmPresets->collapseDetail();

    // --- B&W preview strip (in advanced group, before blackwhite tool) ---
    bwStrip_ = Gtk::manage(new PreviewStrip());
    bwStrip_->setParamModifier([](rtengine::procparams::ProcParams& pp, double t) {
        if (std::abs(t) < 0.001) return;
        double absT = std::min(std::abs(t), 1.0);
        double f = (1.0 - std::cos(absT * M_PI)) / 2.0;
        pp.blackwhite.enabled = true;
        if (pp.blackwhite.method.empty() || pp.blackwhite.method == "Disabled") {
            pp.blackwhite.method = "Desaturation";
        }
        if (t < 0) {
            // Subtle: increased neutrals, reduced strength
            pp.blackwhite.method = "Desaturation";
            pp.blackwhite.filter = "None";
            pp.blackwhite.neutrals = std::min(100, pp.blackwhite.neutrals + static_cast<int>(20 * f));
            pp.blackwhite.strength = std::max(0, pp.blackwhite.strength - static_cast<int>(30 * f));
        } else {
            // Dramatic: channel mixer with orange filter, warm tone
            if (f > 0.3) {
                pp.blackwhite.method = "ChannelMixer";
                pp.blackwhite.filter = "Orange";
            }
            pp.blackwhite.neutrals = std::max(-100, pp.blackwhite.neutrals - static_cast<int>(15 * f));
            pp.blackwhite.tone = std::min(100, pp.blackwhite.tone + static_cast<int>(30 * f));
        }
    });
    bwStrip_->setDragCallback([this](const rtengine::procparams::ProcParams& pp, double) {
        blackwhite->disableListener();
        blackwhite->read(&pp);
        blackwhite->enableListener();
        suppressResetUpdate_ = true;
        panelChangedFromPreviewStrip(bwStrip_, rtengine::EvBWmethod, M("GENERAL_CHANGED"));
        suppressResetUpdate_ = false;
        bwGroup->setResetVisible(true);
    });

    // B&W group: dedicated Black & White section
    bwGroup->getPersistentBox()->pack_start(*bwStrip_, Gtk::PACK_SHRINK);
    addPanel(bwGroup->getContentBox(), blackwhite, 1);
    blackwhite->setFlatMode(true);

    // Advanced group: Niche/legacy tools (hidden from main panel)
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

    // --- Reset callbacks for each group ---
    lightGroup->setResetCallback([this]() {
        ProcParams dp;
        toneCurve->disableListener();
        toneCurve->read(&dp);
        toneCurve->enableListener();
        shadowshighlights->disableListener();
        shadowshighlights->read(&dp);
        shadowshighlights->enableListener();
        rgbcurves->disableListener();
        rgbcurves->read(&dp);
        rgbcurves->enableListener();
        if (exposureStrip_) exposureStrip_->resetScrubber();
        suppressResetUpdate_ = true;
        panelChanged(rtengine::EvProfileChanged, M("GENERAL_CHANGED"));
        suppressResetUpdate_ = false;
        lightGroup->setResetVisible(false);
    });

    bwGroup->setResetCallback([this]() {
        ProcParams dp;
        blackwhite->disableListener();
        blackwhite->read(&dp);
        blackwhite->enableListener();
        if (bwStrip_) bwStrip_->resetScrubber();
        suppressResetUpdate_ = true;
        panelChanged(rtengine::EvProfileChanged, M("GENERAL_CHANGED"));
        suppressResetUpdate_ = false;
        bwGroup->setResetVisible(false);
    });

    colorGroup->setResetCallback([this]() {
        ProcParams dp;
        whitebalance->disableListener();
        whitebalance->read(&dp);
        whitebalance->enableListener();
        vibrance->disableListener();
        vibrance->read(&dp);
        vibrance->enableListener();
        hsvequalizer->disableListener();
        hsvequalizer->read(&dp);
        hsvequalizer->enableListener();
        colorgrading->disableListener();
        colorgrading->read(&dp);
        colorgrading->enableListener();
        pointcolor->disableListener();
        pointcolor->read(&dp);
        pointcolor->enableListener();
        if (colorStrip_) colorStrip_->resetScrubber();
        suppressResetUpdate_ = true;
        panelChanged(rtengine::EvProfileChanged, M("GENERAL_CHANGED"));
        suppressResetUpdate_ = false;
        colorGroup->setResetVisible(false);
    });

    detailGroup->setResetCallback([this]() {
        ProcParams dp;
        // Reset the tools that the detail strip modifies
        sharpening->disableListener();
        sharpening->read(&dp);
        sharpening->enableListener();
        dirpyrdenoise->disableListener();
        dirpyrdenoise->read(&dp);
        dirpyrdenoise->enableListener();
        dehaze->disableListener();
        dehaze->read(&dp);
        dehaze->enableListener();
        if (detailStrip_) detailStrip_->resetScrubber();
        suppressResetUpdate_ = true;
        panelChanged(rtengine::EvProfileChanged, M("GENERAL_CHANGED"));
        suppressResetUpdate_ = false;
        detailGroup->setResetVisible(false);
    });

    effectsGroup->setResetCallback([this]() {
        ProcParams dp;
        texture->disableListener();
        texture->read(&dp);
        texture->enableListener();
        clarity->disableListener();
        clarity->read(&dp);
        clarity->enableListener();
        grain->disableListener();
        grain->read(&dp);
        grain->enableListener();
        tiltshift->disableListener();
        tiltshift->read(&dp);
        tiltshift->enableListener();
        pcvignette->disableListener();
        pcvignette->read(&dp);
        pcvignette->enableListener();
        filmPresets->disableListener();
        filmPresets->read(&dp);
        filmPresets->enableListener();
        softlight->disableListener();
        softlight->read(&dp);
        softlight->enableListener();
        if (effectsStrip_) effectsStrip_->resetScrubber();
        suppressResetUpdate_ = true;
        panelChanged(rtengine::EvProfileChanged, M("GENERAL_CHANGED"));
        suppressResetUpdate_ = false;
        effectsGroup->setResetVisible(false);
    });

    spotGroup->setResetCallback([this]() {
        ProcParams dp;
        spot->disableListener();
        spot->read(&dp);
        spot->enableListener();
        suppressResetUpdate_ = true;
        panelChanged(rtengine::EvProfileChanged, M("GENERAL_CHANGED"));
        suppressResetUpdate_ = false;
        spotGroup->setResetVisible(false);
    });

    maskingGroup->setResetCallback([this]() {
        if (!ipc || !locallab) return;

        // Unsubscribe geometry editing before modifying spots
        locallab->unsubscribe();

        // Write default locallab params directly into the processing params,
        // bypassing locallab->write() which crashes when the control panel
        // has been cleared but old params still contain spots.
        ProcParams* params = ipc->beginUpdateParams();
        ProcParams dp;
        params->locallab = dp.locallab;
        ipc->endUpdateParams(rtengine::RefreshMapper::getInstance()->getAction(
            rtengine::EvProfileChanged));

        // Now safely re-read locallab UI from the cleared params
        locallab->disableListener();
        locallab->read(&dp);
        locallab->enableListener();

        // Re-subscribe if in mask mode
        if (maskModeActive_) {
            locallab->subscribe();
        }

        maskingGroup->setResetVisible(false);
    });

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
    quickAutoEditGeneration_->fetch_add(1, std::memory_order_acq_rel);
    quickAutoEditPool_.reset();
    quickPreviewFinalizeConn_.disconnect();
    deferredPanelChangePending_ = false;
    deferredPanelChangeTimerActive_ = false;
    deferredPanelChangeConn_.disconnect();
    previewStripChangeSource_ = nullptr;
    deferredPreviewStripChangeSource_ = nullptr;
    idle_register.destroy();

    // Gtk-managed tools can already be gone when their parent destroys us.
    // The editor disconnects them while the widget tree is still intact.
    ipc = nullptr;

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

    ProcParams* params = ipc->tryBeginUpdateParams();
    if (!params) {
        deferPanelChanged(event, "");
        return;
    }

    for (auto toolPanel : toolPanels) {
        toolPanel->write (params);
    }

    ipc->endUpdateParams (event);   // starts the IPC processing
}

void ToolPanelCoordinator::turnOffMaskOverlay(bool /*forceRedraw*/)
{
    if (!ipc || !locallab) return;

    hoverMaskApplied_ = false;
    pendingHoverState_ = false;
    hoverMaskDebounce_.disconnect();
    hoverMaskWatchdog_.disconnect();

    locallab->setHoverMaskOverlay(false);
    ipc->setLocallabMaskVisibility(false, false,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);

    // Write current GUI state to params before triggering reprocess,
    // otherwise uncommitted slider/adjuster values can be lost.
    ProcParams* params = ipc->tryBeginUpdateParams();
    if (!params) {
        deferPanelChanged(rtengine::EvlocallabshowmaskMethod, "");
        return;
    }

    if (hoverRestoreSpot_ >= 0
            && hoverRestoreSpot_ < static_cast<int>(params->locallab.spots.size())) {
        params->locallab.selspot = hoverRestoreSpot_;
    }

    if (maskModeActive_) {
        locallab->setSkipToolWrites(true);
    }
    for (auto toolPanel : toolPanels) {
        toolPanel->write(params);
    }
    if (maskModeActive_) {
        locallab->setSkipToolWrites(false);
        // In mask mode, global tool widgets show spot values.
        // Restore the real globals so the engine processes correctly.
        params->toneCurve = savedToneCurve_;
        params->vibrance = savedVibrance_;
        params->sharpening = savedSharpening_;
        params->sh = savedSH_;
        params->blackwhite = savedBlackWhite_;
    }
    ipc->endUpdateParams(AUTOEXP);
    hoverPreviewSpot_ = -1;
    hoverRestoreSpot_ = -1;
}

void ToolPanelCoordinator::hoverMaskChanged(bool hover, bool forceRedraw, int spotIndex)
{
    if (!ipc || !locallab) return;

    // Cancel any pending debounce
    hoverMaskDebounce_.disconnect();
    pendingHoverState_ = hover;

    if (!hover) {
        // Turn off immediately — never debounce "off"
        turnOffMaskOverlay(forceRedraw);
        return;
    }

    if (hoverMaskApplied_ && hoverPreviewSpot_ == spotIndex) return;

    // A row-to-row hover keeps the overlay active, but the previous row's
    // pointer watchdog must not survive after its connection is replaced.
    hoverMaskWatchdog_.disconnect();

    // Debounce "on" to avoid expensive reprocessing on rapid mouse movement
    hoverMaskDebounce_ = Glib::signal_timeout().connect([this, spotIndex]() {
        if (!ipc || !pendingHoverState_) return false;

        locallab->setHoverMaskOverlay(true);
        const Locallab::llMaskVisibility mv = locallab->getMaskVisibility();
        ipc->setLocallabMaskVisibility(mv.previewDeltaE, mv.showMaskOverlay,
            mv.colorMask, mv.colorMaskinv, mv.expMask, mv.expMaskinv,
            mv.SHMask, mv.SHMaskinv, mv.vibMask, mv.softMask,
            mv.blMask, mv.tmMask, mv.retiMask, mv.sharMask,
            mv.lcMask, mv.cbMask, mv.logMask, mv.maskMask, mv.cieMask);
        // Write current GUI state to params before triggering reprocess,
        // otherwise uncommitted slider/adjuster values can be lost.
        ProcParams* params = ipc->tryBeginUpdateParams();
        if (!params) {
            hoverMaskApplied_ = false;
            deferPanelChanged(rtengine::EvlocallabshowmaskMethod, "");
            return false;
        }

        if (hoverRestoreSpot_ < 0) {
            hoverRestoreSpot_ = params->locallab.selspot;
        } else if (hoverRestoreSpot_ < static_cast<int>(params->locallab.spots.size())) {
            params->locallab.selspot = hoverRestoreSpot_;
        }

        if (maskModeActive_) {
            locallab->setSkipToolWrites(true);
        }
        for (auto toolPanel : toolPanels) {
            toolPanel->write(params);
        }
        if (maskModeActive_) {
            locallab->setSkipToolWrites(false);
            // Restore saved globals — write() put spot values into them
            params->toneCurve = savedToneCurve_;
            params->vibrance = savedVibrance_;
            params->sharpening = savedSharpening_;
            params->sh = savedSH_;
            params->blackwhite = savedBlackWhite_;
        }
        if (spotIndex >= 0 && spotIndex < static_cast<int>(params->locallab.spots.size())) {
            params->locallab.selspot = spotIndex;
            hoverPreviewSpot_ = spotIndex;
        }
        hoverMaskApplied_ = true;
        ipc->endUpdateParams(AUTOEXP);

        // Start watchdog: periodically check actual pointer position
        // This catches cases where leave events are missed (common on WSLg/X11)
        hoverMissCount_ = 0;
        hoverMaskWatchdog_ = Glib::signal_timeout().connect([this]() {
            if (!hoverMaskApplied_) {
                return false;  // already off
            }
            if (!locallab->isPointerOverMaskList()) {
                hoverMissCount_++;
                // Require 3 consecutive misses to avoid false positives
                // (Windows GDK can return stale coords during redraws)
                if (hoverMissCount_ >= 3) {
                    turnOffMaskOverlay();
                    // Reset ControlSpotPanel's hover state so it can re-trigger
                    locallab->resetSidebarHover();
                    return false;  // stop watchdog
                }
            } else {
                hoverMissCount_ = 0;
            }
            return true;  // keep checking
        }, 500);

        return false;  // one-shot debounce
    }, 100);
}

void ToolPanelCoordinator::applyHoverMask()
{
    // Unused — logic inlined into hoverMaskChanged
}

void ToolPanelCoordinator::panelChangedFromPreviewStrip(
    PreviewStrip* source,
    const rtengine::ProcEvent& event,
    const Glib::ustring& descr)
{
    previewStripChangeSource_ = source;
    panelChanged(event, descr);
    previewStripChangeSource_ = nullptr;
}

void ToolPanelCoordinator::deferPanelChanged(const rtengine::ProcEvent& event, const Glib::ustring& descr)
{
    toolPanelEditLog("defer event=%d timerActive=%d descr=%s\n",
        int(event), deferredPanelChangeTimerActive_ ? 1 : 0, descr.c_str());

    deferredPanelChangeEvent_ = event;
    deferredPanelChangeDescr_ = descr;
    deferredPreviewStripChangeSource_ = previewStripChangeSource_;
    deferredPanelChangePending_ = true;

    if (!deferredPanelChangeTimerActive_) {
        deferredPanelChangeTimerActive_ = true;
        deferredPanelChangeConn_ = Glib::signal_timeout().connect(
            sigc::mem_fun(*this, &ToolPanelCoordinator::retryDeferredPanelChanged),
            25);
    }
}

bool ToolPanelCoordinator::retryDeferredPanelChanged()
{
    if (!deferredPanelChangePending_) {
        toolPanelEditLog("retry-stop no-pending\n");
        deferredPanelChangeTimerActive_ = false;
        return false;
    }

    const rtengine::ProcEvent event = deferredPanelChangeEvent_;
    const Glib::ustring descr = deferredPanelChangeDescr_;
    PreviewStrip* source = deferredPreviewStripChangeSource_;
    deferredPanelChangePending_ = false;
    toolPanelEditLog("retry event=%d descr=%s\n", int(event), descr.c_str());
    previewStripChangeSource_ = source;
    panelChanged(event, descr);
    previewStripChangeSource_ = nullptr;

    if (!deferredPanelChangePending_) {
        deferredPreviewStripChangeSource_ = nullptr;
        deferredPanelChangeTimerActive_ = false;
        return false;
    }

    return true;
}

void ToolPanelCoordinator::buildQuickEditBar()
{
    quickEditBar_ = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 3));
    quickEditBar_->set_name("QuickEditBar");
    quickEditBar_->set_margin_start(4);
    quickEditBar_->set_margin_end(4);
    quickEditBar_->set_margin_top(2);
    quickEditBar_->set_margin_bottom(4);

    // GtkMenu's own pointer tracking can miss items when the menu opened
    // from a hover rather than a click — drive item selection from raw
    // motion so highlight + preview always follow the pointer, and close
    // the menu (ending any hover preview) when the pointer leaves it.
    const auto wireHoverMenuBehavior = [](Gtk::Menu* menu, Gtk::MenuButton* drop) {
        menu->add_events(Gdk::POINTER_MOTION_MASK | Gdk::LEAVE_NOTIFY_MASK);

        menu->signal_motion_notify_event().connect([menu](GdkEventMotion* ev) -> bool {
            if (!ev) {
                return false;
            }
            for (auto* child : menu->get_children()) {
                auto* mi = dynamic_cast<Gtk::MenuItem*>(child);
                if (!mi || !mi->get_visible() || !mi->get_sensitive()) {
                    continue;
                }
                const auto alloc = mi->get_allocation();
                if (ev->y >= alloc.get_y() && ev->y < alloc.get_y() + alloc.get_height()) {
                    menu->select_item(*mi);
                    break;
                }
            }
            return false;
        });

        menu->signal_leave_notify_event().connect([menu, drop](GdkEventCrossing* ev) -> bool {
            if (!ev || ev->detail == GDK_NOTIFY_INFERIOR) {
                return false;
            }

            // Still inside the menu's own bounds (item-to-item crossing)?
            if (auto win = menu->get_window()) {
                int wx = 0, wy = 0;
                win->get_origin(wx, wy);
                const double pad = 6.0;
                if (ev->x_root >= wx - pad && ev->y_root >= wy - pad
                        && ev->x_root < wx + menu->get_allocated_width() + pad
                        && ev->y_root < wy + menu->get_allocated_height() + pad) {
                    return false;
                }
            }

            // Over the dropdown arrow button itself? Keep the menu open —
            // closing here would fight the button's hover-open and flicker.
            Gtk::Widget* toplevel = drop->get_toplevel();
            if (toplevel && toplevel->get_window()) {
                int ix = 0, iy = 0;
                if (drop->translate_coordinates(*toplevel, 0, 0, ix, iy)) {
                    int ox = 0, oy = 0;
                    toplevel->get_window()->get_origin(ox, oy);
                    if (ev->x_root >= ox + ix && ev->y_root >= oy + iy
                            && ev->x_root < ox + ix + drop->get_allocated_width()
                            && ev->y_root < oy + iy + drop->get_allocated_height()) {
                        return false;
                    }
                }
            }

            drop->set_active(false);  // toggled handler ends the hover preview
            return false;
        });
    };

    // Applied-flash: a brief accent pulse confirms the click landed (the
    // flat buttons otherwise give no press feedback at all).
    const auto flashQuickEditButton = [](Gtk::Button* btn) {
        btn->get_style_context()->add_class("quick-applied");
        Glib::signal_timeout().connect_once([btn]() {
            btn->get_style_context()->remove_class("quick-applied");
        }, 900);
    };

    auto* autoButton = Gtk::manage(new Gtk::Button("Auto"));
    autoButton->set_name("QuickEditButton");
    autoButton->set_relief(Gtk::RELIEF_NONE);
    autoButton->set_tooltip_text("Auto Edit");
    autoButton->add_events(Gdk::ENTER_NOTIFY_MASK | Gdk::LEAVE_NOTIFY_MASK);
    autoButton->signal_enter_notify_event().connect([this](GdkEventCrossing* event) -> bool {
        if (!event || event->detail != GDK_NOTIFY_INFERIOR) {
            quickPreviewVariant_ = 0;
            requestQuickAutoParams(0, "Auto Edit", false);
        }
        return false;
    });
    autoButton->signal_leave_notify_event().connect([this](GdkEventCrossing* event) -> bool {
        if (!event || event->detail != GDK_NOTIFY_INFERIOR) {
            endQuickPreview(true);
        }
        return false;
    });
    autoButton->signal_clicked().connect([this, autoButton, flashQuickEditButton]() {
        flashQuickEditButton(autoButton);
        requestQuickAutoParams(0, "Auto Edit", true);
    });

    auto* autoMenu = Gtk::manage(new Gtk::Menu());
    const std::vector<std::pair<Glib::ustring, int>> autoItems = {
        {"Auto Edit", 0},
        {"Auto Grade", 1},
        {"Film Lab", 2},
        {"Auto Grade + Film Lab", 3}
    };
    for (const auto& item : autoItems) {
        auto* mi = Gtk::manage(new Gtk::MenuItem(item.first));
        mi->add_events(Gdk::ENTER_NOTIFY_MASK | Gdk::POINTER_MOTION_MASK);
        const auto previewItem = [this, item]() {
            if (quickPreviewVariant_ == item.second && quickPreviewActive_) {
                return;
            }
            quickPreviewVariant_ = item.second;
            requestQuickAutoParams(item.second, item.first, false);
            if (!quickPreviewActive_) {
                quickPreviewVariant_ = -1;
            }
        };
        mi->signal_select().connect(previewItem);
        mi->signal_enter_notify_event().connect([previewItem](GdkEventCrossing*) -> bool {
            previewItem();
            return false;
        });
        mi->signal_motion_notify_event().connect([previewItem](GdkEventMotion*) -> bool {
            previewItem();
            return false;
        });
        mi->signal_activate().connect([this, item]() {
            requestQuickAutoParams(item.second, item.first, true);
        });
        autoMenu->append(*mi);
    }
    autoMenu->show_all();

    auto* autoDrop = Gtk::manage(new Gtk::MenuButton());
    autoDrop->set_name("QuickEditDrop");
    autoDrop->set_image(*Gtk::manage(new RTImage("arrow-down-small", Gtk::ICON_SIZE_MENU)));
    autoDrop->set_relief(Gtk::RELIEF_NONE);
    autoDrop->set_tooltip_text("Auto options");
    autoDrop->set_halign(Gtk::ALIGN_END);
    autoDrop->set_valign(Gtk::ALIGN_END);
    autoDrop->set_size_request(24, 22);
    autoDrop->set_popup(*autoMenu);
    // Hover-open: entering the arrow pops the menu without a click.
    // Enter-notify only (motion re-triggering was the stranded-grab bug).
    autoDrop->add_events(Gdk::ENTER_NOTIFY_MASK);
    autoDrop->signal_enter_notify_event().connect([autoDrop](GdkEventCrossing* event) -> bool {
        if ((!event || event->detail != GDK_NOTIFY_INFERIOR) && !autoDrop->get_active()) {
            autoDrop->set_active(true);
        }
        return false;
    });
    autoDrop->signal_toggled().connect([this, autoDrop]() {
        if (!autoDrop->get_active()) {
            endQuickPreview(true);
        }
    });
    wireHoverMenuBehavior(autoMenu, autoDrop);

    auto* bwButton = Gtk::manage(new Gtk::Button("B&W"));
    bwButton->set_name("QuickEditButton");
    bwButton->set_relief(Gtk::RELIEF_NONE);
    bwButton->set_tooltip_text("Black & White");
    bwButton->add_events(Gdk::ENTER_NOTIFY_MASK | Gdk::LEAVE_NOTIFY_MASK);
    bwButton->signal_enter_notify_event().connect([this](GdkEventCrossing* event) -> bool {
        if (!event || event->detail != GDK_NOTIFY_INFERIOR) {
            quickPreviewVariant_ = 100;
            beginQuickPreview(makeQuickBWParams(0), "Black & White");
        }
        return false;
    });
    bwButton->signal_leave_notify_event().connect([this](GdkEventCrossing* event) -> bool {
        if (!event || event->detail != GDK_NOTIFY_INFERIOR) {
            endQuickPreview(true);
        }
        return false;
    });
    bwButton->signal_clicked().connect([this, bwButton, flashQuickEditButton]() {
        flashQuickEditButton(bwButton);
        applyQuickEditParams(makeQuickBWParams(0), "Black & White", true);
    });

    auto* bwMenu = Gtk::manage(new Gtk::Menu());
    const std::vector<std::pair<Glib::ustring, int>> bwItems = {
        {"Neutral B&W", 0},
        {"Portrait B&W", 1},
        {"High Contrast", 2},
        {"Red Filter", 3},
        {"Green Filter", 4},
        {"Infrared", 5}
    };
    for (const auto& item : bwItems) {
        auto* mi = Gtk::manage(new Gtk::MenuItem(item.first));
        mi->add_events(Gdk::ENTER_NOTIFY_MASK | Gdk::POINTER_MOTION_MASK);
        const auto previewItem = [this, item]() {
            const int variant = 100 + item.second;
            if (quickPreviewVariant_ == variant && quickPreviewActive_) {
                return;
            }
            quickPreviewVariant_ = variant;
            beginQuickPreview(makeQuickBWParams(item.second), item.first);
            if (!quickPreviewActive_) {
                quickPreviewVariant_ = -1;
            }
        };
        mi->signal_select().connect(previewItem);
        mi->signal_enter_notify_event().connect([previewItem](GdkEventCrossing*) -> bool {
            previewItem();
            return false;
        });
        mi->signal_motion_notify_event().connect([previewItem](GdkEventMotion*) -> bool {
            previewItem();
            return false;
        });
        mi->signal_activate().connect([this, item]() {
            applyQuickEditParams(makeQuickBWParams(item.second), item.first, true);
        });
        bwMenu->append(*mi);
    }
    bwMenu->show_all();

    auto* bwDrop = Gtk::manage(new Gtk::MenuButton());
    bwDrop->set_name("QuickEditDrop");
    bwDrop->set_image(*Gtk::manage(new RTImage("arrow-down-small", Gtk::ICON_SIZE_MENU)));
    bwDrop->set_relief(Gtk::RELIEF_NONE);
    bwDrop->set_tooltip_text("Black & White options");
    bwDrop->set_halign(Gtk::ALIGN_END);
    bwDrop->set_valign(Gtk::ALIGN_END);
    bwDrop->set_size_request(24, 22);
    bwDrop->set_popup(*bwMenu);
    // Hover-open, enter-notify only (see autoDrop)
    bwDrop->add_events(Gdk::ENTER_NOTIFY_MASK);
    bwDrop->signal_enter_notify_event().connect([bwDrop](GdkEventCrossing* event) -> bool {
        if ((!event || event->detail != GDK_NOTIFY_INFERIOR) && !bwDrop->get_active()) {
            bwDrop->set_active(true);
        }
        return false;
    });
    bwDrop->signal_toggled().connect([this, bwDrop]() {
        if (!bwDrop->get_active()) {
            endQuickPreview(true);
        }
    });
    wireHoverMenuBehavior(bwMenu, bwDrop);

    auto* autoSplit = Gtk::manage(new Gtk::Overlay());
    autoSplit->set_name("QuickEditSplit");
    autoSplit->set_hexpand(true);
    autoSplit->add(*autoButton);
    autoSplit->add_overlay(*autoDrop);

    auto* bwSplit = Gtk::manage(new Gtk::Overlay());
    bwSplit->set_name("QuickEditSplit");
    bwSplit->set_hexpand(true);
    bwSplit->add(*bwButton);
    bwSplit->add_overlay(*bwDrop);

    quickEditBar_->pack_start(*autoSplit, Gtk::PACK_EXPAND_WIDGET, 0);
    quickEditBar_->pack_start(*bwSplit, Gtk::PACK_EXPAND_WIDGET, 0);
    editPanel->pack_start(*quickEditBar_, Gtk::PACK_SHRINK, 0);
    editPanel->reorder_child(*quickEditBar_, 0);
}

void ToolPanelCoordinator::applyQuickEditParams(ProcParams params, const Glib::ustring& descr, bool commit)
{
    if (!ipc) {
        return;
    }

    quickPreviewFinalizeConn_.disconnect();

    if (!commit) {
        // Hover previews must not masquerade as profile changes. Updating the
        // processor directly keeps the tool widgets and history anchored to
        // the committed state, so closing a menu cannot race a later click
        // and restore over it.
        ProcParams* target = ipc->beginUpdateParams();
        *target = std::move(params);
        ipc->endUpdateParams(rtengine::EvProfileChangeNotification);
        return;
    }

    quickPreviewActive_ = false;
    quickPreviewVariant_ = -1;
    toolPanelEditLog("[quickEdit] commit descr=%s film=%d expcomp=%.4f\n",
        descr.c_str(), params.filmPresets.enabled, params.toneCurve.expcomp);

    PartialProfile profile(true);
    profile.set(true);
    *(profile.pparams) = params;
    profile.pedited->locallab.spots.resize(params.locallab.spots.size(), LocallabParamsEdited::LocallabSpotEdited(true));
    profileChange(
        &profile,
        commit ? rtengine::EvProfileChanged : rtengine::EvProfileChangeNotification,
        descr);
    profile.deleteInstance();

}

void ToolPanelCoordinator::requestQuickAutoParams(int mode, const Glib::ustring& descr, bool commit)
{
    toolPanelEditLog(
        "[quickEdit] request descr=%s mode=%d commit=%d ipc=%d thumbnail=%s\n",
        descr.c_str(),
        mode,
        commit ? 1 : 0,
        ipc ? 1 : 0,
        quickAutoEditThumbnail_ ? quickAutoEditThumbnail_->getFileName().c_str() : "none");
    if (!ipc || !quickAutoEditThumbnail_ || !quickAutoEditPool_ || !quickAutoEditGeneration_) {
        return;
    }
    if (!commit && quickAutoEditCommitPending_) {
        return;
    }

    quickPreviewFinalizeConn_.disconnect();

    ProcParams source;
    if (quickPreviewActive_) {
        source = quickPreviewRestore_;
    } else {
        ipc->getParams(&source);
    }
    if (!commit && !quickPreviewActive_) {
        quickPreviewRestore_ = source;
        quickPreviewActive_ = true;
    }

    quickAutoEditCommitPending_ = commit;
    const unsigned generation = quickAutoEditGeneration_->fetch_add(1, std::memory_order_acq_rel) + 1;
    const auto generationState = quickAutoEditGeneration_;
    Thumbnail* const thumbnail = quickAutoEditThumbnail_;
    rtengine::StagedImageProcessor* const targetIpc = ipc;
    thumbnail->increaseRef();

    quickAutoEditPool_->push([
        this,
        generationState,
        generation,
        thumbnail,
        targetIpc,
        source = std::move(source),
        mode,
        descr,
        commit
    ]() mutable {
        if (generationState->load(std::memory_order_acquire) != generation) {
            thumbnail->decreaseRef();
            return;
        }

        ProcParams params;
        buildSteepAutoEditParams(
            *thumbnail,
            mode >= 3 ? SteepAutoEditMode::GradedFilm
                      : mode == 2 ? SteepAutoEditMode::GradeFilm
                      : mode == 1 ? SteepAutoEditMode::Grade : SteepAutoEditMode::Neutral,
            source,
            params);
        toolPanelEditLog(
            "[quickEdit] analyzed descr=%s commit=%d exposure=%.4f brightness=%d contrast=%d film=%s file=%s\n",
            descr.c_str(),
            commit ? 1 : 0,
            params.toneCurve.expcomp,
            params.toneCurve.brightness,
            params.toneCurve.contrast,
            params.filmPresets.enabled ? params.filmPresets.preset.c_str() : "off",
            thumbnail->getFileName().c_str());
        thumbnail->decreaseRef();

        if (generationState->load(std::memory_order_acquire) != generation) {
            return;
        }

        idle_register.add([
            this,
            generationState,
            generation,
            thumbnail,
            targetIpc,
            params = std::move(params),
            descr,
            commit
        ]() mutable -> bool {
            if (generationState->load(std::memory_order_acquire) != generation
                    || !ipc
                    || ipc != targetIpc
                    || quickAutoEditThumbnail_ != thumbnail) {
                // A stale commit must still release the pending flag, or all
                // future hover previews stay blocked for this session.
                if (commit && generationState->load(std::memory_order_acquire) == generation) {
                    quickAutoEditCommitPending_ = false;
                }
                return false;
            }
            if (commit) {
                quickAutoEditCommitPending_ = false;
            } else if (!quickPreviewActive_) {
                return false;
            }
            applyQuickEditParams(std::move(params), descr, commit);
            return false;
        });
    });
}

ProcParams ToolPanelCoordinator::makeQuickBWParams(int mode) const
{
    ProcParams pp;
    if (quickPreviewActive_) {
        pp = quickPreviewRestore_;
    } else if (ipc) {
        ipc->getParams(&pp);
    }

    pp.blackwhite.enabled = true;
    pp.blackwhite.enabledcc = true;
    pp.blackwhite.method = mode == 0 ? "Desaturation" : "ChannelMixer";
    pp.blackwhite.filter = "None";
    pp.blackwhite.setting = "RGB-Rel";
    pp.blackwhite.neutrals = 0;
    pp.blackwhite.tone = 0;
    pp.blackwhite.strength = 100;

    switch (mode) {
        case 1:
            pp.blackwhite.method = "Perceptual";
            pp.blackwhite.setting = "Portrait";
            pp.blackwhite.neutrals = 8;
            pp.blackwhite.tone = 4;
            break;
        case 2:
            pp.blackwhite.method = "ChannelMixer";
            pp.blackwhite.setting = "HighContrast";
            pp.blackwhite.neutrals = -10;
            pp.blackwhite.tone = 10;
            pp.toneCurve.contrast = std::min(100, pp.toneCurve.contrast + 8);
            break;
        case 3:
            pp.blackwhite.method = "ChannelMixer";
            pp.blackwhite.filter = "Red";
            pp.blackwhite.setting = "Panchromatic";
            pp.blackwhite.neutrals = -4;
            break;
        case 4:
            pp.blackwhite.method = "ChannelMixer";
            pp.blackwhite.filter = "Green";
            pp.blackwhite.setting = "Orthochromatic";
            pp.blackwhite.neutrals = 4;
            break;
        case 5:
            pp.blackwhite.method = "ChannelMixer";
            pp.blackwhite.filter = "Red";
            pp.blackwhite.setting = "InfraRed";
            pp.blackwhite.tone = 18;
            pp.toneCurve.contrast = std::min(100, pp.toneCurve.contrast + 5);
            break;
        default:
            break;
    }

    return pp;
}

void ToolPanelCoordinator::beginQuickPreview(const ProcParams& params, const Glib::ustring& descr)
{
    if (!ipc) {
        return;
    }
    if (!quickPreviewActive_) {
        ipc->getParams(&quickPreviewRestore_);
        quickPreviewActive_ = true;
    }
    applyQuickEditParams(params, descr, false);
}

void ToolPanelCoordinator::endQuickPreview(bool restore)
{
    const bool commitPending = quickAutoEditCommitPending_;
    if (!commitPending && quickAutoEditGeneration_) {
        quickAutoEditGeneration_->fetch_add(1, std::memory_order_acq_rel);
    }
    if (!quickPreviewActive_) {
        return;
    }

    const ProcParams restoreParams = quickPreviewRestore_;
    quickPreviewActive_ = false;
    quickPreviewVariant_ = -1;
    if (restore && !commitPending) {
        // With a commit in flight the committed params land momentarily;
        // restoring here would just flash the old image and waste a render.
        applyQuickEditParams(restoreParams, "Preview", false);
    }
    if (restore && !commitPending) {
        rtengine::StagedImageProcessor* const targetIpc = ipc;
        quickPreviewFinalizeConn_ = Glib::signal_timeout().connect(
            [this, targetIpc]() -> bool {
                if (!ipc || ipc != targetIpc) {
                    return false;
                }
                ipc->startProcessing(M_HIGHQUAL | M_MONITOR);
                return false;
            },
            kQuickPreviewHighDetailDelayMs,
            G_PRIORITY_LOW);
    }
}

void ToolPanelCoordinator::panelChanged(const rtengine::ProcEvent& event, const Glib::ustring& descr)
{
    if (!ipc) {
        toolPanelEditLog("panelChanged no-ipc event=%d descr=%s\n", int(event), descr.c_str());
        deferredPanelChangePending_ = false;
        return;
    }

    noteRawLoadForegroundActivity();

    int changeFlags = rtengine::RefreshMapper::getInstance()->getAction(event);
    toolPanelEditLog("panelChanged start event=%d flags=%d descr=%s\n", int(event), changeFlags, descr.c_str());

    ProcParams* params = ipc->tryBeginUpdateParams();
    if (!params) {
        toolPanelEditLog("panelChanged busy event=%d flags=%d\n", int(event), changeFlags);
        deferPanelChanged(event, descr);
        return;
    }

    // In mask mode, locallab tool widgets are hidden and have stale/default values.
    // Skip their write() to prevent overwriting bridged spot settings (expcomp,
    // lightness, etc.) that were set by bridgeGlobalToSpot in a previous cycle.
    // The control spot geometry (center, size, shape) is still written from the treemodel.
    if (maskModeActive_) {
        locallab->setSkipToolWrites(true);
    }

    for (auto toolPanel : toolPanels) {
        toolPanel->write(params);
    }

    if (maskModeActive_) {
        locallab->setSkipToolWrites(false);
    }

    // Manual edits establish a new center for every strip. Strip-generated
    // edits retain the existing centers so revisiting a position is absolute.
    if (!previewStripChangeSource_) {
        PreviewStrip* strips[] = {exposureStrip_, colorStrip_, detailStrip_, effectsStrip_, bwStrip_};
        for (auto* strip : strips) {
            if (strip) strip->setCurrentParams(*params);
        }
    }

    // Bridge global tool values to the gradient locallab spot.
    // Only bridge when the event triggers processing that uses globals (tone curve,
    // locallab, or crop). For unrelated events (sharpening, noise reduction, monitor
    // changes), skip bridging entirely — the previous cycle's results are still valid.
    if ((changeFlags & (M_AUTOEXP | M_RGBCURVE | M_CROP))
            && bridgeGlobalToSpot(params, event)) {
        // Ensure locallab runs. For most bridgeable events (EvExpComp etc.),
        // M_AUTOEXP is already in changeFlags — this is a no-op OR.
        // For RGBCURVE-only events, this adds the locallab trigger.
        changeFlags |= M_AUTOEXP;
    }

    // When in mask mode, tool widgets show spot-local values. The write() loop
    // above wrote those into global params. Restore the saved globals so that
    // non-bridged events (color grading, denoising, etc.) don't corrupt them.
    if (maskModeActive_) {
        params->toneCurve = savedToneCurve_;
        params->vibrance = savedVibrance_;
        params->sharpening = savedSharpening_;
        params->sh = savedSH_;
        params->blackwhite = savedBlackWhite_;
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
    if (locallab && event == rtengine::EvlocallabshowmaskMethod) {
        const Locallab::llMaskVisibility maskStruc = locallab->getMaskVisibility();
        ipc->setLocallabMaskVisibility(maskStruc.previewDeltaE, maskStruc.showMaskOverlay, maskStruc.colorMask, maskStruc.colorMaskinv, maskStruc.expMask, maskStruc.expMaskinv,
                maskStruc.SHMask, maskStruc.SHMaskinv, maskStruc.vibMask, maskStruc.softMask,
                maskStruc.blMask, maskStruc.tmMask, maskStruc.retiMask, maskStruc.sharMask,
                maskStruc.lcMask, maskStruc.cbMask, maskStruc.logMask, maskStruc.maskMask, maskStruc.cieMask);
    } else if (locallab && (event == rtengine::EvLocallabSpotCreated || event == rtengine::EvLocallabSpotSelected ||
            event == rtengine::EvLocallabSpotSelectedWithMask ||
            event == rtengine::EvLocallabSpotDeleted /*|| event == rtengine::Evlocallabshowreset*/ ||
            event == rtengine::EvlocallabToolRemovedWithRefresh)) {
        locallab->resetMaskVisibility();
        hoverMaskApplied_ = false;
        hoverPreviewSpot_ = -1;
        hoverRestoreSpot_ = -1;
        hoverMaskDebounce_.disconnect();
        hoverMaskWatchdog_.disconnect();
        ipc->setLocallabMaskVisibility(false, false, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    } else if (locallab && hoverMaskApplied_) {
        // Hover overlay is active — re-apply mask visibility so the reprocess
        // triggered by this parameter change renders the overlay with updated values.
        const Locallab::llMaskVisibility mv = locallab->getMaskVisibility();
        ipc->setLocallabMaskVisibility(mv.previewDeltaE, mv.showMaskOverlay,
            mv.colorMask, mv.colorMaskinv, mv.expMask, mv.expMaskinv,
            mv.SHMask, mv.SHMaskinv, mv.vibMask, mv.softMask,
            mv.blMask, mv.tmMask, mv.retiMask, mv.sharMask,
            mv.lcMask, mv.cbMask, mv.logMask, mv.maskMask, mv.cieMask);
    }

    toolPanelEditLog("panelChanged commit event=%d flags=%d expcomp=%.4f brightness=%d contrast=%d wbTemp=%.1f\n",
        int(event), changeFlags, params->toneCurve.expcomp, params->toneCurve.brightness,
        params->toneCurve.contrast, static_cast<double>(params->wb.temperature));

    ipc->endUpdateParams(changeFlags);    // starts the IPC processing

    hasChanged = true;
    updateResetButtons();

    for (auto paramcListener : paramcListeners) {
        paramcListener->procParamsChanged(params, event, descr);
    }

    // When spot changes in mask mode, reload spot values into global tools
    if (maskModeActive_ &&
        (event == rtengine::EvLocallabSpotSelectedWithMask ||
         event == rtengine::EvLocallabSpotSelected ||
         event == rtengine::EvLocallabSpotCreated ||
         event == rtengine::EvLocallabSpotDeleted)) {
        loadSpotIntoGlobalTools();
    }

    // Locallab spot curves are set visible if at least one photo has been loaded (to avoid
    // segfault) and locallab panel is active
    // When a new photo is loaded, Locallab spot curves need to be set visible again
const auto func =
    [this]() -> bool
    {
        if (locallab && photoLoadedOnce && modeButtonBar->getActiveMode() == EditorMode::MASK) {
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

    const bool logProfile = toolPanelEditLogEnabled();
    const auto profileStart = std::chrono::steady_clock::now();
    auto lastStep = profileStart;
    const auto logStep =
        [&](const char* step) {
            if (!logProfile) {
                return;
            }

            const auto now = std::chrono::steady_clock::now();
            const long long stepMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - lastStep).count();
            const long long totalMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - profileStart).count();
            toolPanelEditLog(
                "profileChange step event=%d fromLastSave=%d step=%s stepMs=%lld totalMs=%lld\n",
                int(event), fromLastSave ? 1 : 0, step, stepMs, totalMs);
            lastStep = now;
        };

    if (logProfile) {
        toolPanelEditLog(
            "profileChange start event=%d fromLastSave=%d panels=%zu descr=%s\n",
            int(event), fromLastSave ? 1 : 0, toolPanels.size(), descr.c_str());
    }

    ProcParams *params = ipc->beginUpdateParams();
    logStep("begin-update");

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
    logStep("apply-profile");

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
    logStep("merge-params");

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
    logStep("crop-trim");

    std::shared_ptr<ProcParams> deferredMetadataParams;

    // updating the GUI with updated values
    for (size_t i = 0; i < toolPanels.size(); i++) {
        if (event == rtengine::EvPhotoLoaded && toolPanels[i] == metadata) {
            deferredMetadataParams = std::make_shared<ProcParams>(*params);
            continue;
        }

        const auto readStart = std::chrono::steady_clock::now();
        toolPanels[i]->read(params);
        const long long readMs = toolPanelDurationMs(readStart);

        if (logProfile && readMs > 0) {
            toolPanelEditLog(
                "profileChange tool-read event=%d index=%zu name=%s ms=%lld\n",
                int(event), i, toolPanels[i]->getToolName().c_str(), readMs);
        }

        if (event == rtengine::EvPhotoLoaded || event == rtengine::EvProfileChanged) {
            const auto curveStart = std::chrono::steady_clock::now();
            toolPanels[i]->autoOpenCurve();
            const long long curveMs = toolPanelDurationMs(curveStart);

            if (logProfile && curveMs > 0) {
                toolPanelEditLog(
                    "profileChange auto-open event=%d index=%zu name=%s ms=%lld\n",
                    int(event), i, toolPanels[i]->getToolName().c_str(), curveMs);
            }
        }
    }
    logStep("tool-reads");

    if (deferredMetadataParams && metadata) {
        const unsigned metadataGeneration = ++deferredMetadataReadGeneration_;
        idle_register.add(
            [this, deferredMetadataParams, metadataGeneration]() -> bool
            {
                if (metadata && metadataGeneration == deferredMetadataReadGeneration_) {
                    metadata->read(deferredMetadataParams.get());
                }

                return false;
            });
    }
    logStep("metadata-defer");

    if ((event == rtengine::EvPhotoLoaded || event == rtengine::EvProfileChanged) && locallab) {
        const auto locallabStart = std::chrono::steady_clock::now();
        // Reset Locallab expander visibility once per loaded profile, not once per tool.
        locallab->openAllTools();

        if (logProfile) {
            toolPanelEditLog(
                "profileChange locallab-open-all event=%d ms=%lld\n",
                int(event), toolPanelDurationMs(locallabStart));
        }
    }
    logStep("locallab-open-all");

    // Update preview strips with new params
    {
        PreviewStrip* strips[] = {exposureStrip_, colorStrip_, detailStrip_, effectsStrip_, bwStrip_};
        for (auto* strip : strips) {
            if (strip) {
                strip->setCurrentParams(*params);
            }
        }
    }
    logStep("preview-strips");

    if (event == rtengine::EvPhotoLoaded || event == rtengine::EvProfileChanged || event == rtengine::EvHistoryBrowsed || event == rtengine::EvCTRotate) {
        // updating the "on preview" geometry
        gradient->updateGeometry(params->gradient.centerX, params->gradient.centerY, params->gradient.feather, params->gradient.degree, fw, fh);
    }
    logStep("gradient-geometry");

    // Reset Locallab mask visibility — overlay off by default (hover-driven)
    if (locallab) {
        locallab->resetMaskVisibility();
    }
    hoverMaskApplied_ = false;
    hoverMaskDebounce_.disconnect();
    ipc->setLocallabMaskVisibility(false, false, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    logStep("reset-mask-visibility");

    hasChanged = event != rtengine::EvProfileChangeNotification;
    captureBaseline();
    updateResetButtonsFromBaseline();
    logStep("baseline-reset-buttons");

    for (auto paramcListener : paramcListeners) {
        paramcListener->procParamsChanged(params, event, descr);
    }
    logStep("param-listeners");

    // Locallab spot curves are set visible if at least one photo has been loaded (to avoid
    // segfault) and locallab panel is active
    // When a new photo is loaded, Locallab spot curves need to be set visible again
    const auto func =
        [this]() -> bool
        {
            if (locallab && photoLoadedOnce && modeButtonBar->getActiveMode() == EditorMode::MASK) {
                locallab->subscribe();
            }

            return false;
        };

    if (event == rtengine::EvPhotoLoaded) {
        idle_register.add(func);
    }
    logStep("idle-subscribe");

    photoLoadedOnce = true;

    // Start IPC processing after profile GUI state and listeners have finished
    // consuming the update params. Starting earlier lets the worker race the
    // tail of this method during photo-load/profile-change handoff.
    if (filterRawRefresh) {
        ipc->endUpdateParams(rtengine::RefreshMapper::getInstance()->getAction(event) & ALLNORAW);
    } else {
        ipc->endUpdateParams(event);
    }
    logStep("end-update");
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
        if (locallab) {
            ipc->setLocallabListener(locallab);
        }
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
    captureBaseline();
    updateResetButtons();
}


void ToolPanelCoordinator::closeImage()
{
    if (quickAutoEditGeneration_) {
        quickAutoEditGeneration_->fetch_add(1, std::memory_order_acq_rel);
    }
    quickAutoEditThumbnail_ = nullptr;
    quickAutoEditCommitPending_ = false;
    quickPreviewActive_ = false;
    quickPreviewFinalizeConn_.disconnect();
    deferredPanelChangePending_ = false;
    deferredPanelChangeTimerActive_ = false;
    deferredPanelChangeConn_.disconnect();
    previewStripChangeSource_ = nullptr;
    deferredPreviewStripChangeSource_ = nullptr;

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
    if (locallab) {
        locallab->updateShowtooltipVisibility(showtooltip);
    }
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

bool ToolPanelCoordinator::autoLevelRequested(double& correction)
{
    correction = 0.0;
    if (!ipc) {
        return false;
    }

    rtengine::ImageSource *src = dynamic_cast<rtengine::ImageSource *>(ipc->getInitialImage());
    if (!src) {
        return false;
    }

    rtengine::procparams::ProcParams params;
    ipc->getParams(&params);
    const auto result = rtengine::PerspectiveCorrection::autoLevel(src, &params);
    if (!result.success) {
        return false;
    }

    correction = result.angle;
    return true;
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
        Tool::GRAIN, Tool::TILT_SHIFT, Tool::FILM_PRESETS, Tool::FILM_SIMULATION, Tool::SOFT_LIGHT,
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
    if (tool == TMPerspectiveGrid) {
        perspective->setGridEditMode(false);
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

        case TMPerspectiveGrid: {
            toolBar->blockEditDeactivation(false);
            perspective->setGridEditMode(true);
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
    if (flatfield) {
        flatfield->setShortcutPath(dirname);
    }
}

void ToolPanelCoordinator::setEditProvider(EditDataProvider *provider)
{
    editDataProvider = provider;
    imageArea_ = dynamic_cast<ImageArea*>(provider);

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
        case Tool::TILT_SHIFT:
            return tiltshift;
        case Tool::LENS_BLUR:
            return lensblur;
        case Tool::FILM_PRESETS:
            return filmPresets;
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

void ToolPanelCoordinator::setThumbnail(Thumbnail* thm)
{
    if (quickAutoEditGeneration_) {
        quickAutoEditGeneration_->fetch_add(1, std::memory_order_acq_rel);
    }
    quickAutoEditThumbnail_ = thm;
    quickAutoEditCommitPending_ = false;
    toolPanelEditLog(
        "[quickEdit] thumbnail=%s\n",
        thm ? thm->getFileName().c_str() : "none");
    PreviewStrip* strips[] = {exposureStrip_, colorStrip_, detailStrip_, effectsStrip_, bwStrip_};
    for (auto* strip : strips) {
        if (strip) {
            strip->setThumbnail(thm);
        }
    }
}

void ToolPanelCoordinator::captureBaseline()
{
    // Capture what write() actually produces as the "clean" state.
    // This handles quirks like Camera WB writing camera metadata temp
    // instead of ProcParams default temp.
    ProcParams bp;
    for (auto toolPanel : toolPanels) {
        toolPanel->write(&bp);
    }
    baselineParams_ = bp;
}

void ToolPanelCoordinator::updateResetButtonsFromBaseline()
{
    if (!ipc) return;
    if (!lightGroup) return;
    if (suppressResetUpdate_) return;

    lightGroup->setResetVisible(false);
    colorGroup->setResetVisible(false);
    detailGroup->setResetVisible(false);
    effectsGroup->setResetVisible(false);

    ProcParams stock;
    bwGroup->setResetVisible(!(baselineParams_.blackwhite == stock.blackwhite));
    spotGroup->setResetVisible(!(baselineParams_.spot == stock.spot));
    maskingGroup->setResetVisible(locallab && locallab->getSpotCount() > 0);

    if (cropResetBtn_) {
        cropResetBtn_->set_visible(false);
        cropResetBtn_->set_no_show_all(true);
    }

    if (perspResetBtn_) {
        perspResetBtn_->set_visible(false);
        perspResetBtn_->set_no_show_all(true);
    }
}

void ToolPanelCoordinator::updateResetButtons()
{
    if (!ipc) return;
    if (!lightGroup) return;
    if (suppressResetUpdate_) return;

    // Collect current params from all tool panels
    ProcParams current;
    for (auto toolPanel : toolPanels) {
        toolPanel->write(&current);
    }

    const ProcParams& b = baselineParams_;

    // Light group: toneCurve + shadowshighlights + rgbcurves
    bool lightDirty = !(current.toneCurve == b.toneCurve)
                   || !(current.sh == b.sh)
                   || !(current.rgbCurves == b.rgbCurves);
    lightGroup->setResetVisible(lightDirty);

    // B&W group — compare against factory defaults so X shows whenever B&W is enabled
    ProcParams stock;
    bool bwDirty = !(current.blackwhite == stock.blackwhite);
    bwGroup->setResetVisible(bwDirty);

    // Color group: wb + vibrance + hsvequalizer + colorgrading + pointcolor
    bool wbDirty = !(current.wb == b.wb);
    bool vibDirty = !(current.vibrance == b.vibrance);
    bool hsvDirty = !(current.hsvequalizer == b.hsvequalizer);
    bool cgDirty = !(current.colorGrading == b.colorGrading);
    bool pcDirty = !(current.pointcolor == b.pointcolor);
    bool colorDirty = wbDirty || vibDirty || hsvDirty || cgDirty || pcDirty;
    colorGroup->setResetVisible(colorDirty);

    // Detail group: sharpening + dirpyrdenoise + dehaze
    bool detailDirty = !(current.sharpening == b.sharpening)
                    || !(current.dirpyrDenoise == b.dirpyrDenoise)
                    || !(current.dehaze == b.dehaze);
    detailGroup->setResetVisible(detailDirty);

    // Effects group: texture + clarity + grain + tiltshift + pcvignette + filmPresets + softlight
    bool effectsDirty = !(current.texture == b.texture)
                     || !(current.clarity == b.clarity)
                     || !(current.grain == b.grain)
                     || !(current.tiltShift == b.tiltShift)
                     || !(current.pcvignette == b.pcvignette)
                     || !(current.filmPresets == b.filmPresets)
                     || !(current.softlight == b.softlight);
    effectsGroup->setResetVisible(effectsDirty);

    // Spot removal group — compare against factory defaults (like B&W)
    bool spotDirty = !(current.spot == stock.spot);
    spotGroup->setResetVisible(spotDirty);

    // Masking group — dirty when any spots exist in the control panel.
    // Can't use locallab->write() (doesn't populate spots on a fresh ProcParams)
    // and can't use ipc->beginUpdateParams() (interferes with processing pipeline).
    // Query the control panel's treemodel directly via Locallab's expsettings.
    bool maskingDirty = locallab && locallab->getSpotCount() > 0;
    maskingGroup->setResetVisible(maskingDirty);

    // Crop section: crop + rotate
    if (cropResetBtn_) {
        bool cropDirty = !(current.crop == b.crop)
                      || !(current.rotate == b.rotate);
        if (cropDirty) {
            cropResetBtn_->set_no_show_all(false);
            cropResetBtn_->set_visible(true);
            cropResetBtn_->show_all();
        } else {
            cropResetBtn_->set_visible(false);
            cropResetBtn_->set_no_show_all(true);
        }
    }

    // Perspective section
    if (perspResetBtn_) {
        bool perspDirty = !(current.perspective == b.perspective);
        if (perspDirty) {
            perspResetBtn_->set_no_show_all(false);
            perspResetBtn_->set_visible(true);
            perspResetBtn_->show_all();
        } else {
            perspResetBtn_->set_visible(false);
            perspResetBtn_->set_no_show_all(true);
        }
    }
}
