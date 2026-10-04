// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "RenderSettings.hpp"
namespace wxl::water::vpreferences {
namespace settings=wxl::render::settings;
inline constexpr settings::Field fogFields[]{
    {"enabled",0,1,true},{"density",0,2},{"temporal",0,.95f}
};
inline constexpr settings::Field waterFields[]{
    {"enabled",0,1,true},{"clarity",0,2},{"waves",0,2},{"foam",0,2},{"reflections",0,2}
};
inline constexpr std::array<float,3> fogDefaults{1,1,.85f};
inline constexpr std::array<float,5> waterDefaults{1,1,1,1,1};
// Independent files leave accepted legacy/fallback preferences intact.
inline constexpr wchar_t fogFile[]=L"wxl-authored-fog.ini";
inline constexpr wchar_t waterFile[]=L"wxl-forever-water.ini";
}
