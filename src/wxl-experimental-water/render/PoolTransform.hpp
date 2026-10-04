#pragma once
// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cmath>
namespace wxl::water {
using PoolMatrix = std::array<float,16>;
// Native row-vector matrix storage: translation is elements 12..14.
// Limit this path to upright, uniformly scaled yaw transforms. Tilt/shear,
// reflections, projective and nonfinite inputs stay on native rendering.
inline bool PreparePoolTransform(const float* m, PoolMatrix& output) {
    if (!m) return false;
    for (int i=0;i<16;++i) if (!std::isfinite(m[i])) return false;
    constexpr float e=1e-4f;
    if (std::abs(m[3])>e || std::abs(m[7])>e || std::abs(m[11])>e || std::abs(m[15]-1)>e ||
        std::abs(m[2])>e || std::abs(m[6])>e || std::abs(m[8])>e || std::abs(m[9])>e) return false;
    const float scale=m[10];
    if (scale<.1f || scale>10.f) return false;
    const float s2=scale*scale, tolerance=1e-3f*s2;
    if (std::abs(m[0]*m[0]+m[1]*m[1]-s2)>tolerance ||
        std::abs(m[4]*m[4]+m[5]*m[5]-s2)>tolerance ||
        std::abs(m[0]*m[4]+m[1]*m[5])>tolerance ||
        std::abs(m[0]*m[5]-m[1]*m[4]-s2)>tolerance) return false;
    for (int i=12;i<15;++i) if(std::abs(m[i])>100000.f) return false;
    for (int i=0;i<16;++i) output[i]=m[i];
    return true;
}
}
