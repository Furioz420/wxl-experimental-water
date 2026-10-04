// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cmath>
#include <cstdint>
namespace wxl::water::celestial {
// First 12 bytes of DayNightGetInfo, populated by FrameDayNightUpdate.
// Matches the native map fixed-time branch: mode=0, day fraction, rate=0.
struct TimeInput { uint32_t mode; float phase; float rate; };
static_assert(sizeof(TimeInput)==12);
inline bool ApplyTimePreview(TimeInput* input,bool enabled,float hour) {
    if(!input || !enabled || !std::isfinite(hour) || hour<0 || hour>=24) return false;
    *input={0,hour/24.f,0}; return true;
}
}
