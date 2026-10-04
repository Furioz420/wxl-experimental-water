// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
namespace wxl::water {
inline float GlowAmount(bool early,bool effects,bool glowEffect,bool enabled,bool submerged,float amount) {
    return early && effects && glowEffect && enabled && !submerged && std::isfinite(amount)
        ? std::clamp(amount,0.f,1.f) : 0.f;
}
}
