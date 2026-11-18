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
 */

#include "improcfun.h"
#include "alpha.h"
#include "array2D.h"
#include "boxblur.h"
#include "edittrace.h"
#include "procparams.h"
#include "iccstore.h"
#include "imagesource.h"
#include "imagefloat.h"
#include "stdimagesource.h"
#include <glib/gstdio.h>
#include "rt_math.h"
#include "repairmath.h"
#include "gauss.h"
#include "guidedfilter.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <random>
#include <set>
#include <unordered_set>

#ifdef RT_AI_MASKING
#include "aiinpainting.h"
#include "samselector.h"
#include "airemover.h"
#endif

namespace rtengine
{

class SpotBox;

}

namespace
{

using Boxes = std::vector<std::shared_ptr<rtengine::SpotBox>>;

/**
 * Add the spot and its dependencies to a set of dependencies.
 *
 * @param spotNum The spot's index.
 * @param dependencies A set to place the dependencies in. Spots that are
 * already in the set must have all their dependencies included already.
 * @param srcSpots Information on spot sources.
 * @param dstSpots Information on spot destinations.
 */
void addSpotDependencies(int spotNum, std::unordered_set<int> &dependencies, const Boxes &srcSpots, const Boxes &dstSpots);

/**
 * Returns the supplied spots and all their dependencies.
 *
 * @param visibleSpots The spots to get dependencies for.
 * @param srcSpots Information on spot sources.
 * @param dstSpots Information on spot destinations.
 */
std::unordered_set<int> calcSpotDependencies(const std::set<int> &visibleSpots, const Boxes &srcSpots, const Boxes &dstSpots);
}

namespace rtengine
{

class SpotBox {

public:
    enum class Type {
        SOURCE,
        TARGET,
        FINAL
    };

    procparams::SpotMethod method;

    struct Rectangle {
        int x1;
        int y1;
        int x2;
        int y2;

        Rectangle() : Rectangle(0, 0, 0, 0) {}
        Rectangle(int X1, int Y1, int X2, int Y2) : x1(X1), y1(Y1), x2(X2), y2(Y2) {}

        int getWidth() {
            return x2 - x1 + 1;
        }

        int getHeight() {
            return y2 - y1 + 1;
        }

        bool intersects(const Rectangle &other) const {
            return (other.x1 <= x2 && other.x2 >= x1)
                && (other.y1 <= y2 && other.y2 >= y1);
        }

        bool getIntersection(const Rectangle &other, std::unique_ptr<Rectangle> &intersection) const {
            if (intersects(other)) {
                std::unique_ptr<Rectangle> intsec(
                    new Rectangle(
                        rtengine::max(x1, other.x1),
                        rtengine::max(y1, other.y1),
                        rtengine::min(x2, other.x2),
                        rtengine::min(y2, other.y2)
                    )
                );

                if (intsec->x1 > intsec->x2 || intsec->y1 > intsec->y2) {
                    return false;
                }

                intersection = std::move(intsec);
                return true;
            }
            if (intersection) {
                // There's no intersection, we delete the Rectangle structure
                intersection.release();
            }
            return false;
        }

        Rectangle& operator+=(const Coord &v) {
            x1 += v.x;
            y1 += v.y;
            x2 += v.x;
            y2 += v.y;
            return *this;
        }

        Rectangle& operator-=(const Coord &v) {
            x1 -= v.x;
            y1 -= v.y;
            x2 -= v.x;
            y2 -= v.y;
            return *this;
        }

        Rectangle& operator/=(int v) {
            if (v == 1) {
                return *this;
            }

            int w = x2 - x1 + 1;
            int h = y2 - y1 + 1;
            w = w / v + static_cast<bool>(w % v);
            h = h / v + static_cast<bool>(h % v);
            x1 /= v;
            y1 /= v;
            x2 = x1 + w - 1;
            y2 = y1 + h - 1;

            return *this;
        }
    };

private:
    Type type;
    Imagefloat* image;

public:
    // top/left and bottom/right coordinates of the spot in image space (at some point divided by scale factor)
    Rectangle spotArea;
    // top/left and bottom/right coordinates of the spot in scaled image space (on borders, imgArea won't cover spotArea)
    Rectangle imgArea;
    // top/left and bottom/right coordinates of useful part of the image in scaled image space (rounding error workaround)
    Rectangle intersectionArea;
    float radius;
    float featherRadius;

    SpotBox (int tl_x, int tl_y, int br_x, int br_y, int radius, int feather_radius, Imagefloat* image, Type type) :
       method(procparams::SpotMethod::CLONE),
       type(type),
       image(image),
       spotArea(tl_x, tl_y, br_x, br_y),
       imgArea(spotArea),
       intersectionArea(),
       radius(radius),
       featherRadius(feather_radius)
    {}

    SpotBox (int tl_x, int tl_y, int radius, int feather_radius, Imagefloat* image, Type type) :
       method(procparams::SpotMethod::CLONE),
       type(type),
       image(image),
       spotArea(tl_x, tl_y, image ? tl_x + image->getWidth() - 1 : 0, image ? tl_y + image->getHeight() - 1 : 0),
       imgArea(spotArea),
       intersectionArea(),
       radius(radius),
       featherRadius(feather_radius)
    {}

    SpotBox (SpotEntry &spot, Type type) :
        method(spot.method),
        type(type),
        image(nullptr),
        intersectionArea(),
        radius(spot.radius),
        featherRadius(int(spot.getFeatherRadius() + 0.5f))  // rounding to int before resizing
    {
        spotArea.x1 = int ((type == Type::SOURCE ? spot.sourcePos.x : spot.targetPos.x) - featherRadius);
        spotArea.x2 = int ((type == Type::SOURCE ? spot.sourcePos.x : spot.targetPos.x) + featherRadius);
        spotArea.y1 = int ((type == Type::SOURCE ? spot.sourcePos.y : spot.targetPos.y) - featherRadius);
        spotArea.y2 = int ((type == Type::SOURCE ? spot.sourcePos.y : spot.targetPos.y) + featherRadius);
        imgArea = spotArea;
    }

    ~SpotBox() {
        if (image && type != Type::FINAL) {
            delete image;
        }
    }

    SpotBox& operator /=(int v) {
        if (v == 1) {
            return *this;
        }
        spotArea /= v;
        imgArea /= v;
        radius /= float(v);
        featherRadius = getWidth() / 2.f;
        // intersectionArea doesn't need resize, because it's set after resizing
        return *this;
    }

    int getWidth() {
        return spotArea.getWidth();
    }

    int getHeight() {
        return spotArea.getHeight();
    }

    int getImageWidth() {
        return imgArea.getWidth();
    }

    int getImageHeight() {
        return imgArea.getHeight();
    }

    int getIntersectionWidth() {
        return intersectionArea.getWidth();
    }

    int getIntersectionHeight() {
        return intersectionArea.getHeight();
    }

    bool checkImageSize() {
        if (!image || getImageWidth() != image->getWidth() || getImageHeight() != image->getHeight()) {
            return false;
        }
        return true;
    }

    void tuneImageSize() {
        if (!image) {
            return;
        }
        if (getImageWidth() > image->getWidth()) {
            imgArea.x2 = imgArea.x1 + image->getWidth() - 1;
        }
        if (getImageHeight() > image->getHeight()) {
            imgArea.y2 = imgArea.y1 + image->getHeight() - 1;
        }
    }

    Imagefloat *getImage() {  // TODO: this should send back a const value, but getImage don't want it to be const...
        return image;
    }

    void allocImage() {
        int newW = imgArea.x2 - imgArea.x1 + 1;
        int newH = imgArea.y2 - imgArea.y1 + 1;

        if (image && type != Type::FINAL && (image->getWidth() != newW || image->getHeight() != newH)) {
            delete image;
            image = nullptr;
        }
        if (image == nullptr) {
            image = new Imagefloat(newW, newH);
        }
    }

    bool spotIntersects(const SpotBox &other) const {
        return spotArea.intersects(other.spotArea);
    }

    bool getSpotIntersection(const SpotBox &other, std::unique_ptr<Rectangle> &intersection) const {
        return spotArea.getIntersection(other.spotArea, intersection);
    }

    bool imageIntersects(const SpotBox &other, bool atDestLocation=false) const {
        if (atDestLocation) {
            Coord v(other.spotArea.x1 - spotArea.x1, other.spotArea.y1 - spotArea.y1);
            Rectangle imgArea2(imgArea.x1, imgArea.y1, imgArea.x2, imgArea.y2);
            imgArea2 += v;
            return imgArea2.intersects(other.imgArea);
        }
        return imgArea.intersects(other.imgArea);
    }

    bool mutuallyClipImageArea(SpotBox &other) {
        Coord v(other.spotArea.x1 - spotArea.x1, other.spotArea.y1 - spotArea.y1);
        Rectangle imgArea2 = imgArea;
        imgArea2 += v;
        std::unique_ptr<Rectangle> intersection;
        if (!imgArea2.getIntersection(other.imgArea, intersection)) {
            return false;
        }
        other.intersectionArea = *intersection;
        Coord v2(-v.x, -v.y);
        *intersection -= v;
        intersectionArea = *intersection;
        return true;
    }

    bool setIntersectionWith(const SpotBox &other) {
        if (!spotIntersects(other)) {
            return false;
        }
        imgArea.x1 = rtengine::max(spotArea.x1, other.spotArea.x1);
        imgArea.x2 = rtengine::min(spotArea.x2, other.spotArea.x2);
        imgArea.y1 = rtengine::max(spotArea.y1, other.spotArea.y1);
        imgArea.y2 = rtengine::min(spotArea.y2, other.spotArea.y2);
        if (imgArea.x1 > imgArea.x2 || imgArea.y1 > imgArea.y2) {
            return false;
        }
        return true;
    }

    // Cores (scaled x, y, radius) of spots already processed this pass.
    // Pixels inside any of them keep their existing repair, so placing a
    // new spot on the border of an old one never disturbs the overlap.
    const std::vector<std::array<float, 3>>* priorCores = nullptr;

    bool coveredByPrior(float px, float py) const {
        if (!priorCores) {
            return false;
        }
        for (const auto& core : *priorCores) {
            const float ddx = px - core[0];
            const float ddy = py - core[1];
            if (ddx * ddx + ddy * ddy <= core[2] * core[2]) {
                return true;
            }
        }
        return false;
    }

    bool processIntersectionWith(SpotBox &destBox) {
        using procparams::SpotMethod;
        switch (method) {
            case SpotMethod::HEAL:    return processHeal(destBox);
            case SpotMethod::ERASE:   return processErase(destBox);
            case SpotMethod::REDEYE:  return processRedEye(destBox);
            // Auto-detected dust specks. Heal, not erase: erase fills with the
            // surrounding average, which on anything but a perfectly flat sky
            // reads as a soft blurred disc where the speck was. Heal carries
            // the grain and texture over from the source the detector picked.
            case SpotMethod::AI_DUST: return processHeal(destBox);
            default:                  return processClone(destBox);
        }
    }

    // Clone: copy source pixels to destination with feather blending (original behavior)
    bool processClone(SpotBox &destBox) {
        Imagefloat *dstImg = destBox.image;

        if (image == nullptr || dstImg == nullptr) {
            std::cerr << "One of the source or destination SpotBox image is missing !" << std::endl;
            return false;
        }

        int srcImgY = intersectionArea.y1 - imgArea.y1;
        int dstImgY = destBox.intersectionArea.y1 - destBox.imgArea.y1;
        for (int y = intersectionArea.y1; y <= intersectionArea.y2; ++y) {
            float  dy = float(y - spotArea.y1) - featherRadius;

            int srcImgX = intersectionArea.x1 - imgArea.x1;
            int dstImgX = destBox.intersectionArea.x1 - destBox.imgArea.x1;
            for (int x = intersectionArea.x1; x <= intersectionArea.x2; ++x) {
                float dx = float(x - spotArea.x1) - featherRadius;
                float r = sqrt(dx * dx + dy * dy);

                if (r >= featherRadius
                        || destBox.coveredByPrior(float(x - spotArea.x1 + destBox.spotArea.x1),
                                                  float(y - spotArea.y1 + destBox.spotArea.y1))) {
                    ++srcImgX;
                    ++dstImgX;
                    continue;
                }
                if (r <= radius) {
                    dstImg->r(dstImgY, dstImgX) = image->r(srcImgY, srcImgX);
                    dstImg->g(dstImgY, dstImgX) = image->g(srcImgY, srcImgX);
                    dstImg->b(dstImgY, dstImgX) = image->b(srcImgY, srcImgX);
                } else {
                    float opacity = (featherRadius - r) / (featherRadius - radius);
                    dstImg->r(dstImgY, dstImgX) = (image->r(srcImgY, srcImgX) - dstImg->r(dstImgY, dstImgX)) * opacity + dstImg->r(dstImgY,dstImgX);
                    dstImg->g(dstImgY, dstImgX) = (image->g(srcImgY, srcImgX) - dstImg->g(dstImgY, dstImgX)) * opacity + dstImg->g(dstImgY,dstImgX);
                    dstImg->b(dstImgY, dstImgX) = (image->b(srcImgY, srcImgX) - dstImg->b(dstImgY, dstImgX)) * opacity + dstImg->b(dstImgY,dstImgX);
                }
                ++srcImgX;
                ++dstImgX;
            }
            ++srcImgY;
            ++dstImgY;
        }

        return true;
    }

    // Heal: copy source texture but match destination luminance
    bool processHeal(SpotBox &destBox) {
        Imagefloat *dstImg = destBox.image;

        if (image == nullptr || dstImg == nullptr) {
            std::cerr << "One of the source or destination SpotBox image is missing !" << std::endl;
            return false;
        }

        // Pass 1: compute average luminance of source and destination within the inner radius
        double srcLumSum = 0.0, dstLumSum = 0.0;
        int lumCount = 0;

        int srcImgY = intersectionArea.y1 - imgArea.y1;
        int dstImgY = destBox.intersectionArea.y1 - destBox.imgArea.y1;
        for (int y = intersectionArea.y1; y <= intersectionArea.y2; ++y) {
            float dy = float(y - spotArea.y1) - featherRadius;
            int srcImgX = intersectionArea.x1 - imgArea.x1;
            int dstImgX = destBox.intersectionArea.x1 - destBox.imgArea.x1;
            for (int x = intersectionArea.x1; x <= intersectionArea.x2; ++x) {
                float dx = float(x - spotArea.x1) - featherRadius;
                float r = sqrt(dx * dx + dy * dy);
                if (r <= radius) {
                    srcLumSum += 0.2126 * image->r(srcImgY, srcImgX) + 0.7152 * image->g(srcImgY, srcImgX) + 0.0722 * image->b(srcImgY, srcImgX);
                    dstLumSum += 0.2126 * dstImg->r(dstImgY, dstImgX) + 0.7152 * dstImg->g(dstImgY, dstImgX) + 0.0722 * dstImg->b(dstImgY, dstImgX);
                    lumCount++;
                }
                ++srcImgX;
                ++dstImgX;
            }
            ++srcImgY;
            ++dstImgY;
        }

        float lumRatio = 1.f;
        if (lumCount > 0 && srcLumSum > 0.0) {
            lumRatio = LIM(float(dstLumSum / srcLumSum), 0.25f, 4.0f);
        }

        // Pass 2: apply source pixels scaled by luminance ratio
        srcImgY = intersectionArea.y1 - imgArea.y1;
        dstImgY = destBox.intersectionArea.y1 - destBox.imgArea.y1;
        for (int y = intersectionArea.y1; y <= intersectionArea.y2; ++y) {
            float dy = float(y - spotArea.y1) - featherRadius;
            int srcImgX = intersectionArea.x1 - imgArea.x1;
            int dstImgX = destBox.intersectionArea.x1 - destBox.imgArea.x1;
            for (int x = intersectionArea.x1; x <= intersectionArea.x2; ++x) {
                float dx = float(x - spotArea.x1) - featherRadius;
                float r = sqrt(dx * dx + dy * dy);

                if (r >= featherRadius
                        || destBox.coveredByPrior(float(x - spotArea.x1 + destBox.spotArea.x1),
                                                  float(y - spotArea.y1 + destBox.spotArea.y1))) {
                    ++srcImgX;
                    ++dstImgX;
                    continue;
                }

                float srcR = image->r(srcImgY, srcImgX) * lumRatio;
                float srcG = image->g(srcImgY, srcImgX) * lumRatio;
                float srcB = image->b(srcImgY, srcImgX) * lumRatio;

                if (r <= radius) {
                    dstImg->r(dstImgY, dstImgX) = srcR;
                    dstImg->g(dstImgY, dstImgX) = srcG;
                    dstImg->b(dstImgY, dstImgX) = srcB;
                } else {
                    float opacity = (featherRadius - r) / (featherRadius - radius);
                    dstImg->r(dstImgY, dstImgX) = (srcR - dstImg->r(dstImgY, dstImgX)) * opacity + dstImg->r(dstImgY, dstImgX);
                    dstImg->g(dstImgY, dstImgX) = (srcG - dstImg->g(dstImgY, dstImgX)) * opacity + dstImg->g(dstImgY, dstImgX);
                    dstImg->b(dstImgY, dstImgX) = (srcB - dstImg->b(dstImgY, dstImgX)) * opacity + dstImg->b(dstImgY, dstImgX);
                }
                ++srcImgX;
                ++dstImgX;
            }
            ++srcImgY;
            ++dstImgY;
        }

        return true;
    }

    // Erase: fill target area with average color from the feather ring
    bool processErase(SpotBox &destBox) {
        Imagefloat *dstImg = destBox.image;

        if (dstImg == nullptr) {
            std::cerr << "Destination SpotBox image is missing !" << std::endl;
            return false;
        }

        // Compute average RGB from pixels in the feather ring (between radius and featherRadius)
        double avgR = 0.0, avgG = 0.0, avgB = 0.0;
        int count = 0;

        int dstImgY = destBox.intersectionArea.y1 - destBox.imgArea.y1;
        for (int y = destBox.intersectionArea.y1; y <= destBox.intersectionArea.y2; ++y) {
            float dy = float(y - destBox.spotArea.y1) - destBox.featherRadius;
            int dstImgX = destBox.intersectionArea.x1 - destBox.imgArea.x1;
            for (int x = destBox.intersectionArea.x1; x <= destBox.intersectionArea.x2; ++x) {
                float dx = float(x - destBox.spotArea.x1) - destBox.featherRadius;
                float r = sqrt(dx * dx + dy * dy);
                if (r > destBox.radius && r < destBox.featherRadius) {
                    avgR += dstImg->r(dstImgY, dstImgX);
                    avgG += dstImg->g(dstImgY, dstImgX);
                    avgB += dstImg->b(dstImgY, dstImgX);
                    count++;
                }
                ++dstImgX;
            }
            ++dstImgY;
        }

        if (count > 0) {
            avgR /= count;
            avgG /= count;
            avgB /= count;
        }

        // Fill inner area with average, feather-blend at edges
        dstImgY = destBox.intersectionArea.y1 - destBox.imgArea.y1;
        for (int y = destBox.intersectionArea.y1; y <= destBox.intersectionArea.y2; ++y) {
            float dy = float(y - destBox.spotArea.y1) - destBox.featherRadius;
            int dstImgX = destBox.intersectionArea.x1 - destBox.imgArea.x1;
            for (int x = destBox.intersectionArea.x1; x <= destBox.intersectionArea.x2; ++x) {
                float dx = float(x - destBox.spotArea.x1) - destBox.featherRadius;
                float r = sqrt(dx * dx + dy * dy);

                if (r >= destBox.featherRadius
                        || destBox.coveredByPrior(float(x), float(y))) {
                    ++dstImgX;
                    continue;
                }
                if (r <= destBox.radius) {
                    dstImg->r(dstImgY, dstImgX) = float(avgR);
                    dstImg->g(dstImgY, dstImgX) = float(avgG);
                    dstImg->b(dstImgY, dstImgX) = float(avgB);
                } else {
                    float opacity = (destBox.featherRadius - r) / (destBox.featherRadius - destBox.radius);
                    dstImg->r(dstImgY, dstImgX) = (float(avgR) - dstImg->r(dstImgY, dstImgX)) * opacity + dstImg->r(dstImgY, dstImgX);
                    dstImg->g(dstImgY, dstImgX) = (float(avgG) - dstImg->g(dstImgY, dstImgX)) * opacity + dstImg->g(dstImgY, dstImgX);
                    dstImg->b(dstImgY, dstImgX) = (float(avgB) - dstImg->b(dstImgY, dstImgX)) * opacity + dstImg->b(dstImgY, dstImgX);
                }
                ++dstImgX;
            }
            ++dstImgY;
        }

        return true;
    }

    // Red Eye: desaturate red pixels in the target area
    bool processRedEye(SpotBox &destBox) {
        Imagefloat *dstImg = destBox.image;

        if (dstImg == nullptr) {
            std::cerr << "Destination SpotBox image is missing !" << std::endl;
            return false;
        }

        int dstImgY = destBox.intersectionArea.y1 - destBox.imgArea.y1;
        for (int y = destBox.intersectionArea.y1; y <= destBox.intersectionArea.y2; ++y) {
            float dy = float(y - destBox.spotArea.y1) - destBox.featherRadius;
            int dstImgX = destBox.intersectionArea.x1 - destBox.imgArea.x1;
            for (int x = destBox.intersectionArea.x1; x <= destBox.intersectionArea.x2; ++x) {
                float dx = float(x - destBox.spotArea.x1) - destBox.featherRadius;
                float r = sqrt(dx * dx + dy * dy);

                if (r >= destBox.featherRadius
                        || destBox.coveredByPrior(float(x), float(y))) {
                    ++dstImgX;
                    continue;
                }

                float pR = dstImg->r(dstImgY, dstImgX);
                float pG = dstImg->g(dstImgY, dstImgX);
                float pB = dstImg->b(dstImgY, dstImgX);
                float avgGB = (pG + pB) * 0.5f;

                // Check if pixel is "red" (R channel significantly higher than G,B average)
                if (avgGB > 1.f && pR / avgGB > 1.5f) {
                    float newR = avgGB; // Desaturate red to match green/blue average

                    if (r <= destBox.radius) {
                        dstImg->r(dstImgY, dstImgX) = newR;
                    } else {
                        float opacity = (destBox.featherRadius - r) / (destBox.featherRadius - destBox.radius);
                        dstImg->r(dstImgY, dstImgX) = (newR - pR) * opacity + pR;
                    }
                }
                ++dstImgX;
            }
            ++dstImgY;
        }

        return true;
    }

    // Copy the intersecting part
    bool copyImgTo(SpotBox &destBox) {
        Imagefloat *destImg = destBox.image;

        if (image == nullptr || destImg == nullptr) {
            std::cerr << "One of the source or destination SpotBox image is missing !" << std::endl;
            return false;
        }

        std::unique_ptr<Rectangle> intersection;

        if (!intersectionArea.getIntersection(destBox.intersectionArea, intersection)) {
            return false;
        }

        Imagefloat *srcImg = image;
        Imagefloat *dstImg = destBox.image;

        int srcImgY = intersection->y1 - imgArea.y1;
        int dstImgY = intersection->y1 - destBox.imgArea.y1;
        for (int y = intersection->y1; y <= intersection->y2; ++y) {
            int srcImgX = intersection->x1 - imgArea.x1;
            int dstImgX = intersection->x1 - destBox.imgArea.x1;

            for (int x = intersection->x1; x <= intersection->x2; ++x) {
                dstImg->r(dstImgY, dstImgX) = srcImg->r(srcImgY, srcImgX);
                dstImg->g(dstImgY, dstImgX) = srcImg->g(srcImgY, srcImgX);
                dstImg->b(dstImgY, dstImgX) = srcImg->b(srcImgY, srcImgX);
                ++srcImgX;
                ++dstImgX;
            }
            ++srcImgY;
            ++dstImgY;
        }

        return true;
    }
};

namespace {

// Process a stroke-based erase directly on the preview image
// Zero out mask coverage that earlier stroke entries already repaired, so
// touching the border of an existing spot never disturbs the overlap.
void excludePriorStrokeCoverage(std::vector<float>& mask,
                                int bbW, int bbH, int bbMinX, int bbMinY,
                                int cropX, int cropY, int skip,
                                const std::vector<const SpotEntry*>& priorStrokes)
{
    for (const auto* prior : priorStrokes) {
        const float priorRadius = float(prior->radius) / float(skip);
        for (const auto& pt : prior->strokePoints) {
            const float cx = float(pt.x - cropX) / float(skip) - bbMinX;
            const float cy = float(pt.y - cropY) / float(skip) - bbMinY;
            if (cx < -priorRadius || cy < -priorRadius
                    || cx > float(bbW) + priorRadius || cy > float(bbH) + priorRadius) {
                continue;
            }

            const int pyMin = std::max(0, int(cy - priorRadius));
            const int pyMax = std::min(bbH - 1, int(cy + priorRadius));
            const int pxMin = std::max(0, int(cx - priorRadius));
            const int pxMax = std::min(bbW - 1, int(cx + priorRadius));

            for (int py = pyMin; py <= pyMax; ++py) {
                for (int px = pxMin; px <= pxMax; ++px) {
                    const float dx = float(px) - cx;
                    const float dy = float(py) - cy;
                    if (dx * dx + dy * dy <= priorRadius * priorRadius) {
                        mask[py * bbW + px] = 0.f;
                    }
                }
            }
        }
    }
}

void processStrokeErase(Imagefloat* img, const SpotEntry& entry, const PreviewProps &pp,
                        const std::vector<const SpotEntry*>& priorStrokes)
{
    if (entry.strokePoints.empty()) return;

    int skip = pp.getSkip();
    int cropX = pp.getX();
    int cropY = pp.getY();
    int imgW = img->getWidth();
    int imgH = img->getHeight();
    float radius = float(entry.radius) / float(skip);
    float featherRadius = entry.getFeatherRadius() / float(skip);

    // Compute bounding box of all stroke points in crop-local scaled coordinates
    int bbMinX = INT_MAX, bbMinY = INT_MAX, bbMaxX = INT_MIN, bbMaxY = INT_MIN;
    for (const auto& pt : entry.strokePoints) {
        int sx = (pt.x - cropX) / skip;
        int sy = (pt.y - cropY) / skip;
        bbMinX = std::min(bbMinX, sx - int(featherRadius) - 1);
        bbMinY = std::min(bbMinY, sy - int(featherRadius) - 1);
        bbMaxX = std::max(bbMaxX, sx + int(featherRadius) + 1);
        bbMaxY = std::max(bbMaxY, sy + int(featherRadius) + 1);
    }

    // Clip to image bounds
    bbMinX = std::max(0, bbMinX);
    bbMinY = std::max(0, bbMinY);
    bbMaxX = std::min(imgW - 1, bbMaxX);
    bbMaxY = std::min(imgH - 1, bbMaxY);

    if (bbMinX > bbMaxX || bbMinY > bbMaxY) return;

    int bbW = bbMaxX - bbMinX + 1;
    int bbH = bbMaxY - bbMinY + 1;

    // Build a 2D mask within the bounding box
    std::vector<float> mask(bbW * bbH, 0.f);

    for (const auto& pt : entry.strokePoints) {
        float cx = float(pt.x - cropX) / float(skip) - bbMinX;
        float cy = float(pt.y - cropY) / float(skip) - bbMinY;

        int pyMin = std::max(0, int(cy - featherRadius));
        int pyMax = std::min(bbH - 1, int(cy + featherRadius));
        int pxMin = std::max(0, int(cx - featherRadius));
        int pxMax = std::min(bbW - 1, int(cx + featherRadius));

        for (int py = pyMin; py <= pyMax; ++py) {
            for (int px = pxMin; px <= pxMax; ++px) {
                float dx = float(px) - cx;
                float dy = float(py) - cy;
                float dist = sqrtf(dx * dx + dy * dy);
                float val = 0.f;
                if (dist <= radius) {
                    val = 1.f;
                } else if (dist < featherRadius) {
                    val = (featherRadius - dist) / (featherRadius - radius);
                }
                // Max-blend (union of stroke circles)
                mask[py * bbW + px] = std::max(mask[py * bbW + px], val);
            }
        }
    }

    // Keep the repairs from earlier strokes intact in any overlap
    excludePriorStrokeCoverage(mask, bbW, bbH, bbMinX, bbMinY, cropX, cropY, skip, priorStrokes);

    // Sample average RGB from boundary pixels (where mask transitions from 0 to >0)
    double avgR = 0.0, avgG = 0.0, avgB = 0.0;
    int count = 0;

    for (int py = 0; py < bbH; ++py) {
        for (int px = 0; px < bbW; ++px) {
            float m = mask[py * bbW + px];
            if (m > 0.f && m < 0.5f) {
                int ix = bbMinX + px;
                int iy = bbMinY + py;
                if (ix >= 0 && ix < imgW && iy >= 0 && iy < imgH) {
                    avgR += img->r(iy, ix);
                    avgG += img->g(iy, ix);
                    avgB += img->b(iy, ix);
                    count++;
                }
            }
        }
    }

    if (count > 0) {
        avgR /= count;
        avgG /= count;
        avgB /= count;
    }

    // Apply: blend original toward average using mask as opacity
    for (int py = 0; py < bbH; ++py) {
        for (int px = 0; px < bbW; ++px) {
            float m = mask[py * bbW + px];
            if (m <= 0.f) continue;

            int ix = bbMinX + px;
            int iy = bbMinY + py;
            if (ix < 0 || ix >= imgW || iy < 0 || iy >= imgH) continue;

            img->r(iy, ix) = float((avgR - img->r(iy, ix)) * m + img->r(iy, ix));
            img->g(iy, ix) = float((avgG - img->g(iy, ix)) * m + img->g(iy, ix));
            img->b(iy, ix) = float((avgB - img->b(iy, ix)) * m + img->b(iy, ix));
        }
    }
}

// Veiling-glare reduction for the Remove Reflections brush: estimate the
// glare floor inside the painted region (low percentile per channel — a
// reflection adds a near-constant bright haze) and subtract it with a
// white-preserving rescale, feathered by the stroke mask. entry.opacity
// scales the strength.
void processStrokeReflect(Imagefloat* img, const SpotEntry& entry, const PreviewProps &pp,
                          const std::vector<const SpotEntry*>& priorStrokes)
{
    if (entry.strokePoints.empty()) return;

    int skip = pp.getSkip();
    int cropX = pp.getX();
    int cropY = pp.getY();
    int imgW = img->getWidth();
    int imgH = img->getHeight();
    float radius = float(entry.radius) / float(skip);
    // Guard against absurd feather values from hand-edited sidecars.
    float featherRadius = std::min(entry.getFeatherRadius() / float(skip),
                                   radius * 3.f + 32.f);

    int bbMinX = INT_MAX, bbMinY = INT_MAX, bbMaxX = INT_MIN, bbMaxY = INT_MIN;
    for (const auto& pt : entry.strokePoints) {
        int sx = (pt.x - cropX) / skip;
        int sy = (pt.y - cropY) / skip;
        bbMinX = std::min(bbMinX, sx - int(featherRadius) - 1);
        bbMinY = std::min(bbMinY, sy - int(featherRadius) - 1);
        bbMaxX = std::max(bbMaxX, sx + int(featherRadius) + 1);
        bbMaxY = std::max(bbMaxY, sy + int(featherRadius) + 1);
    }

    bbMinX = std::max(0, bbMinX);
    bbMinY = std::max(0, bbMinY);
    bbMaxX = std::min(imgW - 1, bbMaxX);
    bbMaxY = std::min(imgH - 1, bbMaxY);
    if (bbMinX > bbMaxX || bbMinY > bbMaxY) return;

    int bbW = bbMaxX - bbMinX + 1;
    int bbH = bbMaxY - bbMinY + 1;

    std::vector<float> mask(bbW * bbH, 0.f);
    for (const auto& pt : entry.strokePoints) {
        float cx = float(pt.x - cropX) / float(skip) - bbMinX;
        float cy = float(pt.y - cropY) / float(skip) - bbMinY;
        int pyMin = std::max(0, int(cy - featherRadius));
        int pyMax = std::min(bbH - 1, int(cy + featherRadius));
        int pxMin = std::max(0, int(cx - featherRadius));
        int pxMax = std::min(bbW - 1, int(cx + featherRadius));
        for (int py = pyMin; py <= pyMax; ++py) {
            for (int px = pxMin; px <= pxMax; ++px) {
                float dx = float(px) - cx;
                float dy = float(py) - cy;
                float dist = sqrtf(dx * dx + dy * dy);
                float val = 0.f;
                if (dist <= radius) {
                    val = 1.f;
                } else if (dist < featherRadius) {
                    val = (featherRadius - dist) / (featherRadius - radius);
                }
                mask[py * bbW + px] = std::max(mask[py * bbW + px], val);
            }
        }
    }

    if (entry.repairVersion < 2) {
        excludePriorStrokeCoverage(mask, bbW, bbH, bbMinX, bbMinY, cropX, cropY, skip, priorStrokes);
    } else {
        for (auto& value : mask) value = repairmath::smoothstep(value);
    }

    // A reflection or glare rides on the scene as a bright LOW-FREQUENCY
    // layer. Estimate that layer per pixel (blurred min-channel) and remove
    // only its EXCESS over the calmest painted area — a uniformly bright
    // surface then stays as it is, instead of being darkened wholesale.
    array2D<float> veil(bbW, bbH);
    for (int py = 0; py < bbH; ++py) {
        for (int px = 0; px < bbW; ++px) {
            const int ix = bbMinX + px;
            const int iy = bbMinY + py;
            veil[py][px] = std::min(img->r(iy, ix), std::min(img->g(iy, ix), img->b(iy, ix)));
        }
    }
    array2D<float> veilBlur(bbW, bbH);
    const int veilRadius = std::max(4, std::min(std::min(bbW, bbH) / 4, int(featherRadius)));
    boxblur(static_cast<float**>(veil), static_cast<float**>(veilBlur), veilRadius, bbW, bbH, true);
    if (entry.repairVersion >= 2 && bbW >= 8 && bbH >= 8) {
        array2D<float> guide(bbW, bbH), estimate(bbW, bbH);
        for (int y = 0; y < bbH; ++y) {
            for (int x = 0; x < bbW; ++x) {
                guide[y][x] = std::max(0.f, veil[y][x]) / 65535.f;
                estimate[y][x] = std::max(0.f, veilBlur[y][x]) / 65535.f;
            }
        }
        guidedFilter(guide, estimate, veilBlur, std::max(1, veilRadius / 2), .0025f, true, 1);
        for (int y = 0; y < bbH; ++y)
            for (int x = 0; x < bbW; ++x) veilBlur[y][x] *= 65535.f;
    }

    // The anchor: the least-hazy tenth of the painted region keeps its look.
    std::vector<float> veilSamples;
    for (int py = 0; py < bbH; py += 2) {
        for (int px = 0; px < bbW; px += 2) {
            if (mask[py * bbW + px] > 0.5f) {
                veilSamples.push_back(veilBlur[py][px]);
            }
        }
    }
    if (veilSamples.size() < 8) return;

    const size_t n = std::max<size_t>(1, veilSamples.size() / 10);
    std::nth_element(veilSamples.begin(), veilSamples.begin() + (n - 1), veilSamples.end());
    const float veilFloor = veilSamples[n - 1];

    const float strength = 0.9f * std::max(0.f, std::min(1.f, entry.opacity));

    for (int py = 0; py < bbH; ++py) {
        for (int px = 0; px < bbW; ++px) {
            const float m = mask[py * bbW + px];
            if (m <= 0.f) continue;
            float g = strength * std::max(0.f, veilBlur[py][px] - veilFloor);
            if (g < 1.f || g >= 65534.f) continue;
            const int ix = bbMinX + px;
            const int iy = bbMinY + py;
            if (entry.repairVersion >= 2) {
                const float lo = std::max(0.f, std::min({img->r(iy, ix), img->g(iy, ix), img->b(iy, ix)}));
                const float hi = std::max({img->r(iy, ix), img->g(iy, ix), img->b(iy, ix), 1.f});
                // Bound subtraction before any channel can go negative, and
                // protect saturated colors and deep shadows from hue shifts.
                const float protection = repairmath::smoothstep(lo / 6000.f) * (.35f + .65f * lo / hi);
                g = std::min(g * protection, .45f * lo);
            }
            const float rescale = 65535.f / (65535.f - g);
            const auto unveil = [g, rescale](float v) {
                return std::max(0.f, (v - g) * rescale);
            };
            img->r(iy, ix) = img->r(iy, ix) * (1.f - m) + unveil(img->r(iy, ix)) * m;
            img->g(iy, ix) = img->g(iy, ix) * (1.f - m) + unveil(img->g(iy, ix)) * m;
            img->b(iy, ix) = img->b(iy, ix) * (1.f - m) + unveil(img->b(iy, ix)) * m;
        }
    }
}

#ifdef RT_AI_MASKING
// Inpainting is expensive (a full model inference per stroke) and the spot
// stage re-runs every stroke whenever the entry list changes — undo/redo,
// adding a second stroke, toggling the tool. Memoize results keyed by the
// actual input content, so replays cost a hash instead of an inference.
struct InpaintCacheEntry {
    std::uint64_t key;
    int bbW;
    int bbH;
    std::vector<float> outR, outG, outB;
};

std::mutex inpaintCacheMutex;
std::deque<InpaintCacheEntry> inpaintCache;
constexpr std::size_t MAX_INPAINT_CACHE_ENTRIES = 8;
constexpr int MAX_INPAINT_CACHE_PIXELS = 2 * 1024 * 1024;

std::uint64_t hashFloats(std::uint64_t h, const float* data, int count, int stride)
{
    for (int i = 0; i < count; i += stride) {
        std::uint32_t bits;
        std::memcpy(&bits, &data[i], sizeof(bits));
        h ^= bits;
        h *= 1099511628211ULL; // FNV-1a
    }
    return h;
}

// ---------------------------------------------------------------------------
// Full-image-space AI repairs (Phase B/C of the Gen Fill quality plan).
//
// Each AI stroke is computed ONCE, at full resolution, in full-image
// coordinates, from a fresh region pull off the ImageSource — then cached
// and blended into whatever view asks for it (preview, 1:1 crops, export).
// Every view therefore shows the same repair, and panning or re-rendering
// costs a table lookup instead of an inference.
// ---------------------------------------------------------------------------

struct AIPatch {
    std::uint64_t key;
    int x0, y0, w, h;                       // full-image coordinates, skip 1
    std::vector<float> fillR, fillG, fillB; // the repair, linear [0,65535]
    std::vector<float> mask;                // soft blend mask 0..1
    bool delta = false;
    int fullW = 0, fullH = 0;
    std::vector<float> workR, workG, workB;
    procparams::RAWParams raw;
    procparams::ColorManagementParams icm;
};

std::mutex aiPatchMutex;
std::condition_variable aiPatchCv;
std::unordered_set<std::uint64_t> aiPatchPending; // keys whose inference is running right now
std::deque<std::shared_ptr<const AIPatch>> aiPatchCache;
// Sized so an edit session with many strokes never thrashes: a FIFO of 4
// meant a 5th stroke forced every patch to recompute on every render.
constexpr std::size_t MAX_AI_PATCHES = 24;
// Per-patch cap sized so big generative fills stay CACHED — a stroke that
// falls to the uncached legacy path re-infers on every view and makes undo
// crawl. 9M px covers a ~3500x2600 padded fill.
constexpr long long MAX_AI_PATCH_PIXELS = 9000000;
constexpr long long MAX_AI_PATCH_TOTAL_PIXELS = 36000000; // whole cache (~576MB at 16 bytes/px)

std::uint64_t hashInts(std::uint64_t h, const int* data, int count)
{
    for (int i = 0; i < count; ++i) {
        h ^= static_cast<std::uint32_t>(data[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

std::uint64_t hashDouble(std::uint64_t h, double v)
{
    std::uint64_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    h ^= bits;
    h *= 1099511628211ULL;
    return h;
}

std::uint64_t hashString(std::uint64_t h, const std::string& s)
{
    for (const char c : s) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ULL;
    }
    return h;
}

std::uint64_t hashEntryChain(const SpotEntry& entry,
                             const std::vector<const SpotEntry*>& priorStrokes,
                             const ColorTemp& currWB, int tr,
                             const procparams::ProcParams* params,
                             const std::string& fileName)
{
    std::uint64_t h = 14695981039346656037ULL;
    const auto hashEntry = [&h](const SpotEntry& e) {
        const int meta[] = {static_cast<int>(e.method), e.radius,
                            static_cast<int>(e.feather * 1000.f),
                            static_cast<int>(e.opacity * 1000.f)};
        h = hashInts(h, meta, 4);
        if (e.repairVersion >= 2) h = hashInts(h, &e.repairVersion, 1);
        for (const auto& pt : e.strokePoints) {
            const int xy[] = {pt.x, pt.y};
            h = hashInts(h, xy, 2);
        }
        if (!e.objectPoints.empty()) {
            for (const auto& pt : e.objectPoints) {
                const int v[] = {pt.x, pt.y, pt.include ? 1 : 0};
                h = hashInts(h, v, 3);
            }
            const int opts[] = {static_cast<int>(e.objectGrow * 1000.f), e.objectShadow ? 1 : 0, e.objectSize};
            h = hashInts(h, opts, 3);
            // The outline is whatever the installed selection model draws.
            h = hashString(h, SamSelector::getInstance().modelTag());
        }
        if (e.repairVersion >= 3 && e.highQuality && AIRemoverEngine::getInstance().available()) {
            const int hq = 1;
            h = hashInts(h, &hq, 1);
        }
    };
    hashEntry(entry);
    for (const auto* prior : priorStrokes) {
        hashEntry(*prior);
    }
    h = hashDouble(h, currWB.getTemp());
    h = hashDouble(h, currWB.getGreen());
    h = hashInts(h, &tr, 1);
    // Image identity: the cache is process-global, so patches must never
    // leak between images (editor tabs, before/after, batch queue).
    h = hashString(h, fileName);
    // Not named "stat": on POSIX g_stat is a macro for stat(), which a
    // local of that name would shadow.
    GStatBuf fileStat;
    if (entry.repairVersion >= 2 && g_stat(fileName.c_str(), &fileStat) == 0) {
        h = hashDouble(h, double(fileStat.st_size));
        h = hashDouble(h, double(fileStat.st_mtime));
    }
    if (entry.repairVersion >= 2) {
        h = hashDouble(h, currWB.getEqual());
        const int observer = int(currWB.getObserver());
        h = hashInts(h, &observer, 1);
    }
    // The upstream knobs getImage actually consumes. Deliberately NOT
    // toneCurve.expcomp: getImage never reads it, and auto-exposure
    // rewrites it mid-pass, which used to guarantee a cache miss on the
    // 1:1 crop pass — a full re-inference of every stroke on every zoom.
    const int tone[] = {params->toneCurve.hrenabled ? 1 : 0,
                        params->toneCurve.clampOOG ? 1 : 0,
                        params->toneCurve.hlbl,
                        params->raw.bayersensor.imageNum};
    h = hashInts(h, tone, 4);
    h = hashDouble(h, params->toneCurve.hlth);
    h = hashString(h, params->toneCurve.method.raw());
    h = hashDouble(h, params->raw.expos);
    h = hashDouble(h, params->raw.bayersensor.black0);
    h = hashDouble(h, params->raw.bayersensor.black1);
    h = hashDouble(h, params->raw.bayersensor.black2);
    h = hashDouble(h, params->raw.bayersensor.black3);
    h = hashDouble(h, params->raw.xtranssensor.blackred);
    h = hashDouble(h, params->raw.xtranssensor.blackgreen);
    h = hashDouble(h, params->raw.xtranssensor.blackblue);
    h = hashString(h, params->raw.dark_frame.raw());
    h = hashString(h, params->raw.ff_file.raw());
    h = hashString(h, params->raw.bayersensor.method.raw());
    h = hashString(h, params->raw.xtranssensor.method.raw());
    // The model runs in sRGB derived from these profiles.
    h = hashString(h, params->icm.inputProfile.raw());
    h = hashString(h, params->icm.workingProfile.raw());
    return h;
}

// Camera→sRGB for the model: LaMa is trained on sRGB imagery, and feeding
// it camera primaries drifts hallucinated colors (greenish fills). The
// pipeline's camera→working conversion isn't exposed as a matrix, so probe
// it: run convertColorSpace on unit-basis pixels, read the matrix off, and
// verify linearity with a fourth (neutral) probe — a non-linear transform
// (LCMS gamut mapping for custom input profiles) fails the check and the
// caller skips the mapping entirely.
bool probeCameraToSRGB(ImageSource* imgsrc, const ColorTemp& currWB,
                       const procparams::ProcParams* params,
                       float toSRGB[3][3], float fromSRGB[3][3])
{
    constexpr float E = 8000.f;
    Imagefloat probe(3, 2);
    probe.r(0, 0) = E;   probe.g(0, 0) = 0.f; probe.b(0, 0) = 0.f;
    probe.r(0, 1) = 0.f; probe.g(0, 1) = E;   probe.b(0, 1) = 0.f;
    probe.r(0, 2) = 0.f; probe.g(0, 2) = 0.f; probe.b(0, 2) = E;
    probe.r(1, 0) = E;   probe.g(1, 0) = E;   probe.b(1, 0) = E;
    // Second neutral at 4x: a per-channel tone curve (LCMS profiles on
    // non-raw sources, DCP input profiles with LUTs) passes the additivity
    // check at one level but fails proportionality between levels.
    probe.r(1, 1) = 4.f * E; probe.g(1, 1) = 4.f * E; probe.b(1, 1) = 4.f * E;
    probe.r(1, 2) = 0.f; probe.g(1, 2) = 0.f; probe.b(1, 2) = 0.f;
    imgsrc->convertColorSpace(&probe, params->icm, currWB);

    float camToWork[3][3];
    camToWork[0][0] = probe.r(0, 0) / E; camToWork[0][1] = probe.r(0, 1) / E; camToWork[0][2] = probe.r(0, 2) / E;
    camToWork[1][0] = probe.g(0, 0) / E; camToWork[1][1] = probe.g(0, 1) / E; camToWork[1][2] = probe.g(0, 2) / E;
    camToWork[2][0] = probe.b(0, 0) / E; camToWork[2][1] = probe.b(0, 1) / E; camToWork[2][2] = probe.b(0, 2) / E;

    const float neutral[3] = {probe.r(1, 0), probe.g(1, 0), probe.b(1, 0)};
    const float neutral4[3] = {probe.r(1, 1), probe.g(1, 1), probe.b(1, 1)};
    for (int c = 0; c < 3; ++c) {
        const float lin = (camToWork[c][0] + camToWork[c][1] + camToWork[c][2]) * E;
        if (std::fabs(lin - neutral[c]) > 0.02f * E) {
            return false; // not additive: full transform, not a matrix
        }
        if (std::fabs(4.f * neutral[c] - neutral4[c]) > 0.08f * E) {
            return false; // not proportional: tone curve in the loop
        }
    }

    const TMatrix workToXYZ = ICCStore::getInstance()->workingSpaceMatrix(params->icm.workingProfile);
    const TMatrix xyzToSRGB = ICCStore::getInstance()->workingSpaceInverseMatrix("sRGB");
    float workToSRGB[3][3] = {};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            for (int k = 0; k < 3; ++k) {
                workToSRGB[r][c] += static_cast<float>(xyzToSRGB[r][k] * workToXYZ[k][c]);
            }
        }
    }

    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            toSRGB[r][c] = 0.f;
            for (int k = 0; k < 3; ++k) {
                toSRGB[r][c] += workToSRGB[r][k] * camToWork[k][c];
            }
        }
    }

    const float det =
        toSRGB[0][0] * (toSRGB[1][1] * toSRGB[2][2] - toSRGB[1][2] * toSRGB[2][1])
      - toSRGB[0][1] * (toSRGB[1][0] * toSRGB[2][2] - toSRGB[1][2] * toSRGB[2][0])
      + toSRGB[0][2] * (toSRGB[1][0] * toSRGB[2][1] - toSRGB[1][1] * toSRGB[2][0]);
    if (std::fabs(det) < 1e-6f) {
        return false;
    }
    const float id = 1.f / det;
    fromSRGB[0][0] = (toSRGB[1][1] * toSRGB[2][2] - toSRGB[1][2] * toSRGB[2][1]) * id;
    fromSRGB[0][1] = (toSRGB[0][2] * toSRGB[2][1] - toSRGB[0][1] * toSRGB[2][2]) * id;
    fromSRGB[0][2] = (toSRGB[0][1] * toSRGB[1][2] - toSRGB[0][2] * toSRGB[1][1]) * id;
    fromSRGB[1][0] = (toSRGB[1][2] * toSRGB[2][0] - toSRGB[1][0] * toSRGB[2][2]) * id;
    fromSRGB[1][1] = (toSRGB[0][0] * toSRGB[2][2] - toSRGB[0][2] * toSRGB[2][0]) * id;
    fromSRGB[1][2] = (toSRGB[0][2] * toSRGB[1][0] - toSRGB[0][0] * toSRGB[1][2]) * id;
    fromSRGB[2][0] = (toSRGB[1][0] * toSRGB[2][1] - toSRGB[1][1] * toSRGB[2][0]) * id;
    fromSRGB[2][1] = (toSRGB[0][1] * toSRGB[2][0] - toSRGB[0][0] * toSRGB[2][1]) * id;
    fromSRGB[2][2] = (toSRGB[0][0] * toSRGB[1][1] - toSRGB[0][1] * toSRGB[1][0]) * id;
    return true;
}

// Area-average (shrink) / bilinear (grow) for a single plane.
void resamplePlaneSpot(const float* src, int sw, int sh, float* dst, int dw, int dh)
{
    if (dw == sw && dh == sh) {
        std::memcpy(dst, src, static_cast<size_t>(sw) * sh * sizeof(float));
        return;
    }
    if (static_cast<long long>(dw) * dh < static_cast<long long>(sw) * sh) {
        for (int y = 0; y < dh; ++y) {
            const int y0 = static_cast<int>(static_cast<float>(y) * sh / dh);
            const int y1 = std::min(sh, std::max(y0 + 1,
                static_cast<int>(std::ceil(static_cast<float>(y + 1) * sh / dh))));
            for (int x = 0; x < dw; ++x) {
                const int x0 = static_cast<int>(static_cast<float>(x) * sw / dw);
                const int x1 = std::min(sw, std::max(x0 + 1,
                    static_cast<int>(std::ceil(static_cast<float>(x + 1) * sw / dw))));
                double sum = 0.0;
                for (int yy = y0; yy < y1; ++yy) {
                    for (int xx = x0; xx < x1; ++xx) {
                        sum += src[yy * sw + xx];
                    }
                }
                dst[y * dw + x] = static_cast<float>(sum / ((y1 - y0) * (x1 - x0)));
            }
        }
    } else {
        for (int y = 0; y < dh; ++y) {
            const float sy = dh > 1 ? static_cast<float>(y) * (sh - 1) / (dh - 1) : 0.f;
            const int y0 = std::min(static_cast<int>(sy), sh - 1);
            const int y1 = std::min(y0 + 1, sh - 1);
            const float wy = sy - y0;
            for (int x = 0; x < dw; ++x) {
                const float sx = dw > 1 ? static_cast<float>(x) * (sw - 1) / (dw - 1) : 0.f;
                const int x0 = std::min(static_cast<int>(sx), sw - 1);
                const int x1 = std::min(x0 + 1, sw - 1);
                const float wx = sx - x0;
                const float top = src[y0 * sw + x0] + wx * (src[y0 * sw + x1] - src[y0 * sw + x0]);
                const float bot = src[y1 * sw + x0] + wx * (src[y1 * sw + x1] - src[y1 * sw + x0]);
                dst[y * dw + x] = top + wy * (bot - top);
            }
        }
    }
}

// Run the model over a large region as overlapping full-resolution tiles:
// a reduced-size pass establishes structure, seeds the masked area, and each
// tile regenerates crisp texture on top with feather-blended overlaps.
std::vector<float> distanceToSet(const std::vector<unsigned char>& set, int w, int h);

// How wide the hole is at its widest: twice the largest distance from a
// masked pixel to the nearest pixel that is kept, in region pixels.
float holeThickness(const std::vector<float>& mask, int w, int h)
{
    const int step = std::max(1, std::max(w, h) / 256);
    const int gw = (w + step - 1) / step, gh = (h + step - 1) / step;
    // A one-cell kept border, so a hole touching the region's edge still
    // measures from that edge rather than reading as endless.
    std::vector<unsigned char> kept(size_t(gw + 2) * (gh + 2), 1);
    for (int y = 0; y < gh; ++y) {
        for (int x = 0; x < gw; ++x) {
            kept[size_t(y + 1) * (gw + 2) + x + 1] =
                mask[size_t(std::min(h - 1, y * step + step / 2)) * w + std::min(w - 1, x * step + step / 2)] <= .05f;
        }
    }
    const std::vector<float> dist = distanceToSet(kept, gw + 2, gh + 2);
    return 2.f * step * *std::max_element(dist.begin(), dist.end());
}

// The fill model keeps structure (edges, horizons, repeating elements) only
// while the hole is narrow enough for it to see across: measured on test
// fills, about 215 model pixels still held straight lines and 340 turned to
// a blur. Wider holes are therefore shown to it smaller.
constexpr float MODEL_HOLE_THICKNESS = 224.f;

bool inpaintTiled(AIInpaintingEngine& engine,
                  const std::vector<float>& inR, const std::vector<float>& inG,
                  const std::vector<float>& inB, const std::vector<float>& mask,
                  int w, int h,
                  std::vector<float>& fillR, std::vector<float>& fillG,
                  std::vector<float>& fillB, int version, const SmartRepairJob& job,
                  float thickness = 0.f, bool highQuality = false, std::uint32_t seed = 0)
{
    const long long pixels = static_cast<long long>(w) * h;

    // Pass 1: structure at reduced size.
    double structScale = version >= 2 ? std::min(1., double(engine.tileSize(version)) / std::max(w, h)) : std::sqrt(500000.0 / pixels);
    if (version >= 3 && thickness > 0.f) {
        // Never coarser than the 512 px pass of version 2.
        structScale = std::max(std::min(1., 512. / std::max(w, h)),
                               std::min(structScale, double(MODEL_HOLE_THICKNESS) / thickness));
    }
    int sw = std::max(version >= 2 ? 1 : 64, static_cast<int>(w * structScale));
    int sh = std::max(version >= 2 ? 1 : 64, static_cast<int>(h * structScale));
    const auto resizePlane = version >= 2 ? repairmath::resample : resamplePlaneSpot;
    std::vector<float> smallR(sw * sh), smallG(sw * sh), smallB(sw * sh), smallM(sw * sh);
    resizePlane(inR.data(), w, h, smallR.data(), sw, sh);
    resizePlane(inG.data(), w, h, smallG.data(), sw, sh);
    resizePlane(inB.data(), w, h, smallB.data(), sw, sh);
    resizePlane(mask.data(), w, h, smallM.data(), sw, sh);

    std::vector<float> structR(sw * sh), structG(sw * sh), structB(sw * sh);
    bool structured = false;
    // Only where the convolutional model would have to shrink the hole to
    // see across it; on a narrow hole it matches texture and tone better.
    const bool wideHole = thickness * std::min(1., double(engine.tileSize(version)) / std::max(w, h)) > .8f * MODEL_HOLE_THICKNESS;
    if (version >= 3 && highQuality && wideHole && AIRemoverEngine::getInstance().available()) {
        // A wide hole: the diffusion remover decides what the scene is. It
        // takes the hole at any width, so it gets the largest view it works
        // at rather than the reduced one the convolutional model needs.
        const double hqScale = std::min(1., double(AIRemoverEngine::maxSide) / std::max(w, h));
        const int hw = std::max(16, static_cast<int>(w * hqScale)), hh = std::max(16, static_cast<int>(h * hqScale));
        std::vector<float> viewR(hw * hh), viewG(hw * hh), viewB(hw * hh), viewM(hw * hh);
        resizePlane(inR.data(), w, h, viewR.data(), hw, hh);
        resizePlane(inG.data(), w, h, viewG.data(), hw, hh);
        resizePlane(inB.data(), w, h, viewB.data(), hw, hh);
        resizePlane(mask.data(), w, h, viewM.data(), hw, hh);
        std::vector<float> outR(hw * hh), outG(hw * hh), outB(hw * hh);
        if (AIRemoverEngine::getInstance().fill(viewR.data(), viewG.data(), viewB.data(), viewM.data(), hw, hh,
                                                outR.data(), outG.data(), outB.data(), seed, job)) {
            // The model re-renders the whole view through its own image
            // codec, which shifts tone slightly. Measure that shift where
            // the truth is known (outside the hole) and take it out, or the
            // fill sits in the picture as a faintly lighter or darker patch.
            float* outs[] = {outR.data(), outG.data(), outB.data()};
            const float* views[] = {viewR.data(), viewG.data(), viewB.data()};
            for (int c = 0; c < 3; ++c) {
                double sumView = 0., sumOut = 0.;
                for (int i = 0; i < hw * hh; ++i) {
                    if (viewM[i] <= .001f) {
                        sumView += views[c][i];
                        sumOut += outs[c][i];
                    }
                }
                const float gain = sumOut > 1. ? std::max(.8f, std::min(1.25f, float(sumView / sumOut))) : 1.f;
                for (int i = 0; i < hw * hh; ++i) outs[c][i] *= gain;
            }
            sw = hw;
            sh = hh;
            structR = std::move(outR);
            structG = std::move(outG);
            structB = std::move(outB);
            structured = true;
        }
    }
    if (!structured && !engine.inpaint(smallR.data(), smallG.data(), smallB.data(), smallM.data(),
                                       sw, sh, structR.data(), structG.data(), structB.data(), version, job)) {
        return false;
    }

    // Seed: original content outside the mask, upscaled structure inside.
    std::vector<float> seedR(pixels), seedG(pixels), seedB(pixels);
    resizePlane(structR.data(), sw, sh, seedR.data(), w, h);
    resizePlane(structG.data(), sw, sh, seedG.data(), w, h);
    resizePlane(structB.data(), sw, sh, seedB.data(), w, h);
    for (long long i = 0; i < pixels; ++i) {
        if (mask[i] <= 0.05f) {
            seedR[i] = inR[i];
            seedG[i] = inG[i];
            seedB[i] = inB[i];
        }
    }

    if (structured) {
        // The diffusion remover's view is a finished picture, detail
        // included. The frequency split below exists because the
        // convolutional structure pass is only trustworthy in its low
        // frequencies; applied here it would blur away what was just gained
        // and spend most of the repair's time doing it.
        fillR = std::move(seedR);
        fillG = std::move(seedG);
        fillB = std::move(seedB);
        return true;
    }

    // Frequency split: the fill's low frequencies come from the ONE global
    // structure pass; tiles contribute only local texture. Tiles cannot see
    // each other (the model ignores masked input), so without this each
    // tile hallucinated its own continuation of lines entering the region
    // and the feathered overlaps turned that disagreement into phantom
    // edges. The blur also attenuates thin line ghosts the structure pass
    // itself hallucinates.
    constexpr int SPLIT_RADIUS = 12;
    const auto lowpass = [version](float* src, float* dst, int width, int height) {
        if (version < 2) { boxblur(src, dst, SPLIT_RADIUS, width, height, false); return; }
        std::vector<float*> rows(height), outRows(height);
        for (int y = 0; y < height; ++y) {
            rows[y] = const_cast<float*>(src + size_t(y) * width);
            outRows[y] = dst + size_t(y) * width;
        }
        gaussianBlur(rows.data(), outRows.data(), width, height, 5.0);
    };
    std::vector<float> lowR(pixels), lowG(pixels), lowB(pixels);
    lowpass(seedR.data(), lowR.data(), w, h);
    lowpass(seedG.data(), lowG.data(), w, h);
    lowpass(seedB.data(), lowB.data(), w, h);

    // Pass 2: overlapping tiles over the masked region. 640px measured
    // fastest end-to-end: LaMa's FFT cost grows superlinearly, so fewer
    // bigger tiles (768) actually lose to more smaller ones.
    const int TILE = version >= 2 ? engine.tileSize(version) : 640;
    const int STRIDE = std::max(1, TILE - 128);
    constexpr float RAMP = 64.f; // feather width inside the overlap

    std::vector<float> accR(pixels, 0.f), accG(pixels, 0.f), accB(pixels, 0.f), accW(pixels, 0.f);
    std::vector<float> tileR(TILE * TILE), tileG(TILE * TILE), tileB(TILE * TILE), tileM(TILE * TILE);
    std::vector<float> tofR(TILE * TILE), tofG(TILE * TILE), tofB(TILE * TILE);
    std::vector<float> tlowR(TILE * TILE), tlowG(TILE * TILE), tlowB(TILE * TILE);

    for (int ty = 0; ty < h; ty += STRIDE) {
        if (job) job->checkpoint();
        const int y0 = std::min(ty, std::max(0, h - TILE));
        const int th = std::min(TILE, h - y0);
        for (int tx = 0; tx < w; tx += STRIDE) {
            const int x0 = std::min(tx, std::max(0, w - TILE));
            const int tw = std::min(TILE, w - x0);

            long long maskedCount = 0;
            for (int y = 0; y < th; ++y) {
                for (int x = 0; x < tw; ++x) {
                    if (mask[static_cast<long long>(y0 + y) * w + x0 + x] > 0.05f) {
                        ++maskedCount;
                    }
                }
            }
            if (maskedCount == 0) {
                if (x0 + TILE >= w) break;
                continue;
            }

            for (int y = 0; y < th; ++y) {
                const long long srcRow = static_cast<long long>(y0 + y) * w + x0;
                std::memcpy(&tileR[y * tw], &seedR[srcRow], tw * sizeof(float));
                std::memcpy(&tileG[y * tw], &seedG[srcRow], tw * sizeof(float));
                std::memcpy(&tileB[y * tw], &seedB[srcRow], tw * sizeof(float));
                std::memcpy(&tileM[y * tw], &mask[srcRow], tw * sizeof(float));
            }

            if (!engine.inpaint(tileR.data(), tileG.data(), tileB.data(), tileM.data(),
                                tw, th, tofR.data(), tofG.data(), tofB.data(), version, job)) {
                return false;
            }

            // The tile's own low frequencies are its private hallucination —
            // discard them inside the mask and ride on the shared structure.
            lowpass(tofR.data(), tlowR.data(), tw, th);
            lowpass(tofG.data(), tlowG.data(), tw, th);
            lowpass(tofB.data(), tlowB.data(), tw, th);

            for (int y = 0; y < th; ++y) {
                const float wy = version >= 2 ? repairmath::overlapWeight(y, th, y0 == 0, y0 + th == h, 128)
                    : std::min({1.f, (y + 1) / RAMP, (th - y) / RAMP});
                for (int x = 0; x < tw; ++x) {
                    const float wx = version >= 2 ? repairmath::overlapWeight(x, tw, x0 == 0, x0 + tw == w, 128)
                        : std::min({1.f, (x + 1) / RAMP, (tw - x) / RAMP});
                    const float wgt = std::max(0.01f, wy * wx);
                    const long long di = static_cast<long long>(y0 + y) * w + x0 + x;
                    const long long si = static_cast<long long>(y) * tw + x;
                    float vr = tofR[si], vg = tofG[si], vb = tofB[si];
                    if (mask[di] > 0.05f) {
                        // Where a tile invents a different edge, do not paste
                        // its high-pass outline on top of the global structure.
                        const float mismatch = (std::fabs(tlowR[si] - lowR[di]) + std::fabs(tlowG[si] - lowG[di]) + std::fabs(tlowB[si] - lowB[di])) / 3.f;
                        const float agreement = version >= 2 ? 1.f / (1.f + mismatch / 900.f) : 1.f;
                        vr = lowR[di] + agreement * (tofR[si] - tlowR[si]);
                        vg = lowG[di] + agreement * (tofG[si] - tlowG[si]);
                        vb = lowB[di] + agreement * (tofB[si] - tlowB[si]);
                    }
                    accR[di] += vr * wgt;
                    accG[di] += vg * wgt;
                    accB[di] += vb * wgt;
                    accW[di] += wgt;
                }
            }

            if (x0 + TILE >= w) break;
        }
        if (ty + TILE >= h) break;
    }

    fillR.resize(pixels);
    fillG.resize(pixels);
    fillB.resize(pixels);
    for (long long i = 0; i < pixels; ++i) {
        if (accW[i] > 0.f) {
            fillR[i] = accR[i] / accW[i];
            fillG[i] = accG[i] / accW[i];
            fillB[i] = accB[i] / accW[i];
        } else {
            fillR[i] = seedR[i];
            fillG[i] = seedG[i];
            fillB[i] = seedB[i];
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Picked-object mask for Remove Object.
//
// A brush stroke only covers what the hand painted; any sliver of the object
// left outside it is background to the fill model, which then rebuilds the
// object from it. A picked entry instead asks the click-selection model for
// the object's own outline on an untransformed copy of the frame (spots live
// in pre-transform coordinates), and the repair area is that outline grown
// outward. The result is kept as a distance field so growth and the blend
// ramp can change without running the model again.
struct ObjectMask {
    std::uint64_t key = 0;
    int skip = 1, w = 0, h = 0;          // grid over the whole frame
    std::vector<float> dist;             // distance outside the object in full-res px, 0 inside
    float size = 0.f;                    // sqrt(area) in full-res px
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // object bounds in full-res px
    // Which of the selector's nested readings this is (0 smallest), and
    // whether a distinctly smaller or larger one was on offer.
    int candidate = -1;
    bool hasSmaller = false, hasLarger = false;

    float distanceAt(int fx, int fy) const
    {
        const float gx = std::max(0.f, std::min(float(w - 1), (fx + .5f) / skip - .5f));
        const float gy = std::max(0.f, std::min(float(h - 1), (fy + .5f) / skip - .5f));
        const int ix = int(gx), iy = int(gy);
        const int jx = std::min(w - 1, ix + 1), jy = std::min(h - 1, iy + 1);
        const float tx = gx - ix, ty = gy - iy;
        const float* a = &dist[size_t(iy) * w];
        const float* b = &dist[size_t(jy) * w];
        return (a[ix] * (1.f - tx) + a[jx] * tx) * (1.f - ty) + (b[ix] * (1.f - tx) + b[jx] * tx) * ty;
    }
};

std::mutex objectMaskMutex;
std::deque<std::shared_ptr<const ObjectMask>> objectMaskCache;
constexpr std::size_t MAX_OBJECT_MASKS = 8;
constexpr int OBJECT_MASK_DIMENSION = 1600;
// A click that selects this much of the frame hit the sky, a wall or the
// ground, not an object; removing it would be a surprise, so the entry falls
// back to a plain brush dab.
constexpr float OBJECT_MAX_FRAME_SHARE = 0.35f;
// How far below the selector's best-rated reading a larger one may score and
// still be taken, and the growth in area from one reading to the next that
// counts as a jump (see getObjectMask).
constexpr float OBJECT_SCORE_SLACK = 0.12f;
constexpr float OBJECT_JUMP = 4.f;

// Exact Euclidean distance (Felzenszwalb): one pass of lower envelopes.
void squaredDistance1D(const float* f, float* d, int n, std::vector<int>& v, std::vector<double>& z)
{
    int k = 0;
    v[0] = 0;
    z[0] = -1e30;
    z[1] = 1e30;
    for (int q = 1; q < n; ++q) {
        double s;
        for (;;) {
            const int p = v[k];
            s = ((double(f[q]) + double(q) * q) - (double(f[p]) + double(p) * p)) / (2. * (q - p));
            if (s <= z[k] && k > 0) {
                --k;
            } else {
                break;
            }
        }
        ++k;
        v[k] = q;
        z[k] = s;
        z[k + 1] = 1e30;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[k + 1] < q) {
            ++k;
        }
        const double dq = q - v[k];
        d[q] = float(dq * dq + f[v[k]]);
    }
}

// Distance in grid cells from every cell to the nearest set cell (0 on it).
std::vector<float> distanceToSet(const std::vector<unsigned char>& set, int w, int h)
{
    constexpr float FAR = 1e12f;
    std::vector<float> d(size_t(w) * h);
    const int n = std::max(w, h);
    std::vector<float> f(n), out(n);
    std::vector<int> v(n);
    std::vector<double> z(n + 1);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) f[x] = set[size_t(y) * w + x] ? 0.f : FAR;
        squaredDistance1D(f.data(), out.data(), w, v, z);
        std::copy(out.begin(), out.begin() + w, d.begin() + size_t(y) * w);
    }
    for (int x = 0; x < w; ++x) {
        for (int y = 0; y < h; ++y) f[y] = d[size_t(y) * w + x];
        squaredDistance1D(f.data(), out.data(), h, v, z);
        for (int y = 0; y < h; ++y) d[size_t(y) * w + x] = std::sqrt(std::min(out[y], FAR));
    }
    return d;
}

// Keep the connected pieces the include clicks landed on (the largest piece
// when a click fell into a gap). The selection model sometimes adds specks
// elsewhere in the frame, and one of those would stretch the repair region
// across the picture. Returns the kept area in cells.
long long keepClickedPieces(std::vector<unsigned char>& set, int w, int h,
                            const std::vector<std::array<int, 2>>& clicks)
{
    std::vector<int> label(size_t(w) * h, 0);
    std::vector<long long> area(1, 0);
    std::vector<int> stack;
    for (int start = 0; start < w * h; ++start) {
        if (!set[start] || label[start]) continue;
        const int id = int(area.size());
        area.push_back(0);
        label[start] = id;
        stack.push_back(start);
        while (!stack.empty()) {
            const int i = stack.back();
            stack.pop_back();
            ++area[id];
            const int x = i % w, y = i / w;
            const int next[4] = {x > 0 ? i - 1 : -1, x < w - 1 ? i + 1 : -1, y > 0 ? i - w : -1, y < h - 1 ? i + w : -1};
            for (int j : next) {
                if (j >= 0 && set[j] && !label[j]) {
                    label[j] = id;
                    stack.push_back(j);
                }
            }
        }
    }
    if (area.size() == 1) return 0;

    std::vector<unsigned char> keep(area.size(), 0);
    bool any = false;
    for (const auto& c : clicks) {
        // A click right on an outline may sit one cell outside the piece.
        for (int dy = -2; dy <= 2; ++dy) {
            for (int dx = -2; dx <= 2; ++dx) {
                const int x = c[0] + dx, y = c[1] + dy;
                if (x < 0 || y < 0 || x >= w || y >= h) continue;
                const int id = label[size_t(y) * w + x];
                if (id) {
                    keep[id] = 1;
                    any = true;
                }
            }
        }
    }
    if (!any) {
        keep[std::max_element(area.begin() + 1, area.end()) - area.begin()] = 1;
    }
    long long kept = 0;
    for (size_t i = 0; i < set.size(); ++i) {
        if (set[i] && !keep[label[i]]) set[i] = 0;
        kept += set[i];
    }
    return kept;
}

// Add the cast shadow attached to the object's base: cells clearly darker
// than the ground around the object, reachable from its lower part without
// leaving the dark area. Deliberately conservative -- a wrong shadow removes
// real picture content, so anything implausibly large is dropped whole.
void addAttachedShadow(std::vector<unsigned char>& set, const std::vector<float>& luma, int w, int h, long long area)
{
    int by0 = h, by1 = -1;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (set[size_t(y) * w + x]) {
                by0 = std::min(by0, y);
                by1 = std::max(by1, y);
            }
        }
    }
    if (by1 < 0) return;
    const float size = std::sqrt(float(area));
    const std::vector<float> dist = distanceToSet(set, w, h);

    // The ground the shadow falls on: the ring around the object's lower half.
    std::vector<float> ring;
    const int midY = (by0 + by1) / 2;
    for (int y = midY; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float d = dist[size_t(y) * w + x];
            if (d > .25f * size && d < .8f * size) ring.push_back(luma[size_t(y) * w + x]);
        }
    }
    if (ring.size() < 64) return;
    std::nth_element(ring.begin(), ring.begin() + ring.size() / 2, ring.end());
    const float ground = ring[ring.size() / 2];
    if (ground <= 0.f) return;

    const float reach = .7f * size;
    const float attach = std::max(2.f, .04f * size);
    const int baseY = by1 - int(.35f * (by1 - by0));
    const auto dark = [&](int i) { return !set[i] && luma[i] < .62f * ground && dist[i] <= reach; };

    std::vector<unsigned char> shadow(set.size(), 0);
    std::vector<int> stack;
    for (int y = std::max(0, baseY); y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int i = y * w + x;
            if (dark(i) && dist[i] <= attach) {
                shadow[i] = 1;
                stack.push_back(i);
            }
        }
    }
    long long count = stack.size();
    while (!stack.empty()) {
        const int i = stack.back();
        stack.pop_back();
        const int x = i % w, y = i / w;
        const int next[4] = {x > 0 ? i - 1 : -1, x < w - 1 ? i + 1 : -1, y > 0 ? i - w : -1, y < h - 1 ? i + w : -1};
        for (int j : next) {
            if (j >= 0 && !shadow[j] && dark(j)) {
                shadow[j] = 1;
                stack.push_back(j);
                ++count;
            }
        }
    }
    if (count == 0 || count > area * 3 / 2) return;
    for (size_t i = 0; i < set.size(); ++i) {
        if (shadow[i]) set[i] = 1;
    }
}

// Called with the patch mutex held (it serializes image-source access); the
// lock is released while the selection model runs.
std::shared_ptr<const ObjectMask> getObjectMask(
    ImageSource* imgsrc, const SpotEntry& entry, const ColorTemp& currWB, int tr,
    const procparams::ProcParams* params, int fw, int fh,
    std::unique_lock<std::mutex>& patchLock, const SmartRepairJob& job)
{
    if (entry.objectPoints.empty() || !SamSelector::getInstance().available()) return nullptr;

    std::uint64_t key = 14695981039346656037ULL;
    for (const auto& pt : entry.objectPoints) {
        const int v[] = {pt.x, pt.y, pt.include ? 1 : 0};
        key = hashInts(key, v, 3);
    }
    const int flags[] = {entry.objectShadow ? 1 : 0, tr, fw, fh, entry.objectSize};
    key = hashInts(key, flags, 5);
    key = hashString(key, SamSelector::getInstance().modelTag());
    key = hashDouble(key, currWB.getTemp());
    key = hashDouble(key, currWB.getGreen());
    key = hashString(key, imgsrc->getFileName().raw());
    {
        std::lock_guard<std::mutex> cacheLock(objectMaskMutex);
        for (auto it = objectMaskCache.begin(); it != objectMaskCache.end(); ++it) {
            if ((*it)->key == key) {
                auto hit = *it;
                objectMaskCache.erase(it);
                objectMaskCache.push_back(hit);
                return hit->size > 0.f ? hit : nullptr;
            }
        }
    }

    const int skip = std::max(1, int(std::ceil(double(std::max(fw, fh)) / OBJECT_MASK_DIMENSION)));
    const PreviewProps pp(0, 0, fw, fh, skip);
    int w = 0, h = 0;
    imgsrc->getSize(pp, w, h);
    if (w < 16 || h < 16) return nullptr;

    // The selection model takes linear ProPhoto and encodes it itself.
    std::unique_ptr<Imagefloat> frame(new Imagefloat(w, h));
    imgsrc->getImage(currWB, tr, frame.get(), pp, params->toneCurve, params->raw);
    auto icm = params->icm;
    icm.workingProfile = "ProPhoto";
    imgsrc->convertColorSpace(frame.get(), icm, currWB);

    patchLock.unlock();
    struct Relock {
        std::unique_lock<std::mutex>& lock;
        ~Relock() { if (!lock.owns_lock()) lock.lock(); }
    } relock{patchLock};
    if (job) job->checkpoint();

    // Raw scene data can sit stops below a normal exposure; bring the median
    // to mid grey so the model sees an ordinary photograph.
    std::vector<float> luma(size_t(w) * h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            luma[size_t(y) * w + x] = .29f * frame->r(y, x) + .71f * frame->g(y, x);
        }
    }
    {
        std::vector<float> sorted;
        sorted.reserve(luma.size() / 16 + 1);
        for (size_t i = 0; i < luma.size(); i += 16) sorted.push_back(luma[i]);
        std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
        const float median = sorted[sorted.size() / 2];
        const float gain = median > 1.f ? std::max(.25f, std::min(16.f, .18f * 65535.f / median)) : 1.f;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                frame->r(y, x) *= gain;
                frame->g(y, x) *= gain;
                frame->b(y, x) *= gain;
            }
        }
    }

    std::vector<std::array<float, 3>> prompts;
    std::vector<std::array<int, 2>> clicks;
    for (const auto& pt : entry.objectPoints) {
        prompts.push_back({{(pt.x + .5f) / fw, (pt.y + .5f) / fh, pt.include ? 1.f : 0.f}});
        if (pt.include) {
            clicks.push_back({{std::max(0, std::min(w - 1, pt.x / skip)), std::max(0, std::min(h - 1, pt.y / skip))}});
        }
    }
    const long long traceStartUs = edittrace::enabled() ? edittrace::nowUs() : 0;
    char samId[64];
    std::snprintf(samId, sizeof samId, "|spot-object|%d|%dx%d", tr, w, h);
    // One click is ambiguous between a part, the object and the group it
    // belongs to (a click on a car door: the door, the car, or the car and
    // the kerb it stands on). The model offers three nested readings. Its
    // own favourite is often the part, which leaves the rest of the object
    // standing; the largest is often the object plus whatever touches it.
    // Removal wants the whole thing and no more: the largest reading the
    // model itself rates about as well as its best one, as long as that is
    // still an object rather than a large share of the frame. The user can
    // overrule this per pick (objectSize).
    auto result = std::make_shared<ObjectMask>();
    result->key = key;
    long long area = 0;
    int taken = -1;
    std::vector<unsigned char> set;
    SamSelector::Candidates offered;
    const auto reading = [&](int candidate, std::vector<unsigned char>& trial) -> long long {
        if (job) job->checkpoint();
        const array2D<float> selected = SamSelector::getInstance().select(
            *frame, imgsrc->getFileName().raw() + samId, prompts, candidate, nullptr, &offered);
        if (selected.getWidth() != w || selected.getHeight() != h) return -1;
        trial.assign(size_t(w) * h, 0);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) trial[size_t(y) * w + x] = selected[y][x] > .5f;
        }
        return keepClickedPieces(trial, w, h, clicks);
    };
    // An explicit "larger" may take more of the frame than a guess may.
    const float maxShare = entry.objectSize ? .6f : OBJECT_MAX_FRAME_SHARE;
    const long long maxArea = static_cast<long long>(maxShare * w * h);
    std::vector<int> order;
    if (entry.objectSize >= 1 && entry.objectSize <= 3) {
        // As asked; when that reading is unusable, the next smaller ones.
        for (int candidate = entry.objectSize - 1; candidate >= 0; --candidate) order.push_back(candidate);
    } else {
        order = {2, 1, 0};
    }
    bool judged = false;
    for (size_t i = 0; i < order.size() && !clicks.empty(); ++i) {
        const int candidate = order[i];
        std::vector<unsigned char> trial;
        const long long trialArea = reading(candidate, trial);
        if (trialArea < 0) break;
        if (!entry.objectSize && !judged) {
            // The scores are known after the first decode: drop the readings
            // the model trusts clearly less than its best. A much larger
            // reading needs to be nearly as good as the best to be taken --
            // that jump in size is where "the object" turns into "the object
            // and its surroundings".
            judged = true;
            const float best = std::max({offered.score[0], offered.score[1], offered.score[2]});
            std::vector<int> kept;
            for (int c = 2; c >= 0; --c) {
                float slack = OBJECT_SCORE_SLACK;
                if (c > 0 && offered.share[c] > OBJECT_JUMP * std::max(offered.share[c - 1], 1e-5f)) slack *= .35f;
                if (offered.score[c] >= best - slack) kept.push_back(c);
            }
            for (int c = 2; c >= 0; --c) {
                if (std::find(kept.begin(), kept.end(), c) == kept.end()) kept.push_back(c);
            }
            order = kept;
            if (order[0] != candidate) {
                i = size_t(-1);   // start over in the judged order
                continue;
            }
        }
        if (trialArea >= 9 && trialArea <= maxArea) {
            set = std::move(trial);
            area = trialArea;
            taken = candidate;
            break;
        }
    }
    if (taken >= 0) {
        result->candidate = taken;
        const float mine = std::max(offered.share[taken], 1e-5f);
        for (int c = 0; c < 3; ++c) {
            if (c < taken && offered.share[c] < .8f * mine) result->hasSmaller = true;
            if (c > taken && offered.share[c] > 1.25f * mine && offered.share[c] <= .6f) result->hasLarger = true;
        }
    }
    frame.reset();
    const bool usable = area >= 9;
    if (usable) {
        if (entry.objectShadow) {
            addAttachedShadow(set, luma, w, h, area);
            area = std::count(set.begin(), set.end(), 1);
        }
        result->skip = skip;
        result->w = w;
        result->h = h;
        result->dist = distanceToSet(set, w, h);
        int gx0 = w, gy0 = h, gx1 = -1, gy1 = -1;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                result->dist[size_t(y) * w + x] *= skip;
                if (set[size_t(y) * w + x]) {
                    gx0 = std::min(gx0, x); gx1 = std::max(gx1, x);
                    gy0 = std::min(gy0, y); gy1 = std::max(gy1, y);
                }
            }
        }
        result->x0 = gx0 * skip;
        result->y0 = gy0 * skip;
        result->x1 = std::min(fw - 1, (gx1 + 1) * skip - 1);
        result->y1 = std::min(fh - 1, (gy1 + 1) * skip - 1);
        result->size = std::sqrt(float(area)) * skip;
    }
    if (edittrace::enabled()) {
        edittrace::logf("[aiPatch] object pick points=%zu grid=%dx%d candidate=%d share=%.3f shadow=%d usable=%d %.0fms"
                        " offered share=%.4f/%.4f/%.4f score=%.3f/%.3f/%.3f asked=%d",
                        entry.objectPoints.size(), w, h, taken, double(area) / (double(w) * h),
                        entry.objectShadow ? 1 : 0, usable ? 1 : 0,
                        (edittrace::nowUs() - traceStartUs) / 1000.0,
                        offered.share[0], offered.share[1], offered.share[2],
                        offered.score[0], offered.score[1], offered.score[2], entry.objectSize);
    }
    if (const char* dump = std::getenv("STEEP_SPOT_OBJECT_DUMP")) {
        // Debug: the picked outline as a PGM (white = object, grey = within 5% growth).
        if (FILE* f = g_fopen(dump, "wb")) {
            std::fprintf(f, "P5\n%d %d\n255\n", w, h);
            for (size_t i = 0; i < size_t(w) * h; ++i) {
                const float d = usable ? result->dist[i] : 1e9f;
                std::fputc(d <= 0.f ? 255 : d <= .05f * result->size ? 110 : 0, f);
            }
            std::fclose(f);
        }
    }

    std::lock_guard<std::mutex> cacheLock(objectMaskMutex);
    objectMaskCache.push_back(result);
    while (objectMaskCache.size() > MAX_OBJECT_MASKS) objectMaskCache.pop_front();
    return usable ? result : nullptr;
}

// Compute (or fetch) the finished full-resolution repair for one AI stroke.
std::shared_ptr<const AIPatch> getOrComputeAIPatch(
    ImageSource* imgsrc, const SpotEntry& entry,
    const std::vector<const SpotEntry*>& priorStrokes,
    const std::vector<std::shared_ptr<const AIPatch>>& priorPatches,
    const ColorTemp& currWB, int tr, const procparams::ProcParams* params,
    bool computeIfMissing, const SmartRepairJob& job)
{
    auto& engine = rtengine::getAIInpaintingEngine();
    if (!engine.isAvailable() || entry.strokePoints.empty()) {
        return nullptr;
    }

    const std::uint64_t key = hashEntryChain(entry, priorStrokes, currWB, tr, params,
                                             imgsrc->getFileName().raw());

    std::unique_lock<std::mutex> lock(aiPatchMutex);

    for (;;) {
        if (job) job->checkpoint();
        bool waiting = false;
        for (auto it = aiPatchCache.begin(); it != aiPatchCache.end(); ++it) {
            if ((*it)->key == key && (*it)->raw == params->raw && (*it)->icm == params->icm) {
                // LRU refresh: keep live patches at the back so eviction hits
                // strokes that are no longer part of the current edit.
                auto hit = *it;
                aiPatchCache.erase(it);
                aiPatchCache.push_back(hit);
                return hit;
            }
        }
        if (aiPatchPending.count(key)) {
            if (!computeIfMissing) {
                // An interactive pass must not block behind a multi-second
                // inference someone else started; the cheap per-view repair
                // stands in until that one lands.
                return nullptr;
            }

            // Another view (preview vs 1:1 crop) is computing this exact
            // patch — wait for its result instead of duplicating a
            // multi-second inference.
            waiting = true;
            aiPatchCv.wait_for(lock, std::chrono::milliseconds(50));
        }
        if (!waiting) {
            break;
        }
    }

    if (!computeIfMissing) {
        // Nothing cached, and this pass may not build it: let the caller fall
        // back to the per-view repair so the photo appears straight away.
        return nullptr;
    }

    aiPatchPending.insert(key);
    struct PendingGuard {
        std::unique_lock<std::mutex>& lock;
        std::uint64_t key;
        ~PendingGuard() {
            if (!lock.owns_lock()) lock.lock();
            aiPatchPending.erase(key);
            aiPatchCv.notify_all();
        }
    } pendingGuard{lock, key};
    if (job) job->setStage(SmartRepairStage::Preparing);
    // The model's working size decides how the region is laid out, and it
    // is only known once the (deferred) load has finished.
    lock.unlock();
    engine.ensureLoaded(job);
    lock.lock();
    const auto abandonPending = [&key]() {
        aiPatchPending.erase(key);
        aiPatchCv.notify_all();
    };

    int fw = 0, fh = 0;
    imgsrc->getFullSize(fw, fh, tr);
    if (fw <= 0 || fh <= 0) {
        abandonPending();
        return nullptr;
    }

    const float radius = float(entry.radius);
    const float featherRadius = std::min(entry.getFeatherRadius(), radius * 3.f + 32.f);

    // A picked object replaces the brush discs with its own outline. When
    // the pick is unusable (no model, nothing found, or it took a large part
    // of the frame) the entry is repaired as the plain dab it also is.
    std::shared_ptr<const ObjectMask> object;
    float objectGrow = 0.f, objectRamp = 0.f;
    if (entry.repairVersion >= 2 && !entry.objectPoints.empty()) {
        object = getObjectMask(imgsrc, entry, currWB, tr, params, fw, fh, lock, job);
    }
    if (!object) {
        SmartRepairReports::clearObject();
    }
    // Until the wide fill says otherwise.
    SmartRepairReports::setFill(SmartRepairReport::Fill::Standard);

    int bx0 = INT_MAX, by0 = INT_MAX, bx1 = INT_MIN, by1 = INT_MIN;
    int pad = int(featherRadius * 2);
    if (object) {
        // The whole outline is replaced: full strength out to the grown
        // edge, and the blend ramp lies entirely outside it, so no part of
        // the object is ever mixed back in.
        objectGrow = std::max({6.f, 1.5f * object->skip, entry.objectGrow / 100.f * object->size});
        objectRamp = std::max(2.f, entry.feather * .06f * object->size);
        const int reach = int(objectGrow + objectRamp) + 2;
        {
            // For the canvas: the outline and the margin replaced with it.
            SmartRepairReport picked;
            const int skip = object->skip;
            const int gx0 = std::max(0, (object->x0 - reach) / skip), gy0 = std::max(0, (object->y0 - reach) / skip);
            const int gx1 = std::min(object->w - 1, (object->x1 + reach) / skip), gy1 = std::min(object->h - 1, (object->y1 + reach) / skip);
            picked.clickX = entry.objectPoints.front().x;
            picked.clickY = entry.objectPoints.front().y;
            picked.gridWidth = gx1 - gx0 + 1;
            picked.gridHeight = gy1 - gy0 + 1;
            picked.x = gx0 * skip;
            picked.y = gy0 * skip;
            picked.width = picked.gridWidth * skip;
            picked.height = picked.gridHeight * skip;
            auto grid = std::make_shared<std::vector<unsigned char>>(size_t(picked.gridWidth) * picked.gridHeight);
            for (int y = gy0; y <= gy1; ++y) {
                for (int x = gx0; x <= gx1; ++x) {
                    const float d = object->dist[size_t(y) * object->w + x];
                    (*grid)[size_t(y - gy0) * picked.gridWidth + (x - gx0)] = d <= 0.f ? 255 : d <= objectGrow ? 128 : 0;
                }
            }
            picked.grid = grid;
            picked.candidate = object->candidate;
            picked.hasSmaller = object->hasSmaller;
            picked.hasLarger = object->hasLarger;
            SmartRepairReports::setObject(picked);
        }
        bx0 = object->x0 - reach;
        by0 = object->y0 - reach;
        bx1 = object->x1 + reach;
        by1 = object->y1 + reach;
        // Context for the fill model: about a third of the hole's extent on
        // each side keeps the hole near a third of what the model sees.
        // Shrunk when the region would exceed the repair memory limit.
        const long long holeW = bx1 - bx0 + 1, holeH = by1 - by0 + 1;
        pad = std::max(96, int(std::max(holeW, holeH) / 3));
        while (pad > 32 && (holeW + 2 * pad) * (holeH + 2 * pad) > MAX_AI_PATCH_PIXELS) pad = pad * 3 / 4;
    } else {
        for (const auto& pt : entry.strokePoints) {
            bx0 = std::min(bx0, pt.x - int(featherRadius) - 1);
            by0 = std::min(by0, pt.y - int(featherRadius) - 1);
            bx1 = std::max(bx1, pt.x + int(featherRadius) + 1);
            by1 = std::max(by1, pt.y + int(featherRadius) + 1);
        }
    }
    if (!object && entry.method == procparams::SpotMethod::AI_FILL) {
        // Tiles bring their own context; a quarter-size ring suffices for
        // the structure pass without ballooning the patch.
        pad += std::max(bx1 - bx0, by1 - by0) / 4;
    }
    bx0 = std::max(0, bx0 - pad);
    by0 = std::max(0, by0 - pad);
    bx1 = std::min(fw - 1, bx1 + pad);
    by1 = std::min(fh - 1, by1 + pad);
    if (bx0 > bx1 || by0 > by1) {
        abandonPending();
        return nullptr;
    }

    if (entry.repairVersion >= 3) {
        // The model always sees at least a 512 px field. A smaller region
        // used to be padded out with repeated edge pixels; real surroundings
        // cost nothing more and tell the model what the place looks like.
        const auto widen = [](int& lo, int& hi, int limit) {
            const int missing = std::min(512, limit) - (hi - lo + 1);
            if (missing <= 0) return;
            lo -= missing / 2;
            hi += missing - missing / 2;
            if (lo < 0) { hi -= lo; lo = 0; }
            if (hi > limit - 1) { lo = std::max(0, lo - (hi - (limit - 1))); hi = limit - 1; }
        };
        widen(bx0, bx1, fw);
        widen(by0, by1, fh);
    }

    const int w = bx1 - bx0 + 1;
    const int h = by1 - by0 + 1;
    const long long pixels = static_cast<long long>(w) * h;
    // One native pass when the region fits the model, tiles otherwise.
    const int modelSide = engine.tileSize(entry.repairVersion);
    bool tiled = modelSide > 512 ? std::max(w, h) > modelSide : pixels > 700000;
    if (pixels > MAX_AI_PATCH_PIXELS) {
        abandonPending();
        return nullptr; // legacy per-view path takes over
    }

    // Fresh full-resolution pull of the region.
    std::unique_ptr<Imagefloat> crop(new Imagefloat(w, h));
    PreviewProps spp(bx0, by0, w, h, 1);
    imgsrc->getImage(currWB, tr, crop.get(), spp, params->toneCurve, params->raw);

    std::vector<float> inR(pixels), inG(pixels), inB(pixels);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const long long i = static_cast<long long>(y) * w + x;
            inR[i] = crop->r(y, x);
            inG[i] = crop->g(y, x);
            inB[i] = crop->b(y, x);
        }
    }
    crop.reset();

    // Earlier AI repairs are part of this stroke's reality.
    for (const auto& prior : priorPatches) {
        const int ox0 = std::max(bx0, prior->x0);
        const int oy0 = std::max(by0, prior->y0);
        const int ox1 = std::min(bx1, prior->x0 + prior->w - 1);
        const int oy1 = std::min(by1, prior->y0 + prior->h - 1);
        for (int y = oy0; y <= oy1; ++y) {
            for (int x = ox0; x <= ox1; ++x) {
                const long long di = static_cast<long long>(y - by0) * w + (x - bx0);
                const long long si = static_cast<long long>(y - prior->y0) * prior->w + (x - prior->x0);
                if (prior->delta) {
                    inR[di] += prior->fillR[si];
                    inG[di] += prior->fillG[si];
                    inB[di] += prior->fillB[si];
                    continue;
                }
                const float m = prior->mask[si];
                if (m > 0.f) {
                    inR[di] = inR[di] * (1.f - m) + prior->fillR[si] * m;
                    inG[di] = inG[di] * (1.f - m) + prior->fillG[si] * m;
                    inB[di] = inB[di] * (1.f - m) + prior->fillB[si] * m;
                }
            }
        }
    }

    // Soft stroke mask in full resolution.
    std::vector<float> mask(pixels, 0.f);
    if (object) {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const float d = object->distanceAt(bx0 + x, by0 + y);
                if (d < objectGrow + objectRamp) {
                    mask[static_cast<long long>(y) * w + x] =
                        d <= objectGrow ? 1.f : repairmath::smoothstep(1.f - (d - objectGrow) / objectRamp);
                }
            }
        }
    }
    for (const auto& pt : object ? std::vector<Coord>() : entry.strokePoints) {
        const float cx = float(pt.x - bx0);
        const float cy = float(pt.y - by0);
        const int pyMin = std::max(0, int(cy - featherRadius));
        const int pyMax = std::min(h - 1, int(cy + featherRadius));
        const int pxMin = std::max(0, int(cx - featherRadius));
        const int pxMax = std::min(w - 1, int(cx + featherRadius));
        for (int py = pyMin; py <= pyMax; ++py) {
            for (int px = pxMin; px <= pxMax; ++px) {
                const float dx = float(px) - cx;
                const float dy = float(py) - cy;
                const float dist = sqrtf(dx * dx + dy * dy);
                float val = 0.f;
                if (dist <= radius) {
                    val = 1.f;
                } else if (dist < featherRadius) {
                    val = (featherRadius - dist) / (featherRadius - radius);
                }
                if (entry.repairVersion >= 2) val = repairmath::smoothstep(val);
                mask[static_cast<long long>(py) * w + px] =
                    std::max(mask[static_cast<long long>(py) * w + px], val);
            }
        }
    }

    // Keep earlier stroke repairs intact where they overlap this stroke.
    if (entry.repairVersion < 2) excludePriorStrokeCoverage(mask, w, h, bx0, by0, 0, 0, 1, priorStrokes);

    // Probe the color transform while imgsrc access is still serialized.
    float toSRGB[3][3], fromSRGB[3][3];
    bool colorMapped = probeCameraToSRGB(imgsrc, currWB, params, toSRGB, fromSRGB);
    if (!colorMapped && entry.repairVersion >= 2 && imgsrc->isRAW()) {
        // Generate in calibrated camera color; the user's nonlinear input
        // look is applied afterwards, once, by the normal photo pipeline.
        auto matrixParams = *params;
        matrixParams.icm.inputProfile = "(camera)";
        colorMapped = probeCameraToSRGB(imgsrc, currWB, &matrixParams, toSRGB, fromSRGB);
    }
    struct ProfilePair {
        cmsHTRANSFORM forward = nullptr, inverse = nullptr;
        ~ProfilePair() {
            if (forward) cmsDeleteTransform(forward);
            if (inverse) cmsDeleteTransform(inverse);
        }
    } profile;
    if (!colorMapped && entry.repairVersion >= 2) {
        if (!dynamic_cast<StdImageSource*>(imgsrc)) return nullptr;
        cmsHPROFILE input = nullptr;
        const auto& name = params->icm.inputProfile;
        if (!name.empty() && name != "(embedded)" && name != "(camera)" && name != "(cameraICC)") {
            input = ICCStore::getInstance()->getProfile(name);
        }
        if (!input) input = imgsrc->getEmbeddedProfile();
        if (!input || cmsGetColorSpace(input) != cmsSigRgbData) input = ICCStore::getInstance()->getsRGBProfile();
        const auto linearSRGB = ICCStore::getInstance()->workingSpace("sRGB");
        {
            MyMutex::MyLock cmsLock(*lcmsMutex);
            profile.forward = cmsCreateTransform(input, TYPE_RGB_FLT, linearSRGB, TYPE_RGB_FLT,
                INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOOPTIMIZE | cmsFLAGS_NOCACHE);
            profile.inverse = cmsCreateTransform(linearSRGB, TYPE_RGB_FLT, input, TYPE_RGB_FLT,
                INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOOPTIMIZE | cmsFLAGS_NOCACHE);
        }
        if (!profile.forward || !profile.inverse) return nullptr;
    }

    // The image pull is done. The multi-second inference must not hold the
    // mutex: cache hits for other strokes (and the other pipeline thread's
    // different-key computes) proceed while this one runs.
    lock.unlock();

    // Run the model in linear sRGB, not camera primaries (greenish fills).
    std::vector<float> mappedR, mappedG, mappedB;
    const auto transformPlanes = [w, h](cmsHTRANSFORM transform, std::vector<float>& r, std::vector<float>& g, std::vector<float>& b) {
        Imagefloat image(w, h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = size_t(y) * w + x;
                image.r(y, x) = r[i] / 65535.f; image.g(y, x) = g[i] / 65535.f; image.b(y, x) = b[i] / 65535.f;
            }
        image.ExecCMSTransform(transform);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = size_t(y) * w + x;
                r[i] = image.r(y, x) * 65535.f; g[i] = image.g(y, x) * 65535.f; b[i] = image.b(y, x) * 65535.f;
            }
    };
    if (colorMapped) {
        mappedR.resize(pixels);
        mappedG.resize(pixels);
        mappedB.resize(pixels);
        for (long long i = 0; i < pixels; ++i) {
            const float r = inR[i], g = inG[i], b = inB[i];
            mappedR[i] = toSRGB[0][0] * r + toSRGB[0][1] * g + toSRGB[0][2] * b;
            mappedG[i] = toSRGB[1][0] * r + toSRGB[1][1] * g + toSRGB[1][2] * b;
            mappedB[i] = toSRGB[2][0] * r + toSRGB[2][1] * g + toSRGB[2][2] * b;
        }
    }
    if (profile.forward) {
        mappedR = inR; mappedG = inG; mappedB = inB;
        transformPlanes(profile.forward, mappedR, mappedG, mappedB);
    }
    const std::vector<float>& srcR = colorMapped || profile.forward ? mappedR : inR;
    const std::vector<float>& srcG = colorMapped || profile.forward ? mappedG : inG;
    const std::vector<float>& srcB = colorMapped || profile.forward ? mappedB : inB;

    const long long traceStartUs = edittrace::enabled() ? edittrace::nowUs() : 0;

    std::vector<float> fillR(pixels), fillG(pixels), fillB(pixels);
    bool ok;
    // A hole too wide for the model at this size gets its structure from a
    // smaller view first, even when the region itself would fit in one pass.
    const float thickness = entry.repairVersion >= 3 ? holeThickness(mask, w, h) : 0.f;
    if (modelSide > 512 && thickness > MODEL_HOLE_THICKNESS * 1.15f) tiled = true;
    if (!tiled) {
        ok = engine.inpaint(srcR.data(), srcG.data(), srcB.data(), mask.data(),
                            w, h, fillR.data(), fillG.data(), fillB.data(), entry.repairVersion, job);
    } else {
        ok = inpaintTiled(engine, srcR, srcG, srcB, mask, w, h, fillR, fillG, fillB, entry.repairVersion, job, thickness,
                          entry.highQuality, static_cast<std::uint32_t>(key ^ (key >> 32)));
    }

    if (edittrace::enabled()) {
        edittrace::logf("[aiPatch] method=%d v=%d bb=%dx%d model=%d hole=%.0f tiled=%d ok=%d srgb=%d total=%.0fms",
                        static_cast<int>(entry.method), entry.repairVersion, w, h, modelSide, thickness,
                        tiled ? 1 : 0, ok ? 1 : 0, colorMapped ? 1 : 0,
                        (edittrace::nowUs() - traceStartUs) / 1000.0);
    }

    if (!ok) {
        lock.lock();
        abandonPending();
        return nullptr;
    }

    // Bring the fill back to camera space before caching/blitting.
    if (colorMapped) {
        for (long long i = 0; i < pixels; ++i) {
            const float r = fillR[i], g = fillG[i], b = fillB[i];
            fillR[i] = fromSRGB[0][0] * r + fromSRGB[0][1] * g + fromSRGB[0][2] * b;
            fillG[i] = fromSRGB[1][0] * r + fromSRGB[1][1] * g + fromSRGB[1][2] * b;
            fillB[i] = fromSRGB[2][0] * r + fromSRGB[2][1] * g + fromSRGB[2][2] * b;
        }
    }

    if (profile.inverse) transformPlanes(profile.inverse, fillR, fillG, fillB);

    // Grain matching at full resolution — it then downsamples into every
    // view exactly the way the surrounding real grain does.
    std::vector<float> residuals;
    for (int y = 1; y < h - 1; y += 2) {
        for (int x = 1; x < w - 1; x += 2) {
            const long long i = static_cast<long long>(y) * w + x;
            if (mask[i] <= 0.01f) {
                const float neighbors = 0.25f * (inG[i - 1] + inG[i + 1] + inG[i - w] + inG[i + w]);
                residuals.push_back(std::fabs(inG[i] - neighbors));
            }
        }
    }
    if (residuals.size() > 64) {
        const size_t n = residuals.size() * 2 / 5;
        std::nth_element(residuals.begin(), residuals.begin() + n, residuals.end());
        const float grainSigma = std::min(residuals[n] * (entry.repairVersion >= 2 ? .65f : 1.2f), entry.repairVersion >= 2 ? 400.f : 1200.f);
        if (grainSigma > 0.f) {
            std::mt19937 rng(static_cast<std::uint32_t>(key));
            std::normal_distribution<float> grain(0.f, grainSigma);
            for (long long i = 0; i < pixels; ++i) {
                if (mask[i] > 0.f) {
                    const float g = grain(rng) * 0.8f;
                    fillR[i] = LIM(fillR[i] + g, 0.f, 65535.f);
                    fillG[i] = LIM(fillG[i] + g, 0.f, 65535.f);
                    fillB[i] = LIM(fillB[i] + g, 0.f, 65535.f);
                }
            }
        }
    }

    auto patch = std::make_shared<AIPatch>();
    patch->raw = params->raw;
    patch->icm = params->icm;
    patch->key = key;
    patch->x0 = bx0;
    patch->y0 = by0;
    patch->w = w;
    patch->h = h;
    patch->fullW = fw;
    patch->fullH = fh;
    if (entry.repairVersion >= 2) {
        if (job) job->setStage(SmartRepairStage::Blending);
        Imagefloat original(w, h), repaired(w, h);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t i = size_t(y) * w + x;
                const float m = mask[i] * std::max(0.f, std::min(1.f, entry.opacity));
                original.r(y, x) = inR[i]; original.g(y, x) = inG[i]; original.b(y, x) = inB[i];
                fillR[i] = m * (fillR[i] - inR[i]);
                fillG[i] = m * (fillG[i] - inG[i]);
                fillB[i] = m * (fillB[i] - inB[i]);
                repaired.r(y, x) = inR[i] + fillR[i];
                repaired.g(y, x) = inG[i] + fillG[i];
                repaired.b(y, x) = inB[i] + fillB[i];
            }
        }
        // Transform the original and composite, not a color-space delta:
        // input profiles may contain nonlinear tables.
        imgsrc->convertColorSpace(&original, params->icm, currWB);
        imgsrc->convertColorSpace(&repaired, params->icm, currWB);
        patch->workR.resize(pixels); patch->workG.resize(pixels); patch->workB.resize(pixels);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t i = size_t(y) * w + x;
                patch->workR[i] = repaired.r(y, x) - original.r(y, x);
                patch->workG[i] = repaired.g(y, x) - original.g(y, x);
                patch->workB[i] = repaired.b(y, x) - original.b(y, x);
            }
        }
        patch->delta = true;
        mask.clear();
        mask.shrink_to_fit();
    }
    patch->fillR = std::move(fillR);
    patch->fillG = std::move(fillG);
    patch->fillB = std::move(fillB);
    patch->mask = std::move(mask);

    lock.lock();
    abandonPending(); // waiters wake and find the patch in the cache
    aiPatchCache.push_back(patch);

    const auto storedPixels = [](const std::shared_ptr<const AIPatch>& p) {
        return static_cast<long long>(p->w) * p->h * (p->delta ? 6 : 4) / 4;
    };
    long long totalPixels = 0;
    for (const auto& cached : aiPatchCache) {
        totalPixels += storedPixels(cached);
    }
    while (aiPatchCache.size() > 1
            && (aiPatchCache.size() > MAX_AI_PATCHES || totalPixels > MAX_AI_PATCH_TOTAL_PIXELS)) {
        totalPixels -= storedPixels(aiPatchCache.front());
        aiPatchCache.pop_front();
    }

    return patch;
}

// Blend a finished patch into a view buffer at that view's scale and crop.
void blitAIPatch(Imagefloat* img, const PreviewProps& pp, const AIPatch& patch, bool workingSpace)
{
    const int skip = pp.getSkip();
    const int cropX = pp.getX();
    const int cropY = pp.getY();
    const int vw = img->getWidth();
    const int vh = img->getHeight();

    const int vx0 = std::max(0, (patch.x0 - cropX) / skip);
    const int vy0 = std::max(0, (patch.y0 - cropY) / skip);
    const int vx1 = std::min(vw - 1, (patch.x0 + patch.w - 1 - cropX) / skip);
    const int vy1 = std::min(vh - 1, (patch.y0 + patch.h - 1 - cropY) / skip);

    for (int vy = vy0; vy <= vy1; ++vy) {
        for (int vx = vx0; vx <= vx1; ++vx) {
            if (patch.delta) {
                const int ix = cropX + vx * skip, iy = cropY + vy * skip;
                const auto sample = [&](const std::vector<float>& data) {
                    return repairmath::averageDelta(data, patch.w, patch.h, ix - patch.x0, iy - patch.y0,
                                                   skip, patch.fullW, patch.fullH, ix, iy);
                };
                img->r(vy, vx) += sample(workingSpace ? patch.workR : patch.fillR);
                img->g(vy, vx) += sample(workingSpace ? patch.workG : patch.fillG);
                img->b(vy, vx) += sample(workingSpace ? patch.workB : patch.fillB);
                continue;
            }
            // The full-resolution block this view pixel covers, patch-local.
            const int px0 = std::max(0, cropX + vx * skip - patch.x0);
            const int py0 = std::max(0, cropY + vy * skip - patch.y0);
            const int px1 = std::min(patch.w, px0 + skip);
            const int py1 = std::min(patch.h, py0 + skip);
            if (px0 >= px1 || py0 >= py1) {
                continue;
            }

            double sumR = 0.0, sumG = 0.0, sumB = 0.0, sumM = 0.0;
            for (int py = py0; py < py1; ++py) {
                const long long row = static_cast<long long>(py) * patch.w;
                for (int px = px0; px < px1; ++px) {
                    sumR += patch.fillR[row + px];
                    sumG += patch.fillG[row + px];
                    sumB += patch.fillB[row + px];
                    sumM += patch.mask[row + px];
                }
            }
            const int n = (px1 - px0) * (py1 - py0);
            const float m = static_cast<float>(sumM / n);
            if (m <= 0.001f) {
                continue;
            }
            img->r(vy, vx) = img->r(vy, vx) * (1.f - m) + static_cast<float>(sumR / n) * m;
            img->g(vy, vx) = img->g(vy, vx) * (1.f - m) + static_cast<float>(sumG / n) * m;
            img->b(vy, vx) = img->b(vy, vx) * (1.f - m) + static_cast<float>(sumB / n) * m;
        }
    }
}

// Process a stroke-based AI inpainting directly on the preview image
void processStrokeAI(Imagefloat* img, const SpotEntry& entry, const PreviewProps &pp,
                     const std::vector<const SpotEntry*>& priorStrokes)
{
    if (entry.strokePoints.empty()) return;

    auto& engine = rtengine::getAIInpaintingEngine();
    if (!engine.isInitialized()) {
        // Fall back to regular erase if AI engine not available
        processStrokeErase(img, entry, pp, priorStrokes);
        return;
    }

    int skip = pp.getSkip();
    int cropX = pp.getX();
    int cropY = pp.getY();
    int imgW = img->getWidth();
    int imgH = img->getHeight();
    float radius = float(entry.radius) / float(skip);
    // Guard against absurd feather values from hand-edited sidecars.
    float featherRadius = std::min(entry.getFeatherRadius() / float(skip),
                                   radius * 3.f + 32.f);

    // Compute bounding box with padding
    int bbMinX = INT_MAX, bbMinY = INT_MAX, bbMaxX = INT_MIN, bbMaxY = INT_MIN;
    for (const auto& pt : entry.strokePoints) {
        int sx = (pt.x - cropX) / skip;
        int sy = (pt.y - cropY) / skip;
        bbMinX = std::min(bbMinX, sx - int(featherRadius) - 1);
        bbMinY = std::min(bbMinY, sy - int(featherRadius) - 1);
        bbMaxX = std::max(bbMaxX, sx + int(featherRadius) + 1);
        bbMaxY = std::max(bbMaxY, sy + int(featherRadius) + 1);
    }

    // Add padding for AI context. Generative Fill covers big areas, so it
    // brings along proportionally more surroundings for the model to read.
    int pad = int(featherRadius * 2);
    if (entry.method == procparams::SpotMethod::AI_FILL) {
        pad += std::max(bbMaxX - bbMinX, bbMaxY - bbMinY) / 2;
    }
    bbMinX = std::max(0, bbMinX - pad);
    bbMinY = std::max(0, bbMinY - pad);
    bbMaxX = std::min(imgW - 1, bbMaxX + pad);
    bbMaxY = std::min(imgH - 1, bbMaxY + pad);

    if (bbMinX > bbMaxX || bbMinY > bbMaxY) return;

    int bbW = bbMaxX - bbMinX + 1;
    int bbH = bbMaxY - bbMinY + 1;
    int bbSize = bbW * bbH;

    // Extract crop region
    std::vector<float> cropR(bbSize), cropG(bbSize), cropB(bbSize);
    std::vector<float> mask(bbSize, 0.f);

    for (int y = 0; y < bbH; ++y) {
        for (int x = 0; x < bbW; ++x) {
            int ix = bbMinX + x;
            int iy = bbMinY + y;
            int idx = y * bbW + x;
            cropR[idx] = img->r(iy, ix);
            cropG[idx] = img->g(iy, ix);
            cropB[idx] = img->b(iy, ix);
        }
    }

    // Build a soft mask from the stroke: solid inside the brush, feather
    // falloff beyond it. The model fills everything the stroke touches; the
    // write-back blends through these values so there is no hard seam.
    for (const auto& pt : entry.strokePoints) {
        float cx = float(pt.x - cropX) / float(skip) - bbMinX;
        float cy = float(pt.y - cropY) / float(skip) - bbMinY;

        int pyMin = std::max(0, int(cy - featherRadius));
        int pyMax = std::min(bbH - 1, int(cy + featherRadius));
        int pxMin = std::max(0, int(cx - featherRadius));
        int pxMax = std::min(bbW - 1, int(cx + featherRadius));

        for (int py = pyMin; py <= pyMax; ++py) {
            for (int px = pxMin; px <= pxMax; ++px) {
                float dx = float(px) - cx;
                float dy = float(py) - cy;
                float dist = sqrtf(dx * dx + dy * dy);
                float val = 0.f;
                if (dist <= radius) {
                    val = 1.f;
                } else if (dist < featherRadius) {
                    val = (featherRadius - dist) / (featherRadius - radius);
                }
                mask[py * bbW + px] = std::max(mask[py * bbW + px], val);
            }
        }
    }

    // Keep the repairs from earlier strokes intact in any overlap
    excludePriorStrokeCoverage(mask, bbW, bbH, bbMinX, bbMinY, cropX, cropY, skip, priorStrokes);

    // Content-addressed lookup: identical input crop + mask means an
    // identical fill, whatever caused the re-render.
    std::uint64_t key = 14695981039346656037ULL;
    key = hashFloats(key, cropR.data(), bbSize, 3);
    key = hashFloats(key, cropG.data(), bbSize, 3);
    key = hashFloats(key, cropB.data(), bbSize, 3);
    key = hashFloats(key, mask.data(), bbSize, 3);
    key ^= (std::uint64_t(bbW) << 32) ^ std::uint64_t(bbH);

    std::vector<float> outR(bbSize), outG(bbSize), outB(bbSize);
    bool haveResult = false;
    bool cacheHit = false;

    if (bbSize <= MAX_INPAINT_CACHE_PIXELS) {
        std::lock_guard<std::mutex> lock(inpaintCacheMutex);
        for (const auto& cached : inpaintCache) {
            if (cached.key == key && cached.bbW == bbW && cached.bbH == bbH) {
                outR = cached.outR;
                outG = cached.outG;
                outB = cached.outB;
                haveResult = true;
                cacheHit = true;
                break;
            }
        }
    }

    if (!haveResult) {
        const long long traceStartUs = edittrace::enabled() ? edittrace::nowUs() : 0;
        haveResult = engine.inpaint(cropR.data(), cropG.data(), cropB.data(),
                                    mask.data(), bbW, bbH,
                                    outR.data(), outG.data(), outB.data());
        if (edittrace::enabled()) {
            edittrace::logf("[aiFill] method=%d bb=%dx%d skip=%d ok=%d infer=%.0fms",
                            static_cast<int>(entry.method), bbW, bbH, skip,
                            haveResult ? 1 : 0,
                            (edittrace::nowUs() - traceStartUs) / 1000.0);
        }

        if (haveResult && bbSize <= MAX_INPAINT_CACHE_PIXELS) {
            std::lock_guard<std::mutex> lock(inpaintCacheMutex);
            inpaintCache.push_back({key, bbW, bbH, outR, outG, outB});
            while (inpaintCache.size() > MAX_INPAINT_CACHE_ENTRIES) {
                inpaintCache.pop_front();
            }
        }
    }

    if (haveResult) {
        // Grain matching: the model's fill is noise-free and reads as a
        // patch next to real sensor grain. Estimate the grain level from the
        // untouched surroundings (robust percentile of a high-pass) and add
        // a matched amount inside the fill. Seeded from the content key so
        // every re-render lays down identical grain.
        std::vector<float> residuals;
        for (int y = 1; y < bbH - 1; y += 2) {
            for (int x = 1; x < bbW - 1; x += 2) {
                const int idx = y * bbW + x;
                if (mask[idx] <= 0.01f) {
                    const float neighbors = 0.25f * (cropG[idx - 1] + cropG[idx + 1]
                                                     + cropG[idx - bbW] + cropG[idx + bbW]);
                    residuals.push_back(std::fabs(cropG[idx] - neighbors));
                }
            }
        }
        float grainSigma = 0.f;
        if (residuals.size() > 64) {
            const size_t n = residuals.size() * 2 / 5;
            std::nth_element(residuals.begin(), residuals.begin() + n, residuals.end());
            grainSigma = std::min(residuals[n] * 1.2f, 1200.f);
        }

        std::mt19937 rng(static_cast<std::uint32_t>(key));
        std::normal_distribution<float> grain(0.f, std::max(grainSigma, 1.f));

        // Feathered write-back: blend the reconstruction through the soft
        // mask instead of replacing at a hard 0.5 cliff.
        for (int y = 0; y < bbH; ++y) {
            for (int x = 0; x < bbW; ++x) {
                const int idx = y * bbW + x;
                const float m = mask[idx];
                if (m <= 0.f) {
                    continue;
                }
                const int ix = bbMinX + x;
                const int iy = bbMinY + y;
                const float g = grainSigma > 0.f ? grain(rng) * 0.8f : 0.f;
                img->r(iy, ix) = img->r(iy, ix) * (1.f - m)
                    + LIM(outR[idx] + g, 0.f, 65535.f) * m;
                img->g(iy, ix) = img->g(iy, ix) * (1.f - m)
                    + LIM(outG[idx] + g, 0.f, 65535.f) * m;
                img->b(iy, ix) = img->b(iy, ix) * (1.f - m)
                    + LIM(outB[idx] + g, 0.f, 65535.f) * m;
            }
        }
    }
}
#endif // RT_AI_MASKING

} // anonymous namespace

void ImProcFunctions::removeSpots (Imagefloat* img, ImageSource* imgsrc, const std::vector<SpotEntry> &entries, const PreviewProps &pp, const ColorTemp &currWB, const ColorManagementParams *cmp, int tr, bool allowFullResPatch, const SmartRepairJob& repairJob)
{
    bool allRepairsReady = true;
    if (repairJob && repairJob->suspended) allowFullResPatch = false;
    // Process stroke-based entries directly on the image first. Each entry
    // knows which strokes came before it, so repainting the border of an
    // already-repaired spot leaves the earlier repair untouched.
    std::vector<const SpotEntry*> priorStrokes;
#ifdef RT_AI_MASKING
    std::vector<std::shared_ptr<const AIPatch>> aiPriorPatches;
#endif
    for (const auto& entry : params->spot.entries) {
        if (repairJob) repairJob->checkpoint();
        if (entry.isStroke()) {
            if (entry.method == SpotMethod::AI_REFLECT) {
                // Model-free glare reduction; available in every build.
                processStrokeReflect(img, entry, pp, priorStrokes);
            } else
#ifdef RT_AI_MASKING
            if (static_cast<int>(entry.method) >= static_cast<int>(SpotMethod::AI_REMOVE)) {
                // Preferred path: one full-resolution repair in image space,
                // cached, blended into this view at its own scale. Falls back
                // to the per-view path for outsized regions.
                auto patch = getOrComputeAIPatch(imgsrc, entry, priorStrokes,
                                                 aiPriorPatches, currWB, tr, params,
                                                 allowFullResPatch, repairJob);
                if (patch) {
                    blitAIPatch(img, pp, *patch, cmp != nullptr);
                    aiPriorPatches.push_back(patch);
                } else {
                    allRepairsReady = false;
                    if (entry.repairVersion < 2) processStrokeAI(img, entry, pp, priorStrokes);
                    else if (allowFullResPatch) {
                        if (repairJob) repairJob->stage = SmartRepairStage::Failed;
                        else throw std::runtime_error("Smart repair failed: model unavailable, invalid output, or region exceeds the repair memory limit");
                    }
                }
            } else
#endif
            {
#ifndef RT_AI_MASKING
                if (entry.repairVersion >= 2 && static_cast<int>(entry.method) >= static_cast<int>(SpotMethod::AI_REMOVE)) {
                    allRepairsReady = false;
                    if (repairJob) repairJob->stage = SmartRepairStage::Failed;
                    else throw std::runtime_error("This build cannot render the saved AI repair");
                    continue;
                }
#endif
                processStrokeErase(img, entry, pp, priorStrokes);
            }
            priorStrokes.push_back(&entry);
        }
    }

    //Get the clipped image areas (src & dst) from the source image

    std::vector< std::shared_ptr<SpotBox> > srcSpotBoxs;
    std::vector< std::shared_ptr<SpotBox> > dstSpotBoxs;
    std::vector<SpotEntry> boxEntries;   // parallel to the box vectors
    int fullImgWidth = 0;
    int fullImgHeight = 0;
    imgsrc->getFullSize(fullImgWidth, fullImgHeight, tr);
    SpotBox fullImageBox(0, 0, fullImgWidth - 1, fullImgHeight - 1, 0, 0, nullptr, SpotBox::Type::FINAL);
    SpotBox cropBox(pp.getX(), pp.getY(),
                    pp.getX() + pp.getWidth() - 1, pp.getY() + pp.getHeight() - 1,
                    0, 0, img, SpotBox::Type::FINAL);

    std::set<int> visibleSpots;   // list of dest spots intersecting the preview's crop
    int i = 0;

    for (auto entry : params->spot.entries) {
        // Skip stroke-based entries (already processed above)
        if (entry.isStroke()) {
            continue;  // Don't increment i — it indexes into srcSpotBoxs/dstSpotBoxs
        }
        std::shared_ptr<SpotBox> srcSpotBox(new SpotBox(entry,  SpotBox::Type::SOURCE));
        std::shared_ptr<SpotBox> dstSpotBox(new SpotBox(entry,  SpotBox::Type::TARGET));
        if (   !srcSpotBox->setIntersectionWith(fullImageBox)
            || !dstSpotBox->setIntersectionWith(fullImageBox)
            || !srcSpotBox->imageIntersects(*dstSpotBox, true))
        {
            continue;
        }

        // If spot intersect the preview image, add it to the visible spots
        if (dstSpotBox->spotIntersects(cropBox)) {
            visibleSpots.insert(i);
        }
        ++i;

        // Source area
        PreviewProps spp(srcSpotBox->imgArea.x1, srcSpotBox->imgArea.y1,
                         srcSpotBox->getImageWidth(), srcSpotBox->getImageHeight(), pp.getSkip());
        int w = 0;
        int h = 0;
        imgsrc->getSize(spp, w, h);
        *srcSpotBox /= pp.getSkip();
        srcSpotBox->allocImage();
        Imagefloat *srcImage = srcSpotBox->getImage();
        for (int y = 0; y < (int)srcImage->getHeight(); ++y) {
            for (int x = 0; x < (int)srcImage->getWidth(); ++x) {
                srcImage->r(y, x) = 60000.f;
                srcImage->g(y, x) = 500.f;
                srcImage->b(y, x) = 500.f;
            }
        }

        imgsrc->getImage(currWB, tr, srcSpotBox->getImage(), spp, params->toneCurve, params->raw);
        if (cmp) {
            imgsrc->convertColorSpace(srcImage, *cmp, currWB);
        }
        assert(srcSpotBox->checkImageSize());


        // Destination area
        spp.set(dstSpotBox->imgArea.x1, dstSpotBox->imgArea.y1, dstSpotBox->getImageWidth(),
                dstSpotBox->getImageHeight(), pp.getSkip());
        *dstSpotBox /= pp.getSkip();
        dstSpotBox->allocImage();
        Imagefloat *dstImage = dstSpotBox->getImage();
        for (int y = 0; y < (int)dstImage->getHeight(); ++y) {
            for (int x = 0; x < (int)dstImage->getWidth(); ++x) {
                dstImage->r(y, x) = 500.f;
                dstImage->g(y, x) = 500.f;
                dstImage->b(y, x) = 60000.f;
            }
        }
        imgsrc->getImage(currWB, tr, dstSpotBox->getImage(), spp, params->toneCurve, params->raw);
        if (cmp) {
            imgsrc->convertColorSpace(dstImage, *cmp, currWB);
        }
        assert(dstSpotBox->checkImageSize());

        // Update the intersectionArea between src and dest
        if (srcSpotBox->mutuallyClipImageArea(*dstSpotBox)) {
            srcSpotBoxs.push_back(srcSpotBox);
            dstSpotBoxs.push_back(dstSpotBox);
            boxEntries.push_back(entry);
        }

    }

    // Construct list of upstream dependencies

    std::unordered_set<int> requiredSpotsSet = calcSpotDependencies(visibleSpots, srcSpotBoxs, dstSpotBoxs);
    std::vector<int> requiredSpots(requiredSpotsSet.size());
    std::copy(requiredSpotsSet.begin(), requiredSpotsSet.end(), requiredSpots.begin());
    std::sort(requiredSpots.begin(), requiredSpots.end());

    // Process spots and copy them downstream

    // Cores of already-processed spots (scaled coords): later overlapping
    // spots must not repaint pixels an earlier spot already repaired.
    std::vector<std::array<float, 3>> processedCores;

    for (auto i = requiredSpots.begin(); i != requiredSpots.end(); i++) {
        // Process
        dstSpotBoxs.at(*i)->priorCores = &processedCores;
        srcSpotBoxs.at(*i)->processIntersectionWith(*dstSpotBoxs.at(*i));

        {
            const auto& processedEntry = boxEntries.at(*i);
            processedCores.push_back({
                float(processedEntry.targetPos.x) / float(pp.getSkip()),
                float(processedEntry.targetPos.y) / float(pp.getSkip()),
                float(processedEntry.radius) / float(pp.getSkip())});
        }

        // Propagate
        std::set<int> positiveSpots;  // For DEBUG purpose only !
        auto j = i;
        ++j;
        while (j != requiredSpots.end()) {
            bool intersectionFound = false;
            int i_ = *i;
            int j_ = *j;
            intersectionFound |= dstSpotBoxs.at(i_)->copyImgTo(*srcSpotBoxs.at(j_));
            intersectionFound |= dstSpotBoxs.at(i_)->copyImgTo(*dstSpotBoxs.at(j_));
            if (intersectionFound) {
                positiveSpots.insert(j_);
            }
            ++j;
        }
    }

    // Copy the dest spot to the preview image
    cropBox /= pp.getSkip();
    cropBox.tuneImageSize();
    cropBox.intersectionArea = cropBox.imgArea;

    int f = 0;
    for (auto i : visibleSpots) {
        f += dstSpotBoxs.at(i)->copyImgTo(cropBox) ? 1 : 0;
    }
    if (repairJob && allRepairsReady) {
        repairJob->composed = true;
        repairJob->setStage(SmartRepairStage::Presenting);
    }
}

}

namespace
{

void addSpotDependencies(int spotNum, std::unordered_set<int> &dependencies, const Boxes &srcSpots, const Boxes &dstSpots)
{
    dependencies.insert(spotNum);

    // Our spot can depend on previous spots.
    for (int i = spotNum - 1; i >= 0; --i) {
        if (dependencies.find(i) != dependencies.end()) {
            continue; // Spot already has its dependencies added.
        }

        // Check if our spot depends on this previous spot.
        if (srcSpots.at(spotNum)->imageIntersects(*dstSpots.at(i))) {
            // If so, add it and its dependencies.
            addSpotDependencies(i, dependencies, srcSpots, dstSpots);
        }
    }
}

std::unordered_set<int> calcSpotDependencies(const std::set<int> &visibleSpots, const Boxes &srcSpots, const Boxes &dstSpots)
{
    std::unordered_set<int> dependencies;
    std::vector<int> visibleSpotsOrdered(visibleSpots.size());

    std::copy(visibleSpots.begin(), visibleSpots.end(), visibleSpotsOrdered.begin());
    std::sort(visibleSpotsOrdered.begin(), visibleSpotsOrdered.end());

    // Add dependencies, starting with the last spot.
    for (auto i = visibleSpotsOrdered.crbegin(); i != visibleSpotsOrdered.crend(); ++i) {
        if (dependencies.find(*i) != dependencies.end()) {
            continue; // Spot already has its dependencies added.
        }
        addSpotDependencies(*i, dependencies, srcSpots, dstSpots);
    }

    return dependencies;
}

}
