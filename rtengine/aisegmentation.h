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
    NUM_CLASSES,            ///< = 8: classes the model produces directly

    // Pseudo-classes composed by AIMaskCache from the model output. They live
    // past NUM_CLASSES so model-facing code (tensor sizing, class grouping)
    // keeps its 8-class world, while mask consumers may index all of these.
    SUBJECT = NUM_CLASSES,  ///< person|vehicle|animal|foreground → dominant connected regions, holes filled
    NOT_SUBJECT,            ///< complement of SUBJECT
    // By estimated depth rather than by kind, split where the scene's depths
    // fall apart. Filled only when a mask uses them (AIMaskCache::setDepthClasses).
    FOREGROUND_DEPTH,       ///< the near part of the scene
    BACKGROUND_DEPTH,       ///< the far part
    TOTAL_CLASSES           ///< = 12: model classes + composed pseudo-classes
};

class AISegmentationEngine : public NonCopyable
{
public:
    AISegmentationEngine();
    ~AISegmentationEngine();

    bool init(const std::string& modelPath);
    bool isInitialized() const;
    // The model file in use, part of the persisted class maps' key.
    std::string modelPath() const;

    // Takes planar RGB float [0,65535] as row pointers, returns per-class probability maps
    // Output maps are sized to match input dimensions. `ok`, when given, is
    // cleared when inference failed and the maps are not to be trusted.
    std::vector<array2D<float>> segment(float* const* rRows, float* const* gRows, float* const* bRows,
                                         int width, int height, bool multiThread, bool* ok = nullptr) const;

    static int getClassCount();
    static const char* getClassName(AISegClass cls);

private:
    struct Impl;
    Impl* pImpl;
};

AISegmentationEngine& getAISegmentationEngine();

} // namespace rtengine

#endif // RT_AI_MASKING
