#include "depthestimator.h"
#include "imagefloat.h"
#include "iccstore.h"
#include "color.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <list>
#include <glibmm/miscutils.h>
#include <glibmm/convert.h>
#ifdef RT_AI_MASKING
#include "onnxruntime_compat.h"
#include <onnxruntime_cxx_api.h>
#ifdef RT_AI_MASKING_DIRECTML
#include <dml_provider_factory.h>
#endif
#include <cstdio>
#endif
namespace rtengine
{
struct DepthEstimator::Impl {
#ifdef RT_AI_MASKING
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "SteepDepth"};
    Ort::SessionOptions options;
    bool directml = false;
    std::unique_ptr<Ort::Session> depthModel;
    static std::unique_ptr<Impl> create()
    {
        // Check before constructing any ORT wrapper: even a null wrapper's
        // destructor calls through the runtime API table.
        if (!OrtGetApiBase()->GetApi(ORT_API_VERSION))
            throw std::runtime_error(
                "The installed ONNX runtime is too old for depth estimation; repair the AI runtime installation");
        return std::unique_ptr<Impl>(new Impl);
    }
    Impl()
    {
#ifdef RT_AI_MASKING_DIRECTML
        // The same arrangement the masking and denoise models use: DirectML
        // wants sequential execution without the CPU memory planner. A machine
        // without a usable adapter keeps the CPU path without any change in
        // results, only in speed.
        options.DisableMemPattern();
        options.DisableCpuMemArena();
        options.SetExecutionMode(ORT_SEQUENTIAL);
        if (OrtStatus *status = OrtSessionOptionsAppendExecutionProvider_DML(options, 0)) {
            std::fprintf(stderr, "Depth: DirectML unavailable (%s), using CPU\n",
                         Ort::GetApi().GetErrorMessage(status));
            Ort::GetApi().ReleaseStatus(status);
        } else {
            directml = true;
        }
#endif
        if (!directml) {
            options.SetIntraOpNumThreads(4);
            options.SetInterOpNumThreads(1);
        }
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    }
#endif
};
DepthEstimator::DepthEstimator() = default;
DepthEstimator::~DepthEstimator() = default;
std::unique_ptr<Imagefloat> DepthEstimator::depth(const Imagefloat &src, const std::string &dir,
                                                const Glib::ustring &workingProfile)
{
#ifndef RT_AI_MASKING
    throw std::runtime_error("This build has no ONNX Runtime support");
#else
    if (!impl)
        impl = Impl::create();
    if (!impl->depthModel) {
        const auto path = Glib::build_filename(dir, "composite_depth.onnx");
#ifdef _WIN32
        const auto utf16 = Glib::convert(path, "UTF-16LE", "UTF-8");
        std::wstring wide(utf16.size() / 2, L'\0');
        std::memcpy(&wide[0], utf16.data(), utf16.size());
        impl->depthModel.reset(new Ort::Session(impl->env, wide.c_str(), impl->options));
#else
        impl->depthModel.reset(new Ort::Session(impl->env, path.c_str(), impl->options));
#endif
    }
    constexpr int size = 518;
    const double ratio = double(size) / std::max(src.getWidth(), src.getHeight());
    const int w = std::max(1, int(src.getWidth() * ratio)), h = std::max(1, int(src.getHeight() * ratio));
    const int dx = (size - w) / 2, dy = (size - h) / 2;
    std::vector<float> input(size * size * 3);
    const auto matrix = ICCStore::getInstance()->workingSpaceMatrix(workingProfile);
    const float mean[] = {.485f, .456f, .406f}, stddev[] = {.229f, .224f, .225f};
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const int sx = std::max(0, std::min(src.getWidth() - 1, int((x - dx + .5) * src.getWidth() / w)));
            const int sy =
                std::max(0, std::min(src.getHeight() - 1, int((y - dy + .5) * src.getHeight() / h)));
            float X, Y, Z, rgb[3];
            Color::rgbxyz(src.r(sy, sx), src.g(sy, sx), src.b(sy, sx), X, Y, Z, matrix);
            Color::xyz2srgb(X, Y, Z, rgb[0], rgb[1], rgb[2]);
            for (int c = 0; c < 3; ++c) {
                float v = std::max(0.f, std::min(1.f, rgb[c] / 65535.f));
                v = v <= .0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.f / 2.4f) - .055f;
                input[c * size * size + y * size + x] = (v - mean[c]) / stddev[c];
            }
        }
    auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::array<int64_t, 4> shape = {{1, 3, size, size}};
    auto tensor =
        Ort::Value::CreateTensor<float>(memory, input.data(), input.size(), shape.data(), shape.size());
    const char *in[] = {"image"};
    const char *out[] = {"depth"};
    auto result = impl->depthModel->Run(Ort::RunOptions{nullptr}, in, &tensor, 1, out, 1);
    if (result[0].GetTensorTypeAndShapeInfo().GetShape() != std::vector<int64_t>{1, size, size})
        throw std::runtime_error("Unsupported depth model output");
    const float *data = result[0].GetTensorData<float>();
    float low = 1e30f, high = -1e30f;
    for (int y = dy; y < dy + h; ++y)
        for (int x = dx; x < dx + w; ++x) {
            const float d = data[y * size + x];
            if (std::isfinite(d)) {
                low = std::min(low, d);
                high = std::max(high, d);
            }
        }
    if (high - low < 1e-6f)
        throw std::runtime_error("No reliable depth range in this image");
    auto map = std::unique_ptr<Imagefloat>(new Imagefloat(w, h));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float d = data[(y + dy) * size + x + dx];
            const float v = std::isfinite(d) ? 65535.f * (high - d) / (high - low) : -65535.f;
            map->r(y, x) = map->g(y, x) = map->b(y, x) = v;
        }
    return map;
#endif
}
} // namespace rtengine
