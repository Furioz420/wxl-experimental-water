#include "LightInventory.hpp"
#include <windows.h>
#include <cstring>
#include "imgui.h"
#include "common/Log.hpp"
namespace wxl::water::lights {
namespace {
int pending=0;
const char* status="Not captured";
Inventory last{};
bool Read(uint32_t address,void* out,size_t size) {
    __try { std::memcpy(out,reinterpret_cast<const void*>(address),size); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool Layout() {
    __try {
        auto* base=reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
        if(reinterpret_cast<uintptr_t>(base)!=0x400000) return false;
        auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if(dos->e_magic!=IMAGE_DOS_SIGNATURE || dos->e_lfanew<=0 || dos->e_lfanew>0x1000) return false;
        auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS32*>(base+dos->e_lfanew);
        if(nt->Signature!=IMAGE_NT_SIGNATURE || nt->FileHeader.TimeDateStamp!=0x4c2452fe || nt->OptionalHeader.Magic!=0x10b) return false;
        const unsigned char scene[]{0x8b,0x3d,0x4c,0x75,0xcd,0};
        const unsigned char buckets[]{0x83,0x7e,0x08,0x01,0x0f,0x85,0xc6,0,0,0,0x83,0x78,0x24,0};
        const unsigned char links[]{0x8d,0x0c,0xb8,0x89,0x4e,0x64,0x8b,0x09,0x8b,0x56,0x64};
        const unsigned char attenuation[]{0xd9,0x40,0x54,0xd9,0x5d,0xd0,0xd9,0x40,0x58,0xd9,0x5d,0xd4,0xd9,0x40,0x5c};
        return !memcmp(reinterpret_cast<void*>(0x4f90ec),scene,sizeof(scene)) && !memcmp(reinterpret_cast<void*>(0x834c8d),buckets,sizeof(buckets)) && !memcmp(reinterpret_cast<void*>(0x834d3c),links,sizeof(links)) && !memcmp(reinterpret_cast<void*>(0x835539),attenuation,sizeof(attenuation));
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}
void Reset() { pending=0; last={}; status="Not captured (world/device/settings reset)"; }
void Panel() {
    if(!ImGui::CollapsingHeader("Light investigation")) return;
    ImGui::TextWrapped("One snapshot per click. Visuals stay unchanged. Stand near the source, then capture. Details go to Logs/wxl-core.log.");
    if(ImGui::Button("Capture guard lights")) { pending=1; status="Waiting for an early fog frame"; }
    if(ImGui::Button("Capture lamp lights")) { pending=2; status="Waiting for an early fog frame"; }
    ImGui::TextWrapped("%s",status);
    ImGui::Text("Scene nodes: %u; enabled: %u; within 200 yd: %u",last.visited,last.enabled,last.nearby);
    ImGui::Text("Older frame stamps: %u; invalid values: %u",last.stale,last.invalid);
    ImGui::TextWrapped("Counts are before native per-object selection. A snapshot cannot identify the model owning a light.");
}
void Frame(const float* camera,int map) {
    if(!pending) return;
    const char* label=pending==1 ? "guard" : "lamp"; pending=0;
    LARGE_INTEGER begin{},end{},frequency{}; QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&begin);
    const bool supported=Layout();
    last={}; const bool valid=supported && Capture(Read,camera,last);
    QueryPerformanceCounter(&end);
    status=!supported ? "Unsupported client layout" : !valid ? "Capture rejected: unreadable or changed scene" : "Snapshot saved to wxl-core.log";
    WLOG_INFO("light-inventory: label=%s valid=%u layout=%u map=%d camera=(%.2f,%.2f,%.2f) nodes=%u enabled=%u nearby200=%u stale=%u invalid=%u ms=%.3f",label,valid,supported,map,camera[0],camera[1],camera[2],last.visited,last.enabled,last.nearby,last.stale,last.invalid,frequency.QuadPart ? 1000.0*double(end.QuadPart-begin.QuadPart)/double(frequency.QuadPart) : 0);
    for(uint32_t i=0;i<last.samples;++i) {
        const auto& s=last.nearest[i];
        WLOG_INFO("light-inventory: %s nearest=%u distance=%.2f position=(%.2f,%.2f,%.2f) color=(%.4f,%.4f,%.4f) attenuation=(%.6f,%.6f,%.6f) stamp=%u",label,i,s.distance,s.position[0],s.position[1],s.position[2],s.color[0],s.color[1],s.color[2],s.attenuation[0],s.attenuation[1],s.attenuation[2],s.stamp);
    }
}
}
