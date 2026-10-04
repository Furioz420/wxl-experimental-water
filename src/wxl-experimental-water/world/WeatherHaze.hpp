// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "HeightFog.hpp"
namespace wxl::water::fog {
// Native intensity can remain high even when the native kind is Fine.
// Only precipitation kinds may contribute the extra storm atmosphere.
inline float NativeWeatherInput(float raw,int kind,bool available) {
    return available && kind>=1 && kind<=3 && std::isfinite(raw) ? raw : 0.f;
}
inline float StormAmount(float raw) {
    if (!std::isfinite(raw)) return 0;
    return std::clamp((raw-0.25f)/0.75f,0.f,1.f);
}
inline float WeatherLightGain(float raw) {
    // Artistic storm attenuation, not geometric cloud occlusion. Clear weather
    // is identity; retain 40% at full storm rather than switching light off.
    return 1.f-0.6f*StormAmount(raw);
}
inline void ApplyWeatherHaze(Constants& constants,float raw) {
    // Existing native weather is already smoothed. Preserve color, opacity cap,
    // geometry boundaries and clear-weather extinction; only scale added haze.
    constants.row[3][3] *= 1.f+0.5f*StormAmount(raw);
}
}
