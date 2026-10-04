// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cmath>
namespace wxl::water::celestial
{
    enum class Body { Unavailable, Sun, Moon };
    inline Body SelectBody(float phase) {
        if(!std::isfinite(phase) || phase<0 || phase>1) return Body::Unavailable;
        return phase>=0.2013888955116272f && phase<=0.9236111044883728f ? Body::Sun : Body::Moon;
    }
    // Native primary sun/moon selection observed in client routine 0x007EFAE0.
    inline bool Direction(float phase,const float origin[3],const float sun[3],const float moon[3],float out[3])
    {
        const auto selected=SelectBody(phase);
        if(selected==Body::Unavailable) return false;
        const float* body=selected==Body::Sun ? sun : moon;
        float delta[3]{},length2=0;
        for(int i=0;i<3;++i) {
            if(!std::isfinite(origin[i]) || !std::isfinite(body[i])) return false;
            delta[i]=body[i]-origin[i]; length2+=delta[i]*delta[i];
        }
        if(!std::isfinite(length2) || length2<.0001f) return false;
        const float inverse=1/std::sqrt(length2);
        for(int i=0;i<3;++i) out[i]=delta[i]*inverse;
        return true;
    }
}
