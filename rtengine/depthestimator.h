#pragma once
#include <memory>
#include <string>
#include <glibmm/ustring.h>
namespace rtengine
{
class Imagefloat;
// Local monocular depth (Depth Anything V2 Small, ONNX), used by Double
// Exposure's depth confinement through PartnerDepthStore. Sessions are built
// lazily on the calling thread; DirectML is used when the build has it.
class DepthEstimator
{
  public:
    DepthEstimator();
    ~DepthEstimator();
    // Relative depth, 0..65535 near to far, negative where invalid. The source
    // is linear RGB in workingProfile; the map keeps the source's proportions.
    // `farRatio`, when asked for, is the model's own reading of the farthest
    // point over the nearest (its output is inverse depth, zero at infinity):
    // 0 when the scene runs out to the sky, about 0.4 for a wall a little
    // behind the subject. It turns the 0..1 map back into relative distance.
    std::unique_ptr<Imagefloat> depth(const Imagefloat &source, const std::string &modelDir,
                                      const Glib::ustring &workingProfile = "ProPhoto",
                                      float *farRatio = nullptr);

    // The same depth with sharper outlines, up to `longEdge` pixels: the
    // model run again on overlapping tiles, each fitted onto the whole-frame
    // pass (the model's depth is only known up to scale and offset, so a
    // straight fit carries the whole frame's layout into the tile's detail),
    // then snapped to the picture's own edges. Several times the work of
    // depth(); meant for the editor's scene depth.
    std::unique_ptr<Imagefloat> detailedDepth(const Imagefloat &source, const std::string &modelDir,
                                              const Glib::ustring &workingProfile, int longEdge,
                                              float *farRatio = nullptr);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace rtengine
