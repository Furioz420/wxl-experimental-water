// One switch per contribution, in one registry.
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

// EVERY CONTRIBUTION HAS A SWITCH, AND EVERY SWITCH IS THE SAME KIND OF THING.
//
// This replaces two separate collections of booleans that had grown side by side -- layer gates for
// the sea, effect toggles for the shading -- and were diverging: different naming, different
// persistence rules, and no way to enumerate them. Anything that wanted to list what can be
// turned off -- the panel, a shared profile -- had to know both by hand.
//
// A SWITCH IS ALWAYS THE SAME MECHANISM: it forces one uploaded constant to its NEUTRAL value. Not a
// shader permutation, not a branch in the pixel stage, not a skipped bind -- a zero where a strength
// was. That is what makes them free, composable in any combination, and impossible to get subtly
// wrong: with the constant at neutral, the term is still computed and still adds nothing.
//
// The neutral value is the interesting part of each one, and it is not always zero -- a foam
// threshold is disabled by being put out of reach, not by being zeroed, because zero is the value at
// which EVERYTHING breaks. Each is stated at the point it is applied.
namespace wxl::water
{
    enum class Sw : int
    {
        // --- the sea's own layers ---------------------------------------------------------------
        DetailGeometry,  ///< the detail field reaching the vertices at all
        DetailChop,      ///< its sideways gather; off leaves the field vertical-only
        Nonlinear,       ///< crest peaking and the height-driven gather
        OffshoreFoam,    ///< the whitecap signal the field feeds the shading
        Shoaling,        ///< waves answering the bottom as it comes up

        // --- the shading maps --------------------------------------------------------------------
        ShadeMaps,       ///< the synthesized maps as a whole; off is the geometry-only sea
        AnimNormals,
        Fbm,
        FoamTex,
        Sky,
        Glitter,
        Sss,
        Caustics,

        // --- the surface's own terms -------------------------------------------------------------
        Ripple,          ///< the material's ripple normals
        TroughShade,     ///< troughs darkened by their own depth in the wave
        NightFloor,      ///< the floor under daylight, so night is dark rather than black
        Bottom,          ///< the refracted sea floor showing through
        Refraction,      ///< the scene copy behind the water
        Extinction,      ///< colour lost with path length -- off is water with no absorption

        // --- foam ---------------------------------------------------------------------------------
        Foam,            ///< all of it, at the last multiply
        ShoreFoam,       ///< the band along a shallowing bottom
        CrestFoam,       ///< whitecaps born from the fold

        // --- how the field is READ, as opposed to what is in it ----------------------------------
        DetailFilter,    ///< the vertex stage's tent over the field's grid; off is the raw box

        // --- the shore ---------------------------------------------------------------------------
        /// The wave height ceiling set by the water under it. Off, a swell runs onto a beach at its
        /// open-sea height. The dead zone has no switch: nothing may put a crest through a beach.
        DepthLimit,

        Count
    };

    /// Whether a switch is on. Everything defaults on: a fresh install shows the whole sea.
    bool On(Sw s);
    void SetOn(Sw s, bool on);

    /// The bool itself, for a checkbox to bind to.
    bool* Slot(Sw s);

    /// Stable identifier -- what a saved profile stores. Never localised and never changed once
    /// shipped: a profile written by one build has to mean the same thing in the next.
    const char* SwitchId(Sw s);

    /// What the panel shows, in the panel's current language.
    const char* SwitchLabel(Sw s);
    const char* SwitchHelp(Sw s);

    /// Turns them all back on. What a panel button needs after an isolation session, because
    /// finding the one switch left off by hand is exactly the bug this makes easy to create.
    void AllOn();
}
