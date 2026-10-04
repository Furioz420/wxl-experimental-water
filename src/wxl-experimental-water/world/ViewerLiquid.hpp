// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <cmath>
#include <algorithm>
namespace wxl::water
{
    struct ViewerLiquid
    {
        uint32_t type=0;
        float depth=0; // surface minus camera height; positive below water
        bool Submerged() const { return type!=0 && std::isfinite(depth) && depth>0; }
        float AtmosphereGain() const
        {
            if (!type) return 1; // dry ground, regardless of world elevation
            if (!std::isfinite(depth)) return 0;
            const float t=std::clamp(-depth-.10f,0.f,1.f);
            return t*t*(3-2*t);
        }
    };
}
