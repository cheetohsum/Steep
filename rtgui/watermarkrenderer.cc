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
#include "watermarkrenderer.h"

#include <algorithm>
#include <cmath>
#include <cairo.h>
#include <pango/pangocairo.h>

#include "rtengine/iimage.h"

namespace
{

// Approximate Gaussian blur on a Cairo ARGB32 surface using box blur passes.
// This is a simple 3-pass box blur approximation.
void blurSurface(cairo_surface_t* surface, int radius)
{
    if (radius < 1) {
        return;
    }

    cairo_surface_flush(surface);

    const int width = cairo_image_surface_get_width(surface);
    const int height = cairo_image_surface_get_height(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    unsigned char* data = cairo_image_surface_get_data(surface);

    if (!data || width <= 0 || height <= 0) {
        return;
    }

    std::vector<unsigned char> tmp(stride * height);

    // Simple horizontal box blur pass
    auto hblur = [&](unsigned char* src, unsigned char* dst) {
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int r = 0, g = 0, b = 0, a = 0, cnt = 0;
                for (int k = -radius; k <= radius; k++) {
                    int xx = x + k;
                    if (xx >= 0 && xx < width) {
                        int idx = y * stride + xx * 4;
                        b += src[idx + 0];
                        g += src[idx + 1];
                        r += src[idx + 2];
                        a += src[idx + 3];
                        cnt++;
                    }
                }
                int idx = y * stride + x * 4;
                dst[idx + 0] = b / cnt;
                dst[idx + 1] = g / cnt;
                dst[idx + 2] = r / cnt;
                dst[idx + 3] = a / cnt;
            }
        }
    };

    // Simple vertical box blur pass
    auto vblur = [&](unsigned char* src, unsigned char* dst) {
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int r = 0, g = 0, b = 0, a = 0, cnt = 0;
                for (int k = -radius; k <= radius; k++) {
                    int yy = y + k;
                    if (yy >= 0 && yy < height) {
                        int idx = yy * stride + x * 4;
                        b += src[idx + 0];
                        g += src[idx + 1];
                        r += src[idx + 2];
                        a += src[idx + 3];
                        cnt++;
                    }
                }
                int idx = y * stride + x * 4;
                dst[idx + 0] = b / cnt;
                dst[idx + 1] = g / cnt;
                dst[idx + 2] = r / cnt;
                dst[idx + 3] = a / cnt;
            }
        }
    };

    // 2-pass blur (H then V) repeated for smoother result
    hblur(data, tmp.data());
    vblur(tmp.data(), data);
    hblur(data, tmp.data());
    vblur(tmp.data(), data);

    cairo_surface_mark_dirty(surface);
}

} // namespace

void applyWatermark(rtengine::IImagefloat* img, const WatermarkOptions& opts)
{
    if (!opts.enabled || opts.text.empty() || !img) {
        return;
    }

    const int imgW = img->getWidth();
    const int imgH = img->getHeight();
    if (imgW <= 0 || imgH <= 0) {
        return;
    }

    // Compute effective font size
    double effectiveFontSize;
    if (opts.sizeMode == 0) {
        // Fixed pt
        effectiveFontSize = opts.fontSize;
    } else {
        // % of short edge
        int shortEdge = std::min(imgW, imgH);
        effectiveFontSize = shortEdge * opts.sizePercent / 100.0;
    }
    if (effectiveFontSize < 1.0) {
        effectiveFontSize = 1.0;
    }

    // Create a temporary small surface to measure text extents
    cairo_surface_t* measureSurf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t* measureCr = cairo_create(measureSurf);

    PangoLayout* layout = pango_cairo_create_layout(measureCr);
    pango_layout_set_text(layout, opts.text.c_str(), -1);

    PangoFontDescription* fontDesc = pango_font_description_new();
    pango_font_description_set_family(fontDesc, opts.fontFamily.c_str());
    pango_font_description_set_absolute_size(fontDesc, effectiveFontSize * PANGO_SCALE);
    if (opts.fontBold) {
        pango_font_description_set_weight(fontDesc, PANGO_WEIGHT_BOLD);
    }
    if (opts.fontItalic) {
        pango_font_description_set_style(fontDesc, PANGO_STYLE_ITALIC);
    }
    pango_layout_set_font_description(layout, fontDesc);

    // Get text pixel extents
    PangoRectangle inkRect, logicalRect;
    pango_layout_get_pixel_extents(layout, &inkRect, &logicalRect);

    // Compute bounding box with padding for stroke, shadow, blur, and rotation
    double pad = opts.strokeWidth + std::abs(opts.shadowOffsetX) + std::abs(opts.shadowOffsetY) + opts.shadowBlur * 2 + 10;
    int textW = logicalRect.width + static_cast<int>(pad * 2);
    int textH = logicalRect.height + static_cast<int>(pad * 2);

    // If rotation is applied, compute rotated bounding box
    double rotRad = opts.rotation * M_PI / 180.0;
    double cosR = std::abs(std::cos(rotRad));
    double sinR = std::abs(std::sin(rotRad));
    int bbW = static_cast<int>(std::ceil(textW * cosR + textH * sinR));
    int bbH = static_cast<int>(std::ceil(textW * sinR + textH * cosR));

    // Clamp to prevent absurdly large allocations
    bbW = std::min(bbW, imgW);
    bbH = std::min(bbH, imgH);
    if (bbW <= 0 || bbH <= 0) {
        pango_font_description_free(fontDesc);
        g_object_unref(layout);
        cairo_destroy(measureCr);
        cairo_surface_destroy(measureSurf);
        return;
    }

    // Create the watermark surface (only bounding box size)
    cairo_surface_t* wmSurf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, bbW, bbH);
    cairo_t* cr = cairo_create(wmSurf);

    // Move to center of bounding box, apply rotation
    cairo_translate(cr, bbW / 2.0, bbH / 2.0);
    cairo_rotate(cr, rotRad);
    cairo_translate(cr, -textW / 2.0, -textH / 2.0);

    // Re-create layout on the actual surface
    PangoLayout* wmLayout = pango_cairo_create_layout(cr);
    pango_layout_set_text(wmLayout, opts.text.c_str(), -1);
    pango_layout_set_font_description(wmLayout, fontDesc);

    double textDrawX = pad;
    double textDrawY = pad;

    // Apply global opacity
    double globalAlpha = opts.opacity;

    // --- Draw shadow ---
    if (opts.shadowEnabled) {
        // Create separate surface for shadow to apply blur
        cairo_surface_t* shadowSurf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, bbW, bbH);
        cairo_t* shadowCr = cairo_create(shadowSurf);

        cairo_translate(shadowCr, bbW / 2.0, bbH / 2.0);
        cairo_rotate(shadowCr, rotRad);
        cairo_translate(shadowCr, -textW / 2.0, -textH / 2.0);

        PangoLayout* shadowLayout = pango_cairo_create_layout(shadowCr);
        pango_layout_set_text(shadowLayout, opts.text.c_str(), -1);
        pango_layout_set_font_description(shadowLayout, fontDesc);

        cairo_move_to(shadowCr, textDrawX + opts.shadowOffsetX, textDrawY + opts.shadowOffsetY);
        cairo_set_source_rgba(shadowCr, opts.shadowR, opts.shadowG, opts.shadowB, opts.shadowA * globalAlpha);
        pango_cairo_show_layout(shadowCr, shadowLayout);

        g_object_unref(shadowLayout);
        cairo_destroy(shadowCr);

        // Apply blur to shadow surface
        blurSurface(shadowSurf, static_cast<int>(opts.shadowBlur));

        // Paint shadow onto main watermark surface
        cairo_save(cr);
        cairo_identity_matrix(cr);
        cairo_set_source_surface(cr, shadowSurf, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);

        cairo_surface_destroy(shadowSurf);

        // Re-apply transform for subsequent drawing
        // (cairo_save/restore above reset the matrix, so we re-apply)
    }

    // --- Draw stroke ---
    if (opts.strokeEnabled && opts.strokeWidth > 0) {
        cairo_move_to(cr, textDrawX, textDrawY);
        pango_cairo_layout_path(cr, wmLayout);
        cairo_set_source_rgba(cr, opts.strokeR, opts.strokeG, opts.strokeB, opts.strokeA * globalAlpha);
        cairo_set_line_width(cr, opts.strokeWidth * 2.0); // stroke width is from center, so *2 for total width
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        cairo_stroke(cr);
    }

    // --- Draw text fill ---
    cairo_move_to(cr, textDrawX, textDrawY);
    cairo_set_source_rgba(cr, opts.textR, opts.textG, opts.textB, opts.textA * globalAlpha);
    pango_cairo_show_layout(cr, wmLayout);

    g_object_unref(wmLayout);
    pango_font_description_free(fontDesc);
    cairo_destroy(cr);
    cairo_destroy(measureCr);
    cairo_surface_destroy(measureSurf);
    g_object_unref(layout);

    // --- Compute placement position ---
    int posX = 0, posY = 0;
    int col = opts.position % 3; // 0=left, 1=center, 2=right
    int row = opts.position / 3; // 0=top, 1=center, 2=bottom

    switch (col) {
    case 0: posX = opts.marginX; break;
    case 1: posX = (imgW - bbW) / 2; break;
    case 2: posX = imgW - bbW - opts.marginX; break;
    }
    switch (row) {
    case 0: posY = opts.marginY; break;
    case 1: posY = (imgH - bbH) / 2; break;
    case 2: posY = imgH - bbH - opts.marginY; break;
    }

    // Clamp position
    posX = std::max(0, std::min(posX, imgW - 1));
    posY = std::max(0, std::min(posY, imgH - 1));

    // --- Composite watermark onto image ---
    cairo_surface_flush(wmSurf);
    const unsigned char* wmData = cairo_image_surface_get_data(wmSurf);
    const int wmStride = cairo_image_surface_get_stride(wmSurf);

    // IImagefloat pixel values are in 0..65535 range (float)
    const float scale = 65535.0f / 255.0f;

    int copyW = std::min(bbW, imgW - posX);
    int copyH = std::min(bbH, imgH - posY);

    for (int y = 0; y < copyH; y++) {
        const int imgY = posY + y;
        const unsigned char* wmRow = wmData + y * wmStride;

        for (int x = 0; x < copyW; x++) {
            const int imgX = posX + x;
            const int wmIdx = x * 4;

            // Cairo ARGB32: B G R A in memory (little-endian)
            const float a = wmRow[wmIdx + 3] / 255.0f;
            if (a < 0.001f) {
                continue;
            }

            const float wmB = wmRow[wmIdx + 0] * scale;
            const float wmG = wmRow[wmIdx + 1] * scale;
            const float wmR = wmRow[wmIdx + 2] * scale;

            // Premultiplied alpha compositing:
            // Since Cairo uses premultiplied alpha, we just need:
            //   out = src + dst * (1 - alpha)
            const float invA = 1.0f - a;
            img->r(imgY, imgX) = wmR + img->r(imgY, imgX) * invA;
            img->g(imgY, imgX) = wmG + img->g(imgY, imgX) * invA;
            img->b(imgY, imgX) = wmB + img->b(imgY, imgX) * invA;
        }
    }

    cairo_surface_destroy(wmSurf);
}
