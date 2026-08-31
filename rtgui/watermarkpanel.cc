/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2024
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
#include "watermarkpanel.h"
#include "multilangmgr.h"
#include "watermarkrenderer.h"

#include "rtengine/rt_math.h"

#include <cairo.h>
#include <pango/pangocairo.h>

WatermarkPanel::WatermarkPanel() : previewWindow(nullptr)
{
    set_column_spacing(4);
    set_row_spacing(2);

    int row = 0;

    // Helper to create a label right-justified, centered vertically
    auto makeLabel = [](const Glib::ustring& text) -> Gtk::Label* {
        auto* l = Gtk::manage(new Gtk::Label(text));
        setExpandAlignProperties(l, true, false, Gtk::ALIGN_END, Gtk::ALIGN_CENTER);
        l->set_margin_right(4);
        return l;
    };

    // Enable checkbox
    enableChk = Gtk::manage(new Gtk::CheckButton(M("WATERMARK_ENABLE")));
    enableChk->signal_toggled().connect(sigc::mem_fun(*this, &WatermarkPanel::updateSensitivity));
    attach(*enableChk, 0, row, 2, 1);
    row++;

    // Text entry
    textEntry = Gtk::manage(new Gtk::Entry());
    textEntry->set_tooltip_text(M("WATERMARK_TEXT_TOOLTIP"));
    setExpandAlignProperties(textEntry, true, false, Gtk::ALIGN_FILL, Gtk::ALIGN_CENTER);
    attach(*makeLabel(M("WATERMARK_TEXT")), 0, row, 1, 1);
    attach(*textEntry, 1, row, 1, 1);
    row++;

    // Font button
    fontButton = Gtk::manage(new Gtk::FontButton());
    setExpandAlignProperties(fontButton, true, false, Gtk::ALIGN_FILL, Gtk::ALIGN_CENTER);
    attach(*makeLabel(M("WATERMARK_FONT")), 0, row, 1, 1);
    attach(*fontButton, 1, row, 1, 1);
    row++;

    // Size mode
    sizeMode = Gtk::manage(new MyComboBoxText());
    sizeMode->append(M("WATERMARK_SIZEMODE_FIXED"));
    sizeMode->append(M("WATERMARK_SIZEMODE_PERCENT"));
    sizeMode->set_active(1);
    sizeMode->signal_changed().connect(sigc::mem_fun(*this, &WatermarkPanel::sizeModeChanged));
    setExpandAlignProperties(sizeMode, true, false, Gtk::ALIGN_FILL, Gtk::ALIGN_CENTER);
    attach(*makeLabel(M("WATERMARK_SIZEMODE")), 0, row, 1, 1);
    attach(*sizeMode, 1, row, 1, 1);
    row++;

    // Font size (pt)
    fontSizeAdj = Gtk::manage(new Adjuster(M("WATERMARK_FONTSIZE"), 6, 500, 1, 48));
    fontSizeAdj->setAdjusterListener(this);
    attach(*fontSizeAdj, 0, row, 2, 1);
    row++;

    // Size percent
    sizePercentAdj = Gtk::manage(new Adjuster(M("WATERMARK_SIZEPERCENT"), 0.5, 50.0, 0.1, 3.0));
    sizePercentAdj->setAdjusterListener(this);
    attach(*sizePercentAdj, 0, row, 2, 1);
    row++;

    // Text color
    textColorBtn = Gtk::manage(new Gtk::ColorButton());
    textColorBtn->set_use_alpha(true);
    setExpandAlignProperties(textColorBtn, true, false, Gtk::ALIGN_FILL, Gtk::ALIGN_CENTER);
    attach(*makeLabel(M("WATERMARK_COLOR")), 0, row, 1, 1);
    attach(*textColorBtn, 1, row, 1, 1);
    row++;

    // Opacity
    opacityAdj = Gtk::manage(new Adjuster(M("WATERMARK_OPACITY"), 0, 100, 1, 100));
    opacityAdj->setAdjusterListener(this);
    attach(*opacityAdj, 0, row, 2, 1);
    row++;

    // --- Logo / photo section ---
    // Sits above stroke and shadow because those apply to everything here.
    imageEnableChk = Gtk::manage(new Gtk::CheckButton(M("WATERMARK_IMAGE_ENABLE")));
    imageEnableChk->set_tooltip_text(M("WATERMARK_IMAGE_ENABLE_TOOLTIP"));
    imageEnableChk->signal_toggled().connect(sigc::mem_fun(*this, &WatermarkPanel::updateSensitivity));
    attach(*imageEnableChk, 0, row, 2, 1);
    row++;

    {
        Gtk::Box* imageRow = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 2));
        imageChooser = Gtk::manage(new Gtk::FileChooserButton(M("WATERMARK_IMAGE_CHOOSE"), Gtk::FILE_CHOOSER_ACTION_OPEN));

        auto imageFilter = Gtk::FileFilter::create();
        imageFilter->set_name(M("WATERMARK_IMAGE_FILTER"));
        imageFilter->add_pattern("*.png");
        imageFilter->add_pattern("*.jpg");
        imageFilter->add_pattern("*.jpeg");
        imageFilter->add_pattern("*.tif");
        imageFilter->add_pattern("*.tiff");
        imageFilter->add_pattern("*.bmp");
        imageChooser->add_filter(imageFilter);
        setExpandAlignProperties(imageChooser, true, false, Gtk::ALIGN_FILL, Gtk::ALIGN_CENTER);
        imageRow->pack_start(*imageChooser, Gtk::PACK_EXPAND_WIDGET);

        imageClearBtn = Gtk::manage(new Gtk::Button("\xc3\x97"));
        imageClearBtn->set_relief(Gtk::RELIEF_NONE);
        imageClearBtn->set_tooltip_text(M("WATERMARK_IMAGE_CLEAR"));
        imageClearBtn->signal_clicked().connect([this]() {
            imageChooser->unselect_all();
            imageChooser->set_filename("");
        });
        imageRow->pack_start(*imageClearBtn, Gtk::PACK_SHRINK);

        attach(*makeLabel(M("WATERMARK_IMAGE_FILE")), 0, row, 1, 1);
        attach(*imageRow, 1, row, 1, 1);
        row++;
    }

    imagePlacementCombo = Gtk::manage(new MyComboBoxText());
    imagePlacementCombo->append(M("WATERMARK_IMAGE_PLACE_LEFT"));
    imagePlacementCombo->append(M("WATERMARK_IMAGE_PLACE_RIGHT"));
    imagePlacementCombo->append(M("WATERMARK_IMAGE_PLACE_ABOVE"));
    imagePlacementCombo->append(M("WATERMARK_IMAGE_PLACE_BELOW"));
    imagePlacementCombo->set_active(0);
    imagePlacementCombo->signal_changed().connect(sigc::mem_fun(*this, &WatermarkPanel::imagePlacementChanged));
    setExpandAlignProperties(imagePlacementCombo, true, false, Gtk::ALIGN_FILL, Gtk::ALIGN_CENTER);
    attach(*makeLabel(M("WATERMARK_IMAGE_PLACEMENT")), 0, row, 1, 1);
    attach(*imagePlacementCombo, 1, row, 1, 1);
    row++;

    imageSizeAdj = Gtk::manage(new Adjuster(M("WATERMARK_IMAGE_SIZE"), 0.5, 60.0, 0.5, 6.0));
    imageSizeAdj->setAdjusterListener(this);
    imageSizeAdj->set_tooltip_text(M("WATERMARK_IMAGE_SIZE_TOOLTIP"));
    attach(*imageSizeAdj, 0, row, 2, 1);
    row++;

    imageGapAdj = Gtk::manage(new Adjuster(M("WATERMARK_IMAGE_GAP"), 0, 200, 1, 8));
    imageGapAdj->setAdjusterListener(this);
    attach(*imageGapAdj, 0, row, 2, 1);
    row++;

    // --- Stroke section ---
    strokeEnableChk = Gtk::manage(new Gtk::CheckButton(M("WATERMARK_STROKE_ENABLE")));
    strokeEnableChk->signal_toggled().connect(sigc::mem_fun(*this, &WatermarkPanel::updateSensitivity));
    attach(*strokeEnableChk, 0, row, 2, 1);
    row++;

    strokeColorBtn = Gtk::manage(new Gtk::ColorButton());
    strokeColorBtn->set_use_alpha(true);
    setExpandAlignProperties(strokeColorBtn, true, false, Gtk::ALIGN_FILL, Gtk::ALIGN_CENTER);
    attach(*makeLabel(M("WATERMARK_STROKE_COLOR")), 0, row, 1, 1);
    attach(*strokeColorBtn, 1, row, 1, 1);
    row++;

    strokeWidthAdj = Gtk::manage(new Adjuster(M("WATERMARK_STROKE_WIDTH"), 0.5, 20.0, 0.5, 2.0));
    strokeWidthAdj->setAdjusterListener(this);
    attach(*strokeWidthAdj, 0, row, 2, 1);
    row++;

    // --- Shadow section ---
    shadowEnableChk = Gtk::manage(new Gtk::CheckButton(M("WATERMARK_SHADOW_ENABLE")));
    shadowEnableChk->signal_toggled().connect(sigc::mem_fun(*this, &WatermarkPanel::updateSensitivity));
    attach(*shadowEnableChk, 0, row, 2, 1);
    row++;

    shadowColorBtn = Gtk::manage(new Gtk::ColorButton());
    shadowColorBtn->set_use_alpha(true);
    setExpandAlignProperties(shadowColorBtn, true, false, Gtk::ALIGN_FILL, Gtk::ALIGN_CENTER);
    attach(*makeLabel(M("WATERMARK_SHADOW_COLOR")), 0, row, 1, 1);
    attach(*shadowColorBtn, 1, row, 1, 1);
    row++;

    shadowOffsetXAdj = Gtk::manage(new Adjuster(M("WATERMARK_SHADOW_OFFSETX"), -50, 50, 1, 3));
    shadowOffsetXAdj->setAdjusterListener(this);
    attach(*shadowOffsetXAdj, 0, row, 2, 1);
    row++;

    shadowOffsetYAdj = Gtk::manage(new Adjuster(M("WATERMARK_SHADOW_OFFSETY"), -50, 50, 1, 3));
    shadowOffsetYAdj->setAdjusterListener(this);
    attach(*shadowOffsetYAdj, 0, row, 2, 1);
    row++;

    shadowBlurAdj = Gtk::manage(new Adjuster(M("WATERMARK_SHADOW_BLUR"), 0, 50, 1, 3));
    shadowBlurAdj->setAdjusterListener(this);
    attach(*shadowBlurAdj, 0, row, 2, 1);
    row++;

    // --- Position ---
    positionCombo = Gtk::manage(new MyComboBoxText());
    positionCombo->append(M("WATERMARK_POS_TL"));
    positionCombo->append(M("WATERMARK_POS_TC"));
    positionCombo->append(M("WATERMARK_POS_TR"));
    positionCombo->append(M("WATERMARK_POS_CL"));
    positionCombo->append(M("WATERMARK_POS_C"));
    positionCombo->append(M("WATERMARK_POS_CR"));
    positionCombo->append(M("WATERMARK_POS_BL"));
    positionCombo->append(M("WATERMARK_POS_BC"));
    positionCombo->append(M("WATERMARK_POS_BR"));
    positionCombo->set_active(8); // Bottom Right
    setExpandAlignProperties(positionCombo, true, false, Gtk::ALIGN_FILL, Gtk::ALIGN_CENTER);
    attach(*makeLabel(M("WATERMARK_POSITION")), 0, row, 1, 1);
    attach(*positionCombo, 1, row, 1, 1);
    row++;

    marginXAdj = Gtk::manage(new Adjuster(M("WATERMARK_MARGINX"), 0, 500, 1, 20));
    marginXAdj->setAdjusterListener(this);
    attach(*marginXAdj, 0, row, 2, 1);
    row++;

    marginYAdj = Gtk::manage(new Adjuster(M("WATERMARK_MARGINY"), 0, 500, 1, 20));
    marginYAdj->setAdjusterListener(this);
    attach(*marginYAdj, 0, row, 2, 1);
    row++;

    rotationAdj = Gtk::manage(new Adjuster(M("WATERMARK_ROTATION"), -180, 180, 1, 0));
    rotationAdj->setAdjusterListener(this);
    attach(*rotationAdj, 0, row, 2, 1);
    row++;

    // Preview button
    previewBtn = Gtk::manage(new Gtk::Button(M("WATERMARK_PREVIEW")));
    previewBtn->signal_clicked().connect(sigc::mem_fun(*this, &WatermarkPanel::showPreview));
    attach(*previewBtn, 0, row, 2, 1);
    row++;

    show_all();
}

WatermarkPanel::~WatermarkPanel()
{
    delete previewWindow;
}

void WatermarkPanel::init(const WatermarkOptions& opts)
{
    enableChk->set_active(opts.enabled);
    textEntry->set_text(opts.text);

    // Build font description string for the FontButton
    Glib::ustring fontDesc = opts.fontFamily;
    if (opts.fontItalic) {
        fontDesc += " Italic";
    }
    if (opts.fontBold) {
        fontDesc += " Bold";
    }
    fontDesc += " " + std::to_string(opts.fontSize);
    fontButton->set_font_name(fontDesc);

    sizeMode->set_active(opts.sizeMode);
    fontSizeAdj->setValue(opts.fontSize);
    sizePercentAdj->setValue(opts.sizePercent);

    Gdk::RGBA textColor;
    textColor.set_rgba(opts.textR, opts.textG, opts.textB, opts.textA);
    textColorBtn->set_rgba(textColor);

    opacityAdj->setValue(opts.opacity * 100.0);

    imageEnableChk->set_active(opts.imageEnabled);
    if (opts.imagePath.empty()) {
        imageChooser->unselect_all();
    } else {
        imageChooser->set_filename(opts.imagePath);
    }
    imagePlacementCombo->set_active(opts.imagePlacement);
    imageSizeAdj->setValue(opts.imageSizePercent);
    imageGapAdj->setValue(opts.imageGap);

    strokeEnableChk->set_active(opts.strokeEnabled);
    Gdk::RGBA strokeColor;
    strokeColor.set_rgba(opts.strokeR, opts.strokeG, opts.strokeB, opts.strokeA);
    strokeColorBtn->set_rgba(strokeColor);
    strokeWidthAdj->setValue(opts.strokeWidth);

    shadowEnableChk->set_active(opts.shadowEnabled);
    Gdk::RGBA shadowColor;
    shadowColor.set_rgba(opts.shadowR, opts.shadowG, opts.shadowB, opts.shadowA);
    shadowColorBtn->set_rgba(shadowColor);
    shadowOffsetXAdj->setValue(opts.shadowOffsetX);
    shadowOffsetYAdj->setValue(opts.shadowOffsetY);
    shadowBlurAdj->setValue(opts.shadowBlur);

    positionCombo->set_active(opts.position);
    marginXAdj->setValue(opts.marginX);
    marginYAdj->setValue(opts.marginY);
    rotationAdj->setValue(opts.rotation);

    updateSensitivity();
    sizeModeChanged();
}

WatermarkOptions WatermarkPanel::getOptions()
{
    WatermarkOptions opts;

    opts.enabled = enableChk->get_active();
    opts.text = textEntry->get_text();

    // Parse font from FontButton
    Pango::FontDescription fd(fontButton->get_font_name());
    opts.fontFamily = fd.get_family();
    opts.fontSize = fd.get_size() / PANGO_SCALE;
    opts.fontBold = (fd.get_weight() >= Pango::WEIGHT_BOLD);
    opts.fontItalic = (fd.get_style() != Pango::STYLE_NORMAL);

    opts.sizeMode = sizeMode->get_active_row_number();
    opts.fontSize = static_cast<int>(fontSizeAdj->getValue());
    opts.sizePercent = sizePercentAdj->getValue();

    auto tc = textColorBtn->get_rgba();
    opts.textR = tc.get_red();
    opts.textG = tc.get_green();
    opts.textB = tc.get_blue();
    opts.textA = tc.get_alpha();

    opts.opacity = opacityAdj->getValue() / 100.0;

    opts.imageEnabled = imageEnableChk->get_active();
    opts.imagePath = imageChooser->get_filename();
    opts.imagePlacement = imagePlacementCombo->get_active_row_number();
    opts.imageSizePercent = imageSizeAdj->getValue();
    opts.imageGap = static_cast<int>(imageGapAdj->getValue());

    opts.strokeEnabled = strokeEnableChk->get_active();
    auto sc = strokeColorBtn->get_rgba();
    opts.strokeR = sc.get_red();
    opts.strokeG = sc.get_green();
    opts.strokeB = sc.get_blue();
    opts.strokeA = sc.get_alpha();
    opts.strokeWidth = strokeWidthAdj->getValue();

    opts.shadowEnabled = shadowEnableChk->get_active();
    auto shc = shadowColorBtn->get_rgba();
    opts.shadowR = shc.get_red();
    opts.shadowG = shc.get_green();
    opts.shadowB = shc.get_blue();
    opts.shadowA = shc.get_alpha();
    opts.shadowOffsetX = shadowOffsetXAdj->getValue();
    opts.shadowOffsetY = shadowOffsetYAdj->getValue();
    opts.shadowBlur = shadowBlurAdj->getValue();

    opts.position = positionCombo->get_active_row_number();
    opts.marginX = static_cast<int>(marginXAdj->getValue());
    opts.marginY = static_cast<int>(marginYAdj->getValue());
    opts.rotation = rotationAdj->getValue();

    return opts;
}

void WatermarkPanel::updateSensitivity()
{
    bool en = enableChk->get_active();
    textEntry->set_sensitive(en);
    fontButton->set_sensitive(en);
    sizeMode->set_sensitive(en);
    fontSizeAdj->set_sensitive(en);
    sizePercentAdj->set_sensitive(en);
    textColorBtn->set_sensitive(en);
    opacityAdj->set_sensitive(en);
    imageEnableChk->set_sensitive(en);
    const bool img = en && imageEnableChk->get_active();
    imageChooser->set_sensitive(img);
    imageClearBtn->set_sensitive(img);
    imagePlacementCombo->set_sensitive(img);
    imageSizeAdj->set_sensitive(img);
    // The gap and the placement only mean anything when text sits next to
    // the logo; with an empty text field the logo stands alone.
    imageGapAdj->set_sensitive(img && !textEntry->get_text().empty());
    strokeEnableChk->set_sensitive(en);
    strokeColorBtn->set_sensitive(en && strokeEnableChk->get_active());
    strokeWidthAdj->set_sensitive(en && strokeEnableChk->get_active());
    shadowEnableChk->set_sensitive(en);
    shadowColorBtn->set_sensitive(en && shadowEnableChk->get_active());
    shadowOffsetXAdj->set_sensitive(en && shadowEnableChk->get_active());
    shadowOffsetYAdj->set_sensitive(en && shadowEnableChk->get_active());
    shadowBlurAdj->set_sensitive(en && shadowEnableChk->get_active());
    positionCombo->set_sensitive(en);
    marginXAdj->set_sensitive(en);
    marginYAdj->set_sensitive(en);
    rotationAdj->set_sensitive(en);
    previewBtn->set_sensitive(en);
}

void WatermarkPanel::imagePlacementChanged()
{
    // Nothing to reconfigure yet — placement is read at render time. Kept as
    // a hook so the combo has a handler like the other controls.
    updateSensitivity();
}

void WatermarkPanel::sizeModeChanged()
{
    int mode = sizeMode->get_active_row_number();
    fontSizeAdj->set_visible(mode == 0); // Fixed pt
    sizePercentAdj->set_visible(mode == 1); // % of short edge
}

void WatermarkPanel::adjusterChanged(Adjuster* /*a*/, double /*newval*/)
{
    // No live preview needed — values are read at export time
}

void WatermarkPanel::showPreview()
{
    WatermarkOptions opts = getOptions();

    cairo_surface_t* logoSurf = (opts.imageEnabled && !opts.imagePath.empty())
        ? createWatermarkLogoSurface(opts.imagePath)
        : nullptr;

    Glib::ustring text = opts.text;
    // Only stand in sample text when there is nothing else to show; with a
    // logo chosen, an empty text field is a deliberate logo-only watermark
    // and the preview must reflect that.
    if (text.empty() && !logoSurf) {
        text = "Sample Watermark";
    }
    const bool haveText = !text.empty();

    // Preview canvas size
    const int canvasW = 600;
    const int canvasH = 400;

    // Compute font size: for preview, use fixed pt from adjuster,
    // or simulate % mode based on canvas short edge
    double fontSize;
    if (opts.sizeMode == 0) {
        fontSize = opts.fontSize;
    } else {
        int shortEdge = std::min(canvasW, canvasH);
        fontSize = shortEdge * opts.sizePercent / 100.0;
    }
    if (fontSize < 4.0) fontSize = 4.0;

    // Create Cairo surface
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, canvasW, canvasH);
    cairo_t* cr = cairo_create(surface);

    // Draw checkerboard background (to show alpha)
    const int checkSize = 16;
    for (int y = 0; y < canvasH; y += checkSize) {
        for (int x = 0; x < canvasW; x += checkSize) {
            if (((x / checkSize) + (y / checkSize)) % 2 == 0) {
                cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
            } else {
                cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
            }
            cairo_rectangle(cr, x, y, checkSize, checkSize);
            cairo_fill(cr);
        }
    }

    // Set up Pango font
    PangoLayout* layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, text.c_str(), -1);

    PangoFontDescription* fontDesc = pango_font_description_new();
    pango_font_description_set_family(fontDesc, opts.fontFamily.c_str());
    pango_font_description_set_absolute_size(fontDesc, fontSize * PANGO_SCALE);
    if (opts.fontBold) {
        pango_font_description_set_weight(fontDesc, PANGO_WEIGHT_BOLD);
    }
    if (opts.fontItalic) {
        pango_font_description_set_style(fontDesc, PANGO_STYLE_ITALIC);
    }
    pango_layout_set_font_description(layout, fontDesc);

    // Get text extents
    PangoRectangle inkRect, logicalRect;
    pango_layout_get_pixel_extents(layout, &inkRect, &logicalRect);

    const double txtW = haveText ? logicalRect.width : 0.0;
    const double txtH = haveText ? logicalRect.height : 0.0;

    // Same layout rules as the exporter (see watermarkrenderer.cc).
    double logoW = 0.0, logoH = 0.0;
    const bool haveLogo = watermarkLogoSize(opts, logoSurf, std::min(canvasW, canvasH), logoW, logoH);
    const double gap = (haveLogo && haveText) ? opts.imageGap : 0.0;
    const bool sideBySide = opts.imagePlacement <= 1;

    double contentW, contentH;
    if (haveLogo && haveText) {
        if (sideBySide) {
            contentW = logoW + gap + txtW;
            contentH = std::max(logoH, txtH);
        } else {
            contentW = std::max(logoW, txtW);
            contentH = logoH + gap + txtH;
        }
    } else if (haveLogo) {
        contentW = logoW;
        contentH = logoH;
    } else {
        contentW = txtW;
        contentH = txtH;
    }

    double textOffX = 0, textOffY = 0, logoOffX = 0, logoOffY = 0;
    if (haveLogo && haveText) {
        if (sideBySide) {
            if (opts.imagePlacement == 0) {
                logoOffX = 0;
                textOffX = logoW + gap;
            } else {
                textOffX = 0;
                logoOffX = txtW + gap;
            }
            logoOffY = (contentH - logoH) / 2.0;
            textOffY = (contentH - txtH) / 2.0;
        } else {
            if (opts.imagePlacement == 2) {
                logoOffY = 0;
                textOffY = logoH + gap;
            } else {
                textOffY = 0;
                logoOffY = txtH + gap;
            }
            logoOffX = (contentW - logoW) / 2.0;
            textOffX = (contentW - txtW) / 2.0;
        }
    }

    const double logoScaleX = haveLogo ? logoW / cairo_image_surface_get_width(logoSurf) : 1.0;
    const double logoScaleY = haveLogo ? logoH / cairo_image_surface_get_height(logoSurf) : 1.0;

    // Compute position
    double posX = 0, posY = 0;
    int col = opts.position % 3;
    int pRow = opts.position / 3;

    switch (col) {
    case 0: posX = opts.marginX; break;
    case 1: posX = (canvasW - contentW) / 2.0; break;
    case 2: posX = canvasW - contentW - opts.marginX; break;
    }
    switch (pRow) {
    case 0: posY = opts.marginY; break;
    case 1: posY = (canvasH - contentH) / 2.0; break;
    case 2: posY = canvasH - contentH - opts.marginY; break;
    }

    // Apply rotation
    double rotRad = opts.rotation * M_PI / 180.0;
    cairo_save(cr);
    cairo_translate(cr, posX + contentW / 2.0, posY + contentH / 2.0);
    cairo_rotate(cr, rotRad);
    cairo_translate(cr, -contentW / 2.0, -contentH / 2.0);

    double globalAlpha = opts.opacity;

    // Draw shadow — text and logo share it, as at export time
    if (opts.shadowEnabled) {
        cairo_set_source_rgba(cr, opts.shadowR, opts.shadowG, opts.shadowB, opts.shadowA * globalAlpha);

        if (haveText) {
            cairo_save(cr);
            cairo_move_to(cr, textOffX + opts.shadowOffsetX, textOffY + opts.shadowOffsetY);
            pango_cairo_show_layout(cr, layout);
            cairo_restore(cr);
        }

        if (haveLogo) {
            cairo_save(cr);
            cairo_translate(cr, logoOffX + opts.shadowOffsetX, logoOffY + opts.shadowOffsetY);
            cairo_scale(cr, logoScaleX, logoScaleY);
            cairo_set_source_rgba(cr, opts.shadowR, opts.shadowG, opts.shadowB, opts.shadowA * globalAlpha);
            cairo_mask_surface(cr, logoSurf, 0, 0);
            cairo_restore(cr);
        }
    }

    // Draw stroke — likewise shared
    if (opts.strokeEnabled && opts.strokeWidth > 0) {
        if (haveText) {
            cairo_move_to(cr, textOffX, textOffY);
            pango_cairo_layout_path(cr, layout);
            cairo_set_source_rgba(cr, opts.strokeR, opts.strokeG, opts.strokeB, opts.strokeA * globalAlpha);
            cairo_set_line_width(cr, opts.strokeWidth * 2.0);
            cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
            cairo_stroke(cr);
        }

        if (haveLogo) {
            constexpr int kSteps = 16;
            for (int i = 0; i < kSteps; ++i) {
                const double angle = 2.0 * M_PI * i / kSteps;
                cairo_save(cr);
                cairo_translate(cr,
                                logoOffX + std::cos(angle) * opts.strokeWidth,
                                logoOffY + std::sin(angle) * opts.strokeWidth);
                cairo_scale(cr, logoScaleX, logoScaleY);
                cairo_set_source_rgba(cr, opts.strokeR, opts.strokeG, opts.strokeB, opts.strokeA * globalAlpha);
                cairo_mask_surface(cr, logoSurf, 0, 0);
                cairo_restore(cr);
            }
        }
    }

    // Draw the logo, then the text over it
    if (haveLogo) {
        cairo_save(cr);
        cairo_translate(cr, logoOffX, logoOffY);
        cairo_scale(cr, logoScaleX, logoScaleY);
        cairo_set_source_surface(cr, logoSurf, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint_with_alpha(cr, globalAlpha);
        cairo_restore(cr);
    }

    if (haveText) {
        cairo_move_to(cr, textOffX, textOffY);
        cairo_set_source_rgba(cr, opts.textR, opts.textG, opts.textB, opts.textA * globalAlpha);
        pango_cairo_show_layout(cr, layout);
    }

    cairo_restore(cr);

    if (logoSurf) {
        cairo_surface_destroy(logoSurf);
    }

    g_object_unref(layout);
    pango_font_description_free(fontDesc);
    cairo_destroy(cr);

    // Convert to GdkPixbuf for display
    cairo_surface_flush(surface);
    unsigned char* data = cairo_image_surface_get_data(surface);
    int stride = cairo_image_surface_get_stride(surface);

    Glib::RefPtr<Gdk::Pixbuf> pixbuf = Gdk::Pixbuf::create(Gdk::COLORSPACE_RGB, true, 8, canvasW, canvasH);
    guint8* pixels = pixbuf->get_pixels();
    int pbStride = pixbuf->get_rowstride();

    for (int y = 0; y < canvasH; y++) {
        for (int x = 0; x < canvasW; x++) {
            int si = y * stride + x * 4;
            int di = y * pbStride + x * 4;
            // Cairo ARGB32 (premultiplied, BGRA in memory) -> GdkPixbuf RGBA (non-premultiplied)
            unsigned char a = data[si + 3];
            if (a > 0) {
                pixels[di + 0] = std::min(255, data[si + 2] * 255 / a); // R
                pixels[di + 1] = std::min(255, data[si + 1] * 255 / a); // G
                pixels[di + 2] = std::min(255, data[si + 0] * 255 / a); // B
            } else {
                pixels[di + 0] = 0;
                pixels[di + 1] = 0;
                pixels[di + 2] = 0;
            }
            pixels[di + 3] = a;
        }
    }

    cairo_surface_destroy(surface);

    // Show in a popup window
    delete previewWindow;
    previewWindow = new Gtk::Window(Gtk::WINDOW_TOPLEVEL);
    previewWindow->set_title(M("WATERMARK_PREVIEW"));
    previewWindow->set_default_size(canvasW, canvasH);
    previewWindow->set_resizable(false);

    auto* image = Gtk::manage(new Gtk::Image(pixbuf));
    previewWindow->add(*image);
    previewWindow->show_all();
}
