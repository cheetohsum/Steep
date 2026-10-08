/* -*- C++ -*-
 *
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
#pragma once

namespace rtengine
{

namespace procparams
{
struct FilmPresetsParams;
}

// What kind of emulsion the Grain tool is imitating. The tool's own sliders
// say how much grain and how large; this says how it is built: how strongly
// the grains gather into clumps, how much of it is colour, whether it is a
// negative's grain (loudest in the lighter mid-tones, as a dense negative
// prints) or a reversal film's (loudest in the shadows), and the stock's own
// graininess relative to its speed. When Film Lab is on, its stock supplies
// these so the texture matches the film being simulated; otherwise a generic
// colour negative is assumed.
struct GrainCharacter {
    float sizeMul = 1.f;      // multiplies the grain diameter the ISO slider implies
    float clumping = .35f;    // 0..1: weight of the coarser clump lattice
    float chroma = 1.f;       // multiplies the Color slider's dye-cloud amplitude
    bool mono = false;        // silver image: no colour grain at all
    float densityPeak = .62f; // L* fraction (0..1) where granularity peaks
    float gain = 1.f;         // stock's graininess for its speed, 1 = typical
};

GrainCharacter filmGrainCharacter(const procparams::FilmPresetsParams& film);

} // namespace rtengine
