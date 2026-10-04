// Riding the swell: what floats is lifted where it is DRAWN, and nowhere else.
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
//
// WHY THIS IS PURELY VISUAL, AND WHY THAT IS THE STRONGER ANSWER
//   Moving the real position would put every client and the server in an argument about where a
//   swimmer is, several times a second, for a difference that is decorative. The server would correct
//   it, the correction would fight the wave, and the result is the rubber-banding every attempt at
//   this produces.
//
//   Lifting only the drawn model costs none of that. The authoritative position stays on the flat
//   plane, so targeting, spell ranges, collision, pathing and the server all keep the answer they
//   already agree on -- and swimming state keeps coming from the plane, which is the specific thing
//   that made the previous water flicker between SWIMMING and WALKING as crests went past.
//
//   It is also correct in multiplayer without replicating anything. The offset is a function of
//   (shared spectrum, shared clock, position), so two clients compute the same lift for the same
//   character at the same instant. Nobody sends a height; everybody agrees on one.
//
//   What it does not buy: the model is drawn where the game does not think it is. At a metre or two
//   of swell nothing notices. It would matter for something you have to stand on -- a deck -- and
//   that is the point where the position itself has to move and the server has to be told.

#include "wxl-experimental-water/mesh/Lattice.hpp"
#include "wxl-experimental-water/sea/Sea.hpp"

#include "config.hpp"
#include "common/Log.hpp"
#include "engine/hook/Registry.hpp"
#include "engine/events/Event.hpp"
#include "offsets/game/M2.hpp"

#include <cstdint>
#include <cmath>

namespace
{
    namespace ev  = wxl::events;
    namespace m2  = wxl::offsets::game::m2;
    namespace w   = wxl::water;

    /// Water has to be at least this deep for anything to ride it.
    ///
    /// This is what keeps the beach still. A prop resting on wet sand sits where the column is
    /// nearly nothing, and the sea there is already flattened to nothing by the same depth
    /// attenuation -- so the test agrees with the surface rather than being a second opinion about
    /// where the shore is.
    constexpr float kMinDepth = 1.5f;

    /// How far from the still-water level a model may sit and still count as floating on it.
    ///
    /// Narrow on purpose. A swimmer and a floating crate sit within a metre of the plane; a tree on
    /// an island, a bird, or a fish on the bottom do not, and none of them should heave.
    constexpr float kRideBand = 2.5f;

    /// Placement is row-major with the translation in the last row.
    constexpr int kTx = 12, kTy = 13, kTz = 14;

    void OnPlacement(void*, const void* args)
    {
        auto* a = const_cast<ev::M2PlacementArgs*>(static_cast<const ev::M2PlacementArgs*>(args));
        if (!a->renderCtx) return;

        const auto* m = reinterpret_cast<const float*>(static_cast<const uint8_t*>(a->renderCtx)
                                                       + m2::kOffInstPlacement);
        const float x = m[kTx], y = m[kTy], z = m[kTz];

        // A hash lookup, and it rejects almost everything. This runs once per visible model per
        // frame, so the cheap test has to come first and the surface inversion has to be reached
        // only by the few things actually in the water.
        w::WaterPlane plane;
        if (!w::PlaneAt(x, y, plane)) return;
        if (plane.Depth() < kMinDepth) return;
        if (std::fabs(z - plane.level) > kRideBand) return;

        // The lift is how far the wave has carried the water above its own flat level here -- not the
        // surface height itself. Anything else would move a swimmer by the map's water level as well
        // as by the wave, and put them at sea level in a mountain lake.
        a->offsetZ = w::SampleAt(plane, x, y).surface - plane.level;
    }

    bool Install()
    {
        ev::Subscribe(ev::Event::OnM2Placement, &OnPlacement, nullptr);
        WLOG_INFO("water: ride installed (visual only, authoritative position untouched)");
        return true;
    }
}

WXL_REGISTER_FEATURE("water-ride", wxl::features::water, Install)
