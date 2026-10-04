// The live sea: one spectrum, one prepared frame, and the queries everything else asks of it.
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

#include "wxl-experimental-water/sea/Query.hpp"
#include "wxl-experimental-water/sea/Spectrum.hpp"

// The single instance of the sea, and the only place a frame's wave table is built. Everything that
// wants to know about water -- the vertex stage through the uploaded constants, buoyancy, the camera
// submersion test -- reads it from here. Nothing else is allowed to hold a spectrum:
// two of them drift, and a sea that disagrees with itself is a boat floating beside its own wake.
namespace wxl::water
{
    /// How the weather is allowed to move the sea, and how slowly.
    ///
    /// Chop answers the wind within a minute; swell takes far longer to build and far longer to let
    /// go. The lag is the point -- it is what makes a storm read as something that arrived rather
    /// than as a value that changed, and it is the part of a sea no slider can imitate.
    struct SeaResponse
    {
        /// Seconds for the short trains to reach a new wind. Minutes for the long ones.
        float chopLag  = 45.0f;
        float swellLag = 420.0f;

        /// Sea conditions at the two ends of the weather range. The live state is a blend of these
        /// driven by storm intensity, so a single authored pair covers every weather in between.
        SeaConditions calm;
        SeaConditions storm;

        SeaResponse()
        {
            // Wind speed is read as a wavelength, not as a strength: L = V^2/g, so it is the number
            // that decides whether the sea is made of swell or of ripples. A value that looks
            // reasonable as a wind gives waves a few units long -- shorter than a boat, and shorter
            // than the mesh can draw. These are chosen from the wavelength backwards.
            // Amplitude is the SUM over the spectrum, so it is the sea's full excursion about the
            // still-water plane, in world units. A player stands about two units tall: the calm
            // baseline stays well below that scale, while weather is allowed to build a visible sea.
            // Steepness is the SUM of the pinches, so it is how close the whole sea sits to its cusp.
            // Low values are not "calmer", they are ROUNDER: sinusoidal humps with the trough as
            // broad as the crest, which is the plastic look. A real sea spends its time near the
            // cusp -- pointed crests over long flat troughs -- and only the height tells calm from
            // storm.
            //
            // SPREAD NARROWS AS IT BLOWS, it does not widen. A calm sea is a MIXTURE: old swells from
            // storms that happened days ago and hundreds of miles apart, arriving from unrelated
            // directions and crossing each other. That crossing is the chaos of open water.
            //
            // A gale is the opposite. One wind, held long enough, raises and then dominates every
            // system under it, and the sea combs itself into that wind's axis -- which is why a storm
            // reads as ranks of waves all running the same way, and why the wide setting belongs to
            // the calm end.
            // WHAT READS AS A BIG SEA IS THE RATIO OF HEIGHT TO LENGTH, never the height. A swell of
            // two units over a hundred and ninety is a one per cent grade: a faithful ocean swell,
            // and visually a flat sheet. Real water breaks around one in seven, and a stylised sea
            // like the one being aimed at sits near that -- roughly one in six, which is steeper than
            // anything the open ocean does and is exactly why it reads.
            //
            // So the swell is SHORTENED as much as it is raised. Lengthening it is what makes it
            // disappear.
            // THE TRAINS ARE THE SUPPORT AND NOTHING ELSE: eight long systems that carry a hull,
            // arrive from their own directions, and shoal against the bottom. The floor sits at
            // several TENS of units -- everything shorter is the detail field's continuum, and a
            // rung spent down there costs a block of vertex arithmetic to say something the field
            // says better, while packing the ladder into ranks. The high floor is also what keeps
            // eight components honest: the sum's quasi-periodic pattern still exists, but its
            // period is built from wavelengths no frame contains, so the eye never meets it.
            //
            // The shipped baseline has to work on Azeroth's lakes and rivers as well as the open
            // sea. Keep calm water below a player's height and leave the theatrical ocean values
            // reachable from the tuner; making those the baseline pushed the stock liquid boundary
            // visibly onto shore before the weather had even started.
            calm.windDir = Vec2{ 0.9703f, 0.2419f };   // heading 14 degrees
            calm.windSpeed = 7.0f;
            calm.amplitude = 0.85f;
            calm.steepness = 0.28f;
            calm.directionSpread = 55.0f;
            calm.minWavelength = 24.0f;
            calm.crestSpacing = 2.2f;
            calm.seed = 1587605891u;
            calm.swellLength = 150.0f;
            calm.swellShare = 0.32f;

            // Built UP from the calm end rather than authored beside it. The two were drifting apart
            // as the calm end was tuned, to the point where the storm was the smaller sea of the two
            // -- a gale that arrives and flattens the water, which nothing in the code would have
            // objected to because nothing in the code knows which is meant to be bigger.
            //
            // A gale lengthens the swell as well as raising it, but nowhere near enough to give the
            // height back: the ratio has to climb, or a storm is only a calm sea scaled up and reads
            // as one. Hence more height than length, and a spread that NARROWS -- one wind, held long
            // enough, combs every system under it onto its own axis.
            storm = calm;
            storm.windSpeed = 24.0f;
            storm.amplitude = 4.0f;
            storm.steepness = 0.72f;
            storm.directionSpread = 35.0f;
            storm.crestSpacing = 2.8f;
        }
    };

    /// Advances the sea to the current shared time and rebuilds the frame's wave table. Called once
    /// per frame, before anything reads the surface -- a query taken before this and one taken after
    /// would answer from two different instants of the same frame.
    void Tick();

    /// The wave table for this frame. Uploaded to the vertex stage and evaluated on the CPU from the
    /// same eight components, which is the invariant the whole design rests on.
    const WaveFrame& Frame();

    /// The spectrum the frame was built from.
    const WaveSpectrum& Spectrum();

    /// The conditions currently in force, after the weather lag.
    const SeaConditions& Conditions();

    /// The response curve, mutable so the tuning panel can drive it live.
    SeaResponse& Response();


    /// Drops the weather lag for one tick, so the next frame's sea IS the target rather than a step
    /// towards it.
    ///
    /// For tuning, not for weather. The lags exist so a squall arrives as water that grows -- the
    /// swell's is seven minutes, because a swell was raised elsewhere days ago and cannot answer a
    /// gust. Correct there, and useless in front of somebody moving a slider: a change applied
    /// through it is indistinguishable from a control that does nothing.
    void SnapSea();

    /// Overrides the weather-driven state until Release() -- for a demo set piece, or to stage a
    /// storm without waiting for one.
    void ForceConditions(const SeaConditions& conditions);
    void ReleaseForcedConditions();

    /// Resolves a place against the sea. The plane comes from the map (MH2O); this adds the wave.
    /// See Query.hpp for which of the two heights a caller is entitled to.
    WaterSample SampleAt(const WaterPlane& plane, float x, float y);

    /// Where the camera sits relative to the water. Resolved once per frame and shared, because the
    /// surface shading, the volume composite and anything else that asks have to agree: two answers
    /// would put the medium on screen while the surface was still being shaded from outside it.
    struct EyeState
    {
        bool  hasPlane = false;
        bool  submerged = false;   ///< inside the water, with the waterline dead band applied
        float planeLevel = 0.0f;
        float depth = 0.0f;        ///< how far below the displaced surface, 0 when outside
    };

    /// Resolves the eye against the sea at a world position.
    void UpdateEye(const float* worldPos);
    const EyeState& Eye();

    /// Where the eye was this frame, in world XYZ. The lattice grades its refinement against it.
    const float* EyePosition();
}
