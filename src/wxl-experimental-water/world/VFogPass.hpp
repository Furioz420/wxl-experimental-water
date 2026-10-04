#pragma once
#include "wxl/RenderDepthApi.h"
#include "wxl/RenderApi.h"
#include <d3d9.h>
struct FrameInputs;
struct Config;
class DepthCopy;
namespace wxl::water::vfog {
bool MultisampleDepth(IDirect3DDevice9*,WXL_SceneDepth&);
bool MultisampleCopy(IDirect3DDevice9*,WXL_SceneDepth&,DepthCopy&,IDirect3DSurface9*&);
bool FrameData(IDirect3DDevice9*,const WXL_SceneDepth&,FrameInputs&);
Config FogSettings();
bool Draw(IDirect3DDevice9* device, const WXL_SceneDepth& depth, bool indoors,
          float storm, const float direction[3], bool moon, float shafts, bool beforeGlow=false, bool beforeTransparents=false);
bool TransparentReady();
bool FinishTransparent(IDirect3DDevice9*,const WXL_SceneDepth&);
void Reset();
void BeginNativeWorld(const WXL_RenderPassContext&,bool enabled);
void EndNativeWorld();
bool ReplacingStockFog();
void Reload();
void Panel();
bool Enabled();
}
