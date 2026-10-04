// WXL adaptation of coa-vfog 4e31ddf engine.cpp glow layout (GPL-3.0).
#pragma once
#include "ForeverGlowPolicy.hpp"
#include <windows.h>
#include <cstring>
namespace wxl::water::look {
struct CodeBytes { const char* name; uintptr_t address; size_t size; unsigned char bytes[32]; };
inline constexpr CodeBytes guards[] = {
    {"glow feed SetParam call", 0x004F883C, 8, {0x8B, 0x40, 0x10, 0x52, 0x6A, 0x03, 0xFF, 0xD0}},
    {"glow SetParam argument reads", 0x008BFDE0, 17,
     {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x0C, 0x8B, 0x10, 0x89, 0x51, 0x2C, 0x8A, 0x50, 0x04, 0x8A, 0x40, 0x08}},
    {"underwater list composite pass load", 0x008BFDF2, 6, {0x8B, 0x71, 0x24, 0x8B, 0x76, 0x08}},
    {"underwater composite colour store", 0x008BFE08, 3, {0x89, 0x7E, 0x30}},
    {"clear-view list composite pass load", 0x008BFE14, 6, {0x8B, 0x41, 0x10, 0x8B, 0x48, 0x08}},
    {"clear-view composite colour store", 0x008BFE21, 3, {0x89, 0x51, 0x30}},
    {"glow SetParam return", 0x008BFE26, 3, {0xC2, 0x08, 0x00}},
    {"glow effect vtable store", 0x008BFE98, 6, {0xC7, 0x03, 0xC8, 0x41, 0xA9, 0x00}},
    {"glow composite pass vtable store", 0x008C2206, 6, {0xC7, 0x06, 0x94, 0x42, 0xA9, 0x00}},
    {"glow composite colour argument", 0x008C28A1, 3, {0x83, 0xC6, 0x30}},
    {
    "world render tail after FFX end", 0x004F9286, 11,
    {0xE8, 0x55, 0xE8, 0x24, 0x00, 0x5F, 0x5E, 0x8B, 0xE5, 0x5D, 0xC3}},
};
inline bool Layout() noexcept {
    __try {
        const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if(base!=0x400000) return false;
        const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if(dos->e_magic!=IMAGE_DOS_SIGNATURE || dos->e_lfanew<=0 || dos->e_lfanew>4096) return false;
        const auto* pe=reinterpret_cast<const IMAGE_NT_HEADERS32*>(base+dos->e_lfanew);
        if(pe->Signature!=IMAGE_NT_SIGNATURE || pe->FileHeader.TimeDateStamp!=0x4C2452FE) return false;
        for(const auto& g:guards) if(std::memcmp(reinterpret_cast<const void*>(g.address),g.bytes,g.size)) return false;
        return *reinterpret_cast<const uintptr_t*>(0x00A941D8)==0x008BFDE0;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
inline bool Read(uintptr_t pass,uint8_t& out) noexcept {
    __try {
        if(!pass || *reinterpret_cast<const uintptr_t*>(pass)!=0x00A94294) return false;
        out=*reinterpret_cast<const uint8_t*>(pass+0x33); return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
inline bool Write(uintptr_t pass,uint8_t value) noexcept {
    __try {
        if(!pass || *reinterpret_cast<const uintptr_t*>(pass)!=0x00A94294) return false;
        *reinterpret_cast<volatile uint8_t*>(pass+0x33)=value; return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
inline bool Passes(uintptr_t (&out)[2]) noexcept {
    __try {
        const auto effect=*reinterpret_cast<const uintptr_t*>(0x00B74364);
        const auto ffx=*reinterpret_cast<const uintptr_t*>(0x00D45774);
        if(!effect || !ffx || *reinterpret_cast<const int*>(ffx+0x30)==0 ||
           *reinterpret_cast<const uintptr_t*>(0x00D45780)!=effect ||
           *reinterpret_cast<const uintptr_t*>(effect)!=0x00A941C8 ||
           *reinterpret_cast<const uint32_t*>(0x00CD8794)!=0) return false;
        const auto enabled=*reinterpret_cast<const uintptr_t*>(effect+4);
        if(!enabled || *reinterpret_cast<const int*>(enabled+0x30)==0) return false;
        for(int i=0;i<2;++i) {
            const auto list=effect+(i ? 0x1C : 0x08);
            const auto count=*reinterpret_cast<const uint32_t*>(list+4);
            const auto data=*reinterpret_cast<const uintptr_t*>(list+8);
            if(count<=2 || count>64 || !data) return false;
            out[i]=*reinterpret_cast<const uintptr_t*>(data+8);
            uint8_t value=0; if(!Read(out[i],value)) return false;
        }
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
inline bool Ghost() noexcept {
    __try { const auto death=*reinterpret_cast<const uintptr_t*>(0x00B74368);
        return death && *reinterpret_cast<const uintptr_t*>(0x00D45780)==death;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return true; }
}
}
