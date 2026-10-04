#include "wxl/RenderDefaults.hpp"
#include "NormalSamplerState.hpp"
#include "TextureState.hpp"
#include "ConstantState.hpp"
#include "PoolTransform.hpp"
#include "Performance.hpp"
// The takeover: our shader pair and our constants on the client's liquid draw.
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
// WHAT IS TAKEN OVER AND WHAT IS LEFT ALONE
//   The client keeps issuing the draw: it culls, it walks the visible instances, it sets the streams.
//   Everything about WHAT is drawn is ours -- the shape, the shading, the volume -- and none of the
//   client's own water model survives past the vertex declaration.
//
//   Leaving the issue with the client is not deference. That call site is where visibility, the
//   chunk cache and the batch descriptor already agree with one another; replacing it would mean
//   rebuilding all three to arrive at the same list of chunks.
//
// A DEVICE SetShader FROM HERE WOULD NOT STICK. The bind does not reach the device directly: it
// writes wrapper pointers into a GxState cache, and the device call happens later at the deferred
// flush, which re-applies whatever the cache holds. So our shaders go into that same cache, wrapped
// in the minimal object the flush knows how to read.

#include "wxl-experimental-water/render/Surface.hpp"
#include "wxl-experimental-water/sea/Clock.hpp"
#include "wxl-experimental-water/mesh/Lattice.hpp"
#include "wxl-experimental-water/render/Noise.hpp"
#include "wxl-experimental-water/sea/Ocean.hpp"
#include "wxl-experimental-water/render/OceanTexture.hpp"
#include "wxl-experimental-water/render/Refraction.hpp"
#include "wxl-experimental-water/sea/Sea.hpp"
#include "wxl-experimental-water/sea/Shore.hpp"
#include "wxl-experimental-water/panel/Switches.hpp"

#include "config.hpp"
#include "common/Log.hpp"
#include "engine/hook/Hook.hpp"
#include "engine/hook/Registry.hpp"
#include "engine/events/Event.hpp"
#include "game/Gx.hpp"
#include "game/Camera.hpp"
#include "runtime/Extensions.hpp"
#include "wxl/M2DrawApi.h"
#include "wxl/RenderControlApi.h"
#include "wxl/InteriorApi.h"
#include "wxl-experimental-water/world/NativeViewerLiquid.hpp"
#include "wxl-experimental-water/world/VWaterPass.hpp"
#include "wxl-experimental-water/world/NativeCelestialSource.hpp"
#include "offsets/engine/Gx.hpp"
#include "offsets/engine/Liquid.hpp"
#include "offsets/engine/Shader.hpp"
#include "offsets/game/ADT.hpp"

#include <windows.h>
#include <d3d9.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdint>
#include <cstring>

// After windows.h, not before: fxc declares the bytecode as BYTE and has no headers of its own.
#include "wxl-experimental-water/shaders/SurfaceVs.h"
#include "wxl-experimental-water/shaders/SurfacePs.h"
#include "wxl-experimental-water/shaders/SurfaceProbePs.h"
#include "wxl-experimental-water/shaders/SurfaceProbeVs.h"

namespace
{
    namespace liq   = wxl::offsets::engine::liquid;
    namespace gxoff = wxl::offsets::engine::gx;
    namespace sh    = wxl::offsets::engine::shader;
    namespace gx    = wxl::game::gx;
    namespace w     = wxl::water;
    /// Named without its namespace because every constant this file uploads is gated on
    /// one, and `w::Sw::` in front of two dozen of them is noise rather than clarity.
    using w::Sw;

    // Our constants start above the blocks the client uploads for every liquid draw (VS c0..c45,
    // PS c0..c5), so both sets coexist and the matrices and fog colour can be read rather than
    // recomputed.
    constexpr int kVsSeaReg    = 46;   // (pinch ceiling, full depth, coupling, rise normaliser)
    constexpr int kVsCameraReg = 47;   // (camera xyz, detail master gate)
    /// Above the wave table at c48..c63. The stock block runs to c45 and the client refills all of
    /// it for its own draw, so our constants have to sit clear of it entirely.
    constexpr int kVsDetailReg = 64;   // 1/patch of each of the four cascades
    constexpr int kVsLambdaReg = 65;   // representative wavelength of each, for the portability fade
    constexpr int kVsFilterReg = 66;   // how much of the tent reconstruction the geometry fetch uses
    constexpr int kVsShoreReg  = 67;   // c67 depth ladder, c68 shaping -- see Shore.hlsli
    constexpr int kVsWaveReg   = 48;   // 8 components x 2 float4
    constexpr int kPsLookReg   = 6;    // eight float4, see the pixel shader
    /// The same four 1/patch values again, for the pixel stage's own shading fetches, then the
    /// gated foam gain beside them.
    // CLEAR OF c22..c25, which the terrain's height-blend owns. These registers persist across
    // draws -- the client refills only the low block -- so the water sitting on the terrain's
    // constants rewrote its layer weights on every frame with water in it, and the terrain's own
    // "already uploaded" cache then trusted values that were no longer its. The water moved.
    constexpr int kPsDetailUvReg = 34; // c34 uv scales, c35 controls
    constexpr int kPsDebugReg    = 31; // c31 master + column scale, c32/c33 the one-hot selector

    liq::MaterialWaterRenderFn g_origWaterRender = nullptr;
    liq::MaterialWaterRenderFn g_origWaterNoSpec = nullptr;
    liq::GeomGetBuffersFn      g_origChunkGeom   = nullptr;
    liq::GeomGetBuffersFn      g_origMeshGeom    = nullptr;
    liq::MaterialBankGetMaterialFn g_origGetMaterial = nullptr;

    /// Camera position for the instance currently being drawn, stashed on the way past.
    ///
    /// The geometry provider is where the substitution has to happen and it is handed no camera; the
    /// material draw is handed one and is too early to bind. The two calls are strictly nested around
    /// one instance, so carrying the value between them is exact rather than approximate.
    float g_camPos[3] = { 0.0f, 0.0f, 0.0f };

    /// The liquid material context of the draw in flight. Kept because the foam texture is the
    /// client's own -- slot 3 of the material's animated set -- and it is only reachable from here.
    void* g_settings = nullptr;
    bool g_inland = false;
    bool g_meshPool = false, g_poolTransformValid = false;
    w::PoolMatrix g_poolWorld{};

    // Animated textures belong to this material call. Restore an enclosing
    // material on nested calls and release the borrowed settings on return.
    struct MaterialSettingsScope
    {
        void* previous;
        bool previousInland;
        bool previousMesh, previousValid;
        w::PoolMatrix previousWorld;
        float previousCamera[3];
        bool nested;
        MaterialSettingsScope(void* settings, bool inland, const float* world)
            : previous(g_settings), previousInland(g_inland), previousMesh(g_meshPool),
              previousValid(g_poolTransformValid), previousWorld(g_poolWorld), nested(g_settings != nullptr)
        {
            for (int i=0;i<3;++i) previousCamera[i]=g_camPos[i];
            g_settings = settings;
            g_inland = inland;
            g_meshPool = false;
            g_poolTransformValid = w::PreparePoolTransform(world, g_poolWorld);
        }
        ~MaterialSettingsScope()
        {
            g_settings = previous;
            g_inland = previousInland;
            g_meshPool = previousMesh;
            g_poolTransformValid = previousValid;
            g_poolWorld = previousWorld;
            // Retain the outermost capture for the next frame's eye update.
            if (nested) for (int i=0;i<3;++i) g_camPos[i]=previousCamera[i];
        }
        MaterialSettingsScope(const MaterialSettingsScope&) = delete;
        MaterialSettingsScope& operator=(const MaterialSettingsScope&) = delete;
    };

    /// WHETHER WE ARE INSIDE A LIQUID MATERIAL DRAW, and therefore whether the draw interceptor
    /// should renew itself after each use.
    ///
    /// THE INTERCEPTOR IS A ONE-SHOT: hkDIP clears it before calling, by design, so that a nested
    /// draw issued from inside an interceptor recurses into the native path instead of looping. Arm
    /// it once per geometry-provider call, as this file did, and exactly ONE draw is wrapped.
    ///
    /// That is the whole of the missing-water fault, and it is not what any of the three earlier
    /// guesses said. A provider call is not one draw. Whatever the client issues after the first --
    /// a second material pass, another batch off the same buffers -- arrives with our render states
    /// gone, our vertex textures unbound and our sampler filtering replaced by the deferred flush.
    /// The geometry is still the refined one and the shader is still ours, so what comes out is a
    /// subdivided sheet displaced by a detail field that is no longer bound: FLAT WATER AT THE
    /// PLANE, sitting under the waves around it, which reads as a hole exactly the way it looked.
    ///
    /// It also explains why widening the culling margin never helped. A culled chunk is not drawn at
    /// all; this one is drawn, correctly, with half its state missing.
    ///
    /// Bounded by the material call rather than left permanently armed: outside one, a non-liquid
    /// draw would inherit water render states and the sea's textures.
    ///
    /// A DEPTH RATHER THAN A FLAG. The two material entry points are hooked separately and nothing
    /// guarantees one cannot be reached from inside the other; a bool would let the inner exit close
    /// the window while the outer pass still had draws to issue -- the same fault again, with a
    /// smaller footprint and no symptom to find it by.
    int  g_waterMaterialDepth = 0;
    /// Draws wrapped inside the current material call, and the most ever seen. Reported once,
    /// because "a provider call is one draw" was an assumption nothing had ever measured.
    int  g_drawsThisMaterial = 0;
    int  g_peakDrawsPerMaterial = 0;


    /// Sampler stage the foam texture rides on. Above everything the liquid pass configures.
    constexpr unsigned kFoamStage = 7;
    /// Owned centered slope map shared by fine and broad normal detail.
    constexpr unsigned kRippleStage = 5;

    // ---- HANDING THE TEXTURE STAGES BACK ---------------------------------------------------------
    //
    // EVERY STAGE BELOW IS BOUND BEHIND THE ENGINE'S BACK, and leaving it that way is what put our
    // animated maps onto the terrain.
    //
    // Binds go through a deferred cache: a draw states what it wants, and the flush only issues the
    // ones whose cached value CHANGED. Our shading maps cannot go that way -- they are ordinary D3D
    // textures, not engine texture objects -- so they are written straight to the device at the draw.
    // The cache is then wrong about those stages, and it does not know it: the next pass asks for the
    // texture the cache already believes is bound, the flush finds no change and issues nothing, and
    // that pass draws with OURS still on the sampler.
    //
    // The terrain is the visible victim because it claims stages 2 to 4, which is exactly where the
    // grain slices sit -- and the grain is an eight-frame loop that crossfades and wraps, so a ground
    // layer sampling it fades out over the cycle and snaps back at the seam. Nothing about it looks
    // like a texture-binding fault, which is why it reads as the ground breathing.
    //
    // Marking the slot dirty is the coherent repair: the flush re-applies the value the ENGINE has
    // for that stage, so the stage goes back to whatever the engine thought it had all along and
    // nothing has to be guessed or remembered.
    constexpr unsigned kRawBoundStages[] = {
        2, 3, 4, 5,           // fbm, grain slices and centered bump
        6,                    // the scene copy
        8, 9, 10, 11,         // the detail field's shading maps, one per cascade
        12, 13, 14, 15,       // chop, sky, caustics, scene depth
    };

    /// Sampler filtering and addressing are NOT part of that cache -- nothing re-applies them, which
    /// is why they have to be written at the draw and why they must also be put back by hand. The
    /// span covers every stage this pass writes, including the two that bind through the cache.
    constexpr unsigned kSamplerFirstStage = 0;
    constexpr unsigned kSamplerLastStage  = 15;
    constexpr D3DSAMPLERSTATETYPE kSavedSamplerStates[] = {
        D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER,
        D3DSAMP_ADDRESSU,  D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE,
    };
    constexpr int kSamplerStageSpan = static_cast<int>(kSamplerLastStage - kSamplerFirstStage + 1);
    constexpr int kSamplerStateCount =
        static_cast<int>(sizeof(kSavedSamplerStates) / sizeof(kSavedSamplerStates[0]));

    /// First of the two detail registers, c19..c20. See Surface.ps.hlsl.
    constexpr int kPsDetailReg = 19;
    /// First of the four texture-effect registers, c24..c27. See Surface.ps.hlsl.
    constexpr int kPsFxReg = 36;
    /// Stylisation c43/c45, lace c44/c46. See Surface.ps.hlsl.
    constexpr int kPsStyleReg = 43;

    void* g_vsWrapper = nullptr;
    void* g_psWrapper = nullptr;
    bool  g_shaderFailed = false;

    // ---- the shading maps -----------------------------------------------------------------------
    //
    // Six maps the pixel stage textures from, all of them ours and all of them computed. See
    // Noise.cpp for what each one is and why it is the shape it is; here there is only the loop
    // position, because two of the six are animated and the frame they are on is a property of the
    // frame being drawn rather than of the maps.
    float g_seqFrac = 0.0f;
    int   g_seqFrame = 0;
    /// Where the caustic loop stands, in slices, wrapped into [0, 4). A continuous phase rather
    /// than a frame index: the four slices live in one texel and the pixel stage blends between
    /// them, so there is nothing to step.
    float g_causticPhase = 0.0f;
    /// The second layer's, wrapped independently. See where it is computed.
    float g_causticPhaseB = 0.0f;

    /// Whether the maps an effect needs are actually in hand.
    ///
    /// A MISSING MAP HAS TO MEAN THE EFFECT IS OFF, not merely unbound. An unbound sampler reads
    /// whatever the previous pass left on that stage, and the pixel stage would sample it at full
    /// strength, because the strength only ever knew about the panel's switch. So the switch is
    /// ANDed with the maps here, and one shader keeps serving every combination.
    ///
    /// One answer covers all seven effects because the set is built all or nothing: there is no
    /// file to be missing, so the only failure is the device refusing, and it refuses for all of
    /// them at once. The layer gate rides in the same answer, so cutting it cuts the whole stage
    /// rather than merely stopping the build it already did.
    bool FxReady() { return w::On(w::Sw::ShadeMaps) && w::NoiseReady(); }

    w::SurfaceLook   g_look;
    w::FoamShape     g_foam;
    w::ShadeFx       g_fx;
    w::ShadeDebug    g_debug;

    /// First of the four foam registers, c15..c18. See Surface.ps.hlsl.
    constexpr int kPsFoamReg = 15;

    uint8_t g_vsWrapStorage[sh::kCgxShaderWrapBytes];
    uint8_t g_psWrapStorage[sh::kCgxShaderWrapBytes];

    /// Wraps a live D3D shader handle in the minimal object the GxState flush knows how to apply.
    ///
    /// The flush reads the created flag first: set, it binds the handle and never looks at the
    /// bytecode. That is the whole trick -- our shader is applied by the client's own flush, on the
    /// client's own schedule, with no device call of ours to be overwritten.
    void* MintWrapper(uint8_t* storage, void* handle, const void* bytecode, uint32_t byteLen)
    {
        std::memset(storage, 0, sh::kCgxShaderWrapBytes);
        *reinterpret_cast<void**>(storage + sh::kCgxShaderHandle)  = handle;
        *reinterpret_cast<uint32_t*>(storage + sh::kCgxShaderCreated) = 1;
        *reinterpret_cast<uint32_t*>(storage + sh::kCgxShaderByteLen) = byteLen;
        *reinterpret_cast<const void**>(storage + sh::kCgxShaderBytePtr) = bytecode;
        return storage;
    }

    /// Asks the driver, once, whether a texture can be sampled from the vertex stage.
    ///
    /// This decides the sea's architecture, and it cannot be settled by reasoning. A finite sum of
    /// plane waves is quasi-periodic whatever its size: eight trains or a hundred, the surface is a
    /// lattice, and the only escape is a continuum of wavenumbers -- which means a spectrum inverted
    /// by FFT into a displacement map. The map is cheap to build and useless unless the VERTEX stage
    /// can read it, because the lattice is built once when the client rebuilds its liquid batch and
    /// then kept; re-uploading it every frame to displace on the CPU would put a cost on every image
    /// that the current shader-side evaluation does not have.
    ///
    /// vs_3_0 makes that fetch optional, and the split has historically followed vendors rather than
    /// capability, so the answer is asked of this machine and written to the log.
    void ReportVertexTextureSupport(void* dev)
    {
        auto* device = static_cast<IDirect3DDevice9*>(dev);
        IDirect3D9* d3d = nullptr;
        if (!device || FAILED(device->GetDirect3D(&d3d)) || !d3d)
        {
            WLOG_WARN("water: vertex texture probe found no IDirect3D9");
            return;
        }

        D3DDEVICE_CREATION_PARAMETERS cp{};
        D3DDISPLAYMODE mode{};
        const bool queryReady = SUCCEEDED(device->GetCreationParameters(&cp)) &&
                                SUCCEEDED(d3d->GetAdapterDisplayMode(cp.AdapterOrdinal, &mode));

        D3DADAPTER_IDENTIFIER9 adapter{};
        if (queryReady && SUCCEEDED(d3d->GetAdapterIdentifier(cp.AdapterOrdinal, 0, &adapter)))
            WLOG_INFO("water-device: adapter=%s vendor=%04x device=%04x type=%u flags=%08x softwareVP=%u",
                adapter.Description, adapter.VendorId, adapter.DeviceId, static_cast<unsigned>(cp.DeviceType),
                cp.BehaviorFlags, static_cast<unsigned>(device->GetSoftwareVertexProcessing()));

        const auto supports = [&](unsigned fmt) {
            return queryReady && SUCCEEDED(d3d->CheckDeviceFormat(
                cp.AdapterOrdinal, cp.DeviceType, mode.Format, D3DUSAGE_QUERY_VERTEXTEXTURE,
                D3DRTYPE_TEXTURE, static_cast<D3DFORMAT>(fmt)));
        };

        // Half float first: two of them per axis is all a displacement map needs, and it halves the
        // bandwidth the vertex stage pays per fetch.
        WLOG_INFO("water: vertex texture fetch -- A16B16G16R16F %s, R32F %s, A32B32G32R32F %s",
                  supports(D3DFMT_A16B16G16R16F) ? "yes" : "no",
                  supports(D3DFMT_R32F)          ? "yes" : "no",
                  supports(D3DFMT_A32B32G32R32F) ? "yes" : "no");

        d3d->Release();
    }

    bool EnsureShaders()
    {
        auto* dev = static_cast<IDirect3DDevice9*>(gx::RawDevice());
        if (!dev) return false;
        static auto* owner = new Microsoft::WRL::ComPtr<IDirect3DDevice9>;
        if (owner->Get() != dev)
        {
            // These wrappers are ours, outside the native shader registry. Reset can
            // preserve shader handles; a replacement device cannot use them.
            if (g_vsWrapper) gx::Release(*reinterpret_cast<void**>(g_vsWrapStorage + sh::kCgxShaderHandle));
            if (g_psWrapper) gx::Release(*reinterpret_cast<void**>(g_psWrapStorage + sh::kCgxShaderHandle));
            g_vsWrapper = g_psWrapper = nullptr;
            std::memset(g_vsWrapStorage, 0, sizeof(g_vsWrapStorage));
            std::memset(g_psWrapStorage, 0, sizeof(g_psWrapStorage));
            g_shaderFailed = false;
            *owner = dev;
        }
        if (g_vsWrapper && g_psWrapper) return true;
        if (g_shaderFailed) return false;

        // Opt-in diagnostic, not a water preset. Both creation and wrapper metadata
        // must refer to the selected bytecode so deferred state uses the same shader.
        char probeValue[8] = {};
        GetEnvironmentVariableA("WXL_WATER_PIXEL_PROBE", probeValue, sizeof(probeValue));
        const bool pixelProbe = probeValue[0] == '1' && probeValue[1] == '\0';
        const void* pixelCode = pixelProbe ? static_cast<const void*>(kSurfaceProbePs)
                                          : static_cast<const void*>(kSurfacePs);
        const uint32_t pixelBytes = pixelProbe ? sizeof(kSurfaceProbePs) : sizeof(kSurfacePs);
        char vertexValue[8] = {};
        GetEnvironmentVariableA("WXL_WATER_VERTEX_PROBE", vertexValue, sizeof(vertexValue));
        // The minimal vertex output is only compatible with the flat diagnostic PS.
        const bool vertexProbe = pixelProbe && vertexValue[0] == '1' && vertexValue[1] == '\0';
        const void* vertexCode = vertexProbe ? static_cast<const void*>(kSurfaceProbeVs)
                                            : static_cast<const void*>(kSurfaceVs);
        const uint32_t vertexBytes = vertexProbe ? sizeof(kSurfaceProbeVs) : sizeof(kSurfaceVs);
        void* vs = nullptr;
        void* ps = nullptr;
        gx::Vtbl<long(__stdcall*)(void*, const void*, void**)>(dev, gxoff::vt::kCreateVertexShader)
            (dev, vertexCode, &vs);
        gx::Vtbl<long(__stdcall*)(void*, const void*, void**)>(dev, gxoff::vt::kCreatePixelShader)
            (dev, pixelCode, &ps);

        if (!vs || !ps)
        {
            gx::Release(vs);
            gx::Release(ps);
            g_shaderFailed = true;
            WLOG_WARN("water: the device refused the surface shaders -- stock water stands");
            return false;
        }

        g_vsWrapper = MintWrapper(g_vsWrapStorage, vs, vertexCode, vertexBytes);
        WLOG_INFO("water-vertex-probe-v1: mode=%s", vertexProbe ? "transform-only" : "retail-waves");
        g_psWrapper = MintWrapper(g_psWrapStorage, ps, pixelCode, pixelBytes);
        WLOG_INFO("water-pixel-probe-v1: mode=%s", pixelProbe ? "flat-diagnostic" : "retail");
        WLOG_INFO("water: surface shaders live");
        ReportVertexTextureSupport(dev);
        return true;
    }

    w::ReflectionLightPreview g_lightPreview;
    float g_sunDir[3] = { 0.45f, 0.30f, 0.84f };

    // Use the visible primary sun/moon, matching atmosphere and shafts. Native
    // material lighting is a fallback when the celestial billboard data is invalid.
    void ReadSunDir()
    {
        if (w::celestial::ReadDirection(g_sunDir))
        {
            static bool reported=false;
            if (!reported)
            {
                reported=true;
                WLOG_INFO("water: celestial direction shared with atmosphere (%.3f,%.3f,%.3f)",
                    g_sunDir[0],g_sunDir[1],g_sunDir[2]);
            }
            return;
        }
        const auto* vs = reinterpret_cast<const float*>(liq::kVsConstBlock);
        const float* s  = vs + 33 * 4;
        const float* r0 = vs + 5 * 4;
        const float* r1 = vs + 6 * 4;
        const float* r2 = vs + 7 * 4;

        float x = r0[0] * s[0] + r1[0] * s[1] + r2[0] * s[2];
        float y = r0[1] * s[0] + r1[1] * s[1] + r2[1] * s[2];
        float z = r0[2] * s[0] + r1[2] * s[1] + r2[2] * s[2];

        if (g_meshPool) {
            // Inverse modelview gives a model-local direction. Return it to world
            // space for the world-position surface shader (uniform scale cancels).
            const auto& m=g_poolWorld;
            const float wx=m[0]*x+m[4]*y+m[8]*z;
            const float wy=m[1]*x+m[5]*y+m[9]*z;
            const float wz=m[2]*x+m[6]*y+m[10]*z;
            x=wx; y=wy; z=wz;
        }
        // Left alone if the block has not been filled yet -- at that point the authored default is
        // still a better answer than a zero vector, which would put the sun nowhere and kill every
        // highlight on the surface.
        const float len = std::sqrt(x * x + y * y + z * z);
        if (!std::isfinite(len) || len < 1e-4f) return;

        g_sunDir[0] = x / len;
        g_sunDir[1] = y / len;
        g_sunDir[2] = z / len;

        static bool told = false;
        if (!told)
        {
            told = true;
            WLOG_INFO("water: sun taken from the world -- view (%.3f, %.3f, %.3f) -> world "
                      "(%.3f, %.3f, %.3f)", s[0], s[1], s[2],
                      g_sunDir[0], g_sunDir[1], g_sunDir[2]);
        }
    }

    /// Zone water and sky colours, as the day/night interpolation left them this frame.
    ///
    /// Read rather than authored so the sea belongs to the hour and the zone it is in: a palette of
    /// our own would be right at one time of day and wrong at every other, and would part company
    /// with the land it meets at the shoreline.
    void ReadPalette(float shallow[3], float deep[3], float skyLow[3], float skyHigh[3])
    {
        const auto* dn = reinterpret_cast<const uint8_t*>(liq::kDayNightInfo);
        // Not named `near`/`far`: windows.h still defines both as empty macros, and the variable
        // simply vanishes.
        const uint32_t closeRaw = *reinterpret_cast<const uint32_t*>(dn + (g_inland ? liq::kDnRiverCloseColor : liq::kDnOceanCloseColor));
        const uint32_t farRaw   = *reinterpret_cast<const uint32_t*>(dn + (g_inland ? liq::kDnRiverFarColor : liq::kDnOceanFarColor));

        // CImVector: 0xAARRGGBB.
        shallow[0] = ((closeRaw >> 16) & 0xFF) / 255.0f;
        shallow[1] = ((closeRaw >> 8)  & 0xFF) / 255.0f;
        shallow[2] = ( closeRaw        & 0xFF) / 255.0f;
        deep[0] = ((farRaw >> 16) & 0xFF) / 255.0f;
        deep[1] = ((farRaw >> 8)  & 0xFF) / 255.0f;
        deep[2] = ( farRaw        & 0xFF) / 255.0f;

        // Band order: 2..6 run sky top to horizon. The two ends are what the reflection gradient
        // needs, and taking them from the same table as the sky means the reflected horizon matches
        // the real one exactly where the two meet.
        const auto* bands = reinterpret_cast<const uint32_t*>(liq::kDnBandColors);
        const uint32_t zenith  = bands[2];
        const uint32_t horizon = bands[6];
        skyHigh[0] = ((zenith >> 16) & 0xFF) / 255.0f;   // BGRA in memory: byte 2 is red
        skyHigh[1] = ((zenith >> 8)  & 0xFF) / 255.0f;
        skyHigh[2] = ( zenith        & 0xFF) / 255.0f;
        skyLow[0] = ((horizon >> 16) & 0xFF) / 255.0f;
        skyLow[1] = ((horizon >> 8)  & 0xFF) / 255.0f;
        skyLow[2] = ( horizon        & 0xFF) / 255.0f;
    }

    void UploadConstants(const float* camPos, w::ConstantState<IDirect3DDevice9>& constants)
    {
        auto set = [&constants](int target, int start, const float* data, int count) {
            constants.Set(target, start, data, count);
        };
        const w::WaveFrame& frame = w::Frame();

        // The wave table, straight out of the frame. WaveComponent is laid out as the two float4 the
        // vertex stage reads, so this is a copy and not a repack -- which is exactly the property
        // that keeps the CPU and the GPU on one sea.
        set(0, kVsWaveReg, reinterpret_cast<const float*>(frame.components), w::kMaxComponents * 2);

        // How far the shallows are allowed to sharpen the sea before it turns inside out.
        //
        // The trains share ONE cusp budget, so the sum of their pinches is what must stay under 1
        // -- and the reciprocal of that sum is exactly how much headroom shoaling has left. Handing
        // the ceiling to the vertex stage rather than a constant keeps the two honest with each
        // other when the weather moves the budget. Every train counts: the table is all support
        // now, and none of it sits at the cusp by design.
        float pinch = 0.0f;
        for (int i = 0; i < w::kMaxComponents; ++i)
            pinch += frame.components[i].qa * frame.components[i].k;
        const float pinchCeiling = (pinch > 1e-3f) ? (1.0f / pinch) : 1.0f;

        ReadSunDir();
        const float nativeSun[3]{g_sunDir[0],g_sunDir[1],g_sunDir[2]};
        // Override only this draw's constants. Preserve the native fallback cache.
        if (g_lightPreview.enabled)
        {
            const float radians = 0.01745329252f;
            const float bearing = std::atan2(g_sunDir[1], g_sunDir[0]) +
                g_lightPreview.bearingOffset * radians;
            const float elevation = g_lightPreview.elevation * radians;
            g_sunDir[0] = std::cos(bearing) * std::cos(elevation);
            g_sunDir[1] = std::sin(bearing) * std::cos(elevation);
            g_sunDir[2] = std::sin(elevation);
        }

        // Nothing is sent for fog. The vertex stage reads c4 of the stock block, which the client has
        // already filled for its own liquid draw -- the same arrangement as the matrices at c0..c8.
        //
        // The maximum displacement is not sent either: no shader stage reads it. It is a CPU concern
        // only, where it widens the bounds the client culls liquid against -- see Lattice.cpp.
        //
        // Chop is not sent any more either: it is baked into the displacement textures on the CPU,
        // where the cusp clamp and the foam's fold can see it. The coupling still goes down, and it
        // is the ONE gain the vertex stage is left to apply -- its ceiling is divided into the
        // field's amplitude budget when the spectrum is built, which is what makes it safe there.
        // Floored at zero where it becomes a shader input: a negative coupling would turn the
        // redistribution upside down AND slip under the gain ceiling the budget accounts for,
        // which floors it the same way. The last channel is the coupling's yardstick -- half over
        // the sea's own maximum rise, so the crowding term reads the trains' rise as a signed
        // fraction spanning [-1/2, 1/2] whatever the weather has made of the amplitude.
        const float coupling = (w::OceanSettings().waveCoupling > 0.0f)
                                   ? w::OceanSettings().waveCoupling : 0.0f;
        const float riseNorm = 0.5f / ((frame.maxDisplacement > 1.0f) ? frame.maxDisplacement
                                                                      : 1.0f);
        const float sea[4] = { pinchCeiling, g_meshPool ? 2.0f : (g_inland ? 1.0f : 0.0f), coupling, riseNorm };
        set(0, kVsSeaReg, sea, 1);
        if (g_meshPool) set(0, 69, g_poolWorld.data(), 4);

        // THE SHORE LADDER. Every neutral here is a value that makes its term unreachable rather
        // than zero, which is the rule the whole switch registry runs on -- and getting it wrong in
        // this particular block is invisible, because a shore term neutralised the wrong way looks
        // like a shore setting somebody chose.
        //
        // The DEAD ZONE has no switch and is never neutralised: a crest of any height at the
        // waterline cuts straight through the beach behind it, and no combination of switches is
        // allowed to produce that.
        {
            const w::ShoreProfile& sp = w::ShoreSettings();

            // The cap folded into a scale per unit of depth, so the vertex stage never needs the
            // sea's own height. Measured against the TOTAL rise -- trains and field -- because what
            // has to fit inside the water is the surface, not one layer of it.
            const float totalRise = frame.maxDisplacement + w::OceanMaxRise();
            // Off, the cap is put OUT OF REACH rather than at zero: zero is a cap of nothing, which
            // would flatten the entire sea. A huge scale per unit of depth means even a shallow
            // column allows more than the wave was ever going to ask for.
            const float capScale = w::On(Sw::DepthLimit)
                                       ? (sp.breakerIndex / ((totalRise > 1e-3f) ? totalRise : 1.0f))
                                       : 1e9f;

            // The scale is folded in HERE rather than sent as a fifth number, because the vertex
            // stage has no use for the shape and the placement separately -- it only ever reads the
            // three depths. Same floor as ShoreAt(), or the two sides would disagree about what a
            // scale of zero means.
            const float sc = (sp.scale > 0.01f) ? sp.scale : 0.01f;
            const float deadStart = sp.deadStart * sc;
            const float deadFull  = sp.deadFull * sc;

            const float shoreA[4] = { deadStart, deadFull, sp.feelDepth * sc, capScale };

            // Shoaling neutralises to a rise and a pinch of zero -- both are ADDED to one, so zero
            // is genuinely "the bottom is not felt" and not "the bottom removes everything".
            const float rise  = w::On(Sw::Shoaling) ? sp.shoalRise : 0.0f;
            const float pinch = w::On(Sw::Shoaling) ? sp.pinchRise : 0.0f;
            // The detail field's own dead band, ending further out than the swell's by chopLead.
            const float chopEnd = sp.deadStart
                                + (sp.deadFull - sp.deadStart)
                                  * ((sp.chopLead > 0.0f) ? sp.chopLead : 0.0f);
            const float surfFoam = w::On(Sw::ShoreFoam) ? sp.surfFoam : 0.0f;

            const float shoreB[4] = { rise, pinch, chopEnd, surfFoam };
            set(0, kVsShoreReg, shoreA, 1);
            set(0, kVsShoreReg + 1, shoreB, 1);
        }

        // The camera's spare channel is the detail layer's MASTER SWITCH, one or zero, and nothing
        // in between -- the strength slider lives in the CPU normalisation now. A binary gate is
        // the one multiplier that needs no accounting, and it is the known-good fallback: off, the
        // sea is the eight trains alone, and any artefact that survives was never this layer's.
        const bool detailOn = !g_inland && w::On(Sw::DetailGeometry);
        const float cam[4] = { camPos[0], camPos[1], camPos[2],
                               detailOn ? 1.0f : 0.0f };
        set(0, kVsCameraReg, cam, 1);

        // The patch sizes are asked for rather than read off the tuning: they are DERIVED from the
        // band each cascade ended up with, and the derivation is clamped by the grid's resolution.
        // Recomputing that here would be a second copy of the rule and eventually a different one.
        // The same four values go to both stages, because both sample the same grids: the vertex
        // stage the geometry maps, the pixel stage the shading maps.
        const w::OceanTuning& ocean = w::OceanSettings();
        float detailScale[4];
        float detailLambda[4];
        for (int ci = 0; ci < w::kOceanCascades; ++ci)
        {
            detailScale[ci] = 1.0f / ((w::OceanPatch(ci) > 1.0f) ? w::OceanPatch(ci) : 1.0f);
            // The band's geometric mean, the same yardstick every budget in the field uses. The
            // vertex stage grades this against each vertex's own lattice spacing: a cascade the
            // mesh cannot restitute leaves the geometry there and lives on in the shading.
            const w::OceanCascadeReport& rep = w::OceanReport(ci);
            detailLambda[ci] = std::sqrt((rep.bandLong > 0.0f && rep.bandShort > 0.0f)
                                             ? rep.bandLong * rep.bandShort : 0.0f);
        }
        set(0, kVsDetailReg, detailScale, 1);
        set(0, kVsLambdaReg, detailLambda, 1);

        // How much of the tent the geometry fetch applies. NEUTRAL IS ZERO AND ZERO IS THE FAULT:
        // a vertex fetch has no filtering, so switching this off returns the field to the
        // hardware's box reconstruction -- flat plateaus one cell across, which is what the coarse
        // cascades looked like before the tent existed. Off is the comparison, not the fallback.
        const float detailFilter[4] = { w::On(Sw::DetailFilter) ? 1.0f : 0.0f,
            static_cast<float>(w::kOceanN), 1.0f / w::kOceanN, 0.0f };
        set(0, kVsFilterReg, detailFilter, 1);

        const float detailCtl[2][4] = {
            { detailScale[0], detailScale[1], detailScale[2], detailScale[3] },
            { !g_inland && w::On(Sw::OffshoreFoam) ? ocean.foamGain : 0.0f, g_inland ? 1.0f : 0.0f, 0.0f, 0.0f },
        };
        set(4, kPsDetailUvReg, &detailCtl[0][0], 2);

        // THE DEBUG VIEW, as a one-hot vector rather than as a mode number.
        //
        // The pixel stage selects with a dot product, so the choice costs no branch there -- and the
        // cost of that is here: the selection has to be exclusive by construction, because two bits
        // set would ADD two unrelated quantities and produce a picture that looks like a reading.
        // Built by clearing and setting exactly one, which is the only way it can be wrong loudly.
        {
            const w::ShadeDebug& dbg = w::Debug();
            const bool on = dbg.view > w::ShadeDebug::Off && dbg.view < w::ShadeDebug::Count;

            float sel[2][4] = {};
            if (on)
            {
                const int slot = dbg.view - 1;   // Off is not a slot
                if (slot < 4) sel[0][slot] = 1.0f;
                else          sel[1][slot - 4] = 1.0f;
            }

            // The column is divided by the range that fills the ramp, so a map whose water is four
            // units deep and one whose water is four hundred both read. Floored, because a range of
            // zero is a division that saturates the whole map to the top of the ramp.
            const float range = (dbg.columnRange > 0.1f) ? dbg.columnRange : 0.1f;
            static const bool reflectionDiagnostic = [] {
                char value[8]{};
                GetEnvironmentVariableA("WXL_WATER_REFLECTION_DIAGNOSTIC", value, sizeof(value));
                return value[0] == '1';
            }();
            const float dbgA[4] = { on ? 1.0f : 0.0f, 1.0f / range,
                reflectionDiagnostic ? 1.0f : 0.0f, 0.0f };

            set(4, kPsDebugReg, dbgA, 1);
            set(4, kPsDebugReg + 1, &sel[0][0], 2);
        }

        float shallow[3], deep[3], skyLow[3], skyHigh[3];
        ReadPalette(shallow, deep, skyLow, skyHigh);
        static const WXL_InteriorApi* interior = nullptr;
        if (!interior)
            interior = static_cast<const WXL_InteriorApi*>(
                wxl::runtime::extensions::GetInterface("wxl.interior", WXL_INTERIOR_API_VERSION));
        const bool enclosed = WxlViewerEnclosed(interior);
        // Match atmosphere's native sky gate as well as WMO classification.
        // A sky-disabled scene must not acquire our synthetic panorama or source.
        const bool nativeSky = *reinterpret_cast<const uint32_t*>(
            wxl::offsets::engine::sky::kSkyDrawGate) != 0;
        const bool suppressOutdoor = enclosed || !nativeSky;
        const float outdoorLight = suppressOutdoor ? 0.0f : 1.0f;
        // Bounded automatic evidence for the missing reflected source. No render
        // state or user preference changes; compare both existing camera inputs.
        static unsigned diagnosticReports=0;
        static ULONGLONG nextDiagnostic=0;
        const auto diagnosticNow=GetTickCount64();
        if (diagnosticReports<8 && diagnosticNow>=nextDiagnostic)
        {
            nextDiagnostic=diagnosticNow+3000;
            ++diagnosticReports;
            float nativeCamera[3]{};
            wxl::game::camera::GetPosition(nativeCamera);
            const auto& style=w::Fx();
            WLOG_INFO("water-reflection-input: materialCamera=(%.3f,%.3f,%.3f) nativeCamera=(%.3f,%.3f,%.3f) sun=(%.3f,%.3f,%.3f) intensity=%.3f power=%.3f roughness=%.3f fresnel=%.4f outdoor=%.0f inland=%u pool=%u sky=%.3f repaint=%.3f cartoon=%.3f",
                camPos[0],camPos[1],camPos[2],nativeCamera[0],nativeCamera[1],nativeCamera[2],
                g_sunDir[0],g_sunDir[1],g_sunDir[2],g_look.sunIntensity,g_look.specularPower,
                g_look.roughness,g_look.fresnelF0,outdoorLight,g_inland,g_meshPool,
                style.skyStrength,style.styleRepaint,style.styleCartoon);
            const auto* nativeVs=reinterpret_cast<const float*>(liq::kVsConstBlock);
            WLOG_INFO("water-reflection-input: modelview rows=(%.4f,%.4f,%.4f,%.4f)/(%.4f,%.4f,%.4f,%.4f)/(%.4f,%.4f,%.4f,%.4f) fog=(%.4f,%.4f,%.4f,%.4f)",
                nativeVs[20],nativeVs[21],nativeVs[22],nativeVs[23],
                nativeVs[24],nativeVs[25],nativeVs[26],nativeVs[27],
                nativeVs[28],nativeVs[29],nativeVs[30],nativeVs[31],
                nativeVs[16],nativeVs[17],nativeVs[18],nativeVs[19]);
        }
        // No invented sky behind a cave ceiling. Screen-space reflection/refraction
        // remain live, and retain the captured scene's own lighting.
        for (int channel = 0; channel < 3; ++channel)
        {
            skyLow[channel] *= outdoorLight;
            skyHigh[channel] *= outdoorLight;
        }
        static int reportedEnvironment = -1;
        const int environmentState = (enclosed ? 1 : 0) | (nativeSky ? 2 : 0);
        if (reportedEnvironment != environmentState)
        {
            reportedEnvironment = environmentState;
            WLOG_INFO("water: outdoor lighting %s (enclosed=%u nativeSky=%u interior service=%s)",
                suppressOutdoor ? "suppressed" : "enabled", enclosed, nativeSky,
                interior ? "available" : "absent");
        }

        // A half texel, so the sample lands on the pixel the surface is actually covering. D3D9
        // addresses texels by their corner, and without the shift the whole refraction is offset by
        // half a pixel toward the origin -- which reads as a faint edge along every silhouette.
        float sceneW = 0.0f, sceneH = 0.0f;
        w::SceneSize(sceneW, sceneH);
        const float sceneLive = w::SceneCopy() ? 1.0f : 0.0f;
        const float texelW = (sceneW > 0.0f) ? (1.0f / sceneW) : 0.0f;
        const float texelH = (sceneH > 0.0f) ? (1.0f / sceneH) : 0.0f;

        // Wrapped before it reaches a float. The pixel stage runs its ripple off this, and a raw
        // second count loses the low bits it needs within a session -- the detail would coarsen and
        // then stop moving, hours in, with nothing to point at.
        const double kDetailPeriod = 900.0;
        const double wrapped = w::Now() - std::floor(w::Now() / kDetailPeriod) * kDetailPeriod;

        // EVERY SWITCH IS THE SAME MECHANISM: the constant goes out at its NEUTRAL value and the term
        // in the pixel stage multiplies out. No permutation, no branch, no skipped bind -- so any
        // combination of switches is a combination that has been tested, because there is only ever
        // one shader.
        //
        // Neutral is not always zero, and each one below says what it is where it is applied. A
        // strength neutralises at zero; a foam threshold neutralises by going OUT OF REACH, because
        // zero is the value at which every crest breaks.
        const float ext = w::On(Sw::Extinction) ? 1.0f : 0.0f;
        const float look[9][4] = {
            { shallow[0], shallow[1], shallow[2],
              w::On(Sw::Bottom) ? g_look.bottomBrightness : 0.0f },
            { deep[0],    deep[1],    deep[2],    static_cast<float>(wrapped) },
            // Zero attenuation per unit is water that takes nothing out of the light on the way
            // through -- which is the honest "no extinction", and reads as coloured glass.
            { g_look.extinction[0] * ext, g_look.extinction[1] * ext, g_look.extinction[2] * ext,
              g_look.maxPath },
            { g_sunDir[0], g_sunDir[1], g_sunDir[2], g_look.specularPower },
            { outdoorLight, 0.97f * outdoorLight, 0.90f * outdoorLight, g_look.sunIntensity * outdoorLight },
            { camPos[0], camPos[1], camPos[2], g_look.fresnelF0 },
            { skyLow[0], skyLow[1], skyLow[2],
              w::On(Sw::Foam) ? g_look.foam : 0.0f },
            { skyHigh[0], skyHigh[1], skyHigh[2], g_look.roughness },
            // The scene copy's live flag IS the refraction switch: the pixel stage already reads it
            // to decide whether there is anything behind the water to bend.
            { (w::Eye().submerged || w::ReadViewerLiquid().Submerged()) ? 1.0f : 0.0f,
              w::On(Sw::Refraction) ? sceneLive : 0.0f, texelW, texelH },
        };
        set(4, kPsLookReg, &look[0][0], 9);

        // The glitter's switch rides its existing strength: zeroed here, the sparkle term in the
        // pixel stage multiplies out, exactly like every other effect gate below.
        //
        // And it is held to the noise map as well, because the sparkle draws its seed from that
        // texture. Leaving it live with the map absent would rest on what an UNBOUND sampler
        // returns, which is undefined -- black on most drivers and white on some, and the second
        // one is a sea covered in sparks. Off is a decision; undefined is not.
        const float detail[3][4] = {
            { g_look.rippleScale[0], g_look.rippleScale[1],
              (w::On(Sw::Ripple) && FxReady()) ? g_look.rippleStrength : 0.0f,
              w::On(Sw::Sss) ? g_look.glowStrength : 0.0f },
            { g_look.glow[0], g_look.glow[1], g_look.glow[2],
              (w::On(Sw::Glitter) && FxReady()) ? g_look.glintStrength : 0.0f },
            // Normalised against the sea's own excursion, so a trough means the same fraction of the
            // wave in a calm as in a gale -- otherwise the shading would come and go with the weather
            // for a reason that has nothing to do with the light.
            { w::On(Sw::TroughShade) ? g_look.troughShade : 0.0f,
              1.0f / ((frame.maxDisplacement > 1.0f) ? frame.maxDisplacement : 1.0f),
              suppressOutdoor ? 1.0f : (w::On(Sw::NightFloor) ? g_look.nightFloor : 0.0f), 0.0f },
        };
        set(4, kPsDetailReg, &detail[0][0], 3);

        // OUT OF REACH RATHER THAN AT ZERO. The threshold is what a fold has to exceed for a crest
        // to count as breaking, so zeroing it is the opposite of turning whitecaps off -- it turns
        // the entire sea white. Ten is past anything the fold reaches.
        const float breakThr = w::On(Sw::CrestFoam) ? g_foam.breakThreshold : 10.0f;
        const float foam[4][4] = {
            { breakThr, g_foam.breakSharpness,
              g_foam.shoreSlopeMin,  g_foam.shoreSlopeGain },
            { g_foam.shoreDepthFade, g_foam.maskFloor,
              g_foam.maskContrast,
              w::On(Sw::ShoreFoam) ? g_foam.shoreStrength : 0.0f },
            { g_foam.scale[0], g_foam.scale[1], g_foam.scale[2], g_foam.warp },
            { g_foam.density, 0.0f, 0.0f, 0.0f },
        };
        set(4, kPsFoamReg, &foam[0][0], 4);

        // Every strength multiplied by its own switch on the way in: a zero IS the effect off, so
        // one shader serves every combination and judging a contribution costs a click.
        const w::ShadeFx& fx = w::Fx();
        // AND the switch with the maps. Nothing is bound when the set failed to build, which leaves
        // whatever was last on those stages -- and a strength that only knew about the panel would
        // then sample it at full force. SSS is not in this list on purpose: it reads the crest
        // compression the field already computes and needs no texture of its own.
        const bool maps = FxReady();
        const float fxc[7][4] = {
            { (w::On(Sw::AnimNormals) && maps) ? fx.animStrength : 0.0f,
              (w::On(Sw::Fbm) && maps)         ? fx.fbmStrength : 0.0f,
              (w::On(Sw::FoamTex) && maps)     ? fx.foamTexStrength : 0.0f,
              (w::On(Sw::Sky) && maps)         ? fx.skyStrength * outdoorLight : 0.0f },
            { g_causticPhase,
              w::On(Sw::Sss) ? fx.sssBoost : 0.0f,
              (w::On(Sw::Caustics) && maps)    ? fx.causticStrength : 0.0f,
              g_seqFrac },
            { fx.animScaleFine, fx.animScaleBroad, fx.fbmScale, fx.foamTexScale },
            { fx.causticScale, fx.causticScale2, fx.sssLobe, fx.sssThickness },
            { fx.causticSunPower, fx.causticDepthFade, fx.sssSunFalloff,
              maps ? fx.detileWarp : 0.0f },
            // The glitter's shaping numbers. Its STRENGTH is gated elsewhere, with the noise map;
            // these only decide what a spark looks like when there is one.
            { g_look.glintScale, g_look.glintThreshold,
              g_look.glintSharpness, g_look.glintFadeStart },
            // The caustic's surface web rides the same map switch as its floor web: both read the
            // same texture, so neither may be live when that texture is not.
            { (w::On(Sw::Caustics) && maps) ? fx.causticSurface : 0.0f,
              fx.causticViewBias, fx.causticSurfaceFade, g_causticPhaseB },
        };
        set(4, kPsFxReg, &fxc[0][0], 7);

        // The lace crossfade, read off the client's own fixed-function blend: the stock ocean
        // crossfades its two bound frames through the texture factor's alpha, so this is the very
        // phase the material is animating at -- no clock of ours to drift against it. If that
        // factor turns out not to be in play the byte sits constant and the sheet simply rides one
        // frame, which is the stepping behaviour and not a fault.
        float laceBlend = 0.5f;
        if (gx::Device9 dev = gx::Device())
        {
            DWORD tf = 0xFFFFFFFFu;
            static_cast<IDirect3DDevice9*>(dev.raw())->GetRenderState(D3DRS_TEXTUREFACTOR, &tf);
            laceBlend = static_cast<float>((tf >> 24) & 0xFFu) / 255.0f;
        }

        // The stylisation and the lace, past the effect block. Every channel is neutral at zero,
        // so these registers going out unconditionally IS the switch mechanism -- same bargain
        // as everything above. The lace deliberately has no map gate: its textures are the ones
        // the client's own ocean material bound on s0/s1 before this draw, not ours to check.
        const float style[4][4] = {
            { fx.styleCartoon, fx.clarityFloor, fx.styleRepaint, fx.styleGrazing },
            { g_inland ? 0.0f : fx.laceStrength, fx.laceScale, fx.laceWobble, fx.laceCrestBoost },
            { fx.styleSunDiffuse, fx.refractRetail, fx.refractThickness, fx.refractAmount },
            { laceBlend, fx.laceDepthFade, fx.laceDistFade, 0.0f },
        };
        set(4, kPsStyleReg, &style[0][0], 4);
        for (int i=0;i<3;++i) g_sunDir[i]=nativeSun[i];
    }

    /// Substitutes our pair into the shader cache for the draw about to be issued.
    void BindSurface()
    {
        w::perf::Scope timing(w::perf::Stage::Bind);
        if (!EnsureShaders()) return;

        void* gxDev = *reinterpret_cast<void**>(gxoff::kGxDevicePtr);
        if (!gxDev) return;

        auto stateSet = reinterpret_cast<sh::GxStateSetFn>(sh::kGxStateSet);
        stateSet(gxDev, sh::kStateVertexShader, g_vsWrapper);
        stateSet(gxDev, sh::kStatePixelShader,  g_psWrapper);

        // The foam texture goes through the state cache, not the device: it is an engine texture
        // object, and the flush is what turns one into a bound D3D texture. The scene copy is the
        // opposite case -- an ordinary D3D texture on a stage nothing else claims -- which is why
        // that one is written at the draw and this one here.
        if (g_settings)
        {
            auto animTex = reinterpret_cast<liq::SettingsAnimTextureFn>(liq::kSettingsAnimTexture);
            if (void* foam = animTex(g_settings, nullptr, 3, 1250))
                stateSet(gxDev, sh::kStateTexture0 + kFoamStage, foam);
            // Centered normal slopes are bound directly by NoiseBind at draw time.
            // Client liquid textures do not establish that RG slope encoding.
        }

        // Where the two animated maps stand this frame. Resolved here rather than at the draw so
        // every liquid instance in the frame is on the same frame of the loop -- two instances a
        // millisecond apart would put the grain at two different phases along a shared edge.
        //
        // The grain crossfades: the fractional position rides to the pixel stage and the two
        // bracketing slices are both bound, so the loop never steps. The caustics do not -- their
        // frames are closer together, and the shallows shimmer over the difference.
        {
            const w::ShadeFx& fx = w::Fx();
            const double t = w::Now();
            const double fpos = t * ((fx.animFps > 0.5f) ? fx.animFps : 0.5f);
            g_seqFrame = static_cast<int>(fpos);
            g_seqFrac = static_cast<float>(fpos - std::floor(fpos));

            // Reduced to one turn of the loop before it reaches float. The phase is a product of a
            // growing double and a rate; carried in float it loses its low bits within an hour of
            // uptime and the shimmer starts stepping -- the same trap the wave phases sit in.
            const double turns = t * fx.causticSpeed / w::kShadeCausticSlices;
            g_causticPhase = static_cast<float>((turns - std::floor(turns)) * w::kShadeCausticSlices);

            // The second layer, wrapped from the SAME absolute time at its own rate rather than
            // from the first layer's folded phase. Deriving it from the fold made it jump every
            // time the first layer came round -- which is precisely the knock the second layer
            // exists to prevent. The rate is deliberately not a simple ratio: at one half or one
            // third the two would share a period again.
            const double turnsB = t * fx.causticSpeed * 0.6270 / w::kShadeCausticSlices;
            g_causticPhaseB =
                static_cast<float>((turnsB - std::floor(turnsB)) * w::kShadeCausticSlices);
        }

        // Constants are private to WaterDraw; keep them out of the engine cache.
    }

    /// Advances the sea, once per frame, at the frame boundary.
    ///
    /// Deliberately not per draw. Every liquid instance in a frame has to be displaced by the SAME
    /// wave table -- two instances a millisecond apart would meet along a moving seam, which is the
    /// one artefact a shared analytic surface is supposed to make impossible. Advancing it where the
    /// frame ends makes that structural rather than a thing to remember.
    /// The detail textures live in the default pool, so a reset destroys them under us. Dropping
    /// them here rather than checking for loss at every use keeps a crash from being a possibility
    /// instead of a thing to remember: the next update finds nothing and builds them again.
    void OnDeviceLost(void*, const void*)
    {
        w::OceanTextureRelease();
    }

    void OnFrameBoundary(void*, const void* args)
    {
        void* device = static_cast<const wxl::events::FrameArgs*>(args)->device;
        w::perf::Frame();
        { w::perf::Scope timing(w::perf::Stage::Simulation); w::Tick(); }
        // Resolved once per frame, off the table Tick just advanced. Doing it per draw would cost a
        // surface inversion dozens of times for an answer that cannot change within a frame, and
        // would let two draws of the same frame disagree about whether the eye is wet.
        //
        // BEFORE BeginFrame, which resolves the refinement's focus and falls back to this when there
        // is no player object. After it, that fallback would read the previous frame's camera.
        w::UpdateEye(g_camPos);
        w::BeginFrame();
        // Built at a frame boundary, never inside a draw: it is a few hundred milliseconds of
        // arithmetic the first time, and the one place in the frame where spending them cannot land
        // between a state write of ours and the draw it belongs to. After that it is a flag test.
        if (w::On(w::Sw::ShadeMaps)) w::NoiseEnsure(device);
        // After the tick, never before: Tick is what advanced the field, and uploading it first
        // would put last frame's chop on this frame's swell -- a mismatch of one frame, which is
        // exactly the kind of thing that reads as the detail sliding over the waves.
        { w::perf::Scope timing(w::perf::Stage::Upload); w::OceanTextureUpdate(device); }
    }

    /// Wraps one water draw: the sheet is drawn from both sides, and it occludes itself.
    ///
    /// WATER IS OPAQUE TO ITSELF, and a flat sheet never had to say so. It cannot overlap itself on
    /// screen, so the client draws liquid without writing depth -- which is what lets you see the
    /// bottom through it -- and nothing was ever lost.
    ///
    /// A DISPLACED sheet overlaps itself constantly. Look along the water and a single ray crosses
    /// dozens of crests. With no depth being written, every one of them draws, each over the last,
    /// and a wave simply does not hide what is behind it. That is the whole of "the waves are
    /// hollow": not that they are empty, but that they are transparent to their own geometry.
    ///
    /// Culling was the same fault wearing a different face. It removed the far half of each crest,
    /// so the stack was half as deep and read as holes instead of as bands. Both are answered here:
    /// draw both sides, and let the nearest surface win.
    ///
    /// It has to be at the draw and not at the bind. Render state is applied at the deferred flush,
    /// which runs after every seam we hold, so the draw call is the last place the value is still
    /// ours. Everything is restored immediately, so nothing else in the frame inherits any of it.
    const WXL_M2DrawApi* DrawApi()
    {
        static const WXL_M2DrawApi* api = nullptr;
        if (!api)
            api = static_cast<const WXL_M2DrawApi*>(
                wxl::runtime::extensions::GetInterface("wxl.m2draw", WXL_M2DRAW_API_VERSION));
        return api && api->structSize >= sizeof(WXL_M2DrawApi) && api->SetOneShotIntercept
             ? api : nullptr;
    }

    void SetDrawIntercept(WXL_M2Draw_InterceptFn fn)
    {
        if (const WXL_M2DrawApi* api = DrawApi())
        {
            api->SetOneShotIntercept(fn);
            return;
        }

        static bool warned = false;
        if (fn && !warned)
        {
            warned = true;
            WLOG_WARN("water: wxl.m2draw unavailable -- replacement shading remains inactive");
        }
    }

    long ForeverDraw(void* dev,int pt,int bv,unsigned mi,unsigned nv,unsigned si,unsigned pc,WXL_M2Draw_DIPFn orig) {
        auto* d=static_cast<IDirect3DDevice9*>(dev);
        w::forever::BeforeDraw(d);
        const long result=orig(dev,pt,bv,mi,nv,si,pc);
        w::forever::AfterDraw(d);
        if(w::forever::Armed()) SetDrawIntercept(&ForeverDraw);
        return result;
    }
    long WaterDraw(void* dev, int pt, int bv, unsigned mi, unsigned nv, unsigned si, unsigned pc,
                   WXL_M2Draw_DIPFn orig)
    {
        w::perf::Scope timing(w::perf::Stage::Draw);
        w::perf::Draw(pc);
        auto* d = static_cast<IDirect3DDevice9*>(dev);
        w::ConstantState<IDirect3DDevice9> savedConstants(d);
        if (!savedConstants.Ready())
        {
            static unsigned skipped = 0;
            if (skipped++ < 3)
                WLOG_WARN("water-constants-v1: snapshot failed; skipping water draw failures=%u", savedConstants.failures);
            if (g_waterMaterialDepth > 0) SetDrawIntercept(&WaterDraw);
            return D3D_OK;
        }
        UploadConstants(g_camPos, savedConstants);
        static const bool restoreBindings = [] {
            char value[8] = {};
            GetEnvironmentVariableA("WXL_WATER_RESTORE_STATE", value, sizeof(value));
            const bool enabled = value[0] != '0';
            WLOG_INFO("water-state-v1: restore=%u", enabled);
            return enabled;
        }();
        // Capture before the first direct water binding; do not substitute null on failed reads.
        wxl::water::TextureState<IDirect3DDevice9, IDirect3DBaseTexture9> savedTextures(d);

        DWORD prevCull = D3DCULL_CCW, prevZWrite = FALSE, prevZFunc = D3DCMP_LESSEQUAL;
        d->GetRenderState(D3DRS_CULLMODE, &prevCull);
        d->GetRenderState(D3DRS_ZWRITEENABLE, &prevZWrite);
        d->GetRenderState(D3DRS_ZFUNC, &prevZFunc);

        d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        d->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        d->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS);

        // Read back before anything below writes them. Nothing else in the frame restores sampler
        // state, so whatever this pass leaves behind is what every later draw inherits.
        DWORD prevSampler[kSamplerStageSpan][kSamplerStateCount];
        for (int si = 0; si < kSamplerStageSpan; ++si)
            for (int k = 0; k < kSamplerStateCount; ++k)
                d->GetSamplerState(kSamplerFirstStage + si, kSavedSamplerStates[k],
                                   &prevSampler[si][k]);

        // THE LATTICE ITSELF, drawn rather than described. The refinement grading is three numbers
        // in a panel whose whole product is a density that varies across the screen, and no readout
        // conveys that -- where a band edge falls, whether a chunk was graded down, whether the
        // budget ran out behind you. Wireframe answers all of it at once.
        //
        // Restored below with everything else, and never left on across a device reset: the state is
        // read back here every draw rather than assumed, exactly as the three above are.
        DWORD prevFill = D3DFILL_SOLID;
        const bool wire = w::Debug().wireframe;
        if (wire)
        {
            d->GetRenderState(D3DRS_FILLMODE, &prevFill);
            d->SetRenderState(D3DRS_FILLMODE, D3DFILL_WIREFRAME);
        }

        // The scene copy rides here for the same reason the render states do: the texture stages are
        // part of the deferred cache, so a bind at any earlier seam is replaced by the flush with
        // whatever the engine had cached for that stage.
        void* scene = w::SceneCopy();
        if (scene)
        {
            d->SetTexture(w::kSceneStage, static_cast<IDirect3DBaseTexture9*>(scene));
            d->SetSamplerState(w::kSceneStage, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            d->SetSamplerState(w::kSceneStage, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            d->SetSamplerState(w::kSceneStage, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            // Clamped, not wrapped. A refraction offset that runs off the edge of the frame would
            // otherwise fetch the opposite side of the screen and paint it into the water.
            d->SetSamplerState(w::kSceneStage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            d->SetSamplerState(w::kSceneStage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        }

        // Match scene colour and raw depth from the same pre-water frame. The
        // independent R32F copy is safe while native INTZ remains the depth target.
        WXL_SceneDepth depth{};
        const bool depthReady=savedTextures.failures==0 && w::WaterDepth(depth);
        const float depthGate[4]{depthReady ? 1.0f : 0.0f,0,0,0};
        savedConstants.Set(4,52,depthGate,1);
        const auto& sfx=w::Fx();
        const float ssr[4]{sfx.ssrStrength,sfx.ssrDistance,scene ? 1.0f : 0.0f,sfx.ssrRipple};
        savedConstants.Set(4,50,ssr,1);
        if (depthReady)
        {
            float rows[12]{};
            const int component[3]{0,1,3};
            for (int r=0;r<3;++r) for (int j=0;j<4;++j)
                for (int k=0;k<4;++k)
                    rows[r*4+j]+=depth.view[j*4+k]*depth.projection[k*4+component[r]];
            savedConstants.Set(4,47,rows,3);
            const float projection[4]{depth.projection[10],depth.projection[14],depth.depthMinZ,
                1.0f/(depth.depthMaxZ-depth.depthMinZ)};
            savedConstants.Set(4,51,projection,1);

        }
        static unsigned depthReports=0;
        if (depthReports++<4)
            WLOG_INFO("water-screen-trace: depth=%u frame=%llu strength=%.3f range=%.1f",
                depthReady,depth.frameId,sfx.ssrStrength,sfx.ssrDistance);

        // The stock liquid pass only ever configures stages 0 and 1, so anything above sits at the
        // D3D9 defaults -- POINT, no mip -- which is aliased sparkle when the foam minifies and raw
        // texel blocks when it magnifies. Filtering is not part of the deferred cache, so writing it
        // here survives the flush. Wrapped, because the foam is sampled in world coordinates.
        w::OceanTextureBind(dev);

        // The shading maps, for the same reason: they are ordinary D3D textures on stages nothing
        // else claims, and the deferred cache would replace an earlier bind with whatever the
        // engine had recorded for those stages.
        w::NoiseBind(dev, g_seqFrame);
        w::BindLinearNormalSamplers(d);

        // The ocean material's first two stages and its authored ripple/foam stages resolve through
        // the client's texture bank, so replacement MPQs can supply their HD versions unchanged.
        // Preserve the whole sampler range above, then use linear mip filtering for the Retail material.
        // Avoid repeated capability queries and forced 8x anisotropic sampling per water draw.
        static const unsigned kAuthoredStages[] = { 0, 1, kFoamStage };
        for (const unsigned s : kAuthoredStages)
        {
            d->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            d->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            d->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
            d->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
            d->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
        }

        // The shading-map stages, filtered here for the same reason as everything above: sampler
        // filtering survives only a write at the draw. World-space tiling everywhere, except the
        // sky panorama, which wraps in azimuth and must clamp vertically or the zenith would bleed
        // into the sea at the strip's far edge.
        static const unsigned kFxStages[] = { 2, 3, 4, kRippleStage, 12, 13, 14, 15 };
        for (const unsigned s : kFxStages)
        {
            d->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            d->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            d->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
            d->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
            d->SetSamplerState(s, D3DSAMP_ADDRESSV,
                               (s == 13) ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP);
        }

        if (depthReady)
        {
            bool bound=SUCCEEDED(d->SetTexture(15,static_cast<IDirect3DBaseTexture9*>(depth.texture)));
            const auto sampler=[&](D3DSAMPLERSTATETYPE type,DWORD value)
                { bound=SUCCEEDED(d->SetSamplerState(15,type,value)) && bound; };
            sampler(D3DSAMP_MINFILTER,D3DTEXF_POINT); sampler(D3DSAMP_MAGFILTER,D3DTEXF_POINT);
            sampler(D3DSAMP_MIPFILTER,D3DTEXF_NONE); sampler(D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);
            sampler(D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP); sampler(D3DSAMP_SRGBTEXTURE,FALSE);
            if (!bound) { const float disabled[4]{}; savedConstants.Set(4,52,disabled,1); }
        }

        const long r = orig(dev, pt, bv, mi, nv, si, pc);
        savedConstants.Restore();
        static unsigned constantReports = 0;
        if (constantReports++ < 3 || savedConstants.failures)
            WLOG_INFO("water-constants-v1: scoped draw failures=%u", savedConstants.failures);

        // RENEWED, because one provider call is not one draw. hkDIP cleared this pointer before
        // calling us; without putting it back, every draw after the first in this material pass runs
        // with our shader over our geometry and none of our state -- which is the flat water that
        // looked like a missing chunk. Only inside a liquid material, so nothing else inherits it.
        if (g_waterMaterialDepth > 0)
        {
            SetDrawIntercept(&WaterDraw);
            ++g_drawsThisMaterial;
        }

        if (restoreBindings) savedTextures.Restore();
        else savedTextures.Discard();
        // The engine cache must also reassert its own state at its next deferred flush.
        if (void* gxDev = *reinterpret_cast<void**>(gxoff::kGxDevicePtr)) {
            auto stateDirty = reinterpret_cast<sh::GxStateDirtyFn>(sh::kGxStateDirty);
            for (const unsigned s : kRawBoundStages) {
                if (!restoreBindings) d->SetTexture(s, nullptr);
                stateDirty(gxDev, sh::kStateTexture0 + s);
            }
        }
        static unsigned stateReports = 0;
        if (restoreBindings && (stateReports++ < 3 || savedTextures.failures))
            WLOG_INFO("water-state-v1: restored draw failures=%u", savedTextures.failures);

        for (int si = 0; si < kSamplerStageSpan; ++si)
            for (int k = 0; k < kSamplerStateCount; ++k)
                d->SetSamplerState(kSamplerFirstStage + si, kSavedSamplerStates[k],
                                   prevSampler[si][k]);

        if (wire) d->SetRenderState(D3DRS_FILLMODE, prevFill);
        d->SetRenderState(D3DRS_CULLMODE, prevCull);
        d->SetRenderState(D3DRS_ZWRITEENABLE, prevZWrite);
        d->SetRenderState(D3DRS_ZFUNC, prevZFunc);
        return r;
    }

    void StashCamera(const float* camPos)
    {
        if (!camPos) return;
        g_camPos[0] = camPos[0];
        g_camPos[1] = camPos[1];
        g_camPos[2] = camPos[2];
    }

    /// Opens and closes the window in which the draw interceptor renews itself.
    ///
    /// The material call is the right boundary and the provider call is not: the provider hands back
    /// buffers, the material issues the draws, and there may be more than one of them. Closed on the
    /// way out whatever happened inside, and the pointer cleared with it -- an interceptor left armed
    /// past this point would wrap the next thing the client drew, whatever that was.
    void EnterWaterMaterial()
    {
        if (g_waterMaterialDepth++ == 0) g_drawsThisMaterial = 0;
    }

    void LeaveWaterMaterial()
    {
        if (--g_waterMaterialDepth > 0) return;
        if (g_waterMaterialDepth < 0) g_waterMaterialDepth = 0;
        SetDrawIntercept(nullptr);

        if (g_drawsThisMaterial > g_peakDrawsPerMaterial)
        {
            g_peakDrawsPerMaterial = g_drawsThisMaterial;
            // Said once per new high-water mark. "A provider call is one draw" was an assumption
            // this file was built on and nothing had ever measured; anything above one here is the
            // measurement that says how much water was being drawn stateless.
            if (g_peakDrawsPerMaterial > 1)
                WLOG_INFO("water: %d draws in one liquid material pass -- every one past the first "
                          "was previously drawn without our state", g_peakDrawsPerMaterial);
        }
    }

    // Family lookup is shared by both material entry points. Unknown identities
    // always retain native rendering; supported inland takeover can be explicitly disabled.
    bool InlandEnabled()
    {
        static const bool enabled = wxl::render::defaults::Enabled("WXL_INLAND_WATER");
        return enabled;
    }
    bool PoolsEnabled()
    {
        static const bool enabled = wxl::render::defaults::Enabled("WXL_POOL_WATER");
        return enabled;
    }
    unsigned LiquidFamily(void* settings)
    {
        if (!settings) return 0xFFFFFFFFu;

        const uint32_t count = *reinterpret_cast<const uint32_t*>(liq::kSettingsBankCount);
        void** rows          = *reinterpret_cast<void***>(liq::kSettingsBankRows);
        if (!rows || count == 0 || count > 4096) return 0xFFFFFFFFu;

        uint32_t id = 0xFFFFFFFFu;
        for (uint32_t i = 0; i < count; ++i)
            if (rows[i] == settings) { id = i; break; }
        if (id == 0xFFFFFFFFu) return 0xFFFFFFFFu;

        namespace gadt = wxl::offsets::game::adt;
        const uint32_t minId = *reinterpret_cast<const uint32_t*>(gadt::kLiquidTypeDbMinId);
        const uint32_t maxId = *reinterpret_cast<const uint32_t*>(gadt::kLiquidTypeDbMaxId);
        if (id < minId || id > maxId) return 0xFFFFFFFFu;

        uint8_t** dbRows = *reinterpret_cast<uint8_t***>(gadt::kLiquidTypeDbRows);
        const uint8_t* row = dbRows ? dbRows[id - minId] : nullptr;
        if (!row) return 0xFFFFFFFFu;

        // Family lives one field past the flags: 0 water, 1 ocean, 2 magma, 3 slime.
        const unsigned family = *reinterpret_cast<const uint32_t*>(row + 0x0C);
        const bool ocean = family == 1u;

        // Once per type, because the failure mode worth catching is silent: a sea AUTHORED as a
        // modern ocean type that the split degraded to the base water family stops being ours
        // with no error anywhere -- this line is where that shows.
        static uint32_t announced[8] = {};
        static unsigned announcedCount = 0;
        bool seen = false;
        for (unsigned i = 0; i < announcedCount; ++i) seen = seen || announced[i] == id;
        if (!seen && announcedCount < 8)
        {
            announced[announcedCount++] = id;
            WLOG_INFO("water: liquid type %u drawn %s (family %u)", id,
                      ocean ? "as ocean" : (family == 0u && InlandEnabled() ? "inland eligible (map only)" : "stock"), family);
        }
        return family;
    }

    void __fastcall HkWaterRender(void* material, void* edx, void* env, void* geom, void* anim,
                                  const float* camPos, const float* world, const float* sphere,
                                  void* settings)
    {
        if(w::forever::Armed()) {
            w::forever::Select(settings);
            g_origWaterRender(material,edx,env,geom,anim,camPos,world,sphere,settings);
            SetDrawIntercept(nullptr); w::forever::Deselect(); return;
        }
        // Decide before native material setup and before any replacement shader
        // is armed. The core latches this value at world begin for the whole frame.
        if (!w::EnhancementsEnabled())
        {
            g_origWaterRender(material, edx, env, geom, anim, camPos, world, sphere, settings);
            return;
        }
        // Only ordinary water and ocean families have the shared surface contract.
        const unsigned family = LiquidFamily(settings);
        if (family != 1u && !(family == 0u && InlandEnabled()))
        {
            g_origWaterRender(material, edx, env, geom, anim, camPos, world, sphere, settings);
            return;
        }
        w::perf::Scope timing(w::perf::Stage::Material, g_waterMaterialDepth == 0);
        const MaterialSettingsScope settingsScope(settings, family == 0u, world);
        StashCamera(camPos);
        EnterWaterMaterial();
        g_origWaterRender(material, edx, env, geom, anim, camPos, world, sphere, settings);
        LeaveWaterMaterial();
    }

    void __fastcall HkWaterNoSpec(void* material, void* edx, void* env, void* geom, void* anim,
                                  const float* camPos, const float* world, const float* sphere,
                                  void* settings)
    {
        if(w::forever::Armed()) {
            w::forever::Select(settings);
            g_origWaterNoSpec(material,edx,env,geom,anim,camPos,world,sphere,settings);
            SetDrawIntercept(nullptr); w::forever::Deselect(); return;
        }
        // Decide before native material setup and before any replacement shader
        // is armed. The core latches this value at world begin for the whole frame.
        if (!w::EnhancementsEnabled())
        {
            g_origWaterNoSpec(material, edx, env, geom, anim, camPos, world, sphere, settings);
            return;
        }
        const unsigned family = LiquidFamily(settings);
        if (family != 1u && !(family == 0u && InlandEnabled()))
        {
            g_origWaterNoSpec(material, edx, env, geom, anim, camPos, world, sphere, settings);
            return;
        }
        w::perf::Scope timing(w::perf::Stage::Material, g_waterMaterialDepth == 0);
        const MaterialSettingsScope settingsScope(settings, family == 0u, world);
        StashCamera(camPos);
        EnterWaterMaterial();
        g_origWaterNoSpec(material, edx, env, geom, anim, camPos, world, sphere, settings);
        LeaveWaterMaterial();
    }

    /// The map-liquid geometry provider: where the lattice is refined and where our shaders are put
    /// in place of the stock ones.
    ///
    /// This call sits after the stock shader and state binds and immediately before the streams are
    /// set and the batch issued -- the last point at which a write into the deferred shader cache is
    /// still the value the flush applies. Binding at the material entry instead would be undone by
    /// the stock bind that follows it, and the water would draw with the client's own shader over our
    /// geometry: a surface that is subdivided, displaced by nothing, and shaded as though it were.
    int __fastcall HkChunkGeom(void* geom, void* edx, int fmt, void** vb, void** ib, void* batchDesc)
    {
        if(w::forever::Armed()) {
            const int result=g_origChunkGeom(geom,edx,fmt,vb,ib,batchDesc);
            SetDrawIntercept(&ForeverDraw); return result;
        }
        // This provider is shared by every map-liquid family. Only arm the replacement while an
        // supported water material owns the draw window; otherwise a stock liquid batch with no immediate
        // DIP leaves our one-shot alive and the next terrain chunk is shaded as water (solid black).
        if (g_waterMaterialDepth <= 0)
            return g_origChunkGeom(geom, edx, fmt, vb, ib, batchDesc);

        g_meshPool = false;
        w::BeginBatch();
        int r;
        { w::perf::Scope timing(w::perf::Stage::Geometry);
          r = g_origChunkGeom(geom, edx, fmt, vb, ib, batchDesc); }
        if (!g_inland) w::Densify(geom, vb, ib, batchDesc);
        // After the refinement, not before: whatever Densify publishes is complete by construction,
        // so this only ever sees a batch the PROVIDER left unwritten and the module had nothing of
        // its own to show for that group.
        w::EnsureDrawable(batchDesc);
        if (g_inland) {
            static bool announced = false;
            if (!announced) { WLOG_INFO("water-inland-v1: map liquid surface active; flat geometry, river palette, no ocean lace"); announced = true; }
        }
        BindSurface();
        SetDrawIntercept(&WaterDraw);
        return r;
    }

    /// The WMO-liquid provider. Its emitter is not ours to capture, so this water is not subdivided
    /// -- ocean materials retain their existing surface. Opt-in upright inland pools
    /// use native projection plus an independent world transform for shading.
    int __fastcall HkMeshGeom(void* geom, void* edx, int fmt, void** vb, void** ib, void* batchDesc)
    {
        if(w::forever::Armed()) {
            const int result=g_origMeshGeom(geom,edx,fmt,vb,ib,batchDesc);
            SetDrawIntercept(&ForeverDraw); return result;
        }
        if (g_waterMaterialDepth <= 0 || (g_inland && (!PoolsEnabled() || !g_poolTransformValid || fmt != 6)))
        {
            if (g_inland && PoolsEnabled()) {
                static bool reported=false;
                if (!reported) { WLOG_WARN("water-pool-v1: native fallback; validTransform=%u vertexFormat=%d",g_poolTransformValid,fmt); reported=true; }
            }
            SetDrawIntercept(nullptr);
            return g_origMeshGeom(geom, edx, fmt, vb, ib, batchDesc);
        }

        g_meshPool = g_inland;
        if (g_meshPool) {
            static bool announced=false;
            if (!announced) {
                WLOG_INFO("water-pool-v1: WMO pool path active; translation=(%.2f,%.2f,%.2f) scale=%.3f yaw-basis=(%.3f,%.3f)",
                    g_poolWorld[12],g_poolWorld[13],g_poolWorld[14],g_poolWorld[10],g_poolWorld[0],g_poolWorld[1]);
                announced=true;
            }
        }
        int r;
        { w::perf::Scope timing(w::perf::Stage::Geometry);
          r = g_origMeshGeom(geom, edx, fmt, vb, ib, batchDesc); }
        BindSurface();
        SetDrawIntercept(&WaterDraw);
        return r;
    }

    /// Gives a liquid type the client has never heard of something to draw with.
    ///
    /// THIS IS WHAT DECIDES WHETHER A CHUNK OF MH2O EXISTS AT ALL. The bank maps a liquid type id to
    /// the material that draws it, and 3.3.5's bank knows only the handful of types 3.3.5 shipped.
    /// A map built from modern ADTs carries modern ids, the lookup misses, and the client is handed
    /// nothing -- so the chunk is not drawn badly, it is not drawn. No amount of work further down
    /// the pipeline reaches it, because nothing downstream is ever called for it.
    ///
    /// Falling back to type 1 is the adaptation the project is for: the placement, the height and the
    /// extents all come from the map exactly as authored, and only the QUESTION of what to shade it
    /// with is answered with the water we have. Nothing about the asset is altered or discarded.
    ///
    /// Type 1 is asked of the bank rather than cached, so a bank that is itself not ready yet still
    /// answers for us with whatever it would have answered with.
    void* __cdecl HkGetMaterial(uint32_t liquidTypeId)
    {
        void* material = g_origGetMaterial(liquidTypeId);
        // The recursion guard is the second half: asking for 1 when 1 is what already failed would
        // call straight back into this.
        if (material || liquidTypeId == 1) return material;
        return g_origGetMaterial(1);
    }

    bool Install()
    {
        bool ok = wxl::hook::Install("Water_SurfaceRender", liq::kMaterialWaterRender,
                                     &HkWaterRender, &g_origWaterRender);
        ok &= wxl::hook::Install("Water_SurfaceRenderNoSpec", liq::kMaterialWaterNoSpecRender,
                                 &HkWaterNoSpec, &g_origWaterNoSpec);
        ok &= wxl::hook::Install("Water_SurfaceChunkGeom", liq::kChunkGeomGetBuffers,
                                 &HkChunkGeom, &g_origChunkGeom);
        ok &= wxl::hook::Install("Water_SurfaceMeshGeom", liq::kMeshGeomGetBuffers,
                                 &HkMeshGeom, &g_origMeshGeom);
        ok &= wxl::hook::Install("Water_SurfaceMaterialBank", liq::kMaterialBankGetMaterial,
                                 &HkGetMaterial, &g_origGetMaterial);
        ok &= w::InstallHooks();

        wxl::events::Subscribe(wxl::events::Event::OnFrame, &OnFrameBoundary, nullptr);
        wxl::events::Subscribe(wxl::events::Event::OnDeviceLost, &OnDeviceLost, nullptr);
        w::Tick();   // so the first liquid draw of the session has a table to read

        const w::ShoreProfile& sp = w::ShoreSettings();
        WLOG_INFO("water: surface installed (subdivision %d, shore dead %.1f..%.1f, felt to %.0f, "
                  "breaker %.2f)",
                  w::Subdivision(), sp.deadStart, sp.deadFull, sp.feelDepth, sp.breakerIndex);
        return ok;
    }
}

namespace wxl::water
{
    bool EnhancementsEnabled()
    {
        static const WXL_RenderControlApi* control=nullptr;
        if (!control) control=static_cast<const WXL_RenderControlApi*>(
            wxl::runtime::extensions::GetInterface("wxl.render-control", WXL_RENDER_CONTROL_API_VERSION));
        return WxlRenderEffectsEnabled(control);
    }
    SurfaceLook&  Look()    { return g_look; }
    FoamShape&    Foam()    { return g_foam; }
    ShadeFx&      Fx()      { return g_fx; }
    ShadeDebug&   Debug()   { return g_debug; }
    ReflectionLightPreview& LightPreview() { return g_lightPreview; }
    const float*  SunDir()  { return g_sunDir; }
    const float*  CameraPos() { return g_camPos; }

    FxStatus FxTextures()
    {
        FxStatus s;
        s.ready = FxReady();
        s.buildMs = NoiseBuildMs();
        s.grainFrames = kShadeGrainFrames;
        s.causticSlices = kShadeCausticSlices;
        return s;
    }
}

WXL_REGISTER_FEATURE("water", wxl::features::water, Install)
