// The frame as it stood before the water.
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
#include "wxl/RenderDepthApi.h"

namespace wxl::water
{
    /// Sampler stage the scene copy is bound to. Above everything the liquid pass touches -- the
    /// client owns the low stages -- so nothing else is disturbed by it and nothing overwrites it.
    constexpr unsigned kSceneStage = 6;

    /**
     * @brief The frame as captured just before the liquid pass, or null if there is none.
     *
     * Handed back untyped so a caller that only wants to know whether refraction is available does
     * not drag the D3D headers in with it. Null means the surface must fall back to its flat bottom
     * colour rather than sample a stage holding whatever the last draw left there.
     */
    void* SceneCopy();

    /// Dimensions of that copy, for the half-texel the sample needs to land on the right pixel.
    void SceneSize(float& width, float& height);
    bool WaterDepth(WXL_SceneDepth& out);
}
