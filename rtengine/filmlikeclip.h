#pragma once

namespace rtengine {

// rgbProc's hue-preserving clipping, in working RGB at the 0..65535 scale.
void filmlike_clip(float* r, float* g, float* b);

}
