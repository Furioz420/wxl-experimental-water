// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#define main EvaluateFullFog
#include "HeightFog.ps.hlsl"
#undef main
sampler2D lowFog : register(s1);
float4 lowSize : register(c10);
float LinearDepth(float raw) {
    float z=(raw-projection.z)*projection.w;
    return projection.y/min(z-projection.x,-0.000001);
}
float4 main(float2 uv:TEXCOORD0):COLOR0 {
    float raw=tex2D(sceneDepth,uv).r;
    float z=(raw-projection.z)*projection.w;
    if(z<0 || z>=.99999) return float4(fog.rgb,0);
    float center=LinearDepth(raw);
    float2 pixel=uv*lowSize.xy-.5;
    float2 base=floor(pixel), fraction=frac(pixel);
    float4 sum=0; float total=0;
    [unroll] for(int y=0;y<2;++y) [unroll] for(int x=0;x<2;++x) {
        float2 sampleUv=(base+float2(x,y)+.5)*lowSize.zw;
        float sampleRaw=tex2D(sceneDepth,sampleUv).r;
        float sampleZ=(sampleRaw-projection.z)*projection.w;
        float difference=abs(LinearDepth(sampleRaw)-center);
        float weight=(x ? fraction.x : 1-fraction.x)*(y ? fraction.y : 1-fraction.y);
        // Reject opposite sides of silhouettes instead of blurring foreground fog.
        weight*=sampleZ>=0 && sampleZ<.99999 && difference<max(1.0,center*.02) ? 1.0 : 0.0;
        sum+=tex2D(lowFog,sampleUv)*weight; total+=weight;
    }
    // A thin foreground object may have no representative half-resolution sample.
    [branch] if(total<.05) return EvaluateFullFog(uv);
    return sum/total;
}
