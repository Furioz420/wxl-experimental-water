// The shading maps the surface is textured from, synthesized rather than loaded.
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

#include "wxl-experimental-water/render/Noise.hpp"

#include "common/Log.hpp"

#include <windows.h>
#include <d3d9.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace
{
    using namespace wxl::water;

    // ---- sizes ---------------------------------------------------------------------------------
    //
    // Split by what the map is FOR rather than uniformly. The three static maps carry structure the
    // eye follows -- foam filaments, the broad bump, the noise every threshold is jittered by -- and
    // are laid over hundreds of world units, so they get the resolution. The two animated sets are
    // grain: tiled small, read through motion, and multiplied by eight and eight. Halving their side
    // is a quarter of the memory and a quarter of the build, for detail nothing can resolve anyway.
    constexpr int kBroadN = 256;
    constexpr int kFineN  = 128;
    constexpr int kSkyW   = 256;
    constexpr int kSkyH   = 64;

    // ---- the lattice ---------------------------------------------------------------------------

    uint32_t Hash3(int x, int y, int z, uint32_t seed)
    {
        uint32_t h = seed;
        h ^= static_cast<uint32_t>(x) * 0x8DA6B343u;
        h ^= static_cast<uint32_t>(y) * 0xD8163841u;
        h ^= static_cast<uint32_t>(z) * 0xCB1AB31Fu;
        h ^= h >> 15; h *= 0x2C1B3C6Du;
        h ^= h >> 12; h *= 0x297A2D39u;
        h ^= h >> 15;
        return h;
    }

    inline float Unit(uint32_t h) { return static_cast<float>(h & 0xFFFFFFu) * (1.0f / 16777216.0f); }
    inline int   Wrap(int v, int p) { const int r = v % p; return (r < 0) ? r + p : r; }
    inline float Fade(float t) { return t * t * (3.0f - 2.0f * t); }

    /// Value noise on a periodic lattice. The coordinates are in [0,1) and the periods are in
    /// CELLS, which is what makes the result tile exactly: the corner at u=1 is the corner at u=0
    /// by construction, not by a blend at the edge that would show as a seam.
    float Noise3(float u, float v, float w, int pu, int pv, int pw, uint32_t seed)
    {
        const float x = u * static_cast<float>(pu);
        const float y = v * static_cast<float>(pv);
        const float z = w * static_cast<float>(pw);

        const int xi = static_cast<int>(std::floor(x));
        const int yi = static_cast<int>(std::floor(y));
        const int zi = static_cast<int>(std::floor(z));

        const float fx = Fade(x - static_cast<float>(xi));
        const float fy = Fade(y - static_cast<float>(yi));
        const float fz = Fade(z - static_cast<float>(zi));

        const int x0 = Wrap(xi, pu), x1 = Wrap(xi + 1, pu);
        const int y0 = Wrap(yi, pv), y1 = Wrap(yi + 1, pv);
        const int z0 = Wrap(zi, pw), z1 = Wrap(zi + 1, pw);

        const float c000 = Unit(Hash3(x0, y0, z0, seed));
        const float c100 = Unit(Hash3(x1, y0, z0, seed));
        const float c010 = Unit(Hash3(x0, y1, z0, seed));
        const float c110 = Unit(Hash3(x1, y1, z0, seed));
        const float c001 = Unit(Hash3(x0, y0, z1, seed));
        const float c101 = Unit(Hash3(x1, y0, z1, seed));
        const float c011 = Unit(Hash3(x0, y1, z1, seed));
        const float c111 = Unit(Hash3(x1, y1, z1, seed));

        const float a = c000 + (c100 - c000) * fx;
        const float b = c010 + (c110 - c010) * fx;
        const float c = c001 + (c101 - c001) * fx;
        const float d = c011 + (c111 - c011) * fx;
        const float e = a + (b - a) * fy;
        const float f = c + (d - c) * fy;
        return e + (f - e) * fz;
    }

    /// Octave sum, in [0,1). `base` is the octave-zero cell count across the tile and doubles per
    /// octave, which is what keeps every octave periodic and therefore the whole sum periodic.
    ///
    /// `pw` -- the cell count along the LOOP -- deliberately does NOT double. The loop is sampled at
    /// a handful of slices, so a time lattice that refined with the space lattice would end up with
    /// fewer than two slices per cell at the top octave: the finest detail would be a different
    /// pattern in every frame, which is a boil so fast it reads as static rather than as water.
    /// Holding it fixed means every octave crosses its time cells at the same rate.
    float Fbm(float u, float v, float w, int base, int pw, int octaves, uint32_t seed)
    {
        float sum = 0.0f, amp = 1.0f, norm = 0.0f;
        for (int o = 0; o < octaves; ++o)
        {
            sum += amp * Noise3(u, v, w, base << o, base << o, pw,
                                seed + static_cast<uint32_t>(o) * 0x9E3779B9u);
            norm += amp;
            amp *= 0.5f;
        }
        return sum / norm;
    }

    /// The same sum folded at its midpoint. A value field is smooth blobs; folding it puts a CREASE
    /// wherever it crosses the middle, and the creases join up into the connected filaments that
    /// foam lace and caustic webs are made of. Blobs cannot be thresholded into filaments -- that
    /// gives islands -- which is the whole reason this variant exists.
    float RidgedFbm(float u, float v, float w, int base, int pw, int octaves, uint32_t seed)
    {
        float sum = 0.0f, amp = 1.0f, norm = 0.0f;
        for (int o = 0; o < octaves; ++o)
        {
            const float n = Noise3(u, v, w, base << o, base << o, pw,
                                   seed + static_cast<uint32_t>(o) * 0x85EBCA6Bu);
            sum += amp * (1.0f - std::fabs(n * 2.0f - 1.0f));
            norm += amp;
            amp *= 0.5f;
        }
        return sum / norm;
    }

    // ---- packing -------------------------------------------------------------------------------

    inline uint32_t Argb(float r, float g, float b, float a)
    {
        auto q = [](float x) -> uint32_t
        {
            const float c = std::min(std::max(x, 0.0f), 1.0f);
            return static_cast<uint32_t>(c * 255.0f + 0.5f);
        };
        return (q(a) << 24) | (q(r) << 16) | (q(g) << 8) | q(b);
    }

    /// One image, level zero, in ARGB. Kept on the CPU because the mip chain is built from it.
    struct Image
    {
        int w = 0, h = 0;
        std::vector<uint32_t> px;
        Image(int width, int height) : w(width), h(height), px(static_cast<size_t>(width) * height) {}
    };

    /// Box-halves an image. Correct for every map here because all of them are LINEAR quantities --
    /// a slope, a coverage, a brightness -- so the average of four texels is the value of the texel
    /// that replaces them. It is also why the slope maps store a centred value rather than a
    /// normalised vector: averaging normals would shorten them, averaging slopes does not.
    Image Halve(const Image& src)
    {
        Image dst(std::max(src.w >> 1, 1), std::max(src.h >> 1, 1));
        for (int y = 0; y < dst.h; ++y)
        {
            const int y0 = std::min(y * 2, src.h - 1);
            const int y1 = std::min(y * 2 + 1, src.h - 1);
            for (int x = 0; x < dst.w; ++x)
            {
                const int x0 = std::min(x * 2, src.w - 1);
                const int x1 = std::min(x * 2 + 1, src.w - 1);
                const uint32_t a = src.px[static_cast<size_t>(y0) * src.w + x0];
                const uint32_t b = src.px[static_cast<size_t>(y0) * src.w + x1];
                const uint32_t c = src.px[static_cast<size_t>(y1) * src.w + x0];
                const uint32_t d = src.px[static_cast<size_t>(y1) * src.w + x1];
                uint32_t out = 0;
                for (int s = 0; s < 32; s += 8)
                {
                    const uint32_t m = (((a >> s) & 0xFF) + ((b >> s) & 0xFF)
                                      + ((c >> s) & 0xFF) + ((d >> s) & 0xFF) + 2) >> 2;
                    out |= m << s;
                }
                dst.px[static_cast<size_t>(y) * dst.w + x] = out;
            }
        }
        return dst;
    }

    /// Creates the texture with a full mip chain and fills every level from the image.
    ///
    /// The chain is not optional. These maps are sampled in world space and the sea runs to the
    /// horizon, so the far half of every frame minifies them by orders of magnitude; without mips
    /// that is a field of sparkling noise exactly where the water should be calmest.
    IDirect3DTexture9* Upload(IDirect3DDevice9* dev, const Image& level0)
    {
        IDirect3DTexture9* tex = nullptr;
        if (FAILED(dev->CreateTexture(static_cast<UINT>(level0.w), static_cast<UINT>(level0.h),
                                      0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr))
            || !tex)
            return nullptr;

        const UINT levels = tex->GetLevelCount();
        Image cur = level0;
        for (UINT l = 0; l < levels; ++l)
        {
            D3DLOCKED_RECT lr{};
            if (SUCCEEDED(tex->LockRect(l, &lr, nullptr, 0)) && lr.pBits)
            {
                auto* dst = static_cast<uint8_t*>(lr.pBits);
                for (int y = 0; y < cur.h; ++y)
                    std::memcpy(dst + static_cast<size_t>(y) * lr.Pitch,
                                cur.px.data() + static_cast<size_t>(y) * cur.w,
                                static_cast<size_t>(cur.w) * sizeof(uint32_t));
                tex->UnlockRect(l);
            }
            if (l + 1 < levels) cur = Halve(cur);
        }
        return tex;
    }

    // ---- the maps ------------------------------------------------------------------------------

    /// Four channels of five-octave noise, each from its own seed.
    ///
    /// FOUR INDEPENDENT ONES, not one repeated, and that is the point of the map: the body tint, the
    /// foam threshold jitter and the glitter mask all read from it, and if two of them read the same
    /// pattern the eye finds the correlation -- foam appearing exactly where the water is darkest is
    /// a structure nobody put there and everybody sees.
    Image BuildFbm()
    {
        Image img(kBroadN, kBroadN);
        for (int y = 0; y < kBroadN; ++y)
        {
            const float v = (y + 0.5f) / kBroadN;
            for (int x = 0; x < kBroadN; ++x)
            {
                const float u = (x + 0.5f) / kBroadN;
                // The octave count and the base are held so the FINEST octave still spans four
                // texels. Past that the lattice is the texel grid and the map stops being noise --
                // it becomes hash, which no mip chain can filter back into anything.
                const float r = Fbm(u, v, 0.11f, 4, 2, 5, 0x1F3D5B79u);
                const float g = Fbm(u, v, 0.37f, 3, 2, 5, 0x7C4A2E11u);
                const float b = Fbm(u, v, 0.63f, 4, 2, 5, 0x2B9A61C3u);
                const float a = Fbm(u, v, 0.89f, 2, 2, 5, 0x53E17D05u);
                img.px[static_cast<size_t>(y) * kBroadN + x] = Argb(r, g, b, a);
            }
        }
        return img;
    }

    /// Turns a height field into the centred slope pair the pixel stage adds to the normal.
    ///
    /// The differences are taken WRAPPED, so the slope tiles as exactly as the height did -- a
    /// one-sided difference at the edge would leave a visible line along every tile boundary, which
    /// is the seam this whole construction exists to avoid.
    ///
    /// Rescaled to a measured RMS rather than to a constant, because the raw amplitude of a
    /// difference depends on the octave count and the resolution, and neither of those should be
    /// able to change how strong the bump looks. The strength slider decides that, alone.
    void HeightToSlope(const std::vector<float>& h, int n, float targetRms, Image& out)
    {
        std::vector<float> sx(h.size()), sy(h.size());
        double sum = 0.0;
        for (int y = 0; y < n; ++y)
        {
            const int ym = (y + n - 1) % n, yp = (y + 1) % n;
            for (int x = 0; x < n; ++x)
            {
                const int xm = (x + n - 1) % n, xp = (x + 1) % n;
                const size_t i = static_cast<size_t>(y) * n + x;
                const float dx = (h[static_cast<size_t>(y) * n + xp] - h[static_cast<size_t>(y) * n + xm]) * 0.5f;
                const float dy = (h[static_cast<size_t>(yp) * n + x] - h[static_cast<size_t>(ym) * n + x]) * 0.5f;
                sx[i] = dx; sy[i] = dy;
                sum += static_cast<double>(dx) * dx + static_cast<double>(dy) * dy;
            }
        }

        const float rms = static_cast<float>(std::sqrt(std::max(sum / (h.size() * 2.0), 1e-12)));
        const float k = targetRms / rms;
        for (size_t i = 0; i < h.size(); ++i)
            out.px[i] = Argb(0.5f + sx[i] * k, 0.5f + sy[i] * k, 0.0f, 1.0f);
    }

    /// One slice of the grain loop.
    ///
    /// The time axis is the noise's third dimension, which is what makes the animation a LOOP with
    /// no frame that steps: slice seven and slice zero are neighbours on the same periodic lattice,
    /// so the crossfade back to the start is the same crossfade as any other. A folder of authored
    /// frames cannot promise that, and the discontinuity where it fails is the one thing an eye
    /// picks out of moving water immediately.
    Image BuildGrain(int frame)
    {
        const float w = static_cast<float>(frame) / static_cast<float>(kShadeGrainFrames);
        std::vector<float> h(static_cast<size_t>(kFineN) * kFineN);
        for (int y = 0; y < kFineN; ++y)
        {
            const float v = (y + 0.5f) / kFineN;
            for (int x = 0; x < kFineN; ++x)
            {
                const float u = (x + 0.5f) / kFineN;
                h[static_cast<size_t>(y) * kFineN + x] = Fbm(u, v, w, 4, 2, 4, 0x4D3A77E9u);
            }
        }
        Image img(kFineN, kFineN);
        HeightToSlope(h, kFineN, 0.13f, img);
        return img;
    }

    /// The broad bump. Static, at a scale with no common measure with the grain's, so the two never
    /// line up into one pattern -- which is the only reason there are two of them.
    Image BuildBump()
    {
        std::vector<float> h(static_cast<size_t>(kBroadN) * kBroadN);
        for (int y = 0; y < kBroadN; ++y)
        {
            const float v = (y + 0.5f) / kBroadN;
            for (int x = 0; x < kBroadN; ++x)
            {
                const float u = (x + 0.5f) / kBroadN;
                h[static_cast<size_t>(y) * kBroadN + x] = Fbm(u, v, 0.29f, 3, 2, 5, 0x6B1F92D7u);
            }
        }
        Image img(kBroadN, kBroadN);
        HeightToSlope(h, kBroadN, 0.10f, img);
        return img;
    }

    /// The foam pattern: filaments and holes.
    ///
    /// Ridged, then stretched over its own measured range. Foam is read against fixed thresholds in
    /// the pixel stage -- one at 0.55 decides where the lace is -- and a pattern that happened to
    /// span 0.3 to 0.7 would put every one of those thresholds in a different place than intended.
    Image BuildChop()
    {
        std::vector<float> f(static_cast<size_t>(kBroadN) * kBroadN);
        float lo = 1e9f, hi = -1e9f;
        for (int y = 0; y < kBroadN; ++y)
        {
            const float v = (y + 0.5f) / kBroadN;
            for (int x = 0; x < kBroadN; ++x)
            {
                const float u = (x + 0.5f) / kBroadN;
                // Two ridged fields at scales a factor of three apart, multiplied. One alone is an
                // even web; the product punches holes in it, and holes are what tells foam from a
                // wash of white.
                const float a = RidgedFbm(u, v, 0.17f, 5, 2, 4, 0x11C2A5F3u);
                const float b = RidgedFbm(u, v, 0.71f, 15, 2, 3, 0x3E7B0D45u);
                const float n = a * (0.45f + 0.55f * b);
                f[static_cast<size_t>(y) * kBroadN + x] = n;
                lo = std::min(lo, n);
                hi = std::max(hi, n);
            }
        }

        const float k = 1.0f / std::max(hi - lo, 1e-4f);
        Image img(kBroadN, kBroadN);
        for (size_t i = 0; i < f.size(); ++i)
        {
            const float c = (f[i] - lo) * k;
            img.px[i] = Argb(c, c, c, 1.0f);
        }
        return img;
    }

    /// The whole caustic loop, four slices deep, one per channel.
    ///
    /// A caustic is where the wavy surface focuses parallel light, so it is a set of CURVES, not a
    /// spread of bright patches -- the same reason the foam pattern is ridged. Raised to a power to
    /// thin the curves down: the focus is narrow and the space between is dark, and a caustic that
    /// covers half the seabed reads as a stain.
    ///
    /// Normalised across ALL FOUR slices together, never each on its own. Per-slice normalisation
    /// would divide each by its own brightest texel, so a slice that happened to focus weakly would
    /// be scaled up to match one that focused hard -- and the loop would pulse in overall
    /// brightness, at the loop's period, which is exactly the artefact this packing exists to kill.
    Image BuildCaustic()
    {
        const int n = kBroadN;
        std::vector<float> f(static_cast<size_t>(n) * n * kShadeCausticSlices);
        float hi = 1e-4f;
        for (int s = 0; s < kShadeCausticSlices; ++s)
        {
            const float w = static_cast<float>(s) / static_cast<float>(kShadeCausticSlices);
            for (int y = 0; y < n; ++y)
            {
                const float v = (y + 0.5f) / n;
                for (int x = 0; x < n; ++x)
                {
                    const float u = (x + 0.5f) / n;
                    const float r = RidgedFbm(u, v, w, 5, 2, 3, 0x74E3B1A9u);
                    const float c = r * r * r * r;
                    f[(static_cast<size_t>(y) * n + x) * kShadeCausticSlices + s] = c;
                    hi = std::max(hi, c);
                }
            }
        }

        Image img(n, n);
        const float k = 1.0f / hi;
        for (size_t i = 0; i < img.px.size(); ++i)
            img.px[i] = Argb(f[i * 4 + 0] * k, f[i * 4 + 1] * k,
                             f[i * 4 + 2] * k, f[i * 4 + 3] * k);
        return img;
    }

    /// The horizon panorama, in azimuth across and elevation down.
    ///
    /// Deliberately near-grey and near-white: the pixel stage already has an analytic sky gradient
    /// carrying the COLOUR of the sky, and multiplies this over it. What the gradient cannot have is
    /// structure -- a smooth ramp reflected in water is a smooth ramp, and open water reflects a sky
    /// with cloud in it. So this carries the structure and nothing else, and the two multiply into a
    /// reflection that has both.
    ///
    /// The cloud is stretched along azimuth because cloud seen near the horizon is foreshortened
    /// into bands, and banding along the horizon is most of what a reflection actually shows.
    Image BuildSky()
    {
        Image img(kSkyW, kSkyH);
        for (int y = 0; y < kSkyH; ++y)
        {
            // v = 0 is overhead, v = 1 is the horizon: the same convention the pixel stage builds
            // its lookup with.
            const float v = (y + 0.5f) / kSkyH;

            // Brighter towards the horizon, where the air path is longest and the haze thickest.
            const float haze = 0.62f + 0.30f * v * v;
            // Cloud thins out into that haze rather than running into it as hard shapes.
            const float cloudFade = (1.0f - v * v) * 0.85f + 0.05f;

            for (int x = 0; x < kSkyW; ++x)
            {
                const float u = (x + 0.5f) / kSkyW;
                // Wrapping in u is not a choice here: azimuth closes on itself, and a seam would sit
                // in the reflection at one compass bearing, permanently.
                const float c = Fbm(u, v * 0.35f, 0.41f, 4, 2, 4, 0x2FA7C3B1u);
                const float band = Fbm(u, v * 0.14f, 0.77f, 2, 2, 3, 0x5C8B14E7u);
                const float lum = haze + ((c - 0.5f) * 0.55f + (band - 0.5f) * 0.30f) * cloudFade;
                // A touch cooler overhead and warmer at the horizon, which is what the tint it gets
                // multiplied by cannot express on its own.
                img.px[static_cast<size_t>(y) * kSkyW + x] =
                    Argb(lum * (0.94f + 0.10f * v), lum, lum * (1.04f - 0.08f * v), 1.0f);
            }
        }
        return img;
    }

    // ---- state ---------------------------------------------------------------------------------

    IDirect3DTexture9* g_fbm = nullptr;
    IDirect3DTexture9* g_bump = nullptr;
    IDirect3DTexture9* g_chop = nullptr;
    IDirect3DTexture9* g_sky = nullptr;
    IDirect3DTexture9* g_grain[kShadeGrainFrames] = {};
    IDirect3DTexture9* g_caustic = nullptr;
    bool  g_ready = false;
    // Retain identity while cached resources exist; a new device cannot reuse its address.
    auto* g_owner = new Microsoft::WRL::ComPtr<IDirect3DDevice9>;
    bool  g_failed = false;
    float g_buildMs = 0.0f;

    /// Stages the maps ride on. High and contiguous with the rest of the surface's own: the stock
    /// liquid pass configures 0 and 1 and nothing else, and these are written at the draw where a
    /// bind survives the deferred flush.
    constexpr unsigned kStageFbm     = 2;
    constexpr unsigned kStageGrainA  = 3;
    constexpr unsigned kStageGrainB  = 4;
    constexpr unsigned kStageChop    = 12;
    constexpr unsigned kStageSky     = 13;
    constexpr unsigned kStageCaustic = 14;
    // Slot 15 belongs to captured scene depth. The centered slope map owns 5.
    constexpr unsigned kStageBump    = 5;

    /// Managed textures survive Reset, but never replacement of their creating device.
    void ReleaseAll()
    {
        auto drop = [](IDirect3DTexture9*& t) { if (t) t->Release(); t = nullptr; };
        drop(g_fbm); drop(g_bump); drop(g_chop); drop(g_sky); drop(g_caustic);
        for (IDirect3DTexture9*& t : g_grain) drop(t);
    }
}

namespace wxl::water
{
    bool NoiseEnsure(void* device)
    {
        if (!device) return false;
        auto* dev = static_cast<IDirect3DDevice9*>(device);
        if (g_owner->Get() != dev)
        {
            ReleaseAll();
            g_ready = g_failed = false;
            *g_owner = dev;
        }
        if (g_ready) return true;
        if (g_failed) return false;
        const auto t0 = std::chrono::steady_clock::now();

        g_fbm     = Upload(dev, BuildFbm());
        g_bump    = Upload(dev, BuildBump());
        g_chop    = Upload(dev, BuildChop());
        g_sky     = Upload(dev, BuildSky());
        g_caustic = Upload(dev, BuildCaustic());
        for (int i = 0; i < kShadeGrainFrames; ++i) g_grain[i] = Upload(dev, BuildGrain(i));

        bool all = g_fbm && g_bump && g_chop && g_sky && g_caustic;
        for (IDirect3DTexture9* t : g_grain) all = all && t != nullptr;

        g_buildMs = std::chrono::duration<float, std::milli>(
                        std::chrono::steady_clock::now() - t0).count();

        if (!all)
        {
            // ALL OR NOTHING, because a half-built set is worse than none: the effects are gated on
            // this one answer, and a stage left unbound is sampled at full strength from whatever
            // the previous pass happened to leave there.
            ReleaseAll();
            g_failed = true;
            WLOG_WARN("water: the shading maps could not be created -- the seven texture effects "
                      "stay off and the surface shades from geometry alone");
            return false;
        }

        g_ready = true;
        WLOG_INFO("water: shading maps built in %.1f ms -- noise %dpx, bump %dpx, foam %dpx, "
                  "sky %dx%d, grain %d frames at %dpx, caustics %d slices packed at %dpx",
                  g_buildMs, kBroadN, kBroadN, kBroadN, kSkyW, kSkyH,
                  kShadeGrainFrames, kFineN, kShadeCausticSlices, kBroadN);
        return true;
    }

    bool  NoiseReady()   { return g_ready; }
    float NoiseBuildMs() { return g_buildMs; }

    void NoiseBind(void* device, int grainFrame)
    {
        auto* dev = static_cast<IDirect3DDevice9*>(device);
        if (!dev || !g_ready || g_owner->Get() != dev) return;

        const int gf = ((grainFrame % kShadeGrainFrames) + kShadeGrainFrames) % kShadeGrainFrames;

        dev->SetTexture(kStageFbm, g_fbm);
        dev->SetTexture(kStageGrainA, g_grain[gf]);
        dev->SetTexture(kStageGrainB, g_grain[(gf + 1) % kShadeGrainFrames]);
        dev->SetTexture(kStageChop, g_chop);
        dev->SetTexture(kStageSky, g_sky);
        dev->SetTexture(kStageCaustic, g_caustic);
        dev->SetTexture(kStageBump, g_bump);
    }
}
