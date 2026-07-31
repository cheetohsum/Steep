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
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <unordered_set>
#include <vector>

#include <gtkmm.h>

#include "modebuttonbar.h"
#include "tools/bayerpreprocess.h"
#include "tools/bayerprocess.h"
#include "tools/bayerrawexposure.h"
#include "tools/blackwhite.h"
#include "tools/cacorrection.h"
#include "tools/chmixer.h"
#include "coarsepanel.h"
#include "tools/colorappearance.h"
#include "tools/colorgrading.h"
#include "tools/colortoning.h"
#include "tools/compressgamut.h"
#include "tools/crop.h"
#include "tools/darkframe.h"
#include "tools/defringe.h"
#include "tools/dehaze.h"
#include "tools/doubleexposure.h"
#include "tools/dirpyrdenoise.h"
#include "tools/dirpyrequalizer.h"
#include "tools/distortion.h"
#include "tools/epd.h"
#include "tools/fattaltonemap.h"
#include "tools/filmnegative.h"
#include "tools/filmpresets.h"
#include "tools/filmsimulation.h"
#include "tools/flatfield.h"
#include "tools/framing.h"
#include "tools/gradient.h"
#include "guiutils.h"
#include "tools/hsvequalizer.h"
#include "tools/icmpanel.h"
#include "imageareatoollistener.h"
#include "tools/aidenoise.h"
#include "tools/impulsedenoise.h"
#include "tools/labcurve.h"
#include "tools/lensgeom.h"
#include "lensgeomlistener.h"
#include "tools/lensprofile.h"
#include "tools/localcontrast.h"
#include "tools/locallab.h"
#include "tools/texture.h"
#include "tools/clarity.h"
#include "tools/grain.h"
#include "tools/tiltshift.h"
#include "tools/lensblur.h"
#include "tools/pcvignette.h"
#include "tools/pointcolor.h"
#include "tools/pdsharpening.h"
#include "tools/perspective.h"
#include "pparamschangelistener.h"
#include "tools/preprocess.h"
#include "tools/preprocesswb.h"
#include "profilechangelistener.h"
#include "tools/prsharpening.h"
#include "tools/rawcacorrection.h"
#include "tools/rawexposure.h"
#include "tools/resize.h"
#include "tools/retinex.h"
#include "tools/rgbcurves.h"
#include "tools/rotate.h"
#include "tools/sensorbayer.h"
#include "tools/sensorxtrans.h"
#include "tools/shadowshighlights.h"
#include "tools/sharpenedge.h"
#include "tools/sharpening.h"
#include "tools/sharpenmicro.h"
#include "tools/softlight.h"
#include "tools/spot.h"
#include "tools/tonecurve.h"
#include "tools/toneequalizer.h"
#include "toolbar.h"
#include "toolpanel.h"
#include "tools/vibrance.h"
#include "tools/vignetting.h"
#include "tools/wavelet.h"
#include "tools/whitebalance.h"
#include "tools/xtransprocess.h"
#include "tools/xtransrawexposure.h"

#include "exposurepreviewstrip.h"

#include "rtengine/noncopyable.h"
#include "rtengine/rtengine.h"

class ImageEditorCoordinator;
class MetaDataPanel;

class ToolPanelCoordinator :
    public ToolPanelListener,
    public ToolBarListener,
    public ProfileChangeListener,
    public WBProvider,
    public DFProvider,
    public FFProvider,
    public LensGeomListener,
    public SpotWBListener,
    public CropPanelListener,
    public PerspCorrectionPanelListener,
    public ICMPanelListener,
    public ImageAreaToolListener,
    public rtengine::ImageTypeListener,
    public FilmNegProvider,
    public PointColorPickListener,
    public rtengine::NonCopyable
{
protected:
    WhiteBalance* whitebalance;
    Vignetting* vignetting;
    Gradient* gradient;
    Locallab* locallab;
    Retinex*  retinex;
    PCVignette* pcvignette;
    LensGeometry* lensgeom;
    LensProfilePanel* lensProf;
    Rotate* rotate;
    Distortion* distortion;
    PerspCorrection* perspective;
    CACorrection* cacorrection;
    ColorAppearance* colorappearance;
    Vibrance* vibrance;
    ChMixer* chmixer;
    BlackWhite* blackwhite;
    Resize* resize;
    Framing* framing;
    PrSharpening* prsharpening;
    ICMPanel* icm;
    Crop* crop;
    ToneCurve* toneCurve;
    ShadowsHighlights* shadowshighlights;
    ToneEqualizer* toneEqualizer;
    LocalContrast *localContrast;
    Texture *texture;
    Clarity *clarity;
    Grain *grain;
    TiltShift *tiltshift;
    LensBlur *lensblur;
    Spot* spot;
    Defringe* defringe;
    Compressgamut* compressgamut;
    ImpulseDenoise* impulsedenoise;
    AIDenoise* aidenoise;
    DirPyrDenoise* dirpyrdenoise;
    EdgePreservingDecompositionUI *epd;
    Sharpening* sharpening;
    SharpenEdge* sharpenEdge;
    SharpenMicro* sharpenMicro;
    LCurve* lcurve;
    RGBCurves* rgbcurves;
    ColorToning* colortoning;
    ColorGrading* colorgrading;
    Wavelet * wavelet;
    DirPyrEqualizer* dirpyrequalizer;
    HSVEqualizer* hsvequalizer;
    PointColor* pointcolor;
    SoftLight *softlight;
    Dehaze *dehaze;
    DoubleExposure *doubleExposure;
    FilmSimulation *filmSimulation;
    FilmPresets *filmPresets;
    SensorBayer * sensorbayer;
    SensorXTrans * sensorxtrans;
    BayerProcess* bayerprocess;
    XTransProcess* xtransprocess;
    BayerPreProcess* bayerpreprocess;
    PreProcess* preprocess;
    DarkFrame* darkframe;
    FlatField* flatfield;
    RAWCACorr* rawcacorrection;
    RAWExposure* rawexposure;
    PreprocessWB* preprocessWB;
    BayerRAWExposure* bayerrawexposure;
    XTransRAWExposure* xtransrawexposure;
    FattalToneMapping *fattal;
    MetaDataPanel* metadata;
    FilmNegative* filmNegative;
    PdSharpening* pdSharpening;
    std::vector<PParamsChangeListener*> paramcListeners;
    std::unordered_map<Gtk::Widget *, FoldableToolPanel *>
        expanderToToolPanelMap;

    rtengine::StagedImageProcessor* ipc;

    std::vector<ToolPanel*> toolPanels;
    std::vector<FoldableToolPanel*> favoritesToolPanels;
    ToolVBox* favoritePanel;
    ToolVBox* exposurePanel;
    ToolVBox* detailsPanel;
    ToolVBox* colorPanel;
    ToolVBox* transformPanel;
    ToolVBox* rawPanel;
    ToolVBox* advancedPanel;
    ToolVBox* locallabPanel;

    // Edit mode grouped panels
    ToolVBox* editPanel;
    ToolGroup* lightGroup;
    ToolGroup* colorGroup;
    ToolGroup* detailGroup;
    ToolGroup* effectsGroup;
    ToolGroup* bwGroup;
    ToolGroup* advancedGroup;
    ToolGroup* calibrationGroup;

    // Mask mode grouped panels
    ToolGroup* spotGroup;
    ToolGroup* maskingGroup;

    PreviewStrip* exposureStrip_ = nullptr;
    PreviewStrip* colorStrip_ = nullptr;
    PreviewStrip* detailStrip_ = nullptr;
    PreviewStrip* effectsStrip_ = nullptr;
    PreviewStrip* bwStrip_ = nullptr;
    Gtk::Box* quickEditBar_ = nullptr;

    ToolBar* toolBar;
    Gtk::Box* colorPickerRow_;

    Gtk::Image* imgPanelEnd[6];
    Gtk::Box* vbPanelEnd[6];

    // Mode-based UI (replaces notebook)
    Gtk::ScrolledWindow* editPanelSW;
    Gtk::ScrolledWindow* transformPanelSW;
    Gtk::ScrolledWindow* locallabPanelSW;
    Gtk::Box* locallabPanelContainer_;  // outer container for mask panel (for reparenting ToolGroups)

    // Legacy scrolled windows kept for internal compatibility
    std::unique_ptr<Gtk::ScrolledWindow> favoritePanelSW;
    Gtk::ScrolledWindow* exposurePanelSW;
    Gtk::ScrolledWindow* detailsPanelSW;
    Gtk::ScrolledWindow* colorPanelSW;
    Gtk::ScrolledWindow* rawPanelSW;
    Gtk::ScrolledWindow* advancedPanelSW;

    std::vector<MyExpander*> expList;

    bool hasChanged;
    bool batch;

    void addPanel(Gtk::Box* where, FoldableToolPanel* panel, int level = 1);
    void foldThemAll(GdkEventButton* event);
    void updateVScrollbars(bool hide);
    void addfavoritePanel (Gtk::Box* where, FoldableToolPanel* panel, int level = 1);
    void notebookPageChanged(Gtk::Widget* page, guint page_num);
    void updatePanelTools(
        Gtk::Widget *page,
        const std::vector<Glib::ustring> &favorites,
        bool cloneFavoriteTools);
    void modeChanged(EditorMode mode);
    void populateEditPanel();

private:
    EditDataProvider *editDataProvider;
    class ImageArea *imageArea_;
    sigc::connection modeconn;
    bool photoLoadedOnce; // Used to indicated that a photo has been loaded yet
    std::shared_ptr<RTSurface> ornamentSurface;
    EditorMode prevMode;

    // Color tool pagination (orb dots + stack)
    Gtk::Stack* colorToolStack_ = nullptr;
    Gtk::ToggleButton* colorDots_[3] = {};
    bool colorDotBlock_ = false;
    int colorDotActive_ = 0;

    bool maskModeActive_ = false;
    unsigned editGroupRestoreGeneration_ = 0;
    sigc::connection editGroupCollapseConn_;
    bool quickPreviewActive_ = false;
    int quickPreviewVariant_ = -1;
    sigc::connection quickPreviewFinalizeConn_;
    std::unique_ptr<Glib::ThreadPool> quickAutoEditPool_;
    std::shared_ptr<std::atomic<unsigned>> quickAutoEditGeneration_;
    Thumbnail* quickAutoEditThumbnail_ = nullptr;
    bool quickAutoEditCommitPending_ = false;
    rtengine::procparams::ProcParams quickPreviewRestore_;
    rtengine::procparams::ToneCurveParams savedToneCurve_;
    rtengine::procparams::VibranceParams savedVibrance_;
    rtengine::procparams::SharpeningParams savedSharpening_;
    rtengine::procparams::SHParams savedSH_;
    rtengine::procparams::BlackWhiteParams savedBlackWhite_;

    bool bridgeGlobalToSpot(rtengine::procparams::ProcParams* params, const rtengine::ProcEvent& event);
    void loadSpotIntoGlobalTools();
    void updateResetButtons();
    void updateResetButtonsFromBaseline();
    void captureBaseline();
    void buildQuickEditBar();
    void applyQuickEditParams(rtengine::procparams::ProcParams params, const Glib::ustring& descr, bool commit);
    void requestQuickAutoParams(int mode, const Glib::ustring& descr, bool commit);
    rtengine::procparams::ProcParams makeQuickBWParams(int mode) const;
    void beginQuickPreview(const rtengine::procparams::ProcParams& params, const Glib::ustring& descr);
    void endQuickPreview(bool restore);
    rtengine::procparams::ProcParams baselineParams_;
    bool suppressResetUpdate_ = false;

    // Collapsible transform sections (content box + label for programmatic expand)
    Gtk::Box* cropSectionContent_ = nullptr;
    Gtk::Label* cropSectionLabel_ = nullptr;
    Gtk::Button* cropResetBtn_ = nullptr;
    Gtk::Box* perspSectionContent_ = nullptr;
    Gtk::Label* perspSectionLabel_ = nullptr;
    Gtk::Button* perspResetBtn_ = nullptr;
    Gtk::Box* advSectionContent_ = nullptr;
    Gtk::Label* advSectionLabel_ = nullptr;
    void expandTransformSection(Gtk::Box* content, Gtk::Label* label, const Glib::ustring& name);

    // Debounced mask overlay toggling
    sigc::connection hoverMaskDebounce_;
    sigc::connection hoverMaskWatchdog_;
    bool pendingHoverState_ = false;
    bool hoverMaskApplied_ = false;  // tracks what state was last sent to engine
    int hoverPreviewSpot_ = -1;
    int hoverRestoreSpot_ = -1;
    int hoverMissCount_ = 0;  // consecutive watchdog misses before turning off
    void applyHoverMask();
    void turnOffMaskOverlay(bool forceRedraw = false);

public:
    enum class Panel {
        FAVORITE,
        EXPOSURE,
        DETAILS,
        COLOR,
        ADVANCED,
        LOCALLAB,
        TRANSFORM_PANEL,
        RAW,
    };

    enum class Tool {
        TONE_CURVE,
        SHADOWS_HIGHLIGHTS,
        TONE_EQUALIZER,
        IMPULSE_DENOISE,
        AI_DENOISE,
        DEFRINGE_TOOL,
        COMPRESSGAMUT_TOOL,
        SPOT,
        DIR_PYR_DENOISE,
        EPD,
        SHARPENING_TOOL,
        LOCAL_CONTRAST,
        SHARPEN_EDGE,
        SHARPEN_MICRO,
        L_CURVE,
        RGB_CURVES,
        COLOR_TONING,
        COLOR_GRADING,
        LENS_GEOM,
        LENS_PROF,
        DISTORTION,
        ROTATE,
        VIBRANCE,
        COLOR_APPEARANCE,
        WHITE_BALANCE,
        VIGNETTING,
        RETINEX_TOOL,
        GRADIENT,
        LOCALLAB,
        PC_VIGNETTE,
        PERSPECTIVE,
        CA_CORRECTION,
        CH_MIXER,
        BLACK_WHITE,
        RESIZE_TOOL,
        PR_SHARPENING,
        FRAMING,
        CROP_TOOL,
        ICM,
        WAVELET,
        DIR_PYR_EQUALIZER,
        HSV_EQUALIZER,
        POINT_COLOR,
        TEXTURE,
        CLARITY,
        GRAIN,
        TILT_SHIFT,
        LENS_BLUR,
        FILM_PRESETS,
        FILM_SIMULATION,
        SOFT_LIGHT,
        DEHAZE,
        DOUBLE_EXPOSURE,
        SENSOR_BAYER,
        SENSOR_XTRANS,
        BAYER_PROCESS,
        XTRANS_PROCESS,
        BAYER_PREPROCESS,
        PREPROCESS,
        DARKFRAME_TOOL,
        FLATFIELD_TOOL,
        RAW_CA_CORRECTION,
        RAW_EXPOSURE,
        PREPROCESS_WB,
        BAYER_RAW_EXPOSURE,
        XTRANS_RAW_EXPOSURE,
        FATTAL,
        FILM_NEGATIVE,
        PD_SHARPENING,
    };

    struct ToolTree {
        Tool id;
        std::vector<ToolTree> children;
    };

    using ToolLayout = std::unordered_map<Panel, const std::vector<ToolTree> &, ScopedEnumHash>;

    CoarsePanel* coarse;
    Gtk::Notebook* toolPanelNotebook;
    ModeButtonBar* modeButtonBar;
    Gtk::Stack* modeStack;

    ToolPanelCoordinator(bool batch = false);
    ~ToolPanelCoordinator () override;

    static const ToolLayout &getDefaultToolLayout();
    /**
     * Gets the tool with the provided tool name.
     *
     * @param name The tool name as a raw string.
     * @return The tool.
     * @throws std::out_of_range If the name is not recognized.
     */
    static Tool getToolFromName(const std::string &name);
    /**
     * Gets the tool name for the tool's ToolPanel as a string.
     *
     * @param tool The name as a raw string, or an empty string if the tool is
     * unknown.
     */
    static std::string getToolName(Tool tool);
    static bool isFavoritable(Tool tool);

    bool getChangedState()
    {
        return hasChanged;
    }
    void resetChangedState()
    {
        hasChanged = false;
    }
    void updateCurveBackgroundHistogram(
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
    );
    void foldAllButOne(Gtk::Box* parent, FoldableToolPanel* openedSection);
    void applyUIComplexity(int complexityLevel);
    void updateToolLocations(
        const std::vector<Glib::ustring> &favorites, bool cloneFavoriteTools);

    // multiple listeners can be added that are notified on changes (typical: profile panel and the history)
    void addPParamsChangeListener(PParamsChangeListener* pp)
    {
        paramcListeners.push_back(pp);
    }

    // toolpanellistener interface
    void refreshPreview(const rtengine::ProcEvent& event) override;
    void panelChanged(const rtengine::ProcEvent& event, const Glib::ustring& descr) override;
    void setTweakOperator (rtengine::TweakOperator *tOperator) override;
    void unsetTweakOperator (rtengine::TweakOperator *tOperator) override;
    void hoverMaskChanged(bool hover, bool forceRedraw = false, int spotIndex = -1) override;

    // FilmNegProvider interface
    void imageTypeChanged (bool isRaw, bool isBayer, bool isXtrans, bool isMono = false, bool isGainMapSupported = false) override;

    // profilechangelistener interface
    void profileChange(
        const rtengine::procparams::PartialProfile* nparams,
        const rtengine::ProcEvent& event,
        const Glib::ustring& descr,
        const ParamsEdited* paramsEdited = nullptr,
        bool fromLastSave = false
    ) override;
    void setDefaults(const rtengine::procparams::ProcParams* defparams) override;

    // DirSelectionListener interface
    void dirSelected(const Glib::ustring& dirname, const Glib::ustring& openfile);

    // to support the GUI:
    CropGUIListener* getCropGUIListener();  // through the CropGUIListener the editor area can notify the "crop" ToolPanel when the crop selection changes

    // init the toolpanelcoordinator with an image & close it
    void initImage(rtengine::StagedImageProcessor* ipc_, bool israw);
    void closeImage();

    // Forward the browser tab's filter to the double-exposure picker.
    void setDoubleExposureBrowserFilterProvider(std::function<BrowserFilter()> provider);
    void setDoubleExposureBrowserDirProvider(std::function<Glib::ustring()> provider);

    // update the "expanded" state of the Tools
    void updateToolState();
    void openAllTools();
    void closeAllTools();
    // read/write the "expanded" state of the expanders & read/write the crop panel settings (ratio, guide type, etc.)
    void readOptions();
    void writeOptions();
    void writeToolExpandedStatus(std::vector<int> &tpOpen);
    void updateShowtooltipVisibility (bool showtooltip);

    // wbprovider interface
    void getAutoWB (double& temp, double& green, double equal, rtengine::StandardObserver observer, double tempBias) override
    {
        if (ipc) {
            ipc->getAutoWB(temp, green, equal, observer, tempBias);
        }
    }
    void getCamWB (double& temp, double& green, rtengine::StandardObserver observer) override
    {
        if (ipc) {
            ipc->getCamWB(temp, green, observer);
        }
    }

    //DFProvider interface
    const rtengine::RawImage* getDF() override;

    //FFProvider interface
    rtengine::RawImage* getFF() override;
    Glib::ustring GetCurrentImageFilePath() override;

    // FilmNegProvider interface
    bool getFilmNegativeSpot(rtengine::Coord spot, int spotSize, RGB &refInput, RGB &refOutput) override;

    // rotatelistener interface
    void straightenRequested () override;
    bool autoLevelRequested (double& correction) override;
    void autoCropRequested () override;
    void autoPerspRequested (bool corr_pitch, bool corr_yaw, double& rot, double& pitch, double& yaw, const std::vector<rtengine::ControlLine> *lines = nullptr) override;
    double autoDistorRequested () override;

    // spotwblistener interface
    void spotWBRequested (int size) override;

    // pointcolorpicklistener interface
    void pointColorPickRequested() override;

    // croppanellistener interface
    void cropSelectRequested () override;

    // PerspCorrectionPanelListener interface
    void controlLineEditModeChanged(bool active) override;

    // icmpanellistener interface
    void saveInputICCReference(const Glib::ustring& fname, bool apply_wb) override;

    // imageareatoollistener interface
    void spotWBselected(int x, int y, Thumbnail* thm = nullptr) override;
    void pointColorSelected(int x, int y, Thumbnail* thm = nullptr) override;
    void sharpMaskSelected(bool sharpMask) override final;
    int getSpotWBRectSize() const override;
    void cropSelectionReady() override;
    void rotateSelectionReady(double rotate_deg, Thumbnail* thm = nullptr) override;
    ToolBar* getToolBar() const final;
    CropGUIListener* startCropEditing(Thumbnail* thm = nullptr) override;

    void updateTPVScrollbar(bool hide);
    bool handleShortcutKey(GdkEventKey* event);

    // ToolBarListener interface
    void toolDeselected(ToolMode tool) override;
    void toolSelected (ToolMode tool) override;
    void editModeSwitchedOff () final;

    void setEditProvider(EditDataProvider *provider);
    void setLevelingGridCallback(std::function<void(bool)> cb);

    void setThumbnail(Thumbnail* thm);

    void setProgressListener(rtengine::ProgressListener *pl);

protected:
    static std::unordered_map<std::string, Tool> toolNamesReverseMap;

    std::unordered_map<Tool, const ToolTree *, ScopedEnumHash>
        toolToDefaultToolTreeMap;

    FoldableToolPanel *getFoldableToolPanel(Tool tool) const;
    FoldableToolPanel *getFoldableToolPanel(const ToolTree &tool) const;
    void updateFavoritesPanel(
        const std::vector<Glib::ustring> &favorites, bool cloneFavoriteTools);
    template <typename T>
    typename std::enable_if<std::is_convertible<T, const ToolTree>::value, void>::type
    updateToolPanel(
        Gtk::Box *panelBox,
        const std::vector<T> &children,
        int level,
        const std::unordered_set<Tool, ScopedEnumHash> &favorites,
        bool cloneFavoriteTools);

private:
    void panelChangedFromPreviewStrip(
        PreviewStrip* source,
        const rtengine::ProcEvent& event,
        const Glib::ustring& descr);
    void deferPanelChanged(const rtengine::ProcEvent& event, const Glib::ustring& descr);
    bool retryDeferredPanelChanged();

    bool deferredPanelChangePending_ = false;
    bool deferredPanelChangeTimerActive_ = false;
    rtengine::ProcEvent deferredPanelChangeEvent_;
    Glib::ustring deferredPanelChangeDescr_;
    sigc::connection deferredPanelChangeConn_;
    PreviewStrip* previewStripChangeSource_ = nullptr;
    PreviewStrip* deferredPreviewStripChangeSource_ = nullptr;
    unsigned deferredMetadataReadGeneration_ = 0;

    IdleRegister idle_register;
};
