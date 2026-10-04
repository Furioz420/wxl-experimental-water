// The detail field: a spectrum inverted by FFT, laid over the sea rather than replacing it.
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

#include "wxl-experimental-water/sea/Spectrum.hpp"

// TWO LAYERS, ONE SEA, AND ONLY ONE OF THEM IS EVER ASKED A QUESTION.
//
// The Gerstner spectrum next door is THE SEA. It carries the swell and the wind waves -- everything
// long enough to lift a hull -- and it is the only thing buoyancy or the submersion test may
// consult. It is a closed form of (state, seed, time), so every client computes it
// identically, and that is what lets waves matter to gameplay without replicating a height field.
//
// This is not that. This is EVERY WAVE THE EYE READS AS ONE, and it is deliberately
// non-authoritative: it lives in the vertex stage and in the shading and nowhere else, and nothing
// asks it where the water is. The trains are the heave a hull rides; the shapes standing and
// breaking on that heave -- forming, crossing, passing -- are all this field's.
//
// WHY THAT SPLIT IS THE WHOLE POINT. A finite sum of plane waves is quasi-periodic -- eight trains
// or a hundred, the surface is a lattice, and raising the count refines the lattice without ever
// dissolving it. Open water has no lattice because it has no list: its energy is spread over a
// CONTINUUM, and the way to have a continuum on a computer is to give every cell of a frequency grid
// its own drawn phase and transform the whole grid at once. Four thousand phases drawn independently
// have nothing to line up with.
//
// Laying that over the trains rather than in place of them keeps both properties at once: the sea a
// boat floats on stays a formula anyone can evaluate anywhere, and the sea an eye looks at stops
// repeating. Replacing the trains would have bought the second at the cost of the first.
//
// THE AMPLITUDE BUDGET IS SET BY WHAT FLOATS, not by the waves underneath. Physics reads the trains
// only, so anything added here is relief the hull does not know about: half a unit reads as a boat
// in chop, three units reads as a boat being swallowed. If that ceiling ever needs lifting, the
// answer is not to raise it -- it is to let buoyancy sample this field too, over the footprint of
// the hull rather than at a point, which filters out everything shorter than the boat by geometry
// instead of by a number someone has to guess.
namespace wxl::water
{
    // Retail uses the detail grids for shading, not sculpted wave geometry.
    // Four 64-square grids retain all bands at one quarter of the update/upload
    // texels of the experimental 128-square configuration.
    inline constexpr int kOceanN = 64;

    /// Cascades: the same spectrum sampled over several patch sizes, summed -- and each one is a
    /// POPULATION, not a resolution step.
    ///
    /// Four, up from two, and the raise is what "new kinds of wave" means in practice: two cascades
    /// over one band read as a single texture of relief, while four bands a factor of two apart --
    /// forty-unit waves down to four-unit chop -- are systems the eye can tell apart, each with its
    /// own directionality and its own drift rate. Four is also the hard ceiling: one vertex sampler
    /// per cascade, and vs_3_0 has exactly four.
    ///
    /// One patch tiles, and visibly: it is periodic by construction and the period is the patch.
    /// Patches whose sizes share no common measure tile at their common multiple, far past anything
    /// on screen. The band split keeps them from carrying the same wavelengths twice, which would
    /// double that energy and leave a hole elsewhere.
    inline constexpr int kOceanCascades = 4;



    /// What the detail field is made of, live.
    ///
    /// Separate from SeaConditions on purpose: those describe the SEA, and every one of them feeds
    /// something that gameplay reads. Nothing here does. Mixing the two would eventually put one of
    /// these in front of a buoyancy query by accident, which is the exact failure the split exists
    /// to make impossible.
    struct OceanTuning
    {
        /// Largest rise this layer may add, in world units.
        ///
        /// A ceiling, not a target, and the one number here with a hard reason behind it: physics
        /// reads the trains only, so everything this adds is relief the hull does not know about.
        /// Half a unit reads as a boat in chop; three units reads as a boat being swallowed.
        ///
        /// Raising it is allowed and will look bigger. What it costs is that anything floating stops
        /// agreeing with the water under it, and the answer to wanting more is not this slider -- it
        /// is letting buoyancy sample the field too, over the footprint of a hull rather than at a
        /// point, which filters out everything shorter than the boat by geometry rather than by a
        /// number someone has to guess.
        ///
        /// Conservative at the shipped baseline because the stock lattice cannot carry fine
        /// geometric chop without seething. The pixel stage still shades this entire band, and the
        /// tuner can raise its geometry budget for verified denser meshes. The steepness caps still
        /// hold each cascade to what its band can carry -- the panel says what each one got.
        float amplitude = 0.30f;

        /// Shortest wavelength the field may carry.
        ///
        /// A MESH constraint. Below a few vertices per period the wave is sampled under its own rate
        /// and what reaches the screen is the beat against the lattice rather than the wave. The
        /// pixel stage already covers everything finer with the sampled ripple normals, where there
        /// is no lattice to beat against.
        float shortest = 4.0f;

        /// Longest wavelength it may carry. Zero means "wherever the trains start".
        ///
        /// ZERO IS A TRAP AND IS NO LONGER THE DEFAULT. It only makes sense while the trains' own
        /// floor is high; with a floor at five units there is nothing between there and the mesh
        /// limit for this to carry, and the field goes quiet for a reason that looks like a bug.
        ///
        /// Overlapping the trains is fine, which is the correction to what this comment used to say.
        /// Two independent random fields over the same band ADD -- their variances add -- they do not
        /// cancel. What an overlap costs is energy counted twice in that stretch, which is a loudness
        /// to be tuned, not an artefact to be avoided.
        ///
        /// Reaches to the trains' own floor by default: with the support handing over at several
        /// tens of units, this field owns everything the eye reads as a wave.
        float longest = 40.0f;

        /// Multiplier on every cascade's tile span, at 1 the span its component count needs.
        ///
        /// This replaced a cell-size-in-units slider that had died twice over: clamped at the
        /// mesh's finest vertex spacing, and overridden even below that by the span the mode
        /// target demands -- the cell that ships has been patch/kOceanN for some time, whatever
        /// the slider said. As a span multiplier it buys the two things worth buying at build
        /// time: the repeat pushed further out, and independent components by the SQUARE of it.
        ///
        /// The cost is cell size -- the grid covers more world, so each texel covers more of it
        /// -- which the shading does not show (the normal rides the pixel stage's filtered path)
        /// but the GEOMETRY eventually does: past a couple of times the mesh's spacing, crest
        /// shapes stop gaining detail from the grid. The panel's per-cascade cell readout is the
        /// number to watch.
        float cellSize = 1.0f;

        /// How hard each cascade combs into the wind, in [0, 1] -- PER CASCADE, because this is
        /// half of what makes the populations read as different seas rather than one texture at
        /// four sizes. The long waves hold the wind's line; the short chop is confused and crosses
        /// them, which is what open water does.
        ///
        /// Zero is isotropic -- chop with no preferred heading, which is what a confused sea over a
        /// shoal looks like. One is fully aligned, and is wrong on its own: a hard zero across the
        /// wind leaves a seam of dead water running through the field along a single axis, far more
        /// visible than the anisotropy it was expressing.
        float align[kOceanCascades] = { 0.88f, 0.74f, 0.58f, 0.42f };

        /// Per-cascade multiplier on the drift rate, over the dispersion and the shared timeScale.
        ///
        /// The dispersion already makes short waves slower in absolute terms; this is the styling
        /// thumb that stops the populations from feeling locked to one another -- slightly
        /// different rates mean the systems visibly travel THROUGH each other instead of riding as
        /// one sheet.
        float speed[kOceanCascades] = { 0.85f, 0.90f, 0.96f, 1.04f };

        /// Falloff of energy with wavenumber. Four is the equilibrium range every textbook gives.
        ///
        /// Lower puts more into the short end and reads busier and grainier; higher concentrates it
        /// at the long end of the band and reads smoother and swellier. It is the shape of the field
        /// rather than its size, and the two are worth separating.
        float falloff = 4.0f;

        /// How long the crests run, as the power on the directional lobe. One is the classic
        /// square.
        ///
        /// THE SQUARE IS WHY THE FIELD READ AS BUBBLES. Energy forty-five degrees off the wind
        /// still carries half its weight under it, and systems crossing at that angle chop every
        /// front into pieces about one wavelength long -- a sea of wavelength-long crests is a
        /// sheet of round bumps whatever the chop and the peaking do to their profiles. Raising
        /// the power narrows the lobe and stretches the crests crosswind into fronts several
        /// wavelengths long: waves that ARRIVE rather than water that boils.
        ///
        /// Applied inside each cascade's alignment blend, so the confused floor survives: short
        /// chop stays confused, the long populations grow the fronts. Costs nothing per frame --
        /// it shapes the spectrum at build.
        float crestLength = 3.0f;

        /// Energy left to the half of the spectrum travelling AGAINST the wind, in [0, 1].
        ///
        /// One is the symmetric spectrum, and symmetry was the other half of the bubbles: an even
        /// directional lobe weighs a wavevector and its opposite identically, so every component
        /// is born paired with an equal one running the other way -- and two equal waves running
        /// opposite ways are a STANDING wave, lobes inflating and deflating in place instead of
        /// travelling. Half the field's energy was spent pulsing.
        ///
        /// Not zero by default: a few percent of backwash keeps the surface from reading as a
        /// conveyor belt carrying identical fronts.
        float upwind = 0.08f;

        /// How fast this layer runs, against the trains.
        ///
        /// Its own multiplier because chop that moves at the same rate as the swell under it looks
        /// glued to it. Short waves genuinely travel slower -- sqrt(g/k) -- and that is already in
        /// the dispersion; this is the stylistic thumb on top of it.
        float timeScale = 0.55f;

        /// How much of the field reaches the geometry, below the ceiling above.
        ///
        /// Applied on the CPU, where the amplitude budget is computed, and NOT in the vertex stage.
        /// That placement is the point: a gain applied after the budget is a gain the budget cannot
        /// see, and an earlier version of this slider lived exactly there -- one of the four
        /// multipliers whose product shattered the surface while every factor looked reasonable on
        /// its own knob. Here it is an input to the same normalisation that enforces the steepness
        /// cap, so raising it past what the band can carry changes the panel's "granted" readout
        /// and nothing else.
        float strength = 0.65f;

        /// Its own seed. Independent of the trains' on purpose: tying them would mean a wind shift
        /// that reseeds one silently reseeds the other.
        uint32_t seed = 0x0CEA0FFFu;

        /// How hard the crests PEAK, at 0 linear. The first of the two nonlinear terms.
        ///
        /// A transform of a Gaussian spectrum comes out Gaussian, and a Gaussian surface is ROUND --
        /// as much water above the mean as below it, and the same shape either way up. Real water is
        /// nothing like that: a peaked crest over a long flat trough.
        ///
        /// This is the second-order Stokes correction, which is the first term of the feedback the
        /// linear model throws away. Squaring the field produces its second harmonic, and that
        /// harmonic adds height to the crest while taking depth from the trough -- the "mountain"
        /// profile, from one multiply per cell.
        ///
        /// Past about 0.6 the crests grow past the amplitude ceiling, which is measured on the linear
        /// field and knows nothing about this. Nothing breaks; the sea is simply taller than the
        /// slider says.
        float stokes = 0.12f;

        /// How much harder a tall crest gathers than a small one, at 0 uniformly. The second one, and
        /// the one that makes waves run into each other.
        ///
        /// Everything else in this file moves the surface by an amount that depends on WHERE a point
        /// is and never on how tall it is there, so the whole field slides past rigidly: the same
        /// shapes arriving forever. Tie the gathering to the local height and a tall crest pinches
        /// harder than a small one, so the two distort each other as they pass rather than holding
        /// station side by side. That is the collision, and it is the only thing here that no sum of
        /// fixed components can reproduce at any count, in either model.
        ///
        /// A GAIN ON THE DISPLACEMENT, NOT A PUSH OF ITS OWN. The textbook form shifts every point
        /// along the wind in proportion to its height, and here that is wrong: one direction shared
        /// by the whole field turns a smooth height field into a smooth COHERENT shove, which does
        /// not shear the surface but translates it, by an amount that wanders across the world. Under
        /// Gerstner crests hundreds of units long it reads as those crests snaking. Scaling what is
        /// already there keeps each displacement on its own component's heading, which is as
        /// disordered as the spectrum and cannot line up into a direction.
        ///
        /// One means a three-sigma crest gathers twice as hard as flat water, and a trough of the
        /// same depth not at all.
        ///
        /// A gain on a displacement that already carries `chop`, so at a chop of zero this does
        /// nothing either -- and the cusp clamp runs after it, so what it amplifies is still bounded
        /// where it becomes geometry.
        float lean = 0.20f;

        /// How close to the fold the whole field may run, at 1 the engineered ceiling.
        ///
        /// The ceiling prices the WORST case of every gain -- a coincidence the sea shows for
        /// moments an hour -- so at one, a typical crest stands about six times back from folding
        /// and the field reads rounder than any other slider can fix. That distance is the safety,
        /// and it is also the roundness. Raising this scales every cascade's height budget and its
        /// pinch bound together; past about 1.3 the deepest crests begin to GENUINELY fold, and
        /// the cost is honest: the foam is measured on that same fold, so what it buys is white
        /// lying exactly where the surface broke. This is the crumple slider -- the one number
        /// allowed to trade artefacts for shape, and the granted readouts include it.
        float foldBudget = 0.55f;

        /// How far the surface slides sideways under the field's own crests, at 0 vertical only.
        ///
        /// THIS IS WHAT MAKES IT 3D. Vertical motion alone gives crests as broad as troughs -- a
        /// height field cannot gather water, only lift it, so what comes out is a rumpled sheet
        /// rather than waves. Real water creeps forward under a crest and back under a trough, and
        /// that gathering is what narrows and sharpens it. Same mechanism as the trains' pinch.
        ///
        /// Baked into the displacement on the CPU, where the cusp clamp and the foam's fold can see
        /// it -- never multiplied on afterwards by the shader, which is how it once escaped both.
        /// Asking past what the band can carry is legitimate: the geometry stays at its cusp and
        /// the panel's "at the limiter" readout says how much of the field is being held there.
        /// The fold is also what the foam below is measured from, so the two knobs are coupled:
        /// more chop is more white.
        float chop = 0.25f;

        /// How far into the fold a place has to be before it goes white, in [0, 1).
        ///
        /// FOAM IS A FOLD, NOT A SLOPE. Driving it from the surface's steepness put white on every
        /// steep texel independently of its neighbours, and since the field is sampled without
        /// filtering that came out as isolated specks -- dots, not foam. What actually breaks is
        /// where the horizontal displacement folds the surface through itself, and that is a
        /// CONNECTED region: it runs along the front of a crest, because that is where the gathering
        /// happens. Same quantity the trains already use for their own fold, applied to this field.
        ///
        /// Low puts white over most of the sea; high keeps it to the few crests actually collapsing.
        float foamThreshold = 0.72f;

        /// How long white stays after the fold that made it has passed, in seconds to near-nothing.
        ///
        /// Foam without memory appears and vanishes with the wave that made it, which reads as a
        /// flicker rather than as foam -- and it is the other half of why the old signal looked like
        /// specks. Real foam outlives its wave by a good few seconds, drifting and thinning, and that
        /// lag is most of what makes a breaking sea legible: you see where it broke, not only where
        /// it is breaking.
        float foamLife = 3.5f;

        /// How much the detail crowds onto the faces of the big waves, at 0 spread evenly.
        ///
        /// Chop laid down at the same strength everywhere reads as a texture the swell is carrying:
        /// correct in every detail and inert, because nothing about it belongs to the wave beneath.
        /// Real short waves are raised by the face they sit on -- they crowd where it is steep and
        /// go quiet in the trough, so they appear and disappear as the big wave passes.
        ///
        /// A REDISTRIBUTION, NOT A GAIN. Written to only ever multiply up, it became a silent third
        /// multiplier on a layer whose amplitude and chop were being raised at the same time -- three
        /// numbers that each read as reasonable on their own slider and whose product was a wall of
        /// folded geometry. Centred on the middle of the fold's range it takes from the troughs what
        /// it gives the faces, and its ceiling is half of this rather than all of it.
        float waveCoupling = 0.25f;

        /// How much the field's own steepness drives the white.
        ///
        /// This is where whitecaps come from now. Zero returns the foam to the trains' fold alone,
        /// which is the old behaviour and which stopped making sense once they became the support:
        /// a fifty-unit swell at moderate steepness does not break.
        float foamGain = 0.20f;

        /// WHAT FULL STORM DOES TO THIS LAYER, as multipliers on the four numbers a gale actually
        /// changes -- one everywhere is "the weather changes nothing", which was the old behaviour
        /// and read as a sea that had stopped listening: the trains have carried a calm and a
        /// storm response from the start, while this field behaved identically under every sky.
        ///
        /// Blended by the same shared storm intensity the trains mix on, so every client computes
        /// the same water; between calm and full storm each multiplier is approached linearly.
        /// The amplitude and the fold budget are build-time inputs, so the storm moving re-runs
        /// the build -- on the same slow clock as the wind shifting, which already does.
        float stormAmplitude = 5.0f; ///< height ceiling, at full storm
        float stormChop = 2.0f;      ///< sideways gather -- a gale's sea is a pinched sea
        float stormFold = 1.5f;      ///< fold budget -- storms genuinely break
        float stormFoam = 1.30f;     ///< divides the foam threshold: white arrives sooner
    };

    OceanTuning& OceanSettings();

    /// What one cascade ASKED for and what the physics GRANTED it, plus why.
    ///
    /// This exists because every ceiling in this layer is allowed to refuse a request, and a
    /// refusal that only shows as a quieter sea is indistinguishable from a broken transform. The
    /// panel renders these numbers; nothing in here is consumed by the simulation itself.
    struct OceanCascadeReport
    {
        float bandLong = 0.0f;    ///< wavelengths this cascade carries, world units
        float bandShort = 0.0f;
        float patch = 0.0f;       ///< world units before it repeats
        float cell = 0.0f;        ///< world units per texel -- the faceting size, VTF is unfiltered
        int   modes = 0;          ///< independently drawn components inside the band
        float askedRise = 0.0f;   ///< ceiling share x strength, world units
        float grantedRise = 0.0f; ///< after the worst-case steepness cap
        float gainCeiling = 1.0f; ///< largest gain the vertex stage can still apply (coupling)
        float clampedFrac = 0.0f; ///< fraction of cells whose sideways slide hit the cusp clamp
    };

    const OceanCascadeReport& OceanReport(int cascade);

    /// Rebuilds the base spectrum for these conditions.
    ///
    /// Costly enough not to want per frame and pointless per frame: the spectrum is what the weather
    /// IS, and the weather moves in minutes. Only the wind's speed and heading are read from the
    /// conditions -- the height, the steepness and the swell all belong to the trains.
    void OceanBuild(const SeaConditions& conditions);

    /// Advances the field to `timeSeconds` on the shared clock and inverts it. Once per frame.
    void OceanEvolve(double timeSeconds);

    /// The shared storm intensity, in [0, 1], blended into the tuning's storm multipliers.
    ///
    /// Pushed from the sea's tick rather than pulled, so this file keeps no view of the weather
    /// system -- the same direction every other shared input already flows.
    void OceanStorm(float intensity);

    /// A cascade's GEOMETRY texels: (rise, dx, dy, 0), kOceanN^2, row-major, wrapping both axes.
    /// Everything the vertex stage needs and nothing it does not -- which is what buys four
    /// cascades out of four vertex samplers. The sideways pair is what turns a bump into a wave:
    /// vertical motion alone leaves crests as broad as troughs, because a height field has no way
    /// to GATHER water. Chop is already baked in.
    const float* OceanGeoMap(int cascade);

    /// A cascade's SHADING texels: (dZ/dx, dZ/dy, foam, 0), same grid.
    ///
    /// Consumed by the pixel stage, never the vertex stage, and the split is the point: the pixel
    /// samplers have bilinear filtering where a vertex fetch has none, so the normal -- where a
    /// flat step is far more visible than in the height -- comes out smooth, and the foam arrives
    /// already softened. Whitecaps live in the third channel because they are a short-wave event,
    /// and short is what this field is.
    const float* OceanShadeMap(int cascade);

    /// World units a cascade spans before it repeats. Derived from its band and the tile span rather
    /// than authored, because the two used to be independent numbers that had to be kept consistent
    /// by hand -- and were not: a handover point expressed in patch sizes against band limits
    /// expressed in wavelengths can leave a cascade with an empty intersection and no warning.
    float OceanPatch(int cascade);

    /// Largest rise the field can currently reach. Bounded by the tuning's ceiling above, and
    /// reported because the culling bounds have to cover it as well as the trains.
    float OceanMaxRise();
}
