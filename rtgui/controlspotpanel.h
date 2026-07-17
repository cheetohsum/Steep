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

#ifndef _CONTROLSPOTPANEL_H_
#define _CONTROLSPOTPANEL_H_

#include <memory>
#include <vector>

#include "rtengine/coord.h"
#include "editcallbacks.h"
#include "threadutils.h"
#include "toolpanel.h"
#include "widgets/basic/adjuster.h"
#include "widgets/basic/popupbutton.h"

class ControlPanelListener
{
public:
    ControlPanelListener() {};
    virtual ~ControlPanelListener() {};

    virtual void resetToolMaskView() = 0;
    virtual void spotNameChanged(const Glib::ustring &newName) = 0;
    virtual void spotHovered(bool hovered, bool forceRedraw = false) = 0;
};


class ControlSpotPanel:
    public ToolParamBlock,
    public AdjusterListener,
    public EditSubscriber,
    public FoldableToolPanel
{
public:
    /**
     * A SpotRow structure allows exchanges from and to ControlSpotClass
     */
    static constexpr int GEOM_PER_SPOT = 11;

    struct SpotRow {
        Glib::ustring name;
        bool isvisible;
        int prevMethod; // 0 = Normal, 1 = Excluding
        int shape; // 0 = Ellipse, 1 = Rectangle, 2 = Gradient, 3 = Polygon (Lasso)
        int spotMethod; // 0 = Normal, 1 = Excluding  2 = fullimage 3 = main
        int sensiexclu;
        int structexclu;
        int shapeMethod; // 0 = Independent (mouse), 1 = Symmetrical (mouse), 2 = Independent (mouse + sliders), 3 = Symmetrical (mouse + sliders)
        int avoidgamutMethod;
        int locX;
        int locXL;
        int locY;
        int locYT;
        int centerX;
        int centerY;
        int circrad;
        int qualityMethod; // 0 = Standard, 1 = Enhanced, 2 = Enhanced + chroma denoise
        double transit;
        double transitweak;
        double transitgrad;
        double gradangle;
        double feather;
        double struc;
        double thresh;
        double iter;
        double balan;
        double balanh;
        double colorde;
        double colorscope;
        double avoidrad;
        bool hishow;
        bool activ;
        bool avoidneg;
        bool blwh;
        bool recurs;
        bool laplac;
        bool deltae;
        int scopemask;
        double denoichmask;
        bool shortc;
        int lumask;
        //bool savrest;
        int complexMethod; // 0 = Simple, 1 = Moderate, 2 = all
        int wavMethod; // 0 = D2, 1 = D4, 2 = D6, 3 = D10, 4 = D14
        int maskType; // 0 = Normal, 1 = AI Mask
        int aiMaskClass; // 0-7 class index
        std::vector<int> polyMaskPoints; // Polygon vertices: flattened [x1,y1,x2,y2,...]
        double polyMaskFeather; // Polygon feather width
        double polyMaskSnapTolerance; // Magnetic snap search radius
        double polyMaskLegLength; // Min distance between freehand points
    };

    /**
     * An event type enumeration allows exchanges of spot panel event type from and to ControlSpotClass
     */
    enum eventType {
        None = 0,
        SpotCreation = 1,
        SpotDeletion = 2,
        SpotSelection = 3,
        SpotDuplication = 4,
        SpotAllVisibilityChanged = 5,
        SpotCreationAI = 6
    };
    IdleRegister idle_register;

    // Constructor and management functions
    /**
     * Default constructor of ControlSpotPanel class
     */
    ControlSpotPanel();
    /**
     * Destructor of ControlSpotPanel class
     */
    ~ControlSpotPanel();
    /**
     * Implementation of setEditProvider function of toolpanel.h
     *
     * @param provider The EditDataProvider to be linked to the panel to manage curves
     */
    void setEditProvider(EditDataProvider* provider) override;
    /**
     * Setter for controlPanelListener
     *
     * @param cpl The ControlPanelListener to be linked to the panel
     */
    void setControlPanelListener(ControlPanelListener* cpl)
    {
        controlPanelListener = cpl;
    }
    /**
     * Getter of the event type raised by this panel
     *
     * @return The raised event type (refer to eventType enumeration)
     */
    int getEventType();
    /**
     * Getter of params of associated spot
     *
     * @param index The spot index to get params
     * @return A SpotRow structure containing params of associated spot
     */
    std::unique_ptr<SpotRow> getSpot(const int index);
    /**
     * Getter of spots number
     *
     * @return The number of spots in panel
     */
    int getSpotNumber();
    /**
     * Getter of selected spot index
     *
     * @return The index of selected spot in treeview (return -1 if no selected spot)
     */
    int getSelectedSpot();
    /**
     * Setter of selected spot
     *
     * @param index The index of spot to be selected
     * @return True if a spot corresponding to the index has been selected
     */
    bool setSelectedSpot(const int index);
    /**
     * Setter for mask preview active indicator
     *
     * @param ind True is mask preview is active
     */
    void setMaskPrevActive(bool ind)
    {
        maskPrevActive = ind;
    }
    /**
     * Getter for deltaE preview active
     *
     * @return True if preview deltaE is active
     */
    bool isDeltaEPrevActive();
    /**
     * Reset deltaE preview active state
     */
    void resetDeltaEPreview();
    /**
     * Reset hover/eye pin state (called on image switch, spot change, etc.)
     */
    void resetHoverState()
    {
        eyePinned_ = false;
        sidebarHoverActive_ = false;
        hoveredSpotIndex_ = -1;
    }

    int getPendingAIClass() const { return pendingAIClass_; }
    int getPendingShape() const { return pendingShape_; }
    Gtk::Button* getAddMaskButton() const { return button_add_; }

    // Control spot creation functions
    /**
     * Add a new spot (and its associated curve)
     *
     * @param newSpot A SpotRow structure containing new spot params
     */
    void addControlSpot(const SpotRow &newSpot);

    // Control spot delete function
    /**
     * Delete a spot (and its associated curve)
     *
     * @param id The id of the spot to be deleted
     */
    void deleteControlSpot(const int index);

    // Panel widgets management functions
    /**
     * Implementation of setDefaults function of toolpanel.h
     *
     * @param defParams ProcParams containing default values to set to the adjusters
     * @param pedited ParamsEdited containing default state values to set to the adjusters (not used because batch mode is deactivated for Locallab)
     */
    void setDefaults(const rtengine::procparams::ProcParams* defParams, const ParamsEdited* pedited = nullptr) override;
    /**
     * Enable or disable the interactions with panel widgets
     *
     * @param cond Condition to enable interactions
     */
    void setParamEditable(bool cond);
    /**
     * Reset expander collapse state to default one
     */
    void setDefaultExpanderVisibility();

    // Batch mode management
    // Note: Batch mode is deactivated for Locallab
    
    /**
     * upadte function to work with Preferences and spotMethod
    */
    void updateguiset(int spottype, bool iscolor,  bool issh, bool isvib, bool isexpos, bool issoft, bool isblur, bool istom, bool isret, bool issharp, bool iscont, bool iscbdl, bool islog, bool ismas, bool isci);
    void updateguiscopeset(int scope);

private:
    // Cell renderer
    void render_details_toggle(Gtk::CellRenderer* cell, const Gtk::TreeModel::iterator& iter);
    void render_preview(Gtk::CellRenderer* cell, const Gtk::TreeModel::iterator& iter);
    void render_name(Gtk::CellRenderer* cell, const Gtk::TreeModel::iterator& iter);
    void render_isvisible(Gtk::CellRenderer* cell, const Gtk::TreeModel::iterator& iter);

    void on_button_add();
    void on_mask_shape_selected(int shape);
    void on_button_delete();
    void on_button_duplicate();
    void on_button_rename();
    bool on_button_visibility(GdkEventButton* event);
    void on_ai_mask_selected(int classIndex);

    bool blockTreeviewSearch(GdkEventKey* event);
    bool onSpotSelectionEvent(GdkEventButton* event);

    void load_ControlSpot_param();

    void controlspotChanged();

    void prevMethodChanged();
    void shapeChanged(int index);
    void polyDrawClicked();
    void polyFeatherChanged(Adjuster* a, double newval);
    void spotMethodChanged();
    void shapeMethodChanged();
    void qualityMethodChanged();
    void avoidgamutMethodChanged();
   //void complexMethodChanged();
    void wavMethodChanged();
    void maskTypeChanged(int index);
    void aiMaskClassChanged(int index);

    void updateParamVisibility();

    void adjusterChanged(Adjuster* a, double newval) override;

    void hishowChanged();
    void activChanged();
    void avoidnegChanged();
    void blwhChanged();
    void recursChanged();
    void laplacChanged();
    void deltaeChanged();
    void shortcChanged();
    //void savrestChanged();

    void previewChanged();

    void disableParamlistener(bool cond);

    void addControlSpotCurve(Gtk::TreeModel::Row& row);
    void updateControlSpotCurve(const Gtk::TreeModel::Row& row);
    void deleteControlSpotCurve(Gtk::TreeModel::Row& row);
    void updateCurveOpacity(const Gtk::TreeModel::Row& selectedRow);
    CursorShape getCursor(int objectID, int xPos, int yPos) const override;
    bool mouseOver(int modifierKey) override;
    bool button1Pressed(int modifierKey) override;
    bool button1Released() override;
    bool drag1(int modifierKey) override;

    using ToolPanel::setDefaults;

    class ControlSpots:
        public Gtk::TreeModel::ColumnRecord
    {
    public:
        ControlSpots();

        Gtk::TreeModelColumn<bool> mouseover; // Used to manage spot enlightening when mouse over
        Gtk::TreeModelColumn<bool> detailsExpanded;
        Gtk::TreeModelColumn<Glib::ustring> name;
        Gtk::TreeModelColumn<bool> isvisible;
        Gtk::TreeModelColumn<int> curveid; // Associated curve id
        Gtk::TreeModelColumn<int> prevMethod; // 0 = hide, 1 = show
        Gtk::TreeModelColumn<int> shape; // 0 = Ellipse, 1 = Rectangle
        Gtk::TreeModelColumn<int> spotMethod; // 0 = Normal, 1 = Excluding
        Gtk::TreeModelColumn<int> sensiexclu;
        Gtk::TreeModelColumn<int> structexclu;
        Gtk::TreeModelColumn<int> shapeMethod; // 0 = Independent (mouse), 1 = Symmetrical (mouse), 2 = Independent (mouse + sliders), 3 = Symmetrical (mouse + sliders)
        Gtk::TreeModelColumn<int> avoidgamutMethod;
        Gtk::TreeModelColumn<int> locX;
        Gtk::TreeModelColumn<int> locXL;
        Gtk::TreeModelColumn<int> locY;
        Gtk::TreeModelColumn<int> locYT;
        Gtk::TreeModelColumn<int> centerX;
        Gtk::TreeModelColumn<int> centerY;
        Gtk::TreeModelColumn<int> circrad;
        Gtk::TreeModelColumn<int> qualityMethod; // 0 = Standard, 1 = Enhanced, 2 = Enhanced + chroma denoise
        Gtk::TreeModelColumn<double> transit;
        Gtk::TreeModelColumn<double> transitweak;
        Gtk::TreeModelColumn<double> transitgrad;
        Gtk::TreeModelColumn<double> gradangle;
        Gtk::TreeModelColumn<double> feather;
        Gtk::TreeModelColumn<double> struc;
        Gtk::TreeModelColumn<double> thresh;
        Gtk::TreeModelColumn<double> iter;
        Gtk::TreeModelColumn<double> balan;
        Gtk::TreeModelColumn<double> balanh;
        Gtk::TreeModelColumn<double> colorde;
        Gtk::TreeModelColumn<double> colorscope;
        Gtk::TreeModelColumn<double> avoidrad;
        Gtk::TreeModelColumn<bool> hishow;
        Gtk::TreeModelColumn<bool> activ;
        Gtk::TreeModelColumn<bool> avoidneg;
        Gtk::TreeModelColumn<bool> blwh;
        Gtk::TreeModelColumn<bool> recurs;
        Gtk::TreeModelColumn<bool> laplac;
        Gtk::TreeModelColumn<bool> deltae;
        Gtk::TreeModelColumn<int> scopemask;
        Gtk::TreeModelColumn<int> denoichmask;
        Gtk::TreeModelColumn<bool> shortc;
        Gtk::TreeModelColumn<int> lumask;
        //Gtk::TreeModelColumn<bool> savrest;
        Gtk::TreeModelColumn<int> complexMethod; // 0 = Simple, 1 = mod, 2 = all
        Gtk::TreeModelColumn<int> wavMethod; // 0 = D2, 1 = D4, 2 = D6, 3 = D10, 4 = D14
        Gtk::TreeModelColumn<int> maskType; // 0 = Normal, 1 = AI Mask
        Gtk::TreeModelColumn<int> aiMaskClass; // 0-7 class index
        Gtk::TreeModelColumn<std::vector<int>> polyMaskPoints;
        Gtk::TreeModelColumn<double> polyMaskFeather;
        Gtk::TreeModelColumn<double> polyMaskSnapTolerance;
        Gtk::TreeModelColumn<double> polyMaskLegLength;
    };

    class RenameDialog:
        public Gtk::Dialog
    {
    public:
        enum DialogButton {
            OkButton = 1,
            CancelButton = 2
        };

        RenameDialog(const Glib::ustring &actualname, Gtk::Window &parent);
        Glib::ustring get_new_name();

    private:
        Gtk::Entry* const newname_;
    };

    ControlSpots spots_;
    rtengine::ProcEvent EvLocallabavoidgamutMethod;
    rtengine::ProcEvent EvLocallabavoidnegative;

    // Child widgets
    Gtk::ScrolledWindow* const scrolledwindow_;
    Gtk::TreeView* const treeview_;
    sigc::connection treeviewconn_;
    Glib::RefPtr<Gtk::ListStore> treemodel_;
    Gtk::Menu* contextMenu_;

    Gtk::Button* const button_add_;
    sigc::connection buttonaddconn_;
    Gtk::Button* const button_delete_;
    sigc::connection buttondeleteconn_;
    Gtk::Button* const button_duplicate_;
    sigc::connection buttonduplicateconn_;

    Gtk::Button* const button_rename_;
    sigc::connection buttonrenameconn_;
    Gtk::Button* const button_visibility_;
    sigc::connection buttonvisibilityconn_;


    MyComboBoxText* const prevMethod_;
    sigc::connection prevMethodconn_;
    PopUpButton* const shape_;
    sigc::connection shapeconn_;
    MyComboBoxText* const spotMethod_;
    sigc::connection spotMethodconn_;
    MyComboBoxText* const shapeMethod_;
    sigc::connection shapeMethodconn_;
    MyComboBoxText* const qualityMethod_;
    sigc::connection qualityMethodconn_;
    //MyComboBoxText* const complexMethod_;
    //sigc::connection complexMethodconn_;
    MyComboBoxText* const wavMethod_;
    sigc::connection wavMethodconn_;
    MyComboBoxText* const avoidgamutMethod_;
	sigc::connection avoidgamutconn_;
    PopUpButton* const maskType_;
    sigc::connection maskTypeConn_;
    PopUpButton* const aiMaskClass_;
    sigc::connection aiMaskClassConn_;
    sigc::connection aiPreviewRefresh_; // delayed treeview redraw after AI mask computation

    Adjuster* const sensiexclu_;
    Adjuster* const structexclu_;
    Adjuster* const locX_;
    Adjuster* const locXL_;
    Adjuster* const locY_;
    Adjuster* const locYT_;
    Adjuster* const centerX_;
    Adjuster* const centerY_;
    Adjuster* const circrad_;
    Adjuster* const transit_;
    Adjuster* const transitweak_;
    Adjuster* const transitgrad_;
    Adjuster* const gradangle_;
    Adjuster* const feather_;
    Adjuster* const struc_;
    Adjuster* const thresh_;
    Adjuster* const iter_;
    Adjuster* const balan_;
    Adjuster* const balanh_;
    Adjuster* const colorde_;
    Adjuster* const colorscope_;
    Adjuster* const avoidrad_;
    Adjuster* const scopemask_;
    Adjuster* const denoichmask_;
    Adjuster* const lumask_;

    Gtk::CheckButton* const hishow_;
    sigc::connection hishowconn_;
    Gtk::CheckButton* const activ_;
    sigc::connection activConn_;
    Gtk::CheckButton* const avoidneg_;
    sigc::connection avoidnegConn_;
    Gtk::CheckButton* const blwh_;
    sigc::connection blwhConn_;
    Gtk::CheckButton* const recurs_;
    sigc::connection recursConn_;
    Gtk::CheckButton* const laplac_;
    sigc::connection laplacConn_;
    Gtk::CheckButton* const deltae_;
    sigc::connection deltaeConn_;
    Gtk::CheckButton* const shortc_;
    sigc::connection shortcConn_;
    //Gtk::CheckButton* const savrest_;
    //sigc::connection savrestConn_;

    MyExpander* const expTransGrad_;
    MyExpander* const expShapeDetect_;
    MyExpander* const expSpecCases_;
    MyExpander* const expMaskMerge_;
    MyExpander* const expAdvanced_;

    Gtk::ToggleButton* const preview_;
    sigc::connection previewConn_;

    Gtk::Box* const ctboxshape;
    Gtk::Box* const ctboxactivmethod;
    Gtk::Box* const ctboxspotmethod;
    
    Gtk::Box* const ctboxshapemethod;
    Gtk::Box* const ctboxgamut;
    Gtk::Box* const ctboxmasktype;
    Gtk::Box* const ctboxaiclass;
    ToolParamBlock* const artifBox2;

    // Internal variables
    ControlPanelListener* controlPanelListener;
    int lastObject_;
    rtengine::Coord lastCoord_;
    bool nbSpotChanged_;
    bool selSpotChanged_;
    bool nameChanged_;
    bool visibilityChanged_;
    int eventType; // 0 = No event, 1 = Spot creation event, 2 = Spot deletion event, 3 = Spot selection event, 4 = Spot duplication event
    Gtk::Frame* const excluFrame;
    bool maskPrevActive;

    // Backup of ellipse/rect dimensions before switching to gradient
    double savedLocX_ = 150., savedLocXL_ = 150.;
    double savedLocY_ = 150., savedLocYT_ = 150.;
    double savedTransit_ = 60.;
    bool hasSavedDims_ = false;

    // Polygon (Lasso) drawing state
    bool polyDrawing_ = false;   // draw mode enabled (button toggled)
    bool polyDragging_ = false;  // currently in a freehand drag gesture
    std::vector<rtengine::Coord> polyTempPoints_; // vertices being drawn
    Gtk::ToggleButton* polyDrawBtn_;
    sigc::connection polyDrawConn_;
    Adjuster* const polyFeather_;
    Adjuster* const polySnapTol_;
    Adjuster* const polyLegLen_;
    Gtk::Label* polyVertexLabel_;
    RTImage* polyVertexIcon_;
    Gtk::Box* polyBox_; // container for polygon-specific controls
    Gtk::Box* shapeTypeRow_; // row with shape/masktype buttons + poly controls
    rtengine::Coord magneticSnap(int imgX, int imgY); // snap to nearest edge
    static void simplifyPolygon(std::vector<rtengine::Coord>& pts, double epsilon);

    // Per-mask settings section, expanded from the chevron in each mask row
    Gtk::Box* maskDetailBox_;
    Gtk::Revealer* maskRevealer_;
    bool maskDetailExpanded_;
    void setMaskDetailExpanded(bool expanded);
    void setMaskControlsSensitive(bool sensitive);
    void queueMaskPreviewRefresh();
    void startAIPreviewRefresh();

    // Sidebar hover mask overlay
    bool sidebarHoverActive_ = false;
    int hoveredSpotIndex_ = -1;
    bool eyePinned_ = false;  // true = eye clicked to pin overlay on, stays on even without hover
    bool onTreeviewMotion(GdkEventMotion* event);
    bool onTreeviewLeave(GdkEventCrossing* event);
public:
    bool isPointerOverTreeview() const;
    void resetSidebarHover();
private:

    // Row background color
    Gdk::RGBA colorMouseover, colorNominal, colorMouseovertext;

    // Add-mask menu
    Gtk::Menu* addMaskMenu_;
    int pendingShape_ = 0;
    int pendingAIClass_ = -1;
    sigc::connection previewRefresh_;
    int aiPreviewAttempts_ = 0;

    // Treeview mutex
    MyMutex mTreeview;
};

#endif // _CONTROLSPOTPANEL_H_
