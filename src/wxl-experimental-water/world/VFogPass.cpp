#include "VFogPass.hpp"
#include "wxl/TransparentFogApi.h"
#include "NativeForeverGlow.hpp"
#include "wxl/AuthoredGradingFrame.hpp"
#include "runtime/Extensions.hpp"
#include "VFogPreferences.hpp"
#include "VFogQuality.hpp"
#include "UpstreamEffects.hpp"
#include "GlowAmount.hpp"
#include "StockFogReplacement.hpp"
#include "LightInventory.hpp"
#include "wxl/RenderDefaults.hpp"
#include "InteriorFogBlend.hpp"
#include "renderer.h"
#include "water_data.h"
#include "log.h"
#include "common/Log.hpp"
#include "imgui.h"
#include "WaterStageCapture.hpp"
#include "offsets/game/World.hpp"
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdarg>

bool LogEnabled(LogLevel level) { return level == LogLevel::Error; }
void LogWrite(LogLevel level, const char* format, ...) {
    if (level == LogLevel::Debug) return;
    char text[2048]{};
    va_list args; va_start(args, format); vsnprintf(text, sizeof(text), format, args); va_end(args);
    WLOG_INFO("vfog: %s", text);
}
void LogSetLevel(int) {}
void LogOpen(const char*) {}

namespace wxl::water::vfog {
namespace {
const WXL_TransparentFogApi* transparentApi=nullptr;
StockFogFit transparentFit{};
uint64_t transparentFrame=0,transparentGeneration=0;
bool transparentArmed=false;
uint64_t fittedBatches=0;
void __cdecl FitBatch(WXL_M2FogArgs* args) {
 if(!transparentArmed || !args) return;
 ++fittedBatches;
 M2BatchFogArgs native{args->start,args->end,args->exponent,args->colour};
 ApplyStockFogFit(transparentFit,native);
 args->start=native.start;args->end=native.end;args->exponent=native.exponent;args->colour=native.colour;
}
}

namespace {
// Build 12340 inputs from the pinned upstream adapter. Only the separately
// guarded stock-fog range transaction below writes native state.
namespace native {
constexpr uintptr_t kDayFraction=0x00D38B04;
constexpr uintptr_t kFogColor=0x00D38BA0;
constexpr uintptr_t kSunColor=0x00D38BF8;
constexpr uintptr_t kDirectColor=0x00D38BD8;
constexpr uintptr_t kAmbientColor=0x00D38BD4;
constexpr uintptr_t kFogStart=0x00D38BA4;
constexpr uintptr_t kFogEnd=0x00D38BA8;
constexpr uintptr_t kScreenEffectSlot=0x00D38B58;
constexpr uintptr_t kZoneFogDistance=0x00D38C1C;
constexpr uintptr_t kCameraTarget=0x00CD8F68;
}
Renderer& Pass() { static auto* pass = new Renderer; return *pass; }
Config& Settings() {
    static auto* config = [] {
        auto* c = new Config;
        c->localLights=false; c->water=false; c->stockFog=0;
        return c;
    }();
    return *config;
}
look::GlowTransaction glowTransaction;
bool glowFailed=false,glowDelivered=false;
float deliveredGlow=0;
std::array<float,1> foreverGlow{0};
constexpr wxl::render::settings::Field glowFields[]{{"enabled",0,1,true}};
constexpr wchar_t glowFile[]=L"wxl-forever-glow.ini";
void RestoreGlow() { if(!glowTransaction.Restore(look::Read,look::Write)) glowFailed=true; }
wxl::grading::AuthoredFrame gradingFrame;
std::array<float,1> gradingStrength{0};
constexpr wxl::render::settings::Field gradingFields[]{{"strength",0,1}};
constexpr wchar_t gradingFile[]=L"wxl-forever-grading.ini";
int __cdecl GetGrading(uint64_t frame,uint64_t gen,WXL_AuthoredGrading* out) { return gradingFrame.Get(frame,gen,out); }
WXL_AuthoredGradingApi gradingApi{sizeof(gradingApi),WXL_AUTHORED_GRADING_API_VERSION,GetGrading};
Microsoft::WRL::ComPtr<IDirect3DTexture9> msaaTexture;
const WXL_MultisampleDepthApi* msaaApi=nullptr;
DepthCopyProbes msaaProbes;
DepthCopyMethod msaaMethod=DepthCopyMethod::None;
const char* msaaNote="Waiting for world depth";
bool enabled=true;
namespace pref=vpreferences;
const char* note="";
bool initialized=false;
auto Values() { return std::array<float,3>{enabled ? 1.f : 0.f,Settings().density,Settings().temporal}; }
void Apply(const std::array<float,3>& v) { enabled=v[0]!=0; Settings().density=v[1]; Settings().temporal=v[2]; }
void EnsureSettings() { if(!initialized) Reload(); }
uint64_t generation=0;
bool loaded=false;
bool attempted=false;
bool active=false;
unsigned reports=0;
unsigned uploadedLights=0;
LocalLightCapture lightCapture=LocalLightCapture::Captured;
StockFogReplacement stockFog;
bool restoreFailed=false;
constexpr uintptr_t fogRanges[]{0x00D38B90,0x00D38B94,0x00D38BA4,0x00D38BA8};
bool FogLayout() noexcept {
    __try {
        const auto* base=reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
        if(reinterpret_cast<uintptr_t>(base)!=0x400000) return false;
        const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if(dos->e_magic!=IMAGE_DOS_SIGNATURE || dos->e_lfanew<=0 || dos->e_lfanew>0x1000) return false;
        const auto* pe=reinterpret_cast<const IMAGE_NT_HEADERS32*>(base+dos->e_lfanew);
        if(pe->Signature!=IMAGE_NT_SIGNATURE || pe->FileHeader.TimeDateStamp!=engine::kClientTimestamp) return false;
        constexpr uintptr_t stores[]{0x007F1730,0x007F171F,0x007F1983,0x007F1969};
        for(unsigned i=0;i<4;++i) {
            const auto* code=reinterpret_cast<const unsigned char*>(stores[i]);
            if(code[0]!=0xD9 || code[1]!=(i==1 ? 0x15 : 0x1D) ||
                *reinterpret_cast<const uint32_t*>(code+2)!=fogRanges[i]) return false;
        }
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void RestoreStockFog() noexcept {
    if(!stockFog.Pushed()) return;
    __try {
        for(unsigned i=0;i<4;++i) *reinterpret_cast<float*>(fogRanges[i])=stockFog.Saved()[i];
        stockFog.Restored();
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        if(!restoreFailed) WLOG_WARN("vfog: native fog restore failed; replacement disabled, snapshot retained for retry");
        restoreFailed=true; stockFog.Invalidate();
    }
}
void PushStockFog(bool eligible) noexcept {
    __try {
        std::array<float,4> ranges{};
        for(unsigned i=0;i<4;++i) ranges[i]=*reinterpret_cast<const float*>(fogRanges[i]);
        if(!stockFog.Begin(eligible && *reinterpret_cast<const uint32_t*>(0x00CD8794)==0,ranges)) return;
        for(unsigned i=0;i<4;++i) *reinterpret_cast<float*>(fogRanges[i])=i%2 ? 100000.f : 50000.f;
    } __except(EXCEPTION_EXECUTE_HANDLER) { RestoreStockFog(); stockFog.Invalidate(); }
}

bool LoadData() {
    if (attempted) return loaded;
    attempted=true;
    const auto* exe=reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
    const auto* pe=reinterpret_cast<const IMAGE_NT_HEADERS32*>(exe+dos->e_lfanew);
    if(pe->FileHeader.TimeDateStamp!=engine::kClientTimestamp) {
        WLOG_INFO("vfog: unsupported client image; existing atmosphere retained"); return false;
    }
    HMODULE module=nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&LoadData), &module)) return false;
    auto load = [&](int id, auto&& reader) {
        const auto resource=FindResourceA(module,MAKEINTRESOURCEA(id),MAKEINTRESOURCEA(10));
        if (!resource) return false;
        const auto handle=LoadResource(module,resource);
        return handle && reader(static_cast<const uint8_t*>(LockResource(handle)),SizeofResource(module,resource));
    };
    loaded=load(9101,[](const uint8_t* bytes,size_t size) { return GlobalFogData().LoadFromMemory(bytes,size); });
    const bool water=load(9102,[](const uint8_t* bytes,size_t size) { return GlobalWaterData().LoadFromMemory(bytes,size); });
    WLOG_INFO("vfog: pinned data fog=%u Forever-water=%u; embedded renderer data ready",loaded,water);
    return loaded;
}

bool NativeInputs(FrameInputs& in) noexcept {
    __try {
        in.mapId=*reinterpret_cast<const int*>(wxl::offsets::game::world::kCurrentMapId);
        in.dayFraction=*reinterpret_cast<const float*>(native::kDayFraction);
        in.fogColor=*reinterpret_cast<const uint32_t*>(native::kFogColor);
        in.sunColor=*reinterpret_cast<const uint32_t*>(native::kSunColor);
        in.directColor=*reinterpret_cast<const uint32_t*>(native::kDirectColor);
        in.ambientColor=*reinterpret_cast<const uint32_t*>(native::kAmbientColor);
        in.fogStart=*reinterpret_cast<const float*>(native::kFogStart);
        in.fogEnd=*reinterpret_cast<const float*>(native::kFogEnd);
        if(stockFog.Pushed()) { in.fogStart=stockFog.Saved()[2]; in.fogEnd=stockFog.Saved()[3]; }
        const int slot=*reinterpret_cast<const int*>(native::kScreenEffectSlot);
        if(slot>=0 && slot<FogData::kLightParamsSlots) in.lightParams.screenEffectSlot=slot;
        in.zoneFogDistance=*reinterpret_cast<const float*>(native::kZoneFogDistance);
        std::memcpy(in.camTarget,reinterpret_cast<const void*>(native::kCameraTarget),sizeof(in.camTarget));
        return in.mapId>=0 && std::isfinite(in.dayFraction) && in.dayFraction>=0 && in.dayFraction<=1 &&
            std::isfinite(in.fogStart) && std::isfinite(in.fogEnd) && in.fogEnd>0;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
float NativeGlow(bool early) noexcept {
    if(!early) return 0;
    __try {
        const auto effects=*reinterpret_cast<const uintptr_t*>(0x00D45774);
        const auto current=*reinterpret_cast<const uintptr_t*>(0x00D45780);
        const auto glow=*reinterpret_cast<const uintptr_t*>(0x00B74364);
        if(!effects || !current || current!=glow) return 0;
        const auto enabled=*reinterpret_cast<const uintptr_t*>(current+4);
        return GlowAmount(true,*reinterpret_cast<const int32_t*>(effects+0x30)!=0,true,
            enabled && *reinterpret_cast<const int32_t*>(enabled+0x30)!=0,
            *reinterpret_cast<const uint32_t*>(0x00CD8794)!=0,*reinterpret_cast<const float*>(0x00D38C2C));
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
float NativeInteriorBlend(bool enclosed) noexcept {
    if(!enclosed) return 0;
    // Pinned coa-vfog engine_lights.cpp identifies this build-12340 store and
    // its destination. Read only; WXL's existing WMO hook still classifies rooms.
    __try {
        constexpr unsigned char store[]{0xD9,0x15,0x9C,0x8B,0xD3,0x00};
        const bool supported=std::memcmp(reinterpret_cast<const void*>(0x007F1931),store,sizeof(store))==0;
        const float weight=supported ? *reinterpret_cast<const float*>(0x00D38B9C) : 1.f;
        return InteriorFogBlend(enclosed,supported,weight);
    } __except(EXCEPTION_EXECUTE_HANDLER) { return InteriorFogBlend(enclosed,false,0); }
}
}
bool Enabled() { EnsureSettings(); return enabled; }
void Reload() {
    wxl::runtime::extensions::PublishInterface("wxl.authored-grading",WXL_AUTHORED_GRADING_API_VERSION,&gradingApi);
    pref::settings::Load(gradingFile,gradingFields,gradingStrength);
    pref::settings::Load(glowFile,glowFields,foreverGlow);
    msaaApi=static_cast<const WXL_MultisampleDepthApi*>(wxl::runtime::extensions::GetInterface("wxl.multisample-depth",WXL_MULTISAMPLE_DEPTH_API_VERSION));
    if(msaaApi && msaaApi->structSize==sizeof(*msaaApi) && msaaApi->Request && msaaApi->GetDepth) msaaApi->Request();
    else msaaApi=nullptr;
    vquality::Load();
    upstream::Load();
    initialized=true;
    auto values=Values();
    const auto result=pref::settings::Load(pref::fogFile,pref::fogFields,values);
    if(result==pref::settings::Result::Ok) Apply(values);
    note=pref::settings::Message(result); Reset();
}
void BeginNativeWorld(const WXL_RenderPassContext& ctx,bool masterEnabled) {
    transparentArmed=false;transparentFrame=0;fittedBatches=0;
    RestoreGlow(); glowDelivered=false;
    gradingFrame.Reset();
    RestoreStockFog();
    EnsureSettings();
    if(generation!=ctx.deviceGeneration) { Reset(); generation=ctx.deviceGeneration; }
    static const bool supported=FogLayout();
    if(!supported || !ctx.device || !LoadData() || restoreFailed) { stockFog.Invalidate(); return; }
    PushStockFog(masterEnabled && enabled && Settings().density>0 &&
        wxl::render::defaults::Enabled("WXL_RENDER_REPLACE_FOG"));
}
void EndNativeWorld() {
 static unsigned fitReports=0;
 if(transparentArmed && fittedBatches && fitReports++<4)WLOG_INFO("transparent-fog: fitted batches=%llu",fittedBatches);
 transparentArmed=false; RestoreGlow(); RestoreStockFog(); }
bool ReplacingStockFog() { return stockFog.Replaced(); }
void Reset() { transparentArmed=false;transparentFrame=0; msaaProbes=DepthCopyProbes{}; msaaMethod=DepthCopyMethod::None; msaaNote="Waiting for world depth"; msaaTexture.Reset(); RestoreGlow(); glowDelivered=false; gradingFrame.Reset(); lights::Reset(); RestoreStockFog(); stockFog.Invalidate(); Pass().ReleaseAll(); generation=0; active=false; }
void Panel() {
    EnsureSettings();
    ImGui::PushID("authored-fog");
    if (ImGui::Checkbox("Authored volumetric fog",&enabled)) Reset();
    auto& cfg=Settings();
    ImGui::SliderFloat("Fog density",&cfg.density,0,2,"%.2fx");
    upstream::Ensure();
    bool noise=upstream::values[0]!=0,local=upstream::values[1]!=0;
    if(ImGui::Checkbox("Drifting storm fog",&noise)) { upstream::values[0]=noise ? 1.f : 0.f; Reset(); }
    if(ImGui::Checkbox("Light glow in fog",&local)) { upstream::values[1]=local ? 1.f : 0.f; Reset(); }
    bool glow=foreverGlow[0]!=0;
    if(ImGui::Checkbox("Forever glow",&glow)) { foreverGlow[0]=glow ? 1.f : 0.f; Reset(); }
    if(glow) ImGui::Text("Forever glow: %s",glowFailed ? "Unavailable after restore failure" : glowDelivered ? "Active" : "Waiting for eligible native glow");
    ImGui::SliderFloat("Forever colour grading",&gradingStrength[0],0,1,"%.2f");
    ImGui::TextWrapped("Optional zone/time colour curve. 0 keeps your selected LUT; above 0 replaces that LUT while retaining finishing controls. Requires Grading enabled.");
    if(ImGui::CollapsingHeader("Advanced smoothing")) ImGui::SliderFloat("Temporal smoothing",&cfg.temporal,0,.95f,"%.2f");
    ImGui::TextWrapped("Classic zone fog with upstream distance fog. Native fog is retained until a replacement frame succeeds. Save keeps these controls for next time; Reload or F12 restores saved values.");
    if(ImGui::Button("Save")) {
        auto result=pref::settings::Save(pref::fogFile,pref::fogFields,Values());
        if(result==pref::settings::Result::Ok) result=upstream::Save();
        if(result==pref::settings::Result::Ok) result=pref::settings::Save(gradingFile,gradingFields,gradingStrength);
        if(result==pref::settings::Result::Ok) result=pref::settings::Save(glowFile,glowFields,foreverGlow);
        note=pref::settings::Message(result);
    }
    ImGui::SameLine(); if(ImGui::Button("Reload")) Reload();
    if(ImGui::Button("Restore defaults")) {
        foreverGlow[0]=0; gradingStrength[0]=0; Apply(pref::fogDefaults); upstream::values[0]=upstream::defaults[0]; upstream::values[1]=upstream::defaults[1];
        Reset(); note="Defaults restored. Save to keep them.";
    }
    ImGui::TextWrapped("%s",note);
    ImGui::PopID();
    ImGui::Text("Volumetric pass: %s",active ? "Active" : "Waiting / fallback");
    ImGui::Text("Distance fog: %s",stockFog.Replaced() ? "Upstream replacement" : "Native / warming up");
    if(upstream::values[1]!=0) ImGui::Text("Fog lights: %u (%s)",uploadedLights,engine::LocalLightCaptureName(lightCapture));
    ImGui::TextWrapped("Depth resolve: %s",msaaNote);
    lights::Panel();
}
Config FogSettings() {
    EnsureSettings(); upstream::Ensure(); auto cfg=Settings();
    cfg.stockFog=stockFog.Replaced() ? 1 : 0;
    cfg.classicNoise=upstream::values[0]!=0; cfg.localLights=upstream::values[1]!=0;
    return cfg;
}
bool MultisampleDepth(IDirect3DDevice9* device,WXL_SceneDepth& depth) {
    EnsureSettings();
    WXL_MultisampleDepth input{sizeof(input)};
    if(!device || !msaaApi || !msaaApi->GetDepth(&input)) {
        msaaNote="No eligible depth (surface details in wxl-core.log)"; return false;
    }
    if(generation!=input.frame.deviceGeneration) { Reset(); generation=input.frame.deviceGeneration; }
    Microsoft::WRL::ComPtr<IDirect3D9> api; D3DDEVICE_CREATION_PARAMETERS params{};
    if(FAILED(device->GetDirect3D(&api)) || FAILED(device->GetCreationParameters(&params))) return false;
    const auto probe=msaaProbes.Probe(api.Get(),params.AdapterOrdinal,params.DeviceType);
    msaaMethod=probe.method;
    if(msaaMethod==DepthCopyMethod::None) { msaaNote=probe.unavailable; return false; }
    D3DSURFACE_DESC desc{};
    if(msaaTexture && (FAILED(msaaTexture->GetLevelDesc(0,&desc)) || desc.Width!=input.frame.width || desc.Height!=input.frame.height)) msaaTexture.Reset();
    if(!msaaTexture && FAILED(device->CreateTexture(input.frame.width,input.frame.height,1,D3DUSAGE_DEPTHSTENCIL,
        static_cast<D3DFORMAT>(MAKEFOURCC('I','N','T','Z')),D3DPOOL_DEFAULT,&msaaTexture,nullptr))) { msaaNote="Resolve texture unavailable"; return false; }
    depth=input.frame; depth.texture=msaaTexture.Get(); return true;
}
bool MultisampleCopy(IDirect3DDevice9* device,WXL_SceneDepth& depth,DepthCopy& copy,IDirect3DSurface9*& surface) {
    if(!MultisampleDepth(device,depth)) return false;
    WXL_MultisampleDepth input{sizeof(input)};
    if(!msaaApi->GetDepth(&input) || input.frame.frameId!=depth.frameId || input.frame.deviceGeneration!=depth.deviceGeneration) return false;
    surface=static_cast<IDirect3DSurface9*>(input.surface);
    return copy.Attach(msaaMethod,surface,msaaTexture.Get());
}
bool FrameData(IDirect3DDevice9* device,const WXL_SceneDepth& depth,FrameInputs& in) {
    in={};
    if(!LoadData()) return false;
    if (!NativeInputs(in)) return false;
    std::copy_n(depth.view,16,in.cameraRelativeView);
    in.cameraRelativeView[12]=in.cameraRelativeView[13]=in.cameraRelativeView[14]=0;
    std::copy_n(depth.projection,16,in.glProjection);
    std::copy_n(depth.cameraPosition,3,in.camPos);
    if(FAILED(device->GetViewport(&in.viewport)) || in.viewport.Width!=depth.width || in.viewport.Height!=depth.height) return false;
    in.viewport.MinZ=depth.depthMinZ; in.viewport.MaxZ=depth.depthMaxZ;
    in.farClip=depth.projection[14]/(1-depth.projection[10]);
    if (!std::isfinite(in.farClip) || in.farClip<10 || in.farClip>20000) return false;
    return true;
}
bool TransparentReady() {
 if(!transparentApi) {
  auto* service=static_cast<const WXL_TransparentFogApi*>(wxl::runtime::extensions::GetInterface("wxl.transparent-fog",1));
  if(service && service->structSize==sizeof(*service) && service->apiVersion==1 && service->SetObserver && service->DrawGlare && service->SetObserver(FitBatch))transparentApi=service;
 }
 const auto cfg=FogSettings();
 return transparentApi && stockFog.Replaced() && !cfg.debugView && !cfg.sunMarker;
}
bool FinishTransparent(IDirect3DDevice9* device,const WXL_SceneDepth& depth) {
 if(!transparentFrame || depth.frameId!=transparentFrame || depth.deviceGeneration!=transparentGeneration)return false;
 // Reacquire all borrowed inputs at the post-world boundary; no surface survives callbacks.
 IDirect3DSurface9* bound=nullptr;device->GetDepthStencilSurface(&bound);
 DepthCopy resolve;const bool msaa=depth.texture==msaaTexture.Get();
 WXL_MultisampleDepth input{sizeof(input)};
 IDirect3DSurface9* source=bound;
 bool current=!msaa || (msaaApi && msaaApi->GetDepth(&input) && input.frame.frameId==depth.frameId && input.frame.deviceGeneration==depth.deviceGeneration);
 if(msaa && current)source=static_cast<IDirect3DSurface9*>(input.surface);
 if(current && (!msaa || resolve.Attach(msaaMethod,source,msaaTexture.Get())))
  Pass().RenderGodRaysAfterWorld(device,SceneDepth{static_cast<IDirect3DTexture9*>(depth.texture),source,msaa ? &resolve : nullptr});
 resolve.Detach();if(bound)bound->Release();return true;
}
bool Draw(IDirect3DDevice9* device,const WXL_SceneDepth& depth,bool indoors,float storm,const float direction[3],bool moon,float shafts,bool beforeGlow, bool beforeTransparents) {
    active=false;
    EnsureSettings();
    if (!enabled || !device || !depth.texture || !LoadData()) return false;
    if(beforeTransparents && !TransparentReady())return false;
    if (generation!=depth.deviceGeneration) { Reset(); generation=depth.deviceGeneration; }
    FrameInputs in{};
    if(!FrameData(device,depth,in)) return false;
    if(beforeGlow) lights::Frame(in.camPos,in.mapId);
    in.clientGlowAmount=NativeGlow(beforeGlow);
    std::copy_n(direction,3,in.toLight); in.lightIsMoon=moon;
    in.lightParams.stormBlend=std::clamp(storm,0.f,1.f);
    AuthoredFog authored{};
    const bool authoredLook=(gradingStrength[0]>0 || (beforeGlow && foreverGlow[0]!=0)) && GlobalFogData().Resolve(in.mapId,in.camPos,in.dayFraction,in.lightParams,authored);
    if(beforeGlow && foreverGlow[0]!=0 && !glowFailed && authoredLook && authored.hasGlow) {
        static const bool layout=look::Layout();
        uintptr_t passes[2]{};
        if(layout && look::Passes(passes)) {
            glowDelivered=glowTransaction.Begin(passes,authored.glow,look::Weight(authored.coverage),look::Read,look::Write,deliveredGlow);
            if(glowDelivered) in.clientGlowAmount=deliveredGlow;
        }
    }
    const auto captureCfg=FogSettings();
    if(captureCfg.localLights) engine::CaptureLocalLightInputs(in.camPos,true,
        PointLightUpload{captureCfg.colorSpace!=0,captureCfg.localLightIntensity},in.localLights);
    uploadedLights=in.localLights.pointLightCount; lightCapture=in.localLights.capture;
    in.localLights.cameraInterior=indoors; in.localLights.interiorBlend=NativeInteriorBlend(indoors);
    IDirect3DSurface9* bound=nullptr;
    device->GetDepthStencilSurface(&bound);
    Settings().godRays=std::clamp(shafts,0.f,1.f);
    Settings().quality=vquality::Level();
    const auto cfg=FogSettings();
    DepthCopy resolve;
    const bool multisampled=depth.texture==msaaTexture.Get();
    WXL_MultisampleDepth input{sizeof(input)};
    IDirect3DSurface9* sampleSurface=bound;
    const bool current=!multisampled || (msaaApi && msaaApi->GetDepth(&input) &&
        input.frame.frameId==depth.frameId && input.frame.deviceGeneration==depth.deviceGeneration);
    if(multisampled && current) sampleSurface=static_cast<IDirect3DSurface9*>(input.surface);
    bool attached=current && (!multisampled || resolve.Attach(msaaMethod,sampleSurface,msaaTexture.Get()));
    if(stagecapture::frame==depth.frameId && stagecapture::frame && !(stagecapture::stages&8))
        WLOG_INFO("water-stage-image: before-fog hr=0x%08lX",stagecapture::Capture(device,depth,3,L"04-before-fog"));
    if(beforeTransparents && (!attached || !transparentApi->DrawGlare())) {
        resolve.Detach(); RestoreGlow(); glowDelivered=false;
        if(bound)bound->Release();return false;
    }
    active=attached && Pass().Render(device,SceneDepth{static_cast<IDirect3DTexture9*>(depth.texture),sampleSurface,multisampled ? &resolve : nullptr},in,cfg,beforeTransparents ? FogPass::BeforeTransparents : FogPass::WholeFrame);
    if(stagecapture::frame==depth.frameId && stagecapture::frame && !(stagecapture::stages&16))
        WLOG_INFO("water-stage-image: after-fog hr=0x%08lX active=%u",stagecapture::Capture(device,depth,4,L"05-after-fog"),active);
    if(multisampled) msaaNote=active ? DepthCopyMethodName(msaaMethod) : attached ? Pass().LastSkipReason() : "Depth registration failed";
    else msaaNote="Single-sample (MSAA resolve not needed)";
    resolve.Detach(); // No native surface reference or registration survives this callback.
    if(!active && beforeGlow) { RestoreGlow(); glowDelivered=false; }
    if(active) {
        if(beforeTransparents) {
            transparentFit=Pass().LastStockFogFit();transparentFrame=depth.frameId;
            transparentGeneration=depth.deviceGeneration;transparentArmed=true;
            static unsigned transparentReports=0;
            if(transparentReports++<4)WLOG_INFO("transparent-fog: active frame=%llu fit=%u start=%.3f end=%.3f",transparentFrame,transparentFit.fogs,transparentFit.start,transparentFit.end);
        }
        stockFog.Success(beforeGlow);
        if(gradingStrength[0]>0 && authoredLook && authored.hasGradingCurve && !look::Ghost()) {
            WXL_AuthoredGrading grade{sizeof(grade),gradingStrength[0]*look::Weight(authored.coverage)};
            std::copy_n(authored.gradingCurve,32,grade.curve);
            gradingFrame.Set(depth.frameId,depth.deviceGeneration,grade);
        }
    }
    if (bound) bound->Release();
    if (reports<8) { ++reports; WLOG_INFO("vfog: draw=%u early=%u replacement=%u glow=%.3f map=%d reason=%s",active,beforeGlow,stockFog.Replaced(),in.clientGlowAmount,in.mapId,Pass().LastSkipReason()); }
    return active;
}
}
