#include "VWaterPass.hpp"
// Being inside the water: the band between the map's flat plane and the crest above it.
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.
//
// THE PROBLEM THIS EXISTS TO SOLVE
//   The client knows two states, and it decides between them by comparing the camera against the
//   map's FLAT water level. A wave does not move that level. So a crest can rise over the camera and
//   nothing happens: no medium, no tint, no sense of being inside anything. What is drawn is the
//   underside of a surface with clear air between it and the eye, and the wave reads as a hollow
//   shell -- which is exactly what it is, because no part of the client believes there is water up
//   there.
//
//   The band between the still-water plane and the crest above it is therefore ours. Below the
//   plane, the client is already right and this stays out of the way; the two never overlap, so
//   there is no double tint at the waterline and nothing to reconcile.
//
//   This is also why the submersion test cannot be a bare comparison. At the waterline the camera
//   sits within a wave's amplitude of the surface -- which is where a swimming player spends their
//   entire time -- and a plain test there flickers at the wave frequency. The dead band in
//   SubmersionState is what makes the crossing an event rather than a stutter.

#include "wxl-experimental-water/mesh/Lattice.hpp"
#include "wxl-experimental-water/sea/Sea.hpp"
#include "wxl-experimental-water/render/Surface.hpp"

#include "config.hpp"
#include "common/Log.hpp"
#include "engine/hook/Registry.hpp"
#include "engine/events/Event.hpp"
#include "game/Gx.hpp"
#include "game/Render.hpp"

#include <windows.h>
#include <d3d9.h>

#include <cmath>

namespace
{
    namespace gx = wxl::game::gx;
    namespace w  = wxl::water;

    /// Depth below the surface at which the medium reaches full strength, in world units.
    ///
    /// Short on purpose. This band is at most a wave amplitude tall, and a crest sweeps through it in
    /// under a second: the medium has to arrive within that or the wave has gone before it shows.
    constexpr float kFullDepth = 1.1f;
    constexpr float kMaxOpacity = 0.86f;

    void* g_tintPs = nullptr;
    bool  g_psFailed = false;

    /// Flat tint with a vertical gradient. The gradient is not decoration: looking down inside water
    /// is a longer path through the medium than looking up at the surface, so the lower half of the
    /// frame is genuinely deeper water and reads wrong without it.
    const char* kTintHlsl =
        "float4 gTint : register(c0);\n"                    // rgb medium colour, a strength
        "float4 main(float2 uv : TEXCOORD0) : COLOR\n"
        "{\n"
        "    float down = saturate(uv.y * 1.4 - 0.2);\n"
        "    return float4(gTint.rgb * lerp(1.0, 0.55, down),\n"
        "                  saturate(gTint.a * lerp(0.82, 1.0, down)));\n"
        "}\n";

    bool EnsureShader(gx::Device9 dev)
    {
        if (g_tintPs) return true;
        if (g_psFailed) return false;
        g_tintPs = gx::CompilePixelShader(dev, kTintHlsl, "ps_2_0");
        if (!g_tintPs)
        {
            g_psFailed = true;
            WLOG_WARN("water: the volume tint shader would not compile -- crests stay hollow");
            return false;
        }
        return true;
    }

    void ReleaseShader(void*, const void*)
    {
        gx::Release(g_tintPs);
        g_tintPs = nullptr;
        g_psFailed = false;
    }

    /// Draws the medium over the frame, restoring every state it touches.
    ///
    /// The UI is drawn after this point and inherits whatever is left set, so the restore is not
    /// tidiness -- a leaked blend mode or a lost depth test lands on the next thing to draw.
    void Composite(gx::Device9 dev, const float tint[3], float strength)
    {
        if (!EnsureShader(dev)) return;

        auto* d = static_cast<IDirect3DDevice9*>(dev.raw());

        DWORD oldZ = 0, oldZW = 0, oldBlend = 0, oldSrc = 0, oldDst = 0, oldCull = 0,
              oldAlphaTest = 0, oldFog = 0, oldSRGB = 0;
        d->GetRenderState(D3DRS_ZENABLE, &oldZ);
        d->GetRenderState(D3DRS_ZWRITEENABLE, &oldZW);
        d->GetRenderState(D3DRS_ALPHABLENDENABLE, &oldBlend);
        d->GetRenderState(D3DRS_SRCBLEND, &oldSrc);
        d->GetRenderState(D3DRS_DESTBLEND, &oldDst);
        d->GetRenderState(D3DRS_CULLMODE, &oldCull);
        d->GetRenderState(D3DRS_ALPHATESTENABLE, &oldAlphaTest);
        d->GetRenderState(D3DRS_FOGENABLE, &oldFog);
        d->GetRenderState(D3DRS_SRGBWRITEENABLE, &oldSRGB);

        IDirect3DVertexShader9* prevVs = nullptr;
        IDirect3DPixelShader9*  prevPs = nullptr;
        d->GetVertexShader(&prevVs);
        d->GetPixelShader(&prevPs);

        d->SetRenderState(D3DRS_ZENABLE, FALSE);
        d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        d->SetRenderState(D3DRS_FOGENABLE, FALSE);
        d->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);

        // The quad carries its own transformed positions, so the vertex stage has to be the fixed
        // one: a vertex shader left bound would transform coordinates that are already in screen
        // space and put the quad somewhere else entirely.
        d->SetVertexShader(nullptr);
        d->SetPixelShader(static_cast<IDirect3DPixelShader9*>(g_tintPs));

        const float c[4] = { tint[0], tint[1], tint[2], strength };
        d->SetPixelShaderConstantF(0, c, 1);

        gx::DrawFullscreenQuad(dev);

        d->SetVertexShader(prevVs);
        d->SetPixelShader(prevPs);
        if (prevVs) prevVs->Release();
        if (prevPs) prevPs->Release();

        d->SetRenderState(D3DRS_ZENABLE, oldZ);
        d->SetRenderState(D3DRS_ZWRITEENABLE, oldZW);
        d->SetRenderState(D3DRS_ALPHABLENDENABLE, oldBlend);
        d->SetRenderState(D3DRS_SRCBLEND, oldSrc);
        d->SetRenderState(D3DRS_DESTBLEND, oldDst);
        d->SetRenderState(D3DRS_CULLMODE, oldCull);
        d->SetRenderState(D3DRS_ALPHATESTENABLE, oldAlphaTest);
        d->SetRenderState(D3DRS_FOGENABLE, oldFog);
        d->SetRenderState(D3DRS_SRGBWRITEENABLE, oldSRGB);
    }

    void OnWorldEnd(void*, const void*)
    {
        gx::Device9 dev = gx::Device();
        if (!dev) return;

        // The same eye resolution the surface shader was handed this frame. Recomputing it here
        // would be a second opinion, and the two would disagree at exactly the moment that matters:
        // one frame where the medium is on screen and the surface is still lit from outside it.
        const w::EyeState& eye = w::Eye();
        if (!eye.hasPlane || !eye.submerged) return;

        // Below the plane the client owns the treatment. Leaving that band to it is what keeps the
        // two from stacking into a double tint exactly at the waterline.
        const float* cam = w::CameraPos();
        if (cam[2] <= eye.planeLevel) return;

        const float depth = eye.depth;
        const float strength = (depth / kFullDepth < 1.0f ? depth / kFullDepth : 1.0f) * kMaxOpacity;
        if (strength <= 0.01f) return;

        // The medium is the deep-water colour, so the inside of a crest is the same water the
        // outside of it was shaded from. Two colours here would make the surface a lid on something
        // else rather than a boundary of one thing.
        const w::SurfaceLook& look = w::Look();
        const float tint[3] = { look.extinction[0], look.extinction[1], look.extinction[2] };
        // Extinction is per-channel removal; what is LEFT is the colour seen through it.
        const float seen[3] = { 1.0f - tint[0] * 2.2f, 1.0f - tint[1] * 2.2f, 1.0f - tint[2] * 2.2f };
        const float clamped[3] = {
            seen[0] < 0.02f ? 0.02f : seen[0],
            seen[1] < 0.02f ? 0.02f : seen[1],
            seen[2] < 0.02f ? 0.02f : seen[2],
        };

        Composite(dev, clamped, strength);
    }

    void __cdecl OnRenderPass(void* user, const WXL_RenderPassContext* pass)
    {
        if(wxl::water::forever::UsedInFrame(pass->frameId)) return;
        const wxl::events::WorldRenderEndArgs args{pass->device};
        OnWorldEnd(user, &args);
    }

    bool Install()
    {
        if (const auto* api = wxl::game::render::Api())
        {
            if (!api->AddPostProcess(WXL_RENDER_WATER_VOLUME, OnRenderPass, nullptr)) return false;
        }
        else wxl::events::Subscribe(wxl::events::Event::OnWorldRenderEnd, &OnWorldEnd, nullptr);
        // A DEFAULT-pool shader survives a reset, but the device it was created on does not; letting
        // it go and recompiling is cheaper than reasoning about which of the two this is.
        wxl::events::Subscribe(wxl::events::Event::OnDeviceLost, &ReleaseShader, nullptr);
        WLOG_INFO("water: volume installed");
        return true;
    }
}

WXL_REGISTER_FEATURE("water-volume", wxl::features::water, Install)
