// Water surface, vertex stage: displaces the map's flat liquid sheet by the shared wave table.
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
// Compile: fxc /T vs_3_0 /E main /Fh SurfaceVs.h /Vn kSurfaceVs Surface.vs.hlsl

#include "Wave.hlsli"
#include "Shore.hlsli"

// The client's own liquid block occupies c0..c45 and is uploaded for us on every liquid draw; the
// two matrices below are read straight out of it rather than re-sent, so the surface is guaranteed
// to sit in the same space as everything else in the frame.
//
// Both are consumed as mul(matrix, vector): the rows live in consecutive registers, which is the
// layout a dp4-per-row shader needs and the one the block is written in.
float4x4 gProj : register(c0);   // c0..c3
float4x4 gView : register(c5);   // c5..c8, modelview

/// THE WORLD'S OWN FOG, from the same block as the matrices and for the same reason.
///
/// Read rather than authored, because a sea that fades on its own schedule is a sea with an edge.
/// The client puts everything at full fog by around eight hundred units; water fading over a range
/// of our choosing stayed dark and detailed well past that, so it read as a patch of texture pasted
/// onto an already-blank distance -- and then the far plane cut it, leaving a step in colour at a
/// fixed distance, which perspective draws as a straight horizontal line across the sea.
///
/// x is -1/(end - start) and y is end/(end - start), so the factor is a single mad and comes out 1
/// near and 0 far. Taking both means the sea now fades exactly when the land does, follows the zone
/// and the weather with it, and has nothing left to tune.
float4 gFog : register(c4);

/// x: ceiling on the pinch gain. y: flat inland material flag. The depth ladder moved to gShoreA/gShoreB, where a
///    single number could no longer say what the shore does.
/// z: how much the detail crowds onto the crests of the big waves -- the ONE gain this stage still
///    applies to the field, and its ceiling (1 + z/2) is divided into the field's amplitude budget
///    on the CPU, which is what makes it safe to apply here.
/// w: half the reciprocal of the sea's own maximum rise, so the crowding reads the trains' rise as
///    a signed fraction whatever the weather has made of the amplitude.
float4 gSea : register(c46);
/// xyz: camera world position.  w: the detail layer's master switch, one or zero -- the known-good
/// fallback. The strength slider lives in the CPU normalisation, because a free gain here is a
/// gain the amplitude budget cannot see.
float4 gCamera : register(c47);

/// THE DETAIL FIELD: four cascades, one GEOMETRY texture each -- (rise, dx, dy) -- sampled rather
/// than evaluated, which is the whole difference between it and the trains. A sum of plane waves is
/// quasi-periodic however many terms it has; this is four thousand cells per cascade with
/// independently drawn phases, and nothing in it can line up.
///
/// One texture per cascade is the entire trade this stage lives on: vs_3_0 has exactly four vertex
/// samplers, so carrying the slope here too would halve the cascade count. The slope and the foam
/// travel in separate SHADING textures the pixel stage samples instead -- where bilinear filtering
/// exists, which a vertex fetch never has. The sideways pair is what turns a bump into a wave: a
/// height field cannot gather water, and gathering is what narrows a crest. Chop is already baked
/// into these texels, on the CPU, where the cusp clamp and the foam are measured on the same
/// displacement being drawn here.
///
/// Each cascade is periodic in its own patch -- unavoidable in a transform of a finite grid; four
/// patches with no common measure repeat together far past anything on screen.
sampler2D sGeo0 : register(s0);
sampler2D sGeo1 : register(s1);
sampler2D sGeo2 : register(s2);
sampler2D sGeo3 : register(s3);

/// 1 / patch size of each cascade.
///
/// Above the wave table at c48..c63, not below c46: the stock block runs to c45 and the client
/// refills all of it for its own draw, so anything of ours down there is overwritten between the
/// upload and the draw -- silently, and only sometimes.
float4 gDetailScale : register(c64);

/// Each cascade's representative wavelength, in world units, for the PORTABILITY fade below.
float4 gDetailLambda : register(c65);

/// x: how much of the tent reconstruction below is applied. One rebuilds the field between its
/// texels; zero collapses it to the nearest one, which is the hardware's own behaviour and the
/// switch's neutral value.
float4 gDetailFilter : register(c66);
// Validated WMO local-to-world matrix. Native gView already includes this model
// transform; use it only for shading coordinates, never twice for projection.
float4x4 gPoolWorld : register(c69);

/// The grid is kOceanN across in both axes, fixed on the CPU. Named here because the tent below
/// steps in texels and a wrong step is a half-cell shear nobody would read as a filter fault --
/// and it MUST match kOceanN, or every cascade shifts by the mismatch.
// Supplied from kOceanN, so a future owned-lattice shader cannot drift from allocation.
#define kGeoTexels gDetailFilter.y
#define kGeoTexel gDetailFilter.z

/// THE FIELD, RECONSTRUCTED RATHER THAN LOOKED UP -- and the difference is not subtle.
///
/// A vertex fetch is POINT sampled; D3D9 offers no filtering there at all. What that means is not
/// "slightly rougher": it means the reconstruction filter is a BOX, so every vertex landing inside
/// one cell receives the identical displacement and the surface comes out in flat plateaus one cell
/// across. The coarse cascades are where this lands, because their cell is set by the patch their
/// component count needs -- several times the lattice's own spacing -- so three or four vertices in
/// a row are pushed to exactly the same height. Lit by a normal that DOES interpolate (the shading
/// maps ride the pixel stage, where filtering exists), a flat plateau under a smoothly turning
/// normal reads as a raised disc: the surface covered in buttons.
///
/// The information is not missing. A cascade is band-limited to wavelengths several times its own
/// cell, so its samples are far above Nyquist -- what was wrong was only the filter used to read
/// them back. A tent costs three extra taps and restores the signal that was always in the grid.
///
/// Raising the grid instead would cost a transform four times the size, every frame, on the thread
/// the client renders from -- to recover something the samples already contain.
float3 SampleGeo(sampler2D s, float2 uv, float blend)
{
    // D3D9 addresses a texel by its CORNER, so texel n is centred at (n + 0.5)/N. The tent has to be
    // built around those centres or it interpolates between the wrong pair and shifts the whole
    // field half a cell.
    float2 t = uv * kGeoTexels - 0.5;
    float2 f = frac(t) * blend;
    float4 b = float4((floor(t) + 0.5) * kGeoTexel, 0.0, 0.0);

    float3 a00 = tex2Dlod(s, b).xyz;
    float3 a10 = tex2Dlod(s, b + float4(kGeoTexel, 0.0, 0.0, 0.0)).xyz;
    float3 a01 = tex2Dlod(s, b + float4(0.0, kGeoTexel, 0.0, 0.0)).xyz;
    float3 a11 = tex2Dlod(s, b + float4(kGeoTexel, kGeoTexel, 0.0, 0.0)).xyz;

    // Wrapped by the sampler, which is correct rather than merely convenient: the patch IS the
    // period, so the texel past the last one is the first one and no edge case exists.
    return lerp(lerp(a00, a10, f.x), lerp(a01, a11, f.x), f.y);
}

struct VsIn
{
    float4 pos : POSITION;    ///< still-water point, already in world space for map liquids
    /// Not a normal. .x is the water column in world units, .y the slope of the bottom under it,
    /// .z MINUS the local vertex spacing of the refined lattice -- all baked per vertex where the
    /// lattice is built, because the sheet's own normal is a constant up-vector nothing reads. See
    /// Lattice.cpp.
    ///
    /// THE MINUS IS LOAD-BEARING. Not every sheet reaching this stage has been through that bake:
    /// water the refinement never caught, and every sheet the surrounding geometry supplies, arrive
    /// carrying a plain up-vector -- a positive z, and an x that is not a shallow column but no
    /// column at all. Reading that zero as a measurement put those sheets at the waterline, which
    /// the ladder answers with full surf: a solid white plateau with hard straight edges, sitting in
    /// open water. The sign costs nothing and tells the two apart exactly.
    float3 nrm : NORMAL;
    float4 col : COLOR0;
    float2 uv0 : TEXCOORD0;   ///< the client's liquid coordinate
    float2 uv1 : TEXCOORD1;
};

struct VsOut
{
    float4 clip   : POSITION;
    float4 world  : TEXCOORD0;   ///< xyz displaced world position, w water column
    float4 normal : TEXCOORD1;   ///< xyz TRAINS' surface normal, w their fold (shoal-break signal)
    float4 screen : TEXCOORD2;   ///< xyz projective screen coordinate, w the detail layer's gain
    float4 misc   : TEXCOORD3;   ///< x fog, y view distance, z slope of the bottom, w rise of the wave
    /// xy: the STILL-WATER position this vertex started from. The pixel stage fetches the detail
    /// slope and foam at this coordinate -- the same one the geometry was displaced from -- because
    /// sampling at the displaced position would slide the lighting sideways off its own waves by
    /// the swell's sway.
    float4 aux    : TEXCOORD4;
};

VsOut main(VsIn i)
{
    float3 base = i.pos.xyz;

    // Was this vertex measured by us at all? See VsIn.nrm.
    // LatticeCompat only supplies stock geometry. Compile out the owned-lattice
    // displacement fetches; the pixel shader still samples animated detail normals.
    const float mine = 0.0;
    // Spacing is carried negated; everything downstream wants its magnitude.
    const float space = abs(i.nrm.z);

    // AN UNMEASURED COLUMN IS A DEEP ONE, which is the same choice the lattice makes when the
    // terrain grid cannot answer. Deep costs a wave running slightly too tall over a patch nobody
    // surveyed; shallow costs the entire sheet being drawn as breaking surf, and the two are not
    // remotely the same size of mistake.
    const float depth = lerp(kUnmeasuredDepth, max(i.nrm.x, 0.0), mine);

    // WHAT THE BOTTOM ALLOWS HERE. One ladder, evaluated once, feeding the trains, the detail field
    // and the foam alike -- see Shore.hlsli. It replaces a single depth ramp that scaled everything
    // by the same number, which is what made a shoreline read as the whole sea being turned down
    // rather than as waves arriving on a beach.
    ShoreResponse shore = ShoreAt(depth, max(gSea.x, 1.0));

    WaveSurface w = EvaluateWaves(base.xy, shore.height, shore.pinch);

    // Read at the UNDISPLACED position, not at the crest the trains moved this vertex to. Sampling
    // where the point ended up would drag the whole field sideways with the swell -- the chop would
    // slide across the water it is supposed to be part of, which is the one artefact that gives away
    // a detail layer as separate from the sea under it.
    //
    // Attenuated by the same shoaling as the trains: chop does not survive into the shallows any
    // better than a wave does, and letting it run in at full strength would put a metre of ripple on
    // water half a metre deep.
    const float2 uv0 = base.xy * gDetailScale.x;
    const float2 uv1 = base.xy * gDetailScale.y;
    const float2 uv2 = base.xy * gDetailScale.z;
    const float2 uv3 = base.xy * gDetailScale.w;

    // BORN ON THE BIG WAVE, not spread evenly over it -- and driven by the trains' RISE, not their
    // fold. The support carries long swells that never approach their cusp, so the fold is all but
    // silent in open water now; what the short waves actually ride is the crest under them. Real
    // short waves are strained by the long wave they sit on: they crowd on its crest and go quiet
    // in the trough, so they appear and disappear as it passes -- which is what makes them read as
    // waves FORMING ON the swell rather than as a texture it carries.
    //
    // A REDISTRIBUTION, NOT A GAIN. The rise term is signed -- gSea.w folds the sea's own maximum
    // in, so it spans [-1/2, 1/2] whatever the weather -- and the ceiling (1 + coupling/2) is
    // divided into the field's CPU amplitude budget. Raising the coupling cannot push the worst
    // case past the cusp; it moves height from the troughs to the crests.
    const float born = max(1.0 + gSea.z * (w.offset.z * gSea.w), 0.0);

    // One number for how much of the field reaches the geometry, so the rise and the sideways
    // slide can never disagree about how strong the layer is. Everything in it is accounted for by
    // the CPU budget: the gate is binary, the attenuation only reduces, and the coupling's ceiling
    // is divided into the field's normalisation.
    // The field rides its OWN shore band, which ends further out than the swell's: short waves feel
    // a bottom the swell still passes over, and letting the two die together is what made a
    // shoreline look like one master volume coming down.
    const float detailGain = gSea.y > 0.5 ? 0.0 : gCamera.w * shore.detail * born;

    // The split-core compatibility path renders the client's stock liquid lattice. Its normal is
    // an ordinary normal, not the custom negative-Z metadata marker used by the owned lattice, so
    // `mine` is zero. Keep the animated detail in the pixel stage but do not use it to translate
    // stock vertices: without measured depth and boundary refinement, sideways displacement moves
    // the authored water footprint over the terrain at every shoreline.
    const float geometryDetailGain = detailGain * mine;

    // THE GEOMETRY ONLY RECEIVES WHAT THE MESH CAN DRAW; everything shorter lives in the normal.
    //
    // A wave needs several vertices per period to be restituted as a wave. Under about four, each
    // vertex receives a value nearly decorrelated from its neighbour's -- not a travelling wave any
    // more but per-vertex noise, and animated noise reads as the surface SEETHING. The field
    // itself is sampled correctly on its own grid; it is the lattice that cannot carry it, and the
    // lattice's spacing varies with the distance grading, so the fade has to be per vertex and per
    // cascade: nrm.z is the local spacing, and each cascade compares its wavelength against it.
    // Zero at four spacings, full at six.
    //
    // Nothing is lost, only relocated: the pixel stage samples these same cascades' slope and foam
    // at PIXEL resolution, where no lattice exists to alias against -- so a population that leaves
    // the geometry here continues as light, which is where a too-short wave belonged all along.
    const float4 portable = saturate(gDetailLambda * (0.5 / max(space, 0.25)) - 2.0);

    // (rise, dx, dy) per cascade, each weighted by what the mesh can restitute here. Chop is
    // already inside these texels -- baked on the CPU, where the cusp clamp and the foam are
    // measured on the same displacement being drawn. The slope is deliberately NOT fetched in this
    // stage: it rides the shading textures the pixel stage samples with bilinear filtering, which
    // a vertex fetch never has.
    //
    // Read through the tent above rather than sampled directly, because the hardware's own
    // reconstruction here is a box -- see SampleGeo for what that costs the coarse cascades.
    const float f = gDetailFilter.x;
    const float3 d = SampleGeo(sGeo0, uv0, f) * portable.x
                   + SampleGeo(sGeo1, uv1, f) * portable.y
                   + SampleGeo(sGeo2, uv2, f) * portable.z
                   + SampleGeo(sGeo3, uv3, f) * portable.w;

    float3 world  = base + float3(w.offset.xy * mine, w.offset.z);
    world.z  += d.x * geometryDetailGain;
    world.xy += d.yz * geometryDetailGain;
    // Inland map water keeps its authored height and footprint. Zero its wave
    // normal/fold/rise too, so disabling geometry cannot leave ocean surf shading.
    if (gSea.y > 0.5)
    {
        world = base;
        w.offset = 0.0;
        w.normal = float3(0.0, 0.0, 1.0);
        w.fold = 0.0;
        shore.surf = 0.0;
    }
    float4 viewPos = mul(gView, float4(world, 1.0));
    float4 clip    = mul(gProj, viewPos);

    if (gSea.y > 1.5)
    {
        world = mul(gPoolWorld, float4(base, 1.0)).xyz;
        base = world;
    }
    VsOut o;
    o.clip   = clip;
    o.world  = float4(world, depth);
    // The trains' normal, and the BREAK SIGNAL the pixel stage reads as white. Two sources, taken
    // at their larger: the fold, which is the surface's own geometry turning through itself, and
    // the depth cap, which is the water refusing the wave the height it asked for.
    //
    // The second is not redundant. A wave held down by the cap has had exactly the geometry that
    // would have folded taken away from it, so the fold goes QUIET in the surf -- the one place in
    // the scene that should be solid white. Reading both is what puts foam where the wave broke
    // rather than only where it was steep enough to fold on its own.
    o.normal = float4(w.normal, max(w.fold, shore.surf * gShoreB.w));

    // Screen coordinate carried projectively, so the pixel stage divides by w after interpolation
    // rather than interpolating an already-divided value -- which would bend across a triangle and
    // put the refraction lookup somewhere other than under the pixel asking for it. The spare
    // channel carries the detail gain down to the pixel stage, so the slope and the foam it samples
    // are scaled by exactly the gain the geometry was.
    o.screen = float4((clip.x + clip.w) * 0.5, (clip.w - clip.y) * 0.5, clip.w, detailGain);

    // The stock liquid emitter stores its authored depth/opacity ramp in uv0.y. Keep it separate
    // from the true terrain column: the pixel stage uses only its spatial edge to place coastal
    // foam, so a placement with no ramp (constant zero) remains safely quiet instead of turning
    // the entire sea into a white shelf. aux.w says whether the richer owned metadata was present.
    // WMO uv0 is a texture coordinate, not the ADT depth/opacity ramp.
    // Keep its authored-ramp foam off; captured depth still supplies contact foam.
    o.aux = float4(base.xy, gSea.y > 1.5 ? 0.0 : saturate(i.uv0.y), mine);

    float dist = length(world - gCamera.xyz);
    float fog  = saturate(gFog.y + dist * gFog.x);
    // The rise, which the pixel stage reads as THICKNESS. Light entering the back of a wave has less
    // water to cross near the crest than down in the trough, so a crest glows and a trough does not
    // -- and that glow is most of what separates a sea that looks lit from a sea that looks painted.
    // Signed and unscaled: the pixel stage knows the sea state, this does not.
    o.misc = float4(fog, dist, i.nrm.y, w.offset.z);

    return o;
}
