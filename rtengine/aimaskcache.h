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
#include "aisegmentation.h"
#include "rtgui/threadutils.h"

namespace rtengine
{

class AIMaskCache
{
public:
    static AIMaskCache& getInstance();

    // Run segmentation and cache results for the given image identity
    // fullW/fullH are the full-resolution image dimensions (before any downscaling)
    void computeMasks(const std::string& imageId,
                      float* const* rRows, float* const* gRows, float* const* bRows,
                      int width, int height, int fullW, int fullH, bool multiThread);

    // Get cached probability for a specific class at a specific pixel (no imageId check)
    float getMaskValue(AISegClass cls, int y, int x) const;

    // Get entire cached mask for a class (no imageId check).
    // IMPORTANT: caller must hold a ScopedLock from acquireLock() while
    // using the returned pointer to prevent cache invalidation.
    const array2D<float>* getMask(AISegClass cls) const;

    // Get entire cached mask (unsynchronized — caller must already hold the mutex)
    const array2D<float>* getMaskUnsafe(AISegClass cls) const;

    // Expose mutex for callers that need to hold the lock across multiple calls.
    // Use with MyMutex::MyLock to prevent cache invalidation while using pointers.
    MyMutex& mutex() const { return mutex_; }

    // Check if masks are cached for this image
    bool hasCachedMasks(const std::string& imageId) const;

    // Check if any masks are cached
    bool hasCachedMasks() const;

    // Invalidate cache for this image
    void invalidate(const std::string& imageId);

    // Invalidate all cached masks
    void invalidateAll();

    // Get cached image dimensions (self-locking)
    int getCachedWidth() const;
    int getCachedHeight() const;

    // Get cached image dimensions (unsynchronized — caller must already hold a ScopedLock)
    int getCachedWidthUnsafe() const { return cachedWidth_; }
    int getCachedHeightUnsafe() const { return cachedHeight_; }

    // Get full-resolution image dimensions (needed for coordinate mapping)
    int getFullWidth() const;
    int getFullHeight() const;

    // Unsynchronized versions — caller must already hold the mutex
    int getFullWidthUnsafe() const { return fullW_; }
    int getFullHeightUnsafe() const { return fullH_; }

private:
    AIMaskCache();

    mutable MyMutex mutex_;
    std::string cachedImageId_;
    std::vector<array2D<float>> cachedMasks_;
    int cachedWidth_;
    int cachedHeight_;
    int fullW_;
    int fullH_;
};

} // namespace rtengine

#endif // RT_AI_MASKING
