// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WXL_WATER_TRANSMISSION_HLSLI
#define WXL_WATER_TRANSMISSION_HLSLI

float3 WaterBody(float3 shallow, float3 deep, float3 scene, float bottomBrightness,
                 float3 transmission, float3 illumination, float sceneAvailable)
{
    if (sceneAvailable > 0.5)
    {
        // Scene radiance already includes the object's material and lighting. Extinguish
        // it once; the water palette and illumination belong to the scattered term.
        return scene * bottomBrightness * transmission
             + deep * (1.0 - transmission) * illumination;
    }
    // No captured image: preserve the authored, lit shallow-to-deep approximation.
    return lerp(deep, shallow * bottomBrightness, transmission) * illumination;
}
#endif
