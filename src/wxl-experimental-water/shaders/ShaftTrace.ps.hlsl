// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
sampler2D sceneDepth : register(s0);
float4 source : register(c0); // source UV, aspect ratio, final strength
float4 depthRange : register(c1); // minimum, inverse range, source radius, radial decay
float4 main(float2 uv:TEXCOORD0):COLOR0
{
    float sum=0;
    // Deterministic reduced-resolution radial integration; no temporal history.
    [loop] for(int i=0;i<24;++i)
    {
        float t=(i+.5)/24.0;
        float2 q=lerp(uv,source.xy,t*.96);
        if (q.x>=0 && q.x<=1 && q.y>=0 && q.y<=1)
        {
            float2 delta=(q-source.xy)*float2(source.z,1)/depthRange.z;
            float radius=dot(delta,delta);
            if(radius<4)
            {
                float raw=tex2Dlod(sceneDepth,float4(q,0,0)).r;
                float sky=step(.99999,(raw-depthRange.x)*depthRange.y);
                sum+=sky*exp(-2*radius)*(1-depthRange.w*t);
            }
        }
    }
    return float4(sum/24.0,sum/24.0,sum/24.0,1);
}
