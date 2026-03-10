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

#ifdef RT_AI_MASKING

#include "aimaskcache.h"

namespace rtengine
{

AIMaskCache::AIMaskCache() : cachedWidth_(0), cachedHeight_(0), fullW_(0), fullH_(0)
{
}

AIMaskCache& AIMaskCache::getInstance()
{
    static AIMaskCache instance;
    return instance;
}

void AIMaskCache::computeMasks(const std::string& imageId,
                                float* const* rRows, float* const* gRows, float* const* bRows,
                                int width, int height, int fullW, int fullH, bool multiThread)
{
    MyMutex::MyLock lock(mutex_);

    if (cachedImageId_ == imageId && cachedWidth_ == width && cachedHeight_ == height) {
        return; // Already cached
    }

    AISegmentationEngine& engine = getAISegmentationEngine();
    if (!engine.isInitialized()) {
        return;
    }

    cachedMasks_ = engine.segment(rRows, gRows, bRows, width, height, multiThread);
    cachedImageId_ = imageId;
    cachedWidth_ = width;
    cachedHeight_ = height;
    fullW_ = fullW;
    fullH_ = fullH;
}

float AIMaskCache::getMaskValue(AISegClass cls, int y, int x) const
{
    MyMutex::MyLock lock(mutex_);

    if (cachedMasks_.empty()) {
        return 0.f;
    }

    const int classIdx = static_cast<int>(cls);
    if (classIdx < 0 || classIdx >= static_cast<int>(cachedMasks_.size())) {
        return 0.f;
    }

    if (y < 0 || y >= cachedHeight_ || x < 0 || x >= cachedWidth_) {
        return 0.f;
    }

    return cachedMasks_[classIdx][y][x];
}

const array2D<float>* AIMaskCache::getMask(AISegClass cls) const
{
    MyMutex::MyLock lock(mutex_);
    return getMaskUnsafe(cls);
}

const array2D<float>* AIMaskCache::getMaskUnsafe(AISegClass cls) const
{
    if (cachedMasks_.empty()) {
        return nullptr;
    }

    const int classIdx = static_cast<int>(cls);
    if (classIdx < 0 || classIdx >= static_cast<int>(cachedMasks_.size())) {
        return nullptr;
    }

    return &cachedMasks_[classIdx];
}

bool AIMaskCache::hasCachedMasks(const std::string& imageId) const
{
    MyMutex::MyLock lock(mutex_);
    return cachedImageId_ == imageId && !cachedMasks_.empty();
}

bool AIMaskCache::hasCachedMasks() const
{
    MyMutex::MyLock lock(mutex_);
    return !cachedMasks_.empty();
}

void AIMaskCache::invalidate(const std::string& imageId)
{
    MyMutex::MyLock lock(mutex_);
    if (cachedImageId_ == imageId) {
        cachedImageId_.clear();
        cachedMasks_.clear();
        cachedWidth_ = 0;
        cachedHeight_ = 0;
        fullW_ = 0;
        fullH_ = 0;
    }
}

void AIMaskCache::invalidateAll()
{
    MyMutex::MyLock lock(mutex_);
    cachedImageId_.clear();
    cachedMasks_.clear();
    cachedWidth_ = 0;
    cachedHeight_ = 0;
    fullW_ = 0;
    fullH_ = 0;
}

int AIMaskCache::getCachedWidth() const
{
    MyMutex::MyLock lock(mutex_);
    return cachedWidth_;
}

int AIMaskCache::getCachedHeight() const
{
    MyMutex::MyLock lock(mutex_);
    return cachedHeight_;
}

int AIMaskCache::getFullWidth() const
{
    MyMutex::MyLock lock(mutex_);
    return fullW_;
}

int AIMaskCache::getFullHeight() const
{
    MyMutex::MyLock lock(mutex_);
    return fullH_;
}

} // namespace rtengine

#endif // RT_AI_MASKING
