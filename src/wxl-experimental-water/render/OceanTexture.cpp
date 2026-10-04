// Carries the detail field to the vertex stage.
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

#include "wxl-experimental-water/render/OceanTexture.hpp"
#include "wxl-experimental-water/sea/Ocean.hpp"

#include "common/Log.hpp"

#include <windows.h>
#include <d3d9.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstring>

namespace
{
    using namespace wxl::water;

    /// D3D9's vertex texture stages. They are NOT the pixel stages with a different number: the two
    /// sets are separate hardware, which is why binding the foam at stage 7 has never collided with
    /// anything here.
    constexpr unsigned kVertexSampler0 = 257;   // D3DVERTEXTEXTURESAMPLER0

    /// First pixel stage the shading maps ride on. High, clear of everything the liquid pass or our
    /// own surface uses -- the client configures 0 and 1, the ripple/scene/foam sit at 5..7 -- and
    /// bound at the draw, past the deferred flush.
    constexpr unsigned kShadeStage0 = 8;

    /// Two per cascade, split by CONSUMER rather than by content: the first half is the geometry
    /// maps (rise, dx, dy) the vertex stage fetches, one per vertex sampler -- four cascades out of
    /// exactly four samplers, which is the whole trade. The second half is the shading maps
    /// (slope, foam) the pixel stage samples, where bilinear filtering exists -- so the normal,
    /// where a flat step shows hardest, is the one thing that never sees the unfiltered path.
    constexpr int kOceanTextures = kOceanCascades * 2;

    IDirect3DTexture9* g_tex[kOceanTextures] = {};
    auto* g_owner = new Microsoft::WRL::ComPtr<IDirect3DDevice9>;
    bool g_failed = false;
    /// Whether the shading half ended up in half precision. Decided by the device, not by us.
    bool g_shadeHalf = false;

    /// float -> IEEE half, for the shading maps.
    ///
    /// The values are a slope and a foam coverage -- both around one, neither needing more than
    /// three decimal digits -- so the precision lost here is nothing and the filtering bought is
    /// everything. Denormals and infinities are not handled because nothing upstream can produce
    /// them: the field is normalised by measurement and the fold is clamped.
    uint16_t ToHalf(float f)
    {
        uint32_t b;
        std::memcpy(&b, &f, sizeof(b));
        const uint32_t sign = (b >> 16) & 0x8000u;
        int32_t exp = static_cast<int32_t>((b >> 23) & 0xFFu) - 127 + 15;
        uint32_t man = b & 0x7FFFFFu;

        if (exp <= 0)  return static_cast<uint16_t>(sign);                    // underflows to zero
        if (exp >= 31) return static_cast<uint16_t>(sign | 0x7BFFu);          // clamp to half's max
        return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) | (man >> 13));
    }

    /// Whether the device will actually FILTER a format, which is a different question from whether
    /// it will create one -- and the difference is the whole bug this asks about.
    ///
    /// D3D9 makes filtering optional for the float formats. A device that cannot filter one does not
    /// fail the create and does not fail the sampler state: it silently samples POINT. So a texture
    /// carrying the surface NORMAL comes back as flat blocks the size of a texel, and since the
    /// normal is what the light is computed from, the blocks are what the sea is lit in.
    bool Filterable(IDirect3DDevice9* dev, D3DFORMAT fmt)
    {
        IDirect3D9* d3d = nullptr;
        if (FAILED(dev->GetDirect3D(&d3d)) || !d3d) return false;

        D3DDEVICE_CREATION_PARAMETERS cp{};
        dev->GetCreationParameters(&cp);
        D3DDISPLAYMODE mode{};
        d3d->GetAdapterDisplayMode(cp.AdapterOrdinal, &mode);

        const HRESULT hr = d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, mode.Format,
                                                  D3DUSAGE_QUERY_FILTER, D3DRTYPE_TEXTURE, fmt);
        d3d->Release();
        return SUCCEEDED(hr);
    }

    bool Ensure(IDirect3DDevice9* dev)
    {
        if (g_owner->Get() != dev)
        {
            OceanTextureRelease();
            g_failed = false;
            *g_owner = dev;
            WLOG_INFO("water: detail textures rebound to device=%p", static_cast<void*>(dev));
        }
        if (g_failed) return false;
        if (g_tex[0]) return true;

        // The two halves want different things and no longer get the same format.
        //
        // The GEOMETRY half is read by the vertex stage, where D3D9 offers only R32F and
        // A32B32G32R32F and no filtering at all -- so there is nothing to choose and nothing to
        // gain by choosing.
        //
        // The SHADING half is read by the pixel stage, where filtering exists and is the entire
        // reason that half was split out in the first place. Full float is asked for first and
        // dropped the moment the device says it cannot filter it, which is the common answer on
        // the hardware this client runs on. Half float carries a slope to more digits than a
        // slope has.
        const bool full16 = Filterable(dev, D3DFMT_A16B16G16R16F);
        const bool full32 = Filterable(dev, D3DFMT_A32B32G32R32F);
        g_shadeHalf = !full32 && full16;

        for (int i = 0; i < kOceanTextures; ++i)
        {
            const bool shade = i >= kOceanCascades;
            const D3DFORMAT fmt = (shade && g_shadeHalf) ? D3DFMT_A16B16G16R16F
                                                         : D3DFMT_A32B32G32R32F;
            // DYNAMIC and DEFAULT together: the field changes every frame, so it has to be lockable
            // without a round trip, and a vertex texture cannot come from the managed pool. The pair
            // is what costs a release on device reset, handled below.
            const HRESULT hr = dev->CreateTexture(kOceanN, kOceanN, 1, D3DUSAGE_DYNAMIC,
                                                  fmt, D3DPOOL_DEFAULT, &g_tex[i], nullptr);
            if (FAILED(hr) || !g_tex[i])
            {
                OceanTextureRelease();
                g_failed = true;
                WLOG_WARN("water: detail field has nowhere to live (hr=0x%08lX) -- the sea keeps its "
                          "trains and loses its chop", static_cast<unsigned long>(hr));
                return false;
            }
        }

        // Said either way, because this one line answers "why is the sea in blocks" without a
        // single guess: the shading half's format is exactly what decides whether the surface
        // normal interpolates between texels or steps at every one of them.
        if (!full32 && !full16)
            WLOG_WARN("water: this device filters NEITHER float texture format -- the detail "
                      "normal will step at every texel and the sea will read as blocks. Nothing "
                      "in the sea's tuning can answer that; it is the device");
        else
            WLOG_INFO("water: detail field shading half in %s, filtered",
                      g_shadeHalf ? "half float (full float is not filterable here)"
                                  : "full float");
        return true;
    }
}

namespace wxl::water
{
    void OceanTextureUpdate(void* device)
    {
        auto* dev = static_cast<IDirect3DDevice9*>(device);
        if (!dev || !Ensure(dev)) return;

        for (int i = 0; i < kOceanTextures; ++i)
        {
            const float* src = (i < kOceanCascades) ? OceanGeoMap(i)
                                                    : OceanShadeMap(i - kOceanCascades);
            if (!src) continue;

            D3DLOCKED_RECT lr{};
            // DISCARD rather than a plain lock: the previous contents are never read back, and
            // saying so is what lets the driver hand over a fresh block instead of waiting for the
            // GPU to finish with the one being replaced.
            if (FAILED(g_tex[i]->LockRect(0, &lr, nullptr, D3DLOCK_DISCARD)) || !lr.pBits)
                continue;

            // Row by row against the reported pitch. A texture's rows are padded to the driver's own
            // alignment, so a single memcpy of the whole block writes the last rows past the end.
            auto* dst = static_cast<uint8_t*>(lr.pBits);
            const bool half = g_shadeHalf && i >= kOceanCascades;

            if (half)
            {
                for (int r = 0; r < kOceanN; ++r)
                {
                    auto* row = reinterpret_cast<uint16_t*>(dst + static_cast<size_t>(r) * lr.Pitch);
                    const float* s = src + static_cast<size_t>(r) * kOceanN * 4;
                    for (int c = 0; c < kOceanN * 4; ++c) row[c] = ToHalf(s[c]);
                }
            }
            else
            {
                const size_t rowBytes = static_cast<size_t>(kOceanN) * 4 * sizeof(float);
                for (int r = 0; r < kOceanN; ++r)
                    std::memcpy(dst + static_cast<size_t>(r) * lr.Pitch,
                                src + static_cast<size_t>(r) * kOceanN * 4, rowBytes);
            }

            g_tex[i]->UnlockRect(0);
        }
    }

    void OceanTextureBind(void* device)
    {
        auto* dev = static_cast<IDirect3DDevice9*>(device);
        if (!dev || !g_tex[0] || g_owner->Get() != dev) return;

        for (int i = 0; i < kOceanCascades; ++i)
        {
            const unsigned stage = kVertexSampler0 + static_cast<unsigned>(i);
            dev->SetTexture(stage, g_tex[i]);

            // POINT, and not for want of trying: D3D9 offers no filtering on a vertex fetch, on any
            // device, in any format. This is not a preference being expressed -- it is the only
            // thing the stage can do, which is why the vertex shader reconstructs the field with a
            // tent of its own out of four taps. What is set here is what those four taps read.
            dev->SetSamplerState(stage, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            dev->SetSamplerState(stage, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            dev->SetSamplerState(stage, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            // Wrapped, because the patch IS the period. That is what a transform of a finite grid
            // produces, and the cascades exist so the period is never large enough to be seen.
            dev->SetSamplerState(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
            dev->SetSamplerState(stage, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
        }

        for (int i = 0; i < kOceanCascades; ++i)
        {
            // The shading maps ride ordinary pixel stages, and that is the whole prize of the
            // split: LINEAR is available there, so the slope the surface is lit from -- and the
            // foam mask -- interpolate between texels instead of stepping at them.
            const unsigned stage = kShadeStage0 + static_cast<unsigned>(i);
            dev->SetTexture(stage, g_tex[kOceanCascades + i]);
            dev->SetSamplerState(stage, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            dev->SetSamplerState(stage, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            dev->SetSamplerState(stage, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            dev->SetSamplerState(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
            dev->SetSamplerState(stage, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
        }
    }

    void OceanTextureRelease()
    {
        for (int i = 0; i < kOceanTextures; ++i)
        {
            if (g_tex[i]) g_tex[i]->Release();
            g_tex[i] = nullptr;
        }
    }
}
