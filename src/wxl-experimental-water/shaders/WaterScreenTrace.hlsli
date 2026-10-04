// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
// Original WXL depth-tested screen-space tracing. Matrices match the captured
// depth camera, not a later material's modelview. Raw depth includes viewport mapping.
float4 gSsrRowX : register(c47);
float4 gSsrRowY : register(c48);
float4 gSsrRowW : register(c49);
float4 gSsr : register(c50); // strength, range, scene-valid, normal ripple
float4 gDepthProjection : register(c51); // P33, P43, MinZ, 1/(MaxZ-MinZ)
float4 gWaterDepth : register(c52); // valid, unused
sampler2D sWaterDepth : register(s15);

float WaterViewDepth(float2 uv)
{
    float raw=tex2Dlod(sWaterDepth,float4(uv,0,0)).r;
    float ndc=(raw-gDepthProjection.z)*gDepthProjection.w;
    if (ndc<0.0 || ndc>=0.99999) return 100000.0;
    return gDepthProjection.y/(ndc-gDepthProjection.x);
}
float3 WaterProject(float3 world)
{
    float4 p=float4(world,1);
    float z=dot(p,gSsrRowW);
    return float3(float2(dot(p,gSsrRowX),-dot(p,gSsrRowY))*(0.5/max(z,0.001))+0.5,z);
}
float2 WaterRefractionUv(float2 center,float2 bent,float surfaceDepth)
{
    // Missing depth keeps undistorted transmission, never foreground smearing.
    if (gWaterDepth.x<0.5 || any(bent<0.0) || any(bent>1.0)) return center;
    float z=WaterViewDepth(bent);
    return (z>surfaceDepth+0.03 && z<99999.0) ? bent : center;
}
float WaterGeometryPath(float2 uv,float surfaceDepth,float cameraDistance)
{
    // Negative means unavailable, not zero-thickness contact. Share this single
    // depth lookup between transmission and contact foam.
    if (gWaterDepth.x<0.5 || surfaceDepth<=0.1 || any(uv<0.0) || any(uv>1.0)) return -1.0;
    float behind=WaterViewDepth(uv);
    if (behind<surfaceDepth || behind>=99999.0) return -1.0;
    float measured=(behind-surfaceDepth)*max(cameraDistance/surfaceDepth,1.0);
    return max(measured,0.0);
}
float WaterTransmissionPath(float2 uv,float surfaceDepth,float cameraDistance,float fallback)
{
    float measured=WaterGeometryPath(uv,surfaceDepth,cameraDistance);
    return measured>=0.0 ? min(fallback,measured) : fallback;
}
float WaterContactFoam(float measured)
{
    // A restrained half-unit contact band; existing foam textures supply breakup.
    // No depth/foreground/sky must never become a white sheet.
    if (measured<0.0) return 0.0;
    return 0.35*(1.0-smoothstep(0.0,0.5,measured));
}
float WaterReflectionEdge(float2 uv,float depth,float2 texel)
{
    float border=min(min(uv.x,uv.y),min(1-uv.x,1-uv.y));
    float edge=smoothstep(0.0,0.085,border);
    // Fade a one-pixel neighborhood where a hit cannot be trusted across a silhouette.
    // A sky neighbor is also a discontinuity. Cost is paid only for a candidate hit.
    float difference=max(abs(WaterViewDepth(uv+float2(texel.x,0))-depth),abs(WaterViewDepth(uv-float2(texel.x,0))-depth));
    difference=max(difference,max(abs(WaterViewDepth(uv+float2(0,texel.y))-depth),abs(WaterViewDepth(uv-float2(0,texel.y))-depth)));
    float tolerance=max(0.35,depth*0.02);
    return edge*(1.0-smoothstep(tolerance,tolerance*3.0,difference));
}
float WaterReflectionRange(float fraction)
{
    return 1.0-smoothstep(0.85,1.0,fraction);
}
float3 WaterReflectionHit(float3 origin,float3 direction,float2 texel)
{
    float3 hit=0;
    if (gWaterDepth.x<0.5 || gSsr.z<0.5 || gSsr.x<=0.0) return hit;
    float previous=0.25;
    float3 start=WaterProject(origin+direction*previous);
    float2 startUv=start.xy+texel*0.5;
    if(start.z<=0.1 || any(startUv<0.0) || any(startUv>1.0)) return hit;
    float previousGap=start.z-WaterViewDepth(startUv);
    float range=clamp(gSsr.y,8.0,160.0);
    // Quadratic spacing spends samples near the surface; a bounded loop limits
    // cost. A crossing is refined and rejected if it hits a depth discontinuity.
    [loop] for (int index=1;index<=20;++index)
    {
        float q=index/20.0;
        float distance=0.25+q*q*range;
        float3 projected=WaterProject(origin+direction*distance);
        float2 uv=projected.xy+texel*0.5;
        if (projected.z<=0.1 || any(uv<0.0) || any(uv>1.0)) break;
        float depth=WaterViewDepth(uv);
        float gap=projected.z-depth;
        if (depth<99999.0 && gap>=0.0 && previousGap<0.0)
        {
            float lo=previous,hi=distance;
            [loop] for (int refine=0;refine<6;++refine)
            {
                float mid=(lo+hi)*0.5;
                float3 p=WaterProject(origin+direction*mid);
                float d=WaterViewDepth(p.xy+texel*0.5);
                if (p.z>d) hi=mid; else lo=mid;
            }
            float3 p=WaterProject(origin+direction*hi);
            uv=p.xy+texel*0.5;
            depth=WaterViewDepth(uv);
            float tolerance=0.12+0.002*p.z;
            float confidence=saturate(1.0-abs(p.z-depth)/tolerance);
            float edge=WaterReflectionEdge(uv,depth,texel);
            hit=float3(uv,confidence*edge*WaterReflectionRange((hi-0.25)/range));
            break;
        }
        previous=distance;
        previousGap=gap;
    }
    return hit;
}
