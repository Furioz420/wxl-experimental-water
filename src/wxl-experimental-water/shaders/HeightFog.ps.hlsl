// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
// Analytic height integration with optional eight-segment world-space mist. No temporal history or scene-color copy.
sampler2D sceneDepth : register(s0);
float4 projection : register(c0); // A, B, viewport MinZ, inverse viewport depth range
float4 rayScale : register(c1);   // reciprocal projection scales, projection offsets
float4 worldZ : register(c2);     // camera basis projected on world Z, camera height
float4 fog : register(c3);        // native fog RGB, density at sea level
float4 layer : register(c4);      // base height, height falloff, opacity cap, max distance
float4 nearFade : register(c5);   // start distance, inverse fade distance, aerial density fraction, aerial start
float4 celestial : register(c6);  // view-space direction toward light, bounded gain
float4 worldX : register(c7);
float4 worldY : register(c8);
float4 mist : register(c9); // bounded modulation, periodic time phase

float ExponentialMean(float x)
{
    // Continuous at horizontal rays, without cancellation in (1-exp(-x))/x.
    return x < 0.01 ? 1.0 - x*0.5 + x*x/6.0 : (1.0-exp(-x))/max(x,0.00001);
}
float MeanDensity(float first, float last)
{
    float low=min(first,last)-layer.x;
    float high=max(first,last)-layer.x;
    if (high<=0.0) return 1.0;
    if (low>=0.0) return exp(-low*layer.y)*ExponentialMean((high-low)*layer.y);
    // Crossing the base: constant density below it, exponential density above it.
    return (-low + high*ExponentialMean(high*layer.y))/max(high-low,0.00001);
}
float MistVariation(float3 position)
{
    // Smooth periodic domain warping breaks the long straight interference bands.
    // Coordinates are world anchored; integer temporal harmonics preserve wrapping.
    float3 warp=float3(sin(position.y*.009+position.z*.013+mist.y),
                       sin(position.x*.011-position.z*.008-mist.y),
                       sin(position.x*.007+position.y*.012+2*mist.y));
    position+=warp*24.0;
    return 0.5*sin(position.x*0.018+position.y*0.013-mist.y)
         + 0.3*sin(position.x*(-0.011)+position.y*0.021-2.0*mist.y)
         + 0.2*sin(position.x*0.009-position.y*0.007+position.z*0.027+mist.y);
}
float4 main(float2 uv:TEXCOORD0):COLOR0
{
    float raw=tex2D(sceneDepth,uv).r;
    float z=(raw-projection.z)*projection.w;
    // Clear-depth sky (1, beyond the native 0.94 range), far plane and invalid pixels.
    if (z<0.0 || z>=0.99999) return float4(fog.rgb,0.0);
    float forward=projection.y/min(z-projection.x,-0.000001);
    float3 ray=float3((float2(uv.x*2.0-1.0,1.0-uv.y*2.0)-rayScale.zw)*rayScale.xy,1.0);
    float rayLength=length(ray);
    float sceneDistance=forward*rayLength;
    float distance=min(sceneDistance,layer.w);
    float endHeight=worldZ.w+dot(ray,worldZ.xyz)*(distance/rayLength);
    float extinction=fog.w*distance*MeanDensity(worldZ.w,endHeight);
    // Low-frequency world-space modulation at the ray midpoint. Both temporal
    // harmonics wrap continuously. No screen-space texture or extra depth fetch.
    float3 midpoint=ray*(0.5*distance/rayLength);
    float2 position=float2(dot(midpoint,worldX.xyz)+worldX.w,dot(midpoint,worldY.xyz)+worldY.w);
    float variation=0.6*sin(position.x*0.018+position.y*0.013-mist.y)
                   +0.4*sin(position.x*(-0.011)+position.y*0.021-2.0*mist.y);
    if(mist.z>0.5 && mist.x>0.0)
    {
        // Integrate the exact height density of each segment and sample only its
        // slowly varying modulation. This preserves the analytic clear-mist limit.
        float3 worldDelta=float3(dot(ray,worldX.xyz),dot(ray,worldY.xyz),dot(ray,worldZ.xyz))*(distance/rayLength);
        float3 origin=float3(worldX.w,worldY.w,worldZ.w);
        float weighted=0.0;
        [loop] for(int i=0;i<8;++i)
        {
            float t=(i+0.5)/8.0;
            float density=MeanDensity(origin.z+worldDelta.z*(i/8.0),origin.z+worldDelta.z*((i+1)/8.0));
            weighted+=density*(1.0+mist.x*MistVariation(origin+worldDelta*t));
        }
        extinction=fog.w*distance*(weighted/8.0);
    }
    else extinction*=1.0+mist.x*variation;
    // Homogeneous distant air adds optical depth, not a second alpha overlay.
    // It shares density/weather/waterline gain and the final cap with the low layer.
    extinction*=1.0-mist.w; // temporary isolated-air comparison suppresses only the low layer
    // Distant air follows geometry beyond the bounded local mist march.
    // A separate safety limit bounds optical depth for unusually long views.
    extinction+=fog.w*nearFade.z*max(min(sceneDistance,2000.0)-nearFade.w,0.0);
    float opacity=min(1.0-exp(-extinction),layer.z)*saturate((distance-nearFade.x)*nearFade.y);
    float cosine=clamp(dot(ray/rayLength,celestial.xyz),-1.0,1.0);
    // Broad molecular lobe plus a forward aerosol lobe (g=0.65), normalized
    // to one at its peak. Native fog colour carries the zone/time palette.
    float molecular=0.5*(1.0+cosine*cosine);
    float aerosol=pow(0.1225/max(1.4225-1.3*cosine,0.1225),1.5);
    float3 litFog=saturate(fog.rgb*(1.0+celestial.w*(0.15*molecular+0.85*aerosol)));
    return float4(litFog,opacity);
}
