/*
 *  This file is part of RawTherapee.
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

#ifdef RT_AI_MASKING

// Composing "the subject" out of the model's class probabilities. Shared by
// the edited image's mask cache and the double exposure tool's partner mask
// store, so both mean the same thing by SUBJECT.

#include <algorithm>
#include <vector>

#include "aisegmentation.h"
#include "aisubjectmodel.h"
#include "array2D.h"
#include "rt_math.h"

namespace rtengine
{

/** Compose the SUBJECT pseudo-class from the model output: the union of the
 *  person/vehicle/animal/foreground-object probabilities, cut down to its
 *  dominant connected regions (every region at least 30% the size of the
 *  largest, so a second person in a group shot survives), with interior
 *  holes filled. NOT_SUBJECT is its complement. Both are appended to @p maps
 *  in AISegClass order.
 */
inline void appendSubjectMasks(std::vector<array2D<float>>& maps, int width, int height, bool multiThread)
{
    if (static_cast<int>(maps.size()) != static_cast<int>(AISegClass::NUM_CLASSES)
            || width <= 0 || height <= 0) {
        return;
    }

    maps.reserve(static_cast<std::size_t>(AISegClass::TOTAL_CLASSES));

    const array2D<float>& person = maps[static_cast<int>(AISegClass::PERSON)];
    const array2D<float>& vehicle = maps[static_cast<int>(AISegClass::VEHICLE)];
    const array2D<float>& animal = maps[static_cast<int>(AISegClass::ANIMAL)];
    const array2D<float>& object = maps[static_cast<int>(AISegClass::FOREGROUND_OBJECT)];

    maps.emplace_back(width, height);
    array2D<float>& subject = maps[static_cast<int>(AISegClass::SUBJECT)];

#ifdef _OPENMP
    #pragma omp parallel for if(multiThread)
#endif
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            subject[y][x] = std::max(std::max(person[y][x], vehicle[y][x]),
                                     std::max(animal[y][x], object[y][x]));
        }
    }

    // The support threshold matches the default mask threshold, so the soft
    // probability skirt around a kept region survives into the composed map.
    constexpr float support = 0.3f;

    // Connected components over the support region (4-neighborhood).
    const int n = width * height;
    std::vector<int> label(n, 0);
    std::vector<int> stack;
    std::vector<int> sizes(1, 0); // 1-based; sizes[0] unused
    int labelCount = 0;

    for (int seed = 0; seed < n; ++seed) {
        if (label[seed] != 0 || subject[seed / width][seed % width] <= support) {
            continue;
        }

        ++labelCount;
        int size = 0;
        stack.assign(1, seed);
        label[seed] = labelCount;

        while (!stack.empty()) {
            const int p = stack.back();
            stack.pop_back();
            ++size;
            const int y = p / width;
            const int x = p % width;

            const auto visit = [&](int q) {
                if (label[q] == 0 && subject[q / width][q % width] > support) {
                    label[q] = labelCount;
                    stack.push_back(q);
                }
            };
            if (y > 0) visit(p - width);
            if (y < height - 1) visit(p + width);
            if (x > 0) visit(p - 1);
            if (x < width - 1) visit(p + 1);
        }

        sizes.push_back(size);
    }

    if (labelCount > 0) {
        int largest = 0;
        for (int l = 1; l <= labelCount; ++l) {
            largest = std::max(largest, sizes[l]);
        }
        const int keepAbove = std::max(1, largest * 3 / 10);
        std::vector<char> keep(labelCount + 1, 0);
        for (int l = 1; l <= labelCount; ++l) {
            keep[l] = sizes[l] >= keepAbove ? 1 : 0;
        }

        const auto kept = [&](int p) {
            return label[p] != 0 && keep[label[p]];
        };

        // Flood the true outside (non-kept pixels reachable from the border);
        // what remains un-flooded and non-kept is an interior hole.
        std::vector<char> outside(n, 0);
        stack.clear();
        const auto seedOutside = [&](int p) {
            if (!outside[p] && !kept(p)) {
                outside[p] = 1;
                stack.push_back(p);
            }
        };
        for (int x = 0; x < width; ++x) {
            seedOutside(x);
            seedOutside(n - width + x);
        }
        for (int y = 0; y < height; ++y) {
            seedOutside(y * width);
            seedOutside(y * width + width - 1);
        }
        while (!stack.empty()) {
            const int p = stack.back();
            stack.pop_back();
            const int y = p / width;
            const int x = p % width;
            if (y > 0) seedOutside(p - width);
            if (y < height - 1) seedOutside(p + width);
            if (x > 0) seedOutside(p - 1);
            if (x < width - 1) seedOutside(p + 1);
        }

        for (int p = 0; p < n; ++p) {
            if (!kept(p)) {
                if (!outside[p]) {
                    // An interior hole is part of the subject: the model
                    // rarely fires on an eye or a dark patch of fur, and the
                    // user did not ask for a subject with holes in it.
                    subject[p / width][p % width] = 1.f;
                } else {
                    // Everything else is held BELOW the support threshold
                    // rather than zeroed. Zeroing threw away the model's own
                    // ordering, and with it the tolerance control: a region
                    // the model gave 0.28 became a flat 0, so lowering the
                    // threshold could never reach it however far it went.
                    // Capped like this the default behaviour is unchanged --
                    // nothing here passes 0.3 -- but a subject the model was
                    // unsure about can now be dialled in, strongest first.
                    float& value = subject[p / width][p % width];
                    value = std::min(value, support * 0.99f);
                }
            }
        }
    }

    maps.emplace_back(width, height);
    array2D<float>& notSubject = maps[static_cast<int>(AISegClass::NOT_SUBJECT)];

#ifdef _OPENMP
    #pragma omp parallel for if(multiThread)
#endif
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            notSubject[y][x] = LIM(1.f - subject[y][x], 0.f, 1.f);
        }
    }
}

/** Replaces the composed SUBJECT (and its complement) with a purpose-built
 *  saliency model's answer, when one is loaded. The composition above is a
 *  reasonable guess assembled out of a scene parser's object classes; U^2-Net
 *  was trained to do this one job, and on anything with fur or hair the
 *  difference is not subtle. Falls back silently, so a build without the
 *  model — or one where it is still loading — keeps the composed subject.
 */
inline bool applySubjectModel(std::vector<array2D<float>>& maps,
                              float* const* rRows, float* const* gRows, float* const* bRows,
                              int width, int height, bool multiThread)
{
    if (static_cast<int>(maps.size()) < static_cast<int>(AISegClass::TOTAL_CLASSES)) {
        return false;
    }

    AISubjectEngine& engine = getAISubjectEngine();

    if (!engine.isInitialized()) {
        return false;
    }

    array2D<float> saliency = engine.saliency(rRows, gRows, bRows, width, height, multiThread);

    if (saliency.getWidth() != width || saliency.getHeight() != height) {
        return false;
    }

    array2D<float>& subject = maps[static_cast<int>(AISegClass::SUBJECT)];
    array2D<float>& notSubject = maps[static_cast<int>(AISegClass::NOT_SUBJECT)];

#ifdef _OPENMP
    #pragma omp parallel for if(multiThread)
#endif
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float value = LIM01(saliency[y][x]);
            subject[y][x] = value;
            notSubject[y][x] = 1.f - value;
        }
    }

    return true;
}

} // namespace rtengine

#endif // RT_AI_MASKING
