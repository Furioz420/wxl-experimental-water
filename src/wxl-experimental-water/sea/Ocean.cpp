// The detail field: builds it, evolves it, inverts it.
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

#include "wxl-experimental-water/sea/Ocean.hpp"
#include "wxl-experimental-water/sea/OceanFft.hpp"
#include "wxl-experimental-water/sea/Sea.hpp"
#include "wxl-experimental-water/panel/Switches.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace wxl::water
{
    namespace
    {
        constexpr int kCells = kOceanN * kOceanN;

        /// Steepness ka a cascade is allowed to reach AFTER every gain that can still touch it.
        ///
        /// One is where the surface folds through itself. This bound used to be half, and it bounded
        /// only the linear field -- the crest peaking, the wave coupling and the strength slider all
        /// multiplied on top of it, each bounded at its own knob and none at the geometry, and their
        /// product is what shattered the surface. The budget below counts every one of those gains,
        /// the coincidence of their peaks priced in quadrature -- see the gain ceiling in
        /// OceanBuild -- so it is the all-in ceiling of what a vertex can be asked to do, which is
        /// why it sits higher than the old half: nothing is left outside it.
        constexpr float kMaxDetailSteep = 0.8f;

        /// Independent components each cascade's band must hold. Under a couple of hundred the
        /// field draws a pattern instead of dissolving one -- that is the panel's own lattice
        /// warning line -- and the patch floor in OceanBuild is derived from this count rather
        /// than from a multiple of the band edge, so it keeps buying the count when the band
        /// limits move.
        constexpr float kOceanModeTarget = 250.0f;

        using Cx = detail::FftComplex;

        Cx Mul(Cx a, Cx b)
        {
            return Cx{ a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re };
        }

        /// Box-Muller. The spectrum needs GAUSSIAN draws, not uniform ones, and the difference is not
        /// cosmetic: a sea's surface elevation is Gaussian because it is a sum of many independent
        /// components, and that is what puts the occasional crest well above its neighbours. Uniform
        /// draws would make every cell equally likely to be loud and the field would come out flat
        /// and even -- busy, but without the few tall places that read as water.
        void GaussPair(uint32_t seed, int index, float& a, float& b)
        {
            const float u1 = std::max(SeededUnit(seed, index * 2), 1e-6f);
            const float u2 = SeededUnit(seed, index * 2 + 1);
            const float r = std::sqrt(-2.0f * std::log(u1));
            a = r * std::cos(kTwoPi * u2);
            b = r * std::sin(kTwoPi * u2);
        }

        Cx g_work[kCells];
        Cx g_row[kOceanN];
        /// This frame's spectrum, kept because the height and the two sideways components are three
        /// transforms of the SAME thing and rebuilding it twice more would be pure waste.
        Cx g_hk[kCells];

        void Fft2D(Cx* a)
        {
            static const detail::InverseFftPlan<kOceanN> plan{kTwoPi};
            for (int r = 0; r < kOceanN; ++r)
                plan.Transform(a + r * kOceanN);

            for (int c = 0; c < kOceanN; ++c)
            {
                for (int r = 0; r < kOceanN; ++r) g_row[r] = a[r * kOceanN + c];
                plan.Transform(g_row);
                for (int r = 0; r < kOceanN; ++r) a[r * kOceanN + c] = g_row[r];
            }
        }

        struct Cascade
        {
            Cx    h0[kCells];       ///< amplitude and phase of each wavevector, fixed by the weather
            Cx    h0neg[kCells];    ///< conj(h0(-k)), the half that travels the other way
            float omega[kCells];    ///< dispersion, precomputed because it never moves with time
            float height[kCells];   ///< this frame's field
            /// (rise, dx, dy, 0) per texel: what the VERTEX stage consumes, laid out for upload.
            float geo[kCells * 4];
            /// (dZ/dx, dZ/dy, foam, 0) per texel: what the PIXEL stage consumes. Split from the
            /// geometry so each stage fetches only what it reads -- one vertex sampler per cascade
            /// is what makes four cascades possible at all, and the pixel side gets bilinear
            /// filtering on the normal, where a flat step shows hardest.
            float shade[kCells * 4];
            float scale = 0.0f;     ///< normalisation, measured once so the ceiling is exact
            float bandLong = 0.0f;  ///< wavelengths this cascade ended up with
            float bandShort = 0.0f;
            float patch = 1.0f;     ///< derived from the band, not authored -- see OceanBuild
            float maxGather = 0.0f; ///< cusp bound on the sideways slide, all VS gains counted

            /// Asked/granted/why, for the panel. The simulation never reads it back.
            OceanCascadeReport report;

            /// Unit wavevector per cell, kept because the horizontal displacement needs it every
            /// frame and it never moves.
            float knx[kCells];
            float kny[kCells];
            /// (dx, dy, 0, 0) per texel: where the surface slides SIDEWAYS.
            float disp[kCells * 4];

            /// White, and it CARRIES BETWEEN FRAMES -- which is the difference between foam and a
            /// flicker. Born where the displacement folds the surface through itself, then left to
            /// decay on its own clock, so a crest that broke a second ago is still marked.
            float foam[kCells];
        };

        Cascade    g_cascade[kOceanCascades];
        bool       g_built = false;
        float      g_maxRise = 0.0f;
        OceanTuning g_tuning;
        /// Shared storm intensity, pushed by the sea's tick. Blended into the tuning's storm
        /// multipliers where each one is consumed -- build-time for the amplitude and the fold
        /// budget, evolve-time for the chop and the foam threshold.
        float      g_storm = 0.0f;
        /// Last evolve, so the foam's decay runs on elapsed seconds rather than on frames. Negative
        /// means "no previous frame": a rebuild, or the first evolve after one.
        double     g_lastTime = -1.0;
        /// Suppresses the nonlinear terms for the build's own settling passes.
        ///
        /// The ceiling is measured by running the field once and looking at it, and that first run
        /// happens at a scale of one -- an unnormalised transform whose values are arbitrary. A term
        /// in h SQUARED against arbitrary h is not merely wrong, it dominates: the measured spread
        /// would come almost entirely from the correction, the scale derived from it would be far too
        /// small, and the sea would end up a fraction of the height asked for.
        ///
        /// It is also the right definition. The amplitude ceiling describes the LINEAR field; peaking
        /// is documented as growing past it.
        bool       g_linearOnly = false;

        /// Phillips, restricted to one cascade's band.
        ///
        /// THE BAND SPLIT IS NOT AN OPTIMISATION. Two patches over the same wavenumbers would carry
        /// that energy twice, so those lengths would come out at double amplitude while the spectrum
        /// as a whole stayed normalised -- a loud ring in the middle of the field and a hole either
        /// side of it. Each cascade owns its stretch and nothing else.
        float Energy(float kx, float ky, float windSpeed, float wx, float wy,
                     float loK, float hiK, float comb)
        {
            const float k2 = kx * kx + ky * ky;
            if (k2 < 1e-8f) return 0.0f;

            const float k = std::sqrt(k2);
            if (k < loK || k >= hiK) return 0.0f;

            const float L = (windSpeed * windSpeed) / kGravity;
            const float kL = k * L;
            const float suppression = std::exp(-1.0f / std::max(kL * kL, 1e-6f));

            // Square-on to the wind survives, across it does not -- but never to nothing. A hard zero
            // across the wind leaves a seam of dead water running through the field along one axis,
            // which is far more visible than the anisotropy it was trying to express. The comb is
            // the CASCADE'S own: long waves hold the wind's line, short chop crosses it, and that
            // difference is half of what separates the populations to the eye.
            //
            // THE LOBE IS RAISED TO A POWER, AND THE POWER IS THE LENGTH OF THE CRESTS. At the
            // classic square, energy forty-five degrees off the wind still carries half its weight,
            // and systems crossing at that angle chop every front into pieces about one wavelength
            // long -- and a sea of wavelength-long crests is a sheet of round bumps whatever else
            // is done to it. Narrowing the lobe stretches the crests crosswind into fronts several
            // wavelengths long, which is most of what separates water that ARRIVES from water that
            // boils. Inside the comb blend, so each cascade's confused floor is untouched: the
            // short populations stay confused, the long ones grow fronts.
            const float align = (kx * wx + ky * wy) / k;
            const float lobe = std::pow(align * align, std::max(g_tuning.crestLength, 1.0f));
            float directional = (1.0f - comb) + comb * lobe;

            // AND THE SEA IS GIVEN A DIRECTION TO TRAVEL, which no even lobe can express. An even
            // function weighs a wavevector and its opposite identically, so every component was
            // born paired with an equal one running the other way -- and two equal waves running
            // opposite ways are a STANDING wave: lobes that inflate and deflate in place instead
            // of travelling. Half the field's energy was spent on that pulsing, and a pulsing bump
            // is precisely what reads as a bubble rather than a wave. Suppressed rather than
            // removed: a few percent of backwash keeps the surface from reading as a conveyor.
            if (align < 0.0f)
                directional *= std::clamp(g_tuning.upwind, 0.0f, 1.0f);

            return suppression * directional / std::pow(k, std::max(g_tuning.falloff, 0.5f));
        }
    }

    void OceanBuild(const SeaConditions& conditions)
    {
        const float windSpeed = std::max(conditions.windSpeed, 0.5f);
        const float wlen = std::sqrt(conditions.windDir.x * conditions.windDir.x +
                                     conditions.windDir.y * conditions.windDir.y);
        const float wx = (wlen > 1e-6f) ? conditions.windDir.x / wlen : 1.0f;
        const float wy = (wlen > 1e-6f) ? conditions.windDir.y / wlen : 0.0f;

        // Zero still means "wherever the trains begin", but overlapping them is allowed and is now
        // the default. Two independent random fields over the same stretch ADD -- their variances
        // add -- they do not cancel; what an overlap costs is that band counted twice, which is a
        // loudness to tune rather than an artefact to avoid. Deferring to the trains' floor is only
        // sensible when that floor is high, and with a floor of five units it left this field a
        // sliver two units wide.
        const float shortest = std::max(g_tuning.shortest, 0.1f);
        const float asked    = (g_tuning.longest > 0.0f) ? g_tuning.longest
                                                         : conditions.minWavelength;
        const float longest  = std::max(asked, shortest * 2.0f);

        for (int ci = 0; ci < kOceanCascades; ++ci)
        {
            Cascade& cs = g_cascade[ci];

            // THE BAND IS SPLIT IN WAVELENGTHS, AND THIS IS THE CORRECTION OF A REAL FAULT.
            //
            // The handover used to be expressed as the NEXT cascade's patch size while the band
            // limits were expressed as wavelengths -- two different currencies, kept consistent by
            // hand and therefore not kept consistent at all. With a long limit of six units against
            // a fine patch of seven, the coarse cascade was asked for everything between seven and
            // six units: an empty intersection, a field of exact zeros, and a sideways displacement
            // computed from it that could only ever be zero too. The sea moved straight up and down
            // and nothing in the panel said why.
            //
            // A geometric ladder from the long limit to the short one has no way to be empty: every
            // rung is a fixed ratio of the one above, whatever the two ends are set to.
            const float lg = std::log(longest / shortest);
            const float hiOfBand = longest * std::exp(-lg * static_cast<float>(ci)
                                                          / static_cast<float>(kOceanCascades));
            const float loOfBand = longest * std::exp(-lg * static_cast<float>(ci + 1)
                                                          / static_cast<float>(kOceanCascades));

            const float bandLo = kTwoPi / hiOfBand;   // long wavelength, small wavenumber
            const float bandHi = kTwoPi / loOfBand;

            // THE PATCH IS THE SPAN THE COMPONENT COUNT NEEDS, TIMES ONE KNOB. A cascade's chaos
            // is the number of grid cells inside its band's annulus -- pi * patch^2 *
            // (1/lo^2 - 1/hi^2) -- and the floor used to ask for that count by proxy: five of the
            // band's longest wave, justified as "a couple of hundred modes at this band ratio".
            // Then the band limits moved, the ratio narrowed, and the proxy silently stopped
            // buying what it was there to buy: every annulus shrank to a hundred-odd cells and
            // the panel called all four cascades a lattice at once. So ask for the count itself:
            // the base span is the patch at which the annulus holds the target, whatever the two
            // band edges are set to.
            //
            // The knob on top replaced a cell-size slider that had quietly died twice over: it
            // was clamped at the mesh's finest vertex spacing, and even under that clamp the
            // span this floor needs kept every cascade's patch far above what the slider could
            // reach -- the cell that ships has been patch/N for some time, whatever it said.
            // As a span multiplier it buys what the panel's own advice asks for: repeats pushed
            // further out and modes by the SQUARE of it, paid for in cell size -- a coarseness
            // the shading no longer shows, because the normal rides the pixel stage's filtered
            // path and the vertex read goes through the tent.
            //
            // No stagger any more: it existed so patches settling on one shared clamp could not
            // sit in a whole ratio, and the spans are band-derived now -- four bands a fixed
            // irrational-looking ratio apart have no common measure to come back into step on.
            const float invLo = 1.0f / (loOfBand * loOfBand);
            const float invHi = 1.0f / (hiOfBand * hiOfBand);
            const float spanForModes = std::sqrt(kOceanModeTarget
                                       / (3.1415927f * std::max(invLo - invHi, 1e-9f)));
            const float patch = spanForModes * std::clamp(g_tuning.cellSize, 0.5f, 3.0f);
            cs.patch = patch;

            // Kept so the panel can say what this cascade actually got.
            cs.bandLong  = hiOfBand;
            cs.bandShort = loOfBand;

            const float comb  = std::clamp(g_tuning.align[ci], 0.0f, 1.0f);
            const float drift = std::max(g_tuning.timeScale, 0.0f)
                              * std::max(g_tuning.speed[ci], 0.0f);
            for (int r = 0; r < kOceanN; ++r)
            {
                for (int c = 0; c < kOceanN; ++c)
                {
                    const int i = r * kOceanN + c;
                    const float kx = kTwoPi * static_cast<float>(c - kOceanN / 2) / patch;
                    const float ky = kTwoPi * static_cast<float>(r - kOceanN / 2) / patch;

                    const float e = Energy(kx, ky, windSpeed, wx, wy, bandLo, bandHi, comb);
                    const float amp = std::sqrt(std::max(e, 0.0f)) * 0.7071068f;

                    float ga = 0.0f, gb = 0.0f;
                    GaussPair(g_tuning.seed + static_cast<uint32_t>(ci) * 0x9E3779B9u, i, ga, gb);
                    cs.h0[i] = Cx{ ga * amp, gb * amp };

                    const float k = std::sqrt(kx * kx + ky * ky);
                    cs.omega[i] = std::sqrt(kGravity * k) * kTimeScale * drift;

                    // Unit wavevector. A wave slides sideways along its OWN heading and by an amount
                    // that does not depend on its length, which is why this is normalised and the
                    // wavenumber itself plays no part below.
                    const float invK = (k > 1e-6f) ? (1.0f / k) : 0.0f;
                    cs.knx[i] = kx * invK;
                    cs.kny[i] = ky * invK;
                }
            }

            // conj(h0(-k)), taken after the whole grid exists because it reads the opposite cell.
            for (int r = 0; r < kOceanN; ++r)
            {
                for (int c = 0; c < kOceanN; ++c)
                {
                    const int i = r * kOceanN + c;
                    const int rn = (kOceanN - r) % kOceanN;
                    const int cn = (kOceanN - c) % kOceanN;
                    const Cx o = cs.h0[rn * kOceanN + cn];
                    cs.h0neg[i] = Cx{ o.re, -o.im };
                }
            }

            cs.scale = 1.0f;
        }

        g_built = true;
        // The foam carries between frames, so it needs a clock, and the clock has to be told that the
        // two settling passes below are not elapsed time.
        g_lastTime = -1.0;

        // NORMALISED BY MEASUREMENT, not by algebra. The transform's own constant factor, the band
        // limits and the directional floor all scale the result, and carrying each of them through
        // by hand is three chances to be wrong about a number nobody can check by looking. One
        // evolve and one pass over the field settles it exactly.
        //
        // Measured on the LINEAR field. This first pass runs at a scale of one, and a term in h
        // squared against an arbitrary h does not merely perturb the measurement, it dominates it.
        g_linearOnly = true;
        OceanEvolve(0.0);

        // THE ALL-IN GAIN CEILING, computed once and divided into every budget below.
        //
        // This is the correction of the fault that shattered the surface: each bound used to hold
        // the LINEAR field, and the wave coupling, the crest peaking and the strength slider then
        // multiplied on top -- three gains each reasonable at its own knob, none counted where they
        // became geometry, product well past the cusp. Every multiplier the vertex stage or the
        // evolve can still apply is enumerated HERE, and the field is normalised so that even their
        // worst case stays inside kMaxDetailSteep. The knobs become genuinely free: asking past the
        // budget moves the "granted" readout, never the geometry past its fold.
        //
        //   coupling  -- the vertex stage scales the field by 1 + c*(fold - 0.5), fold in [0,1],
        //                so its ceiling is 1 + c/2 (the floor only ever reduces).
        //   peaking   -- the Stokes term is clamped to double a crest at most; at the three-sigma
        //                reference the normalisation is measured against, it reaches
        //                1 + min(8/3 * stokes, 1).
        //   strength  -- an INPUT to the budget now, not a gain after it.
        const float couplingMax = 1.0f + std::max(g_tuning.waveCoupling, 0.0f) * 0.5f;
        const float peakingMax  = 1.0f + std::min(std::max(g_tuning.stokes, 0.0f) * (8.0f / 3.0f),
                                                  1.0f);

        // COMBINED IN QUADRATURE, NOT MULTIPLIED, and the difference is a third of the sea's
        // steepness. The two gains ride INDEPENDENT random fields -- the crowding follows the
        // trains' rise, the peaking follows this field's own height -- so each ceiling is a
        // three-sigma event of its own field, and their product priced the coincidence of the two
        // as if it held at every vertex, always. That insurance is what kept every crest too round
        // to ever read as chop. Excesses added in quadrature is the same rule the cascades' shares
        // already use: the full budget where one gain peaks alone, a few percent past the bound on
        // the rare texel and frame where both peak together at once. The systematic product that
        // shattered the surface stays impossible -- both factors are still counted in the ceiling;
        // only their coincidence stopped being billed as certainty.
        const float gainCeil = 1.0f + std::sqrt((couplingMax - 1.0f) * (couplingMax - 1.0f)
                                              + (peakingMax - 1.0f) * (peakingMax - 1.0f));

        // THE CEILING ITSELF, HANDED TO TASTE -- explicitly, where every other knob stays inside
        // the budget. At one the field runs at the engineered bound: worst cases priced, a typical
        // crest well back from the fold, a sea that is safe and reads round. Past one the height
        // and the pinch bounds scale together and the deepest crests begin to genuinely fold --
        // which is where the foam is measured, so the cost lands as white lying exactly where it
        // happened rather than as a mystery artefact. The one number allowed to buy folds --
        // and the one a storm leans on: gales break, so the storm blend rides this and not the
        // amplitude alone.
        const float stormFold = 1.0f + (std::max(g_tuning.stormFold, 0.0f) - 1.0f) * g_storm;
        const float budget = std::clamp(g_tuning.foldBudget * stormFold, 0.25f, 2.5f);
        const float strength = std::max(g_tuning.strength, 0.0f);
        const float stormAmp = 1.0f + (std::max(g_tuning.stormAmplitude, 0.0f) - 1.0f) * g_storm;

        for (int ci = 0; ci < kOceanCascades; ++ci)
        {
            Cascade& cs = g_cascade[ci];

            double sum = 0.0;
            for (int i = 0; i < kCells; ++i)
                sum += static_cast<double>(cs.height[i]) * cs.height[i];
            const float rms = static_cast<float>(std::sqrt(sum / kCells));

            // Three sigma rather than the observed peak: the peak of one sample of a Gaussian field
            // is itself random, so normalising to it would make the sea's height depend on which
            // instant happened to be measured.
            const float asked = std::max(g_tuning.amplitude, 0.0f) * strength * stormAmp
                              / static_cast<float>(kOceanCascades);

            // CAPPED BY WHAT THE BAND CAN PHYSICALLY CARRY. A height in world units means nothing
            // on its own -- two units of rise is a gentle swell over twenty-unit waves and a
            // vertical wall over three-unit ones -- so the limit lives on the steepness ka, and the
            // amplitude allowed follows from the band's wavenumber. Split between cascades in
            // QUADRATURE, the same rule the trains use: two independent fields fold together only
            // where they happen to agree, so each may carry the budget over root-two rather than
            // half of it.
            const float kRep = kTwoPi / std::sqrt(std::max(cs.bandLong * cs.bandShort, 1e-3f));
            const float quadShare = std::sqrt(static_cast<float>(kOceanCascades));
            const float roomForBand = budget * kMaxDetailSteep / (quadShare * kRep * gainCeil);
            const float granted = std::min(asked, roomForBand);

            cs.scale = (rms > 1e-8f) ? (granted / (3.0f * rms)) : 0.0f;

            // The cusp bound on the sideways slide, with the same worst-case gains divided in. The
            // clamp itself runs at evolve, where chop and the height-driven gain have already been
            // folded into the displacement -- so what it bounds is the geometry, not an input to it.
            // Scaled by the same fold budget as the height: pinch is what sharpens a crest, and a
            // raised ceiling that lifted only the rise would buy taller ROUND waves. The foam's
            // reach is measured from this bound, so past-one folds arrive already whitened.
            cs.maxGather = budget * std::sqrt(std::max(cs.bandLong * cs.bandShort, 1e-3f)) / kTwoPi
                         / (couplingMax * quadShare);

            // What the panel says. A refusal that only shows as a quieter sea is indistinguishable
            // from a broken transform, so every one of these is rendered rather than inferred.
            cs.report.bandLong    = cs.bandLong;
            cs.report.bandShort   = cs.bandShort;
            cs.report.patch       = cs.patch;
            cs.report.cell        = cs.patch / static_cast<float>(kOceanN);
            cs.report.askedRise   = asked;
            cs.report.grantedRise = granted;
            cs.report.gainCeiling = gainCeil;
            // Independent components: grid cells inside the band's annulus in frequency space,
            // pi * (r_out^2 - r_in^2) with each radius the patch measured in wavelengths.
            const float rIn  = cs.patch / cs.bandLong;
            const float rOut = cs.patch / cs.bandShort;
            cs.report.modes = static_cast<int>(3.1415927f * (rOut * rOut - rIn * rIn));
        }

        g_linearOnly = false;
        OceanEvolve(0.0);

        // Wiped AFTER the settling passes, not before them. Foam accumulates rather than being
        // recomputed, and the first of those passes runs at a scale of one -- an unnormalised field
        // whose displacement folds everywhere -- so it leaves the whole grid white, and the max()
        // that gives foam its memory then holds it there. Clearing it here costs a frame of foam
        // after a weather change and is the only order in which it cannot stick.
        for (Cascade& cs : g_cascade)
        {
            std::memset(cs.foam, 0, sizeof(cs.foam));
            for (int i = 0; i < kCells; ++i) cs.shade[i * 4 + 2] = 0.0f;
        }
    }

    void OceanEvolve(double timeSeconds)
    {
        if (!g_built) return;

        // Elapsed seconds for the foam's decay, and only for that -- everything else is a closed
        // form of absolute time and does not care how it got here. Clamped so a stall, a rebuild or
        // a clock that went backwards wipes the foam rather than carrying a decade of it.
        const float dt = (g_lastTime >= 0.0 && timeSeconds > g_lastTime)
                             ? static_cast<float>(std::min(timeSeconds - g_lastTime, 0.25))
                             : 0.0f;
        g_lastTime = timeSeconds;

        g_maxRise = 0.0f;

        for (int ci = 0; ci < kOceanCascades; ++ci)
        {
            Cascade& cs = g_cascade[ci];

            for (int i = 0; i < kCells; ++i)
            {
                // Reduced before it reaches float. A phase built from a growing double keeps its low
                // bits; the same product accumulated in float loses them within an hour of uptime and
                // the field starts stepping instead of moving.
                const double turns = static_cast<double>(cs.omega[i]) * timeSeconds / kTwoPi;
                const double frac = turns - std::floor(turns);
                const float theta = static_cast<float>(frac * kTwoPi);

                const Cx fwd{ std::cos(theta), std::sin(theta) };
                const Cx bwd{ fwd.re, -fwd.im };

                // THE DRAW AT k MUST TRAVEL ALONG k. The inverse transform reconstructs on
                // e^{+ik.x}, so the wavevector's coefficient must carry e^{-i omega t} for the
                // pair to read k.x - omega t -- a wave marching the way its wavevector points.
                // This pairing was reversed, and the consequence was not cosmetic: every choice
                // the spectrum makes ABOUT direction -- the lobe, its power, the upwind
                // suppression -- acted on a sea travelling the other way, so the suppression kept
                // exactly the half it was built to remove and the whole field marched against the
                // wind. The conjugate mirror keeps the field real either way, which is why nothing
                // else ever flagged it.
                const Cx a = Mul(cs.h0[i], bwd);
                const Cx b = Mul(cs.h0neg[i], fwd);
                g_hk[i] = Cx{ a.re + b.re, a.im + b.im };
            }

            std::memcpy(g_work, g_hk, sizeof(g_work));
            Fft2D(g_work);

            double sumSq = 0.0;
            for (int r = 0; r < kOceanN; ++r)
            {
                for (int c = 0; c < kOceanN; ++c)
                {
                    const int i = r * kOceanN + c;
                    // Undoes the half-grid shift the wavevectors were laid out with. Without it the
                    // field comes out with every other sample negated -- a checkerboard at exactly
                    // the lattice frequency, which is the most visible artefact this can produce.
                    const float sign = ((r + c) & 1) ? -1.0f : 1.0f;
                    const float h = g_work[i].re * sign * cs.scale;
                    cs.height[i] = h;
                    sumSq += static_cast<double>(h) * h;
                }
            }

            // AND HERE THE FIELD STOPS BEING LINEAR, which is the only way it can stop being a
            // pattern that merely slides past.
            //
            // Everything above -- and every Gerstner train next door -- is a SUM OF COMPONENTS WITH
            // FIXED AMPLITUDES TRAVELLING AT FIXED SPEEDS. That is the whole content of both models,
            // and it has a consequence no amount of tuning reaches: a wave in either of them can only
            // translate and superpose. It cannot steepen, cannot lean, cannot overtake its
            // neighbour, cannot collapse. Groups form and dissolve where components beat, and that is
            // the entire repertoire.
            //
            // Real water is not like that because its speed depends on its own height: a crest
            // travels faster than the trough behind it, catches the wave ahead, and the face steepens
            // until it goes over. Amplitude feeding back into the motion IS the nonlinearity, and
            // both models are defined by not having it.
            //
            // The second-order Stokes correction is that feedback's first term, and on a grid it is
            // one multiply. Squaring the field produces its second harmonic -- h^2 of a cosine is a
            // constant plus a cosine of twice the angle -- and that harmonic peaks exactly at the
            // crests and troughs, adding height to the crest and taking depth from the trough. What
            // comes out is a peaked crest over a long flat trough, which is the profile of real water
            // and the "mountain" silhouette rather than a hump. The mean is subtracted because the
            // constant would lift the whole sea.
            //
            // Divided by the field's own spread rather than multiplied by its wavenumber, which is
            // the physical form and is useless as a control. The true coefficient is k/2, so the
            // correction relative to the wave is ka/2 -- and this layer is a ceiling of half a unit
            // over wavelengths of ten to twenty-five, which puts ka around a tenth. Three per cent of
            // a crest nobody can see the shape of. Correct, and indistinguishable from off.
            //
            // Normalised by the spread instead, the knob means the same thing whatever the amplitude
            // and the band are set to: one is a correction the size of the field itself. The shape it
            // produces is the same shape; only its size stops being decided by two other sliders.
            const float meanSq = static_cast<float>(sumSq / kCells);
            const float fieldRms = std::sqrt(std::max(meanSq, 1e-12f));
            const bool  nonlinear = !g_linearOnly && On(Sw::Nonlinear);
            const float stokes = nonlinear ? std::max(g_tuning.stokes, 0.0f) / fieldRms : 0.0f;

            for (int i = 0; i < kCells; ++i)
            {
                float h = cs.height[i];
                float add = stokes * (h * h - meanSq);

                // BOUNDED BY THE HEIGHT IT IS CORRECTING, both ways, and the upper half of that was
                // missing entirely.
                //
                // The term is QUADRATIC on a field that is Gaussian, which means it has no worst
                // case: a four-sigma cell -- and a Gaussian field has them -- receives sixteen times
                // what a one-sigma cell does. A correction meant to round a crest into a peak instead
                // turns the rare tall cell into a spike several times the height of everything around
                // it, at a spacing of one grid cell. That is a field of shards, not a sea, and it is
                // what a screenshot of it looks like.
                //
                // The even symmetry also means it pushes UP below the waterline, which is the trough
                // flattening and is wanted -- until it exceeds the depth it is filling and the
                // deepest troughs come out above the shallow ones, inverting the surface.
                //
                // One bound answers both: never move a point by more than it already stands from the
                // waterline. A crest can at most double, a trough can at most reach flat.
                const float cap = std::fabs(h);
                add = std::clamp(add, -cap, cap);

                h += add;
                cs.height[i] = h;

                const float rise = std::fabs(h);
                if (rise > g_maxRise) g_maxRise = rise;
            }

            // WHERE THE SURFACE SLIDES SIDEWAYS, which is what turns a bump into a wave.
            //
            // Everything above moves points straight up and down, and a field that only does that is
            // flat in the sense that matters: its crests are as broad as its troughs, because a
            // height field has no way to gather water. Real waves gather -- the surface creeps
            // FORWARD under a crest and back under a trough, so crests narrow and sharpen while
            // troughs widen out. It is the same thing the trains' pinch does, and it is why they
            // read as waves while this read as a rumpled sheet.
            //
            // The transform of -i * khat * h gives exactly that, and it is two more passes over a
            // spectrum already built. Every cascade, not only the coarse one: the argument for
            // sparing the fine one was that a short wave has no room to gather, and that is true of
            // its ABSOLUTE displacement and irrelevant to its shape -- a three-unit wave gathers
            // three-unit-wave amounts and comes out just as pointed. Sparing it left half the field
            // as the rumpled sheet the other half had stopped being.
            //
            // It also has to come before the foam below, which is measured from it.
            //
            // CHOP IS BAKED IN RIGHT HERE, and no longer applied in the vertex stage. Three things
            // consume this displacement -- the geometry, the cusp clamp below, and the fold the
            // foam is born from -- and while chop was multiplied on afterwards by the shader, the
            // clamp and the foam were bounding and measuring a displacement that was not the one
            // being drawn. Baking it before all three puts one number in front of every consumer,
            // which is the whole of what "bounded where it becomes geometry" means. The gate is the
            // vertical-only fallback: chop off, the field still rises and falls, and any artefact
            // that survives is not the sideways term's.
            // The storm's share of the gather rides here at evolve time, so a rolling gale
            // pinches the sea harder frame by frame without waiting on a rebuild. The cusp clamp
            // downstream still bounds the result, so a storm cannot buy a fold the budget has
            // not priced.
            const float stormChop = 1.0f + (std::max(g_tuning.stormChop, 0.0f) - 1.0f) * g_storm;
            const float chopEff = On(Sw::DetailChop)
                                      ? std::max(g_tuning.chop, 0.0f) * stormChop : 0.0f;
            for (int pass = 0; pass < 2; ++pass)
            {
                const float* comp = (pass == 0) ? cs.knx : cs.kny;
                for (int i = 0; i < kCells; ++i)
                    g_work[i] = Mul(g_hk[i], Cx{ 0.0f, -comp[i] });

                Fft2D(g_work);

                for (int r = 0; r < kOceanN; ++r)
                {
                    for (int c = 0; c < kOceanN; ++c)
                    {
                        const int i = r * kOceanN + c;
                        const float sign = ((r + c) & 1) ? -1.0f : 1.0f;
                        cs.disp[i * 4 + pass] = g_work[i].re * sign * cs.scale * chopEff;
                    }
                }
            }

            // THE SECOND NONLINEAR TERM, and the one that makes waves run into each other.
            //
            // Everything above still moves the surface by an amount that depends only on WHERE it is,
            // never on how tall it is there. So the whole field slides past at the speeds its
            // wavelengths dictate and the pattern is rigid: the same shapes arriving, forever.
            //
            // This makes the GATHERING depend on the local height: a tall crest pinches harder than a
            // small one, so the two distort each other as they pass instead of holding station side
            // by side. That is the collision, and no sum of fixed components can do it at any count.
            //
            // A GAIN ON THE DISPLACEMENT ALREADY THERE, NOT A PUSH OF ITS OWN, and the difference is
            // the whole correctness of it. The textbook form shifts every point along the wind in
            // proportion to its height, and that is wrong here for a reason worth keeping: one
            // direction shared by the entire field means a smooth height field produces a smooth
            // COHERENT shove, which does not shear the surface -- it TRANSLATES it, by an amount that
            // wanders slowly across the world. Laid over Gerstner crests hundreds of units long, that
            // reads as those crests snaking, and it was calibrated at a fifth of a wavelength on top
            // of that, some twenty times the real term.
            //
            // Scaling what is already there keeps every displacement pointing along its own
            // component's heading, which is as disordered as the spectrum. It cannot line up into a
            // direction, so it cannot meander. It can only pinch harder where the water is higher,
            // which is all that was ever wanted.
            const float lean = nonlinear ? g_tuning.lean : 0.0f;
            if (lean > 1e-6f)
            {
                const float invPeak = 1.0f / (3.0f * fieldRms);
                for (int i = 0; i < kCells; ++i)
                {
                    // Never negative. Past a full cancellation the displacement reverses, and a
                    // reversed gathering puts a crest in the bottom of every trough.
                    const float gain = std::max(1.0f + lean * cs.height[i] * invPeak, 0.0f);
                    cs.disp[i * 4 + 0] *= gain;
                    cs.disp[i * 4 + 1] *= gain;
                }
            }

            // AND THE WHOLE SIDEWAYS TERM IS HELD AT THE CUSP, which is the same bound the trains
            // have always had and this field never did.
            //
            // A surface gathered by more than a wavelength over two pi has folded through itself --
            // that is what the cusp IS, and it does not care whether the displacement came from the
            // transform, the chop baked in above, or the gain just applied. All of them are in the
            // number being clamped now, and the bound itself (see OceanBuild) has the vertex
            // stage's remaining gain and the other cascade's share divided in -- so this is the
            // last word on the geometry, not on one input to it.
            //
            // Clamped in length rather than per axis, so a diagonal gather is bounded by the same
            // number as an axial one and the direction survives the clamp untouched. The clamped
            // fraction goes to the panel: chop asking past the cusp is legitimate tuning, but only
            // if something says how much of the field is living on the limiter.
            int clamped = 0;
            for (int i = 0; i < kCells; ++i)
            {
                const float dx = cs.disp[i * 4 + 0];
                const float dy = cs.disp[i * 4 + 1];
                const float len = std::sqrt(dx * dx + dy * dy);
                if (len > cs.maxGather)
                {
                    const float s = cs.maxGather / len;
                    cs.disp[i * 4 + 0] = dx * s;
                    cs.disp[i * 4 + 1] = dy * s;
                    ++clamped;
                }
            }
            cs.report.clampedFrac = static_cast<float>(clamped) / static_cast<float>(kCells);

            // WHERE THE WHITE COMES FROM, and this is the second correction in this file.
            //
            // It used to be the surface's own STEEPNESS, and steepness is the wrong quantity twice
            // over. It is wrong physically -- a steep face is not a breaking face, a swell can be
            // steep for a mile without a fleck on it -- and it is wrong in shape: steepness is a
            // per-texel number, so with an unfiltered fetch every texel over the threshold went
            // white on its own and the result was a scatter of specks. Dots, exactly as reported.
            //
            // What actually breaks is where the horizontal displacement folds the surface through
            // itself, and the determinant of that map says where. It is CONNECTED by construction:
            // the gathering runs along the front of a crest, so the mask comes out as streaks lying
            // on the wave rather than as isolated points. Same quantity the trains already use for
            // their own fold, so the two agree about what breaking means.
            const float step = cs.patch / static_cast<float>(kOceanN);
            const float inv2 = 0.5f / step;
            // Divided, not subtracted: the storm lowers the bar the fold has to clear, so white
            // arrives earlier on the same crests -- the one change of the four the eye reads as
            // "the weather turned", because foam coverage is what a sea state LOOKS like.
            const float stormFoam = 1.0f + (std::max(g_tuning.stormFoam, 1.0f) - 1.0f) * g_storm;
            const float thr  = std::clamp(g_tuning.foamThreshold / stormFoam, 0.0f, 0.99f);
            const float invThr = 1.0f / std::max(1.0f - thr, 0.01f);

            // THE FOLD IS READ AGAINST WHAT THE CLAMP STILL ALLOWS, not against the geometric fold
            // at one. The cusp clamp above holds this field's displacement at a fraction of the
            // cusp -- the vertex stage's worst-case gain and the other cascades' share are divided
            // into its bound -- so the raw 1 - det of the drawn field tops out well short of one
            // BY CONSTRUCTION. Read absolutely, the threshold contradicted the clamp: the deepest
            // fold the geometry was ever permitted sat UNDER the default threshold, and open water
            // could not whiten at any chop setting -- a knob that promised foam and a bound that
            // quietly forbade it. Normalised by the reach, the slider means what it says again:
            // how far into the fold this layer can actually produce.
            const float foldReach = std::max(cs.maxGather * kTwoPi
                                    / std::sqrt(std::max(cs.bandLong * cs.bandShort, 1e-3f)), 1e-3f);

            // AND IT OUTLIVES THE FOLD THAT MADE IT. Foam recomputed from scratch every frame
            // appears and disappears with the wave, which reads as a flicker; real foam lingers and
            // thins, and that lag is most of what makes a breaking sea legible -- you see where it
            // broke, not only where it is breaking. Decayed on elapsed seconds so the look does not
            // depend on the frame rate.
            const float keep = (dt > 0.0f)
                                   ? std::exp(-dt * 4.0f / std::max(g_tuning.foamLife, 0.05f))
                                   : 1.0f;

            for (int r = 0; r < kOceanN; ++r)
            {
                const int rm = (r + kOceanN - 1) % kOceanN;
                const int rp = (r + 1) % kOceanN;
                for (int c = 0; c < kOceanN; ++c)
                {
                    const int cm = (c + kOceanN - 1) % kOceanN;
                    const int cp = (c + 1) % kOceanN;
                    const int i = r * kOceanN + c;

                    const float gx = (cs.height[r * kOceanN + cp] - cs.height[r * kOceanN + cm]) * inv2;
                    const float gy = (cs.height[rp * kOceanN + c] - cs.height[rm * kOceanN + c]) * inv2;

                    // Jacobian of the horizontal map, by central difference on a grid that already
                    // exists. Four subtractions against the two extra transforms the analytic form
                    // would have cost, for a difference nothing can see at this cell size.
                    const float dxx = (cs.disp[(r * kOceanN + cp) * 4 + 0]
                                     - cs.disp[(r * kOceanN + cm) * 4 + 0]) * inv2;
                    const float dyy = (cs.disp[(rp * kOceanN + c) * 4 + 1]
                                     - cs.disp[(rm * kOceanN + c) * 4 + 1]) * inv2;
                    const float dxy = (cs.disp[(rp * kOceanN + c) * 4 + 0]
                                     - cs.disp[(rm * kOceanN + c) * 4 + 0]) * inv2;
                    const float dyx = (cs.disp[(r * kOceanN + cp) * 4 + 1]
                                     - cs.disp[(r * kOceanN + cm) * 4 + 1]) * inv2;

                    // Chop is already inside the displacement (baked at the transform above), so
                    // the fold measured here is the fold of the displacement actually drawn -- the
                    // one place all three consumers of this field finally agree.
                    const float det = (1.0f + dxx) * (1.0f + dyy) - dxy * dyx;
                    const float fold = std::clamp((1.0f - det) / foldReach, 0.0f, 1.0f);
                    const float birth = std::clamp((fold - thr) * invThr, 0.0f, 1.0f);

                    cs.foam[i] = std::max(birth, cs.foam[i] * keep);

                    // Split at the source into what each stage reads. The slope goes to the SHADING
                    // map, not the geometry: displacing the surface and lighting it with the trains'
                    // normal alone moves the water without moving the light, which reads as the
                    // swell being bent out of shape rather than as chop standing on it.
                    float* geo = cs.geo + i * 4;
                    geo[0] = cs.height[i];
                    geo[1] = cs.disp[i * 4 + 0];
                    geo[2] = cs.disp[i * 4 + 1];

                    // THE SLOPE OF THE SURFACE BEING DRAWN, not of the height field alone -- the
                    // same correction the trains' shader has always applied to its own normal,
                    // arrived at late here. The pixel stage lights the DISPLACED surface, and
                    // under the sideways gather the height's plain gradient is the slope of a
                    // surface nobody is looking at: too shallow exactly on the pinched face of a
                    // crest, so every wave was lit as the round hump the gather had just stopped
                    // it being -- and the harder the chop worked, the rounder the light said it
                    // was, which is why no spectrum shaping could reach this from its end. The
                    // map x -> x + D(x) re-slopes the height by its Jacobian, and the division by
                    // the determinant is what steepens the lighting where the water is gathered;
                    // the floor on the determinant keeps a near-fold bright and finite instead of
                    // flipped.
                    const float invDet = 1.0f / std::max(det, 0.30f);
                    float* shade = cs.shade + i * 4;
                    shade[0] = (gx * (1.0f + dyy) - dyx * gy) * invDet;
                    shade[1] = (gy * (1.0f + dxx) - dxy * gx) * invDet;
                }
            }

            // Spread over its neighbours before it ships. The fetch is unfiltered -- D3D9 offers no
            // filtering on a vertex sample -- so a texel edge is a hard edge in the mask, and at a
            // patch of two hundred units that edge is three metres of coastline-straight white.
            // Smoothing here costs one pass over a grid in cache and removes the only thing about
            // this signal that still looked square.
            for (int r = 0; r < kOceanN; ++r)
            {
                const int rm = (r + kOceanN - 1) % kOceanN;
                const int rp = (r + 1) % kOceanN;
                for (int c = 0; c < kOceanN; ++c)
                {
                    const int cm = (c + kOceanN - 1) % kOceanN;
                    const int cp = (c + 1) % kOceanN;

                    const float sum =
                        cs.foam[rm * kOceanN + cm] + 2.0f * cs.foam[rm * kOceanN + c] + cs.foam[rm * kOceanN + cp] +
                 2.0f * cs.foam[r  * kOceanN + cm] + 4.0f * cs.foam[r  * kOceanN + c] + 2.0f * cs.foam[r * kOceanN + cp] +
                        cs.foam[rp * kOceanN + cm] + 2.0f * cs.foam[rp * kOceanN + c] + cs.foam[rp * kOceanN + cp];

                    cs.shade[(r * kOceanN + c) * 4 + 2] = sum * (1.0f / 16.0f);
                }
            }
        }
    }

    const float* OceanGeoMap(int cascade)
    {
        if (cascade < 0 || cascade >= kOceanCascades) return nullptr;
        return g_cascade[cascade].geo;
    }

    float OceanMaxRise() { return g_maxRise; }

    const float* OceanShadeMap(int cascade)
    {
        if (cascade < 0 || cascade >= kOceanCascades) return nullptr;
        return g_cascade[cascade].shade;
    }

    float OceanPatch(int cascade)
    {
        if (cascade < 0 || cascade >= kOceanCascades) return 1.0f;
        return g_cascade[cascade].patch;
    }

    OceanTuning& OceanSettings() { return g_tuning; }

    void OceanStorm(float intensity)
    {
        g_storm = std::clamp(intensity, 0.0f, 1.0f);
    }

    const OceanCascadeReport& OceanReport(int cascade)
    {
        static const OceanCascadeReport kEmpty;
        if (cascade < 0 || cascade >= kOceanCascades) return kEmpty;
        return g_cascade[cascade].report;
    }
}
