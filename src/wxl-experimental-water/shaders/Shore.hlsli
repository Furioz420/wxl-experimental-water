// What the bottom does to the sea above it, GPU side.
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

// THIS FILE AND Shore.hpp DECIDE THE SAME THING. One is what is drawn, the other is what a hull
// floats on, and they were previously two different rules -- a ramp of 24 units on the query side
// against whatever the panel said on this one, with no shoaling gain on the query side at all. A
// boat therefore sat beside its own wave in the shallows, and nothing anywhere said so.
//
// Changing the arithmetic here without changing ShoreAt() there brings that fault back. The
// reasoning for every term lives in Shore.hpp; this is the same ladder, spelled for HLSL.

/// x: dead zone start.  y: dead zone end.  z: depth the bottom stops being felt.
/// w: breaker index divided by the sea's total rise -- the cap as a scale per unit of depth,
///    folded on the CPU so this stage does not need the sea's own height.
float4 gShoreA : register(c67);
/// x: shoaling rise.  y: pinch rise.  z: dead zone end for the DETAIL field, which goes first.
/// w: extra foam through the breaking band.
float4 gShoreB : register(c68);

/// The column reported where nothing measured one. Deep, and it must stay in step with the lattice's
/// own fallback for a terrain query that could not answer -- see Lattice.cpp.
#define kUnmeasuredDepth 1000.0

struct ShoreResponse
{
    float height;
    float pinch;
    float detail;
    float surf;
};

/// @param depth        water column here, world units.
/// @param pinchCeiling what the trains' shared cusp budget still allows.
ShoreResponse ShoreAt(float depth, float pinchCeiling)
{
    ShoreResponse r;

    const float live = smoothstep(gShoreA.x, gShoreA.y, depth);
    const float feel = 1.0 - smoothstep(gShoreA.y, gShoreA.z, depth);

    const float rise = 1.0 + gShoreB.x * 4.0 * feel * (1.0 - feel);

    // A ceiling, not a multiplier: in deep water it is never reached and the open sea pays nothing.
    const float cap = gShoreA.w * depth;

    const float wanted = live * rise;
    r.height = min(wanted, cap);
    r.pinch  = live * min(1.0 + gShoreB.y * feel, pinchCeiling);
    r.detail = min(smoothstep(gShoreA.x, gShoreB.z, depth), cap);

    // HOW HARD THE CAP IS BITING -- and it is only a question at all where a wave was wanted.
    //
    // Guarding the divide with max() alone was not the same statement as the CPU's `if (wanted > 0)`,
    // and the difference was the whole of the white sheet over the shallows: with wanted and cap both
    // zero the expression reads 1.0 - 0.0/1e-3, which is ONE. Full surf, at exactly the depths where
    // there is no wave to break -- and, worse, at every vertex whose column was never measured, which
    // the vertex stage reads as a column of zero.
    const float wave = step(1e-3, wanted);
    r.surf = wave * saturate(1.0 - cap / max(wanted, 1e-3));

    return r;
}
