// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cmath>
#include <algorithm>
namespace wxl::water::fog {
// Local visual phase only; never modifies the shared wind or water simulation.
class MistDrift {
    double last=-1, x=0, y=0;
public:
    void Reset() { last=-1; x=y=0; }
    float X() const { return static_cast<float>(x); }
    float Y() const { return static_cast<float>(y); }
    void Update(double now,float speed,float dx,float dy) {
        if(!std::isfinite(now) || now<0) { last=-1; return; }
        const double dt=last<0 ? 0 : now-last; last=now;
        // Never catch up a loading screen, paused effect or clock discontinuity.
        if(dt<=0 || dt>1 || !std::isfinite(speed) || !std::isfinite(dx) || !std::isfinite(dy) || speed<=0) return;
        const double length=std::hypot(double(dx),double(dy));
        if(length<.0001 || length>2) return;
        const double travel=std::min(double(speed),40.0)*.15*dt;
        // All spatial mist frequencies are integer multiples of .001.
        // This common period wraps the complete warped field continuously.
        constexpr double period=6283.185307179586;
        x=std::remainder(x+travel*dx/length,period);
        y=std::remainder(y+travel*dy/length,period);
    }
};
}
