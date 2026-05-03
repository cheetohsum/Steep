/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2004-2010 Gabor Horvath <hgabor@rawtherapee.com>
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
#include "aidenoise.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

#include <glibmm/fileutils.h>
#include <glibmm/miscutils.h>

#include "imageformat.h"
#include "settings.h"

#ifdef RT_AI_DENOISE
// onnxruntime_c_api.h on Windows defines `ORT_API_CALL` as `_stdcall` (single
// underscore — an MSVC-only spelling). MinGW's gcc only knows the canonical
// `__stdcall`. Map them so the headers parse cleanly.
#if defined(_WIN32) && defined(__GNUC__) && !defined(_stdcall)
#define _stdcall __stdcall
#endif
#include <onnxruntime_cxx_api.h>
#ifdef _WIN32
#include <dml_provider_factory.h>
#endif
#endif

namespace rtengine
{

#ifdef RT_AI_DENOISE
struct AIDenoiseManager::Impl
{
    // Reused across invocations. Re-created on CPU<->GPU mode switch.
    std::unique_ptr<Ort::Env>     env;
    std::unique_ptr<Ort::Session> session;
    bool sessionUsesGpu = false;
};
#else
struct AIDenoiseManager::Impl {};
#endif

AIDenoiseManager::AIDenoiseManager()
    : impl_(new Impl)
    , available_(false)
    , detecting_(false)
    , running_(false)
    , cancelled_(false)
    , cachedIso_(0)
{
}

AIDenoiseManager::~AIDenoiseManager() = default;

AIDenoiseManager& AIDenoiseManager::getInstance()
{
    static AIDenoiseManager instance;
    return instance;
}

namespace
{

// Where the upstream RawRefinery package stores its model on Windows,
// keeping compatibility so existing users don't redownload.
//   %LOCALAPPDATA%\RawRefinery\RawRefinery\ShadowWeightedL1.onnx
// On Linux/macOS we fall back to XDG_DATA_HOME or ~/.local/share.
Glib::ustring defaultModelDir()
{
#ifdef _WIN32
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path))) {
        // Wide path -> narrow UTF-8
        int n = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
        std::string s(n > 0 ? n : 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, path, -1, &s[0], n, nullptr, nullptr);
        if (!s.empty() && s.back() == '\0') s.pop_back();
        CoTaskMemFree(path);
        return Glib::build_filename(s, "RawRefinery", "RawRefinery");
    }
    return Glib::ustring();
#else
    const char* xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg) {
        return Glib::build_filename(xdg, "RawRefinery");
    }
    const char* home = getenv("HOME");
    if (home && *home) {
        return Glib::build_filename(home, ".local", "share", "RawRefinery");
    }
    return Glib::ustring();
#endif
}

// Convert a UTF-8 path to the form ONNX Runtime expects on this platform.
// On Windows the C++ API takes wchar_t*, elsewhere const char*.
#ifdef _WIN32
std::wstring toOrtPath(const Glib::ustring& utf8)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n : 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &w[0], n);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}
#else
std::string toOrtPath(const Glib::ustring& utf8) { return utf8.raw(); }
#endif

} // namespace

Glib::ustring AIDenoiseManager::findModelPath() const
{
    Glib::ustring dir = defaultModelDir();
    if (dir.empty()) return Glib::ustring();

    // Preferred filenames in priority order. ShadowWeightedL1 is the
    // "Tree Net Denoise" model from RawRefinery v1.2.1-alpha.
    static const char* kCandidates[] = {
        "ShadowWeightedL1.onnx",
        "TreeNetDenoise.onnx",
    };
    for (const char* name : kCandidates) {
        Glib::ustring path = Glib::build_filename(dir, name);
        if (Glib::file_test(path, Glib::FILE_TEST_EXISTS)) {
            return path;
        }
    }
    return Glib::ustring();
}

void AIDenoiseManager::detect()
{
    available_ = false;
    detecting_ = true;

#ifndef RT_AI_DENOISE
    // ONNX Runtime not compiled in — denoise is permanently unavailable.
    detecting_ = false;
    if (detectDoneCb_) detectDoneCb_(false);
    return;
#else
    std::thread([this]() {
        Glib::ustring modelPath = findModelPath();
        if (modelPath.empty()) {
            fprintf(stderr, "AI Denoise: model file not found in %s. "
                    "Place ShadowWeightedL1.onnx there to enable.\n",
                    defaultModelDir().c_str());
            detecting_ = false;
            if (detectDoneCb_) detectDoneCb_(false);
            return;
        }

        // Try to instantiate a CPU session as a smoke test. The actual
        // session used for inference is created lazily in startDenoising
        // (per-invocation GPU/CPU mode).
        try {
            Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "aidenoise-detect");
            Ort::SessionOptions opts;
            opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_BASIC);
            auto p = toOrtPath(modelPath);
            Ort::Session session(env, p.c_str(), opts);
            available_ = true;
            fprintf(stderr, "AI Denoise: ONNX model loaded OK from %s\n",
                    modelPath.c_str());
        } catch (const Ort::Exception& e) {
            fprintf(stderr, "AI Denoise: ONNX session creation failed: %s\n",
                    e.what());
            available_ = false;
        }

        detecting_ = false;
        if (detectDoneCb_) detectDoneCb_(available_.load());
    }).detach();
#endif
}

#ifdef RT_AI_DENOISE
namespace
{

constexpr int kTileSize = 256;
constexpr int kStride   = 64;   // 75% overlap, matches rawrefinery_cli v12

// Compute tile origin positions along an axis: 0, stride, 2*stride, ...,
// then add a final position so the last tile covers the right/bottom edge.
std::vector<int> tilePositions(int length)
{
    std::vector<int> out;
    if (length <= kTileSize) { out.push_back(0); return out; }
    for (int p = 0; p + kTileSize <= length; p += kStride) {
        out.push_back(p);
    }
    if (out.empty() || out.back() + kTileSize < length) {
        out.push_back(length - kTileSize);
    }
    // De-duplicate (in case the last position equals the previous one)
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// 1D weight along an axis for tile `idx` at origin `tileStart`.
// Weight is 1.0 in the owned interior, sin²/cos² taper at boundaries with
// neighbouring tiles. Half-window = stride/2 (mirrors rawrefinery_cli).
std::vector<float> tileWeight1D(int tileStart, const std::vector<int>& positions, int idx)
{
    const int blendHalf = kStride / 2;
    std::vector<int> centers; centers.reserve(positions.size());
    for (int p : positions) centers.push_back(p + kTileSize / 2);

    std::vector<float> w(kTileSize, 1.0f);
    for (int i = 0; i < kTileSize; ++i) {
        const float y = static_cast<float>(i + tileStart);
        if (idx > 0) {
            const float bnd = 0.5f * (centers[idx - 1] + centers[idx]);
            if (y < bnd - blendHalf) {
                w[i] = 0.0f;
            } else if (y <= bnd + blendHalf) {
                float t = (y - bnd + blendHalf) / (2.0f * blendHalf);
                t = std::min(1.0f, std::max(0.0f, t));
                w[i] *= std::sin(static_cast<float>(M_PI) * 0.5f * t) *
                        std::sin(static_cast<float>(M_PI) * 0.5f * t);
            }
        }
        if (idx + 1 < static_cast<int>(positions.size())) {
            const float bnd = 0.5f * (centers[idx] + centers[idx + 1]);
            if (y > bnd + blendHalf) {
                w[i] = 0.0f;
            } else if (y >= bnd - blendHalf) {
                float t = (y - bnd + blendHalf) / (2.0f * blendHalf);
                t = std::min(1.0f, std::max(0.0f, t));
                w[i] *= std::cos(static_cast<float>(M_PI) * 0.5f * t) *
                        std::cos(static_cast<float>(M_PI) * 0.5f * t);
            }
        }
    }
    return w;
}

// Read an Imagefloat (R/G/B planar) into a contiguous CHW float32 buffer.
// Imagefloat stores values in [0, 65535] (RT internal scale). The model
// expects [0, 1], so we normalise here. Returns the max in [0, 1] space
// for highlight-preservation logic.
constexpr float kRTScale = 65535.0f;
constexpr float kRTScaleInv = 1.0f / kRTScale;

std::vector<float> imageToCHW(const Imagefloat& img, float* outMax)
{
    const int H = img.getHeight();
    const int W = img.getWidth();
    std::vector<float> chw(static_cast<size_t>(3) * H * W);
    float mx = 0.0f;
    float* rOut = chw.data();
    float* gOut = chw.data() + static_cast<size_t>(H) * W;
    float* bOut = chw.data() + static_cast<size_t>(2) * H * W;
    for (int y = 0; y < H; ++y) {
        const float* rIn = img.r(y);
        const float* gIn = img.g(y);
        const float* bIn = img.b(y);
        for (int x = 0; x < W; ++x) {
            float r = rIn[x] * kRTScaleInv;
            float g = gIn[x] * kRTScaleInv;
            float b = bIn[x] * kRTScaleInv;
            if (r > mx) mx = r;
            if (g > mx) mx = g;
            if (b > mx) mx = b;
            rOut[static_cast<size_t>(y) * W + x] = r;
            gOut[static_cast<size_t>(y) * W + x] = g;
            bOut[static_cast<size_t>(y) * W + x] = b;
        }
    }
    if (outMax) *outMax = mx;
    return chw;
}

// Write CHW float32 buffer (in [0, 1] model space) back into an Imagefloat
// (R/G/B planar, RT [0, 65535] scale).
void chwToImage(const float* chw, int H, int W, Imagefloat& out)
{
    const float* rIn = chw;
    const float* gIn = chw + static_cast<size_t>(H) * W;
    const float* bIn = chw + static_cast<size_t>(2) * H * W;
    for (int y = 0; y < H; ++y) {
        float* rOut = out.r(y);
        float* gOut = out.g(y);
        float* bOut = out.b(y);
        for (int x = 0; x < W; ++x) {
            rOut[x] = rIn[static_cast<size_t>(y) * W + x] * kRTScale;
            gOut[x] = gIn[static_cast<size_t>(y) * W + x] * kRTScale;
            bOut[x] = bIn[static_cast<size_t>(y) * W + x] * kRTScale;
        }
    }
}

// Pad an image to ensure both dimensions are at least kTileSize. Returns
// padded buffer (CHW), and writes the padded size into outH/outW. If no
// padding is needed, returns an empty vector (caller uses the original).
// We replicate the edge for padding (matches Python `np.pad(...,'edge')`
// behaviour, which is what TilingModule does internally).
std::vector<float> padToMinTile(const std::vector<float>& src, int H, int W, int& outH, int& outW)
{
    outH = std::max(H, kTileSize);
    outW = std::max(W, kTileSize);
    if (outH == H && outW == W) return {};
    std::vector<float> dst(static_cast<size_t>(3) * outH * outW);
    for (int c = 0; c < 3; ++c) {
        const float* srcC = src.data() + static_cast<size_t>(c) * H * W;
        float* dstC = dst.data() + static_cast<size_t>(c) * outH * outW;
        for (int y = 0; y < outH; ++y) {
            const int sy = std::min(y, H - 1);
            const float* srcRow = srcC + static_cast<size_t>(sy) * W;
            float* dstRow = dstC + static_cast<size_t>(y) * outW;
            for (int x = 0; x < outW; ++x) {
                const int sx = std::min(x, W - 1);
                dstRow[x] = srcRow[sx];
            }
        }
    }
    return dst;
}

} // namespace
#endif // RT_AI_DENOISE

void AIDenoiseManager::startDenoising(
    const Glib::ustring& rawPath,
    const procparams::AIDenoiseParams& params,
    const Glib::ustring& outputPath,
    std::function<void(double)> progressCb,
    std::function<void(bool, const Glib::ustring&)> doneCb,
    const Glib::ustring& inputTiffPath,
    int /*iso*/)
{
#ifndef RT_AI_DENOISE
    if (doneCb) doneCb(false, "AI Denoise not compiled in (ONNX Runtime missing).");
    return;
#else
    if (!available_ || running_) {
        if (doneCb) {
            doneCb(false, "AI Denoise not available or already running");
        }
        return;
    }

    if (inputTiffPath.empty()) {
        if (doneCb) doneCb(false, "Native AI Denoise requires a pre-demosaiced TIFF input.");
        return;
    }

    running_ = true;
    cancelled_ = false;

    std::thread([this, rawPath, params, outputPath, progressCb, doneCb, inputTiffPath]() {
        auto fail = [&](const Glib::ustring& msg) {
            running_ = false;
            if (doneCb) doneCb(false, msg);
        };
        auto progress = [&](double p) {
            if (progressCb) progressCb(p);
        };

        try {
            // ---- 1. Load input TIFF ----------------------------------
            progress(0);
            Imagefloat inputImg;
            inputImg.setSampleFormat(IIOSF_FLOAT32);
            int loadErr = inputImg.loadTIFF(inputTiffPath);
            if (loadErr != 0) return fail("Failed to load input TIFF");

            const int H0 = inputImg.getHeight();
            const int W0 = inputImg.getWidth();
            float originalMax = 0.0f;
            std::vector<float> rgb = imageToCHW(inputImg, &originalMax);
            std::vector<float> originalRgb = rgb;  // for highlight blend
            const bool hasHighlights = originalMax > 1.0f;

            // Pad if image is smaller than a single tile
            int H = H0, W = W0;
            std::vector<float> padded = padToMinTile(rgb, H0, W0, H, W);
            const float* rgbPtr = padded.empty() ? rgb.data() : padded.data();

            // Clip to [0, 1] for inference (model trained on this range)
            std::vector<float> clipped(static_cast<size_t>(3) * H * W);
            for (size_t i = 0; i < clipped.size(); ++i) {
                clipped[i] = std::min(1.0f, std::max(0.0f, rgbPtr[i]));
            }
            progress(5);

            // ---- 2. (Re)create ONNX session if mode changed ----------
            const bool wantGpu = params.useGpu;
            if (!impl_->session || impl_->sessionUsesGpu != wantGpu || !impl_->env) {
                impl_->session.reset();
                impl_->env.reset(new Ort::Env(ORT_LOGGING_LEVEL_WARNING, "aidenoise"));
                Ort::SessionOptions opts;
                opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
                opts.DisableMemPattern();  // required for DML
#ifdef _WIN32
                if (wantGpu) {
                    try {
                        opts.DisableCpuMemArena();
                        OrtSessionOptionsAppendExecutionProvider_DML(opts, 0);
                        fprintf(stderr, "AI Denoise: using DirectML execution provider\n");
                    } catch (const Ort::Exception& e) {
                        fprintf(stderr, "AI Denoise: DirectML unavailable (%s), falling back to CPU\n", e.what());
                    }
                }
#endif
                Glib::ustring modelPath = findModelPath();
                if (modelPath.empty()) return fail("ONNX model file not found");
                auto p = toOrtPath(modelPath);
                impl_->session.reset(new Ort::Session(*impl_->env, p.c_str(), opts));
                impl_->sessionUsesGpu = wantGpu;
            }
            progress(10);

            // ---- 3. Plan tiles ---------------------------------------
            std::vector<int> yPos = tilePositions(H);
            std::vector<int> xPos = tilePositions(W);
            const int nTiles = static_cast<int>(yPos.size() * xPos.size());
            fprintf(stderr, "AI Denoise: tiles %zux%zu = %d, image %dx%d\n",
                    yPos.size(), xPos.size(), nTiles, W, H);

            // Pre-compute 1D weights (independent of tile content)
            std::vector<std::vector<float>> wY(yPos.size());
            std::vector<std::vector<float>> wX(xPos.size());
            for (size_t i = 0; i < yPos.size(); ++i) wY[i] = tileWeight1D(yPos[i], yPos, static_cast<int>(i));
            for (size_t i = 0; i < xPos.size(); ++i) wX[i] = tileWeight1D(xPos[i], xPos, static_cast<int>(i));

            // ---- 4. Run inference per tile, store outputs ------------
            // tilesOut layout: [tileIdx][c=3][kTileSize][kTileSize]
            std::vector<std::vector<float>> tilesOut(nTiles);

            // ISO conditioning (matches Python: clamp, normalise by 6400)
            const float isoCond = static_cast<float>(params.isoConditioning) / 6400.0f;

            auto memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            const char* inNames[]  = { "rgb", "cond" };
            const char* outNames[] = { "denoised" };

            // Tile-batched inference. Larger batches amortise GPU dispatch
            // overhead and tile-extraction memcpy cost. 4 is a good balance
            // for 4 GB+ GPUs at 256×256 tiles (~3 MB per tile).
            constexpr int kBatchSize = 4;
            const size_t kTilePixels = static_cast<size_t>(3) * kTileSize * kTileSize;
            std::vector<float> batchBuf(kBatchSize * kTilePixels);
            std::vector<float> condBuf(kBatchSize, isoCond);
            int64_t rgbShape[]  = { kBatchSize, 3, kTileSize, kTileSize };
            int64_t condShape[] = { kBatchSize, 1 };

            for (int idx = 0; idx < nTiles; idx += kBatchSize) {
                if (cancelled_) {
                    running_ = false;
                    if (doneCb) doneCb(false, "Cancelled");
                    return;
                }
                const int batch = std::min(kBatchSize, nTiles - idx);
                rgbShape[0] = batch;
                condShape[0] = batch;

                // Pack `batch` tiles into batchBuf
                for (int b = 0; b < batch; ++b) {
                    const int tileIdx = idx + b;
                    const int yi = tileIdx / static_cast<int>(xPos.size());
                    const int xi = tileIdx % static_cast<int>(xPos.size());
                    const int y = yPos[yi];
                    const int x = xPos[xi];
                    float* dst = batchBuf.data() + b * kTilePixels;
                    for (int c = 0; c < 3; ++c) {
                        const float* srcC = clipped.data() + static_cast<size_t>(c) * H * W;
                        float* dstC = dst + static_cast<size_t>(c) * kTileSize * kTileSize;
                        for (int ty = 0; ty < kTileSize; ++ty) {
                            const float* srcRow = srcC + static_cast<size_t>(y + ty) * W + x;
                            float* dstRow = dstC + static_cast<size_t>(ty) * kTileSize;
                            std::memcpy(dstRow, srcRow, kTileSize * sizeof(float));
                        }
                    }
                }

                Ort::Value rgbT = Ort::Value::CreateTensor<float>(memInfo, batchBuf.data(),
                    static_cast<size_t>(batch) * kTilePixels, rgbShape, 4);
                Ort::Value condT = Ort::Value::CreateTensor<float>(memInfo, condBuf.data(),
                    batch, condShape, 2);
                Ort::Value inputs[] = { std::move(rgbT), std::move(condT) };
                auto outputs = impl_->session->Run(Ort::RunOptions{nullptr},
                    inNames, inputs, 2, outNames, 1);

                const float* outData = outputs.front().GetTensorData<float>();
                for (int b = 0; b < batch; ++b) {
                    const int tileIdx = idx + b;
                    tilesOut[tileIdx].assign(outData + b * kTilePixels,
                                              outData + (b + 1) * kTilePixels);
                }

                progress(10.0 + 80.0 * std::min(nTiles, idx + batch) / nTiles);
            }

            // ---- 5. Two-pass cosine blend with bias correction -------
            //
            // Stitching passes don't write the same output pixel from
            // different tiles in parallel (we serialise outer y/x), but
            // within one tile-row the OpenMP loop is parallelisable across
            // independent tile rows that touch disjoint output Y bands when
            // we add atomic accumulation. To avoid the atomic cost on hot
            // path we instead parallelise the per-tile inner work which is
            // a clean speedup at no correctness cost.
            const size_t pixCount = static_cast<size_t>(H) * W;
            std::vector<float> numerator(3 * pixCount, 0.0f);
            std::vector<float> denom(pixCount, 0.0f);

            auto accumulateTiles = [&](bool subtractBias, const float* biasMap) {
                int idx = 0;
                for (size_t yi = 0; yi < yPos.size(); ++yi) {
                    for (size_t xi = 0; xi < xPos.size(); ++xi, ++idx) {
                        const int y = yPos[yi], x = xPos[xi];
                        const float* tile = tilesOut[idx].data();
                        const std::vector<float>& wYi = wY[yi];
                        const std::vector<float>& wXi = wX[xi];
                        const bool firstPass = !subtractBias;
                        // Parallelise across tile rows. Different ty values
                        // in one tile write disjoint output rows, so no race.
#ifdef _OPENMP
                        #pragma omp parallel for schedule(static) if (kTileSize >= 64)
#endif
                        for (int ty = 0; ty < kTileSize; ++ty) {
                            const float wy = wYi[ty];
                            const size_t outRow = static_cast<size_t>(y + ty) * W;
                            for (int tx = 0; tx < kTileSize; ++tx) {
                                const float w2 = wy * wXi[tx];
                                const size_t outIdx = outRow + (x + tx);
                                if (firstPass) denom[outIdx] += w2;
                                for (int c = 0; c < 3; ++c) {
                                    const size_t tIdx = static_cast<size_t>(c) * kTileSize * kTileSize +
                                                        static_cast<size_t>(ty) * kTileSize + tx;
                                    float v = tile[tIdx];
                                    if (subtractBias) v -= biasMap[tIdx];
                                    numerator[c * pixCount + outIdx] += v * w2;
                                }
                            }
                        }
                    }
                }
            };

            // Pass 1: compute reference
            accumulateTiles(false, nullptr);

            std::vector<float> reference(3 * pixCount);
#ifdef _OPENMP
            #pragma omp parallel for schedule(static)
#endif
            for (long long i = 0; i < static_cast<long long>(pixCount); ++i) {
                const float d = std::max(denom[i], 1e-6f);
                for (int c = 0; c < 3; ++c) {
                    reference[c * pixCount + i] = numerator[c * pixCount + i] / d;
                }
            }

            // Estimate position-dependent bias: avg(tile - reference_crop)
            std::vector<double> biasSum(static_cast<size_t>(3) * kTileSize * kTileSize, 0.0);
            int idx = 0;
            for (size_t yi = 0; yi < yPos.size(); ++yi) {
                for (size_t xi = 0; xi < xPos.size(); ++xi, ++idx) {
                    const int y = yPos[yi], x = xPos[xi];
                    const float* tile = tilesOut[idx].data();
                    for (int c = 0; c < 3; ++c) {
                        const float* refC = reference.data() + c * pixCount;
                        const float* tileC = tile + static_cast<size_t>(c) * kTileSize * kTileSize;
                        double* biasC = biasSum.data() + static_cast<size_t>(c) * kTileSize * kTileSize;
#ifdef _OPENMP
                        #pragma omp parallel for schedule(static)
#endif
                        for (int ty = 0; ty < kTileSize; ++ty) {
                            const float* refRow = refC + static_cast<size_t>(y + ty) * W + x;
                            const float* tileRow = tileC + static_cast<size_t>(ty) * kTileSize;
                            double* biasRow = biasC + static_cast<size_t>(ty) * kTileSize;
                            for (int tx = 0; tx < kTileSize; ++tx) {
                                biasRow[tx] += static_cast<double>(tileRow[tx]) - static_cast<double>(refRow[tx]);
                            }
                        }
                    }
                }
            }
            std::vector<float> biasMap(biasSum.size());
            for (size_t i = 0; i < biasSum.size(); ++i) {
                biasMap[i] = static_cast<float>(biasSum[i] / nTiles);
            }

            // Pass 2: blend bias-corrected tiles
            std::fill(numerator.begin(), numerator.end(), 0.0f);
            accumulateTiles(true, biasMap.data());

            std::vector<float> stitched(3 * pixCount);
#ifdef _OPENMP
            #pragma omp parallel for schedule(static)
#endif
            for (long long i = 0; i < static_cast<long long>(pixCount); ++i) {
                const float d = std::max(denom[i], 1e-6f);
                for (int c = 0; c < 3; ++c) {
                    stitched[c * pixCount + i] = numerator[c * pixCount + i] / d;
                }
            }
            progress(92);

            // ---- 6. Crop padding back to original size, highlight blend ----
            std::vector<float> finalRgb(static_cast<size_t>(3) * H0 * W0);
            for (int c = 0; c < 3; ++c) {
                const float* sIn  = stitched.data()    + c * pixCount;
                const float* origIn = originalRgb.data() + static_cast<size_t>(c) * H0 * W0;
                float* sOut = finalRgb.data() + static_cast<size_t>(c) * H0 * W0;
                for (int y = 0; y < H0; ++y) {
                    const float* sRow = sIn + static_cast<size_t>(y) * W;
                    const float* oRow = origIn + static_cast<size_t>(y) * W0;
                    float* oOut = sOut + static_cast<size_t>(y) * W0;
                    for (int x = 0; x < W0; ++x) {
                        float v = std::min(1.0f, std::max(0.0f, sRow[x]));
                        if (hasHighlights) {
                            const float orig = oRow[x];
                            const float t = std::min(1.0f, std::max(0.0f, (orig - 1.0f) / 0.05f));
                            v = (1.0f - t) * v + t * orig;
                        }
                        oOut[x] = v;
                    }
                }
            }
            progress(95);

            // ---- 7. Write output TIFF + cache result ----------------
            std::unique_ptr<Imagefloat> result(new Imagefloat(W0, H0));
            chwToImage(finalRgb.data(), H0, W0, *result);
            result->setSampleFormat(IIOSF_FLOAT32);
            int saveErr = result->saveTIFF(outputPath, 32, true);
            if (saveErr != 0) return fail("Failed to save denoised TIFF");

            // Cache for the GUI side
            setCachedResult(std::move(result), rawPath, params.isoConditioning);

            progress(100);
            running_ = false;
            if (doneCb) doneCb(true, "");
        } catch (const Ort::Exception& e) {
            running_ = false;
            if (doneCb) doneCb(false, Glib::ustring("ONNX Runtime: ") + e.what());
        } catch (const std::exception& e) {
            running_ = false;
            if (doneCb) doneCb(false, e.what());
        }
    }).detach();
#endif // RT_AI_DENOISE
}

void AIDenoiseManager::cancel()
{
    cancelled_ = true;
}

bool AIDenoiseManager::isCacheValid(const Glib::ustring& rawPath, double iso) const
{
    MyMutex::MyLock lock(cacheMutex_);
    return cachedResult_ != nullptr && cachedRawPath_ == rawPath && cachedIso_ == iso;
}

Imagefloat* AIDenoiseManager::getCachedResult() const
{
    MyMutex::MyLock lock(cacheMutex_);
    return cachedResult_.get();
}

void AIDenoiseManager::setCachedResult(std::unique_ptr<Imagefloat> result,
                                       const Glib::ustring& rawPath, double iso)
{
    MyMutex::MyLock lock(cacheMutex_);
    cachedResult_ = std::move(result);
    cachedRawPath_ = rawPath;
    cachedIso_ = iso;
}

void AIDenoiseManager::clearCache()
{
    MyMutex::MyLock lock(cacheMutex_);
    cachedResult_.reset();
    cachedRawPath_.clear();
    cachedIso_ = 0;
}

} // namespace rtengine
