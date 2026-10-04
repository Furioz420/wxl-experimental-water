// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace wxl::water::look {
inline float Weight(float coverage) { return std::isfinite(coverage) ? std::clamp((coverage-.5f)*2,0.f,1.f) : 0; }
inline uint8_t GlowByte(uint8_t client,float glow,float weight) {
    if(!std::isfinite(glow) || !std::isfinite(weight)) return client;
    const float blended=client/255.f+(glow-client/255.f)*std::clamp(weight,0.f,1.f);
    return static_cast<uint8_t>(std::floor(std::clamp(blended,0.f,1.f)*255+.5f));
}
// Restore only bytes still bearing our value. A failed restore retains its journal.
class GlowTransaction {
    struct Entry { uintptr_t pass=0; uint8_t original=0,fed=0; } entries[2];
public:
    template<class Read,class Write> bool Restore(Read read,Write write) {
        bool ok=true;
        for(auto& e:entries) if(e.pass) {
            uint8_t current=0;
            if(!read(e.pass,current) || (current==e.fed && !write(e.pass,e.original))) { ok=false; continue; }
            e={};
        }
        return ok;
    }
    template<class Read,class Write> bool Begin(const uintptr_t (&passes)[2],float glow,float weight,Read read,Write write,float& delivered) {
        if(!Restore(read,write) || !passes[0] || !passes[1] || passes[0]==passes[1]) return false;
        uint8_t original[2]{};
        for(int i=0;i<2;++i) if(!read(passes[i],original[i])) return false;
        for(int i=0;i<2;++i) {
            const uint8_t fed=GlowByte(original[i],glow,weight);
            if(fed!=original[i]) {
                entries[i]={passes[i],original[i],fed};
                if(!write(passes[i],fed)) { Restore(read,write); return false; }
            }
        }
        uint8_t value=0;
        if(!read(passes[0],value)) { Restore(read,write); return false; }
        delivered=value/255.f; return true;
    }
};
}
