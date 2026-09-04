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

#include "array2D.h"
#include "noncopyable.h"

namespace rtengine
{

// Finding the subject, as opposed to naming what is in the picture.
//
// The scene parser behind AISegmentationEngine knows 150 ADE20K classes and
// is good at sky, foliage and buildings. It is poor at "the subject": one
// generic animal class, rare in its training set, and an output eight times
// downsampled, so fur and hair never survive. U^2-Net is trained for exactly
// this instead -- salient object detection, one foreground map, fine
// structure kept -- and it is what the SUBJECT pseudo-class uses when it is
// available. When it is not, the composed-from-classes subject stands in.
class AISubjectEngine : public NonCopyable
{
public:
    AISubjectEngine();
    ~AISubjectEngine();

    bool init(const std::string& modelPath);
    // Session creation for a 170 MB model is measured in seconds; doing it on
    // a worker keeps it out of startup, as the inpainting model does.
    void initDeferred(const std::string& modelPath);
    bool isInitialized() const;

    // Planar linear RGB [0,65535] in, one saliency map in 0..1 at the same
    // size out. Empty when the model is unavailable.
    array2D<float> saliency(float* const* rRows, float* const* gRows, float* const* bRows,
                            int width, int height, bool multiThread) const;

private:
    struct Impl;
    Impl* pImpl;
};

AISubjectEngine& getAISubjectEngine();

} // namespace rtengine

#endif // RT_AI_MASKING
