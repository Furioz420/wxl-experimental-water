// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
// A bounded reflected celestial source, independent of the panorama/tint blend.
// Broaden unresolved normals instead of erasing the source at distance.
float3 CelestialReflection(float alignment, float sharp, float roughness,
                           float3 lightColor, float intensity, float elevation, float specularPower)
{
    float spread = saturate(max(1.0 - sharp, roughness));
    float power = lerp(max(specularPower,1.0)*8.0, max(specularPower,1.0)*0.5, spread);
    float peak = lerp(1.5, 0.45, spread);
    float core = pow(saturate(alignment), power) * peak;
    float halo = pow(saturate(alignment), 24.0) * 0.25;
    float horizon = smoothstep(0.0, 0.04, elevation);
    return lightColor * (max(intensity, 0.0) * horizon * (core + halo));
}
