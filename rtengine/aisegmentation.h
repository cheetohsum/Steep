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

#include <string>
#include <vector>
#include "array2D.h"
#include "noncopyable.h"

namespace rtengine
{

enum class AISegClass {
    BACKGROUND = 0,
    PERSON,
    SKY,
    VEGETATION,
    BUILDING,
    VEHICLE,
    ANIMAL,
    FOREGROUND_OBJECT,
    NUM_CLASSES
};

class AISegmentationEngine : public NonCopyable
{
public:
    AISegmentationEngine();
    ~AISegmentationEngine();

    bool init(const std::string& modelPath);
    bool isInitialized() const;

    // Takes planar RGB float [0,65535] as row pointers, returns per-class probability maps
    // Output maps are sized to match input dimensions
    std::vector<array2D<float>> segment(float* const* rRows, float* const* gRows, float* const* bRows,
                                         int width, int height, bool multiThread) const;

    static int getClassCount();
    static const char* getClassName(AISegClass cls);

private:
    struct Impl;
    Impl* pImpl;
};

AISegmentationEngine& getAISegmentationEngine();

} // namespace rtengine

#endif // RT_AI_MASKING
