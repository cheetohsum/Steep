#pragma once
#include <array>
#include <memory>
#include <string>
#include <vector>
#include <glibmm/ustring.h>
#include "array2D.h"

namespace rtengine
{
class Imagefloat;

// Click-to-select on a double exposure partner: SAM 2.1 Tiny, promptable
// with include/exclude points, run locally through ONNX Runtime. One image
// embedding is kept per partner so further clicks on the same picture cost
// only the decoder. The selected candidate is refined against the picture's
// own luminance so the mask follows edges between decoder pixels.
class SamSelector final
{
public:
    static SamSelector& getInstance();

    SamSelector(const SamSelector&) = delete;
    SamSelector& operator=(const SamSelector&) = delete;

    // Directory holding sam2_encoder.onnx and sam2_decoder.onnx; set once by rtengine::init.
    void setModelDirectory(const Glib::ustring& directory);
    bool available() const;

    // Prompts are normalised source coordinates with label 1 include / 0
    // exclude. Returns a 0..1 mask over the source's pixel grid, or an empty
    // array when the model is unavailable or inference failed. Blocking:
    // run it on a worker.
    // `candidate` picks one of the decoder's three nested candidates by size
    // (0 smallest .. 2 largest); -1 lets the scores decide, as a click does.
    // With `reference` (a 0..1 map over the source grid), the candidate that
    // overlaps it best is taken instead: how a coarse mask asks SAM to redraw
    // its own outline rather than whatever part or group the clicks suggest.
    array2D<float> select(const Imagefloat& source, const std::string& sourceId,
                          const std::vector<std::array<float, 3>>& prompts, int candidate = -1,
                          const array2D<float>* reference = nullptr);

private:
    SamSelector();
    ~SamSelector();
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace rtengine
