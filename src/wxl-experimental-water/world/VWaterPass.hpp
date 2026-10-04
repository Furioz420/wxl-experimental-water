#pragma once
#include <d3d9.h>
#include <cstdint>
namespace wxl::water::forever {
bool Armed();
bool UsedInFrame(uint64_t frame);
void Select(const void* settings);
void Deselect();
void BeforeDraw(IDirect3DDevice9* device);
void AfterDraw(IDirect3DDevice9* device);
bool Panel();
}
