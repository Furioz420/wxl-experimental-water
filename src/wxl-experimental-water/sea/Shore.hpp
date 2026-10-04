// What the bottom does to the sea above it. Must stay identical to Shore.hlsli -- see the warning.
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

// ONE SEA THAT ARRIVES, NOT TWO SEAS THAT MEET.
//
// The thing being asked for here -- big water offshore, small water piling onto a beach, and no line
// anywhere between them -- is not a blend of two settings. Blending two seas is exactly what makes a
// visible seam, because the two have different wavelengths and the crossfade shows them sliding
// through each other.
//
// It is ONE sea, limited. A wave cannot stand taller than the water it is standing in: past about
// three quarters of the local depth the crest has no water left to draw on and the wave breaks. That
// single rule -- the breaker index -- is the whole transformation, and it is continuous by
// construction because the depth is. The same six-unit swell passes untouched through forty units of
// water, is cut to three in four units, and to nothing at the waterline. Nothing is faded into
// anything; the wave that arrives at the beach IS the wave that left the open sea.
//
// Three more terms shape the approach, and each is a real thing the water does:
//
//   THE RISE. A wave slows as it feels the bottom, the energy behind it catches up, and it GAINS
//   height before it loses any -- Green's law. So the approach is a hump: nothing in deep water,
//   nothing in the surf where there is no room left, a gain in between.
//
//   THE PINCH. Far more visible than the height, and it keeps climbing while the cap is taking the
//   height away. That combination is what "small waves smacking onto the beach" actually looks like:
//   not small round humps, but short steep ones leaning forward over a flat trough.
//
//   THE DEAD ZONE. The last stretch to the waterline, where everything is released to flat. Its own
//   band rather than a single depth, because a wave that reaches zero at a hard number leaves a
//   crease running along that contour -- a straight line across water with no reason to contain one.
//
// AND THE SHORT WAVES GO FIRST. Chop feels a bottom the swell still passes over, so the detail field
// is given its own, earlier band. Letting both die on the same ramp is what makes a shoreline read
// as the whole sea being turned down by one knob.
namespace wxl::water
{
    /// The depth ladder. Every number is a DEPTH in world units or a pure ratio, so a profile means
    /// the same thing on a map with a different vertical scale.
    struct ShoreProfile
    {
        /// ONE MULTIPLIER OVER ALL THREE DEPTHS BELOW, and the reason it exists is that the three
        /// were unusable without it.
        ///
        /// They are the SHAPE of the approach -- where the release begins against where it ends,
        /// how far out the bottom is first felt -- and that shape is a judgement made once. What
        /// changes from map to map, and what somebody actually stands on a beach wanting to move, is
        /// HOW FAR OUT the whole thing happens. Expressed as three absolute depths, that one
        /// intention meant dragging three coupled numbers in the same direction by the same ratio
        /// and hoping, on a quantity that cannot be seen from inside the game.
        ///
        /// So this is the knob for the common case and the three below are the shape it scales.
        /// Above one pushes the entire approach further out to sea; below one pulls it in tight
        /// against the beach.
        float scale = 1.0f;

        /// Depth at which the sea is completely flat. Under this there is no wave at all.
        ///
        /// Not zero: a crest of any height at the waterline cuts through the beach behind it, and
        /// the terrain has no notion that the water in front of it has moved.
        float deadStart = 0.35f;

        /// Depth at which the dead zone has finished letting go -- full wave height above this,
        /// as far as this term is concerned.
        ///
        /// THE BAND IS THE POINT, and it is what the single depth this replaced could not express.
        /// The old control put the fade and its end on one number, so making the shallows calmer
        /// also pushed the fade far out to sea. These two move independently: a narrow band high up
        /// is a sea that stays big until it suddenly stops, a wide band is a long gentle release.
        float deadFull = 2.25f;

        /// Depth at which the bottom stops being felt at all. Above this the sea is open water and
        /// nothing in this file touches it.
        ///
        /// Far out on purpose. Shoaling that begins where the beach begins reads as a strip of
        /// different water rather than as an approach; real swell starts feeling a shelf at about
        /// half its own wavelength, which is tens of units out.
        float feelDepth = 18.0f;

        /// How tall a wave may stand relative to the water under it, before it breaks.
        ///
        /// THE ONE NUMBER THE WHOLE TRANSFORMATION HANGS ON. About 0.78 is the textbook solitary
        /// wave limit and looks right; lower breaks the sea further out and leaves a wider, calmer
        /// surf; higher lets big water run closer in before it is cut.
        ///
        /// Applied against the TOTAL rise -- trains and detail field together -- because what has to
        /// fit inside the depth is the surface, not one layer of it.
        float breakerIndex = 0.65f;

        /// How much a wave gains on the approach, at the peak of the hump. Zero is a wave that only
        /// ever loses height, which reads as the sea being switched off towards the shore.
        float shoalRise = 0.12f;

        /// How much steeper a wave gets as it stands up, at the shallow end.
        ///
        /// Bounded by the trains' shared cusp budget where it is applied, so raising it cannot turn
        /// the surface inside out -- it stops climbing instead.
        float pinchRise = 0.65f;

        /// How much earlier the short chop dies than the swell, as a multiple of the dead band.
        ///
        /// One means they die together, which is the behaviour this replaced and which makes a
        /// shoreline look like one master volume being turned down. Above one the detail field is
        /// gone while the swell is still running in, so what reaches the beach is a clean swell
        /// rather than a swell carrying chop it should have lost a hundred units ago.
        float chopLead = 1.5f;

        /// How much extra foam the breaking band gets, over what the fold already produces.
        ///
        /// Its own term because the fold measures the surface's own geometry, and a wave held down
        /// by the depth cap has had exactly the geometry that would have folded taken away from it.
        /// Without this the surf is the one place in the scene with no white in it.
        float surfFoam = 0.55f;
    };

    /// What the sea is allowed to do at one depth.
    struct ShoreResponse
    {
        float height = 1.0f;   ///< scale on the trains' amplitude
        float pinch  = 1.0f;   ///< scale on their pinch
        float detail = 1.0f;   ///< scale on the detail field
        float surf   = 0.0f;   ///< 0 offshore, 1 where the depth cap is hard at work
    };

    /// Smoothstep, spelled out because both sides of this file have to agree bit for bit and the
    /// two languages do not share a library.
    inline float ShoreStep(float lo, float hi, float x)
    {
        const float d = hi - lo;
        float t = (d > 1e-4f) ? ((x - lo) / d) : ((x >= hi) ? 1.0f : 0.0f);
        t = (t < 0.0f) ? 0.0f : ((t > 1.0f) ? 1.0f : t);
        return t * t * (3.0f - 2.0f * t);
    }

    /// @param depth      water column at this point, world units.
    /// @param totalRise  the largest rise the sea can currently reach, trains and field together.
    ///                   The cap is a statement about the surface, so it is measured against all of
    ///                   it -- capping one layer against its own height would let the two sum past
    ///                   the depth they are both supposed to fit inside.
    /// @param pinchCeiling  what the trains' shared cusp budget still allows.
    inline ShoreResponse ShoreAt(const ShoreProfile& p, float depth, float totalRise,
                                 float pinchCeiling)
    {
        ShoreResponse r;

        // The three depths are a SHAPE and the scale is where that shape sits. Applied here, once,
        // so nothing downstream has to remember to do it -- and floored, because a scale of zero
        // collapses the whole ladder to a single depth and makes the sea vanish at a step.
        const float sc = (p.scale > 0.01f) ? p.scale : 0.01f;
        const float deadStart = p.deadStart * sc;
        const float deadFull  = p.deadFull * sc;
        const float feelDepth = p.feelDepth * sc;

        // THE DEAD ZONE, and it is the only term here that is allowed to reach zero.
        const float live = ShoreStep(deadStart, deadFull, depth);

        // How hard the bottom is felt: one at the dead zone's top, nothing in open water. Every
        // shaping term below rides this, so they all arrive and leave together and none of them can
        // be left switched on in the middle of the ocean by a number set somewhere else.
        const float feel = 1.0f - ShoreStep(deadFull, feelDepth, depth);

        // THE RISE. A hump peaking mid-approach: 4t(1-t) is one at the middle and zero at both
        // ends, so deep water gains nothing and the surf -- which has no room -- gains nothing.
        const float rise = 1.0f + p.shoalRise * 4.0f * feel * (1.0f - feel);

        // THE BREAKER CAP. Everything above is what the wave WANTS; this is what the water under it
        // will allow. A ceiling rather than a multiplier, so in deep water it is simply never
        // reached and costs the open sea nothing at all.
        const float cap = (totalRise > 1e-3f) ? (p.breakerIndex * depth / totalRise) : 1e9f;

        const float wanted = live * rise;
        r.height = (wanted < cap) ? wanted : cap;

        // THE PINCH, still climbing while the cap takes the height away -- which is the entire
        // difference between a small wave and a breaking one. Held down through the dead zone too:
        // the pinch is a HORIZONTAL slide and does not need amplitude to be visible, so a pinch left
        // running on flat water slides the surface sideways over a beach that is not moving.
        const float wantPinch = 1.0f + p.pinchRise * feel;
        r.pinch = live * ((wantPinch < pinchCeiling) ? wantPinch : pinchCeiling);

        // THE SHORT WAVES GO FIRST, on their own band, and capped by the same depth: chop standing
        // taller than the water it is in is the same fault as a swell doing it.
        const float chopLive = ShoreStep(deadStart,
                                         deadStart + (deadFull - deadStart) * p.chopLead, depth);
        r.detail = (chopLive < cap) ? chopLive : cap;

        // HOW HARD THE CAP IS BITING, which is the definition of breaking and therefore of where
        // foam belongs. Zero wherever the wave got what it asked for.
        if (wanted > 1e-3f)
        {
            const float held = 1.0f - cap / wanted;
            r.surf = (held < 0.0f) ? 0.0f : ((held > 1.0f) ? 1.0f : held);
        }
        return r;
    }

    /// The live profile. Function-local static so this stays header-only: the shore is read from the
    /// vertex upload, from the panel and from the buoyancy query, and a separate translation unit
    /// for one struct would be three includes to save nothing.
    inline ShoreProfile& ShoreSettings()
    {
        static ShoreProfile p;
        return p;
    }
}
