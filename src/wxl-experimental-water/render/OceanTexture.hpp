// Carries the detail field to the vertex stage.
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

// Ocean.cpp knows nothing about a device, deliberately: it is a pure function of state, seed and
// time, which is what makes it reproducible and what would make it testable off a graphics card.
// Everything that has to touch D3D lives here instead.
//
// THE FETCH IS THE WHOLE REASON THIS IS POSSIBLE. A field is not a formula -- it has to be sampled,
// not evaluated -- and the lattice is built once when the client rebuilds its liquid batch and then
// kept, so displacing it on the CPU would mean rewriting every vertex of it every frame. Reading the
// field from the VERTEX stage keeps the geometry static and the per-frame cost at two texture
// fetches per vertex. vs_3_0 makes that fetch optional, which is why the device was asked at startup
// whether it could do it.
namespace wxl::water
{
    /// Uploads this frame's cascades, creating the textures on first use. Cheap and idempotent.
    void OceanTextureUpdate(void* device);

    /// Binds them for the draw about to be issued: the geometry maps to the four vertex samplers,
    /// the shading maps to high pixel stages where bilinear filtering exists -- the split that buys
    /// four cascades from four vertex samplers and unfacets the lighting at the same time.
    ///
    /// At the DRAW and not at a bind seam, for the same reason the render states and the scene copy
    /// are: the engine's deferred flush runs after every earlier seam and would replace anything set
    /// there. The vertex sampler stages sit far outside the range the flush touches, but the draw is
    /// the one place that is true by construction rather than by observation.
    void OceanTextureBind(void* device);

    /// Drops the textures. They live in the default pool, so a device reset destroys them and
    /// touching one afterwards is a crash rather than a black surface.
    void OceanTextureRelease();
}
