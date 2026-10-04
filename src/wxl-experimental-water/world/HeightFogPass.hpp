// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "HeightFog.hpp"
#include <d3d9.h>
#include <wrl/client.h>

namespace wxl::water::fog
{
    // Caller supplies only a CURRENT readable depth snapshot, during the core read interval.
    // Never change attachments: the core owns depth detachment/restoration around callbacks.
    inline HRESULT Draw(IDirect3DDevice9* d, IDirect3DPixelShader9* shader,
                        const WXL_SceneDepth& depth, const Constants& constants)
    {
        using Microsoft::WRL::ComPtr;
        if (!d || !shader || !depth.texture) return D3DERR_INVALIDCALL;
        D3DVIEWPORT9 vp{};
        if (FAILED(d->GetViewport(&vp)) || vp.X || vp.Y || vp.Width!=depth.width || vp.Height!=depth.height)
            return S_FALSE;
        ComPtr<IDirect3DSurface9> target, back, depthBinding;
        if (FAILED(d->GetRenderTarget(0,target.GetAddressOf())) ||
            FAILED(d->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,back.GetAddressOf())) || target.Get()!=back.Get())
            return S_FALSE;
        // Sampling a bound depth target is invalid. Fail closed even if the caller misuses the API.
        HRESULT hr=d->GetDepthStencilSurface(depthBinding.GetAddressOf());
        if ((FAILED(hr) && hr!=D3DERR_NOTFOUND) || depthBinding) return S_FALSE;
        D3DSURFACE_DESC desc{};
        if (FAILED(target->GetDesc(&desc)) || desc.Width!=depth.width || desc.Height!=depth.height ||
            desc.MultiSampleType!=D3DMULTISAMPLE_NONE) return S_FALSE;
        for (DWORD i=1; i<4; ++i)
        {
            ComPtr<IDirect3DSurface9> extra;
            const HRESULT read=d->GetRenderTarget(i,extra.GetAddressOf());
            if (extra || (FAILED(read) && read!=D3DERR_NOTFOUND && read!=D3DERR_INVALIDCALL)) return S_FALSE;
        }
        ComPtr<IDirect3DStateBlock9> saved;
        if (FAILED(hr=d->CreateStateBlock(D3DSBT_ALL,saved.GetAddressOf())) ||
            FAILED(hr=saved->Capture())) return hr;
        auto set=[&](HRESULT result) { if (FAILED(result) && SUCCEEDED(hr)) hr=result; };
        hr=S_OK;
        set(d->SetVertexShader(nullptr)); set(d->SetPixelShader(shader));
        set(d->SetRenderState(D3DRS_ZENABLE,FALSE)); set(d->SetRenderState(D3DRS_ZWRITEENABLE,FALSE));
        set(d->SetRenderState(D3DRS_STENCILENABLE,FALSE)); set(d->SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE));
        set(d->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE)); set(d->SetRenderState(D3DRS_FOGENABLE,FALSE));
        set(d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE)); set(d->SetRenderState(D3DRS_FILLMODE,D3DFILL_SOLID));
        set(d->SetRenderState(D3DRS_SRGBWRITEENABLE,FALSE)); set(d->SetRenderState(D3DRS_COLORWRITEENABLE,7));
        set(d->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE)); set(d->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_ADD));
        set(d->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA));
        set(d->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA));
        set(d->SetTexture(0,static_cast<IDirect3DTexture9*>(depth.texture)));
        set(d->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_POINT));
        set(d->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_POINT));
        set(d->SetSamplerState(0,D3DSAMP_MIPFILTER,D3DTEXF_NONE));
        set(d->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP));
        set(d->SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP));
        set(d->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,FALSE));
        set(d->SetPixelShaderConstantF(0,constants.row[0],10));
        struct V { float x,y,z,w,u,v; };
        const float width=static_cast<float>(vp.Width), height=static_cast<float>(vp.Height);
        const V quad[]{{-.5f,-.5f,0,1,0,0},{width-.5f,-.5f,0,1,1,0},
            {-.5f,height-.5f,0,1,0,1},{width-.5f,height-.5f,0,1,1,1}};
        set(d->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1));
        if (SUCCEEDED(hr)) hr=d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,quad,sizeof(V));
        const HRESULT restored=saved->Apply();
        return FAILED(restored) ? restored : hr;
    }
}
