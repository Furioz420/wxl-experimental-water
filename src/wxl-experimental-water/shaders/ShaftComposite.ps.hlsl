// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
sampler2D sceneDepth : register(s0);
sampler2D shafts : register(s1);
float4 source : register(c0);
float4 depthRange : register(c1);
float4 projection : register(c2);
float4 color : register(c3);
float4 traceSize : register(c4); // width, height, inverse width, inverse height
float ViewDepth(float z) { return projection.y/min(z-projection.x,-.000001); }
float Compatible(float center,float other)
{
    if(center<0 || other<0) return 0;
    bool a=center>=.99999, b=other>=.99999;
    if(a || b) return a && b ? 1 : 0;
    float d=ViewDepth(center);
    float difference=abs(ViewDepth(other)-d);
    return 1-smoothstep(max(1.0,d*.05),max(2.0,d*.10),difference);
}
float Upsample(float2 uv,float z)
{
    float2 p=uv*traceSize.xy-.5;
    float2 base=floor(p), f=frac(p);
    float sum=0, weights=0;
    [unroll] for(int y=0;y<2;++y) [unroll] for(int x=0;x<2;++x)
    {
        float2 q=(clamp(base+float2(x,y),0,traceSize.xy-1)+.5)*traceSize.zw;
        // Snap to a full-resolution texel center; half-size centers can lie on depth texel boundaries.
        float2 guide=(floor(q*projection.zw+.0001)+.5)/projection.zw;
        float neighbor=(tex2Dlod(sceneDepth,float4(guide,0,0)).r-depthRange.x)*depthRange.y;
        float w=(x ? f.x : 1-f.x)*(y ? f.y : 1-f.y)*Compatible(z,neighbor);
        sum+=tex2Dlod(shafts,float4(q,0,0)).r*w;
        weights+=w;
    }
    // Thin geometry with no compatible low-resolution sample receives no shaft.
    return weights>0.00001 ? sum/weights : 0;
}
float4 main(float2 uv:TEXCOORD0):COLOR0
{
    float z=(tex2D(sceneDepth,uv).r-depthRange.x)*depthRange.y;
    float forward=projection.y/min(z-projection.x,-.000001);
    float gain=z>=.99999 ? 1 : saturate((forward-1)/40);
    if(z<0) gain=0;
    return float4(color.rgb*(Upsample(uv,z)*source.w*gain),0);
}
