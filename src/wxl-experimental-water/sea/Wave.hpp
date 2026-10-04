// The wave model: one definition of the sea, consumed by both the CPU and the vertex stage.
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

#include <cmath>

// The sea is a sum of Gerstner components. Every consumer -- the vertex stage that draws the
// surface, the buoyancy query that floats a boat, the camera test that decides you are underwater --
// reads the SAME component table, prepared once per frame by Prepare(). Nothing anywhere re-derives
// a wave from its own constants.
//
// That rule exists because the previous water broke it: the vertex shader carried four components as
// baked literals while the C++ evaluator built eight from a seeded array, so the surface a boat
// floated on was not the surface being drawn. Any divergence of that kind is silent, and in a
// multiplayer session it becomes two players disagreeing about where the water is.
//
// The surface is also reproducible from (sea state, seed, time) alone. No accumulated state, no
// frame-rate dependence, no random source consulted at runtime: two clients that agree on those
// three inputs compute the same sea, which is what lets waves be authoritative without replicating
// a height field.
namespace wxl::water
{
    /// Deep-water gravity constant used by the dispersion relation, in world units per second squared.
    /// WoW's world unit is close enough to a metre that the real value gives believable wave periods.
    inline constexpr float kGravity = 9.80665f;

    inline constexpr float kTwoPi = 6.283185307179586f;

    /// Playback rate of the long-wave trains.
    ///
    /// Deep-water waves go at sqrt(g/k), so an eighty-unit swell takes seven seconds to pass and
    /// crawls across the frame -- which is correct, and which is why filmed open ocean looks static
    /// until something is floating on it for scale. With no horizon traffic and no hull to pitch, the
    /// only reading of speed left is the surface itself. The earlier 1.5x baseline made the entire
    /// ocean race past the coast; a deliberately slower value lets the larger swell read clearly.
    ///
    /// Applied to the angular frequency alone, so the wavelengths, heights and directions are all
    /// untouched: the sea keeps its shape exactly and only plays faster. Every consumer takes it from
    /// here, so the boat still rides the wave it is drawn on.
    inline constexpr float kTimeScale = 0.70f;

    /// Component budget, and the number the whole division of labour hangs on.
    ///
    /// Eight, down from sixteen, because the trains carry ONLY what a hull answers to: the crossing
    /// swell systems and a few long wind waves. Everything shorter belongs to the detail field,
    /// which is a continuum and draws no lattice however hard it is looked at. A finite sum of
    /// plane waves always has a quasi-periodic pattern -- what saves eight is that with nothing
    /// shorter than several tens of units in the table, that pattern's period sits far outside any
    /// frame a player sees.
    ///
    /// The other half of the reason is the vertex budget: each unrolled component costs a fixed
    /// block of arithmetic, and halving the table is what bought the detail field its extra
    /// cascades and the pixel stage its filtered normals.
    inline constexpr int kMaxComponents = 8;

    struct Vec2
    {
        float x = 0.0f;
        float y = 0.0f;
    };

    struct Vec3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    /// A single Gerstner train, in the form both consumers evaluate.
    ///
    /// Laid out as two float4 so the table uploads to vs_3_0 constants without repacking:
    ///   c[i*2 + 0] = (dir.x, dir.y, k, phase)
    ///   c[i*2 + 1] = (amplitude, qa,  0, 0   )
    struct WaveComponent
    {
        Vec2  dir;        ///< Unit direction of travel in world XY.
        float k = 0.0f;   ///< Wavenumber, 2*pi / wavelength.
        float phase = 0.0f; ///< Phase for this frame, already reduced into [0, 2*pi).

        float amplitude = 0.0f; ///< Crest height above the mean, in world units.
        float qa = 0.0f;        ///< Horizontal pinch, steepness * amplitude. Pre-multiplied for the shader.
        float pad0 = 0.0f;
        float pad1 = 0.0f;
    };

    /// The spectrum: what the sea is made of, before time is applied.
    ///
    /// Derived from the sea state and a world seed, and stable across frames -- it changes only when
    /// the weather does, and then by interpolation rather than by reseeding, because rebuilding the
    /// spectrum mid-flight makes the whole surface visibly restart.
    struct WaveSpectrum
    {
        struct Train
        {
            Vec2  dir;
            float k = 0.0f;
            float amplitude = 0.0f;
            float steepness = 0.0f; ///< Q in [0,1]; 1 is the cusp limit, past which the surface self-intersects.
            float phase = 0.0f;     ///< Fixed offset from the seed, so the sea is reproducible but not aligned.
        };

        Train trains[kMaxComponents];
        int   count = 0;

        /// Largest vertical excursion this spectrum can produce, for culling bounds and for the
        /// depth at which the surface can no longer be reached from below.
        float MaxDisplacement() const
        {
            float sum = 0.0f;
            for (int i = 0; i < count; ++i)
                sum += trains[i].amplitude;
            return sum;
        }
    };

    /// The spectrum with a timestamp applied: what actually gets evaluated and uploaded.
    ///
    /// Time is folded into each component's phase here rather than being handed to the shader,
    /// because the shader would otherwise receive a float that grows without bound and loses the
    /// precision the phase needs. Reducing modulo 2*pi per component keeps every number small and
    /// keeps the wave continuous across the wrap -- the previous water wrapped a shared tick counter
    /// instead, which stepped the whole sea every time it rolled over.
    struct WaveFrame
    {
        WaveComponent components[kMaxComponents];
        int   count = 0;
        float maxDisplacement = 0.0f;

        /// Fills the table for `timeSeconds`, measured on the shared clock.
        void Prepare(const WaveSpectrum& spectrum, double timeSeconds)
        {
            count = spectrum.count;
            maxDisplacement = spectrum.MaxDisplacement();

            for (int i = 0; i < count; ++i)
            {
                const WaveSpectrum::Train& t = spectrum.trains[i];

                // Deep-water dispersion: long waves outrun short ones on their own, so wavelength is
                // the only thing an author has to choose. Tying speed to wavelength by hand is what
                // makes a sum of sines read as a scrolling texture instead of as water.
                const float omega = std::sqrt(kGravity * t.k) * kTimeScale;

                // Reduce (omega * time) before it reaches float. The subtraction of the double-
                // precision product from its own floor is what preserves the low bits at long uptime.
                const double turns = static_cast<double>(omega) * timeSeconds / kTwoPi;
                const double frac  = turns - std::floor(turns);

                WaveComponent& c = components[i];
                c.dir       = t.dir;
                c.k         = t.k;
                c.phase     = static_cast<float>(frac * kTwoPi) + t.phase;
                c.amplitude = t.amplitude;

                // Q is normalised so that 1 is exactly the cusp: beyond kA = 1 a Gerstner crest
                // folds through itself and the surface turns inside out.
                const float kA = t.k * t.amplitude;
                const float q  = (t.steepness < 1.0f) ? t.steepness : 1.0f;
                c.qa = (kA > 1e-6f) ? (q / t.k) : 0.0f;
            }

            for (int i = count; i < kMaxComponents; ++i)
                components[i] = WaveComponent{};
        }
    };

    /// Displacement of the surface point that *starts* at (x, y).
    ///
    /// Gerstner is parametric: this is where the point (x, y) moves to, not the height of the water
    /// above (x, y). For a height query use HeightAt(), which inverts it.
    inline Vec3 Displace(const WaveFrame& frame, float x, float y)
    {
        Vec3 d{ 0.0f, 0.0f, 0.0f };
        for (int i = 0; i < frame.count; ++i)
        {
            const WaveComponent& c = frame.components[i];
            const float theta = c.k * (c.dir.x * x + c.dir.y * y) - c.phase;
            const float s = std::sin(theta);
            const float cs = std::cos(theta);

            d.x += c.dir.x * c.qa * cs;
            d.y += c.dir.y * c.qa * cs;
            d.z += c.amplitude * s;   // sum of sines: zero mean, so the still-water level is preserved
        }
        return d;
    }

    /// Surface height above the still-water plane at world (x, y).
    ///
    /// Inverts the parametric displacement by fixed-point iteration: find the point that lands on
    /// (x, y), then read its rise. Two iterations converge at usable steepness; without this step a
    /// float sits systematically off the crest it should be riding, by roughly the horizontal pinch.
    inline float HeightAt(const WaveFrame& frame, float x, float y, int iterations = 2)
    {
        float px = x;
        float py = y;
        for (int i = 0; i < iterations; ++i)
        {
            const Vec3 d = Displace(frame, px, py);
            px = x - d.x;
            py = y - d.y;
        }
        return Displace(frame, px, py).z;
    }

    /// Surface normal at world (x, y), for shading a CPU-side proxy and for orienting what floats.
    inline Vec3 NormalAt(const WaveFrame& frame, float x, float y)
    {
        // Analytic tangents would need the inverted parameter too; a small central difference is
        // cheaper here and is only ever consumed by gameplay, never by the surface shading.
        constexpr float e = 0.5f;
        const float hL = HeightAt(frame, x - e, y);
        const float hR = HeightAt(frame, x + e, y);
        const float hD = HeightAt(frame, x, y - e);
        const float hU = HeightAt(frame, x, y + e);

        Vec3 n{ (hL - hR) / (2.0f * e), (hD - hU) / (2.0f * e), 1.0f };
        const float len = std::sqrt(n.x * n.x + n.y * n.y + 1.0f);
        n.x /= len;
        n.y /= len;
        n.z /= len;
        return n;
    }
}
