// Builds the wave spectrum from the sea state: what the water is made of, before time is applied.
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

#include <cstdint>

#include "wxl-experimental-water/sea/Wave.hpp"

namespace wxl::water
{
    /// A HASH, NOT A RANDOM SOURCE, and that distinction is what makes the sea shareable.
    ///
    /// Every draw the sea makes is this function of a seed and an index, so two machines land on the
    /// same value without exchanging anything -- there is no stream, no state, and nothing to keep
    /// in step. Two fields stay independent by using different SEEDS, never by keeping their own
    /// copy of the arithmetic: a pure function cannot couple its callers.
    ///
    /// @return a value in [0, 1).
    inline float SeededUnit(uint32_t seed, int index)
    {
        uint32_t h = seed ^ (static_cast<uint32_t>(index) * 0x9E3779B9u);
        h ^= h >> 16;
        h *= 0x7FEB352Du;
        h ^= h >> 15;
        h *= 0x846CA68Bu;
        h ^= h >> 16;
        return static_cast<float>(h & 0xFFFFFFu) / static_cast<float>(0x1000000u);
    }

    /// What the weather asks of the sea. Every field is a shared input: two clients handed the same
    /// SeaConditions at the same time compute the same water, which is the whole basis for letting
    /// waves affect gameplay without replicating them.
    struct SeaConditions
    {
        /// Wind direction in world XY, as a unit vector. Sets the dominant travel direction.
        Vec2 windDir{ 1.0f, 0.0f };

        /// Wind speed in world units per second. Drives the peak wavelength through L = V^2 / g,
        /// so raising it lengthens the swell rather than merely making the same waves taller --
        /// which is the difference between a storm and a calm sea with the amplitude turned up.
        float windSpeed = 12.0f;

        /// Overall height multiplier, in world units. The art control.
        float amplitude = 1.0f;

        /// Crest sharpness in [0, 1]. Zero is a rounded swell, one is the cusp limit. Storms sit
        /// high, calm water sits low; this is what separates a heaving sea from a rolling one.
        float steepness = 0.55f;

        /// How far the short trains fan out from the wind direction, in degrees. Zero gives a
        /// corduroy sea travelling in lockstep; real water spreads.
        float directionSpread = 65.0f;

        /// How far apart the biggest wind crests stand, as a multiple of the wind's own peak length.
        ///
        /// The ladder is fitted between this and the floor below, so it does two things at once and
        /// both are wanted: raising it pushes the longest wind waves further apart AND widens the
        /// span, which is what stops neighbouring trains from beating into ranks. Lowering it packs
        /// the crests together and fills the flat water between them.
        ///
        /// It is a multiple rather than a length because the wind already decides the scale of its
        /// own sea -- an absolute value here would fight the weather rather than shape it.
        float crestSpacing = 2.6f;

        /// Shortest wavelength the trains are allowed to contain, in world units.
        ///
        /// THE HANDOVER LINE to the detail field, and deliberately high -- several tens of units,
        /// not single digits. The trains exist for what a hull answers to, and a hull does not
        /// answer to anything short; every rung spent down there costs a block of vertex
        /// arithmetic to say something the field's continuum says better, and packs the ladder,
        /// which is what makes neighbouring trains beat into ranks. It also keeps the trains'
        /// quasi-periodic pattern's period far outside the frame: a lattice of hundred-unit waves
        /// has nothing visible to repeat.
        float minWavelength = 40.0f;

        /// Wavelength of the underlying swell, in world units.
        ///
        /// SWELL IS NOT THE LOCAL WIND'S DOING, and deriving it from the wind is why the sea has been
        /// all waves and no heave. The Phillips envelope suppresses exactly the lengths the wind
        /// cannot sustain, so stretching the wind spectrum's long end only produces a long wave that
        /// has been damped out of existence. Real swell was raised by a storm days ago and hundreds
        /// of miles away and has been travelling ever since, which is why an oily calm can still be
        /// rolling: it is a system of its own, and it is modelled as one here.
        ///
        /// This is the slow lift under everything else -- the horizon rising and falling -- rather
        /// than any wave you could point at.
        float swellLength = 190.0f;

        /// Share of the total amplitude the swell takes, in [0, 1]. The rest goes to the wind sea.
        ///
        /// There is no third system. Wave SHAPES -- short crests that stand up, lean and break --
        /// are the detail field's job now; a band of short near-cusp trains used to live here to
        /// fake them, and it was the wrong tool: a handful of plane waves is a pattern, and the
        /// vertex arithmetic it cost is exactly what the field's extra cascades needed.
        float swellShare = 0.42f;

        /// Fraction of the open-sea amplitude this location gets, in [0, 1]. Shelter -- a lee shore,
        /// a cove, the middle of an island cluster -- reduces the sea without changing the weather,
        /// which is how a harbour stays workable during a storm.
        float exposure = 1.0f;

        /// Seed for the fixed per-train phases. Shared with every client; changing it produces a
        /// different but equally valid sea.
        uint32_t seed = 0x5EA0F17Eu;
    };

    /// Builds the component table for the given conditions.
    ///
    /// The result is stable frame to frame: only the weather moves it, and then continuously, because
    /// re-deriving a spectrum from scratch mid-session makes the entire surface visibly restart.
    WaveSpectrum BuildSpectrum(const SeaConditions& conditions);

}
