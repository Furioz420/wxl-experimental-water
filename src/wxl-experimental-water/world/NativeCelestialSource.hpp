// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "CelestialSource.hpp"
#include "offsets/engine/Sky.hpp"
#include <cstdint>
namespace wxl::water::celestial
{
    // Read during world rendering. This is the native billboard source already
    // used by the accepted shaft alignment, not the model's diffuse light vector.
    inline bool ReadDirection(float out[3],Body* selected=nullptr)
    {
        namespace sky=wxl::offsets::engine::sky;
        const auto* info=static_cast<const uint8_t*>(
            reinterpret_cast<sky::DayNightGetInfoFn>(sky::kDayNightGetInfo)());
        if(selected) *selected=Body::Unavailable;
        const bool valid=info && Direction(*reinterpret_cast<const float*>(info+sky::kInfoDayFraction),
            reinterpret_cast<const float*>(info+sky::kInfoSkyOrigin),
            reinterpret_cast<const float*>(info+sky::kInfoSunPosition),
            reinterpret_cast<const float*>(info+sky::kInfoMoonPosition),out);
        if(valid && selected) *selected=SelectBody(*reinterpret_cast<const float*>(info+sky::kInfoDayFraction));
        return valid;
    }
}
