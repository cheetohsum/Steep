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

unsigned long long getSettledPreviewSerial(const StagedImageProcessor* processor);

/**
 * Smoothed cost of one interactive (non-settled) pass, in milliseconds, or 0
 * before any has run. Lets the GUI pace slider updates to what the pipeline
 * can actually render for this image and profile.
 */
unsigned int getInteractivePassMs(const StagedImageProcessor* processor);

}
