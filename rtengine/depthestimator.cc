#include "depthestimator.h"
#include "imagefloat.h"
#include "iccstore.h"
#include "color.h"
#include "guidedfilter.h"
#include "array2D.h"
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
        // No busy-waiting between runs (see aisegmentation.cc).
        options.AddConfigEntry("session.intra_op.allow_spinning", "0");
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    }
#endif
};
#ifdef RT_AI_MASKING
namespace
{

// The model's raw output (larger = nearer) over the source region
// [x0, x0 + cw) x [y0, y0 + ch), letterboxed into its square input.
std::vector<float> runRegion(Ort::Session &model, const Imagefloat &src, const Glib::ustring &workingProfile,
                             int x0, int y0, int cw, int ch, int &w, int &h)
{
    constexpr int size = 518;
    const double ratio = double(size) / std::max(cw, ch);
    w = std::max(1, int(cw * ratio));
    h = std::max(1, int(ch * ratio));
    const int dx = (size - w) / 2, dy = (size - h) / 2;
    std::vector<float> input(size * size * 3, 0.f);
    const auto matrix = ICCStore::getInstance()->workingSpaceMatrix(workingProfile);
    const float mean[] = {.485f, .456f, .406f}, stddev[] = {.229f, .224f, .225f};
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const int sx = x0 + std::max(0, std::min(cw - 1, int((x - dx + .5) * cw / w)));
            const int sy = y0 + std::max(0, std::min(ch - 1, int((y - dy + .5) * ch / h)));
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
    auto tensor = Ort::Value::CreateTensor<float>(memory, input.data(), input.size(), shape.data(), shape.size());
    const char *in[] = {"image"};
    const char *out[] = {"depth"};
    auto result = model.Run(Ort::RunOptions{nullptr}, in, &tensor, 1, out, 1);
    if (result[0].GetTensorTypeAndShapeInfo().GetShape() != std::vector<int64_t>{1, size, size})
        throw std::runtime_error("Unsupported depth model output");
    const float *data = result[0].GetTensorData<float>();
    std::vector<float> raw(w * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            raw[y * w + x] = data[(y + dy) * size + x + dx];
    return raw;
}

float sampleGrid(const std::vector<float> &g, int w, int h, float x, float y)
{
    x = std::max(0.f, std::min(float(w - 1), x));
    y = std::max(0.f, std::min(float(h - 1), y));
    const int x0 = int(x), y0 = int(y), x1 = std::min(w - 1, x0 + 1), y1 = std::min(h - 1, y0 + 1);
    const float fx = x - x0, fy = y - y0;
    return (g[y0 * w + x0] * (1 - fx) + g[y0 * w + x1] * fx) * (1 - fy) + (g[y1 * w + x0] * (1 - fx) + g[y1 * w + x1] * fx) * fy;
}

} // namespace
#endif

DepthEstimator::DepthEstimator() = default;
DepthEstimator::~DepthEstimator() = default;
std::unique_ptr<Imagefloat> DepthEstimator::depth(const Imagefloat &src, const std::string &dir,
                                                const Glib::ustring &workingProfile, float *farRatio)
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
    if (farRatio)
        *farRatio = high > 0.f ? std::max(0.f, std::min(1.f, low / high)) : 0.f;
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
std::unique_ptr<Imagefloat> DepthEstimator::detailedDepth(const Imagefloat &src, const std::string &dir,
                                                        const Glib::ustring &workingProfile, int longEdge,
                                                        float *farRatio)
{
#ifndef RT_AI_MASKING
    throw std::runtime_error("This build has no ONNX Runtime support");
#else
    // the whole frame first: it holds the layout every tile is fitted onto
    auto base = depth(src, dir, workingProfile, farRatio);
    const int bw = base->getWidth(), bh = base->getHeight();
    const int sw = src.getWidth(), sh = src.getHeight();
    const double s = std::min(1.0, double(longEdge) / std::max(sw, sh));
    const int W = std::max(bw, int(std::lround(sw * s))), H = std::max(bh, int(std::lround(sh * s)));
    if (W <= bw + 32) return base;    // a small source has no more to give

    std::vector<float> B(bw * bh);
    for (int y = 0; y < bh; ++y)
        for (int x = 0; x < bw; ++x) {
            const float d = base->r(y, x);
            B[y * bw + x] = d >= 0.f ? d / 65535.f : 0.5f;
        }

    // tiles about twice the model's resolution over the frame, overlapping by a third
    const int nx = sw >= sh ? 3 : 2, ny = sw >= sh ? 2 : 3;
    const double tw = sw / (nx - (nx - 1) / 3.0), th = sh / (ny - (ny - 1) / 3.0);
    std::vector<double> acc(W * H, 0.0), wsum(W * H, 0.0);

    for (int ty = 0; ty < ny; ++ty)
        for (int tx = 0; tx < nx; ++tx) {
            const int x0 = int(std::lround(tx * tw * 2.0 / 3.0)), y0 = int(std::lround(ty * th * 2.0 / 3.0));
            const int cw = std::min(sw - x0, int(std::lround(tw))), ch = std::min(sh - y0, int(std::lround(th)));
            if (cw < 16 || ch < 16) continue;
            int rw = 0, rh = 0;
            const auto raw = runRegion(*impl->depthModel, src, workingProfile, x0, y0, cw, ch, rw, rh);
            // fit B = a * raw + b over the tile
            double sx = 0, sy = 0, sxx = 0, sxy = 0, n = 0;
            for (int y = 0; y < rh; ++y)
                for (int x = 0; x < rw; ++x) {
                    const float r = raw[y * rw + x];
                    if (!std::isfinite(r)) continue;
                    const float fx = (x0 + (x + .5f) * cw / rw) / sw * bw - .5f;
                    const float fy = (y0 + (y + .5f) * ch / rh) / sh * bh - .5f;
                    const double b = sampleGrid(B, bw, bh, fx, fy);
                    sx += r; sy += b; sxx += double(r) * r; sxy += double(r) * b; n += 1;
                }
            const double var = sxx - sx * sx / std::max(1.0, n);
            if (n < 16 || var <= 1e-12) continue;
            const double a = (sxy - sx * sy / n) / var, c = (sy - a * sx) / n;
            if (!(a < 0.0)) continue;    // the model's nearer is larger: a must flip it
            // lay it into the output with a feather toward the tile's inner edges
            const int ox0 = int(double(x0) / sw * W), oy0 = int(double(y0) / sh * H);
            const int ox1 = std::min(W, int(std::ceil(double(x0 + cw) / sw * W))), oy1 = std::min(H, int(std::ceil(double(y0 + ch) / sh * H)));
            const double fw = std::max(1.0, (ox1 - ox0) / 6.0), fh = std::max(1.0, (oy1 - oy0) / 6.0);
            for (int y = oy0; y < oy1; ++y)
                for (int x = ox0; x < ox1; ++x) {
                    const float rx = ((x + .5f) * sw / W - x0) / cw * rw - .5f;
                    const float ry = ((y + .5f) * sh / H - y0) / ch * rh - .5f;
                    const float r = sampleGrid(raw, rw, rh, rx, ry);
                    if (!std::isfinite(r)) continue;
                    const double ex = std::min(tx == 0 ? fw : x - ox0 + .5, tx == nx - 1 ? fw : ox1 - x - .5);
                    const double ey = std::min(ty == 0 ? fh : y - oy0 + .5, ty == ny - 1 ? fh : oy1 - y - .5);
                    const double wgt = std::max(1e-3, std::min(1.0, ex / fw)) * std::max(1e-3, std::min(1.0, ey / fh));
                    acc[y * W + x] += wgt * (a * r + c);
                    wsum[y * W + x] += wgt;
                }
        }

    // The tiles add detail only: their fine structure on top of the whole
    // frame's layout. A tile's fit is good but not exact, and its own
    // large-scale shape would shift whole walls against the next tile's.
    array2D<float> tiles(W, H), coarse(W, H), joined(W, H), guide(W, H), snapped(W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float b = sampleGrid(B, bw, bh, (x + .5f) * bw / W - .5f, (y + .5f) * bh / H - .5f);
            tiles[y][x] = wsum[y * W + x] > 0 ? float(acc[y * W + x] / wsum[y * W + x]) : b;
        }
    coarse = tiles;
    {
        // box blur over a sixtieth of the frame: what counts as layout
        const int r = std::max(2, W / 60);
        std::vector<float> row(std::max(W, H));
        for (int pass = 0; pass < 2; ++pass) {
            for (int y = 0; y < H; ++y) {
                double sum = 0; int n = 0;
                for (int x = -r; x <= r; ++x) if (x >= 0 && x < W) { sum += coarse[y][x]; ++n; }
                for (int x = 0; x < W; ++x) {
                    row[x] = float(sum / n);
                    if (x - r >= 0) { sum -= coarse[y][x - r]; --n; }
                    if (x + r + 1 < W) { sum += coarse[y][x + r + 1]; ++n; }
                }
                for (int x = 0; x < W; ++x) coarse[y][x] = row[x];
            }
            for (int x = 0; x < W; ++x) {
                double sum = 0; int n = 0;
                for (int y = -r; y <= r; ++y) if (y >= 0 && y < H) { sum += coarse[y][x]; ++n; }
                for (int y = 0; y < H; ++y) {
                    row[y] = float(sum / n);
                    if (y - r >= 0) { sum -= coarse[y - r][x]; --n; }
                    if (y + r + 1 < H) { sum += coarse[y + r + 1][x]; ++n; }
                }
                for (int y = 0; y < H; ++y) coarse[y][x] = row[y];
            }
        }
    }
    const auto matrix = ICCStore::getInstance()->workingSpaceMatrix(workingProfile);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float b = sampleGrid(B, bw, bh, (x + .5f) * bw / W - .5f, (y + .5f) * bh / H - .5f);
            joined[y][x] = b + (tiles[y][x] - coarse[y][x]);
            const int sx = std::min(sw - 1, int((x + .5) * sw / W)), sy = std::min(sh - 1, int((y + .5) * sh / H));
            float X, Y, Z;
            Color::rgbxyz(src.r(sy, sx), src.g(sy, sx), src.b(sy, sx), X, Y, Z, matrix);
            guide[y][x] = std::sqrt(std::max(0.f, std::min(1.f, Y / 65535.f)));
        }
    // snapped to the picture's own edges within a few pixels
    guidedFilter(guide, joined, snapped, std::max(2, W / 400), 1e-4f, false, 1);

    auto map = std::unique_ptr<Imagefloat>(new Imagefloat(W, H));
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float v = 65535.f * std::max(0.f, std::min(1.f, snapped[y][x]));
            map->r(y, x) = map->g(y, x) = map->b(y, x) = v;
        }
    return map;
#endif
}

} // namespace rtengine
