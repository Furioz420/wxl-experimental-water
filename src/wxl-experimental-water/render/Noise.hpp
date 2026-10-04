// The shading maps the surface is textured from, synthesized rather than loaded.
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#pragma once

// WHY THE MAPS ARE COMPUTED HERE AND NOT READ FROM ANYWHERE.
//
// The pixel stage needs six things a wave model does not produce: fractal noise to break up
// everything that would otherwise repeat, a living grain finer than any vertex, a broad bump under
// it, a foam pattern with filaments and holes, a horizon panorama, and caustics. All six are
// AUTHORED CONTENT in every engine that has them, so the obvious move was to ask the asset pipeline
// for them by name. That is abandoned: it means calling out to code we do not own at a moment we do
// not choose, and the cost of being wrong about that call is the process.
//
// Every one of those six is a noise function with a different reduction on top. Computing them costs
// a fraction of a second, once, and buys three things a file could not: they TILE exactly, because
// the lattice is periodic by construction and a seam is impossible; they animate in a closed LOOP,
// because the time axis is a third periodic dimension rather than a folder of frames; and they are
// OURS, so nothing about them depends on an asset existing.
//
// The maps are generated once into managed-pool textures -- managed, so a device reset restores them
// without a rebuild -- and bound at the draw, past the deferred state flush, like everything else
// this surface puts on a stage the client does not configure.

namespace wxl::water
{
    /// Frames in the grain loop. The pixel stage crossfades between consecutive ones, so eight
    /// slices of a field that is periodic in time read as continuous boiling rather than as eight
    /// pictures: what the fade hides is the step, and what the loop hides is the seam.
    inline constexpr int kShadeGrainFrames = 8;

    /// Slices in the caustic loop, PACKED INTO THE FOUR CHANNELS OF ONE MAP.
    ///
    /// A loop of separate frames was the wrong shape here twice over. Stepping between them is
    /// visible -- a caustic web that jumps is nothing a pool ever did -- and crossfading needs two
    /// frames bound at once, which is a second sampler on a stage budget that is already full.
    /// Four slices in one texel are one fetch, blended cyclically at no sampler cost, and the loop
    /// closes on itself so slice three's neighbour is slice zero.
    ///
    /// Four is short on purpose: the length of the visible pattern does not come from the slice
    /// count, it comes from the two layers the pixel stage multiplies together at different scales
    /// and different rates. Their combined period is long past anything anybody watches.
    inline constexpr int kShadeCausticSlices = 4;

    /// Builds the maps if they are not built yet. Cheap after the first call; the first one is the
    /// only place the cost lives, and it is reported to the log in milliseconds.
    ///
    /// Returns false if the device refused a texture, which is the one failure mode here -- there is
    /// no file to be missing. Every effect stays off in that case rather than sampling a stage
    /// somebody else owns.
    bool NoiseEnsure(void* device);

    /// Whether the maps are in hand. A false here has to mean the effects are OFF: an unbound
    /// sampler reads whatever the last pass left on that stage, at full strength.
    bool NoiseReady();

    /// Milliseconds the build took, for the panel.
    float NoiseBuildMs();

    /// Puts the maps on their stages, at the draw. `grainFrame` indexes the grain loop and is
    /// wrapped here, so a caller may hand over a free-running counter. The caustic loop takes no
    /// index: its position is a continuous phase the pixel stage blends with.
    void NoiseBind(void* device, int grainFrame);
}
