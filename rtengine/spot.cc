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
#include "procparams.h"
#include "imagesource.h"
#include "imagefloat.h"
#include "rt_math.h"
#include <array>
#include <iostream>
#include <set>
#include <unordered_set>

#ifdef RT_AI_MASKING
#include "aiinpainting.h"
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
            case SpotMethod::HEAL:   return processHeal(destBox);
            case SpotMethod::ERASE:  return processErase(destBox);
            case SpotMethod::REDEYE: return processRedEye(destBox);
            default:                 return processClone(destBox);
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

#ifdef RT_AI_MASKING
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
    float featherRadius = entry.getFeatherRadius() / float(skip);

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

    // Add padding for AI context
    int pad = int(featherRadius * 2);
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

    // Build mask from stroke points
    for (const auto& pt : entry.strokePoints) {
        float cx = float(pt.x - cropX) / float(skip) - bbMinX;
        float cy = float(pt.y - cropY) / float(skip) - bbMinY;

        int pyMin = std::max(0, int(cy - radius));
        int pyMax = std::min(bbH - 1, int(cy + radius));
        int pxMin = std::max(0, int(cx - radius));
        int pxMax = std::min(bbW - 1, int(cx + radius));

        for (int py = pyMin; py <= pyMax; ++py) {
            for (int px = pxMin; px <= pxMax; ++px) {
                float dx = float(px) - cx;
                float dy = float(py) - cy;
                float dist = sqrtf(dx * dx + dy * dy);
                if (dist <= radius) {
                    mask[py * bbW + px] = 1.f;
                }
            }
        }
    }

    // Keep the repairs from earlier strokes intact in any overlap
    excludePriorStrokeCoverage(mask, bbW, bbH, bbMinX, bbMinY, cropX, cropY, skip, priorStrokes);

    // Run AI inpainting
    std::vector<float> outR(bbSize), outG(bbSize), outB(bbSize);
    if (engine.inpaint(cropR.data(), cropG.data(), cropB.data(),
                       mask.data(), bbW, bbH,
                       outR.data(), outG.data(), outB.data())) {
        // Write results back to image
        for (int y = 0; y < bbH; ++y) {
            for (int x = 0; x < bbW; ++x) {
                int ix = bbMinX + x;
                int iy = bbMinY + y;
                int idx = y * bbW + x;
                if (mask[idx] > 0.5f) {
                    img->r(iy, ix) = outR[idx];
                    img->g(iy, ix) = outG[idx];
                    img->b(iy, ix) = outB[idx];
                }
            }
        }
    }
}
#endif // RT_AI_MASKING

} // anonymous namespace

void ImProcFunctions::removeSpots (Imagefloat* img, ImageSource* imgsrc, const std::vector<SpotEntry> &entries, const PreviewProps &pp, const ColorTemp &currWB, const ColorManagementParams *cmp, int tr)
{
    // Process stroke-based entries directly on the image first. Each entry
    // knows which strokes came before it, so repainting the border of an
    // already-repaired spot leaves the earlier repair untouched.
    std::vector<const SpotEntry*> priorStrokes;
    for (const auto& entry : params->spot.entries) {
        if (entry.isStroke()) {
#ifdef RT_AI_MASKING
            if (static_cast<int>(entry.method) >= static_cast<int>(SpotMethod::AI_REMOVE)) {
                processStrokeAI(img, entry, pp, priorStrokes);
            } else
#endif
            {
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

