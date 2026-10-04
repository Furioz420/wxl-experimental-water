// The frame as it stood before the water: what the surface bends when you look through it.
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
// Until now the surface has had NOTHING behind it. The medium was shaded from an extinction curve
// against a flat guess at what the bottom might look like, which gives a believable depth gradient
// and no sense whatever of looking into something. Sand, rock, a swimmer's own legs -- none of it
// was there to be seen.
//
// The copy taken here is what those are read out of. It is the ordinary frame, captured at the last
// instant before any water draws over it, and the surface then samples it displaced by its own
// normal: the bottom bends and swims with the waves instead of lying flat behind glass.

#include "wxl-experimental-water/render/Refraction.hpp"
#include "Performance.hpp"

#include "config.hpp"
#include "common/Log.hpp"
#include "engine/hook/Registry.hpp"
#include "engine/events/Event.hpp"
#include "game/Gx.hpp"
#include "game/Render.hpp"
#include "offsets/engine/Gx.hpp"

#include <windows.h>
#include <d3d9.h>

namespace
{
    namespace gx = wxl::game::gx;

    gx::RenderTarget g_scene;
    bool     g_captured = false;
    const WXL_RenderApi* g_shared = nullptr;
    const WXL_WaterDepthApi* g_depth = nullptr;

    /// Captures the frame into our own target.
    ///
    /// A plain surface-to-surface blit rather than a re-render: the frame is already drawn, and the
    /// only thing that matters is taking it before the water covers it. Point filtering, because the
    /// copy is the same size as the source and any filtering here would only soften the image the
    /// refraction is about to read.
    void Capture(void*, const void* args)
    {
        auto* a = static_cast<const wxl::events::LiquidRenderArgs*>(args);

        // Fires once per pass; only the first of a frame is before any water. Keyed off the pass
        // index rather than a frame counter, because a capture taken after the main pass would
        // already contain the water it is meant to sit behind.
        if (a->passType != 0) return;
        wxl::water::perf::Scope timing(wxl::water::perf::Stage::Capture);

        g_captured = false;

        gx::Device9 dev = gx::Device();
        if (!dev) return;
        if (!gx::EnsureBackbufferTarget(dev, g_scene, D3DFMT_A8R8G8B8)) return;

        auto* d = static_cast<IDirect3DDevice9*>(dev.raw());

        // THE FRAME LIVES IN THE CURRENT RENDER TARGET, NOT THE BACKBUFFER. With antialiasing on,
        // the scene is drawn into a multisampled target and only RESOLVED into the backbuffer at
        // the end of the frame -- so a mid-frame backbuffer copy captures last frame's leftovers,
        // and the refraction faithfully showed a stale near-empty image: the sea went dark and
        // nothing showed through, but only with AA enabled, which is what made it read as an
        // extinction fault instead of a capture fault. Copying from the active target is right in
        // both worlds: without AA it IS the backbuffer, and with AA the StretchRect below is
        // exactly D3D9's multisample resolve.
        IDirect3DSurface9* src = nullptr;
        if (FAILED(d->GetRenderTarget(0, &src)) || !src) return;

        const HRESULT hr = d->StretchRect(src, nullptr,
                                          static_cast<IDirect3DSurface9*>(g_scene.surface), nullptr,
                                          D3DTEXF_NONE);
        src->Release();

        if (FAILED(hr))
        {
            static unsigned logged = 0;
            if (logged < 4)
            {
                ++logged;
                WLOG_WARN("water: scene copy failed hr=0x%08lX -- the surface falls back to its flat"
                          " bottom colour", static_cast<unsigned long>(hr));
            }
            return;
        }
        g_captured = true;
    }

    void OnDeviceLost(void*, const void*)
    {
        gx::Release(g_scene);
        g_captured = false;
    }

    bool Install()
    {
        g_shared = wxl::game::render::Api();
        if (g_shared)
        {
            g_shared->RequestSceneColor();
            auto* depth = static_cast<const WXL_WaterDepthApi*>(wxl::runtime::extensions::GetInterface(
                "wxl.water-depth", WXL_WATER_DEPTH_API_VERSION));
            if (depth && depth->structSize>=sizeof(*depth) && depth->apiVersion==WXL_WATER_DEPTH_API_VERSION &&
                depth->sceneDepthSize==sizeof(WXL_SceneDepth) && depth->RequestCopy && depth->GetDepth)
            { g_depth=depth; g_depth->RequestCopy(); }
            WLOG_INFO("water: using core pre-liquid scene capture (wxl.render v1)");
            return true;
        }
        wxl::events::Subscribe(wxl::events::Event::OnLiquidRender, &Capture, nullptr);
        wxl::events::Subscribe(wxl::events::Event::OnDeviceLost, &OnDeviceLost, nullptr);
        WLOG_INFO("water: refraction installed");
        return true;
    }
}

namespace wxl::water
{
    bool WaterDepth(WXL_SceneDepth& out)
    {
        if(!wxl::game::render::EffectsEnabled()) { out={}; out.structSize=sizeof(out); return false; }
        out={}; out.structSize=sizeof(out);
        WXL_SceneColor color{}; color.structSize=sizeof(color);
        if (!g_depth || !g_shared || !g_depth->GetDepth(&out) || !g_shared->GetSceneColor(&color) ||
            out.frameId!=color.frameId || out.deviceGeneration!=color.deviceGeneration ||
            out.width!=color.width || out.height!=color.height || !out.texture)
        { out={}; out.structSize=sizeof(out); return false; }
        return true;
    }
    void* SceneCopy()
    {
        if (g_shared)
        {
            WXL_SceneColor color{};
            color.structSize = sizeof(color);
            return g_shared->GetSceneColor(&color) ? color.texture : nullptr;
        }
        return g_captured ? g_scene.texture : nullptr;
    }
    void  SceneSize(float& width, float& height)
    {
        if (g_shared)
        {
            WXL_SceneColor color{};
            color.structSize = sizeof(color);
            g_shared->GetSceneColor(&color);
            width = static_cast<float>(color.width);
            height = static_cast<float>(color.height);
            return;
        }
        width  = static_cast<float>(g_scene.width);
        height = static_cast<float>(g_scene.height);
    }
}

WXL_REGISTER_FEATURE("water-refraction", wxl::features::water, Install)
