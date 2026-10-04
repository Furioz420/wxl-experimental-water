// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cmath>
namespace wxl::water {
class StockFogReplacement {
    bool previousSuccess_=false,replaced_=false,pushed_=false;
    std::array<float,4> saved_{};
public:
    bool Begin(bool eligible,const std::array<float,4>& ranges) {
        const bool ready=previousSuccess_; previousSuccess_=false; replaced_=false;
        if(pushed_ || !eligible || !ready) return false;
        for(float value:ranges) if(!std::isfinite(value) || value<0 || value>100000) return false;
        if(ranges[1]<=ranges[0] || ranges[3]<=ranges[2]) return false;
        saved_=ranges; pushed_=replaced_=true; return true;
    }
    void Success(bool early) { if(early) previousSuccess_=true; }
    bool Pushed() const { return pushed_; }
    bool Replaced() const { return replaced_; }
    const std::array<float,4>& Saved() const { return saved_; }
    void Restored() { pushed_=false; }
    // Never discard saved ranges while a failed restore still needs them.
    void Invalidate() { previousSuccess_=false; replaced_=false; }
};
}
