// The live sea.
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

#include "wxl-experimental-water/sea/Sea.hpp"
#include "wxl-experimental-water/sea/Clock.hpp"
#include "wxl-experimental-water/mesh/Lattice.hpp"
#include "wxl-experimental-water/sea/Ocean.hpp"

#include "wxl/EnvironmentWind.hpp"

#include "offsets/game/Weather.hpp"

#include <algorithm>
#include <cmath>

namespace
{
    namespace wo = wxl::offsets::game::weather;
    using namespace wxl::water;

    SeaResponse   g_response;
    SeaConditions g_live;          ///< after the lag
    SeaConditions g_forced;
    bool          g_forcing = false;

    WaveSpectrum g_spectrum;
    WaveFrame    g_frame;
    EyeState     g_eye;
    /// Kept because the lattice needs it and has no other way to reach it: refinement is graded by
    /// distance from here, and the geometry hook runs where no camera is in scope.
    float        g_eyePos[3] = { 0.0f, 0.0f, 0.0f };
    /// Narrower than the default: the camera crosses the waterline deliberately far more often than
    /// a hull does, and too wide a band here reads as the medium arriving late.
    SubmersionState g_eyeBand{ 0.10f };
    bool         g_started = false;
    double       g_lastTick = 0.0;

    /// Storm intensity, 0..1, as the world keeps it.
    ///
    /// The client's own dead zone is reused rather than replaced: below the knee nothing else in the
    /// world reacts either, so a sea that rose there would be the only thing on screen responding to
    /// a sky that has not changed.
    float StormIntensity()
    {
        void* w = *reinterpret_cast<void**>(wo::kWorldWeather);
        if (!w) return 0.0f;
        const float raw = *reinterpret_cast<const float*>(static_cast<uint8_t*>(w) + wo::kIntensity);
        if (raw <= wo::kIntensityKnee) return 0.0f;
        return std::clamp((raw - wo::kIntensityKnee) / (1.0f - wo::kIntensityKnee), 0.0f, 1.0f);
    }

    /// THE WIND IS NOT THE SEA'S TO DECIDE, and it used to be.
    ///
    /// A heading and a speed lived here, and another heading and speed lived in the grass, and
    /// nothing made them agree -- so the swell could run one way while the grass on the headland
    /// leaned another. They now come from one field that both read.
    ///
    /// Applied to the TARGET rather than to the live conditions, which is what makes the coupling
    /// behave. The wind gusts on a scale of seconds; the swell answers on a scale of minutes, and
    /// the lag below is already what expresses that. So a gust roughens the surface and leaves the
    /// swell alone, exactly as it should, and nothing extra had to be written to get it.
    void TakeWindFromTheAir(SeaConditions& target)
    {
        const wxl::wind::WindSample& air = wxl::wind::Frame();
        // A dead field means the wind module is not installed; the presets keep their own values
        // rather than collapsing the sea to a flat calm.
        if (air.speed <= 0.0f) return;

        target.windSpeed = air.speed;
        target.windDir   = Vec2{ air.dir[0], air.dir[1] };
    }

    /// Exponential approach, framerate-independent.
    ///
    /// A per-frame lerp with a fixed factor converges at a rate that depends on how fast the machine
    /// is running, so the same storm arrives at two speeds on two machines. That would be a visual
    /// nuisance anywhere else; here it is a correctness fault, because the sea is meant to be the
    /// same for everyone.
    float Approach(float current, float target, float lagSeconds, float dt)
    {
        if (lagSeconds <= 1e-3f) return target;
        const float a = 1.0f - std::exp(-dt / lagSeconds);
        return current + (target - current) * a;
    }

    /// Rebuilds the detail field when the weather has moved it, and not otherwise.
    ///
    /// It depends on four things only -- the wind's speed and heading, the length the trains hand
    /// over at, and the seed -- and all four move on the slow clock, so this fires perhaps once a
    /// minute. Rebuilding per frame would be several transforms of pure waste: the field's SHAPE is
    /// what the weather decides, and only its phase moves with time.
    void RefreshOceanSpectrum(const SeaConditions& c)
    {
        static bool  built = false;
        static float lastSpeed = 0.0f, lastX = 0.0f, lastY = 0.0f, lastMin = 0.0f;
        static float lastStorm = -1.0f;
        static OceanTuning last;

        // Pushed every tick, not only on rebuilds: the storm blend's evolve-time consumers -- the
        // chop and the foam threshold -- follow the weather continuously, while its build-time
        // consumers are covered by the storm term in the rebuild test below.
        const float storm = StormIntensity();
        OceanStorm(storm);

        // The panel's values are watched alongside the weather's, because a slider that changes
        // nothing until the wind happens to shift is a slider nobody can tune with. Rebuilding on a
        // drag is affordable precisely because it is a drag: a person moves one of these a few times
        // a minute, not sixty times a second.
        //
        // Compared EXACTLY rather than within a tolerance, unlike the weather above. The weather is
        // a continuous approach and would rebuild on every frame of a change; these move only when a
        // hand moves them, so "did it change" is the whole question. The knobs left out -- chop,
        // lean, the foam shape -- are applied at evolve time and answer on the next frame without a
        // rebuild. Strength, coupling and peaking are watched because they are INPUTS to the
        // amplitude budget now: the field is normalised so their worst case stays inside the cusp,
        // which is only true if moving them renormalises it.
        const OceanTuning& t = OceanSettings();

        // The storm term rebuilds on the same slow approach as the wind: the amplitude and the
        // fold budget are build-time inputs, so a storm that rolls in renormalises the field a
        // handful of times on its way up rather than once per frame.
        const bool weatherMoved = std::fabs(c.windSpeed - lastSpeed) > 0.25f
                               || std::fabs(c.windDir.x - lastX) > 0.02f
                               || std::fabs(c.windDir.y - lastY) > 0.02f
                               || std::fabs(c.minWavelength - lastMin) > 0.25f
                               || std::fabs(storm - lastStorm) > 0.04f;

        bool tuningMoved = t.amplitude != last.amplitude
                        || t.shortest  != last.shortest
                        || t.longest   != last.longest
                        || t.cellSize  != last.cellSize
                        || t.falloff   != last.falloff
                        || t.timeScale != last.timeScale
                        || t.strength  != last.strength
                        || t.waveCoupling != last.waveCoupling
                        || t.stokes    != last.stokes
                        // The three spectrum-shaping knobs this list was missing -- each one is a
                        // BUILD-time input, and leaving one out makes it a slider that does
                        // nothing until the wind happens to shift, which is indistinguishable
                        // from a slider that does nothing.
                        || t.crestLength != last.crestLength
                        || t.upwind    != last.upwind
                        || t.foldBudget != last.foldBudget
                        || t.stormAmplitude != last.stormAmplitude
                        || t.stormFold != last.stormFold
                        || t.seed      != last.seed;
        for (int ci = 0; ci < kOceanCascades; ++ci)
            tuningMoved = tuningMoved || t.align[ci] != last.align[ci]
                                      || t.speed[ci] != last.speed[ci];

        if (built && !weatherMoved && !tuningMoved) return;

        built = true;
        lastSpeed = c.windSpeed;
        lastX = c.windDir.x;
        lastY = c.windDir.y;
        lastMin = c.minWavelength;
        lastStorm = storm;
        last = t;
        OceanBuild(c);
    }

    /// Set by SnapSea(), consumed by the next Tick. A flag rather than a direct write because the
    /// panel runs on the render thread and the sea is advanced from the frame boundary; letting the
    /// button rebuild the spectrum itself would put a half-written table in front of a draw.
    bool g_snap = false;

    SeaConditions Mix(const SeaConditions& a, const SeaConditions& b, float t)
    {
        SeaConditions r = a;
        r.windSpeed       = a.windSpeed + (b.windSpeed - a.windSpeed) * t;
        r.amplitude       = a.amplitude + (b.amplitude - a.amplitude) * t;
        r.steepness       = a.steepness + (b.steepness - a.steepness) * t;
        r.directionSpread = a.directionSpread + (b.directionSpread - a.directionSpread) * t;
        r.minWavelength   = a.minWavelength + (b.minWavelength - a.minWavelength) * t;
        r.crestSpacing    = a.crestSpacing + (b.crestSpacing - a.crestSpacing) * t;
        r.swellLength     = a.swellLength + (b.swellLength - a.swellLength) * t;
        r.swellShare      = a.swellShare + (b.swellShare - a.swellShare) * t;
        return r;
    }
}

namespace wxl::water
{
    void Tick()
    {
        const double now = Now();
        const double realNow = SessionSeconds();

        if (!g_started)
        {
            g_started = true;
            g_lastTick = realNow;
            g_live = g_forcing ? g_forced : Mix(g_response.calm, g_response.storm, StormIntensity());
            g_spectrum = BuildSpectrum(g_live);
            g_frame.Prepare(g_spectrum, now);
            RefreshOceanSpectrum(g_live);
            OceanEvolve(now);
            return;
        }

        const float dt = static_cast<float>(std::clamp(realNow - g_lastTick, 0.0, 1.0));
        g_lastTick = realNow;

        SeaConditions target = g_forcing ? g_forced
                                         : Mix(g_response.calm, g_response.storm, StormIntensity());
        TakeWindFromTheAir(target);

        // ONE FRAME WHERE THE LAG IS SKIPPED, and it exists for tuning rather than for weather.
        //
        // Every field below is approached rather than assigned, which is what makes a squall arrive
        // as water that grows instead of water that changes. The swell's own lag is seven minutes,
        // deliberately, because a swell was raised somewhere else days ago and cannot answer a gust.
        //
        // That is correct for weather and useless for a person moving a slider: a preset applied
        // through it would take those seven minutes to be visible, and would be indistinguishable
        // from a button that does nothing.
        if (g_snap)
        {
            g_snap = false;
            g_live = target;
            g_spectrum = BuildSpectrum(g_live);
            g_frame.Prepare(g_spectrum, now);
            RefreshOceanSpectrum(g_live);
            OceanEvolve(now);
            return;
        }

        // Wind speed sets the wavelength, so it is the swell's own variable and moves on the slow
        // clock. Amplitude and steepness are what the chop does, and they answer far sooner: a squall
        // roughens the surface long before it lengthens the swell underneath it.
        g_live.windSpeed       = Approach(g_live.windSpeed, target.windSpeed, g_response.swellLag, dt);
        g_live.amplitude       = Approach(g_live.amplitude, target.amplitude, g_response.chopLag, dt);
        g_live.steepness       = Approach(g_live.steepness, target.steepness, g_response.chopLag, dt);
        g_live.directionSpread = Approach(g_live.directionSpread, target.directionSpread,
                                          g_response.chopLag, dt);
        // The swell moves on the slow clock too, and for a stronger reason than the wind: it was
        // raised somewhere else days ago, so it cannot answer this squall at all. Its share may rise
        // faster than its length, because a rising local sea buries a swell long before it lengthens
        // one.
        g_live.swellLength     = Approach(g_live.swellLength, target.swellLength,
                                          g_response.swellLag, dt);
        g_live.swellShare      = Approach(g_live.swellShare, target.swellShare,
                                          g_response.swellLag, dt);
        g_live.minWavelength   = Approach(g_live.minWavelength, target.minWavelength,
                                          g_response.chopLag, dt);
        // Spacing is a property of the wind sea's length, so it moves on the slow clock with the
        // wind that sets it rather than with the chop that rides on it.
        g_live.crestSpacing    = Approach(g_live.crestSpacing, target.crestSpacing,
                                          g_response.swellLag, dt);
        // THE SWELL DOES NOT TURN WHEN THE WIND DOES, and this was an assignment until the wind
        // became a field that veers. Two things broke at once when it did.
        //
        // The physical one: a swell is the accumulated work of a wind that has been blowing for
        // hours. It swings round to a new heading over minutes, not on the frame the gust arrives,
        // and an instant assignment made the whole sea pivot with every wander of the air.
        //
        // The expensive one: the detail field is rebuilt whenever this heading moves more than a
        // hundredth, on the stated assumption that it moves "perhaps once a minute". A continuously
        // veering wind crossed that every tenth of a second, so several transforms were being run
        // per second to chase a direction nothing could see change.
        //
        // Approached per component and renormalised. The veer is small, so the two are the same
        // rotation to well within what the sea can express -- and the visible result is the one
        // worth having: during a shift the wind indicator leads and the swell follows it round.
        g_live.windDir.x = Approach(g_live.windDir.x, target.windDir.x, g_response.swellLag, dt);
        g_live.windDir.y = Approach(g_live.windDir.y, target.windDir.y, g_response.swellLag, dt);
        {
            const float len = std::sqrt(g_live.windDir.x * g_live.windDir.x +
                                        g_live.windDir.y * g_live.windDir.y);
            if (len > 1e-6f) { g_live.windDir.x /= len; g_live.windDir.y /= len; }
            else             { g_live.windDir = target.windDir; }
        }
        g_live.exposure        = target.exposure;
        g_live.seed            = target.seed;

        // Rebuilt every frame rather than blended, because the inputs themselves are already moving
        // continuously: the lag above is what makes the transition smooth, so BuildSpectrum here
        // produces a spectrum that differs from last frame's by an imperceptible amount. Phases are a
        // pure function of the seed, so nothing slides even though the table is remade.
        g_spectrum = BuildSpectrum(g_live);
        g_frame.Prepare(g_spectrum, now);

        // The detail rides on the same clock and the same seed, so it is as reproducible as the
        // trains -- even though, unlike them, nothing is ever allowed to ask it a question.
        RefreshOceanSpectrum(g_live);
        OceanEvolve(now);
    }

    const WaveFrame&     Frame()      { return g_frame; }
    const WaveSpectrum&  Spectrum()   { return g_spectrum; }
    const SeaConditions& Conditions() { return g_live; }
    SeaResponse&         Response()   { return g_response; }
    void                 SnapSea()    { g_snap = true; }

    void ForceConditions(const SeaConditions& conditions)
    {
        g_forced = conditions;
        g_forcing = true;
    }

    void ReleaseForcedConditions() { g_forcing = false; }

    WaterSample SampleAt(const WaterPlane& plane, float x, float y)
    {
        return Sample(g_frame, plane, x, y);
    }

    void UpdateEye(const float* worldPos)
    {
        g_eyePos[0] = worldPos[0];
        g_eyePos[1] = worldPos[1];
        g_eyePos[2] = worldPos[2];

        WaterPlane plane;
        if (!PlaneAt(worldPos[0], worldPos[1], plane))
        {
            g_eyeBand.Reset(false);
            g_eye = EyeState{};
            return;
        }

        const WaterSample s = Sample(g_frame, plane, worldPos[0], worldPos[1]);

        g_eye.hasPlane = true;
        g_eye.planeLevel = plane.level;
        g_eye.submerged = g_eyeBand.Update(worldPos[2], s.surface);
        g_eye.depth = g_eye.submerged ? (s.surface - worldPos[2]) : 0.0f;
        if (g_eye.depth < 0.0f) g_eye.depth = 0.0f;
    }

    const EyeState& Eye() { return g_eye; }
    const float*    EyePosition() { return g_eyePos; }
}
