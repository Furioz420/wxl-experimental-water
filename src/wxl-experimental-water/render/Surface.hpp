// What the water looks like: the values the surface is shaded from.
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

namespace wxl::water
{
    // Existing render-thread master state. No settings mutation or shader rebuild.
    bool EnhancementsEnabled();
    /// The look, separated from the motion. Everything here is safe to change at any moment: none of
    /// it feeds a query, so none of it can put two clients on different water.
    struct SurfaceLook
    {
        /// How fast each channel is removed per world unit travelled through the medium.
        ///
        /// This is the ONE control over depth colour. Shallow-to-deep is not a gradient between two
        /// painted colours here -- it falls out of these three numbers, which is why the water reads
        /// as a volume rather than as a sheet with a ramp on it. Red goes within a few metres, blue
        /// survives tens: that spread is the whole reason deep water is blue.
        float extinction[3] = { 0.24f, 0.062f, 0.031f };
        /// Longest path considered. Past it the medium is already opaque and the exponential is only
        /// costing precision.
        float maxPath = 180.0f;

        /// Multiplier on the shallow-water bottom colour taken from the zone palette.
        float bottomBrightness = 1.15f;

        /// Reflectance straight on. Water is 0.02; the surface is a mirror only at grazing angles,
        /// and that contrast is most of what makes it read as water and not as metal.
        float fresnelF0 = 0.02f;

        /// Blinn-Phong exponent and intensity for the sun. The exponent is the widest the lobe ever
        /// gets, reached only where the pixel sees one smooth patch of water; the pixel stage opens
        /// it further wherever more surface than that falls inside a pixel.
        float specularPower = 90.0f;
        float sunIntensity = 0.9f;
        /// Widens the specular lobe as the sea state rises: a storm gets a broad sheen, calm water a
        /// tight highlight that moves.
        float roughness = 0.25f;

        // THERE IS NO SUN DIRECTION HERE EITHER, and for the same reason there is no fog distance.
        //
        // An authored sun agrees with itself and with nothing else: the highlight, the glints and the
        // through-glow all pointing one way while every shadow on the shore points another. And it
        // does not move -- the sun crosses the sky all day and the sea keeps its noon highlight.
        //
        // It is read out of the stock vertex block instead, where the client posts it in view space
        // for its own lighting. See ReadSunDir in Surface.cpp, and SunDir() below to look at it.

        /// How much of the fold and shore signal reaches the white.
        float foam = 1.0f;

        // There is no detail strength here any more. It lives in OceanTuning, because it is an
        // INPUT to the amplitude budget now rather than a gain applied after it -- a strength
        // multiplied on in the vertex stage was one of the four factors whose product shattered
        // the surface while every knob looked reasonable on its own.

        /// The two scales the ripple normal map is sampled at, and how hard it bends the surface.
        ///
        /// KEEP THEM WITHOUT COMMON MEASURE, for the same reason the foam scales are: each layer
        /// tiles, and the only thing stopping the tile from being visible is that the two never
        /// repeat together.
        float rippleScale[2] = { 0.021f, 0.049f };
        float rippleStrength = 0.36f;

        /// Light that crosses a crest rather than bouncing off it, and the colour it comes out as.
        ///
        /// Greener and lighter than the body: a short path through water keeps the wavelengths a
        /// long one loses. This is the single most recognisable thing about a stylised sea, and no
        /// amount of reflection or specular will stand in for it -- a wave lit only from the front
        /// reads as a solid object with water painted on.
        float glow[3] = { 0.30f, 0.75f, 0.62f };
        float glowStrength = 0.55f;

        /// Hard white sparks on far water, where thousands of facets fall inside one pixel and the
        /// few turned exactly right return almost the whole sun. Fades IN with distance, unlike
        /// everything else here: it is an artefact of many surfaces per pixel, so it cannot exist up
        /// close where there is only one.
        float glintStrength = 0.45f;

        /// World scale of the noise the sparks are seeded from.
        ///
        /// THE ONE TO REACH FOR WHEN THE SEA LOOKS LIKE STATIC. Too fine and one texel of the seed
        /// covers less than a screen pixel, at which point the threshold below is deciding
        /// white-or-black on sub-pixel noise and the result is speckle rather than sparks.
        float glintScale = 0.035f;
        /// What the seed plus the sun lobe must exceed for a spark to exist. Higher is rarer.
        float glintThreshold = 1.02f;
        /// How abruptly that threshold is crossed. High is a binary decision and aliases; low
        /// spreads each spark over a few pixels and stays clean.
        float glintSharpness = 8.0f;
        /// Distance at which sparks start to appear. They fade IN, unlike everything else here:
        /// this is an artefact of many facets falling inside one pixel, so it cannot exist up close
        /// where there is only one.
        float glintFadeStart = 40.0f;

        /// How deeply a trough is shaded by the crest in front of it.
        ///
        /// Not a shadow map and not a trace: it is read off the wave's own rise, which the surface
        /// already carries. What makes it read as light being blocked rather than as water being
        /// painted darker is that it scales with how LOW the sun is -- overhead there is no shadow
        /// to cast, and the term has to disappear on its own as the day goes on.
        float troughShade = 0.38f;

        /// How lit the body still is with the sun on the horizon.
        ///
        /// The sun's height decides how much light ENTERS the water rather than glancing off it, so
        /// a noon sea glows from the inside and an evening one goes dark under a mirror. This is the
        /// floor under that, and it is deliberately not zero: the whole sky lights the sea even when
        /// the sun does not, and water at night is dark rather than black.
        float nightFloor = 0.28f;

        // THERE IS NO FOG DISTANCE HERE, DELIBERATELY.
        //
        // There were two, and they were the reason the sea ended at a hard horizontal line. The world
        // is at full fog by around eight hundred units; a sea fading over a range of its own stayed
        // dark and detailed well past that, so it read as a patch of texture pasted onto an already
        // blank distance -- and then the far plane cut it, leaving a step in colour at a fixed
        // distance, which perspective draws as a straight line across the water.
        //
        // The vertex stage now reads the world's own fog factors out of the stock constant block, the
        // same way it already reads the matrices and the fog COLOUR. So the sea fades exactly when
        // the land fades, follows the zone and the weather without being told, and cannot be set
        // wrong. A knob here would only be a way to reintroduce the bug.
    };

    SurfaceLook& Look();

    /// The texture-driven shading effects, each with its own strength -- and each with its own
    /// switch in the Switches registry, so every contribution can be judged alone instead of as a
    /// package. All of it is look: nothing here feeds a query.
    struct ShadeFx
    {
        /// The animated ocean normal sequence plus a static broad bump layer, in world space at
        /// two scales without common measure. This is the grain at the scale of a PIXEL -- the
        /// geometry stops at the mesh, and everything below it lives here, including at the
        /// horizon where geometry has nothing left.
        float animStrength = 0.28f;
        float animScaleFine = 0.045f;
        float animScaleBroad = 0.011f;
        /// Frames per second of the grain loop, crossfaded so it never steps.
        float animFps = 6.0f;

        /// Fractal noise as the breaker of repetition: jitters the foam threshold, varies the body
        /// colour at large scale so open water stops being one flat tint, and masks the glitter.
        float fbmStrength = 0.18f;
        float fbmScale = 0.0021f;

        /// The dedicated foam pattern: filaments and laced edges instead of a modulated tint. The
        /// accumulated foam amount decides WHERE; this texture decides WHAT IT LOOKS LIKE.
        float foamTexStrength = 1.0f;
        float foamTexScale = 0.043f;

        /// Panorama sky reflection blended over the analytic gradient.
        float skyStrength = 0.35f;

        /// Sparkle field for far water: noise thresholded inside the sun lobe, replacing the
        /// power-900 highlight that aliased per pixel.
        float glitterStrength = 0.35f;

        /// Light coming THROUGH a crest towards a viewer looking at the sun.
        ///
        /// Strength is an outer factor, so zero is off. It was not, once: it sat inside a saturate
        /// multiplied by a signal that is near zero away from a breaking crest, which made the
        /// control inert and the switch a no-op.
        float sssBoost = 1.0f;
        /// How narrowly the effect is confined to looking straight at the sun. Higher is tighter.
        float sssLobe = 3.0f;
        /// How fast a wave's rise counts as thinness. Higher makes only the tallest crests glow.
        float sssThickness = 0.5f;
        /// How strongly it is confined to a LOW sun. Light through a crest needs a path through the
        /// wave, which a midday sun does not have at any bearing.
        float sssSunFalloff = 1.5f;

        /// How far the low-frequency noise displaces every tiled lookup in the pixel stage.
        ///
        /// THE TILE IS NOT THE PROBLEM; THE GRID IT REPEATS ON IS. A texture sampled on world XY
        /// repeats on a square aligned to the world axes, and an axis-aligned repeat is the one an
        /// eye finds in a second -- it reads as banding running straight across the sea. Each layer
        /// is rotated off the axes by its own angle, and then displaced by this: the tile is still
        /// there, but its corners no longer land on any grid.
        ///
        /// It costs sharpness at the very largest scales, so it is a value to raise until the
        /// banding goes and no further. It cannot be applied to the detail field's own lookups --
        /// those have a vertex stage counterpart that must sample the same place.
        float detileWarp = 0.018f;

        /// Animated caustics on the shallow bottom, faded out with the water column.
        ///
        /// Two layers at different scales and different loop rates, multiplied. The repetition
        /// anybody sees in a caustic is the loop coming round; the product of two loops comes round
        /// at their common multiple instead, which is why there is a second scale here and not
        /// simply more frames.
        float causticStrength = 0.6f;
        float causticScale = 0.05f;
        /// The second layer's scale, as a multiple of the first. Keep it well away from a simple
        /// ratio -- at exactly two or three the two layers share a period and the trick is undone.
        float causticScale2 = 2.37f;
        /// Loop slices per second. The visible period is far longer than one loop, so this reads as
        /// the speed of the shimmer rather than as the length of the repeat.
        float causticSpeed = 1.6f;
        /// How strongly the caustic is confined to a HIGH sun -- it is a focusing effect, and a
        /// glancing sun focuses nothing.
        float causticSunPower = 1.0f;
        /// How fast the floor web dies with the water column above the bottom, per world unit.
        float causticDepthFade = 0.12f;

        /// Strength of the SURFACE web -- the same focusing drawn in the body of the water instead
        /// of on the floor.
        ///
        /// This is the one that adds detail to open water. The floor web needs a floor to land on
        /// and a line of sight through the surface to be seen; this needs neither, so it survives
        /// any viewing angle and any depth.
        float causticSurface = 0.0f;
        /// How far the FLOOR web follows the viewing angle. One is physical: seeing the bottom
        /// means light coming back through the surface, which a grazing view mostly reflects
        /// instead. Zero draws it whatever the angle -- wrong physically, right for a stylised sea.
        float causticViewBias = 1.0f;
        /// How fast the surface web dies with the water column. Zero keeps it out to open water,
        /// which is normally the point of having it.
        float causticSurfaceFade = 0.0f;

        // THE STYLISATION BLOCK. Three numbers that trade the physical sea for the painted one,
        // each neutral at zero so the physical look stays the default and a saved profile from
        // before they existed reads back unchanged.

        /// Cartoon amount: lerps the Fresnel from Schlick's curve to a soft bounded ramp -- the
        /// mirror never total, never absent. Most of what separates a rendered sea from a painted
        /// one is in this band.
        float styleCartoon = 0.0f;
        /// The least fraction of the scene that survives the medium however deep the column runs.
        /// Extinction is physics and physics makes deep water a wall; a stylised sea keeps its
        /// bottom visible because the artist said so.
        float clarityFloor = 0.0f;
        /// How much of the reflection is repainted in the water's own colour. A mirror shows the
        /// world; a painted sea shows mostly itself, with the world hinted through.
        float styleRepaint = 0.0f;
        /// How much the surface thins out again at grazing angles: a small subtraction from the
        /// body's opacity where the view flattens, so far water leans on its reflection rather
        /// than on its own paint. The reference value is a tenth -- a lean, not a window.
        float styleGrazing = 0.0f;
        /// Daylight worked into the body itself by the sun's DIRECTION: a sun-facing slope is a
        /// brighter water, not merely a water with a brighter highlight on it.
        float styleSunDiffuse = 0.0f;

        /// Which way the refraction bends: zero is the world-anchored offset that shrinks with
        /// distance, one the screen-anchored one whose reach grows with the thickness of water
        /// behind the surface and dies at grazing angles. The visible difference is far water:
        /// distant underwater shapes keep swimming under the screen-anchored bend where the
        /// world-anchored one has already shrunk to stillness.
        float refractRetail = 0.0f;
        /// How fast thickness counts toward that reach, per world unit of path. Small values make
        /// only genuinely deep water swim; large ones give every puddle the full bend.
        float refractThickness = 2.0f;
        /// The screen-anchored bend's magnitude.
        float refractAmount = 1.2f;

        /// The world in the mirror, read out of the frame itself: a point down the reflected ray
        /// is projected and the scene copy sampled there. Reflects exactly what is on screen --
        /// the cliff across the bay, the boat at anchor -- and falls back to the sky for
        /// everything that is not. Zero is off.
        float ssrStrength = 0.0f;
        /// How far down the reflected ray the probe point sits, in world units. Short reaches
        /// only mirror what stands at the waterline; long ones lean toward the horizon.
        float ssrDistance = 128.0f;
        /// How much of the wave normal reaches the mirror ray. The Fresnel and the sun want the
        /// full churning normal; the mirror wants an almost-planar one with a wobble, or every
        /// pixel samples a different patch of screen and the reflection is abstract soup.
        float ssrRipple = 0.35f;

        // THE LACE: the animated sheet the stock ocean has always worn, read back off the two
        // frames the client's own material bound -- no upload, no ownership, and therefore only
        // ever visible on draws where that material is an ocean's.

        /// Strength of the sheet. Zero is off and is the default: the sheet is only meaningful
        /// where the ocean material bound its frames underneath us.
        float laceStrength = 0.55f;
        /// World scale of the pattern. The stock ocean repeats its sheet about once per liquid
        /// cell -- a shade over four units -- which is this value.
        float laceScale = 0.24f;
        /// How far the lookup boils in place. Movement without direction is the whole character
        /// of the thing -- scrolled, it is instantly a texture sliding over the sea.
        float laceWobble = 0.35f;
        /// How much a breaking crest brightens the sheet, tying it to the sea instead of over it.
        float laceCrestBoost = 0.55f;
        /// How fast the sheet dies with the water column. Zero keeps it out to open water, which
        /// is what an OCEAN sheet normally wants; raised, it pulls the lace to the shore like
        /// surf, at about the reciprocal of the depth where it gives out.
        float laceDepthFade = 0.0f;
        /// Distance fade rate: how soon the filaments give up before they drop under a pixel and
        /// alias. The default gives out around a hundred and fifty units.
        float laceDistFade = 0.0045f;
    };
    ShadeFx& Fx();


    /// WHAT A SETTING IS DOING, PAINTED ONTO THE MAP -- a measuring instrument, not an effect.
    ///
    /// The panel's charts say what the RULES are: this curve against that depth, this band against
    /// that wavelength. What they cannot say is where those rules LAND on the water in front of you,
    /// because every quantity involved is invisible -- a water column, a fold, how much chop
    /// actually arrived somewhere. Two views answer that, and they answer different halves of it.
    ///
    /// Neither is a shader permutation. The false colour is a dot product against a one-hot vector
    /// and a lerp the panel drives to zero; the wireframe is a render state around the draw. Both
    /// cost the same whether they are on or off, which is what makes them safe to leave in.
    struct ShadeDebug
    {
        /// Which quantity the surface is painted with. Off is 0; the rest index the one-hot vector
        /// the pixel stage selects with. Order fixed once shipped -- a saved profile stores it.
        enum View : int
        {
            Off = 0,
            Column,        ///< water column: the quantity every shore setting is expressed in
            BottomSlope,   ///< how fast the floor is rising, which is what the shore band reads
            Break,         ///< where the surface is folding or the depth cap is biting
            Foam,          ///< the white that came out of it
            DetailGain,    ///< how much of the detail field is arriving here
            FoamMask,      ///< the break-up pattern, alone
            Count
        };
        int view = Off;

        /// Depth that fills the ramp, in world units. A map whose water is four units deep and one
        /// whose water is four hundred cannot share a scale, and a false colour that saturates
        /// everywhere is a picture of nothing.
        float columnRange = 30.0f;

        /// The lattice itself. The refinement grading is three numbers whose whole product is a
        /// density varying across the screen, and no readout conveys that -- where a band edge
        /// falls, whether the budget ran out behind you. This answers all of it at once.
        bool wireframe = false;
    };
    ShadeDebug& Debug();


    /// Whether the shading maps are in hand, for the panel: an unbuilt set otherwise looks exactly
    /// like seven effects tuned to nothing.
    struct FxStatus
    {
        bool  ready = false;
        float buildMs = 0.0f;     ///< what the one-time synthesis cost
        int   grainFrames = 0;    ///< slices in the animated grain loop
        int   causticSlices = 0;  ///< slices packed in the caustic map
    };
    FxStatus FxTextures();

    /// Where the foam is and what it looks like.
    ///
    /// Split from the look because it is the part that cannot be reasoned to. Colour and extinction
    /// follow from physics and can be argued about at a desk; a foam threshold is a property of one
    /// map's beaches, and the only way to find it is to stand on one and move the number. Every
    /// field here reaches the pixel stage directly, so it can be moved while looking at the result.
    struct FoamShape
    {
        /// Fold at which a crest counts as breaking, and how abruptly that turns on. The fold is the
        /// vertex stage's own measure of a crest exceeding its cusp, so this is what puts white on
        /// the tops of waves rather than on the shore.
        float breakThreshold = 0.62f;
        float breakSharpness = 4.0f;

        /// Gentlest bottom slope that still counts as a shore, and how abruptly THAT turns on. Below
        /// the threshold a rising bottom is just a rising bottom; above it, it is a beach.
        float shoreSlopeMin = 0.12f;
        float shoreSlopeGain = 0.8f;

        /// Water column at which the shore band gives out -- reciprocal, so 0.18 is about five and
        /// a half units of depth. This keeps surf on the beach instead of trailing into open water.
        float shoreDepthFade = 0.18f;

        /// How hard the shore band drives the accumulation.
        float shoreStrength = 0.65f;

        /// Floor under the texture mask and how much of its contrast is used. The floor exists so
        /// that a band never disappears entirely between texture features; raising it flattens the
        /// break-up back towards a painted sheet.
        float maskFloor = 0.55f;
        float maskContrast = 0.45f;

        /// The three texture scales. KEEP THEM INCOMMENSURATE: the anti-tiling rests entirely on no
        /// two layers repeating together, and rounding these to neat fractions of each other brings
        /// the visible tile straight back.
        float scale[3] = { 0.031f, 0.113f, 0.277f };

        /// How far the wave normal drags the foam texture. All the foam's movement comes from here --
        /// it is the water's own motion, which is why nothing is scrolled by time.
        float warp = 0.25f;

        /// How fast the accumulation saturates towards white. Higher is whiter sooner; the curve
        /// approaches full white without arriving, so overlapping foam densifies instead of blowing
        /// the alpha out.
        float density = 2.2f;
    };

    FoamShape& Foam();

    /// Direction toward the sun in world space, as the world last posted it. Read-only on purpose:
    /// it is a fact about the sky, not a setting.
    const float* SunDir();

    // Session-only diagnostic; excluded from profile serialization.
    struct ReflectionLightPreview {
        bool enabled = false;
        float elevation = 10.0f;
        float bearingOffset = 0.0f;
    };
    ReflectionLightPreview& LightPreview();

    /// Camera world position as the last liquid draw was handed it. Valid whenever any water drew
    /// this frame, which is the only frame anything downstream has a question about.
    const float* CameraPos();
}
