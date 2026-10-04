// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "HeightFogPass.hpp"
#include "wxl-experimental-water/shaders/FogUpsamplePs.h"
namespace wxl::water::fog {
    class HalfPass
    {
        template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
        Ptr<IDirect3DTexture9> trace_;
        Ptr<IDirect3DSurface9> surface_;
        Ptr<IDirect3DPixelShader9> compositeShader_;
        uint64_t generation_=0;
        UINT width_=0,height_=0;
        bool failed_=false;
    public:
        void Reset()
        {
            surface_.Reset(); trace_.Reset(); compositeShader_.Reset();
            generation_=0; width_=height_=0; failed_=false;
        }
        HRESULT Draw(IDirect3DDevice9* d,const WXL_SceneDepth& depth,const Constants& constants,IDirect3DPixelShader9* fullShader)
        {
            if (!d || !depth.texture || !fullShader) return D3DERR_INVALIDCALL;
            if (generation_!=depth.deviceGeneration) { Reset(); generation_=depth.deviceGeneration; }
            if (failed_) return fog::Draw(d,fullShader,depth,constants);

            D3DVIEWPORT9 vp{}; D3DSURFACE_DESC desc{};
            Ptr<IDirect3DSurface9> target,back,depthBinding;
            if (FAILED(d->GetViewport(&vp)) || vp.X || vp.Y || vp.Width!=depth.width || vp.Height!=depth.height ||
                FAILED(d->GetRenderTarget(0,target.GetAddressOf())) ||
                FAILED(d->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,back.GetAddressOf())) || target.Get()!=back.Get() ||
                FAILED(target->GetDesc(&desc)) || desc.Width!=depth.width || desc.Height!=depth.height || desc.MultiSampleType!=D3DMULTISAMPLE_NONE) return S_FALSE;
            HRESULT hr=d->GetDepthStencilSurface(depthBinding.GetAddressOf());
            if (depthBinding || (FAILED(hr) && hr!=D3DERR_NOTFOUND)) return S_FALSE;
            for(DWORD i=1;i<4;++i)
            {
                Ptr<IDirect3DSurface9> extra; hr=d->GetRenderTarget(i,extra.GetAddressOf());
                if(extra || (FAILED(hr) && hr!=D3DERR_NOTFOUND && hr!=D3DERR_INVALIDCALL)) return S_FALSE;
            }
            const UINT width=(depth.width+1)/2,height=(depth.height+1)/2;
            if (!trace_ || width_!=width || height_!=height)
            {
                surface_.Reset(); trace_.Reset();
                hr=d->CreateTexture(width,height,1,D3DUSAGE_RENDERTARGET,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,trace_.GetAddressOf(),nullptr);
                if (SUCCEEDED(hr)) hr=trace_->GetSurfaceLevel(0,surface_.GetAddressOf());
                if (FAILED(hr)) { failed_=true; return fog::Draw(d,fullShader,depth,constants); }
                width_=width; height_=height;
            }
            if (!compositeShader_) {
                hr=d->CreatePixelShader(reinterpret_cast<const DWORD*>(kFogUpsamplePs),compositeShader_.GetAddressOf());
                if(FAILED(hr)) { failed_=true; return fog::Draw(d,fullShader,depth,constants); }
            }
            Ptr<IDirect3DStateBlock9> saved;
            if (FAILED(hr=d->CreateStateBlock(D3DSBT_ALL,saved.GetAddressOf())) || FAILED(hr=saved->Capture())) return hr;
            hr=S_OK;
            auto set=[&](HRESULT result) { if(FAILED(result) && SUCCEEDED(hr)) hr=result; };
            set(d->SetVertexShader(nullptr));
            for(auto state : {D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_STENCILENABLE,D3DRS_SCISSORTESTENABLE,
                D3DRS_ALPHATESTENABLE,D3DRS_FOGENABLE,D3DRS_SRGBWRITEENABLE,D3DRS_ALPHABLENDENABLE}) set(d->SetRenderState(state,FALSE));
            set(d->SetRenderState(D3DRS_FILLMODE,D3DFILL_SOLID)); set(d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE));
            set(d->SetRenderState(D3DRS_COLORWRITEENABLE,15));
            for(DWORD s=0;s<2;++s)
            {
                set(d->SetTexture(s,nullptr));
                set(d->SetSamplerState(s,D3DSAMP_MINFILTER,s ? D3DTEXF_LINEAR : D3DTEXF_POINT));
                set(d->SetSamplerState(s,D3DSAMP_MAGFILTER,s ? D3DTEXF_LINEAR : D3DTEXF_POINT));
                set(d->SetSamplerState(s,D3DSAMP_MIPFILTER,D3DTEXF_NONE));
                set(d->SetSamplerState(s,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP));
                set(d->SetSamplerState(s,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP));
                set(d->SetSamplerState(s,D3DSAMP_SRGBTEXTURE,FALSE));
            }
            set(d->SetPixelShaderConstantF(0,constants.row[0],10));
            set(d->SetTexture(0,static_cast<IDirect3DTexture9*>(depth.texture)));
            set(d->SetPixelShader(fullShader));
            set(d->SetRenderTarget(0,surface_.Get()));
            const D3DVIEWPORT9 half{0,0,width,height,0,1}; set(d->SetViewport(&half));
            struct V { float x,y,z,w,u,v; };
            auto draw=[&](UINT w,UINT h)
            {
                const float x=static_cast<float>(w)-.5f,y=static_cast<float>(h)-.5f;
                const V quad[]{{-.5f,-.5f,0,1,0,0},{x,-.5f,0,1,1,0},{-.5f,y,0,1,0,1},{x,y,0,1,1,1}};
                set(d->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1));
                if (SUCCEEDED(hr)) set(d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,quad,sizeof(V)));
            };
            draw(width,height);
            set(d->SetRenderTarget(0,target.Get())); set(d->SetViewport(&vp));
            set(d->SetTexture(1,trace_.Get())); set(d->SetPixelShader(compositeShader_.Get()));
            const float traceSize[]{static_cast<float>(width),static_cast<float>(height),1.f/width,1.f/height};
            set(d->SetPixelShaderConstantF(10,traceSize,1));
            set(d->SetSamplerState(1,D3DSAMP_MINFILTER,D3DTEXF_POINT));
            set(d->SetSamplerState(1,D3DSAMP_MAGFILTER,D3DTEXF_POINT));
            set(d->SetRenderState(D3DRS_COLORWRITEENABLE,7));
            set(d->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE)); set(d->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_ADD));
            set(d->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA)); set(d->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA));
            draw(depth.width,depth.height);
            // State blocks omit attachments. Restore RT and viewport even on failure.
            const HRESULT rt=d->SetRenderTarget(0,target.Get());
            const HRESULT state=saved->Apply(); const HRESULT viewport=d->SetViewport(&vp);
            if(FAILED(rt)) return rt; if(FAILED(state)) return state; if(FAILED(viewport)) return viewport;
            return hr;
        }
    };
}
