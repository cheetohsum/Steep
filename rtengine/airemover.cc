/*
 *  This file is part of RawTherapee.
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
#include "airemover.h"
#include "aiidlereaper.h"
#include "edittrace.h"
#include "memguard.h"
#include "repairmath.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <random>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>
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

namespace
{
const char* const kUnet = "rorem_unet.onnx";
const char* const kUnetData = "rorem_unet.onnx.data";
const char* const kEncoder = "rorem_vae_encoder.onnx";
const char* const kDecoder = "rorem_vae_decoder.onnx";

// The sampler the model was trained and published with: Euler over the
// scaled-linear SDXL noise schedule, "leading" step spacing with offset 1.
constexpr int kTrainSteps = 1000;
constexpr double kBetaStart = 0.00085, kBetaEnd = 0.012;
constexpr int kSteps = 20;
// The authors run with strength 0.99: the first step is skipped and the
// start is the picture's own latent under that step's noise.
constexpr double kStrength = 0.99;
}

namespace
{
void memoryTrace(const char* stage)
{
    if (!edittrace::enabled()) return;
    edittrace::logf("[aiRemover] %s: process %.1f GB, system can still commit %.1f GB", stage,
                    MemGuard::processPrivateBytes() / double(1ULL << 30), MemGuard::commitHeadroomBytes() / double(1ULL << 30));
}
}

struct AIRemoverEngine::Impl {
    mutable std::mutex mutex;
    Glib::ustring directory;
    bool haveModels = false;
#ifdef RT_AI_MASKING
    std::unique_ptr<Ort::Env> env;
    // The denoiser (2.6 billion weights, run twenty times) needs the GPU.
    // The image codec does not: it runs three times per fill, and on
    // DirectML its full-resolution float32 activations ask for gigabytes at
    // once -- the first thing to fail when other programs hold GPU memory.
    std::unique_ptr<Ort::SessionOptions> options, cpuOptions;
    std::unique_ptr<Ort::Session> unet, encoder, decoder;
    bool directml = false;
    std::atomic<long long> lastUse{0};
    bool watched = false;
    int reaperId = -1;

    void releaseIfIdle(bool force)
    {
        std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
        if (!lock.owns_lock() || !unet || (!force && AIIdleReaper::nowMs() - lastUse.load() < AIIdleReaper::kIdleMs)) {
            return;
        }
        unet.reset();
        std::fprintf(stderr, "AI Remover: model unloaded after %lld s idle\n", AIIdleReaper::kIdleMs / 1000);
    }

    std::unique_ptr<Ort::Session> session(const char* file, bool onCpu = false)
    {
        const Ort::SessionOptions& options = onCpu ? *cpuOptions : *this->options;
        const auto path = Glib::build_filename(directory, file);
#ifdef _WIN32
        const auto utf16 = Glib::convert(path, "UTF-16LE", "UTF-8");
        std::wstring wide(utf16.size() / 2, L'\0');
        std::memcpy(&wide[0], utf16.data(), utf16.size());
        return std::unique_ptr<Ort::Session>(new Ort::Session(*env, wide.c_str(), options));
#else
        return std::unique_ptr<Ort::Session>(new Ort::Session(*env, path.c_str(), options));
#endif
    }

    // Caller holds the mutex.
    void prepare()
    {
        lastUse = AIIdleReaper::nowMs();
        if (!watched) {
            watched = true;
            Impl* const self = this;
            reaperId = AIIdleReaper::watch([self](bool force) { self->releaseIfIdle(force); });
        }
        if (!env) {
            if (!OrtGetApiBase()->GetApi(ORT_API_VERSION)) {
                throw std::runtime_error("the installed ONNX runtime is too old");
            }
            env.reset(new Ort::Env(ORT_LOGGING_LEVEL_WARNING, "SteepRemover"));
            options.reset(new Ort::SessionOptions());
#ifdef RT_AI_MASKING_DIRECTML
            options->DisableMemPattern();
            options->DisableCpuMemArena();
            options->SetExecutionMode(ORT_SEQUENTIAL);
            if (OrtStatus* status = OrtSessionOptionsAppendExecutionProvider_DML(*options, 0)) {
                std::fprintf(stderr, "AI Remover: DirectML unavailable (%s), using CPU\n",
                             Ort::GetApi().GetErrorMessage(status));
                Ort::GetApi().ReleaseStatus(status);
            } else {
                directml = true;
            }
#endif
            if (!directml) {
                options->SetIntraOpNumThreads(std::max(1u, std::thread::hardware_concurrency() / 2));
                options->SetInterOpNumThreads(1);
            }
            options->AddConfigEntry("session.intra_op.allow_spinning", "0");
            options->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            cpuOptions.reset(new Ort::SessionOptions());
            cpuOptions->SetIntraOpNumThreads(std::max(1u, std::thread::hardware_concurrency() / 2));
            cpuOptions->SetInterOpNumThreads(1);
            cpuOptions->AddConfigEntry("session.intra_op.allow_spinning", "0");
            cpuOptions->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            // Without this the runtime keeps every buffer a run needed --
            // measured 4 GB each for one encode and one decode at 1024 px --
            // for as long as the session lives.
            cpuOptions->DisableCpuMemArena();
            cpuOptions->DisableMemPattern();
        }
    }

    // Only one of the three models is in memory at a time (the codec loads in
    // about a second), so the peak is the largest of them, not their sum.
    void loadDenoiser()
    {
        if (unet) return;
        const long long start = AIIdleReaper::nowMs();
        // About 12 s, nearly all of it reading 5 GB of weights and handing
        // them to the GPU: the graph optimisation level makes no measurable
        // difference, so there is nothing cheap to trim here.
        unet = session(kUnet);
        std::fprintf(stderr, "AI Remover: model loaded on %s in %.1f s\n", directml ? "DirectML" : "CPU",
                     (AIIdleReaper::nowMs() - start) / 1000.0);
    }

    // One model call: float planes in NCHW, one output of `outChannels`.
    std::vector<float> run(Ort::Session& model, const std::vector<const char*>& names,
                           std::vector<std::vector<float>>& data, const std::vector<std::vector<int64_t>>& shapes,
                           const char* outputName, const SmartRepairJob& job)
    {
        auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::vector<Ort::Value> inputs;
        for (size_t i = 0; i < data.size(); ++i) {
            inputs.push_back(Ort::Value::CreateTensor<float>(memory, data[i].data(), data[i].size(),
                                                             shapes[i].data(), shapes[i].size()));
        }
        Ort::RunOptions runOptions;
        if (job) {
            OrtRunOptions* raw = runOptions;
            job->setCancelRun([raw]() {
                if (auto* status = Ort::GetApi().RunOptionsSetTerminate(raw)) Ort::GetApi().ReleaseStatus(status);
            });
        }
        struct ClearCancel {
            const SmartRepairJob& job;
            ~ClearCancel() { if (job) job->setCancelRun({}); }
        } clearCancel{job};
        const char* outputs[] = {outputName};
        auto result = model.Run(runOptions, names.data(), inputs.data(), inputs.size(), outputs, 1);
        const float* out = result[0].GetTensorData<float>();
        const size_t count = result[0].GetTensorTypeAndShapeInfo().GetElementCount();
        return std::vector<float>(out, out + count);
    }
#endif
};

AIRemoverEngine::AIRemoverEngine() : impl(new Impl)
{
}

AIRemoverEngine::~AIRemoverEngine() = default;

AIRemoverEngine& AIRemoverEngine::getInstance()
{
    // Never destroyed: the idle reaper holds a pointer into it.
    static auto* const instance = new AIRemoverEngine();
    return *instance;
}

void AIRemoverEngine::setModelDirectory(const Glib::ustring& directory)
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    impl->directory = directory;
    impl->haveModels = !directory.empty();
    for (const char* file : {kUnet, kUnetData, kEncoder, kDecoder}) {
        impl->haveModels = impl->haveModels
            && Glib::file_test(Glib::build_filename(directory, file), Glib::FILE_TEST_EXISTS);
    }
}

bool AIRemoverEngine::available() const
{
#ifdef RT_AI_MASKING
    if (std::getenv("STEEP_REMOVER_OFF")) return false;
    std::lock_guard<std::mutex> lock(impl->mutex);
    return impl->haveModels;
#else
    return false;
#endif
}

bool AIRemoverEngine::fill(const float* r, const float* g, const float* b, const float* mask, int w, int h,
                           float* outR, float* outG, float* outB, std::uint32_t seed, const SmartRepairJob& job)
{
#ifndef RT_AI_MASKING
    (void)r; (void)g; (void)b; (void)mask; (void)w; (void)h; (void)outR; (void)outG; (void)outB; (void)seed; (void)job;
    return false;
#else
    if (w < 16 || h < 16) return false;
    std::unique_lock<std::mutex> lock(impl->mutex, std::defer_lock);
    while (!lock.try_lock()) {
        if (job) job->checkpoint();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    if (!impl->haveModels) return false;
    const struct TouchOnExit {
        Impl* impl;
        ~TouchOnExit() { impl->lastUse = AIIdleReaper::nowMs(); }
    } touch{impl.get()};

    for (int attempt = 0;; ++attempt) try {
        const long long startMs = AIIdleReaper::nowMs();
        impl->prepare();
        if (!impl->unet) {
            // The mask models this repair has just used (selection, depth,
            // subject) sit idle holding gigabytes; they reload on demand.
            AIIdleReaper::releaseNow(impl->reaperId);
        }
        memoryTrace("start");
        // Running out of memory part-way does not fail cleanly inside the
        // GPU runtime, so do not start what the system cannot finish: the
        // denoiser commits about 7 GB and the codec about 4 GB on top.
        const unsigned long long needed = (impl->unet ? 5ULL : 12ULL) << 30;
        const unsigned long long canCommit = MemGuard::commitHeadroomBytes();
        if (canCommit < needed) {
            std::fprintf(stderr, "AI Remover: skipped, the system can only commit %.1f GB more (needs %.0f GB)\n",
                         canCommit / double(1ULL << 30), needed / double(1ULL << 30));
            return false;
        }
        if (job) job->checkpoint();

        // Model view: at most maxSide, aspect kept, sides padded to a
        // multiple of 32 (the network halves the 1/8-size latent twice).
        const double scale = std::min(1., double(maxSide) / std::max(w, h));
        const int rw = std::max(16, int(std::round(w * scale)));
        const int rh = std::max(16, int(std::round(h * scale)));
        const int mw = (rw + 31) / 32 * 32, mh = (rh + 31) / 32 * 32;
        const int ox = (mw - rw) / 2, oy = (mh - rh) / 2;
        const int count = mw * mh;
        const int lw = mw / 8, lh = mh / 8, lcount = lw * lh;

        float white = 65535.f;
        const float* planes[] = {r, g, b};
        for (int i = 0; i < w * h; ++i) {
            for (const auto* plane : planes) {
                if (!std::isfinite(plane[i])) return false;
                white = std::max(white, plane[i]);
            }
        }

        // sRGB-encoded picture in -1..1, edges repeated into the padding.
        std::vector<float> image(size_t(3) * count), encoded(size_t(w) * h), resized(size_t(rw) * rh);
        const auto padded = [&](std::vector<float>& dst, int offset) {
            for (int y = 0; y < mh; ++y) {
                const int sy = std::max(0, std::min(rh - 1, y - oy));
                for (int x = 0; x < mw; ++x) {
                    dst[offset + y * mw + x] = resized[size_t(sy) * rw + std::max(0, std::min(rw - 1, x - ox))];
                }
            }
        };
        for (int c = 0; c < 3; ++c) {
            for (int i = 0; i < w * h; ++i) {
                const float v = std::max(0.f, planes[c][i] / white);
                encoded[i] = (v <= .0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.f / 2.4f) - .055f) * 2.f - 1.f;
            }
            repairmath::resample(encoded.data(), w, h, resized.data(), rw, rh);
            padded(image, c * count);
        }
        std::vector<float> hole(count);
        repairmath::resample(mask, w, h, resized.data(), rw, rh);
        padded(hole, 0);
        for (auto& v : hole) v = v > .05f ? 1.f : 0.f;

        // Conditioning: the picture with the hole blanked, and the hole
        // itself at latent resolution (nearest, as the model was trained).
        std::vector<float> masked(image);
        for (int c = 0; c < 3; ++c) {
            for (int i = 0; i < count; ++i) {
                if (hole[i] > .5f) masked[c * count + i] = 0.f;
            }
        }
        std::vector<float> latentHole(lcount);
        for (int y = 0; y < lh; ++y) {
            for (int x = 0; x < lw; ++x) latentHole[y * lw + x] = hole[(y * 8 + 4) * mw + x * 8 + 4];
        }

        const std::vector<std::vector<int64_t>> imageShape = {{1, 3, mh, mw}};
        const auto encode = [&](std::vector<float>& pixels) {
            std::vector<std::vector<float>> in;
            in.push_back(std::move(pixels));
            auto out = impl->run(*impl->encoder, {"image"}, in, imageShape, "latent", job);
            pixels = std::move(in[0]);
            if (int(out.size()) != 4 * lcount) throw std::runtime_error("unexpected encoder output");
            return out;
        };
        // The codec is ten times faster on the GPU than on the CPU (about a
        // second against fifteen), but its float32 activations at this size
        // are the first thing to fail when GPU memory is short. That failure
        // is a clean error, so: GPU first, CPU when it will not fit.
        const auto codec = [&](const char* what, const std::function<void(bool onCpu)>& work) {
            if (impl->directml && !std::getenv("STEEP_REMOVER_CPU_CODEC")) {
                try {
                    work(false);
                    return;
                } catch (const Ort::Exception& e) {
                    impl->encoder.reset();
                    impl->decoder.reset();
                    if (job) job->checkpoint();
                    std::fprintf(stderr, "AI Remover: %s on the GPU failed (%.120s); using the CPU\n", what, e.what());
                }
            }
            work(true);
        };
        std::vector<float> pictureLatent, maskedLatent;
        codec("encode", [&](bool onCpu) {
            impl->encoder = impl->session(kEncoder, onCpu);
            pictureLatent = encode(image);
            maskedLatent = encode(masked);
            impl->encoder.reset();
        });
        memoryTrace("encoded");
        impl->loadDenoiser();
        memoryTrace("denoiser loaded");
        if (job) {
            job->checkpoint();
            job->setStage(SmartRepairStage::Generating);
        }

        // Noise levels for every training step, then the ones this run visits.
        std::vector<double> sigmaAll(kTrainSteps);
        {
            double alphaProduct = 1.;
            for (int i = 0; i < kTrainSteps; ++i) {
                const double rootBeta = std::sqrt(kBetaStart)
                    + (std::sqrt(kBetaEnd) - std::sqrt(kBetaStart)) * i / (kTrainSteps - 1);
                alphaProduct *= 1. - rootBeta * rootBeta;
                sigmaAll[i] = std::sqrt((1. - alphaProduct) / alphaProduct);
            }
        }
        int steps = kSteps;
        if (const char* value = std::getenv("STEEP_REMOVER_STEPS")) steps = std::max(2, std::min(60, std::atoi(value)));
        std::vector<int> timesteps;
        for (int i = steps - 1; i >= 0; --i) timesteps.push_back(std::min(kTrainSteps - 1, i * (kTrainSteps / steps) + 1));
        const int skip = steps - std::min(steps, int(steps * kStrength));
        timesteps.erase(timesteps.begin(), timesteps.begin() + skip);

        std::mt19937 rng(seed);
        std::normal_distribution<float> gaussian(0.f, 1.f);
        std::vector<float> latent(size_t(4) * lcount);
        const float startSigma = float(sigmaAll[timesteps.front()]);
        for (size_t i = 0; i < latent.size(); ++i) latent[i] = pictureLatent[i] + startSigma * gaussian(rng);

        const std::vector<std::vector<int64_t>> unetShapes = {{1, 9, lh, lw}, {1}, {1, 6}};
        std::vector<std::vector<float>> unetIn(3);
        unetIn[0].resize(size_t(9) * lcount);
        unetIn[2] = {float(mh), float(mw), 0.f, 0.f, float(mh), float(mw)};
        std::copy(latentHole.begin(), latentHole.end(), unetIn[0].begin() + 4 * lcount);
        std::copy(maskedLatent.begin(), maskedLatent.end(), unetIn[0].begin() + 5 * lcount);
        for (size_t step = 0; step < timesteps.size(); ++step) {
            if (job) job->checkpoint();
            const double sigma = sigmaAll[timesteps[step]];
            const double next = step + 1 < timesteps.size() ? sigmaAll[timesteps[step + 1]] : 0.;
            const float inputScale = float(1. / std::sqrt(sigma * sigma + 1.));
            for (int i = 0; i < 4 * lcount; ++i) unetIn[0][i] = latent[i] * inputScale;
            unetIn[1] = {float(timesteps[step])};
            const auto noise = impl->run(*impl->unet, {"latent", "timestep", "time_ids"}, unetIn, unetShapes, "noise", job);
            if (int(noise.size()) != 4 * lcount) throw std::runtime_error("unexpected denoiser output");
            const float dt = float(next - sigma);
            for (int i = 0; i < 4 * lcount; ++i) {
                if (!std::isfinite(noise[i])) throw std::runtime_error("the denoiser produced a non-finite value");
                latent[i] += noise[i] * dt;
            }
        }
        if (job) job->checkpoint();
        memoryTrace("denoised");
        // Kept for a next fill only when the system has room to spare; the
        // decode and the rest of the repair still need several gigabytes.
        if (MemGuard::commitHeadroomBytes() < 16ULL << 30) {
            impl->unet.reset();
            std::fprintf(stderr, "AI Remover: model released at once, memory is short\n");
        }

        std::vector<std::vector<float>> decodeIn;
        decodeIn.push_back(std::move(latent));
        std::vector<float> decoded;
        codec("decode", [&](bool onCpu) {
            impl->decoder = impl->session(kDecoder, onCpu);
            decoded = impl->run(*impl->decoder, {"latent"}, decodeIn, {{1, 4, lh, lw}}, "image", job);
            impl->decoder.reset();
        });
        if (int(decoded.size()) != 3 * count) throw std::runtime_error("unexpected decoder output");
        memoryTrace("decoded");

        float* outputs[] = {outR, outG, outB};
        for (int c = 0; c < 3; ++c) {
            for (int y = 0; y < rh; ++y) {
                for (int x = 0; x < rw; ++x) {
                    const float v = decoded[c * count + (y + oy) * mw + x + ox];
                    if (!std::isfinite(v)) return false;
                    resized[size_t(y) * rw + x] = std::max(0.f, std::min(1.f, v * .5f + .5f));
                }
            }
            repairmath::resample(resized.data(), rw, rh, outputs[c], w, h);
            for (int i = 0; i < w * h; ++i) {
                const float v = outputs[c][i];
                outputs[c][i] = (v <= .04045f ? v / 12.92f : std::pow((v + .055f) / 1.055f, 2.4f)) * white;
            }
        }
        if (edittrace::enabled()) {
            edittrace::logf("[aiRemover] %dx%d -> model %dx%d, %d steps, %s, %.1fs", w, h, mw, mh, int(timesteps.size()),
                            impl->directml ? "DirectML" : "CPU", (AIIdleReaper::nowMs() - startMs) / 1000.0);
        }
        return true;
    } catch (const SmartRepairCancelled&) {
        throw;
    } catch (const Ort::Exception& e) {
        std::fprintf(stderr, "AI Remover: %s\n", e.what());
        if (job) job->checkpoint(); // a cancelled run surfaces as an ORT error
        if (AIIdleReaper::outOfMemory(e.what())) {
            // Another program (or this one's idle mask models) holds the
            // memory: drop ours, free every idle model, and try once more.
            impl->unet.reset();
            if (attempt == 0) {
                std::fprintf(stderr, "AI Remover: out of memory; freeing idle models and retrying\n");
                AIIdleReaper::releaseNow(impl->reaperId);
                std::this_thread::sleep_for(std::chrono::milliseconds(750));
                continue;
            }
        }
        return false;
    } catch (const std::runtime_error& e) {
        std::fprintf(stderr, "AI Remover: %s\n", e.what());
        return false;
    }
#endif
}

} // namespace rtengine
