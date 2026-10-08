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
    std::unique_ptr<Imagefloat> depth(const Imagefloat &source, const std::string &modelDir,
                                      const Glib::ustring &workingProfile = "ProPhoto");

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace rtengine
