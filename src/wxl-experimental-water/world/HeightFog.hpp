// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "wxl/RenderDepthApi.h"
#include <cmath>
#include <initializer_list>
#include <algorithm>

namespace wxl::water::fog
{
    struct Constants { float row[10][4]{}; };
    struct Settings
    {
        float strength=1.0f;
        float height=35.0f;
        float maxOpacity=0.30f;
        bool isolateAerial=false;
        float aerial=0.0f; // session comparison: distant air independent of layer elevation
    };
    inline Settings StrongerPreview() { return {2.0f,50.0f,0.45f}; }
    inline bool PrepareMist(const WXL_SceneDepth& depth,double seconds,Constants& out,bool integrated=true)
    {
        for(int r=7;r<9;++r) for(float& x:out.row[r]) x=0;
        for(int i=0;i<3;++i) out.row[9][i]=0; // preserve independent low-layer bypass
        if(!std::isfinite(seconds) || seconds<0) return false;
        for(int axis=0;axis<2;++axis)
        {
            for(int i=0;i<3;++i) out.row[7+axis][i]=depth.view[axis*4+i];
            out.row[7+axis][3]=depth.cameraPosition[axis];
        }
        // Stronger local structure; base extinction and opacity cap are unchanged.
        out.row[9][0]=integrated ? 0.35f : 0.18f;
        out.row[9][2]=integrated ? 1.f : 0.f;
        out.row[9][1]=static_cast<float>(std::fmod(seconds*0.025,6.283185307179586));
        return true;
    }

    // Direction TOWARD the active native celestial source, transformed into the
    // same view basis as reconstructed depth rays. Invalid light is identity.
    inline bool PrepareLight(const WXL_SceneDepth& depth,const float toward[3],Constants& out)
    {
        for (float& x:out.row[6]) x=0;
        for(int i=0;i<3;++i) if(!std::isfinite(toward[i])) return false;
        const float length=std::sqrt(toward[0]*toward[0]+toward[1]*toward[1]+toward[2]*toward[2]);
        if(!std::isfinite(length) || length<0.0001f) return false;
        for(int axis=0;axis<3;++axis)
            for(int i=0;i<3;++i) out.row[6][axis]+=toward[i]*depth.view[i*4+axis]/length;
        const float elevation=std::clamp(toward[2]/length*4.0f,0.0f,1.0f);
        out.row[6][3]=0.25f*elevation*elevation*(3-2*elevation);
        return true;
    }

    // Fade only near a known liquid surface. Absolute world height cannot tell whether
    // a camera is underwater: a beach or valley below Z=2 is still dry ground.
    inline float WaterlineGain(bool hasPlane,float cameraZ,float planeZ,float surfaceZ)
    {
        if (!std::isfinite(cameraZ)) return 0;
        if (!hasPlane) return 1;
        if (!std::isfinite(planeZ) || !std::isfinite(surfaceZ)) return 0;
        const float t=std::clamp(cameraZ-std::max(planeZ,surfaceZ)-0.10f,0.0f,1.0f);
        return t*t*(3-2*t);
    }

    // Initial coastal profile. Fixed world-space height, never attached to the camera.
    // This augments native zone fog with a bounded low layer; it does not replace it.
    inline bool Prepare(const WXL_SceneDepth& depth, const float color[3], Constants& out,
                        const Settings& settings={},float baseHeight=0)
    {
        out = {};
        if (!std::isfinite(baseHeight) || baseHeight < -10000 || baseHeight > 10000) return false;
        if (!std::isfinite(settings.aerial) || settings.aerial<0 || settings.aerial>1) return false;
        if (!std::isfinite(settings.strength) || settings.strength<0 || settings.strength>3 ||
            !std::isfinite(settings.height) || settings.height<5 || settings.height>200 ||
            !std::isfinite(settings.maxOpacity) || settings.maxOpacity<0 || settings.maxOpacity>0.6f) return false;
        if (!depth.width || !depth.height || !depth.texture) return false;
        for (float v : depth.view) if (!std::isfinite(v)) return false;
        for (float v : depth.projection) if (!std::isfinite(v)) return false;
        for (float v : depth.cameraPosition) if (!std::isfinite(v)) return false;
        for (int i=0; i<3; ++i) if (!std::isfinite(color[i]) || color[i]<0 || color[i]>1) return false;
        const auto& p = depth.projection;
        if (p[0]<=0 || p[5]<=0 || p[10]<=1 || p[14]>=0 ||
            std::abs(p[11]-1)>0.00001f || std::abs(p[15])>0.00001f ||
            !std::isfinite(depth.depthMinZ) || !std::isfinite(depth.depthMaxZ) ||
            depth.depthMinZ<0 || depth.depthMaxZ>1 || depth.depthMaxZ-depth.depthMinZ<0.0001f)
            return false;
        // Only perspective projections with an orthonormal camera rotation. Reject skew
        // rather than silently applying the wrong inverse to world-space fog heights.
        for (int i : {1,2,3,4,6,7,12,13}) if (std::abs(p[i])>0.00001f) return false;
        const auto& v = depth.view;
        for (int a=0; a<3; ++a) for (int b=0; b<3; ++b)
        {
            const float dot = v[a]*v[b]+v[4+a]*v[4+b]+v[8+a]*v[8+b];
            if (std::abs(dot-(a==b ? 1.0f : 0.0f))>0.002f) return false;
        }
        if (std::abs(v[3])+std::abs(v[7])+std::abs(v[11])+std::abs(v[15]-1)>0.0001f) return false;
        for (int axis=0; axis<3; ++axis)
        {
            const float camera = -(v[12]*v[axis*4]+v[13]*v[axis*4+1]+v[14]*v[axis*4+2]);
            if (std::abs(camera-depth.cameraPosition[axis])>0.25f) return false;
        }
        out.row[0][0]=p[10]; out.row[0][1]=p[14]; out.row[0][2]=depth.depthMinZ;
        out.row[0][3]=1/(depth.depthMaxZ-depth.depthMinZ);
        out.row[1][0]=1/p[0]; out.row[1][1]=1/p[5]; out.row[1][2]=p[8]; out.row[1][3]=p[9];
        out.row[2][0]=v[8]; out.row[2][1]=v[9]; out.row[2][2]=v[10]; out.row[2][3]=depth.cameraPosition[2];
        for (int i=0; i<3; ++i) out.row[3][i]=color[i];
        out.row[3][3]=0.002f*settings.strength; // extinction per world unit at/below sea level
        out.row[4][0]=baseHeight; out.row[4][1]=1/settings.height; out.row[4][2]=settings.maxOpacity; out.row[4][3]=400;
        out.row[5][0]=12; out.row[5][1]=1/48.0f;
        out.row[9][3]=settings.isolateAerial ? 1.f : 0.f;
        out.row[5][2]=0.2f*settings.aerial; out.row[5][3]=80;
        return true;
    }
}
