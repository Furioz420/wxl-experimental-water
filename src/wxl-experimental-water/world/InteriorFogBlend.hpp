// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
namespace wxl::water {
// Preserve the accepted enclosed fallback if the native input cannot be trusted.
inline float InteriorFogBlend(bool enclosed,bool available,float nativeWeight) {
    if(!enclosed) return 0;
    return available && std::isfinite(nativeWeight) ? std::clamp(nativeWeight,0.f,1.f) : 1.f;
}
}
