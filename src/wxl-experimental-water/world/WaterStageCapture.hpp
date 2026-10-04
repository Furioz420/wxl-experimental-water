// WXL-owned, opt-in one-frame diagnostics. No retained GPU resources.
#pragma once
#include <d3d9.h>
#include <wrl/client.h>
#include <wxl/RenderDepthApi.h>
#include <cstdio>
#include <cstring>
#include <vector>
namespace wxl::water::stagecapture {
inline uint64_t frame=0;
inline unsigned stages=0, draws=0;
inline wchar_t folder[1024]{};
inline bool Enabled() {
    static const bool enabled=[] {
        const auto size=GetEnvironmentVariableW(L"WXL_RENDER_WATER_CAPTURE",folder,1024);
        return size>0 && size<1024;
    }();
    return enabled;
}
inline bool Select(const WXL_SceneDepth& depth) {
    if(!Enabled()) return false;
    if(!frame && depth.frameId>=90) frame=depth.frameId;
    return frame && frame==depth.frameId;
}
// Capture only a bounded set of native water draws, before our state overrides.
inline void DrawState(IDirect3DDevice9* device,unsigned draw) {
    wchar_t path[1200]{}; swprintf_s(path,L"%s/draw-%u.txt",folder,draw);
    FILE* file=nullptr;
    if(_wfopen_s(&file,path,L"wx")!=0 || !file) return;
    D3DVIEWPORT9 vp{}; device->GetViewport(&vp);
    fprintf(file,"viewport %u %u %u %u %.9g %.9g\n",vp.X,vp.Y,vp.Width,vp.Height,vp.MinZ,vp.MaxZ);
    Microsoft::WRL::ComPtr<IDirect3DSurface9> ds,rt;
    device->GetDepthStencilSurface(&ds); device->GetRenderTarget(0,&rt);
    for(auto* surface:{ds.Get(),rt.Get()}) {
        D3DSURFACE_DESC desc{};
        if(surface && SUCCEEDED(surface->GetDesc(&desc)))
            fprintf(file,"surface %p format=%u samples=%u quality=%lu size=%ux%u\n",surface,desc.Format,desc.MultiSampleType,desc.MultiSampleQuality,desc.Width,desc.Height);
    }
    constexpr D3DRENDERSTATETYPE states[]{D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_ZFUNC,
        D3DRS_ALPHATESTENABLE,D3DRS_ALPHAFUNC,D3DRS_ALPHAREF,D3DRS_ALPHABLENDENABLE,
        D3DRS_SRCBLEND,D3DRS_DESTBLEND,D3DRS_COLORWRITEENABLE,D3DRS_CULLMODE,
        D3DRS_DEPTHBIAS,D3DRS_SLOPESCALEDEPTHBIAS,D3DRS_MULTISAMPLEANTIALIAS,D3DRS_MULTISAMPLEMASK};
    for(auto state:states) { DWORD value=0; auto hr=device->GetRenderState(state,&value);
        fprintf(file,"state %u = 0x%08lX hr=%08lX\n",state,value,hr); }
    float constants[256*4]{};
    if(SUCCEEDED(device->GetVertexShaderConstantF(0,constants,256)))
        for(unsigned n=0;n<256;++n) fprintf(file,"vs-c%u %.9g %.9g %.9g %.9g\n",n,constants[n*4],constants[n*4+1],constants[n*4+2],constants[n*4+3]);
    fclose(file);
    Microsoft::WRL::ComPtr<IDirect3DVertexShader9> vs;
    Microsoft::WRL::ComPtr<IDirect3DPixelShader9> ps;
    device->GetVertexShader(&vs); device->GetPixelShader(&ps);
    auto save=[&](auto* shader,const wchar_t* kind) {
        UINT size=0;
        if(!shader || FAILED(shader->GetFunction(nullptr,&size)) || !size || size>65536) return;
        std::vector<unsigned char> bytes(size);
        if(FAILED(shader->GetFunction(bytes.data(),&size))) return;
        swprintf_s(path,L"%s/draw-%u-%s.bin",folder,draw,kind);
        file=nullptr;
        if(_wfopen_s(&file,path,L"wbx")==0 && file) { fwrite(bytes.data(),1,size,file); fclose(file); }
    };
    save(vs.Get(),L"vs"); save(ps.Get(),L"ps");
}
inline HRESULT Capture(IDirect3DDevice9* device,const WXL_SceneDepth& depth,unsigned stage,const wchar_t* name) {
    if(!Select(depth) || stage>=8 || (stages&(1u<<stage))) return S_FALSE;
    stages|=1u<<stage; // Bounded, including errors and reentrancy.
    using Microsoft::WRL::ComPtr;
    ComPtr<IDirect3DSurface9> target,resolved,read;
    D3DSURFACE_DESC desc{};
    HRESULT hr=device->GetRenderTarget(0,&target);
    if(FAILED(hr) || !target) return FAILED(hr) ? hr : E_FAIL;
    if(FAILED(hr=target->GetDesc(&desc))) return hr;
    if((desc.Format!=D3DFMT_X8R8G8B8 && desc.Format!=D3DFMT_A8R8G8B8) ||
        !desc.Width || !desc.Height || desc.Width>8192 || desc.Height>4096) return D3DERR_NOTAVAILABLE;
    if(FAILED(hr=device->CreateRenderTarget(desc.Width,desc.Height,desc.Format,D3DMULTISAMPLE_NONE,0,FALSE,&resolved,nullptr))) return hr;
    if(FAILED(hr=device->StretchRect(target.Get(),nullptr,resolved.Get(),nullptr,D3DTEXF_NONE))) return hr;
    if(FAILED(hr=device->CreateOffscreenPlainSurface(desc.Width,desc.Height,desc.Format,D3DPOOL_SYSTEMMEM,&read,nullptr))) return hr;
    if(FAILED(hr=device->GetRenderTargetData(resolved.Get(),read.Get()))) return hr;
    D3DLOCKED_RECT locked{};
    if(FAILED(hr=read->LockRect(&locked,nullptr,D3DLOCK_READONLY))) return hr;
    std::vector<unsigned char> pixels(static_cast<size_t>(desc.Width)*desc.Height*4);
    for(UINT y=0;y<desc.Height;++y) {
        auto* row=pixels.data()+static_cast<size_t>(y)*desc.Width*4;
        memcpy(row,static_cast<const char*>(locked.pBits)+y*locked.Pitch,desc.Width*4);
        for(UINT x=0;x<desc.Width;++x) row[x*4+3]=255;
    }
    read->UnlockRect();
    wchar_t path[1200]{}; swprintf_s(path,L"%s/%s.bmp",folder,name);
    HANDLE file=CreateFileW(path,GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    BITMAPFILEHEADER header{}; header.bfType=0x4d42;
    header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER); header.bfSize=header.bfOffBits+static_cast<DWORD>(pixels.size());
    BITMAPINFOHEADER info{}; info.biSize=sizeof(info); info.biWidth=desc.Width;
    info.biHeight=-static_cast<LONG>(desc.Height); info.biPlanes=1; info.biBitCount=32; info.biCompression=BI_RGB;
    DWORD written=0;
    const bool ok=WriteFile(file,&header,sizeof(header),&written,nullptr) && written==sizeof(header) &&
        WriteFile(file,&info,sizeof(info),&written,nullptr) && written==sizeof(info) &&
        WriteFile(file,pixels.data(),static_cast<DWORD>(pixels.size()),&written,nullptr) && written==pixels.size();
    CloseHandle(file); return ok ? S_OK : E_FAIL;
}
}
