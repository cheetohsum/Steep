/* -*- C++ -*-
 *
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
#include "partnerdepthstore.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <stdexcept>

#include <glibmm/fileutils.h>
#include <glibmm/miscutils.h>

#include "cache.h"
#include "compositemath.h"
#include "depthestimator.h"
#include "imagefloat.h"
#include "partnerimagestore.h"

namespace rtengine
{

float PartnerDepth::sample(float u, float v) const
{
    if (!valid()) {
        return -1.f;
    }

    const float tu = u * width / std::max(1, fullWidth) - 0.5f;
    const float tv = v * height / std::max(1, fullHeight) - 0.5f;

    int x0 = static_cast<int>(std::floor(tu));
    int y0 = static_cast<int>(std::floor(tv));
    float dx = tu - x0;
    float dy = tv - y0;

    if (x0 < 0) {
        x0 = 0;
        dx = 0.f;
    } else if (x0 > width - 1) {
        x0 = width - 1;
        dx = 0.f;
    }

    if (y0 < 0) {
        y0 = 0;
        dy = 0.f;
    } else if (y0 > height - 1) {
        y0 = height - 1;
        dy = 0.f;
    }

    const int x1 = x0 + 1 < width ? x0 + 1 : width - 1;
    const int y1 = y0 + 1 < height ? y0 + 1 : height - 1;
    const float d00 = depth[y0][x0], d10 = depth[y0][x1], d01 = depth[y1][x0], d11 = depth[y1][x1];

    if (d00 < 0.f || d10 < 0.f || d01 < 0.f || d11 < 0.f) {
        return -1.f;
    }

    return (d00 * (1.f - dx) + d10 * dx) * (1.f - dy) + (d01 * (1.f - dx) + d11 * dx) * dy;
}

bool DepthSelection::operator==(const DepthSelection& other) const
{
    return range == other.range && nearShape == other.nearShape && farShape == other.farShape
           && invert == other.invert;
}

float DepthSelection::weight(float depth) const
{
    return composite::depthWeight(depth, range, nearShape, farShape, invert);
}

struct PartnerDepthStore::Impl {
    std::mutex mutex;                        // the directory, the estimator and its session
    Glib::ustring directory;
    bool haveModel = false;
    std::unique_ptr<DepthEstimator> estimator;
    Cache<Glib::ustring, std::shared_ptr<PartnerDepth>> cache{8};
};

PartnerDepthStore::PartnerDepthStore() : impl(new Impl)
{
}

PartnerDepthStore::~PartnerDepthStore() = default;

PartnerDepthStore& PartnerDepthStore::getInstance()
{
    static PartnerDepthStore instance;
    return instance;
}

void PartnerDepthStore::setModelDirectory(const Glib::ustring& directory)
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    impl->directory = directory;
    impl->haveModel = !directory.empty()
                      && Glib::file_test(Glib::build_filename(directory, "composite_depth.onnx"), Glib::FILE_TEST_EXISTS);
}

bool PartnerDepthStore::available() const
{
#ifdef RT_AI_MASKING
    std::lock_guard<std::mutex> lock(impl->mutex);
    return impl->haveModel;
#else
    return false;
#endif
}

std::shared_ptr<const PartnerDepth> PartnerDepthStore::peekDepth(const Glib::ustring& path, const Glib::ustring& workingProfile)
{
    if (path.empty()) {
        return nullptr;
    }

    std::shared_ptr<PartnerDepth> result;

    if (impl->cache.get(path + "\n" + workingProfile, result)) {
        return result;
    }

    return nullptr;
}

std::shared_ptr<const PartnerDepth> PartnerDepthStore::getDepth(const Glib::ustring& path, const Glib::ustring& workingProfile)
{
    if (path.empty() || !available()) {
        return nullptr;
    }

    const Glib::ustring key = path + "\n" + workingProfile;
    std::shared_ptr<PartnerDepth> result;

    if (impl->cache.get(key, result)) {
        return result;
    }

    // The preview tier the composite already decoded; the estimator reads a
    // 518 pixel square, so nothing larger would help it.
    auto partner = PartnerImageStore::getInstance().getPartner(path, workingProfile, false);

    if (!partner || !partner->image || partner->fullWidth <= 0 || partner->fullHeight <= 0) {
        return nullptr;
    }

    std::unique_ptr<Imagefloat> map;
    {
        std::lock_guard<std::mutex> lock(impl->mutex);

        if (impl->cache.get(key, result)) {
            return result;
        }

        try {
            if (!impl->estimator) {
                impl->estimator.reset(new DepthEstimator());
            }

            map = impl->estimator->depth(*partner->image, impl->directory.raw(), workingProfile);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "Double exposure depth: %s\n", e.what());
            return nullptr;
        }
    }

    if (!map || map->getWidth() < 1 || map->getHeight() < 1) {
        return nullptr;
    }

    result = std::make_shared<PartnerDepth>();
    result->width = map->getWidth();
    result->height = map->getHeight();
    result->fullWidth = partner->fullWidth;
    result->fullHeight = partner->fullHeight;
    result->depth(result->width, result->height);

    for (int y = 0; y < result->height; ++y) {
        for (int x = 0; x < result->width; ++x) {
            const float d = map->r(y, x);
            result->depth[y][x] = std::isfinite(d) && d >= 0.f ? std::min(1.f, d / 65535.f) : -1.f;
        }
    }

    impl->cache.insert(key, result);
    return result;
}

void PartnerDepthStore::clearCache()
{
    impl->cache.clear();
}

} // namespace rtengine
