// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "RenderSettings.hpp"
namespace wxl::water::vquality {
namespace settings=wxl::render::settings;
inline constexpr settings::Field fields[]{{"performance",0,1,true}};
inline constexpr wchar_t file[]=L"wxl-vfog-quality.ini";
inline bool performance=false,initialized=false;
inline const char* note="";
inline settings::Result Load() {
    initialized=true;
    std::array<float,1> values{performance ? 1.f : 0.f};
    const auto result=settings::Load(file,fields,values);
    if(result==settings::Result::Ok) performance=values[0]!=0;
    note=settings::Message(result); return result;
}
inline void Ensure() { if(!initialized) Load(); }
inline int Level() { Ensure(); return performance ? 1 : 2; }
inline void Save() { note=settings::Message(settings::Save(file,fields,std::array<float,1>{performance ? 1.f : 0.f})); }
}
