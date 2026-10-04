#include "wxl/RenderDefaults.hpp"
#include "WeatherHaze.hpp"
#include "VFogPass.hpp"
#include "VWaterPass.hpp"
#include "wxl-experimental-water/render/Surface.hpp"
#include "MistDrift.hpp"
#include "HalfFogPass.hpp"
#include "wxl/EnvironmentWind.hpp"
#include "DayNightPreview.hpp"
#include "engine/hook/Hook.hpp"
#include "offsets/game/World.hpp"
#include "offsets/game/Weather.hpp"
// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
// Opt-in coastal height-fog prototype using WXL's ordered atmosphere stage.
#include "config.hpp"
#include "wxl/RenderReloadKey.hpp"
#include "wxl/InteriorApi.h"
#include "common/Log.hpp"
#include "engine/events/Event.hpp"
#include "engine/hook/Registry.hpp"
#include "engine/render/PreviewHotkey.hpp"
#include "engine/ui/ImGuiHost.hpp"
#include "imgui.h"
#include "game/Render.hpp"
#include "offsets/engine/Sky.hpp"
#include "wxl-experimental-water/sea/Sea.hpp"
#include "wxl-experimental-water/mesh/Lattice.hpp"
#include "wxl-experimental-water/world/HeightFogPass.hpp"
#include "wxl-experimental-water/world/LightShafts.hpp"
#include "wxl-experimental-water/world/NativeCelestialSource.hpp"
#include "wxl-experimental-water/shaders/HeightFogPs.h"
#include <algorithm>
#include "AtmosphereSettings.hpp"
#include "NativeViewerLiquid.hpp"

namespace
{
    namespace fog=wxl::water::fog;
    namespace vfog=wxl::water::vfog;
    fog::MistDrift g_mistDrift;
    const WXL_RenderDepthApi* g_depth=nullptr;
    const WXL_InteriorApi* g_interior=nullptr;
    Microsoft::WRL::ComPtr<IDirect3DPixelShader9>& Shader()
    {
        // Process lifetime, released explicitly on device loss; avoid driver calls under
        // the DLL loader lock from a global COM destructor at process shutdown.
        static auto* shader=new Microsoft::WRL::ComPtr<IDirect3DPixelShader9>;
        return *shader;
    }
    wxl::water::shafts::Pass& Shafts()
    { static auto* pass=new wxl::water::shafts::Pass; return *pass; }
    fog::HalfPass& HalfFog() { static auto* pass=new fog::HalfPass; return *pass; }
    bool g_shafts=true;
    float g_shaftStrength=.18f;
    float g_shaftSpread=.12f;
    bool g_shaftVisible=false;
    wxl::water::celestial::Body g_celestialBody=wxl::water::celestial::Body::Unavailable;
    wxl::water::celestial::Body g_reportedBody=wxl::water::celestial::Body::Unavailable;
    unsigned g_sourceReports=0;
    bool g_showShaftOrigin=false; // diagnostic only; never saved as an appearance preference
    float g_shaftOrigin[2]{};
    unsigned g_shaftReports=0;
    uint64_t g_generation=0;
    uint64_t g_earlyFogFrame=0,g_earlyFogGeneration=0;
    bool g_enabled=true;
    bool g_directional=true;
    bool g_mist=true;
    bool g_integratedMist=true; // session A/B choice; existing saved settings remain compatible
    bool g_shaderFailed=false;
    wxl::render::depth::PreviewHotkey g_key;
    int g_status=-1;
    unsigned g_drawReports=0;
    fog::Settings g_settings;
    bool g_timePreview=false;
    float g_previewHour=0;
    bool g_timeHook=false;
    using TimeUpdateFn=void(__fastcall*)(void*,void*,float);
    TimeUpdateFn g_originalTimeUpdate=nullptr;
    void __fastcall TimeUpdate(void* frame,void* edx,float elapsed) {
        g_originalTimeUpdate(frame,edx,elapsed);
        if(!g_timePreview) return;
        namespace sky=wxl::offsets::engine::sky;
        auto* input=static_cast<wxl::water::celestial::TimeInput*>(
            reinterpret_cast<sky::DayNightGetInfoFn>(sky::kDayNightGetInfo)());
        wxl::water::celestial::ApplyTimePreview(input,true,g_previewHour);
    }
    bool g_weatherPreview=false;
    float g_weatherRaw=0;
    int g_weatherKind=0;
    bool g_weatherAvailable=false;
    const char* WeatherKindName(int kind) {
        return kind==1 ? "Rain" : kind==2 ? "Snow" : kind==3 ? "Sand" : "No precipitation";
    }
    // Evidence only: bounded and throttled, with no retained rendering response.
    unsigned g_weatherReports=0;
    int g_lastWeatherReport=-1;
    ULONGLONG g_nextWeatherReport=0;
    float g_waterlineGain=1;
    float g_mistBase=0,g_viewHeight=0;
    bool g_canPlaceMist=false;
    namespace settings=wxl::render::settings;
    namespace preferences=wxl::water::atmospheresettings;
    constexpr auto& kFields=preferences::fields;
    constexpr wchar_t kSettingsFile[]=L"wxl-atmosphere-air.ini";
    preferences::Values g_startup{};
    const char* g_note="";
    preferences::Values Values()
    { return {g_enabled ? 1.f : 0.f,g_directional ? 1.f : 0.f,g_mist ? 1.f : 0.f,g_shafts ? 1.f : 0.f,
        g_shaftStrength,g_settings.strength,g_settings.height,g_settings.maxOpacity,g_shaftSpread,g_settings.aerial}; }
    void Apply(const preferences::Values& values)
    {
        g_enabled=values[0]!=0; g_directional=values[1]!=0; g_mist=values[2]!=0; g_shafts=values[3]!=0;
        g_shaftStrength=values[4]; g_settings={values[5],values[6],values[7]}; g_shaftSpread=values[8]; g_settings.aerial=values[9];
    }
    settings::Result LoadSettings()
    {
        g_weatherPreview=false; g_timePreview=false; g_settings.isolateAerial=false;
        auto values=Values(); auto result=preferences::Load(kSettingsFile,L"wxl-atmosphere-shape.ini",L"wxl-atmosphere.ini",values);
        if(result==settings::Result::Ok) Apply(values);
        WLOG_INFO("render-fog: load settings result=%u",static_cast<unsigned>(result));
        return result;
    }
    void Report(int status,const char* reason)
    {
        if (status==g_status) return;
        g_status=status;
        WLOG_INFO("render-fog: %s",reason);
    }
    void OnInput(void*,const void* args)
    {
        const auto* a=static_cast<const wxl::events::InputArgs*>(args);
        static wxl::render::ReloadKey reload;
        if(reload.Handle(a->message,a->wparam,a->handled)) {
            vfog::Reload();
            const auto result=LoadSettings();
            g_note=result==settings::Result::Ok ? "F12: saved atmosphere loaded; mist placement kept." : settings::Message(result);
        }
        const bool before=g_enabled;
        if (g_key.Handle(a->message,a->wparam,g_enabled,VK_F7)) *a->handled=true;
        if (before!=g_enabled) WLOG_INFO("render-fog: %s (F7)",g_enabled ? "enabled" : "disabled");
    }
    void __cdecl Panel(void*)
    {
        if (ImGui::Checkbox("Enabled (F7)",&g_enabled))
            WLOG_INFO("render-fog: %s (panel)",g_enabled ? "enabled" : "disabled");
        if(g_timeHook && ImGui::CollapsingHeader("Time of day test (temporary)")) {
            bool timeChanged=ImGui::Checkbox("Preview client time",&g_timePreview);
            timeChanged|=ImGui::SliderFloat("Preview hour",&g_previewHour,0,23.9f,"%.1f h");
            if(ImGui::Button("Midnight")) { g_previewHour=0; g_timePreview=true; timeChanged=true; }
            ImGui::SameLine();
            if(ImGui::Button("Noon")) { g_previewHour=12; g_timePreview=true; timeChanged=true; }
            ImGui::SameLine();
            if(ImGui::Button("Live time")) { g_timePreview=false; timeChanged=true; }
            ImGui::TextWrapped("Local sky and lighting only. Server time stays live. Not saved; Live time, F12 or leaving the world clears the preview.");
            if(timeChanged) WLOG_INFO("render-time-preview: enabled=%u hour=%.1f",g_timePreview,g_previewHour);
        }
        vfog::Panel();
        bool changed=false;
        if (ImGui::CollapsingHeader("Weather response")) {
            ImGui::Text("Last native weather: %s",g_weatherAvailable ? WeatherKindName(g_weatherKind) : "Unavailable");
            ImGui::Text("Last native storm response: %.0f%%",100.f*fog::StormAmount(fog::NativeWeatherInput(g_weatherRaw,g_weatherKind,g_weatherAvailable)));
            ImGui::TextWrapped("Fine weather removes the extra storm haze; normal zone fog and your base haze remain.");
            ImGui::Checkbox("Preview storm atmosphere (temporary)",&g_weatherPreview);
            ImGui::TextWrapped("Preview changes fog weather response without creating rain or snow. F12 clears it; Save does not store it.");
        }
        if(vfog::Enabled()) return;
        if(ImGui::Button("Place mist here") && g_canPlaceMist) {
            g_mistBase=std::clamp(g_viewHeight-2.f,-10000.f,10000.f);
            g_note="Mist anchored here for this world session. Save does not store its elevation.";
            WLOG_INFO("render-fog: session base placed at %.2f",g_mistBase);
        }
        ImGui::SameLine();
        if(ImGui::Button("Sea level")) { g_mistBase=0; g_note="Mist returned to sea level."; }
        ImGui::Text("Mist elevation: %.1f (session only)",g_mistBase);
        ImGui::Checkbox("Light shafts",&g_shafts);
        ImGui::SliderFloat("Shaft strength",&g_shaftStrength,0,.6f,"%.2f");
        ImGui::Checkbox("Show shaft origin (diagnostic)",&g_showShaftOrigin);
        if(g_showShaftOrigin)
        {
            ImGui::Text("Native source: %s",g_celestialBody==wxl::water::celestial::Body::Sun ? "Sun" : g_celestialBody==wxl::water::celestial::Body::Moon ? "Moon" : "Unavailable / pass paused");
            if(g_shaftVisible)
            {
                const auto size=ImGui::GetIO().DisplaySize;
                const ImVec2 point(g_shaftOrigin[0]*size.x,g_shaftOrigin[1]*size.y);
                auto* overlay=ImGui::GetForegroundDrawList();
                const auto cyan=IM_COL32(0,255,255,255);
                overlay->AddCircle(point,14,cyan,32,2);
                overlay->AddLine(ImVec2(point.x-22,point.y),ImVec2(point.x+22,point.y),cyan,2);
                overlay->AddLine(ImVec2(point.x,point.y-22),ImVec2(point.x,point.y+22),cyan,2);
                overlay->AddText(ImVec2(point.x+18,point.y+16),cyan,"Computed shaft origin");
                ImGui::Text("Source UV: %.3f, %.3f",g_shaftOrigin[0],g_shaftOrigin[1]);
                ImGui::TextWrapped("Keep this panel open. The cyan cross should coincide with the visible sun or moon. This does not change the effect.");
            }
            else ImGui::TextWrapped("%s",g_status==7 ? "Outdoor light source hidden inside this interior." :
                "No active shaft source to mark in this view. Turn toward the outdoor sun or moon.");
        }
        ImGui::TextDisabled("%s",!g_enabled || !g_shafts || g_shaftStrength<=0 ? "Shafts off" :
            g_status==7 ? "Outdoor shafts paused indoors" :
            g_shaftVisible ? "Light source in view; geometry controls visibility" : "Turn toward the outdoor light source");
        if (ImGui::Button("Natural")) { g_weatherPreview=false; g_mistBase=0; g_settings={}; g_directional=true; g_mist=true; g_shafts=true; g_shaftStrength=.18f; g_shaftSpread=.12f; changed=true; }
        ImGui::SameLine();
        if (ImGui::Button("Stronger preview")) { g_settings=fog::StrongerPreview(); g_shaftStrength=.5f; changed=true; }
        changed|=ImGui::SliderFloat("Haze strength",&g_settings.strength,0,3,"%.2fx");
        changed|=ImGui::SliderFloat("Distant air",&g_settings.aerial,0,1,"%.2f");
        if(ImGui::CollapsingHeader("Advanced atmosphere"))
        {
            ImGui::TextUnformatted("Shaft spread");
            if(ImGui::Button("Focused")) g_shaftSpread=.08f;
            ImGui::SameLine(); if(ImGui::Button("Natural##shaft")) g_shaftSpread=.12f;
            ImGui::SameLine(); if(ImGui::Button("Soft")) g_shaftSpread=.18f;
            ImGui::Text("Current: %s",g_shaftSpread<.10f ? "Focused" : g_shaftSpread>.15f ? "Soft" : "Natural");
            ImGui::Checkbox("Directional light",&g_directional);
            ImGui::Checkbox("Moving mist",&g_mist);
            ImGui::TextWrapped("Mist drift follows the shared wind; its local animation continues in calm weather.");
            if(ImGui::Checkbox("Isolate distant air",&g_settings.isolateAerial)) {
                if(g_settings.isolateAerial) g_settings.aerial=1;
                WLOG_INFO("render-fog: isolated distant air=%u",g_settings.isolateAerial);
            }
            ImGui::TextWrapped("For comparison: hides the added low mist only. Keep checked and compare Distant air 0 and 1. Native zone fog remains. Isolation is temporary; Save stores the amount, not isolation.");

            ImGui::Checkbox("Layered mist (session comparison)",&g_integratedMist);
            changed|=ImGui::SliderFloat("Layer height",&g_settings.height,5,200,"%.0f");
            changed|=ImGui::SliderFloat("Maximum haze",&g_settings.maxOpacity,0,0.6f,"%.2f");
        }
        if(ImGui::Button("Save"))
        {
            const auto result=settings::Save(kSettingsFile,kFields,Values());
            g_note=result==settings::Result::Ok ? "Atmosphere saved for next time." : settings::Message(result);
            WLOG_INFO("render-fog: save settings result=%u",static_cast<unsigned>(result));
        }
        ImGui::SameLine();
        if(ImGui::Button("Reload"))
        {
            const auto result=LoadSettings();
            g_note=result==settings::Result::Ok ? "Saved atmosphere loaded." : settings::Message(result);
        }
        if(ImGui::Button("Restore startup")) { g_timePreview=false; g_weatherPreview=false; g_mistBase=0; Apply(g_startup); g_note="Startup atmosphere restored; saved file unchanged."; }
        ImGui::TextWrapped("%s",g_note);
        if (changed)
            WLOG_INFO("render-fog: settings strength=%.3f height=%.1f cap=%.3f",
                g_settings.strength,g_settings.height,g_settings.maxOpacity);
        const char* status=!g_enabled || (g_settings.strength==0 || g_settings.maxOpacity==0) && (!g_shafts || g_shaftStrength==0) ? "Off" :
            g_status==6 ? (g_waterlineGain<1 ? "Fading near the waterline" : "Active") :
            g_status==1 ? "Paused at or below the waterline" :
            g_status==2 ? "Inactive in this area" :
            g_status==7 ? "Paused indoors; native fog retained" : "Waiting for a supported world view";
        ImGui::Text("Status: %s",status);
        ImGui::TextWrapped("Coastal haze and outdoor light shafts. Natural restores the subtle default. Save keeps these settings for next time.");
    }
    void ClearSource(void*,const void*) { g_shaftVisible=false; g_celestialBody=wxl::water::celestial::Body::Unavailable; g_canPlaceMist=false; }
    void OnWorldLeave(void*,const void*) { vfog::Reset(); g_mistDrift.Reset(); g_settings.isolateAerial=false; g_timePreview=false; g_weatherPreview=false; g_weatherRaw=0; g_weatherKind=0; g_weatherAvailable=false; g_lastWeatherReport=-1; g_mistBase=0; g_canPlaceMist=false; g_shaftVisible=false; g_celestialBody=wxl::water::celestial::Body::Unavailable; }
    void OnLost(void*,const void*) { vfog::Reset(); HalfFog().Reset(); g_mistDrift.Reset(); g_canPlaceMist=false; Shafts().Reset(); g_shaftVisible=false; g_celestialBody=wxl::water::celestial::Body::Unavailable; Shader().Reset(); g_generation=0; g_status=-1; g_shaderFailed=false; }
    void __cdecl OnPass(void*,const WXL_RenderPassContext* pass)
    {
        const bool transparent=pass->stage==WXL_RENDER_BEFORE_TRANSPARENTS;
        const bool early=transparent || pass->stage==WXL_RENDER_BEFORE_SCREEN_EFFECTS;
        if(transparent && !vfog::TransparentReady())return;
        if(early && !vfog::Enabled()) return;
        if(!early && g_earlyFogFrame==pass->frameId && g_earlyFogGeneration==pass->deviceGeneration) return;
        g_shaftVisible=false; g_celestialBody=wxl::water::celestial::Body::Unavailable;
        g_canPlaceMist=false;
        if (!g_enabled) return;
        if (!g_interior)
            g_interior=static_cast<const WXL_InteriorApi*>(wxl::runtime::extensions::GetInterface(
                "wxl.interior",WXL_INTERIOR_API_VERSION));
        const bool enclosed=WxlViewerEnclosed(g_interior);
        WXL_SceneDepth depth{}; depth.structSize=sizeof(depth);
        const bool singleSample=g_depth->GetSceneDepth(&depth)!=0;
        if ((!singleSample && !vfog::MultisampleDepth(static_cast<IDirect3DDevice9*>(pass->device),depth)) || depth.frameId!=pass->frameId ||
            depth.deviceGeneration!=pass->deviceGeneration) { Report(0,"skipped: no current readable depth"); return; }
        if(!transparent && early && vfog::FinishTransparent(static_cast<IDirect3DDevice9*>(pass->device),depth))return;
        // Read the existing wave/plane model without changing the accepted surface's eye state.
        wxl::water::WaterPlane plane;
        const bool hasPlane=wxl::water::PlaneAt(depth.cameraPosition[0],depth.cameraPosition[1],plane);
        const float surface=hasPlane ? wxl::water::SampleAt(plane,depth.cameraPosition[0],depth.cameraPosition[1]).surface : 0;
        const auto nativeLiquid=wxl::water::ReadViewerLiquid();
        g_waterlineGain=std::min(nativeLiquid.AtmosphereGain(),
            fog::WaterlineGain(hasPlane,depth.cameraPosition[2],plane.level,surface));
        if(wxl::water::forever::UsedInFrame(pass->frameId)) g_waterlineGain=nativeLiquid.AtmosphereGain();
        if (g_waterlineGain<=0)
        { Report(1,"skipped: at or below native/wave water surface"); return; }
        namespace weather=wxl::offsets::game::weather;
        const auto* weatherObject=*reinterpret_cast<const uint8_t* const*>(weather::kWorldWeather);
        g_weatherRaw=weatherObject ? *reinterpret_cast<const float*>(weatherObject+weather::kIntensity) : 0.f;
        g_weatherAvailable=weatherObject && std::isfinite(g_weatherRaw);
        g_weatherKind=0;
        if (weatherObject) {
            if (*reinterpret_cast<const uint32_t*>(weatherObject+weather::kRainCount)) g_weatherKind=1;
            else if (*reinterpret_cast<const uint32_t*>(weatherObject+weather::kSnowCount)) g_weatherKind=2;
            else if (*reinterpret_cast<const uint32_t*>(weatherObject+weather::kSandCount)) g_weatherKind=3;
        }
        const float nativeWeatherInput=fog::NativeWeatherInput(g_weatherRaw,g_weatherKind,g_weatherAvailable);
        const float weatherInput=g_weatherPreview ? 1.f : nativeWeatherInput;
        float sourceDirection[3]{};
        wxl::water::celestial::ReadDirection(sourceDirection,&g_celestialBody);
        if (vfog::Draw(static_cast<IDirect3DDevice9*>(pass->device),depth,enclosed,
            fog::StormAmount(weatherInput),sourceDirection,g_celestialBody==wxl::water::celestial::Body::Moon,g_shafts ? g_shaftStrength : 0.f,early,transparent))
        {
            if(early) { g_earlyFogFrame=pass->frameId; g_earlyFogGeneration=pass->deviceGeneration; }
            Report(6,early ? "active: authored fog before native screen effects" : "active: authored fog late fallback"); return;
        }
        // Imported RenderPasses returns false before its final target composite;
        // do not draw legacy haze here. The late callback can safely retry/fallback.
        if(early || !singleSample) return; // Never feed a failed MSAA resolve to legacy 1x effects.
        if(enclosed) { Report(7,"paused: enclosed interior; native fog retained"); return; }
        namespace sky=wxl::offsets::engine::sky;
        if (!*reinterpret_cast<const uint32_t*>(sky::kSkyDrawGate))
        { Report(2,"skipped: native sky disabled"); return; }
        const uint32_t argb=*reinterpret_cast<const uint32_t*>(sky::kFogColor);
        const float color[]{((argb>>16)&255)/255.0f,((argb>>8)&255)/255.0f,(argb&255)/255.0f};
        fog::Constants constants;
        if (!fog::Prepare(depth,color,constants,g_settings,g_mistBase)) { Report(3,"skipped: unsupported camera or settings"); return; }
        const float weatherLight=fog::WeatherLightGain(weatherInput);
        fog::ApplyWeatherHaze(constants,weatherInput);
        const int weatherReportKey=static_cast<int>(fog::StormAmount(nativeWeatherInput)*20.f)
            +32*g_weatherKind+128*int(g_weatherPreview)+256*int(g_weatherAvailable);
        const auto weatherNow=GetTickCount64();
        if (g_weatherReports<128 && weatherReportKey!=g_lastWeatherReport && weatherNow>=g_nextWeatherReport) {
            ++g_weatherReports; g_lastWeatherReport=weatherReportKey; g_nextWeatherReport=weatherNow+2000;
            WLOG_INFO("render-weather: native=%s available=%u raw=%.3f response=%.3f preview=%u appliedHaze=%.3f appliedLight=%.3f",
                WeatherKindName(g_weatherKind),g_weatherAvailable,g_weatherRaw,fog::StormAmount(nativeWeatherInput),
                g_weatherPreview,1.f+.5f*fog::StormAmount(weatherInput),weatherLight);
        }
        g_viewHeight=depth.cameraPosition[2]; g_canPlaceMist=true;
        wxl::water::shafts::Constants shaftConstants;
        bool preparedShafts=false;
        if (g_directional || g_shafts)
        {
            // Same primary visible disc used by water highlights.
            float discDirection[3]{};
            if(wxl::water::celestial::ReadDirection(discDirection,&g_celestialBody))
            {
                if(g_celestialBody!=g_reportedBody && g_sourceReports<32) {
                    g_reportedBody=g_celestialBody; ++g_sourceReports;
                    WLOG_INFO("render-celestial: source=%s direction=%.3f,%.3f,%.3f",
                        g_celestialBody==wxl::water::celestial::Body::Sun ? "Sun" : "Moon",
                        discDirection[0],discDirection[1],discDirection[2]);
                }
                if(g_directional) {
                    fog::PrepareLight(depth,discDirection,constants);
                    constants.row[6][3]*=weatherLight;
                }
                if(g_shafts) preparedShafts=wxl::water::shafts::Prepare(depth,discDirection,color,g_shaftStrength*weatherLight,g_waterlineGain,shaftConstants,g_shaftSpread);
            }
        }
        constants.row[3][3]*=g_waterlineGain;
        if(g_mist) {
            const auto& wind=wxl::wind::Frame();
            const double now=static_cast<double>(GetTickCount64())/1000.0;
            g_mistDrift.Update(now,wind.speed,wind.dir[0],wind.dir[1]);
            if(fog::PrepareMist(depth,now,constants,g_integratedMist)) {
                constants.row[7][3]-=g_mistDrift.X();
                constants.row[8][3]-=g_mistDrift.Y();
            }
        }
        auto* d=static_cast<IDirect3DDevice9*>(pass->device);
        if (g_generation!=pass->deviceGeneration) { Shader().Reset(); g_generation=pass->deviceGeneration; g_shaderFailed=false; }
        if (g_shaderFailed) return;
        if (!Shader() && FAILED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(kHeightFogPs),Shader().GetAddressOf())))
        { g_shaderFailed=true; Report(4,"skipped: shader unavailable"); return; }
        const HRESULT hr=HalfFog().Draw(d,depth,constants,Shader().Get());
        if (hr!=S_OK) { Report(5,"skipped: incompatible target or draw/state failure"); return; }
        if(preparedShafts)
        {
            const HRESULT shaftResult=Shafts().Draw(d,depth,shaftConstants);
            g_shaftVisible=shaftResult==S_OK;
            g_shaftOrigin[0]=shaftConstants.row[0][0];
            g_shaftOrigin[1]=shaftConstants.row[0][1];
            if(g_shaftReports<3)
            {
                WLOG_INFO("render-shafts: result=0x%08lX source=%.3f,%.3f gain=%.3f half-resolution=1",
                    static_cast<unsigned long>(shaftResult),shaftConstants.row[0][0],shaftConstants.row[0][1],shaftConstants.row[0][3]);
                ++g_shaftReports;
            }
        }
        Report(6,"active: world-anchored height layer; native zone fog retained");
        if (g_drawReports<3)
            WLOG_INFO("render-fog-light: view direction=%.3f,%.3f,%.3f gain=%.3f",
                constants.row[6][0],constants.row[6][1],constants.row[6][2],constants.row[6][3]);
        if (g_drawReports++<3)
            WLOG_INFO("render-fog: draw frame=%llu size=%ux%u cameraZ=%.3f range=%.3f..%.3f color=%06X waterlineGain=%.3f plane=%u liquid=%u nativeDepth=%.3f",
                depth.frameId,depth.width,depth.height,depth.cameraPosition[2],depth.depthMinZ,depth.depthMaxZ,argb&0xffffff,g_waterlineGain,hasPlane,nativeLiquid.type,nativeLiquid.depth);
    }
    bool Install()
    {
        if (!wxl::render::defaults::Enabled("WXL_RENDER_FOG")) return true;
        const auto* render=wxl::game::render::Api();
        g_depth=static_cast<const WXL_RenderDepthApi*>(wxl::runtime::extensions::GetInterface("wxl.render-depth",WXL_RENDER_DEPTH_API_VERSION));
        if (!render || !g_depth || g_depth->structSize<sizeof(*g_depth) ||
            g_depth->apiVersion!=WXL_RENDER_DEPTH_API_VERSION || g_depth->sceneDepthSize!=sizeof(WXL_SceneDepth) ||
            !g_depth->RequestSceneDepth || !g_depth->GetSceneDepth)
        { WLOG_WARN("render-fog: required render/depth service unavailable; inactive"); return true; }
        g_timeHook=wxl::hook::Install("Water_TimePreview",wxl::offsets::game::world::kFrameDayNightUpdate,&TimeUpdate,&g_originalTimeUpdate);
        if(!g_timeHook) WLOG_WARN("render-time-preview: native update hook unavailable; control hidden");
        LoadSettings(); g_startup=Values();
        if (!render->AddPostProcess(WXL_RENDER_ATMOSPHERE,OnPass,nullptr)) return false;
        render->AddPostProcess(WXL_RENDER_BEFORE_SCREEN_EFFECTS,OnPass,&g_earlyFogFrame);
        render->AddPostProcess(WXL_RENDER_BEFORE_TRANSPARENTS,OnPass,&g_earlyFogGeneration);
        g_depth->RequestSceneDepth();
        wxl::events::Subscribe(wxl::events::Event::OnNativeWorldBegin,[](void*,const void* args) {
            vfog::BeginNativeWorld(*static_cast<const WXL_RenderPassContext*>(args),g_enabled && wxl::water::EnhancementsEnabled());
        },nullptr);
        wxl::events::Subscribe(wxl::events::Event::OnNativeWorldEnd,[](void*,const void*) { vfog::EndNativeWorld(); },nullptr);
        wxl::events::Subscribe(wxl::events::Event::OnFrame,ClearSource,nullptr);
        wxl::events::Subscribe(wxl::events::Event::OnWorldLeave,OnWorldLeave,nullptr);
        wxl::events::Subscribe(wxl::events::Event::OnInput,OnInput,nullptr);
        wxl::events::Subscribe(wxl::events::Event::OnDeviceLost,OnLost,nullptr);
        wxl::ui::AddPanel("Coastal fog",Panel,nullptr,430,360);
        WLOG_INFO("render-fog: integrated atmosphere; F7 toggles; F9 Coastal fog controls; Natural defaults");
        return true;
    }
}
WXL_REGISTER_FEATURE("water-atmosphere",wxl::features::water,Install)
