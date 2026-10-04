#pragma once
#include "RenderSettings.hpp"

namespace wxl::water::upstream {
namespace settings=wxl::render::settings;
inline constexpr settings::Field fields[]{
    {"authored_noise",0,1,true},{"local_lights",0,1,true},{"water_ripples",0,2}
};
inline constexpr std::array<float,3> defaults{1,1,.5f};
inline std::array<float,3> values=defaults;
inline bool initialized=false;
inline constexpr wchar_t file[]=L"wxl-vfog-effects.ini";
inline void Load() { initialized=true; settings::Load(file,fields,values); }
inline void Ensure() { if(!initialized) Load(); }
inline settings::Result Save() { return settings::Save(file,fields,values); }
}
