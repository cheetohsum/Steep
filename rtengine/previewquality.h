#pragma once

namespace rtengine
{

class StagedImageProcessor;

unsigned long long getSettledPreviewSerial(const StagedImageProcessor* processor);

/**
 * Smoothed cost of one interactive (non-settled) pass, in milliseconds, or 0
 * before any has run. Lets the GUI pace slider updates to what the pipeline
 * can actually render for this image and profile.
 */
unsigned int getInteractivePassMs(const StagedImageProcessor* processor);

}
