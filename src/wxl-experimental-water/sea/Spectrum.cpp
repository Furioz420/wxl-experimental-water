// Builds the wave spectrum from the sea state.
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

#include "wxl-experimental-water/sea/Spectrum.hpp"

#include <algorithm>
#include <cmath>

namespace wxl::water
{
    namespace
    {
        Vec2 Rotate(Vec2 v, float radians)
        {
            const float c = std::cos(radians);
            const float s = std::sin(radians);
            return Vec2{ v.x * c - v.y * s, v.x * s + v.y * c };
        }

        Vec2 Normalise(Vec2 v)
        {
            const float len = std::sqrt(v.x * v.x + v.y * v.y);
            return (len > 1e-6f) ? Vec2{ v.x / len, v.y / len } : Vec2{ 1.0f, 0.0f };
        }

        /// Phillips envelope, evaluated for one wavenumber.
        ///
        /// The exp(-1/(kL)^2) term is what makes wind speed set the wavelength rather than just the
        /// height: waves longer than the wind can raise are suppressed, so the peak of the spectrum
        /// moves as the weather changes.
        float PhillipsAmplitude(float k, float windSpeed, float alignment)
        {
            if (k <= 1e-6f)
                return 0.0f;

            const float L = (windSpeed * windSpeed) / kGravity;   // longest wave this wind sustains
            const float kL = k * L;
            const float suppression = std::exp(-1.0f / std::max(kL * kL, 1e-6f));

            // Directional term: waves square-on to the wind survive, waves across it do not.
            const float directional = alignment * alignment;

            // The k^-4 falloff is the shape of the spectrum -- but it is a density PER UNIT of
            // wavenumber, and the ladder samples it geometrically. Each sample therefore stands for a
            // band whose width grows with k, so it has to carry k times the density or the long end
            // is credited with the short end's energy as well.
            //
            // Skipping that factor leaves amplitude going as k^-2: over a ladder spanning a factor of
            // five in wavelength the longest train ends up twenty-five times the shortest, takes
            // essentially the whole normalised height, and the sea becomes one wave with a texture on
            // it -- parallel ridges, which is what a single dominant train looks like.
            const float energy = suppression * directional / (k * k * k);
            return std::sqrt(std::max(energy, 0.0f));
        }
    }

    WaveSpectrum BuildSpectrum(const SeaConditions& conditions)
    {
        WaveSpectrum out;
        out.count = kMaxComponents;

        const Vec2  wind = Normalise(conditions.windDir);
        const float spreadRad = conditions.directionSpread * 0.017453293f;
        const float exposure = std::clamp(conditions.exposure, 0.0f, 1.0f);

        // Start from the wavelength this wind actually peaks at, so the sea is built around the
        // weather rather than around a fixed authored length the weather then scales.
        // THE LADDER'S SPAN IS WHAT MAKES WAVES MEET, and it matters more than how many rungs it has.
        //
        // Two trains whose wavenumbers are close do not cross -- they beat. Their sum has an envelope
        // that travels at the group speed of the difference, so over any patch you can actually see,
        // it behaves as one wave slowly swelling and fading. That is the same failure as putting two
        // directions on one wavelength, merely softened by however far apart the two lengths are.
        //
        // Which makes the span the governing number. A dozen rungs inside a factor of three sit nine
        // per cent apart: an envelope eleven wavelengths long, far larger than anything on screen, and
        // a sea that reads as ranks. The same dozen rungs across a factor of six separate by a sixth,
        // the beats fall inside the frame, and crests visibly run into each other and break up.
        // Reaching further down costs nothing but mesh; reaching further up is the swell's job.
        const float peakLength = std::max((conditions.windSpeed * conditions.windSpeed) / kGravity, 8.0f);
        const float longest  = peakLength * std::max(conditions.crestSpacing, 1.2f);
        // The floor's own floor sits at ten units: below that a train is saying something the
        // detail field's continuum says better, for a block of vertex arithmetic per vertex.
        const float shortest = std::clamp(conditions.minWavelength, 10.0f, longest * 0.5f);

        // NO TWO COMPONENTS MAY SHARE A WAVELENGTH.
        //
        // This is the rule the sea's whole character rests on. Two plane waves of the SAME wavelength
        // at different angles do not interfere into anything irregular -- they sum exactly to
        //
        //     2 cos((k1-k2).x / 2) . cos((k1+k2).x / 2 - wt)
        //
        // which is one wave travelling along the mean direction, multiplied by an envelope that does
        // not move at all. The crests stay rigidly straight and perpendicular to that mean; only
        // their height varies along their length. Grouping several directions inside one wavelength
        // band therefore builds exactly the corrugation it was meant to break, and lays a standing
        // lattice over it.
        //
        // So every component gets its own wavelength AND its own direction. Interference between
        // unequal wavelengths has no stationary envelope: the pattern travels, crests bend, join and
        // terminate, and that is what reads as open water.
        //
        // THE SEA IS SEVERAL SEAS AT ONCE, AND THAT IS WHERE ITS CHAOS LIVES.
        //
        // Everything derived from the local wind is the wind sea. Under it run the swells, each raised
        // by a storm days ago and hundreds of miles off and travelling ever since -- which is why a
        // calm can still be rolling. They cannot come out of the wind spectrum, whose envelope exists
        // precisely to suppress the lengths this wind cannot sustain.
        //
        // AND THEY DO NOT COME FROM THE WIND'S DIRECTION EITHER. That was the mistake: the swell held
        // the largest waves in the sea and was pinned to within a few degrees of the wind, so the only
        // components big enough to be looked at were the only ones forbidden to cross. Whatever the
        // wind sea did underneath, the sea read as ranks, because the ranks were the swell.
        //
        // A storm that happened elsewhere sends its waves from wherever it was. Two of them, from two
        // storms, arrive at an angle to each other and to today's wind, and where they meet the crests
        // pile into short-lived peaks and cancel into holes a moment later. That crossing IS the chaos
        // of open water -- not detail, not noise, but two large systems disagreeing.
        //
        // So there are two swell systems, each with its own axis, each holding two lengths.
        constexpr int kSwellTrains  = 4;
        constexpr int kSystemTrains = 2;

        const float swellBase = std::max(conditions.swellLength, longest);
        // Four distinct lengths, INTERLEAVED between the two systems rather than stacked: A takes the
        // first and third, B the second and fourth. Giving A both long ones and B both short ones
        // would make the crossing a matter of scale -- a big swell with a smaller one laid over it,
        // which the eye separates and reads as swell plus detail. Interleaved, the two systems are
        // comparable, and neither is the sea while the other is texture on it. Within a system the
        // two lengths still sit a third apart, so they do not beat with each other either.
        const float swellLen[kSwellTrains] = { swellBase,         swellBase * 0.63f,
                                               swellBase * 0.81f, swellBase * 0.47f };
        // A carries slightly more than B: two swells of equal weight give a symmetric lattice, and
        // symmetry is the one thing that never happens at sea.
        const float swellWeight[kSwellTrains] = { 1.00f, 0.66f, 0.74f, 0.45f };

        // The axes, seeded so they are the same on every client, and separated by construction so a
        // draw can never quietly place both systems on the same heading.
        //
        // Scaled by the spread, which is what ties this to the weather: in a calm the spread is wide
        // and the two swells arrive from genuinely different quarters, crossing everything. In a gale
        // it narrows, the axes fold back onto the wind, and the sea combs itself into ranks -- which is
        // what a gale actually looks like, and the one time ranks are correct.
        const float uA  = 2.0f * SeededUnit(conditions.seed, 811) - 1.0f;
        const float sep = 0.9f + 0.6f * SeededUnit(conditions.seed, 1447);
        const float uB  = std::clamp(uA + ((uA >= 0.0f) ? -sep : sep), -1.0f, 1.0f);

        // Deliberately short of the full spread. At the extremes of the wide calm setting the two
        // systems would end up head-on, and two swells running at each other with similar lengths
        // stand rather than travel. Two thirds keeps them between roughly a right angle and a broad
        // oblique -- a crossing sea, which is what this is for.
        const float axisSpread = spreadRad * 0.65f;
        const Vec2  swellAxis[2] = { Rotate(wind, axisSpread * uA),
                                     Rotate(wind, axisSpread * uB) };

        // There is no third system past these two. Short near-cusp trains used to sit at the end of
        // the table to give the sea wave shapes; shapes are the detail field's job now, and the
        // vertex arithmetic those trains cost is what bought the field its extra cascades.
        const float swellShare = std::clamp(conditions.swellShare, 0.0f, 1.0f);
        const int   windTrains = out.count - kSwellTrains;
        const float windRatio  = std::pow(shortest / longest,
                                          1.0f / static_cast<float>(windTrains - 1));

        // Golden-ratio stepping rather than a seeded draw per train. A draw clusters, and a fan laid
        // out in index order makes direction a smooth function of wavelength -- an ordering the eye
        // finds as readily as a grid. This covers the spread evenly while never placing neighbouring
        // wavelengths at neighbouring angles.
        const float seedFrac = SeededUnit(conditions.seed, 977);

        float rawAmplitudes[kMaxComponents] = {};
        float swellTotal = 0.0f;
        float windTotal = 0.0f;

        for (int i = 0; i < out.count; ++i)
        {
            WaveSpectrum::Train& t = out.trains[i];
            const bool isSwell = (i < kSwellTrains);

            Vec2  axis;         // heading this train fans out around
            float wavelength;
            float width;        // share of the full spread this train may be thrown across
            if (isSwell)
            {
                axis       = swellAxis[i / kSystemTrains];
                wavelength = swellLen[i];
                // Swell holds its line far better than wind chop -- it has travelled too far not to.
                // Its disorder comes from the angle between the two systems, not from fanning either.
                width = 0.18f;
            }
            else
            {
                const int   j = i - kSwellTrains;
                const float shortness = static_cast<float>(j) / static_cast<float>(windTrains - 1);

                // Knocked off the exact geometric rung. A clean ladder makes every difference
                // frequency a power of the same ratio, so the beats between the trains are themselves
                // laid out in a self-similar progression -- and a progression is a pattern, which is
                // what turns up at distance once the near detail has faded out of resolution.
                const float jitter = 1.0f + 0.10f * (SeededUnit(conditions.seed, 2000 + j) - 0.5f);
                wavelength = longest * std::pow(windRatio, static_cast<float>(j)) * jitter;

                axis = wind;
                // Never closes: the trains carrying the height have to be off the wind line too, or
                // the loudest half of the spectrum runs in lockstep whatever the rest does. With
                // only four wind rungs, this fan and the ladder's span are the whole defence
                // against ranks -- which is why both are wide.
                width = 0.55f + 0.45f * shortness;
            }

            float turn = 0.6180339887f * static_cast<float>(i) + seedFrac;
            turn -= std::floor(turn);
            const float u = 2.0f * turn - 1.0f;

            t.dir   = Rotate(axis, spreadRad * width * u);
            t.k     = kTwoPi / wavelength;
            t.phase = SeededUnit(conditions.seed, i) * kTwoPi;
            // Held as a raw weight and normalised below; the weight is the train's own amplitude.
            t.steepness = 0.0f;

            const float alignment = t.dir.x * wind.x + t.dir.y * wind.y;

            // The swell takes a flat weight: it is not this wind's doing and has no business being
            // scored by this wind's envelope.
            if (isSwell)
            {
                rawAmplitudes[i] = swellWeight[i];
                swellTotal += rawAmplitudes[i];
            }
            else
            {
                rawAmplitudes[i] = PhillipsAmplitude(t.k, std::max(conditions.windSpeed, 0.5f),
                                                     alignment);
                windTotal += rawAmplitudes[i];
            }
        }

        // Normalised in TWO pools, so each system keeps its share of the height whatever the other
        // is doing. One pool would let a rising wind drown the swell simply by contributing more
        // terms, and the heave would come and go with the weather instead of underlying it.
        const float height = conditions.amplitude * exposure;
        const float swellNorm = (swellTotal > 1e-6f) ? (height * swellShare / swellTotal) : 0.0f;
        const float windNorm  = (windTotal  > 1e-6f)
                                    ? (height * (1.0f - swellShare) / windTotal) : 0.0f;

        for (int i = 0; i < out.count; ++i)
            out.trains[i].amplitude = rawAmplitudes[i] * ((i < kSwellTrains) ? swellNorm : windNorm);

        // WHERE THE PINCH GOES, which decides whether the sea has crests or humps.
        //
        // Q is a train's horizontal pinch, and Q = 1 is where a crest folds through itself. The whole
        // spectrum shares one budget, because the folds add -- give eight trains their own Q of 1
        // and the surface spends its life inside out.
        //
        // But HOW the budget is shared matters more than its size, and spreading it evenly is what
        // made this sea plastic. Sixteen near-equal shares leave the dominant train -- the one wave
        // actually being looked at -- with a Q of about three hundredths: a pure sine hump, as round
        // at the crest as at the trough. Meanwhile the pinch is spent on short trains carrying almost
        // no height, where nothing can be seen of it.
        //
        // So the budget follows the AMPLITUDE. The waves that are visible are the waves that get to
        // be sharp, and the invisible ones stop consuming the allowance.
        //
        // And it is summed IN QUADRATURE, not linearly. Linear addition assumes every train folds at
        // the same instant in the same place, which would need them to share a direction and a phase
        // -- the one thing the spectrum is built to prevent. Independent contributions combine as a
        // root of squares, so the honest bound leaves the dominant train several times the pinch and
        // the crests finally lean. Where they do overrun, the vertex stage measures the fold and the
        // surface throws foam, which is what a crest exceeding its own limit does anyway.
        float weightSq = 0.0f;
        for (int i = 0; i < out.count; ++i)
            weightSq += out.trains[i].amplitude * out.trains[i].amplitude;

        const float steepBudget = std::clamp(conditions.steepness, 0.0f, 1.0f);
        const float rms = std::sqrt(weightSq);
        const float steepScale = (rms > 1e-6f) ? (steepBudget / rms) : 0.0f;
        for (int i = 0; i < out.count; ++i)
        {
            float q = out.trains[i].amplitude * steepScale;

            // AND EVERY TRAIN IS HELD TO A CIRCULAR ORBIT. A train's horizontal sway is Q/k and
            // does not depend on its amplitude, while in deep water the particles go round in
            // circles: the sideways radius cannot exceed the vertical one, which is Q <= kA. Past
            // it the orbit lies flat and the train shears the surface sideways without lifting it
            // -- on crests hundreds of units long that reads as a slow serpentine meander running
            // along them, a bug this sea has produced once already. The steepness slider is a
            // ceiling the physics may refuse; more height or less wavelength is what actually
            // steepens a wave.
            const float kA = out.trains[i].k * out.trains[i].amplitude;
            if (q > kA) q = kA;
            out.trains[i].steepness = (q < 1.0f) ? q : 1.0f;
        }

        return out;
    }
}
