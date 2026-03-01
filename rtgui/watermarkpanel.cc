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
    Glib::ustring text = opts.text;
    if (text.empty()) {
        text = "Sample Watermark";
    }

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

    // Compute position
    double posX = 0, posY = 0;
    int col = opts.position % 3;
    int pRow = opts.position / 3;

    switch (col) {
    case 0: posX = opts.marginX; break;
    case 1: posX = (canvasW - logicalRect.width) / 2.0; break;
    case 2: posX = canvasW - logicalRect.width - opts.marginX; break;
    }
    switch (pRow) {
    case 0: posY = opts.marginY; break;
    case 1: posY = (canvasH - logicalRect.height) / 2.0; break;
    case 2: posY = canvasH - logicalRect.height - opts.marginY; break;
    }

    // Apply rotation
    double rotRad = opts.rotation * M_PI / 180.0;
    cairo_save(cr);
    cairo_translate(cr, posX + logicalRect.width / 2.0, posY + logicalRect.height / 2.0);
    cairo_rotate(cr, rotRad);
    cairo_translate(cr, -logicalRect.width / 2.0, -logicalRect.height / 2.0);

    double globalAlpha = opts.opacity;

    // Draw shadow
    if (opts.shadowEnabled) {
        cairo_save(cr);
        cairo_move_to(cr, opts.shadowOffsetX, opts.shadowOffsetY);
        cairo_set_source_rgba(cr, opts.shadowR, opts.shadowG, opts.shadowB, opts.shadowA * globalAlpha);
        pango_cairo_show_layout(cr, layout);
        cairo_restore(cr);
    }

    // Draw stroke
    if (opts.strokeEnabled && opts.strokeWidth > 0) {
        cairo_move_to(cr, 0, 0);
        pango_cairo_layout_path(cr, layout);
        cairo_set_source_rgba(cr, opts.strokeR, opts.strokeG, opts.strokeB, opts.strokeA * globalAlpha);
        cairo_set_line_width(cr, opts.strokeWidth * 2.0);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        cairo_stroke(cr);
    }

    // Draw text fill
    cairo_move_to(cr, 0, 0);
    cairo_set_source_rgba(cr, opts.textR, opts.textG, opts.textB, opts.textA * globalAlpha);
    pango_cairo_show_layout(cr, layout);

    cairo_restore(cr);

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
