#pragma once
#include <algorithm>
#include <cmath>

namespace effectpreview
{
// Let bounded in-flight drafts finish during a drag, but never display time backwards.
inline bool canPublish(bool draft, unsigned completed, unsigned requested, unsigned presented)
{
    return completed >= presented && (draft || completed == requested);
}
inline int draftEdge(unsigned visibleLayers)
{
    // Keep interaction work bounded as stacks grow; settled previews keep their full tier.
    const double edge = 768. / std::sqrt(std::max(1., visibleLayers / 2.));
    return std::max(256, int(edge / 64) * 64);
}
}
