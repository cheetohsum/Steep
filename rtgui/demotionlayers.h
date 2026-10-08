#pragma once
#include "rtengine/procparams.h"
#include <algorithm>

namespace demotionui
{
inline int addMotionLayer(rtengine::procparams::DoubleExposureParams &params, const Glib::ustring &base)
{
    using DE = rtengine::procparams::DoubleExposureParams;
    using Path = rtengine::procparams::DEMotionParams::Path;
    if (params.layers.size() >= 8 || (params.layers.empty() && base.empty()))
        return -1;
    if (params.layers.empty()) {
        DE::Layer sharp;
        sharp.path = base;
        params.layers.push_back(sharp);
    }
    auto source = std::find_if(params.layers.begin(), params.layers.end(),
                               [](const DE::Layer &l) { return l.motion.path == Path::OFF; });
    if (source == params.layers.end())
        source = params.layers.begin();
    const int index = int(source - params.layers.begin());
    DE::Layer motion = *source;
    motion.enabled = true;
    motion.blendMode = DE::BlendMode::ADD;
    motion.motion = {};
    motion.motion.path = Path::LINE;
    source->blendMode = DE::BlendMode::ADAPTIVE;
    source->compare = DE::Compare::LUMINANCE;
    params.layers.insert(params.layers.begin() + index, motion);
    params.enabled = true;
    return index;
}
} // namespace demotionui
