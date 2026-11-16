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

#include <memory>

#include <cstring>

namespace rtengine
{

class LabImage final
{
private:
    void allocLab(size_t w, size_t h);

public:
    int W, H;
    float * data;
    float** L;
    float** a;
    float** b;

    LabImage (int w, int h, bool initZero = false, bool multiThread = true);
    LabImage (const LabImage& source, bool multiThread);
    ~LabImage ();

    //Copies image data in Img into this instance.
    void CopyFrom(const LabImage *Img, bool multiThread = true);
    void getPipetteData (float &L, float &a, float &b, int posX, int posY, int squareSize) const;
    void deleteLab();
    void reallocLab();
    void clear(bool multiThread = false);
};

}

// Local adjustments keep two copies of the Lab image per pass. The copies
// are needed; allocating them (and faulting in ~20 MB of fresh pages each)
// on every pass is not, so the buffers persist while their size holds.
inline const std::unique_ptr<rtengine::LabImage>& reuseLabCopy(std::unique_ptr<rtengine::LabImage>& buffer,
                                                               const rtengine::LabImage& source)
{
    if (!buffer || buffer->W != source.W || buffer->H != source.H) {
        buffer.reset(new rtengine::LabImage(source, true));
    } else {
        buffer->CopyFrom(&source);
    }
    return buffer;
}
