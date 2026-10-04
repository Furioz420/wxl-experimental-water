// Water height queries, and the rule about which answer each caller gets.
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

#include "wxl-experimental-water/sea/Shore.hpp"
#include "wxl-experimental-water/sea/Wave.hpp"

// There are two water levels, and telling them apart is the whole design.
//
//   Plane   -- the still-water level the map declares (MH2O). Flat, authoritative, identical on the
//              server. Everything that decides *state* reads this: whether you are swimming, whether
//              a spell needs line of sight through water, what the server thinks your position means.
//
//   Surface -- the plane plus the wave displacement. What is drawn, what a hull floats on, what the
//              camera crosses when it goes under.
//
// Feeding the displaced height to the state machine is what broke the previous attempt: a crest
// passing a standing player flipped SWIMMING to WALKING and back several times a second. The
// response then was to flatten everything, which cost the swell. The right answer is that gameplay
// state is a property of the plane and never of the wave, while everything the eye judges follows
// the surface.
//
// The wave displacement is a sum of sines and therefore has zero mean, so the two levels agree on
// average and the server's notion of the water line stays true no matter how large the sea gets.
namespace wxl::water
{
    /// Where the map says the still water is, and whether there is any here at all.
    struct WaterPlane
    {
        bool  present = false;
        float level = 0.0f;   ///< Still-water Z, world space.
        float floor = 0.0f;   ///< Terrain or WMO floor beneath, for the water column.

        float Depth() const { return present ? (level - floor) : 0.0f; }
    };

    /// Both answers for one place, resolved once so a caller cannot accidentally mix them.
    struct WaterSample
    {
        WaterPlane plane;
        float      surface = 0.0f;   ///< plane.level + wave displacement
        Vec3       normal{ 0.0f, 0.0f, 1.0f };

        /// Height a hull or a swimmer rides at.
        float RideHeight() const { return surface; }

        /// Height the state machine is allowed to see.
        float StateHeight() const { return plane.level; }
    };

    /// Resolves a place: the declared plane, then the sea on top of it.
    ///
    /// THE SHORE IS READ FROM THE SAME LADDER THE VERTEX STAGE USES, and that is the correction of a
    /// real fault. This function used to carry its own depth ramp with its own hardcoded end -- 24
    /// units, against whatever the panel had set for the drawn water -- and no shoaling gain at all.
    /// So in the shallows a hull floated on a wave that was a different height and a different shape
    /// from the one on screen, and nothing anywhere said so. One function, both sides.
    inline WaterSample Sample(const WaveFrame& frame, const WaterPlane& plane, float x, float y,
                              float detailRise = 0.0f)
    {
        WaterSample s;
        s.plane = plane;

        if (!plane.present)
        {
            s.surface = plane.level;
            return s;
        }

        // The pinch ceiling is the trains' shared cusp budget, the same sum the vertex upload
        // computes. Nothing here consumes the pinch -- a height query does not need it -- so it is
        // passed as unbounded rather than recomputed for a value that is thrown away.
        const ShoreResponse shore = ShoreAt(ShoreSettings(), plane.Depth(),
                                            frame.maxDisplacement + detailRise, 1e9f);
        const float attenuation = shore.height;
        s.surface = plane.level + HeightAt(frame, x, y) * attenuation;

        if (attenuation > 0.0f)
        {
            s.normal = NormalAt(frame, x, y);
            // Flatten the normal alongside the height, or a shoreline reads as choppy water lying
            // motionless.
            s.normal.x *= attenuation;
            s.normal.y *= attenuation;
            const float len = std::sqrt(s.normal.x * s.normal.x + s.normal.y * s.normal.y + s.normal.z * s.normal.z);
            if (len > 1e-6f)
            {
                s.normal.x /= len;
                s.normal.y /= len;
                s.normal.z /= len;
            }
        }

        return s;
    }

    /// Tracks whether a point is submerged, with a dead band around the crossing.
    ///
    /// A bare comparison against the surface flickers whenever the camera sits within a wave's
    /// amplitude of it -- which, at the waterline, is exactly where a player spends their time. The
    /// band has to be crossed properly before the state changes, so a crest lapping the lens no
    /// longer strobes the underwater treatment on and off.
    class SubmersionState
    {
    public:
        /// Half-width of the dead band, in world units.
        explicit SubmersionState(float band = 0.12f) : m_band(band) {}

        bool Update(float pointZ, float surfaceZ)
        {
            const float delta = surfaceZ - pointZ;   // positive: below the surface
            if (m_submerged)
            {
                if (delta < -m_band)
                    m_submerged = false;
            }
            else
            {
                if (delta > m_band)
                    m_submerged = true;
            }
            return m_submerged;
        }

        bool Submerged() const { return m_submerged; }

        /// Signed distance to the surface from the last update, for fading the underwater treatment
        /// in rather than switching it.
        void Reset(bool submerged) { m_submerged = submerged; }

    private:
        float m_band = 0.12f;
        bool  m_submerged = false;
    };
}
