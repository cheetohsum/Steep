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

#include <functional>

#include <gtkmm.h>

#include "cropguilistener.h"
#include "cropwindow.h"
#include "editcallbacks.h"
#include "editenums.h"
#include "imageareapanel.h"
#include "imageareatoollistener.h"
#include "indclippedpanel.h"
#include "previewhandler.h"
#include "previewmodepanel.h"
#include "toolbar.h"
#include "zoompanel.h"

class ImageAreaPanel;

class ImageArea final :
    public Gtk::DrawingArea,
    public CropWindowListener,
    public EditDataProvider,
    public LockablePickerToolListener
{

    friend class ZoomPanel;

protected:

    Glib::ustring infotext;
    Glib::RefPtr<Pango::Layout> deglayout;
    BackBuffer iBackBuffer;
    int backBufferDeviceScale;
    int infoMouseX_, infoMouseY_; // last mouse pos for info overlay
    bool showClippedH, showClippedS;

    ImageAreaPanel* parent;

    std::list<CropWindow*> cropWins;
    PreviewHandler* previewHandler;
    rtengine::StagedImageProcessor* ipc;

    bool        dirty;
    CropWindow* focusGrabber;
    CropGUIListener* cropgl;
    PointerMotionListener* pmlistener;
    PointerMotionListener* pmhlistener;
    ImageAreaToolListener* listener;
    bool quickPreviewFit_;
    bool cropPreviewSolid_ = false;  // clip the preview to the crop instead of dimming around it
    std::function<bool()> beforeLockGet_;
    std::function<void(bool)> beforeLockSet_;

    CropWindow* getCropWindow (int x, int y);
    Gtk::SizeRequestMode get_request_mode_vfunc () const override;
    void get_preferred_height_vfunc (int &minimum_height, int &natural_height) const override;
    void get_preferred_width_vfunc (int &minimum_width, int &natural_width) const override;
    void get_preferred_height_for_width_vfunc (int width, int &minimum_height, int &natural_height) const override;
    void get_preferred_width_for_height_vfunc (int height, int &minimum_width, int &natural_width) const override;

    int fullImageWidth, fullImageHeight;
public:
    CropWindow* mainCropWindow;
    CropWindow* flawnOverWindow;
    ZoomPanel* zoomPanel;
    IndicateClippedPanel* indClippedPanel;
    PreviewModePanel* previewModePanel;
    ImageArea* iLinkedImageArea; // link to the counterpart view while before/after is enabled
    bool isBeforeView = false;   // true for the 'before' (left) pane of before/after
    // True while the before/after split is showing. Unlike iLinkedImageArea
    // (nulled during a content-only refresh on image switch), this persists,
    // so snug positioning doesn't bounce the after pane to center and back.
    bool inBeforeAfterSplit = false;
    bool suppressZoomSync_ = false; // prevents zoom sync recursion between before/after views

    // Right-click context menu for the before/after view
    void showBeforeAfterContextMenu ();
    // Lets the editor panel expose its before-view lock in that menu
    void setBeforeLockHandlers (const std::function<bool()>& get,
                                const std::function<void(bool)>& set)
    {
        beforeLockGet_ = get;
        beforeLockSet_ = set;
    }

    explicit ImageArea (ImageAreaPanel* p);
    ~ImageArea () override;

    rtengine::StagedImageProcessor* getImProcCoordinator() const;
    void setImProcCoordinator(rtengine::StagedImageProcessor* ipc_);
    void setPreviewModePanel(PreviewModePanel* previewModePanel_)
    {
        previewModePanel = previewModePanel_;
    };
    void setIndicateClippedPanel(IndicateClippedPanel* indClippedPanel_)
    {
        indClippedPanel = indClippedPanel_;
    };

    void getScrollImageSize (int& w, int& h);
    void getScrollPosition  (int& x, int& y);
    void setScrollPosition  (int x, int y);     // called by the imageareapanel when the scrollbars have been changed

    // enabling and setting text of info area
    void setInfoText (Glib::ustring&& text);
    void updateInfoTextBackBuffer();
    void infoEnabled (bool e);

    // widget base events
    void on_realize () override;
    bool on_draw                 (const ::Cairo::RefPtr< Cairo::Context> &cr) override;
    bool on_motion_notify_event  (GdkEventMotion* event) override;
    bool on_button_press_event   (GdkEventButton* event) override;
    bool on_button_release_event (GdkEventButton* event) override;
    bool on_scroll_event         (GdkEventScroll* event) override;
    bool on_leave_notify_event   (GdkEventCrossing* event) override;
    void on_resized              (Gtk::Allocation& req);
    void on_style_updated        () override;
    void syncBeforeAfterViews    ();

    void            setCropGUIListener       (CropGUIListener* l);
    void            setPointerMotionListener  (PointerMotionListener* pml);
    void            setPointerMotionHListener (PointerMotionListener* pml);
    void            setImageAreaToolListener (ImageAreaToolListener* l)
    {
        listener = l;
    }
    void            setPreviewHandler        (PreviewHandler* ph);
    void            setQuickPreviewFit       (bool enabled);
    bool            getQuickPreviewFit       () const { return quickPreviewFit_; }
    PreviewHandler* getPreviewHandler        ()
    {
        return previewHandler;
    }

    void grabFocus          (CropWindow* cw);
    void unGrabFocus        ();
    void addCropWindow      ();
    void cropWindowSelected (CropWindow* cw);
    void cropWindowClosed   (CropWindow* cw);
    ToolMode getToolMode    ();
    bool showColorPickers   ();
    void setToolHand        ();
    void straightenReady    (double rotDeg);
    void spotWBSelected     (int x, int y);
    void pointColorSelected (int x, int y);
    void sharpMaskSelected  (bool sharpMask);
    int  getSpotWBRectSize  ();
    void redraw             ();
    void setCropPreviewMode (bool editingCrop);
    void setCropPreviewSolid (bool solid, bool refit = true);
    bool isCropPreviewSolid () const
    {
        return cropPreviewSolid_;
    }

    void zoomFit     ();
    double getZoom   ();
    void   setZoom   (double zoom);

    // EditDataProvider interface
    void subscribe(EditSubscriber *subscriber) override;
    void unsubscribe() override;
    void getImageSize (int &w, int&h) override;
    void getPreviewCenterPos(int &x, int &y) override;
    void getPreviewSize(int &w, int &h) override;

    // CropWindowListener interface
    void cropPositionChanged   (CropWindow* cw) override;
    void cropWindowSizeChanged (CropWindow* cw) override;
    void cropZoomChanged       (CropWindow* cw) override;
    void initialImageArrived   () override;

    // LockablePickerToolListener interface
    void switchPickerVisibility (bool isVisible) override;

    CropWindow* getMainCropWindow ()
    {
        return mainCropWindow;
    }
};
