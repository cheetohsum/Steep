#pragma once

namespace rtengine
{

class StagedImageProcessor;

// Small crop/FFT stages suffer badly from large OpenMP teams competing with
// the GUI, thumbnail loaders and other applications. Respect a lower limit.
constexpr int previewThreadCount(int available)
{
    return available < 1 ? 1 : (available < 4 ? available : 4);
}

// Team width for one preview pass. Measured on a 26 MP frame at fit zoom
// (16 logical processors), 4 -> 8 threads: exposure 25 -> 20 ms per slider
// tick, vibrance 41 -> 33, channel mixer 27 -> 22 -- but a pass that runs
// local adjustments goes from 84 to 150 ms, so those keep the narrow team.
// Half the machine at most: the GUI thread scales and paints every frame
// while the next one renders.
constexpr int interactiveThreadCount(int available, bool localAdjustments)
{
    return localAdjustments || available <= 4
        ? previewThreadCount(available)
        : (available / 2 < 4 ? 4 : (available / 2 > 8 ? 8 : available / 2));
}

// Decoding-side work on the whole sensor (preprocess, demosaic) is the
// opposite case: tens of megapixels per stage, and it scales. Measured on a
// 26 MP X-Trans frame (16 logical processors), 3-pass demosaic: 819 ms at 4
// threads. It gets three quarters of the machine; the rest stays with the GUI
// and whatever else is running. STEEP_RAW_THREADS=<n> overrides, for tuning.
int rawStageThreadCount(int available);

// Widens the calling thread's OpenMP team for a raw stage and puts the
// previous width back on the way out.
class RawStageThreads
{
public:
    explicit RawStageThreads(bool widen);
    ~RawStageThreads();
    RawStageThreads(const RawStageThreads&) = delete;
    RawStageThreads& operator=(const RawStageThreads&) = delete;

private:
    int saved_ = 0;
};

unsigned long long getSettledPreviewSerial(const StagedImageProcessor* processor);

/**
 * Smoothed cost of one interactive (non-settled) pass, in milliseconds, or 0
 * before any has run. Lets the GUI pace slider updates to what the pipeline
 * can actually render for this image and profile.
 */
unsigned int getInteractivePassMs(const StagedImageProcessor* processor);

/**
 * True while the preview shows a local-adjustment mask or deltaE view instead
 * of the photograph. That state lives outside the ProcParams, so a captured
 * preview frame cannot be trusted as "the image these params produce".
 */
bool isPreviewShowingMask(const StagedImageProcessor* processor);

}
