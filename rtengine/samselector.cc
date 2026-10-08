#include "samselector.h"
#include "imagefloat.h"
#include "iccstore.h"
#include "color.h"
#include "compositemath.h"
#include "guidedfilter.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <list>
#include <mutex>
#include <stdexcept>
#include <glibmm/fileutils.h>
#include <glibmm/miscutils.h>
#include <glibmm/convert.h>
#ifdef RT_AI_MASKING
#include "onnxruntime_compat.h"
#include <onnxruntime_cxx_api.h>
#ifdef RT_AI_MASKING_DIRECTML
#include <dml_provider_factory.h>
#endif
#endif

namespace rtengine
{

struct SamSelector::Impl {
    std::mutex mutex;
    Glib::ustring directory;
    bool haveModels = false;
#ifdef RT_AI_MASKING
    std::unique_ptr<Ort::Env> env;
    std::unique_ptr<Ort::SessionOptions> options;
    std::unique_ptr<Ort::Session> encoder, decoder;
    bool directml = false;
    struct Embedding {
        std::string id;
        std::vector<Ort::Value> features;
        size_t bytes = 0;
    };
    std::list<Embedding> embeddings;
    size_t embeddingBytes = 0;

    void prepare()
    {
        if (env) return;
        // Check before constructing any ORT wrapper: even a null wrapper's
        // destructor calls through the runtime API table.
        if (!OrtGetApiBase()->GetApi(ORT_API_VERSION))
            throw std::runtime_error("The installed ONNX runtime is too old for click selection");
        env.reset(new Ort::Env(ORT_LOGGING_LEVEL_WARNING, "SteepSelect"));
        options.reset(new Ort::SessionOptions());
#ifdef RT_AI_MASKING_DIRECTML
        options->DisableMemPattern();
        options->DisableCpuMemArena();
        options->SetExecutionMode(ORT_SEQUENTIAL);
        if (OrtStatus* status = OrtSessionOptionsAppendExecutionProvider_DML(*options, 0)) {
            std::fprintf(stderr, "Click selection: DirectML unavailable (%s), using CPU\n",
                         Ort::GetApi().GetErrorMessage(status));
            Ort::GetApi().ReleaseStatus(status);
        } else {
            directml = true;
        }
#endif
        if (!directml) {
            options->SetIntraOpNumThreads(4);
            options->SetInterOpNumThreads(1);
        }
        options->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        const auto session = [&](const char* file) {
            const auto path = Glib::build_filename(directory, file);
#ifdef _WIN32
            const auto utf16 = Glib::convert(path, "UTF-16LE", "UTF-8");
            std::wstring wide(utf16.size() / 2, L'\0');
            std::memcpy(&wide[0], utf16.data(), utf16.size());
            return std::unique_ptr<Ort::Session>(new Ort::Session(*env, wide.c_str(), *options));
#else
            return std::unique_ptr<Ort::Session>(new Ort::Session(*env, path.c_str(), *options));
#endif
        };
        encoder = session("sam2_encoder.onnx");
        decoder = session("sam2_decoder.onnx");
    }
#endif
};

SamSelector::SamSelector() : impl(new Impl)
{
}

SamSelector::~SamSelector() = default;

SamSelector& SamSelector::getInstance()
{
    static SamSelector instance;
    return instance;
}

void SamSelector::setModelDirectory(const Glib::ustring& directory)
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    impl->directory = directory;
    impl->haveModels = !directory.empty()
                       && Glib::file_test(Glib::build_filename(directory, "sam2_encoder.onnx"), Glib::FILE_TEST_EXISTS)
                       && Glib::file_test(Glib::build_filename(directory, "sam2_decoder.onnx"), Glib::FILE_TEST_EXISTS);
}

bool SamSelector::available() const
{
#ifdef RT_AI_MASKING
    std::lock_guard<std::mutex> lock(impl->mutex);
    return impl->haveModels;
#else
    return false;
#endif
}

namespace
{

// Upsample the chosen decoder logits onto the source grid and refine them
// against luminance: a narrow image-guided band follows detail between
// decoder pixels without adding texture to confident interiors.
array2D<float> refineSelection(const Imagefloat& src, const std::vector<float>& logits, int size)
{
    const int w = src.getWidth(), h = src.getHeight();
    array2D<float> seed(w, h), guide(w, h), refined(w, h), mask(w, h);
    const auto mat = ICCStore::getInstance()->workingSpaceMatrix("ProPhoto");
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const double px = std::max(0., std::min(double(size - 1), (x + .5) * size / w - .5));
            const double py = std::max(0., std::min(double(size - 1), (y + .5) * size / h - .5));
            const int ix = px, iy = py, x1 = std::min(size - 1, ix + 1), y1 = std::min(size - 1, iy + 1);
            const float value =
                (logits[iy * size + ix] * (1 - (px - ix)) + logits[iy * size + x1] * (px - ix)) * (1 - (py - iy)) +
                (logits[y1 * size + ix] * (1 - (px - ix)) + logits[y1 * size + x1] * (px - ix)) * (py - iy);
            seed[y][x] = composite::ease(.5f + value * .25f, 1);
            float X, Y, Z;
            Color::rgbxyz(src.r(y, x), src.g(y, x), src.b(y, x), X, Y, Z, mat);
            guide[y][x] = std::sqrt(composite::unit(Y / 65535.f));
        }
    if (std::min(w, h) >= 8) {
        const int radius = std::max(2, std::min(24, int(std::ceil(double(std::max(w, h)) / size))));
        guidedFilter(guide, seed, refined, radius, .0001f, false, 1);
    } else {
        refined = seed;
    }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            mask[y][x] = seed[y][x] == 0 || seed[y][x] == 1 ? seed[y][x]
                                                              : composite::ease((refined[y][x] - .08f) / .84f, 1);
    return mask;
}

} // namespace

array2D<float> SamSelector::select(const Imagefloat& src, const std::string& id,
                                   const std::vector<std::array<float, 3>>& prompts)
{
#ifndef RT_AI_MASKING
    (void)src; (void)id; (void)prompts;
    return array2D<float>();
#else
    if (prompts.empty() || prompts.size() > 128 || src.getWidth() < 2 || src.getHeight() < 2) return array2D<float>();
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (!impl->haveModels) return array2D<float>();
    try {
        impl->prepare();
        auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        auto cached = std::find_if(impl->embeddings.begin(), impl->embeddings.end(),
                                   [&](const Impl::Embedding& e) { return e.id == id; });
        if (cached != impl->embeddings.end()) {
            impl->embeddings.splice(impl->embeddings.begin(), impl->embeddings, cached);
        } else {
            constexpr int size = 1024;
            std::vector<float> input(3 * size * size);
            // Partners arrive in the base's working profile; the model wants sRGB.
            const auto mat = ICCStore::getInstance()->workingSpaceMatrix("ProPhoto");
            const float mean[] = {.485f, .456f, .406f}, stddev[] = {.229f, .224f, .225f};
            for (int y = 0; y < size; ++y)
                for (int x = 0; x < size; ++x) {
                    const float sx = (x + .5f) * src.getWidth() / size - .5f,
                                sy = (y + .5f) * src.getHeight() / size - .5f;
                    const int x0 = std::max(0, std::min(src.getWidth() - 1, int(std::floor(sx))));
                    const int y0 = std::max(0, std::min(src.getHeight() - 1, int(std::floor(sy))));
                    const int x1 = std::min(x0 + 1, src.getWidth() - 1), y1 = std::min(y0 + 1, src.getHeight() - 1);
                    const float fx = std::max(0.f, sx - x0), fy = std::max(0.f, sy - y0);
                    auto at = [&](int c) {
                        auto val = [&](int yy, int xx) {
                            return c == 0 ? src.r(yy, xx) : c == 1 ? src.g(yy, xx) : src.b(yy, xx);
                        };
                        return (val(y0, x0) * (1 - fx) + val(y0, x1) * fx) * (1 - fy) +
                               (val(y1, x0) * (1 - fx) + val(y1, x1) * fx) * fy;
                    };
                    float X, Y, Z, rgb[3];
                    Color::rgbxyz(at(0), at(1), at(2), X, Y, Z, mat);
                    Color::xyz2srgb(X, Y, Z, rgb[0], rgb[1], rgb[2]);
                    for (int c = 0; c < 3; ++c) {
                        float v = std::max(0.f, std::min(1.f, rgb[c] / 65535.f));
                        v = v <= .0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.f / 2.4f) - .055f;
                        input[c * size * size + y * size + x] = (v - mean[c]) / stddev[c];
                    }
                }
            std::array<int64_t, 4> shape = {{1, 3, size, size}};
            auto tensor = Ort::Value::CreateTensor<float>(memory, input.data(), input.size(), shape.data(), shape.size());
            const char* in[] = {"image"};
            const char* out[] = {"high0", "high1", "embedding"};
            auto features = impl->encoder->Run(Ort::RunOptions{nullptr}, in, &tensor, 1, out, 3);
            size_t bytes = 0;
            for (const auto& feature : features)
                bytes += feature.GetTensorTypeAndShapeInfo().GetElementCount() * sizeof(float);
            while (!impl->embeddings.empty() &&
                   (impl->embeddingBytes + bytes > 128U * 1024U * 1024U || impl->embeddings.size() >= 4)) {
                impl->embeddingBytes -= impl->embeddings.back().bytes;
                impl->embeddings.pop_back();
            }
            impl->embeddings.push_front({id, std::move(features), bytes});
            impl->embeddingBytes += bytes;
        }
        std::vector<float> xy, labels;
        for (const auto& p : prompts) {
            xy.push_back(std::max(0.f, std::min(1.f, p[0])) * 1024);
            xy.push_back(std::max(0.f, std::min(1.f, p[1])) * 1024);
            labels.push_back(p[2] > .5f ? 1.f : 0.f);
        }
        std::array<int64_t, 3> ps = {{1, int64_t(prompts.size()), 2}};
        std::array<int64_t, 2> ls = {{1, int64_t(prompts.size())}};
        auto pt = Ort::Value::CreateTensor<float>(memory, xy.data(), xy.size(), ps.data(), ps.size());
        auto lb = Ort::Value::CreateTensor<float>(memory, labels.data(), labels.size(), ls.data(), ls.size());
        const char* ins[] = {"high0", "high1", "embedding", "points", "labels"};
        const char* outs[] = {"masks", "scores"};
        std::vector<Ort::Value> inputs;
        for (auto& f : impl->embeddings.front().features) {
            auto shape = f.GetTensorTypeAndShapeInfo().GetShape();
            inputs.push_back(Ort::Value::CreateTensor<float>(memory, f.GetTensorMutableData<float>(),
                                                             f.GetTensorTypeAndShapeInfo().GetElementCount(),
                                                             shape.data(), shape.size()));
        }
        inputs.push_back(std::move(pt));
        inputs.push_back(std::move(lb));
        auto result = impl->decoder->Run(Ort::RunOptions{nullptr}, ins, inputs.data(), inputs.size(), outs, 2);
        const auto shape = result[0].GetTensorTypeAndShapeInfo().GetShape();
        if (shape != std::vector<int64_t>{1, 3, 256, 256})
            throw std::runtime_error("Unsupported SAM 2 decoder shape");
        const float* data = result[0].GetTensorData<float>();
        // The decoder offers three candidates (part, object, group); with
        // several prompts its first is the one they jointly describe, with a
        // single click the highest-scoring whole candidate reads best.
        int chosen = 0;
        if (prompts.size() == 1 && result.size() > 1) {
            const float* scores = result[1].GetTensorData<float>();
            const size_t n = result[1].GetTensorTypeAndShapeInfo().GetElementCount();
            for (size_t c = 1; c < std::min<size_t>(3, n); ++c)
                if (std::isfinite(scores[c]) && scores[c] > scores[chosen]) chosen = int(c);
        }
        std::vector<float> logits(data + chosen * 256 * 256, data + (chosen + 1) * 256 * 256);
        if (!std::all_of(logits.begin(), logits.end(), [](float v) { return std::isfinite(v); }))
            throw std::runtime_error("Invalid SAM mask logits");
        return refineSelection(src, logits, 256);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Click selection: %s\n", e.what());
        return array2D<float>();
    }
#endif
}

} // namespace rtengine
