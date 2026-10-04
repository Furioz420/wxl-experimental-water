#include "CelestialReflection.hlsli"
// Water surface, pixel stage: one medium, shaded the same way from either side of it.
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
// Compile: fxc /T ps_3_0 /E main /Fh SurfacePs.h /Vn kSurfacePs Surface.ps.hlsl
//
// WHY THIS IS ONE SHADER AND NOT TWO
//   The client treats above-water and below-water as different worlds: a surface pass, and a
//   separate full-screen tint that switches on when the camera crosses. The switch is the artefact
//   -- it happens at an instant, it happens to the whole screen at once, and at the waterline it
//   happens repeatedly. Here the surface is a boundary of one medium, drawn double-sided, and the
//   colour on both sides comes from the same extinction. Crossing it changes which side of a face
//   is lit, and nothing else.

#include "Transmission.hlsli"

/// Fog colour, from the client's own liquid block. Taken rather than re-sent so the horizon matches
/// the land beside it exactly.
float4 gFogColor : register(c4);

float4 gShallow    : register(c6);   ///< rgb shallow-water tint, w bottom brightness
float4 gDeep       : register(c7);   ///< rgb deep-water tint, w shared time in seconds (wrapped)
float4 gExtinction : register(c8);   ///< rgb attenuation per world unit, w longest path considered
float4 gSun        : register(c9);   ///< xyz direction toward the sun (world), w specular exponent
float4 gSunColor   : register(c10);  ///< rgb sun colour, w intensity
float4 gCamera     : register(c11);  ///< xyz camera world position, w Fresnel reflectance at normal
float4 gSkyLow     : register(c12);  ///< rgb sky at the horizon, w foam strength
float4 gSkyHigh    : register(c13);  ///< rgb sky at the zenith, w surface roughness
/// x: 1 when the camera is inside the water. y: 1 when the scene copy below is live.
/// zw: one texel of that copy.
float4 gEye        : register(c14);

/// Foam shape, live from the tuning panel. These were literals until the sea had to be fitted to a
/// map rather than to an idea of one -- and a foam constant is not something anyone can reason to,
/// it is found by moving it while looking at a beach.
///
/// gFoam0  x: fold at which a crest counts as breaking   y: how abruptly it turns on
///         z: gentlest bottom slope that still counts     w: how abruptly THAT turns on
/// gFoam1  x: depth at which the shore band gives out     y: floor under the texture mask
///         z: how much of the mask's contrast is used     w: how hard the shore band drives the sum
/// gFoam2  xyz: the three texture scales, deliberately incommensurate so no two layers ever repeat
///              together                                  w: how far the wave normal warps them
/// gFoam3  x: how fast the accumulation saturates towards white
float4 gFoam0 : register(c15);
float4 gFoam1 : register(c16);
float4 gFoam2 : register(c17);
float4 gFoam3 : register(c18);

/// Micro-relief and the light that passes through a crest.
///
/// gDetail  xy: the two ripple scales, without common measure so neither tiles with the other
///          z: how hard the sampled normal bends the surface   w: strength of the through-glow
/// gGlow    rgb: the colour light comes out as after crossing a crest -- greener and lighter than
///               the body, because the short wavelengths are what survive a short path
///          w: strength of the far glints
float4 gDetail : register(c19);
float4 gGlow   : register(c20);

/// gShade  x: how deeply a trough is shaded   y: reciprocal of the sea's own maximum rise, so the
///            effect means the same thing in a calm as in a gale
///         z: how lit the body still is with the sun on the horizon -- the floor under daylight,
///            never zero, because the whole sky lights the sea even when the sun does not
float4 gShade : register(c21);

/// 1 / patch size of each detail cascade -- the same four numbers the vertex stage displaces by,
/// re-sent here because this stage samples the matching SHADING textures itself.
// FROM c34 UP, AND NOT BELOW: c22..c25 belong to the terrain's height-blend block, and these
// constants persist across draws -- the client only refills c0..c5 -- so the water living there
// overwrote the terrain's layer weights on every frame it drew. The terrain was here first.
float4 gDetailUv  : register(c34);
/// x: how hard the detail field's own fold drives the white, already gated. y: flat inland material.
float4 gDetailCtl : register(c35);

/// THE TEXTURE-DRIVEN EFFECTS, one strength each, every one already multiplied by its own switch
/// on the way in -- a zero here IS the effect turned off, so judging a contribution costs a click
/// and no shader permutation.
///
/// gFx0  x: animated-normal strength      y: fractal-noise break-up strength
///       z: foam texture strength         w: panorama sky reflection blend
/// gFx1  x: caustic loop phase, in slices y: crest through-light strength
///       z: caustic strength              w: crossfade between the two bound grain slices
/// gFx2  x: animated-normal fine scale    y: its broad scale (no common measure with x)
///       z: noise scale                   w: foam texture scale
/// gFx3  x: caustic scale                 y: second caustic layer's scale, relative to the first
///       z: through-light lobe exponent   w: how fast rise counts as thinness
/// gFx4  x: caustic sun-height exponent   y: how fast the caustic dies with the water column
///       z: through-light sun-height exponent
///       w: how far the low-frequency noise displaces every tiled lookup -- the de-tiling warp
float4 gFx0 : register(c36);
float4 gFx1 : register(c37);
float4 gFx2 : register(c38);
float4 gFx3 : register(c39);
float4 gFx4 : register(c40);

/// The sun glitter, whose three shaping numbers were literals until a sea under static made it
/// clear they were settings.
///
/// x: world scale of the noise seed -- coarse enough that a texel spans several screen pixels
/// y: threshold the seed plus the sun lobe must cross for a spark
/// z: how abruptly it crosses. High is a hard binary decision and aliases; low is a soft sparkle
/// w: distance at which sparks begin to appear (they fade IN, being a many-facets-per-pixel effect)
float4 gGlint : register(c41);

/// THE STYLISATION, each channel neutral at zero so the physical look is the default.
///
/// x: cartoon amount -- lerps the Fresnel from the physical Schlick curve to a soft squared ramp
///    with a floor, so the mirror is never total and never absent. This is most of what separates
///    a rendered sea from a painted one.
/// y: clarity floor -- the least fraction of the scene that always survives the medium. Extinction
///    is physics and physics makes deep water opaque; a stylised sea keeps its bottom visible
///    because the artist said so, not because the numbers allow it.
/// z: how much of the reflection is repainted in the water's own colour. A mirror shows the world;
///    a painted sea shows mostly itself, with the world hinted through.
/// w: how much the surface thins out again at grazing angles -- a small subtraction from the
///    body's opacity where the view flattens, so the far water leans on its reflection rather
///    than on its own paint.
float4 gStyle : register(c43);

/// THE LACE: the client's own animated ocean sheet, drawn back on top of the water.
///
/// x: strength (zero is off)          y: world scale of the pattern
/// z: how far the lookup wobbles, which is what makes the lace boil in place rather than scroll
/// w: how much a breaking crest brightens it
float4 gLace : register(c44);

/// The second stylisation row.
///
/// x: sun diffuse strength -- N.L daylight worked into the body itself, so a sun-facing slope is
///    a brighter water and not merely a brighter highlight
/// y: refraction style -- 0 is the world-anchored bend, 1 the screen-anchored one whose reach
///    grows with the thickness of water behind the surface and dies at grazing angles
/// z: how fast thickness counts toward that reach (per world unit of path)
/// w: the screen-anchored bend's magnitude
float4 gStyle2 : register(c45);

/// The lace's second row.
///
/// x: crossfade between the two bound frames, read off the client's own animation blend
/// y: how fast the sheet dies with the water column. Zero keeps it out to open water, which is
///    what an OCEAN sheet normally wants; raised, it pulls the lace to the shore like surf
/// z: distance fade rate -- how soon the filaments give up before they alias
float4 gLace2 : register(c46);

#include "WaterScreenTrace.hlsli"

/// The caustic's second contribution and the shape of its first.
///
/// x: strength of the SURFACE term -- the same web drawn in the body of the water rather than on
///    the floor, so it survives any viewing angle and any depth
/// y: how far the floor term follows the viewing angle. One is physical (visible only through the
///    surface); zero draws it whatever the angle
/// z: how fast the surface term dies with the water column. Zero keeps it out to open water
/// w: loop phase of the SECOND caustic layer, wrapped on the CPU from absolute time at its own
///    rate -- never derived from the first layer's already-wrapped phase
float4 gCaustic : register(c42);

/// THE DEBUG VIEW, which is a measuring instrument and not an effect.
///
/// x: master, one or zero. Zero makes the whole block below the identity, at the cost of the dot
///    products themselves -- the price of the view being switchable at runtime rather than a second
///    shader nobody would keep in step with this one.
/// y: scale on the water column, so a map whose water is four units deep and one whose water is four
///    hundred both fill the ramp. Set from what the lattice measured last frame.
float4 gDbg : register(c31);
/// One-hot weights over the quantities the view can show, so the selection is a dot product rather
/// than a branch: (water column, bottom slope, break signal, foam).
float4 gDbgSel : register(c32);
/// The two that did not fit above: (detail field arriving, foam break-up mask).
float4 gDbgSel2 : register(c33);

/// Weights over the four caustic slices packed in one texel, for a loop position in [0, 4).
///
/// CYCLIC, which is the whole reason the loop never steps: the distance from slice three to slice
/// zero is one, not three, so the wrap back to the start is the same blend as any other step. A
/// linear ramp would fade the last slice out into nothing and snap.
float4 CausticWeights(float phase)
{
    float4 d = abs(float4(0.0, 1.0, 2.0, 3.0) - phase);
    return saturate(1.0 - min(d, 4.0 - d));
}

/// THE CLIENT'S OWN OCEAN SHEET, exactly as the stock material bound it: two consecutive frames
/// of the animated lace sequence on the first two stages. Nothing here binds or owns them -- the
/// material set them for its own draw, this shader only reads what is already there. That is the
/// whole bargain: an animated, thirty-frame, artist-authored ocean pattern for zero uploads.
sampler2D sLaceA : register(s0);
sampler2D sLaceB : register(s1);

/// The frame as it stood before any water drew over it. See Refraction.cpp.
sampler2D sScene : register(s6);
/// The client's own liquid foam texture -- slot 3 of the material's animated set. See Surface.cpp.
sampler2D sFoam  : register(s7);
/// Centered RG slopes generated by Noise::BuildBump and bound directly on stage 5.
sampler2D sRipple : register(s5);

/// THE DETAIL FIELD'S SHADING MAPS: (dZ/dx, dZ/dy, foam) per cascade, on the grids the vertex
/// stage displaced from. Sampled HERE and not there for one reason worth the extra fetches: these
/// stages have bilinear filtering and a vertex fetch has none, so the normal -- where a flat texel
/// step is far more visible than in the height -- interpolates instead of stepping. It is also
/// what freed a vertex sampler per cascade, which is what four cascades cost.
sampler2D sShade0 : register(s8);
sampler2D sShade1 : register(s9);
sampler2D sShade2 : register(s10);
sampler2D sShade3 : register(s11);

/// THE SHADING MAPS, synthesized rather than authored or loaded: every one is a noise field with a
/// different reduction on top, so all of them tile without a seam and the two animated ones loop
/// without a step. Bound and filtered at the draw, where a write survives the deferred flush.
///
/// sFbm      four channels of precomputed five-octave fractal noise -- the break-up source for
///           everything that would otherwise repeat: foam thresholds, body tint, the sparkles.
/// sSeqA/B   two consecutive slices of the animated grain loop, crossfaded by gFx1.w so it never
///           steps. This is grain at the scale of a pixel, alive.
/// Broad and fine normal detail share the owned centered slope map on s5.
/// Captured scene depth exclusively uses s15.
/// sChop     a dedicated foam pattern: filaments and holes, not a tint.
/// sSky      a horizon panorama for the reflection, over the analytic gradient.
/// sCaustic  one frame of an animated caustic set, for sunlit shallows.
sampler2D sFbm     : register(s2);
sampler2D sSeqA    : register(s3);
sampler2D sSeqB    : register(s4);
sampler2D sChop    : register(s12);
sampler2D sSky     : register(s13);
sampler2D sCaustic : register(s14);
// s15 now holds immutable pre-water depth. Broad detail reuses the authored ripple map.

struct PsIn
{
    float4 world  : TEXCOORD0;
    float4 normal : TEXCOORD1;   ///< xyz trains' normal, w their fold
    float4 screen : TEXCOORD2;   ///< xyz projective screen coordinate, w the detail layer's gain
    float4 misc   : TEXCOORD3;   ///< x fog, y view distance, z slope of the bottom
    float4 aux    : TEXCOORD4;   ///< xy still-water position, where the detail was sampled from
};

/// Sky along a direction, as a gradient plus the sun. Stands in for a probe: on an open sea the
/// reflected ray almost never finds anything but sky, and the one thing in it that has to be right
/// is the sun -- the glitter path is most of what reads as water at a distance.
///
/// `sharp` collapses toward zero where the normal is churning within one pixel; see the note on
/// specular aliasing below for why the sun has to be told about that.
float3 SkyAlong(float3 dir, float sharp)
{
    float  up  = saturate(dir.z * 0.5 + 0.5);
    float3 sky = lerp(gSkyLow.rgb, gSkyHigh.rgb, up * up);

    return sky;
}

/// Ripple below the size of a vertex, as a slope rather than as geometry.
///
/// The spectrum stops at a wavelength the mesh can carry -- around nine units. Between that and a
/// pixel there is nothing at all, and water with an empty band there reads as moulded plastic: the
/// shape is right and the surface is dead. Real water carries texture the whole way down, and below
/// the spacing of a vertex the only place it can live is the normal.
///
/// Each octave travels at its own deep-water speed, for the same reason the spectrum does: ripples
/// that share a speed slide as one sheet and are read instantly as a scrolling texture.
/// Micro-relief, sampled rather than computed.
///
/// This replaced four analytic octaves, and it is both the better picture and the cheaper one --
/// which is unusual enough to be worth saying why.
///
/// A cosine octave has ONE direction and ONE exact period. Four of them cross into a grid, the same
/// grid at every point on the map, and the previous version spent its effort bending each octave
/// along the one above it purely to disguise that. What it bought was smoothness: every ripple the
/// same shape as every other, which is exactly the plastic reading -- real water at this scale is
/// full of creases, flats and little sharp ridges that no sum of cosines will produce.
///
/// A normal map has all of that baked in, and two taps of it carry more variety than eight octaves
/// would. The two layers run at scales with no common measure and drift in opposite directions, so
/// what tiles in each is never in step with the other.
float2 RippleSlope(float2 p, float t)
{
    float2 uv1 = p * gDetail.x + float2( 0.0048,  0.0028) * t;
    float2 uv2 = p * gDetail.y - float2( 0.0036, -0.0056) * t;

    float2 n1 = tex2D(sRipple, uv1).xy * 2.0 - 1.0;
    float2 n2 = tex2D(sRipple, uv2).xy * 2.0 - 1.0;

    return (n1 + n2) * gDetail.z;
}

float4 main(PsIn i) : COLOR
{
    float3 n = normalize(i.normal.xyz);

    // THE DETAIL FIELD'S LIGHT, reunited with its geometry. The vertex stage displaced by the four
    // cascades; here the same four are sampled for the slope that displacement implies, at the same
    // still-water coordinate, scaled by the same gain the geometry got (screen.w carries it down).
    // Slopes of height fields add, so this is a lean on the trains' normal rather than a rebuild.
    // The foam channel rides along: whitecaps are a short-wave event, and short is this field.
    float  dgain = i.screen.w;
    float4 s0 = tex2D(sShade0, i.aux.xy * gDetailUv.x);
    float4 s1 = tex2D(sShade1, i.aux.xy * gDetailUv.y);
    float4 s2 = tex2D(sShade2, i.aux.xy * gDetailUv.z);
    float4 s3 = tex2D(sShade3, i.aux.xy * gDetailUv.w);
    float2 dslope = (s0.xy + s1.xy + s2.xy + s3.xy) * dgain;
    float  dfoam  = max(max(s0.z, s1.z), max(s2.z, s3.z));
    n = normalize(n + float3(-dslope.x, -dslope.y, 0.0));

    // Detail fades out with distance, and sooner than it looks like it should. Its shortest octave
    // is under a unit: past about seventy the period is well under a pixel, where keeping it adds no
    // texture at all -- only the beat between the ripple and the pixel grid, which is the same fault
    // the spectrum floor exists to avoid.
    float detailFade = saturate(1.0 - i.misc.y * 0.014);
    if (detailFade > 0.0)
    {
        float2 s = RippleSlope(i.world.xy, gDeep.w) * detailFade;
        n = normalize(n + float3(-s.x, -s.y, 0.0));
    }

    // The break-up source, sampled once at two scales and consumed by everything that would
    // otherwise repeat: the body tint, the foam thresholds, the sparkles, and the DOMAIN WARP
    // below. Four channels of independent fractal noise, so no two consumers read the same pattern.
    //
    // Sampled here, above the grain, because the grain is now warped by it. Its own scale is
    // hundreds of units -- far longer than anything it breaks up -- which is what lets it displace
    // a lookup without being seen doing it.
    float4 fbmA = tex2D(sFbm, i.world.xy * gFx2.z);
    float4 fbmB = tex2D(sFbm, i.world.xy * (gFx2.z * 9.7) + fbmA.xy * 0.15);

    // WHY EVERY LOOKUP BELOW IS ROTATED AND WARPED.
    //
    // A tiled texture sampled on i.world.xy repeats on a SQUARE GRID ALIGNED TO THE WORLD AXES, and
    // an axis-aligned repeat is the one a human eye finds instantly -- it reads as banding running
    // straight across the sea. The tile was always there; the alignment is what made it visible.
    //
    // Two things fix it and they fix different halves. Rotating each layer's frame by its own angle
    // means no two layers repeat along the same lines and none of them repeat along the world's.
    // Warping the lookup by the low-frequency noise then breaks the periodicity itself, not merely
    // its orientation -- the tile is still there, but its corners no longer land on a grid.
    //
    // This is free to do here and NOT free on the detail field's own lookups: those have a vertex
    // stage counterpart that must sample the same place, so warping the shading alone would slide
    // the lighting off the geometry it belongs to.
    float2 warpN = (fbmA.zw - 0.5) * gFx4.w;

    // THE LIVING GRAIN: an animated water-normal loop at two world scales without common measure,
    // crossfaded between slices so it never steps. The fine layer follows the ripple's distance
    // fade; the broad layer NEVER fades -- past where geometry and ripples have both given out,
    // this is the only thing still saying "water" at the scale of a pixel, and its mips keep it
    // clean out to the horizon. The slow uv drifts keep the two layers from agreeing with
    // themselves.
    {
        float2 w = i.world.xy;
        // 31.7 and -17.3 degrees. Deliberately not round numbers and deliberately not multiples of
        // each other: two layers a right angle apart still share a lattice.
        float2 wF = float2(w.x * 0.8508 - w.y * 0.5255, w.x * 0.5255 + w.y * 0.8508);
        float2 wB = float2(w.x * 0.9548 + w.y * 0.2974, -w.x * 0.2974 + w.y * 0.9548);
        float2 uvF = wF * gFx2.x + warpN + float2(gDeep.w * 0.0024, -gDeep.w * 0.0016);
        float2 uvB = wB * gFx2.y + warpN * 0.6 + float2(-gDeep.w * 0.0009, gDeep.w * 0.0012);
        float2 aF = lerp(tex2D(sSeqA, uvF).rg, tex2D(sSeqB, uvF).rg, gFx1.w) - 0.5;
        float2 aB = tex2D(sRipple, uvB).rg - 0.5;
        float2 aslope = (aF * (0.35 + 0.65 * detailFade) + aB) * (2.0 * gFx0.x);
        n = normalize(n + float3(-aslope.x, -aslope.y, 0.0));
    }

    float3 v = gCamera.xyz - i.world.xyz;
    float  viewLen = max(length(v), 1e-3);
    v /= viewLen;

    // The normal is always turned to face the viewer. The surface is drawn double-sided -- a
    // displaced sheet turns away from the camera constantly, and culling those faces is what leaves
    // a wave looking hollow -- so a back face is an ordinary part of the surface and has to be lit
    // as one rather than inverted.
    float3 sn = (dot(n, v) < 0.0) ? -n : n;

    // WHICH SIDE OF THE MEDIUM THE EYE IS ON, which is a fact about the camera and not about the
    // triangle. Deciding it from the facing bit instead would put Snell's window on the far slope of
    // every wave, seen from dry land.
    bool under = gDetailCtl.y > 0.5 ? gCamera.z < i.world.z : gEye.x > 0.5;

    float ndv = saturate(dot(sn, v));

    // SPECULAR ALIASING, and why it is not a lighting problem but a sampling one.
    //
    // A tight highlight is a function that varies far faster than one pixel. Where several wave
    // crests fall inside a single pixel -- which is everywhere past the near field -- point-sampling
    // it reports whichever crest happened to land on the pixel centre, and the result is a quilt of
    // hard white blobs standing still while the water moves under them. Nothing about it is fixed by
    // dimming the sun; the highlight has to be widened to cover what the pixel actually contains.
    //
    // The measure of "how much surface is in this pixel" is how fast the normal turns across it.
    float3 dnx = ddx(sn);
    float3 dny = ddy(sn);
    float  churn = saturate((dot(dnx, dnx) + dot(dny, dny)) * 320.0);
    float  sharp = 1.0 - churn;

    float2 uv = i.screen.xy / max(i.screen.z, 1e-4);

    // Authored seabed depth remains the maximum path. Captured geometry can
    // shorten it for submerged characters/rocks and shallow shore contact.
    float column = max(i.world.w, 0.0);
    float path   = min(column / max(ndv, 0.10), gExtinction.w);
    float geometryPath=-1.0;
    if (!under)
        geometryPath=WaterGeometryPath(uv+gEye.zw*0.5,
            dot(float4(i.world.xyz,1),gSsrRowW),i.misc.y);
    if (geometryPath>=0.0) path=min(path,geometryPath);

    float3 transmit = exp(-gExtinction.rgb * path);

    // THE CLARITY FLOOR, which is where this stops being physics on purpose. The exponential is
    // honest and honesty makes deep water a wall: past a few tens of units nothing comes back, and
    // a sea nobody can see into reads as dye. The stylised answer -- the one the hand-painted
    // water this engine grew up with always used -- is that opacity is AUTHORED: however deep the
    // column, at least this fraction of what is behind the surface survives. Zero is the physical
    // sea back.
    transmit = max(transmit, gStyle.y);

    // WHAT IS ACTUALLY BEHIND THE SURFACE.
    //
    // Sampled from the copy of the frame taken before any water drew, displaced by the surface
    // normal. The displacement is the entire point: a straight sample would show the bottom lying
    // flat behind glass, and it is the bending -- the sand swimming under a passing crest -- that
    // reads as water rather than as a coloured pane.
    //
    // The offset shrinks with distance so it stays roughly constant in world terms, and it is capped:
    // an unbounded one reaches past the waterline and pulls sky and shoreline down into the water.
    float3 scene = 0.0;
    if (gEye.y > 0.5)
    {
        float2 bendW = sn.xy * (2.4 / max(i.misc.y, 4.0));
        // Bound refraction displacement, then reject foreground samples using depth.
        bendW = clamp(bendW, -0.018, 0.018);
        // Scaled by how much water there is: a film over wet sand must not bend anything, or the
        // shoreline crawls.
        bendW *= saturate(path * 0.5);

        // THE OTHER WAY TO BEND, screen-anchored rather than world-anchored. The offset holds its
        // size on screen instead of in the world, its reach grows with how much water actually
        // stands behind the surface -- a film over wet sand bends nothing, a deep column swims --
        // and it dies at grazing angles, where a flattened view mostly reflects and what little
        // refracts should barely move. Two regimes with one visible difference: distant underwater
        // shapes stay alive here, where the world-anchored bend has already shrunk to stillness.
        float  inv2  = 1.0 - ndv;
        float2 bendR = sn.xy * (0.01 * gStyle2.w)
                     * saturate(path * gStyle2.z)
                     * saturate(1.2 - inv2 * inv2);
        float2 bend = lerp(bendW, bendR, gStyle2.y);

        float2 center=uv+gEye.zw*0.5;
        float surfaceDepth=dot(float4(i.world.xyz,1),gSsrRowW);
        float2 refractedUv=WaterRefractionUv(center,center+bend,surfaceDepth);
        scene = tex2D(sScene, refractedUv).rgb;
    }

    // Large-scale tint variation from the noise, centred so it neither brightens nor darkens on
    // average. Open water shaded from one extinction is one flat colour to the horizon; real sea
    // is mottled by cloud, depth and turbidity, and this is the cheapest honest stand-in.
    float3 bodyLight = 1.0 + (fbmA.x - 0.5) * (0.6 * gFx0.y);

    // HOW MUCH LIGHT GETS INTO THE WATER AT ALL, which is a property of the sun's height and of
    // nothing else on this surface.
    //
    // Everything the sun did here until now was a reflection -- the highlight, the glints, the glow
    // through a crest. The BODY did not know whether it was day. So a noon sea and an evening sea
    // came out the same colour underneath, with only the sparkle on top telling them apart, and that
    // is backwards: at noon the light arrives near perpendicular, almost all of it is transmitted
    // rather than reflected, and the water lights up from the inside. Near the horizon most of it
    // glances off instead, and what is left of the body goes dark while the surface turns to mirror.
    //
    // The floor is not zero. Even with the sun down, the whole sky is still an emitter over every
    // point of the sea -- water at night is dark, not black, and driving this to nothing would read
    // as a bug rather than as dusk.
    float entering = lerp(gShade.z, 1.0, saturate(gSun.z));
    bodyLight *= entering;

    // DAYLIGHT ON THE BODY ITSELF. Everything above is what the sun's HEIGHT lets in; this is what
    // its DIRECTION does once inside -- a sun-facing slope is a brighter water, not merely a water
    // with a brighter highlight on it. Multiplied by the body rather than added over it, so it
    // brightens the colour the water already has instead of washing it toward the sun's.
    bodyLight += bodyLight * gSunColor.rgb * (saturate(dot(sn, gSun.xyz)) * gSunColor.w * gStyle2.x);

    // A captured object is already lit. Apply this illumination only to the water's
    // scattered color; retain the previous lit palette when scene capture is unavailable.
    float3 body = WaterBody(gShallow.rgb, gDeep.rgb, scene, gShallow.w,
                            transmit, bodyLight, gEye.y);

    // CAUSTICS, where three things coincide: shallow water, transmitted light, and a sun high
    // enough to focus. The surface normal warps the lookup so the pattern swims with the water
    // above it, and the column fade keeps it to the shallows -- the same signal the shore foam
    // already trusts.
    //
    // TWO LAYERS MULTIPLIED, at scales and loop rates with no common measure, and that is what
    // fixes the repetition rather than any number of extra frames. One layer repeats at the length
    // of its loop, and a loop short enough to hold in memory is a loop you watch come round. The
    // product of two repeats at their common multiple -- minutes, not seconds -- and it also looks
    // more like the thing: real caustics are webs crossing webs, not one web.
    {
        // Rotated off the world axes and warped, for the same reason as the grain: the caustic
        // tile is twenty world units across, and twenty units of axis-aligned repeat on a lit
        // seabed is a chequerboard.
        float2 cw = float2(i.world.x * 0.7501 - i.world.y * 0.6613,
                           i.world.x * 0.6613 + i.world.y * 0.7501);
        float2 cuv = cw * gFx3.x + warpN * 1.4 + sn.xy * 0.08;
        // BOTH PHASES ARRIVE ALREADY WRAPPED, each from absolute time at its own rate.
        //
        // The second used to be derived from the first AFTER the first had been folded into its
        // loop -- so every time the first wrapped, the second jumped somewhere unrelated. That
        // discontinuity is the visible knock at the end of the cycle, and it came from the layer
        // whose entire purpose was to make the cycle unnoticeable.
        float  c0  = dot(tex2D(sCaustic, cuv), CausticWeights(gFx1.x));
        float  c1  = dot(tex2D(sCaustic, cuv * gFx3.y + float2(0.41, -0.23)),
                         CausticWeights(gCaustic.w));
        // Squared AFTER the product, not before: squaring each layer first would thin both webs to
        // filaments and their intersection to almost nothing.
        float  ca  = c0 * c1;
        float  web = ca * ca * 4.0 * pow(saturate(gSun.z), gFx4.x);

        // TWO CONTRIBUTIONS FROM ONE WEB, because a caustic is two different things and only one of
        // them was being drawn.
        //
        // The FLOOR term is light that crossed the surface and landed on the bottom. Seeing it
        // means seeing through the water, so it is gated on the depth of the column and on how much
        // is transmitted rather than reflected -- which depends on the viewing angle, and is why
        // the whole effect used to vanish the moment you looked along the sea instead of down into
        // it. gCaustic.y lets that dependence be dialled back: at zero the floor web is drawn
        // whatever the angle, which is wrong physically and right for a stylised sea.
        //
        // The SURFACE term is not the bottom at all -- it is the same focusing happening in the
        // body of the water, and it belongs on open water at any angle and any depth. That is the
        // one that adds detail rather than decorating a shallow.
        const float view = lerp(1.0, transmit.g, gCaustic.y);
        body += gSunColor.rgb * (web * gFx1.z * saturate(1.0 - column * gFx4.y) * view);
        body += gSunColor.rgb * (web * gCaustic.x * saturate(1.0 - column * gCaustic.z));
    }

    // --- reflection ------------------------------------------------------------------------------
    float3 r = reflect(-v, sn);
    float3 reflection;
    float  fresnel;
    float reflectedSourceVisibility = 1.0;

    if (under)
    {
        // Looking up from beneath, refraction out of the water only succeeds inside Snell's window
        // -- a cone about 97 degrees wide, beyond which the surface is a mirror of the water itself.
        // That cone is the single most recognisable thing about being underwater and the client has
        // never had it, because from below it simply stopped drawing a surface.
        float3 outward = refract(-v, sn, 1.0 / 1.333);
        float  escaped = dot(outward, outward) > 0.0 ? 1.0 : 0.0;
        reflection = lerp(body * 0.7, SkyAlong(outward, sharp) +
            CelestialReflection(dot(outward,gSun.xyz),sharp,gSkyHigh.w,gSunColor.rgb,gSunColor.w,gSun.z,gSun.w), escaped);
        // Total internal reflection is total: at the rim of the window the surface goes fully to
        // the mirror rather than fading toward it.
        fresnel = 1.0 - escaped * 0.85;
    }
    else
    {
        reflection = SkyAlong(r, sharp);

        // A REAL SKY IN THE MIRROR, not only a gradient. The panorama wraps in azimuth and covers
        // the band above the horizon -- which is all a sea surface ever reflects. It is tinted by
        // the zone's own sky palette, so it follows the hour and the weather the way everything
        // else here does, and it rides on top of the gradient rather than replacing it: the
        // gradient carries the colour, the panorama carries the STRUCTURE the gradient never had.
        float az = atan2(r.y, r.x) * 0.1591549 + 0.5;
        float ev = saturate(1.0 - saturate(r.z) * 1.7);
        float3 skyTex = tex2D(sSky, float2(az, ev)).rgb;
        float3 skyTint = (gSkyLow.rgb + gSkyHigh.rgb) * 0.75;
        reflection = lerp(reflection, skyTex * skyTint, gFx0.w);

        // THE REPAINT. A mirror at the cartoon end is not a mirror: most of whatever the
        // reflection found is painted over in the water's own colour, and only the remainder of
        // the world shows through. The tint is the same extinction gradient the body uses WITHOUT
        // the scene behind it, lit by the same daylight -- so the repainted sea agrees with itself
        // at every hour and every depth.
        float3 waterTint = lerp(gDeep.rgb, gShallow.rgb * gShallow.w, transmit) * entering;
        reflection = lerp(reflection, waterTint, gStyle.z);

        // The source belongs to the environment but must survive panorama and
        // stylized tint blending. A valid geometry hit below replaces it with the
        // captured scene, just as it replaces the rest of the sky reflection.
        reflection += CelestialReflection(dot(r,gSun.xyz),sharp,gSkyHigh.w,
            gSunColor.rgb,gSunColor.w,gSun.z,gSun.w);

        // Depth-tested reflected ray; misses retain the environment reflection.
        // No arbitrary distant scene sample or screen-vertical blur remains.
        if (gSsr.x>0.0 && gSsr.z>0.5 && gWaterDepth.x>0.5)
        {
            float3 mn=normalize(lerp(float3(0,0,1),sn,gSsr.w));
            float3 ray=reflect(-v,mn);
            float3 hit=WaterReflectionHit(i.world.xyz+mn*0.08,ray,gEye.zw);
            float3 reflectedScene=tex2Dlod(sScene,float4(hit.xy,0,0)).rgb;
            reflectedSourceVisibility = 1.0-saturate(hit.z*gSsr.x);
            reflection=lerp(reflection,reflectedScene,1.0-reflectedSourceVisibility);
        }



        // Two Fresnel curves, one slider between them. Schlick's is the physical one: near nothing
        // face on, a true mirror at grazing. The soft ramp is the stylised one -- squared rather
        // than fifth power, floored so the surface always reflects SOMETHING and capped by its own
        // shape so it never becomes all mirror. That bounded band is what keeps a stylised sea the
        // same sea from every camera.
        float phys = gCamera.w + (1.0 - gCamera.w) * pow(1.0 - ndv, 5.0);
        float inv  = 1.0 - ndv;
        fresnel = lerp(phys, 0.2 + 0.5 * inv * inv, gStyle.x);
    }

    float3 color = lerp(body, reflection, saturate(fresnel));

    // WHERE THE SUN REACHES, without a shadow map and without tracing anything.
    //
    // A crest stands in the light; the trough behind it does not. Tracing that properly is genuinely
    // possible here -- the sea is a closed-form function, so a ray against it is four evaluations
    // along a line rather than a walk through a scene -- but each step costs the whole spectrum, and
    // the vertex stage has no room for that. This is the same phenomenon read off a number already
    // being carried: how far this point sits above or below the mean.
    //
    // THE SUN'S HEIGHT IS WHAT MAKES IT READ AS SHADOW. Overhead, a trough is not in shade at all and
    // the term has to vanish; low, the smallest crest throws a long way. Without that coupling the
    // troughs are merely painted darker, which the eye takes as dirt on the water rather than as
    // light being blocked -- and it would go on being wrong all day while the sun moved.
    float sunLow   = saturate(1.0 - gSun.z);
    float trough   = saturate(-i.misc.w * gShade.y);
    float sunShade = 1.0 - sunLow * trough * gShade.x;

    // Sparse glitter retains the half-vector; broad reflection is composed above.
    float3 h = normalize(gSun.xyz + v);
    // Broad celestial reflection is composed above and obeys Fresnel/SSR.
    // Retain this half-vector only for the sparse glitter below.

    // GLINTS, which are a different thing from the highlight above and cannot come out of it.
    //
    // The broad lobe is what one patch of water does. Far out, thousands of patches fall inside one
    // pixel, and what reaches the eye is not their average -- it is the few that happen to be turned
    // exactly right, each returning almost the whole sun. Averaging gives a dull sheen; the sea
    // gives hard white sparks that come and go.
    //
    // Faked with an exponent far past anything physical, faded IN with distance -- the opposite of
    // every other detail here, and correct: this is an artefact of many surfaces per pixel, so it
    // cannot exist up close where there is only one.
    // Faked with NOISE crossing a threshold inside a moderate sun lobe, not with an exponent of
    // nine hundred. The old power sampled a function far narrower than a pixel, so each spark was
    // whichever facet landed on the pixel centre -- hard white points that popped per frame. Here
    // the lobe (power sixty, resolvable) says WHERE sparkling is allowed, and a drifting noise
    // field says WHICH points currently spark: the population turns over smoothly, and a spark is
    // born and dies instead of flickering into existence.
    // THE SCALE IS THE WHOLE STORY HERE, and it was a literal.
    //
    // The seed is a 256px noise map sampled in world space. At the scale this used to carry, one
    // texel covered three hundredths of a world unit -- one to two texels per SCREEN pixel at any
    // normal viewing distance, which is where a texture stops being a pattern and starts being
    // noise. Thresholding that hard then turns every sub-pixel wobble into a full white decision,
    // and the sea comes out under a layer of static.
    //
    // Held coarse enough that a texel spans several pixels, the same threshold produces sparks with
    // an actual size, which is what a glint is: a facet, not a sample.
    float glint = smoothstep(gGlint.w, gGlint.w * 5.5, i.misc.y);
    float lobe  = pow(saturate(dot(sn, h)), 60.0);
    float sparkSrc = tex2D(sFbm, i.world.xy * gGlint.x
                           + float2(gDeep.w * 0.011, -gDeep.w * 0.017)).b;
    float spark = saturate((sparkSrc + lobe * 0.9 - gGlint.y) * gGlint.z);
    color += gSunColor.rgb * (spark * lobe * glint * gGlow.w * gSunColor.w * sunShade);

    // LIGHT THROUGH THE WAVE, which is the one thing a sea most needs and a reflection model has no
    // way to produce. Looking towards the sun with a crest between, some of it comes THROUGH -- and
    // it arrives greener and lighter than the body, because a short path through water keeps the
    // wavelengths a long one loses.
    //
    // Thickness is the wave's own rise: a crest is a thin wall, a trough is metres of water. Taking
    // it from the geometry rather than from a texture is what makes the glow move with the wave that
    // causes it instead of sitting on the surface.
    // ITS STRENGTH IS AN OUTER FACTOR, and it was not. The control used to sit inside the saturate
    // below, multiplied by a compression signal that is near zero everywhere except on a breaking
    // crest, inside a sum whose other term already reached one -- so the switch removed a
    // second-order modulation of somebody else's effect and nothing on screen moved. A strength
    // that cannot turn its effect off is not a strength.
    float through   = pow(saturate(dot(v, -gSun.xyz)), gFx3.z);
    // Thinness is the trains' rise PLUS the detail field's own crest compression: where the short
    // waves gather, the water stands in a thin ridge the light crosses easily, and that is exactly
    // where the vivid green flash lives on a stylised sea. The compression signal is already in
    // hand -- it is the same one the whitecaps are born from.
    float thin      = saturate(i.misc.w * gFx3.w + dfoam * dgain);
    // Gated on the sun's HEIGHT as well as its bearing. Light through a crest is a low-sun effect:
    // at noon the sun is behind the viewer's shoulder for every bearing and there is no path
    // through anything, which is why an ungated version glowed at midday.
    color += gGlow.rgb * (through * thin * gFx1.y * gDetail.w * gSunColor.w * sunShade
                          * pow(saturate(1.0 - gSun.z), gFx4.z));

    // --- foam ------------------------------------------------------------------------------------
    // From the fold the vertex stage measured, plus a band where the column runs out. The first is
    // where the sea breaks on itself, the second is where it breaks on something else, and they are
    // the same white.
    //
    // The fold is thresholded, not scaled. Every crest carries some horizontal compression, so a
    // straight multiply whitens the entire sea; foam belongs only where the surface is close to
    // turning through itself, which is the top of the range and nothing below it.
    // ONE shore term, not two. The column and the slope both describe the same event -- a bottom on
    // its way up to the surface -- so running them side by side lays two bands of white over each
    // other, with an edge where the narrower one stops.
    //
    // The slope subsumes both cases on its own, because a beach is simply a gentle version of a reef:
    // the threshold is set low enough that a shelf climbing at a few degrees already qualifies, and a
    // cliff face saturates it. The column stays as the RANGE, not as a second signal -- it fades the
    // white out with depth, so the white follows the waterline instead of tracing the whole flank of
    // a trench that never reaches the surface.
    //
    // The two numbers are the knobs: 0.12 is how gentle a bottom still counts as rising, and 0.09 is
    // how deep the water may be before it stops mattering -- here about eleven units.
    // WHERE the foam is, before anything decides what it looks like. Two folds feed it: the
    // trains' own, which only speaks where the bottom shoals a long wave into breaking, and the
    // detail field's -- the whitecaps of open water, born on the CPU where the displacement folds,
    // aged there over seconds, and scaled here by the same gain the geometry got.
    float fold = max(i.normal.w, dfoam * dgain * gDetailCtl.x);

    // The thresholds breathe with the noise. A hard constant threshold draws the same contour on
    // every crest of the same strength -- a family resemblance the eye finds within seconds. A
    // few percent of jitter, uncorrelated with anything the waves do, is what makes each breaking
    // event its own event.
    float thrJit = (fbmB.y - 0.5) * (0.22 * gFx0.y);
    float breaking = saturate((fold - (gFoam0.x + thrJit)) * gFoam0.y);
    float shoreBand = saturate((i.misc.z - gFoam0.z - thrJit) * gFoam0.w)
                    * saturate(1.0 - column * gFoam1.x);

    // Stock-topology coast detection. uv0.y is the map liquid's authored depth/opacity ramp; its
    // screen-space gradient marks the shallow transition without expanding or displacing a single
    // coverage cell. Requiring a gradient is load-bearing: old placements with no depth stream are
    // constant zero, and therefore produce no false full-sheet foam. The shallow-end gate follows
    // the stock convention of zero at the bank and one offshore.
    const float stockRamp = i.aux.z;
    const float rampGradient = length(float2(ddx(stockRamp), ddy(stockRamp)));
    const float authoredCoast = (1.0 - i.aux.w)
                              * saturate(rampGradient * 24.0)
                              * saturate((0.40 - stockRamp) * 4.0);
    shoreBand = max(shoreBand, authoredCoast);
    // Share the above-water depth sample with absorption; max avoids stacking
    // a second foam layer where the map already supplies a shoreline band.
    shoreBand = max(shoreBand, WaterContactFoam(geometryPath));

    // WHAT IT LOOKS LIKE. A band shaded a flat white is a painted sheet with a soft edge, which is
    // what the shore has looked like until now. Three samples of the client's own foam texture break
    // it up, and the way they are combined is the whole trick:
    //
    //   - scales with no common multiple (0.031, 0.113, 0.277), so no two layers ever line up again;
    //   - each on its own rotation, so the texture's own axes never survive into the result;
    //   - dragged by the wave normal, so the pattern moves with the water instead of under it.
    //
    // Sampled in WORLD space, not in the liquid's own uv: the liquid uv restarts per chunk, and any
    // pattern built on it draws the chunk grid.
    //
    // AND NOT SCROLLED BY TIME. Foam does not travel -- it stays where the sea breaks, and the water
    // runs through it. A per-layer time offset gives every layer its own speed and its own direction,
    // so one of them always drifts against the swell, which is the single thing that gives a texture
    // away as a texture. All the movement here comes from the warp, which is the wave's own normal
    // and therefore already moves with the water and at its speed.
    float2 M    = i.world.xy;
    float2 warp = sn.xy * gFoam2.w;
    float2 uvA  = float2(M.x * 0.866 - M.y * 0.5,   M.x * 0.5   + M.y * 0.866);
    float2 uvB  = float2(M.x * 0.707 + M.y * 0.707, -M.x * 0.707 + M.y * 0.707);

    float f1 = tex2D(sFoam, uvA * gFoam2.x + warp).r;
    float f2 = tex2D(sFoam, uvB * gFoam2.y + warp * 0.5).r;
    float f3 = tex2D(sFoam, M   * gFoam2.z + warp * 1.5).r;

    // Added and saturated rather than multiplied. A product of three masks is dark almost everywhere
    // and leaves only the few places all three happen to agree, which reads as scattered blobs.
    float mask = saturate(f1 * 1.2 + f2 * 0.8) * (gFoam1.y + gFoam1.z * saturate(f3 * 2.0));

    // AND THE BREAKING CRESTS GO THROUGH THE SAME TEXTURE AS THE SHORE, which they were not doing.
    // Whitecaps arrived as the mask straight from the vertex stage and were painted flat -- so a
    // crest went white as one solid shape with a soft edge, which at open-water distance is a blob.
    // The shore band has been broken up by these three layers all along; there was never a reason
    // for the same white in deep water to skip them, and they are already sampled.
    //
    // A higher floor than the shore's, because a whitecap IS mostly solid where it is at all -- the
    // texture is there to give it an edge and a grain, not to eat it.
    //
    // Accumulated through an exponential, never summed. Two sources of white over the same pixel add
    // past one, flatten into a solid patch and take the alpha with them; this approaches full white
    // without ever arriving, so overlapping foam gets denser instead of blowing out.
    float fsum = breaking * (0.45 + 1.1 * mask) + shoreBand * (0.25 + 1.35 * mask) * gFoam1.w;

    // FOAM WITH A STRUCTURE, from a texture that IS foam rather than a tint to modulate. The
    // accumulated amount above decides WHERE white lives; this decides what white LOOKS like:
    // the pattern carves filaments through the body of a patch, and the lace term throws thin
    // bright threads just outside its edge -- the skirt of bubbles around real foam, which no
    // amount of blurring a solid patch will ever produce. Warped by the wave normal like the
    // other layers, so it moves with the water and never scrolls.
    {
        // Its own angle again, and one that shares no simple ratio with the grain's or the
        // caustic's -- three layers rotated to the same family of angles is one pattern in three
        // colours.
        float2 fw = float2(M.x * 0.4462 - M.y * 0.8949, M.x * 0.8949 + M.y * 0.4462);
        float chopT = tex2D(sChop, fw * gFx2.w + warpN * 0.9 + warp * 1.2).r;
        float carved = fsum * (0.35 + 1.35 * chopT);
        float lace = saturate((chopT - 0.55) * 6.0) * saturate(fsum * 4.0) * saturate(1.2 - fsum);
        fsum = lerp(fsum, carved + lace * 0.8, gFx0.z);
    }

    float foam = saturate(1.0 - exp2(-fsum * gFoam3.x)) * gSkyLow.w;
    color = lerp(color, float3(0.92, 0.95, 0.97), foam);

    // --- ocean lace ------------------------------------------------------------------------------
    // The animated sheet the stock ocean has always worn, read straight off the two frames the
    // material bound. Two taps: consecutive frames of the loop at slightly different lookups, so
    // the sum both advances the animation and never shows one frame's pattern alone.
    //
    // The WOBBLE is the whole character of the thing. Scrolled, the lace is instantly a texture
    // sliding over the sea; boiled in place -- each point of the lookup circling its home on its
    // own phase -- it reads as the surface churning, with no direction anyone can catch. The wave
    // normal drags the second tap so the pattern also leans with the swell.
    //
    // Fades with DISTANCE, not depth: the lace is a surface event and lives wherever the ocean
    // does, but at range its filaments drop under a pixel and alias -- the same reason the ripple
    // fades. A breaking crest brightens it, which ties the sheet to the sea instead of over it.
    float laceW = 0.0;
    if (gLace.x > 0.0)
    {
        float2 luv = i.world.xy * gLace.y;
        float2 wob = float2(sin(gDeep.w * 1.6 + i.world.x * 0.35),
                            sin(gDeep.w * 0.8 + i.world.y * 0.35)) * (0.06 * gLace.z);
        float2 uvA = luv + wob;
        float2 uvB = luv - wob * 0.7 + sn.xy * 0.15;
        // Each lookup crossfades between the two bound frames on the client's own animation
        // phase, so the sheet advances smoothly instead of stepping when the material swaps
        // frames underneath us. Four fetches instead of two, and the step was worth exactly that.
        float  lA = lerp(tex2D(sLaceA, uvA).a, tex2D(sLaceB, uvA).a, gLace2.x);
        float  lB = lerp(tex2D(sLaceA, uvB).a, tex2D(sLaceB, uvB).a, gLace2.x);
        float  lfade = saturate(1.1 - i.misc.y * gLace2.z);
        float  lace  = saturate((lA + lB) * 0.5 * (1.0 + fold * gLace.w));
        // The column fade is the sheet's PLACEMENT: zero keeps it out to open water, which is what
        // an ocean sheet wants; raised, it pulls the lace back to the shallows like surf.
        laceW = saturate(lace * lfade * gLace.x * saturate(1.0 - column * gLace2.y));
        // Lit by the same daylight as the body, floored the same way: lace at night is pale, not
        // luminous.
        color = lerp(color, float3(0.92, 0.96, 1.0) * (0.35 + 0.65 * entering), laceW);
    }

    // --- alpha -----------------------------------------------------------------------------------
    // With a scene copy the surface OWNS the composite: what is behind it has already been sampled
    // and put through the medium, so blending the frame in a second time underneath would count the
    // seabed twice and wash the colour out. Opaque is the honest output.
    //
    // Without one there is nothing behind, and the only way to suggest depth is to let the real
    // frame through: opacity then follows the same extinction as the colour, so the surface stops
    // being see-through for exactly the reason it stops being clear.
    float alpha = saturate(max(1.0 - dot(transmit, float3(0.33, 0.34, 0.33)), fresnel));
    // The grazing thin-out: a small subtraction where the view flattens, so the far water leans on
    // what shows through it rather than on its own paint. Neutral at zero, and deliberately small
    // at its reference value -- it is a lean, not a window.
    alpha *= 1.0 - gStyle.w * saturate(fresnel);
    alpha = saturate(max(max(alpha, max(foam, laceW)), gEye.y));

    // And a soft edge where the water runs out. A surface that stays fully opaque right up to its own
    // edge draws a hard line along the whole shore, the most recognisable tell of a water plane that
    // is a plane. Taken from the column, this fades the last third of a unit of depth -- so a beach
    // dissolves properly, while a rock standing in deep water still meets the surface on a hard line,
    // because nothing here knows the rock is there.
    alpha *= saturate(column * 3.0);

    color = lerp(gFogColor.rgb, color, saturate(i.misc.x));

    // --- debug view ------------------------------------------------------------------------------
    //
    // WHAT A SETTING IS ACTUALLY DOING, PAINTED ONTO THE WATER. Every quantity the shore, the foam
    // and the detail field are tuned against is invisible: nobody can see a water column, a fold, or
    // how much chop arrived somewhere. The panel's charts say what the RULES are; this says where
    // they land on the map in front of you, which is the half a chart cannot show.
    //
    // Selected without a branch. The weights are a one-hot vector uploaded from the panel, so the
    // quantities are all computed -- they were computed anyway, every one of these is a value this
    // shader already had -- and exactly one survives the dot. A branch here would cost more than the
    // arithmetic it skipped and would have to be predicated on every pixel regardless.
    //
    // Off, gDbg.x is zero, the lerp is the identity, and what remains is a dot product and a ramp:
    // some ten instructions on a shader with over a hundred to spare. Paying that always is the
    // price of the view being switchable at runtime rather than being a second shader to keep in
    // step with this one.
    float4 dbgQty = float4(column * gDbg.y,           // water column, scaled to the range in view
                           i.misc.z,                  // slope of the bottom underneath
                           fold,                      // where the surface is breaking
                           foam);                     // the white that came out of it
    float dbgT = saturate(dot(gDbgSel, dbgQty) + dot(gDbgSel2.xy, float2(dgain, mask)));

    // Blue through green and yellow to red. Three offset triangles, which is the cheapest ramp that
    // still has four distinguishable zones -- a greyscale one cannot be read off a photograph and a
    // two-stop one hides everything happening in the middle.
    float3 dbgCol = saturate(float3(1.5 - abs(4.0 * dbgT - 3.0),
                                    1.5 - abs(4.0 * dbgT - 2.0),
                                    1.5 - abs(4.0 * dbgT - 1.0)));
    color = lerp(color, dbgCol, gDbg.x);
    // Opaque under the debug view: a false colour read through a variable alpha is a false colour
    // that means two things at once, and the one it appears to mean is the wrong one.
    alpha = max(alpha, gDbg.x);

    // Opt-in diagnostic. R: flat-water alignment; G: animated-normal alignment;
    // B: source visibility after SSR and native fog. Opaque false color avoids
    // confusing transmission with these signals. No saved preference changes.
    if (gDbg.z > 0.5)
    {
        float3 flatRay = reflect(-v, float3(0,0,1));
        return float4(pow(saturate(dot(flatRay,gSun.xyz)),24.0),
                      pow(saturate(dot(r,gSun.xyz)),24.0),
                      reflectedSourceVisibility*saturate(i.misc.x),1);
    }
    return float4(color, alpha);
}
