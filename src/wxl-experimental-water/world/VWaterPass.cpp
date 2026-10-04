#include "VWaterPass.hpp"
#include "VFogPreferences.hpp"
#include "VFogQuality.hpp"
#include "UpstreamEffects.hpp"
#include "wxl/RenderReloadKey.hpp"
#include "config.hpp"
#include "VFogPass.hpp"
#include "NativeCelestialSource.hpp"
#include "NativeViewerLiquid.hpp"
#include "water_renderer.h"
#include "water_classify.h"
#include "WaterStageCapture.hpp"
#include "wxl-experimental-water/render/Surface.hpp"
#include "engine/events/Event.hpp"
#include "engine/hook/Registry.hpp"
#include "runtime/Extensions.hpp"
#include "game/Gx.hpp"
#include "offsets/engine/Liquid.hpp"
#include "offsets/game/Adt.hpp"
#include "common/Log.hpp"
#include "imgui.h"
#include "wxl/InteriorApi.h"
#include <cstring>
namespace wxl::water::forever {
namespace {
WaterRenderer& Pass() { static auto* p=new WaterRenderer; return *p; }
const WXL_LiquidDepthApi* depthApi=nullptr;
bool enabled=true,active=false;
WaterClass selected=WaterClass::None;
DWORD savedWrite=0;
constexpr D3DRENDERSTATETYPE stencilStates[]{D3DRS_STENCILENABLE,D3DRS_STENCILFUNC,D3DRS_STENCILPASS,
    D3DRS_STENCILFAIL,D3DRS_STENCILZFAIL,D3DRS_STENCILREF,D3DRS_STENCILMASK,D3DRS_STENCILWRITEMASK,D3DRS_TWOSIDEDSTENCILMODE};
constexpr DWORD stencilValues[]{TRUE,D3DCMP_ALWAYS,D3DSTENCILOP_REPLACE,D3DSTENCILOP_KEEP,D3DSTENCILOP_KEEP,0,255,255,FALSE};
DWORD savedStencil[9]{};
unsigned reports=0;
uint64_t usedFrame=0, observedFrame=0;
bool enclosed=false;
bool contactCapture=false;
const WXL_InteriorApi* interior=nullptr;
Config controls;
namespace pref=vpreferences;
const char* note="";
auto Values() { return std::array<float,5>{enabled ? 1.f : 0.f,controls.waterClarity,controls.waterWaves,controls.waterFoam,controls.waterReflections}; }
void Apply(const std::array<float,5>& v) {
    enabled=v[0]!=0; controls.waterClarity=v[1]; controls.waterWaves=v[2]; controls.waterFoam=v[3]; controls.waterReflections=v[4];
}
WaterClass Classify(const void* settings) noexcept {
    __try {
        namespace liquid=wxl::offsets::engine::liquid;
        namespace adt=wxl::offsets::game::adt;
        const auto count=*reinterpret_cast<const uint32_t*>(liquid::kSettingsBankCount);
        const auto rows=*reinterpret_cast<const void* const* const*>(liquid::kSettingsBankRows);
        if(!settings || !rows || count>4096) return WaterClass::None;
        uint32_t id=0; for(;id<count && rows[id]!=settings;++id) {}
        if(id==count) return WaterClass::None;
        const auto min=*reinterpret_cast<const uint32_t*>(adt::kLiquidTypeDbMinId);
        const auto max=*reinterpret_cast<const uint32_t*>(adt::kLiquidTypeDbMaxId);
        const auto db=*reinterpret_cast<const uint8_t* const* const*>(adt::kLiquidTypeDbRows);
        if(!db || id<min || id>max || !db[id-min]) return WaterClass::None;
        char texture[128]{}; std::memcpy(texture,settings,127);
        return ClassifyLiquid(id,*reinterpret_cast<const uint32_t*>(db[id-min]+0x0c),
            *reinterpret_cast<const uint32_t*>(db[id-min]+0x38),texture);
    } __except(EXCEPTION_EXECUTE_HANDLER) { return WaterClass::None; }
}
bool Colours(WaterInputs& water) noexcept {
    __try {
        constexpr uintptr_t sky=0x00D38BE0,river=0x00D38C14,ocean=0x00D38C0C;
        std::memcpy(water.skyColors,reinterpret_cast<const void*>(sky),sizeof(water.skyColors));
        std::memcpy(water.riverColors,reinterpret_cast<const void*>(river),sizeof(water.riverColors));
        std::memcpy(water.oceanColors,reinterpret_cast<const void*>(ocean),sizeof(water.oceanColors));
        water.stockFogApplies=!vfog::ReplacingStockFog(); return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void Reset(void*,const void*) {
    Pass().Abort(static_cast<IDirect3DDevice9*>(wxl::game::gx::RawDevice()));
    Pass().ReleaseAll(); selected=WaterClass::None; active=false; usedFrame=0; observedFrame=0;
}
void Reload() {
    vquality::Load();
    upstream::Load();
    auto values=Values();
    const auto result=pref::settings::Load(pref::waterFile,pref::waterFields,values);
    if(result==pref::settings::Result::Ok) Apply(values);
    note=pref::settings::Message(result); Reset(nullptr,nullptr);
}
void OnInput(void*,const void* args) {
    const auto* a=static_cast<const wxl::events::InputArgs*>(args);
    static wxl::render::ReloadKey reload;
    if(reload.Handle(a->message,a->wparam,a->handled)) Reload();
}
void ProbeDepth(IDirect3DDevice9* d,const WXL_SceneDepth& depth,DepthCopy& resolve,bool single,const char* stage) {
    if(!stagecapture::Select(depth)) return;
    constexpr unsigned columns=64,rows=36,count=columns*rows;
    DepthTexel texels[count]{}; float values[count]{};
    for(unsigned y=0;y<rows;++y) for(unsigned x=0;x<columns;++x)
        texels[y*columns+x]={depth.width*(2*x+1)/(2*columns),depth.height*(2*y+1)/(2*rows)};
    const bool ok=(single || resolve.Copy(d)) && ReadDepthTexels(d,static_cast<IDirect3DTexture9*>(depth.texture),texels,count,values);
    wchar_t path[1200]{}; swprintf_s(path,L"%s/%S-depth.csv",stagecapture::folder,stage);
    FILE* file=nullptr;
    if(ok && _wfopen_s(&file,path,L"wx")==0 && file) {
        fprintf(file,"x,y,depth\n");
        for(unsigned i=0;i<count;++i) fprintf(file,"%u,%u,%.9g\n",texels[i].x,texels[i].y,values[i]);
        fclose(file);
    }
    WLOG_INFO("water-stage-depth: %s frame=%llu ok=%u grid=%ux%u range=%.9g/%.9g",stage,depth.frameId,ok,columns,rows,depth.depthMinZ,depth.depthMaxZ);
}
void Begin(void*,const void*) {
    if(!enabled || !EnhancementsEnabled() || ReadViewerLiquid().Submerged() || !depthApi) { active=false; return; }
    auto* d=static_cast<IDirect3DDevice9*>(wxl::game::gx::RawDevice());
    WXL_SceneDepth depth{}; depth.structSize=sizeof(depth);
    FrameInputs in{}; WaterInputs water{};
    DepthCopy resolve; IDirect3DSurface9* sampleSurface=nullptr;
    const bool single=d && depthApi->GetDepth(&depth);
    if(!d || (!single && !vfog::MultisampleCopy(d,depth,resolve,sampleSurface)) || !vfog::FrameData(d,depth,in) || !Colours(water)) { active=false; return; }
    if(observedFrame!=depth.frameId) { active=false; observedFrame=depth.frameId; }
    celestial::Body body{}; celestial::ReadDirection(in.toLight,&body); in.lightIsMoon=body==celestial::Body::Moon;
    enclosed=WxlViewerEnclosed(interior);
    in.localLights.cameraInterior=enclosed; in.localLights.interiorBlend=enclosed ? 1.f : 0.f;
    Config cfg=vfog::FogSettings(); cfg.water=true; cfg.waterQuality=vquality::Level();
    cfg.waterWaves=controls.waterWaves; cfg.waterFoam=controls.waterFoam; cfg.waterClarity=controls.waterClarity;
    cfg.waterReflections=controls.waterReflections;
    IDirect3DSurface9* surface=nullptr; d->GetDepthStencilSurface(&surface);
    upstream::Ensure();
    cfg.waterRipples=upstream::values[2];
    contactCapture=cfg.waterRipples>0 && engine::CaptureWaterContacts(in.camTarget,water.contacts);
    if(!contactCapture) cfg.waterRipples=0;
    cfg.waterClientSplashes=true;
    if(stagecapture::Select(depth) && !(stagecapture::stages&1)) {
        WLOG_INFO("water-stage-image: before-water hr=0x%08lX",stagecapture::Capture(d,depth,0,L"01-before-water"));
        ProbeDepth(d,depth,resolve,single,"before-water");
    }
    const bool armed=Pass().Begin(d,SceneDepth{static_cast<IDirect3DTexture9*>(depth.texture),single ? surface : sampleSurface,single ? nullptr : &resolve},in,water,cfg);
    if(surface) surface->Release();
    if(!armed && reports++<8) WLOG_INFO("Forever water begin: %s",Pass().LastSkipReason());
}
void End(void*,const void*) {
    if(!Pass().Armed()) return;
    auto* d=static_cast<IDirect3DDevice9*>(wxl::game::gx::RawDevice());
    WXL_SceneDepth depth{}; depth.structSize=sizeof(depth);
    DepthCopy resolve; IDirect3DSurface9* sampleSurface=nullptr;
    const bool single=d && depthApi->GetDepth(&depth);
    if(!d || (!single && !vfog::MultisampleCopy(d,depth,resolve,sampleSurface))) { Pass().Abort(d); return; }
    IDirect3DSurface9* surface=nullptr; d->GetDepthStencilSurface(&surface);
    if(stagecapture::Select(depth) && !(stagecapture::stages&2)) {
        WLOG_INFO("water-stage-image: native-water hr=0x%08lX",stagecapture::Capture(d,depth,1,L"02-native-water"));
        ProbeDepth(d,depth,resolve,single,"native-water");
    }
    const bool shaded=Pass().End(d,SceneDepth{static_cast<IDirect3DTexture9*>(depth.texture),single ? surface : sampleSurface,single ? nullptr : &resolve});
    if(surface) surface->Release();
    if(stagecapture::Select(depth) && !(stagecapture::stages&4))
        WLOG_INFO("water-stage-image: enhanced-water hr=0x%08lX shaded=%u",stagecapture::Capture(d,depth,2,L"03-enhanced-water"),shaded);
    active=active || shaded;
    if(shaded) usedFrame=depth.frameId;
    if(reports++<8) WLOG_INFO("Forever water: shaded=%u classes=%u waves=%u reason=%s",shaded,Pass().ShadedClasses(),Pass().WavesSimulated(),Pass().LastSkipReason());
}
bool Install() {
    depthApi=static_cast<const WXL_LiquidDepthApi*>(wxl::runtime::extensions::GetInterface("wxl.liquid-depth",WXL_LIQUID_DEPTH_API_VERSION));
    if(!depthApi) return false;
    interior=static_cast<const WXL_InteriorApi*>(wxl::runtime::extensions::GetInterface("wxl.interior",WXL_INTERIOR_API_VERSION));
    Reload();
    depthApi->RequestCopy();
    using namespace wxl::events;
    Subscribe(Event::OnInput,OnInput,nullptr);
    Subscribe(Event::OnLiquidRenderBegin,Begin,nullptr); Subscribe(Event::OnLiquidRenderEnd,End,nullptr);
    Subscribe(Event::OnDeviceLost,Reset,nullptr); Subscribe(Event::OnWorldLeave,Reset,nullptr);
    return true;
}
}
bool UsedInFrame(uint64_t frame) { return enabled && frame!=0 && usedFrame==frame; }
bool Armed() { return Pass().Armed(); }
void Select(const void* settings) {
    selected=Classify(settings);
    if(enclosed && selected!=WaterClass::None) selected=WaterClass::Interior;
}
void Deselect() { selected=WaterClass::None; }
void BeforeDraw(IDirect3DDevice9* d) {
    if(stagecapture::frame==observedFrame && stagecapture::frame && stagecapture::draws++<8) {
        stagecapture::DrawState(d,stagecapture::draws);
        DWORD z=0,write=0,mask=0,alpha=0,stencil=0;
        d->GetRenderState(D3DRS_ZENABLE,&z); d->GetRenderState(D3DRS_ZWRITEENABLE,&write);
        d->GetRenderState(D3DRS_MULTISAMPLEMASK,&mask); d->GetRenderState(D3DRS_ALPHATESTENABLE,&alpha);
        d->GetRenderState(D3DRS_STENCILENABLE,&stencil);
        WLOG_INFO("water-stage-draw: class=%u z=%lu native-zwrite=%lu mask=0x%lX alpha-test=%lu stencil=%lu",static_cast<unsigned>(selected),z,write,mask,alpha,stencil);
    }
    d->GetRenderState(D3DRS_ZWRITEENABLE,&savedWrite);
    if(selected!=WaterClass::None) d->SetRenderState(D3DRS_ZWRITEENABLE,TRUE);
    for(int i=0;i<9;++i) { d->GetRenderState(stencilStates[i],&savedStencil[i]); d->SetRenderState(stencilStates[i],stencilValues[i]); }
    Pass().Tag(d,selected);
}
void AfterDraw(IDirect3DDevice9* d) {
    Pass().Untag(d); d->SetRenderState(D3DRS_ZWRITEENABLE,savedWrite);
    for(int i=0;i<9;++i) d->SetRenderState(stencilStates[i],savedStencil[i]);
}
bool Panel() {
    ImGui::PushID("forever-water");
    vquality::Ensure();
    if(ImGui::Checkbox("Performance mode (fog and water)",&vquality::performance)) { Reset(nullptr,nullptr); vfog::Reset(); }
    ImGui::TextWrapped("Off keeps the accepted quality. On reduces fog resolution and wave detail to lower GPU cost.");
    if(ImGui::Button("Save performance preference")) vquality::Save();
    ImGui::TextWrapped("%s",vquality::note);
    if(ImGui::Checkbox("Forever water",&enabled)) Reset(nullptr,nullptr);
    ImGui::TextWrapped("Forever liquid presets, FFT waves, foam and reflections. F11 compares with native water. Save keeps these controls for next time; Reload or F12 restores saved values.");
    upstream::Ensure();
    ImGui::SliderFloat("Swimming ripples",&upstream::values[2],0,2,"%.2fx");
    ImGui::SliderFloat("Clarity",&controls.waterClarity,0,2,"%.2fx");
    ImGui::SliderFloat("Waves",&controls.waterWaves,0,2,"%.2fx");
    ImGui::SliderFloat("Foam",&controls.waterFoam,0,2,"%.2fx");
    ImGui::SliderFloat("Reflections",&controls.waterReflections,0,2,"%.2fx");
    if(ImGui::Button("Save")) {
        auto result=pref::settings::Save(pref::waterFile,pref::waterFields,Values());
        if(result==pref::settings::Result::Ok) result=upstream::Save();
        note=pref::settings::Message(result);
    }
    ImGui::SameLine(); if(ImGui::Button("Reload")) Reload();
    if(ImGui::Button("Restore defaults")) { Apply(pref::waterDefaults); upstream::values[2]=upstream::defaults[2]; Reset(nullptr,nullptr); note="Defaults restored. Save to keep them."; }
    ImGui::TextWrapped("%s",note);
    ImGui::PopID();
    ImGui::Text("Water pass: %s",active ? "Active" : "Waiting / fallback");
    if(upstream::values[2]>0) ImGui::Text("Ripple contacts: %u (%s)",Pass().RippleStats().contacts,
        contactCapture ? "available" : "waiting / unavailable");
    return enabled;
}
WXL_REGISTER_FEATURE("forever-water",wxl::features::water,Install)
}
