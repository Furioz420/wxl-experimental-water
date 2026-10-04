// The wave model on the GPU side. Must stay identical to Wave.hpp -- see the warning below.
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

// THIS FILE AND Wave.hpp EVALUATE THE SAME SEA. Neither derives a component; both read the table
// prepared once per frame and uploaded to the registers below. Changing the arithmetic here without
// changing HeightAt() there is a silent fault: the water a boat floats on stops being the water
// being drawn, and the symptom is a hull sitting beside its own wake with nothing in any log.

// Eight components, and the whole division of labour is in that number: the trains carry only what
// a hull answers to -- crossing swells and long wind waves -- and everything shorter is the detail
// field's. Halving the unrolled loop is what bought the field its cascades and the pixel stage its
// filtered normals. Must stay equal to kMaxComponents in Wave.hpp.
#define WXL_WAVE_COUNT 8

// Two float4 per component, laid out by WaveComponent:
//   [i*2 + 0] = (dir.x, dir.y, k, phase)
//   [i*2 + 1] = (amplitude, qa, 0, 0)
// Registers c48..c63: the client's own liquid block runs to c45, so this starts clear of it.
float4 gWave[WXL_WAVE_COUNT * 2] : register(c48);

struct WaveSurface
{
    float3 offset;    ///< displacement from the still-water point, world space
    float3 normal;    ///< unit surface normal, Z up
    float  fold;      ///< 0 where the surface is stretched, 1 where it is folding onto itself
};

/**
 * @brief Evaluates every component at one still-water point.
 * @param p       world XY of the point before displacement.
 * @param ampScale scale on the sea's HEIGHT here.
 * @param qScale   scale on the sea's PINCH here.
 *
 * THE TWO ARE NOT THE SAME NUMBER, and that is the whole character of shallow water. Scaling both by
 * the depth makes the shallows the calmest water in the scene, which is the opposite of every shore
 * anyone has stood on: a wave running onto a rising bottom slows down, the energy behind it catches
 * up, and it STANDS UP -- gaining height and, far more visibly, gaining pinch until the crest leans
 * over its own trough. Height eventually has to go, because there is no room for it; the pinch is
 * what has to arrive first.
 *
 * Neither touches the wavenumber, so a shoaling patch carries the same wavelengths as the sea beside
 * it rather than turning into different water along a line.
 */
WaveSurface EvaluateWaves(float2 p, float ampScale, float qScale)
{
    WaveSurface s;
    s.offset = float3(0.0, 0.0, 0.0);

    // Jacobian of the horizontal map, accumulated alongside the displacement. Its determinant is
    // below zero exactly where a crest has folded through itself -- which is the same place the real
    // thing breaks and throws foam, so the foam mask costs nothing beyond these three sums.
    float jxx = 0.0, jyy = 0.0, jxy = 0.0;
    float3 dzd = float3(0.0, 0.0, 0.0);   // (dZ/dx, dZ/dy, unused)

    [unroll]
    for (int i = 0; i < WXL_WAVE_COUNT; ++i)
    {
        float4 a = gWave[i * 2 + 0];      // dir.xy, k, phase
        float4 b = gWave[i * 2 + 1];      // amplitude, qa

        float2 dir = a.xy;
        float  k   = a.z;
        float  amp = b.x * ampScale;
        float  qa  = b.y * qScale;

        float theta = k * dot(dir, p) - a.w;
        float sn, cs;
        sincos(theta, sn, cs);

        s.offset.xy += dir * (qa * cs);
        s.offset.z  += amp * sn;

        // qa * k is the normalised steepness Q: the pinch expressed independently of wavelength,
        // which is what keeps the fold test meaningful across the whole spectrum at once.
        float q = qa * k * sn;
        jxx += dir.x * dir.x * q;
        jyy += dir.y * dir.y * q;
        jxy += dir.x * dir.y * q;

        float ka = amp * k * cs;
        dzd.xy += dir * ka;
    }

    // Analytic tangents. The horizontal derivative is not the identity here -- the pinch moves the
    // point sideways as well as up -- so a naive (-dZ/dx, -dZ/dy, 1) normal leans the wrong way on
    // exactly the steep faces where the lighting is being looked at.
    float3 tangentX = float3(1.0 - jxx, -jxy,       dzd.x);
    float3 tangentY = float3(-jxy,      1.0 - jyy,  dzd.y);
    s.normal = normalize(cross(tangentX, tangentY));

    float det = (1.0 - jxx) * (1.0 - jyy) - jxy * jxy;
    s.fold = saturate(1.0 - det);

    return s;
}
