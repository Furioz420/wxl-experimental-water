// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "RenderSettings.hpp"
#include <algorithm>
namespace wxl::water::atmospheresettings {
    namespace settings=wxl::render::settings;
    inline constexpr settings::Field legacy[]{ {"enabled",0,1,true},{"directional",0,1,true},
        {"mist",0,1,true},{"shafts",0,1,true},{"shaftStrength",0,.6f},
        {"hazeStrength",0,3},{"layerHeight",5,200},{"maxHaze",0,.6f} };
    inline constexpr settings::Field previous[]{ {"enabled",0,1,true},{"directional",0,1,true},
        {"mist",0,1,true},{"shafts",0,1,true},{"shaftStrength",0,.6f},
        {"hazeStrength",0,3},{"layerHeight",5,200},{"maxHaze",0,.6f},{"shaftSpread",.08f,.18f} };
    inline constexpr settings::Field fields[]{ {"enabled",0,1,true},{"directional",0,1,true},
        {"mist",0,1,true},{"shafts",0,1,true},{"shaftStrength",0,.6f},
        {"hazeStrength",0,3},{"layerHeight",5,200},{"maxHaze",0,.6f},{"shaftSpread",.08f,.18f},{"distantAir",0,1} };
    using Values=std::array<float,10>;
    inline settings::Result Load(const std::filesystem::path& current,const std::filesystem::path& old,
                                const std::filesystem::path& oldest,Values& values) {
        Values next{};
        auto result=settings::Load(current,fields,next);
        if(result==settings::Result::Missing) {
            std::array<float,9> shape{};
            result=settings::Load(old,previous,shape);
            if(result==settings::Result::Ok) std::copy(shape.begin(),shape.end(),next.begin());
            else if(result==settings::Result::Missing) {
                std::array<float,8> original{};
                result=settings::Load(oldest,legacy,original);
                if(result==settings::Result::Ok) { std::copy(original.begin(),original.end(),next.begin()); next[8]=.12f; }
            }
        }
        if(result==settings::Result::Ok) values=next;
        return result;
    }
}
