// The water tuning panel: every number the sea is made of, movable while looking at it.
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
// WHY THIS EXISTS
//   A sea has perhaps forty numbers in it and not one of them can be reasoned to. Extinction can be
//   argued from physics, but where a beach starts foaming is a property of one map's bathymetry, and
//   the only way to find it is to stand on that beach and move the number. Every value found by
//   rebuilding and relaunching costs minutes; every value found here costs a drag.
//
//   So the panel is not a convenience. It is the difference between tuning a sea and guessing at one.
//
// WHAT IT IS NOT ALLOWED TO TOUCH
//   Nothing here may make two clients disagree about where the water is. The look and the foam are
//   safe by construction -- no query reads them. The sea state is NOT: it feeds buoyancy, the
//   submersion test and the drawn surface alike, so the override below is a local staging tool, and
//   a session that means to be shared has to leave it off.

#include "wxl-experimental-water/panel/Lang.hpp"
#include "wxl-experimental-water/mesh/Lattice.hpp"
#include "wxl-experimental-water/sea/Ocean.hpp"
#include "wxl-experimental-water/panel/Profile.hpp"
#include "wxl-experimental-water/sea/Sea.hpp"
#include "wxl-experimental-water/sea/Shore.hpp"
#include "wxl-experimental-water/render/Surface.hpp"
#include "wxl-experimental-water/panel/Switches.hpp"
#include "wxl-experimental-water/panel/Widgets.hpp"

#include "wxl/EnvironmentWind.hpp"

#include "config.hpp"
#include "common/Log.hpp"
#include "engine/hook/Registry.hpp"
#include "engine/ui/ImGuiHost.hpp"

#include "imgui.h"
#include "wxl-experimental-water/world/VWaterPass.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "wxl/RenderReloadKey.hpp"
#include "wxl-experimental-water/panel/SettingNumbers.hpp"
#include "engine/events/Event.hpp"
namespace
{
    namespace w = wxl::water;
    namespace ui = wxl::water::ui;
    namespace profile = wxl::water::profile;

    // Every setting in the tabs below goes through ui::Field, so its prefix is the same noise L()'s
    // would be. Sw is here for the same reason: a switch appears beside the control it gates.
    using w::Sw;

    // Unqualified on purpose: every label and every tooltip in this file goes through L(), so a
    // prefix on it would be noise on the most-repeated call in the module.
    using w::L;
    using w::Lang;
    using w::Language;
    using w::SetLanguage;

    constexpr float kDeg = 57.29577951f;

    bool           g_override = false;
    w::SeaConditions g_forced;
    bool           g_forcedInit = false;

    /// The (?) marker for the few call sites that are not a Field -- a button, a seed box. Every
    /// actual setting gets one from the widget instead.
    void Help(const char* text)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::BeginItemTooltip())
        {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
            ImGui::TextUnformatted(text);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }

    // ---- persistence ----------------------------------------------------------------------------
    //
    // WHY THIS EARNS ITS PLACE. Every value in this panel was found by moving it and looking, which
    // is the only way any of them can be found -- and until now every one of them was lost on the
    // next build. Worse, a default changed in the source silently overwrote work that had taken an
    // evening, with no way to tell that it had.
    //
    // One enumeration serves both directions, so a field cannot be saved and not loaded. Adding a
    // setting anywhere in the sea means adding one line here and nothing else.

    constexpr const char* kSettingsFile = "wxl-water.cfg";
    float g_retailProfileVersion = 2.0f;
    void ApplyRetailWater();

    /// Seeds go through their own visitor because they are NOT numbers here, they are identities. A
    /// float carries 24 bits of mantissa; a seed carries 32, and the ones the panel generates are
    /// well past the point where a round trip through float would come back changed -- silently, and
    /// as a completely different sea.
    template <class V>
    void EachSetting(V& v)
    {
        w::SeaResponse&  r = w::Response();
        w::SurfaceLook&  l = w::Look();
        w::FoamShape&    f = w::Foam();
        w::OceanTuning&  o = w::OceanSettings();

        v.Num("chopLag", r.chopLag);
        v.Num("swellLag", r.swellLag);

        w::SeaConditions* pair[2] = { &r.calm, &r.storm };
        const char* tag[2] = { "calm", "storm" };
        for (int i = 0; i < 2; ++i)
        {
            w::SeaConditions& c = *pair[i];
            v.Group(tag[i]);
            v.Num("windX", c.windDir.x);
            v.Num("windY", c.windDir.y);
            v.Num("windSpeed", c.windSpeed);
            v.Num("amplitude", c.amplitude);
            v.Num("steepness", c.steepness);
            v.Num("spread", c.directionSpread);
            v.Num("minWavelength", c.minWavelength);
            v.Num("crestSpacing", c.crestSpacing);
            v.Num("swellLength", c.swellLength);
            v.Num("swellShare", c.swellShare);
            v.Num("exposure", c.exposure);
            v.Seed("seed", c.seed);
        }
        v.Group("");

        v.Num("extinctionR", l.extinction[0]);
        v.Num("extinctionG", l.extinction[1]);
        v.Num("extinctionB", l.extinction[2]);
        v.Num("maxPath", l.maxPath);
        v.Num("bottomBrightness", l.bottomBrightness);
        v.Num("fresnelF0", l.fresnelF0);
        v.Num("specularPower", l.specularPower);
        v.Num("sunIntensity", l.sunIntensity);
        v.Num("roughness", l.roughness);
        v.Num("foam", l.foam);
        v.Num("rippleScaleA", l.rippleScale[0]);
        v.Num("rippleScaleB", l.rippleScale[1]);
        v.Num("rippleStrength", l.rippleStrength);
        v.Num("glowR", l.glow[0]);
        v.Num("glowG", l.glow[1]);
        v.Num("glowB", l.glow[2]);
        v.Num("glowStrength", l.glowStrength);
        v.Num("glintStrength", l.glintStrength);
        v.Num("glintScale", l.glintScale);
        v.Num("glintThreshold", l.glintThreshold);
        v.Num("glintSharpness", l.glintSharpness);
        v.Num("glintFadeStart", l.glintFadeStart);
        v.Num("troughShade", l.troughShade);
        v.Num("nightFloor", l.nightFloor);
        // Same file key it always had; the value moved into the ocean tuning when it became an
        // input to the amplitude budget, and a saved file predating that still loads.
        v.Num("detailStrength", o.strength);

        v.Num("breakThreshold", f.breakThreshold);
        v.Num("breakSharpness", f.breakSharpness);
        v.Num("shoreSlopeMin", f.shoreSlopeMin);
        v.Num("shoreSlopeGain", f.shoreSlopeGain);
        v.Num("shoreDepthFade", f.shoreDepthFade);
        v.Num("shoreStrength", f.shoreStrength);
        v.Num("maskFloor", f.maskFloor);
        v.Num("maskContrast", f.maskContrast);
        v.Num("foamScaleA", f.scale[0]);
        v.Num("foamScaleB", f.scale[1]);
        v.Num("foamScaleC", f.scale[2]);
        v.Num("foamWarp", f.warp);
        v.Num("foamDensity", f.density);

        // The texture effects' strengths and scales persist; their switches do not -- a toggle is
        // for judging a contribution, and a judgement that survives a restart silently is a
        // preference nobody remembers making.
        w::ShadeFx& x = w::Fx();
        v.Num("fxAnimStrength", x.animStrength);
        v.Num("fxAnimScaleFine", x.animScaleFine);
        v.Num("fxAnimScaleBroad", x.animScaleBroad);
        v.Num("fxAnimFps", x.animFps);
        v.Num("fxFbmStrength", x.fbmStrength);
        v.Num("fxFbmScale", x.fbmScale);
        v.Num("fxFoamTexStrength", x.foamTexStrength);
        v.Num("fxFoamTexScale", x.foamTexScale);
        v.Num("fxSkyStrength", x.skyStrength);
        v.Num("fxSssBoost", x.sssBoost);
        v.Num("fxSssLobe", x.sssLobe);
        v.Num("fxSssThickness", x.sssThickness);
        v.Num("fxSssSunFalloff", x.sssSunFalloff);
        v.Num("fxDetileWarp", x.detileWarp);
        v.Num("fxCausticStrength", x.causticStrength);
        v.Num("fxCausticScale", x.causticScale);
        v.Num("fxCausticScale2", x.causticScale2);
        v.Num("fxCausticSpeed", x.causticSpeed);
        v.Num("fxCausticSunPower", x.causticSunPower);
        v.Num("fxCausticDepthFade", x.causticDepthFade);
        v.Num("fxCausticSurface", x.causticSurface);
        v.Num("fxCausticViewBias", x.causticViewBias);
        v.Num("fxCausticSurfaceFade", x.causticSurfaceFade);
        v.Num("fxStyleCartoon", x.styleCartoon);
        v.Num("fxClarityFloor", x.clarityFloor);
        v.Num("fxStyleRepaint", x.styleRepaint);
        v.Num("fxStyleGrazing", x.styleGrazing);
        v.Num("fxStyleSunDiffuse", x.styleSunDiffuse);
        v.Num("fxRefractRetail", x.refractRetail);
        v.Num("fxRefractThickness", x.refractThickness);
        v.Num("fxRefractAmount", x.refractAmount);
        v.Num("fxSsrStrength", x.ssrStrength);
        v.Num("fxSsrDistance", x.ssrDistance);
        v.Num("fxSsrRipple", x.ssrRipple);
        v.Num("fxLaceStrength", x.laceStrength);
        v.Num("fxLaceScale", x.laceScale);
        v.Num("fxLaceWobble", x.laceWobble);
        v.Num("fxLaceCrestBoost", x.laceCrestBoost);
        v.Num("fxLaceDepthFade", x.laceDepthFade);
        v.Num("fxLaceDistFade", x.laceDistFade);

        v.Num("oceanAmplitude", o.amplitude);
        v.Num("oceanShortest", o.shortest);
        v.Num("oceanLongest", o.longest);
        v.Num("oceanCellSize", o.cellSize);
        v.Num("oceanFoamThreshold", o.foamThreshold);
        v.Num("oceanFoamLife", o.foamLife);
        // One key per cascade, because they are one setting per cascade. The name buffer in the
        // reader caps a key at under a hundred characters, which these sit far inside.
        {
            char key[24];
            for (int ci = 0; ci < w::kOceanCascades; ++ci)
            {
                std::snprintf(key, sizeof(key), "oceanAlign%d", ci);
                v.Num(key, o.align[ci]);
                std::snprintf(key, sizeof(key), "oceanSpeed%d", ci);
                v.Num(key, o.speed[ci]);
            }
        }
        v.Num("oceanFalloff", o.falloff);
        v.Num("oceanCrestLength", o.crestLength);
        v.Num("oceanUpwind", o.upwind);
        v.Num("oceanTimeScale", o.timeScale);
        v.Num("oceanChop", o.chop);
        v.Num("oceanFoldBudget", o.foldBudget);
        v.Num("oceanStokes", o.stokes);
        v.Num("oceanLean", o.lean);
        v.Num("oceanWaveCoupling", o.waveCoupling);
        v.Num("oceanFoamGain", o.foamGain);
        v.Num("oceanStormAmplitude", o.stormAmplitude);
        v.Num("oceanStormChop", o.stormChop);
        v.Num("oceanStormFold", o.stormFold);
        v.Num("oceanStormFoam", o.stormFoam);
        v.Seed("oceanSeed", o.seed);

        // THE SWITCHES ARE SETTINGS NOW, and they were not.
        //
        // They began as kill switches -- staging tools, deliberately never saved, on the argument
        // that a fallback surviving a restart is a silent difference from every other client. That
        // argument held while there were five of them and they all meant "turn a layer off to find\n// a bug". It stopped holding once every contribution had one: which effects a sea uses IS
        // the sea, and a look somebody spent an evening on cannot come back without them.
        //
        // Carried as floats so all four visitors -- the file writer, the file reader and both ends
        // of the shareable key -- handle them with no new method: every field in this enumeration
        // stays exactly four bytes wide, which is what lets the key verify its own field count.
        v.Group("switch");
        for (int i = 0; i < static_cast<int>(w::Sw::Count); ++i)
        {
            const w::Sw s = static_cast<w::Sw>(i);
            float on = w::On(s) ? 1.0f : 0.0f;
            v.Num(w::SwitchId(s), on);
            w::SetOn(s, on != 0.0f);
        }

        w::ShoreProfile& sh = w::ShoreSettings();

        // deadFull SITS WHERE fullDepth USED TO, and the position is deliberate rather than
        // incidental. A shareable key is laid out by this enumeration's ORDER, so a field removed
        // from the middle slides every later field one place along and an older key lands each of
        // its values on the wrong setting -- the exact scrambling the version check used to prevent
        // by refusing the key outright.
        //
        // The two also mean the same thing: the depth at which the sea reaches its full height. So
        // an older key migrates rather than merely surviving, and the rest of the shore is appended
        // at the end where it belongs.
        v.Num("deadFull", sh.deadFull);

        float cullMargin = w::CullMargin();
        // TWO SLOTS FEWER THAN THE LAST BUILD. The refinement bands are gone with the grading, and a
        // shareable key is laid out by this enumeration's ORDER -- so a key written before this
        // build lands its remaining values two places along and will misread the mesh block. That is
        // the one-time cost of deleting a subsystem; re-copy the profile once and it is paid.
        float subdiv = static_cast<float>(w::Subdivision());
        v.Num("cullMargin", cullMargin);
        v.Num("subdivision", subdiv);
        // Written back unconditionally: the setters clamp, so pushing values through them is what
        // makes a hand-edited file safe to load.
        w::SetCullMargin(cullMargin);
        w::SetSubdivision(static_cast<int>(subdiv + 0.5f));

        // The rest of the shore, appended: every field here is new, so an older key simply stops
        // before them and they keep the values they already had.
        v.Group("shore");
        v.Num("scale", sh.scale);
        v.Num("deadStart", sh.deadStart);
        v.Num("feelDepth", sh.feelDepth);
        v.Num("breakerIndex", sh.breakerIndex);
        v.Num("shoalRise", sh.shoalRise);
        v.Num("pinchRise", sh.pinchRise);
        v.Num("chopLead", sh.chopLead);
        v.Num("surfFoam", sh.surfFoam);

        // APPENDED, and the position matters more than the grouping. A shareable key is laid out by
        // this enumeration's order, so a field inserted anywhere but the end slides every later one
        // along and an older key lands each of its values on the wrong setting. It belongs with the
        // mesh above and it sits here instead, which is the trade that keeps old keys loadable.
        float rebuilds = static_cast<float>(w::RebuildBudget());
        v.Num("rebuildBudget", rebuilds);
        w::SetRebuildBudget(static_cast<int>(rebuilds + 0.5f));

        float nearAlways = w::NearAlwaysRadius();
        v.Num("nearAlwaysRadius", nearAlways);
        w::SetNearAlwaysRadius(nearAlways);
        // Appended for positional profile compatibility; never insert among old fields.
        v.Num("retailProfileVersion", g_retailProfileVersion);
    }

    struct Writer
    {
        std::FILE* fp;
        const char* group = "";
        void Group(const char* g) { group = g; }
        void Num(const char* name, float& value)
        { std::fprintf(fp, "%s%s = %.6g\n", group, name, value); }
        void Seed(const char* name, uint32_t& value)
        { std::fprintf(fp, "%s%s = %u\n", group, name, value); }
    };

    struct Reader
    {
        const char* text = nullptr;
        bool dryRun=false,valid=true; unsigned hits=0;
        const char* group = "";
        void Group(const char* g) { group = g; }

        /// Finds one key. Linear over the file per field, which is O(fields x file) and completely
        /// irrelevant: this runs when a person presses a button, over a few kilobytes.
        const char* Find(const char* name, char* key, size_t keyLen)
        {
            std::snprintf(key, keyLen, "\n%s%s = ", group, name);
            const char* at = std::strstr(text, key);
            if (at) return at + std::strlen(key);
            // The first line has no newline before it.
            std::snprintf(key, keyLen, "%s%s = ", group, name);
            return (std::strncmp(text, key, std::strlen(key)) == 0) ? text + std::strlen(key)
                                                                    : nullptr;
        }

        void Num(const char* name, float& value)
        {
            char key[96];
            if (const char* at = Find(name, key, sizeof(key)))
            {
                ++hits; float parsed=0;
                if(!w::settingnumbers::Float(at,parsed)) valid=false;
                else if(!dryRun) value=parsed;
            }
        }
        void Seed(const char* name, uint32_t& value)
        {
            char key[96];
            if (const char* at = Find(name, key, sizeof(key)))
            {
                ++hits; uint32_t parsed=0;
                if(!w::settingnumbers::Seed(at,parsed)) valid=false;
                else if(!dryRun) value=parsed;
            }
        }
    };

    // ---- the shareable profile ------------------------------------------------------------------
    //
    // Both directions ride the SAME enumeration the file does, which is the point: a setting added
    // to EachSetting is in the file, in the key and in the field count on the same edit. Any scheme
    // with its own list of what to serialise is a list somebody forgets to extend, and the symptom
    // is a profile that silently drops whatever was added last.
    //
    // Raise this whenever the enumeration's ORDER changes. The field count catches additions and
    // removals on its own; it cannot catch two fields swapping places, which produces a key that
    // verifies and means something else.
    constexpr uint8_t kProfileVersion = 1;

    struct KeyWriter
    {
        std::vector<uint8_t> bytes;
        void Group(const char*) {}
        void Num(const char*, float& value) { Push(&value); }
        void Seed(const char*, uint32_t& value) { Push(&value); }
        void Push(const void* p)
        {
            uint8_t b[profile::kFieldBytes];
            std::memcpy(b, p, sizeof(b));
            bytes.insert(bytes.end(), b, b + sizeof(b));
        }
    };

    struct KeyReader
    {
        const uint8_t* p = nullptr;
        const uint8_t* end = nullptr;
        void Group(const char*) {}
        void Num(const char*, float& value) { Pull(&value); }
        void Seed(const char*, uint32_t& value) { Pull(&value); }
        void Pull(void* out)
        {
            if (p + profile::kFieldBytes > end) return;
            std::memcpy(out, p, profile::kFieldBytes);
            p += profile::kFieldBytes;
        }
    };

    /// How many fields the enumeration currently produces. Counted by running it, so it cannot
    /// disagree with itself.
    size_t ProfileFieldCount()
    {
        KeyWriter w;
        EachSetting(w);
        return w.bytes.size() / profile::kFieldBytes;
    }

    std::string ExportProfile()
    {
        KeyWriter w;
        EachSetting(w);
        return profile::Encode(kProfileVersion, w.bytes);
    }

    /// How many settings the last pasted key carried, and how many this build knows about. Zero
    /// carried means nothing has been pasted yet or the last paste was not a key.
    int g_importCarried = 0;
    int g_importKnown = 0;

    /// Applies a key, whatever build wrote it.
    ///
    /// The enumeration is APPEND-ONLY, so an older key is a prefix: KeyReader::Pull stops when the
    /// payload runs out and every field past the end keeps the value it already had. A NEWER key
    /// runs the other way -- the reader stops when the enumeration does and the trailing bytes are
    /// dropped. Both are partial loads, and both are reported rather than hidden, because a setting
    /// that stayed behind is invisible unless something says so.
    bool ImportProfile(const char* key)
    {
        std::vector<uint8_t> payload;
        size_t carried = 0;
        if (!profile::Decode(key, payload, nullptr, &carried))
        {
            g_importCarried = 0;
            return false;
        }

        KeyReader r{ payload.data(), payload.data() + payload.size() };
        g_retailProfileVersion = 0.0f;
        EachSetting(r);
        if (g_retailProfileVersion != 2.0f) ApplyRetailWater();
        g_importCarried = static_cast<int>(carried);
        g_importKnown = static_cast<int>(ProfileFieldCount());
        // The same snap the presets take. Every lag in the sea exists so weather ARRIVES rather
        // than switches, which is right for weather and useless in front of somebody who just
        // pasted a profile and is looking for what changed.
        w::SnapSea();
        return true;
    }

    bool SaveSettings()
    {
        std::FILE* fp = std::fopen(kSettingsFile, "wb");
        if (!fp) return false;
        std::fprintf(fp, "# WarcraftXL water settings. Written by the in-game panel.\n");
        // Outside the enumeration, deliberately. The language is a property of whoever is looking at
        // the panel, not of the sea -- so it belongs in the file and NOT in the shareable key, which
        // rides that same enumeration. Handing somebody your sea must not change their language.
        std::fprintf(fp, "panel.language = %s\n",
                     (Language() == Lang::French) ? "fr" : "en");
        Writer w{ fp };
        EachSetting(w);
        std::fclose(fp);
        WLOG_INFO("water: settings written to %s", kSettingsFile);
        return true;
    }

    bool LoadSettings()
    {
        w::LightPreview().enabled = false;
        std::FILE* fp = std::fopen(kSettingsFile, "rb");
        if (!fp) return false;

        std::fseek(fp, 0, SEEK_END);
        const long len = std::ftell(fp);
        std::fseek(fp, 0, SEEK_SET);
        // Strictly less than the buffer, not at most: the terminator below needs the last byte, and
        // a file of exactly the buffer's size would put it one past the end.
        if (len <= 0 || len >= (1 << 20)) { std::fclose(fp); return false; }

        static char buffer[1 << 20];
        const size_t got = std::fread(buffer, 1, static_cast<size_t>(len), fp);
        std::fclose(fp);
        if(got!=static_cast<size_t>(len)) return false;
        buffer[got] = '\0';
        Reader check{buffer}; check.dryRun=true; EachSetting(check);
        if(!check.valid || !check.hits) return false;

        // A MISSING KEY LEAVES ITS FIELD ALONE rather than zeroing it. That is what lets a file
        // written by an older build load into a newer one: whatever was added since keeps its
        // default instead of arriving as zero, which for half of these is an invisible sea.
        if (const char* lang = std::strstr(buffer, "panel.language = "))
            SetLanguage((lang[17] == 'f') ? Lang::French : Lang::English);

        Reader r{ buffer };
        g_retailProfileVersion = 0.0f;
        EachSetting(r);
        if (g_retailProfileVersion != 2.0f) {
            ApplyRetailWater();
            WLOG_INFO("water: migrated legacy settings to Retail baseline");
        }
        WLOG_INFO("water: settings read from %s", kSettingsFile);
        return true;
    }

    /// THE SHORE, DRAWN. Every setting in this tab is a DEPTH, and a depth is the one quantity in
    /// this system nobody can see from inside the game -- so the whole tab was being tuned blind,
    /// which is exactly what it felt like.
    ///
    /// Two things are on this plot and the second is what makes it work. The curves are what the
    /// ladder does; the grey behind them is WHERE THIS MAP'S WATER ACTUALLY IS, measured off the
    /// vertices drawn last frame. A transition set at twelve units on a map whose water runs two to
    /// four hundred is a transition nobody will ever swim through, and no amount of dragging finds
    /// that out -- it is only ever visible as the curve sitting beside the grey rather than on it.
    void ShorePlot()
    {
        const w::ShoreProfile& s = w::ShoreSettings();
        const float totalRise = w::Frame().maxDisplacement + w::OceanMaxRise();

        if (!ui::BeginChart("##shore", 132.0f)) return;

        // The map's own water first, so the curves are read against it rather than over it.
        const int buckets = w::DepthBuckets();
        ui::ChartBars(w::DepthHistogram(), buckets);

        // Over one is legitimate -- the shoaling gain lifts the height above the open sea's -- so
        // the vertical axis runs past it and the hump is not silently flattened at the ceiling.
        constexpr float kTop = 1.6f;
        constexpr int kSamples = 160;
        float height[kSamples], detail[kSamples], surf[kSamples];
        for (int i = 0; i < kSamples; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(kSamples - 1);
            const float depth = w::DepthForBucket(t * static_cast<float>(buckets));
            const w::ShoreResponse r = w::ShoreAt(s, depth, totalRise, 1e9f);
            height[i] = r.height / kTop;
            detail[i] = r.detail / kTop;
            surf[i]   = r.surf / kTop;
        }

        // The depths the ladder turns on, so a curve's shoulder can be tied to the control that put
        // it there. Same log axis as the histogram, solved the other way.
        const float sc = (s.scale > 0.01f) ? s.scale : 0.01f;
        const float axisLog = std::log(1.0f + w::DepthForBucket(static_cast<float>(buckets)));
        const float marks[3] = { s.deadStart * sc, s.deadFull * sc, s.feelDepth * sc };
        const unsigned markCol[3] = { IM_COL32(200, 90, 80, 160), IM_COL32(200, 150, 70, 160),
                                      IM_COL32(90, 130, 200, 160) };
        for (int m = 0; m < 3; ++m)
            if (marks[m] > 0.0f) ui::ChartVLine(std::log(1.0f + marks[m]) / axisLog, markCol[m]);

        // The open sea's own level, so the hump reads as a gain rather than as just a bump.
        ui::ChartHLine(1.0f / kTop, IM_COL32(255, 255, 255, 40));

        ui::ChartCurve(surf, kSamples, IM_COL32(235, 235, 245, 150), 1.5f);
        ui::ChartCurve(detail, kSamples, IM_COL32(90, 200, 210, 220), 1.5f);
        ui::ChartCurve(height, kSamples, IM_COL32(120, 170, 255, 255), 2.0f);

        ui::EndChart();
        ui::Legend(IM_COL32(120, 170, 255, 255), "legend.waveheight");
        ui::Legend(IM_COL32(90, 200, 210, 220), "legend.detailfield");
        ui::Legend(IM_COL32(235, 235, 245, 150), "legend.surf");
        ui::Legend(IM_COL32(64, 76, 96, 200), "legend.mapwater");
        ui::Legend(markCol[0], "legend.deadstart");
        ui::Legend(markCol[1], "legend.deadfull");
        ui::Legend(markCol[2], "legend.feeldepth");
        ui::Legend(IM_COL32(255, 255, 255, 70), "legend.opensea");
    }

    /// THE DETAIL FIELD, AS BANDS. What each cascade was granted, laid on a wavelength axis.
    ///
    /// The four cascades are the one thing in this module that cannot be read from a list of
    /// numbers: they are ranges, they abut, and what matters is where they sit relative to two
    /// limits neither of them states -- the shortest wave the MESH can carry, and the band the
    /// panel's own shortest/longest sliders asked for. A bar chart over wavelength puts all three
    /// on one axis, and a cascade that has quietly become shading-only is then obvious rather than
    /// buried in a line of text under it.
    void DetailPlot()
    {
        if (!ui::BeginChart("##detail", 108.0f)) return;

        // Log axis over the wavelengths this field can plausibly carry. Fixed rather than fitted to
        // the current band, so dragging the band MOVES the bars instead of rescaling the axis under
        // them -- which is the whole point of watching them.
        constexpr float kLoLambda = 1.0f;
        constexpr float kHiLambda = 300.0f;
        const float axisLog = std::log(kHiLambda / kLoLambda);
        auto axis = [&](float lambda)
        {
            return std::log(std::max(lambda, kLoLambda) / kLoLambda) / axisLog;
        };

        float tallest = 1e-4f;
        for (int ci = 0; ci < w::kOceanCascades; ++ci)
            tallest = std::max(tallest, w::OceanReport(ci).grantedRise);

        const float spacing = w::FinestVertexSpacing();
        for (int ci = 0; ci < w::kOceanCascades; ++ci)
        {
            const w::OceanCascadeReport& r = w::OceanReport(ci);
            if (r.bandLong <= 0.0f || r.bandShort <= 0.0f) continue;

            const float x0 = axis(r.bandShort);
            const float x1 = axis(r.bandLong);
            const float h = r.grantedRise / tallest;

            // Bands the mesh cannot restitute are drawn dimmer, because the height they were
            // granted is height that reaches the shading and never the geometry -- a distinction
            // the number alone does not make.
            const float lambdaRep = std::sqrt(r.bandLong * r.bandShort);
            const bool geometric = (spacing > 1e-3f) && (lambdaRep / spacing >= 4.0f);
            const unsigned fill = geometric ? IM_COL32(90, 140, 220, 190)
                                            : IM_COL32(120, 120, 140, 130);
            ui::ChartRect(x0, 0.0f, x1, h, fill);
            ui::ChartText(x0 + 0.005f, h - 0.02f, IM_COL32(230, 235, 245, 220), "%d", ci);
        }

        // The mesh's own floor: under about four vertices to a wave, a cascade stops being geometry
        // whatever its amplitude says. This is the line a band must sit to the RIGHT of.
        if (spacing > 1e-3f)
        {
            ui::ChartVLine(axis(spacing * 4.0f), IM_COL32(230, 120, 90, 200));
            ui::ChartText(axis(spacing * 4.0f) + 0.004f, 0.96f, IM_COL32(230, 120, 90, 220),
                          "mesh floor");
        }

        // What the sliders ASKED for, against what the ladder produced.
        const w::OceanTuning& o = w::OceanSettings();
        ui::ChartVLine(axis(o.shortest), IM_COL32(200, 190, 90, 150));
        if (o.longest > 0.0f) ui::ChartVLine(axis(o.longest), IM_COL32(200, 190, 90, 150));

        ui::EndChart();
        ui::Legend(IM_COL32(90, 140, 220, 190), "legend.geometric");
        ui::Legend(IM_COL32(120, 120, 140, 130), "legend.shadingonly");
        ui::Legend(IM_COL32(230, 120, 90, 200), "legend.meshfloor");
        ui::Legend(IM_COL32(200, 190, 90, 150), "legend.bandasked");
    }

    /// THE WATER'S COLOUR, AS THE LIGHT ACTUALLY LOSES IT. A ramp, not three coefficients.
    ///
    /// Extinction is edited as a tint at a reference distance, which is already far better than the
    /// raw numbers -- but a single tint says nothing about the CURVE, and the curve is the whole
    /// look: how fast the red goes, how long the green holds, what colour the deep ends up. This
    /// draws the colour a beam keeps at every path length out to the longest one, which is the
    /// gradient the surface is actually shaded from.
    void ColourPlot()
    {
        const w::SurfaceLook& l = w::Look();
        if (!ui::BeginChart("##colour", 56.0f)) return;

        constexpr int kSteps = 96;
        for (int i = 0; i < kSteps; ++i)
        {
            const float t0 = static_cast<float>(i) / static_cast<float>(kSteps);
            const float t1 = static_cast<float>(i + 1) / static_cast<float>(kSteps);
            const float path = l.maxPath * t0;

            float rgb[3];
            for (int c = 0; c < 3; ++c)
            {
                const float v = std::exp(-l.extinction[c] * path);
                rgb[c] = (v < 0.0f) ? 0.0f : ((v > 1.0f) ? 1.0f : v);
            }
            ui::ChartRect(t0, 0.0f, t1, 1.0f,
                          IM_COL32(static_cast<int>(rgb[0] * 255.0f),
                                   static_cast<int>(rgb[1] * 255.0f),
                                   static_cast<int>(rgb[2] * 255.0f), 255));
        }

        // Distance ticks, so the ramp is a measurement rather than a mood board.
        for (int m = 1; m < 4; ++m)
        {
            const float t = static_cast<float>(m) / 4.0f;
            ui::ChartVLine(t, IM_COL32(255, 255, 255, 60));
            ui::ChartText(t + 0.005f, 0.12f, IM_COL32(255, 255, 255, 150), "%.0f u", l.maxPath * t);
        }

        ui::EndChart();
        ui::Note("surface.plot.legend");
    }

    // ---- sea conditions -------------------------------------------------------------------------

    /// @param def what this particular state returns to. It is NOT one constant: the calm sea, the
    ///            storm and the staging override each have their own -- and for the override the
    ///            only meaningful answer is the sea the weather is currently producing, so a
    ///            modified marker there reads as "differs from what the world would give".
    bool EditConditions(const char* id, w::SeaConditions& c, const w::SeaConditions& def)
    {
        ImGui::PushID(id);
        bool changed = false;

        // The heading and the wind speed are NOT edited here any more. They are not the sea's to
        // decide -- one field supplies them, the grass reads it too, and it has its own section
        // below. Editing them per preset would have been three sliders for one wind, two of which
        // could never be the one in effect.
        changed |= ui::Field("sea.amplitude", &c.amplitude, 0.0f, 40.0f, def.amplitude,
                             "u", "%.2f");
        changed |= ui::Field("sea.steepness", &c.steepness, 0.0f, 1.0f, def.steepness, "", "%.3f");
        changed |= ui::Field("sea.spread", &c.directionSpread, 0.0f, 180.0f, def.directionSpread,
                             "deg", "%.1f");
        changed |= ui::Field("sea.crestspacing", &c.crestSpacing, 1.2f, 6.0f, def.crestSpacing,
                             "x", "%.2f");
        changed |= ui::Field("sea.minwavelength", &c.minWavelength, 20.0f, 120.0f,
                             def.minWavelength, "u", "%.1f");
        changed |= ui::Field("sea.swelllength", &c.swellLength, 20.0f, 400.0f, def.swellLength,
                             "u", "%.1f");
        changed |= ui::Field("sea.swellshare", &c.swellShare, 0.0f, 1.0f, def.swellShare,
                             "", "%.3f");

        ImGui::SeparatorText(L("sea.place.label"));
        changed |= ui::Field("sea.exposure", &c.exposure, 0.0f, 1.0f, def.exposure, "", "%.3f");

        int seed = static_cast<int>(c.seed);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.44f);
        if (ImGui::InputInt(L("sea.seed.label"), &seed))
        {
            c.seed = static_cast<uint32_t>(seed);
            changed = true;
        }
        Help(L("sea.seed.help"));

        ImGui::PopID();
        return changed;
    }

    // ---- tabs -----------------------------------------------------------------------------------

    /// The wind, edited here because this is where somebody tuning a sea looks for it -- but owned
    /// elsewhere, and read by the grass as well. Everything below writes the one field; there is no
    /// per-preset copy to get out of step with it.
    void SectionWind()
    {
        if (!ui::Section("sea.wind")) return;

        wxl::wind::WindProfile& p = wxl::wind::Settings();
        const wxl::wind::WindProfile def;
        const wxl::wind::WindSample& air = wxl::wind::Frame();

        ui::Readout("now %.1f u/s at %.0f deg   gust %.0f%%",
                    air.speed, air.headingDeg, air.gust * 100.0f);

        ui::Field("sea.wind.heading", &p.headingDeg, 0.0f, 360.0f, def.headingDeg, "deg", "%.0f");
        ui::Field("sea.wind.base", &p.baseSpeed, 0.0f, 40.0f, def.baseSpeed, "u/s", "%.1f");
        ui::Field("sea.wind.gust", &p.gust, 0.0f, 1.0f, def.gust, "", "%.2f");
        ui::Field("sea.wind.gustperiod", &p.gustPeriod, 2.0f, 90.0f, def.gustPeriod, "s", "%.0f");
        ui::Field("sea.wind.veer", &p.veerDeg, 0.0f, 90.0f, def.veerDeg, "deg", "%.0f");
        ui::Field("sea.wind.veerperiod", &p.veerPeriod, 5.0f, 300.0f, def.veerPeriod, "s", "%.0f");
        ui::Field("sea.wind.weather", &p.weatherCoupling, 0.0f, 1.0f, def.weatherCoupling,
                  "", "%.2f");

        ui::EndSection();
    }

    void TabSeaState()
    {
        const w::SeaConditions& live = w::Conditions();
        const w::WaveSpectrum&  spec = w::Spectrum();

        SectionWind();

        if (ui::Section("seastate.live"))
        {
            ui::Readout("wind %.1f   amplitude %.2f   steepness %.2f   spread %.0f deg",
                        live.windSpeed, live.amplitude, live.steepness, live.directionSpread);
            ui::Readout("swell %.0f at %.0f%%   shortest %.1f   peak %.1f",
                        live.swellLength, live.swellShare * 100.0f, live.minWavelength,
                        (live.windSpeed * live.windSpeed) / 9.80665f);
            ui::Readout("max displacement %.2f world units over %d components",
                        w::Frame().maxDisplacement, spec.count);

            // The pinch ledger, which is the number the whole crest character hangs on. The trains
            // share one cusp budget; their LINEAR sum is what the shore's headroom is computed
            // from, and a sum past one is a sea that folds somewhere on purpose. Shown because it
            // is a product of several sliders and no single knob owns it.
            const w::WaveFrame& fr = w::Frame();
            float pinch = 0.0f;
            for (int i = 0; i < fr.count; ++i)
                pinch += fr.components[i].qa * fr.components[i].k;
            ui::Readout("pinch %.2f of 1.00 at the cusp -- shore headroom x%.2f",
                        pinch, (pinch > 1e-3f) ? (1.0f / pinch) : 1.0f);
            ui::EndSection();
        }

        if (!ui::Section("seastate.override")) return;
        ui::Note("seastate.override.text");

        if (ImGui::Checkbox(L("seastate.overridetoggle.label"), &g_override))
        {
            if (g_override)
            {
                // Seeded from what is currently on screen, so switching it on changes nothing and
                // the first drag starts from the sea being looked at rather than from a default.
                if (!g_forcedInit) { g_forced = live; g_forcedInit = true; }
                w::ForceConditions(g_forced);
            }
            else
            {
                w::ReleaseForcedConditions();
            }
        }

        if (!g_override) ImGui::BeginDisabled();
        // Marked against the LIVE sea, not against a struct default: the question here is always
        // "how far am I holding this away from what the weather would give".
        if (EditConditions("forced", g_forced, live) && g_override)
            w::ForceConditions(g_forced);
        if (ImGui::Button(L("seastate.copylive.label")))
        {
            g_forced = live;
            g_forcedInit = true;
            if (g_override) w::ForceConditions(g_forced);
        }
        if (!g_override) ImGui::EndDisabled();
        ui::EndSection();
    }

    void TabWeather()
    {
        w::SeaResponse& r = w::Response();
        // One fresh response, so each of the two states below is marked against its OWN default
        // rather than against the other's.
        static const w::SeaResponse kFresh;

        ui::FilterBox();

        if (ui::Section("weather.response"))
        {
            ui::Note("weather.response.text");
            ui::Field("weather.choplag", &r.chopLag, 0.0f, 300.0f, 45.0f, "s", "%.1f");
            ui::Field("weather.swelllag", &r.swellLag, 0.0f, 1800.0f, 420.0f, "s", "%.1f");
            ui::EndSection();
        }

        if (ui::Section("weather.calm"))
        {
            ui::Note("weather.calm.text");
            EditConditions("calm", r.calm, kFresh.calm);
            ui::EndSection();
        }

        if (ui::Section("weather.storm"))
        {
            ui::Note("weather.storm.text");
            EditConditions("storm", r.storm, kFresh.storm);
            ui::EndSection();
        }

        ImGui::Separator();
        if (ImGui::Button(L("weather.resetboth.label")))
        {
            const w::SeaResponse fresh;
            r.calm = fresh.calm;
            r.storm = fresh.storm;
            r.chopLag = fresh.chopLag;
            r.swellLag = fresh.swellLag;
        }
    }

    void TabFoam()
    {
        w::FoamShape& f = w::Foam();
        w::SurfaceLook& look = w::Look();

        ui::FilterBox();

        if (ui::Section("foam.amount"))
        {
            ui::Switch(Sw::Foam);
            ui::Field("foam.strength", &look.foam, 0.0f, 2.0f, 1.0f, "", "%.3f");
            ui::Field("foam.density", &f.density, 0.2f, 8.0f, 2.2f, "", "%.3f");
            ui::EndSection();
        }

        if (ui::Section("foam.breaking"))
        {
            ui::Note("foam.breaking.text");
            ui::Switch(Sw::CrestFoam);
            ui::Field("foam.foldthreshold", &f.breakThreshold, 0.0f, 1.0f, 0.62f, "", "%.3f");
            ui::Field("foam.foldsharpness", &f.breakSharpness, 0.5f, 20.0f, 4.0f, "", "%.2f");
            ui::EndSection();
        }

        if (ui::Section("foam.shoreband"))
        {
            ui::Note("foam.shoreband.text");
            ui::Switch(Sw::ShoreFoam);
            ui::Field("foam.slopethreshold", &f.shoreSlopeMin, 0.0f, 1.0f, 0.12f, "", "%.3f");
            ui::Field("foam.slopegain", &f.shoreSlopeGain, 0.05f, 5.0f, 0.8f, "", "%.3f");
            ui::Field("foam.depthfade", &f.shoreDepthFade, 0.0f, 0.5f, 0.09f, "/u", "%.4f");
            ui::Field("foam.bandstrength", &f.shoreStrength, 0.0f, 6.0f, 1.6f, "", "%.3f");
            ui::EndSection();
        }

        if (ui::Section("foam.breakup"))
        {
            ui::Note("foam.breakup.text");
            ui::Field("foam.scalebroad", &f.scale[0], 0.002f, 0.20f, 0.031f, "/u", "%.4f");
            ui::Field("foam.scalemedium", &f.scale[1], 0.01f, 0.50f, 0.113f, "/u", "%.4f");
            ui::Field("foam.scalefine", &f.scale[2], 0.02f, 1.00f, 0.277f, "/u", "%.4f");
            ui::Field("foam.normalwarp", &f.warp, 0.0f, 2.0f, 0.25f, "", "%.3f");
            ui::Field("foam.maskfloor", &f.maskFloor, 0.0f, 1.0f, 0.55f, "", "%.3f");
            ui::Field("foam.maskcontrast", &f.maskContrast, 0.0f, 1.0f, 0.45f, "", "%.3f");
            ui::EndSection();
        }

        ImGui::Separator();
        if (ImGui::Button(L("foam.reset.label"))) f = w::FoamShape{};
    }

    void TabSurface()
    {
        w::SurfaceLook& l = w::Look();

        ColourPlot();
        ui::FilterBox();

        if (ui::Section("surface.depthcolour"))
        {
            ui::Note("surface.depthcolour.text");
            ui::Switch(Sw::Extinction);
            // The three coefficients are edited as the colour deep water reaches and how far it
            // takes to get there -- see Widgets.hpp for why the raw numbers are unusable.
            ui::Extinction("surface.extinction", l.extinction, &l.maxPath);
            ui::Switch(Sw::Bottom);
            ui::Field("surface.bottombrightness", &l.bottomBrightness, 0.0f, 3.0f, 1.15f,
                      "", "%.3f");
            ui::EndSection();
        }

        if (ui::Section("surface.microrelief"))
        {
            ui::Note("surface.microrelief.text");
            ui::Switch(Sw::Ripple);
            ui::Field("surface.ripplescalebroad", &l.rippleScale[0], 0.002f, 0.20f, 0.021f,
                      "/u", "%.4f");
            ui::Field("surface.ripplescalefine", &l.rippleScale[1], 0.005f, 0.40f, 0.049f,
                      "/u", "%.4f");
            ui::Field("surface.ripplestrength", &l.rippleStrength, 0.0f, 2.0f, 0.36f, "", "%.3f");
            ui::EndSection();
        }

        if (ui::Section("surface.lightthroughwave"))
        {
            ui::Note("surface.lightthroughwave.text");
            ui::Switch(Sw::Sss);
            // One picker rather than three sliders: this is a colour and was being edited as three
            // unrelated numbers whose product nobody could picture.
            static const float kGlow[3] = { 0.30f, 0.75f, 0.62f };
            ui::Color3("surface.glow", l.glow, kGlow);
            ui::Field("surface.glowstrength", &l.glowStrength, 0.0f, 4.0f, 0.55f, "", "%.3f");
            ui::EndSection();
        }

        if (ui::Section("surface.daylight"))
        {
            ui::Note("surface.daylight.text");
            ui::Switch(Sw::NightFloor);
            ui::Field("surface.nightfloor", &l.nightFloor, 0.0f, 1.0f, 0.28f, "", "%.3f");
            ui::Switch(Sw::TroughShade);
            ui::Field("surface.troughshading", &l.troughShade, 0.0f, 1.0f, 0.38f, "", "%.3f");
            ui::EndSection();
        }

        if (ui::Section("surface.light"))
        {
            ui::Field("surface.fresnel", &l.fresnelF0, 0.0f, 0.20f, 0.02f, "", "%.4f");
            ui::Field("surface.specularpower", &l.specularPower, 1.0f, 400.0f, 90.0f, "", "%.1f");
            ui::Field("surface.sunintensity", &l.sunIntensity, 0.0f, 4.0f, 0.9f, "", "%.3f");
            ui::Field("surface.roughness", &l.roughness, 0.0f, 1.0f, 0.25f, "", "%.3f");

            const float* sun = w::SunDir();
            const float horiz = std::sqrt(sun[0] * sun[0] + sun[1] * sun[1]);
            ui::Readout("Sun: azimuth %.1f deg, elevation %.1f deg",
                        std::atan2(sun[1], sun[0]) * kDeg,
                        std::atan2(sun[2], horiz) * kDeg);
            ui::Note("surface.sunreadout.text");
            ui::EndSection();
        }

        if (ui::Section("surface.distance"))
        {
            ui::Note("surface.distance.text");
            ui::Switch(Sw::Refraction);
            ui::EndSection();
        }

        ImGui::Separator();
        if (ImGui::Button(L("surface.reset.label")))
        {
            const w::SurfaceLook fresh;
            const float keepFoam = l.foam;   // owned by the Foam tab, not this one
            l = fresh;
            l.foam = keepFoam;
        }
    }

    void TabDetail()
    {
        w::OceanTuning& o = w::OceanSettings();

        ui::Note("detail.intro.text");
        DetailPlot();
        ui::FilterBox();

        if (ui::Section("detail.size"))
        {
            ui::Field("detail.amplitudeceiling", &o.amplitude, 0.0f, 10.0f, 0.30f, "u", "%.3f");
            ui::Field("detail.strength", &o.strength, 0.0f, 2.0f, 1.0f, "x", "%.3f");
            ui::EndSection();
        }

        if (ui::Section("detail.band"))
        {
            ui::Field("detail.shortest", &o.shortest, 0.1f, 12.0f, 3.0f, "u", "%.2f");
            ui::Field("detail.longest", &o.longest, 0.0f, 120.0f, 40.0f, "u", "%.1f");
            ui::Field("detail.cellsize", &o.cellSize, 0.5f, 3.0f, 1.0f, "x", "%.2f");
            // Beside the cell it argues with, because the two are one decision: the cell is what
            // the field is sampled on, this is whether the vertex stage reads between those samples
            // or lands on one of them.
            ui::Switch(Sw::DetailFilter);
            ui::EndSection();
        }

        if (ui::Section("detail.populations"))
        {
            ui::Note("detail.populations.text");
            for (int ci = 0; ci < w::kOceanCascades; ++ci)
            {
                // Per-cascade defaults, so a modified marker on cascade 2 means cascade 2 and not
                // "one of the four was touched".
                static const float kAlign[] = { 0.88f, 0.74f, 0.58f, 0.42f };
                static const float kSpeed[] = { 0.85f, 0.90f, 0.96f, 1.04f };
                char key[48];
                std::snprintf(key, sizeof(key), "detail.align%d", ci);
                ui::Field(key, &o.align[ci], 0.0f, 1.0f, kAlign[ci], "", "%.3f");
                std::snprintf(key, sizeof(key), "detail.drift%d", ci);
                ui::Field(key, &o.speed[ci], 0.25f, 2.5f, kSpeed[ci], "x", "%.3f");
            }
            ui::EndSection();
        }

        if (ui::Section("detail.shape"))
        {
            ui::Field("detail.falloff", &o.falloff, 2.0f, 7.0f, 4.0f, "", "%.2f");
            // The two numbers that decide whether the field reads as waves ARRIVING or as water
            // boiling in place -- see the spectrum's directional lobe for why each one was a
            // bubble-maker at its old implicit value.
            // Up to forty, not eight: the lobe is cos^(2p), and a mountain-chain crest three or
            // four wavelengths long needs the half-width down near seven degrees -- p in the
            // twenties -- which is where the oceanographic spreading models put real swell. Eight
            // still left the long cascade's crests barely past one wavelength: domes.
            ui::Field("detail.crestlength", &o.crestLength, 1.0f, 40.0f, 3.0f, "", "%.1f");
            ui::Field("detail.upwind", &o.upwind, 0.0f, 1.0f, 0.08f, "", "%.3f");
            ui::Field("detail.speed", &o.timeScale, 0.0f, 3.0f, 0.55f, "x", "%.3f");
            ui::Field("detail.chop", &o.chop, 0.0f, 2.0f, 1.0f, "", "%.3f");
            ui::Field("detail.foldbudget", &o.foldBudget, 0.25f, 2.0f, 1.0f, "x", "%.2f");
            ui::Switch(Sw::DetailChop);
            ui::EndSection();
        }

        if (ui::Section("detail.nonlinear"))
        {
            ui::Note("detail.nonlinear.text");
            ui::Switch(Sw::Nonlinear);
            ui::Field("detail.crestpeaking", &o.stokes, 0.0f, 1.0f, 0.35f, "", "%.3f");
            ui::Field("detail.heightdrivengather", &o.lean, 0.0f, 1.5f, 0.60f, "", "%.3f");
            ui::Field("detail.bornonwave", &o.waveCoupling, 0.0f, 4.0f, 1.2f, "", "%.3f");
            ui::EndSection();
        }

        if (ui::Section("detail.foam"))
        {
            ui::Note("detail.foam.text");
            ui::Switch(Sw::OffshoreFoam);
            ui::Field("detail.foamthreshold", &o.foamThreshold, 0.0f, 0.95f, 0.45f, "", "%.3f");
            ui::Field("detail.foamlife", &o.foamLife, 0.1f, 20.0f, 3.5f, "s", "%.2f");
            ui::Field("detail.foamgain", &o.foamGain, 0.0f, 3.0f, 1.0f, "", "%.3f");
            ui::EndSection();
        }

        if (ui::Section("detail.storm"))
        {
            ui::Note("detail.storm.text");
            ui::Field("detail.stormamp",  &o.stormAmplitude, 1.0f, 6.0f, 2.2f, "x", "%.2f");
            ui::Field("detail.stormchop", &o.stormChop, 0.5f, 2.5f, 1.35f, "x", "%.2f");
            ui::Field("detail.stormfold", &o.stormFold, 1.0f, 2.0f, 1.30f, "x", "%.2f");
            ui::Field("detail.stormfoam", &o.stormFoam, 1.0f, 3.0f, 1.60f, "x", "%.2f");
            ui::EndSection();
        }

        if (ui::Section("detail.cascades"))
        {
            int seed = static_cast<int>(o.seed);
            // Its own control rather than a Field: this is an identity, not a magnitude. A slider
            // over it would invite dragging, and every intermediate value is a different sea.
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.44f);
            if (ImGui::InputInt(L("detail.seed.label"), &seed)) o.seed = static_cast<uint32_t>(seed);
            Help(L("detail.seed.help"));
            ui::EndSection();
        }

        if (!ui::Section("detail.whatitgot")) return;
        ui::Note("detail.whatitgot.text");

        for (int ci = 0; ci < w::kOceanCascades; ++ci)
        {
            const w::OceanCascadeReport& r = w::OceanReport(ci);

            ImGui::Text("Cascade %d: %.1f down to %.1f units, cell %.2f, tiling every %.0f",
                        ci, r.bandLong, r.bandShort, r.cell, r.patch);

            // HOW MANY VERTICES SHARE ONE CELL, which is the number that decides whether the field
            // arrives as a surface or as plateaus. The cell is set by the patch the component count
            // needs, not by the mesh, so on the coarse cascades it is several times the vertex
            // spacing -- and a vertex fetch has no filtering of its own. With the smoothing switch
            // on, the vertex stage rebuilds the field between cells and this is only a resolution;
            // with it off, it is literally the width of a flat step.
            const float perCell = r.cell / std::max(w::FinestVertexSpacing(), 1e-3f);
            if (perCell > 1.5f && !w::On(w::Sw::DetailFilter))
                ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.30f, 1.0f),
                                   "    %.1f vertices per cell and SMOOTHING IS OFF -- flat "
                                   "plateaus %.1f units across", perCell, r.cell);
            else
                ImGui::TextDisabled("    %.1f vertices per cell", perCell);

            // The height it was ALLOWED, which is routinely less than the ceiling asked and MUST
            // NOT be silently so: a refusal that only shows as a quieter sea looks exactly like a
            // broken transform. Yellow when the physics is holding the request down, and the reason
            // beside it -- the cap now divides in every gain that can still reach the geometry, so
            // "granted" is a promise about the worst case, not about the linear field.
            if (r.grantedRise < r.askedRise * 0.98f)
                ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.30f, 1.0f),
                                   "    %.2f units GRANTED of %.2f asked -- steepness cap, "
                                   "worst-case gain x%.2f counted in",
                                   r.grantedRise, r.askedRise, r.gainCeiling);
            else
                ImGui::TextDisabled("    %.2f units granted (worst-case gain x%.2f counted in)",
                                    r.grantedRise, r.gainCeiling);

            // How much of the field is living on the cusp limiter. A few percent is a sea that
            // breaks somewhere; tens of percent is chop asking far past what the band can carry,
            // and the surface is spending its life clamped -- turn the chop or the coupling down.
            if (r.clampedFrac > 0.10f)
                ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.30f, 1.0f),
                                   "    %.0f%% of cells AT THE CUSP LIMITER -- lower chop/coupling",
                                   r.clampedFrac * 100.0f);
            else if (r.clampedFrac > 0.0f)
                ImGui::TextDisabled("    %.1f%% of cells at the cusp limiter", r.clampedFrac * 100.0f);

            if (r.modes < 200)
                ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.30f, 1.0f),
                                   "    %d components -- a lattice, raise the span", r.modes);
            else
                ImGui::Text("    %d components", r.modes);

            // RESTITUTION: how many vertices the mesh offers one wave of this cascade at FULL
            // refinement. Under about four, a vertex receives a value nearly decorrelated from its
            // neighbour's -- per-vertex noise that animates as seething, never a travelling wave --
            // so the vertex stage fades such a cascade out of the geometry (per vertex, since the
            // spacing grows with distance) and it lives on in the shading, where resolution is the
            // pixel. This line is that fate, named instead of discovered on screen.
            const float lambdaRep = std::sqrt((r.bandLong > 0.0f && r.bandShort > 0.0f)
                                                  ? r.bandLong * r.bandShort : 0.0f);
            const float vertsPerWave = lambdaRep / w::FinestVertexSpacing();
            if (vertsPerWave < 4.0f)
                ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.30f, 1.0f),
                                   "    %.1f verts/wave at full refinement -- SHADING-ONLY "
                                   "(normal and foam, no geometry)", vertsPerWave);
            else if (vertsPerWave < 6.0f)
                ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.30f, 1.0f),
                                   "    %.1f verts/wave at full refinement -- marginal geometry, "
                                   "fades out just past the camera", vertsPerWave);
            else
                ImGui::TextDisabled("    %.1f verts/wave at full refinement -- geometry near the "
                                    "camera, shading beyond", vertsPerWave);
        }

        ui::Readout("Reach: %.2f units of rise right now", w::OceanMaxRise());
        ui::Note("detail.reachnote.text");
        ui::EndSection();
    }

    void TabMesh()
    {
        ui::FilterBox();

        if (ui::Section("mesh.refinement"))
        {
            ui::Note("mesh.refinement.text");

            int sub = w::Subdivision();
            if (ui::FieldInt("mesh.subdivision", &sub, 1, 8, 8, "x")) w::SetSubdivision(sub);

            // The bands, the fog fractions and the budget readout are gone with the grading:
            // every chunk gets the factor above, at every distance.
            ui::Readout("spacing %.2f u everywhere", w::FinestVertexSpacing());

            // Still here, and now for one thing only: a group the client has never re-emitted while
            // we were watching has never passed through our hands, so it draws as stock water.
            int rebuilds = w::RebuildBudget();
            if (ui::FieldInt("mesh.rebuildbudget", &rebuilds, 1, 64, 12, "/frame"))
                w::SetRebuildBudget(rebuilds);
            ui::EndSection();
        }

        if (ui::Section("mesh.culling"))
        {
            float cull = w::CullMargin();
            if (ui::Field("mesh.extramargin", &cull, 0.0f, 80.0f, 12.0f, "u", "%.1f"))
                w::SetCullMargin(cull);

            float nearAlways = w::NearAlwaysRadius();
            if (ui::Field("mesh.nearalways", &nearAlways, 0.0f, 300.0f, 0.0f, "u", "%.0f"))
                w::SetNearAlwaysRadius(nearAlways);
            ui::EndSection();
        }

    }

    /// WHERE THE SETTINGS LAND ON THE MAP, as opposed to what they say.
    ///
    /// The charts in the other tabs draw the RULES -- this curve against that depth, this band
    /// against that wavelength. What no chart can show is where those rules fall on the water in
    /// front of you, because every quantity they turn on is invisible: a column, a fold, how much
    /// chop actually arrived. These two views are that missing half, and they answer different
    /// parts of it -- the false colour is about the water, the wireframe is about the mesh.
    void TabDebug()
    {
        w::ShadeDebug& d = w::Debug();

        ui::Note("debug.intro.text");

        // A radio list rather than a combo: the whole point is switching between views while
        // watching the water, and a combo hides every option but the current one behind a click.
        for (int v = 0; v < w::ShadeDebug::Count; ++v)
        {
            char key[48];
            std::snprintf(key, sizeof(key), "debug.view%d", v);
            if (ImGui::RadioButton(ui::Text(key, "label"), d.view == v)) d.view = v;
            Help(ui::Text(key, "help"));
        }

        ImGui::Separator();
        if (d.view == w::ShadeDebug::Column)
        {
            ui::Field("debug.columnrange", &d.columnRange, 1.0f, 400.0f, 30.0f, "u", "%.1f");
            // The ramp itself, so a colour on the water can be read back as a number without
            // holding the mapping in your head.
            if (ui::BeginChart("##dbgramp", 34.0f))
            {
                constexpr int kSteps = 96;
                for (int i = 0; i < kSteps; ++i)
                {
                    const float t0 = static_cast<float>(i) / static_cast<float>(kSteps);
                    const float t1 = static_cast<float>(i + 1) / static_cast<float>(kSteps);
                    // The same three offset triangles the pixel stage builds the ramp from.
                    const float r = std::min(std::max(1.5f - std::fabs(4.0f * t0 - 3.0f), 0.0f), 1.0f);
                    const float g = std::min(std::max(1.5f - std::fabs(4.0f * t0 - 2.0f), 0.0f), 1.0f);
                    const float b = std::min(std::max(1.5f - std::fabs(4.0f * t0 - 1.0f), 0.0f), 1.0f);
                    ui::ChartRect(t0, 0.0f, t1, 1.0f,
                                  IM_COL32(static_cast<int>(r * 255.0f),
                                           static_cast<int>(g * 255.0f),
                                           static_cast<int>(b * 255.0f), 255));
                }
                for (int m = 1; m < 4; ++m)
                {
                    const float t = static_cast<float>(m) / 4.0f;
                    ui::ChartVLine(t, IM_COL32(255, 255, 255, 70));
                    ui::ChartText(t + 0.005f, 0.2f, IM_COL32(20, 20, 25, 220),
                                  "%.0f u", d.columnRange * t);
                }
                ui::EndChart();
            }
        }
        else if (d.view != w::ShadeDebug::Off)
        {
            ui::Note("debug.ramp.text");
        }

        ImGui::Separator();
        ImGui::Checkbox(L("debug.wireframe.label"), &d.wireframe);
        Help(L("debug.wireframe.help"));
    }

    /// The approach: one sea arriving on a beach, not two seas blended along a contour.
    ///
    /// Its own window rather than a strip inside Mesh, because it is where the map's bathymetry
    /// meets the sea state and neither tab owns that. Ordered as the water crosses it -- deep first,
    /// waterline last -- so reading down the panel is reading the wave in.
    void TabShore()
    {
        w::ShoreProfile& s = w::ShoreSettings();

        ui::Note("shore.intro.text");
        ShorePlot();
        ui::FilterBox();

        // THE TWO KNOBS THAT ANSWER THE QUESTION SOMEBODY ACTUALLY HAS, above everything else.
        //
        // One says how far out the whole approach happens, the other how hard the water cuts the
        // waves down. Between them they cover the request this tab exists for -- big water offshore,
        // small water on the beach -- and every control below is shape that is set once. The
        // ordering is the fix for a tab that was seven coupled depths in a flat list.
        if (ui::Section("shore.main"))
        {
            ui::Note("shore.main.text");
            ui::Field("shore.scale", &s.scale, 0.1f, 6.0f, 1.0f, "x", "%.2f");

            ui::Switch(Sw::DepthLimit);
            ui::Field("shore.breakerindex", &s.breakerIndex, 0.2f, 2.0f, 0.78f, "", "%.3f");

            // What that index means at THIS sea's height, because the index alone is a ratio and
            // the question anybody has is "from what depth does my sea start shrinking".
            const float totalRise = w::Frame().maxDisplacement + w::OceanMaxRise();
            if (totalRise > 1e-3f && s.breakerIndex > 1e-3f)
                ui::Readout("this sea (%.1f u of rise) starts being cut below %.0f u of water",
                            totalRise, totalRise / s.breakerIndex);

            // The scale is a multiplier and multipliers are unreadable on their own, so the three
            // depths it lands on are spelled out underneath it. This is the line to watch against
            // the plot's grey: a shore whose numbers sit outside the map's own range is a shore
            // nobody will ever swim through.
            const float sc = (s.scale > 0.01f) ? s.scale : 0.01f;
            ui::Readout("flat below %.1f u, full height at %.1f u, felt from %.0f u",
                        s.deadStart * sc, s.deadFull * sc, s.feelDepth * sc);
            ui::EndSection();
        }

        if (ui::Section("shore.standup"))
        {
            ui::Note("shore.standup.text");
            ui::Switch(Sw::Shoaling);
            ui::Field("shore.shoalrise", &s.shoalRise, 0.0f, 2.0f, 0.45f, "", "%.3f");
            ui::Field("shore.pinchrise", &s.pinchRise, 0.0f, 6.0f, 2.4f, "", "%.2f");
            ui::Switch(Sw::ShoreFoam);
            ui::Field("shore.surffoam", &s.surfFoam, 0.0f, 3.0f, 1.0f, "", "%.3f");
            ui::EndSection();
        }

        // Closed by default. These are the SHAPE of the approach -- a judgement made once and then
        // scaled by the knob above -- and having them open was most of why this tab read as seven
        // numbers with no obvious entry point.
        if (ui::Section("shore.shape", false))
        {
            ui::Note("shore.shape.text");
            ui::Field("shore.deadstart", &s.deadStart, 0.0f, 20.0f, 0.8f, "u", "%.2f");
            ui::Field("shore.deadfull", &s.deadFull, 0.5f, 60.0f, 6.0f, "u", "%.2f");
            // The band is a pair, and a pair can be inverted by hand. Said rather than silently
            // corrected: a start above the end is a smoothstep that never leaves zero, which is a
            // sea that has vanished for a reason nothing else in the panel would explain.
            if (s.deadStart >= s.deadFull)
                ui::Warn("the dead zone starts deeper than it ends -- no wave survives anywhere");
            ui::Field("shore.feeldepth", &s.feelDepth, 5.0f, 200.0f, 45.0f, "u", "%.1f");
            ui::Field("shore.choplead", &s.chopLead, 1.0f, 8.0f, 2.2f, "x", "%.2f");
            ui::EndSection();
        }
    }

    void TabEffects()
    {
        w::ShadeFx&      x = w::Fx();

        ui::Note("fx.intro.text");

        // The maps every effect below reads. Said whether or not they are there, because an effect
        // that is inert for want of its map looks exactly like an effect tuned to nothing.
        const w::FxStatus st = w::FxTextures();
        ui::Switch(Sw::ShadeMaps);
        if (!st.ready)
            ui::Warn("%s", L("fx.mapsmissing.text"));
        else
            ui::Readout("%.0f ms, %d grain frames, %d caustic slices",
                        st.buildMs, st.grainFrames, st.causticSlices);

        ui::FilterBox();

        if (ui::Section("fx.animnormals"))
        {
            ui::Switch(Sw::AnimNormals);
            ui::Field("fx.animstrength", &x.animStrength, 0.0f, 2.0f, 0.28f, "", "%.3f");
            ui::Field("fx.animscalefine", &x.animScaleFine, 0.005f, 0.2f, 0.045f, "/u", "%.4f");
            ui::Field("fx.animscalebroad", &x.animScaleBroad, 0.002f, 0.05f, 0.011f, "/u", "%.4f");
            ui::Field("fx.animfps", &x.animFps, 4.0f, 32.0f, 6.0f, "fps", "%.1f");
            ui::EndSection();
        }

        if (ui::Section("fx.breakupnoise"))
        {
            ui::Switch(Sw::Fbm);
            ui::Field("fx.noisestrength", &x.fbmStrength, 0.0f, 1.5f, 0.18f, "", "%.3f");
            ui::Field("fx.noisescale", &x.fbmScale, 0.0002f, 0.01f, 0.0021f, "/u", "%.5f");
            ui::EndSection();
        }

        if (ui::Section("fx.foamstructure"))
        {
            ui::Switch(Sw::FoamTex);
            ui::Field("fx.structurestrength", &x.foamTexStrength, 0.0f, 1.0f, 1.0f, "", "%.3f");
            ui::Field("fx.structurescale", &x.foamTexScale, 0.01f, 0.2f, 0.043f, "/u", "%.4f");
            ui::EndSection();
        }

        if (ui::Section("fx.skyreflection"))
        {
            ui::Switch(Sw::Sky);
            ui::Field("fx.panoramablend", &x.skyStrength, 0.0f, 1.0f, 0.35f, "", "%.3f");
            ui::EndSection();
        }

        if (ui::Section("fx.sunglitter"))
        {
            w::SurfaceLook& look = w::Look();
            ui::Switch(Sw::Glitter);
            ui::Field("fx.glitterstrength", &look.glintStrength, 0.0f, 6.0f, 0.45f, "", "%.3f");
            ui::Field("fx.glitterscale", &look.glintScale, 0.005f, 0.2f, 0.035f, "/u", "%.4f");
            ui::Field("fx.glitterthreshold", &look.glintThreshold, 0.6f, 1.6f, 1.02f, "", "%.3f");
            ui::Field("fx.glittersharpness", &look.glintSharpness, 1.0f, 40.0f, 8.0f, "", "%.1f");
            ui::Field("fx.glitterfadestart", &look.glintFadeStart, 0.0f, 300.0f, 40.0f, "u", "%.0f");
            ui::EndSection();
        }

        if (ui::Section("fx.detilegroup"))
        {
            ui::Field("fx.detile", &x.detileWarp, 0.0f, 0.3f, 0.018f, "", "%.4f");
            ui::EndSection();
        }

        if (ui::Section("fx.crestthroughlight"))
        {
            ui::Switch(Sw::Sss);
            ui::Field("fx.sssstrength", &x.sssBoost, 0.0f, 4.0f, 1.0f, "", "%.3f");
            ui::Field("fx.ssslobe", &x.sssLobe, 1.0f, 12.0f, 3.0f, "", "%.2f");
            ui::Field("fx.sssthinness", &x.sssThickness, 0.0f, 2.0f, 0.5f, "", "%.3f");
            ui::Field("fx.ssslowsunfalloff", &x.sssSunFalloff, 0.0f, 4.0f, 1.5f, "", "%.2f");
            ui::EndSection();
        }

        if (ui::Section("fx.caustics"))
        {
            ui::Switch(Sw::Caustics);
            ui::Field("fx.causticstrength", &x.causticStrength, 0.0f, 2.0f, 0.6f, "", "%.3f");
            ui::Field("fx.causticspeed", &x.causticSpeed, 0.0f, 8.0f, 1.6f, "x", "%.2f");
            ui::Field("fx.causticscale", &x.causticScale, 0.005f, 0.3f, 0.05f, "/u", "%.4f");
            ui::Field("fx.causticscale2", &x.causticScale2, 1.05f, 6.0f, 2.37f, "x", "%.3f");
            ui::Field("fx.causticsunpower", &x.causticSunPower, 0.0f, 4.0f, 1.0f, "", "%.2f");
            ui::Field("fx.causticdepthfade", &x.causticDepthFade, 0.0f, 0.6f, 0.12f, "/u", "%.3f");
            ui::Field("fx.causticsurfacestrength", &x.causticSurface, 0.0f, 2.0f, 0.0f,
                      "", "%.3f");
            ui::Field("fx.causticsurfacefade", &x.causticSurfaceFade, 0.0f, 0.6f, 0.0f,
                      "/u", "%.3f");
            ui::Field("fx.causticviewbias", &x.causticViewBias, 0.0f, 1.0f, 1.0f, "", "%.3f");
            ui::EndSection();
        }

        // The stylisation and the lace have no switches on purpose: every channel is neutral at
        // zero, so the slider IS the switch and a saved profile from before they existed reads
        // back as the physical sea unchanged.
        if (ui::Section("fx.stylisation"))
        {
            ui::Field("fx.cartoon", &x.styleCartoon, 0.0f, 1.0f, 0.0f, "", "%.2f");
            ui::Field("fx.clarityfloor", &x.clarityFloor, 0.0f, 0.8f, 0.0f, "", "%.2f");
            ui::Field("fx.repaint", &x.styleRepaint, 0.0f, 1.0f, 0.0f, "", "%.2f");
            ui::Field("fx.grazing", &x.styleGrazing, 0.0f, 0.5f, 0.0f, "", "%.2f");
            ui::Field("fx.sundiffuse", &x.styleSunDiffuse, 0.0f, 2.0f, 0.0f, "", "%.2f");
            ui::Field("fx.refractstyle", &x.refractRetail, 0.0f, 1.0f, 0.0f, "", "%.2f");
            ui::Field("fx.refractthickness", &x.refractThickness, 0.1f, 10.0f, 2.0f, "/u", "%.2f");
            ui::Field("fx.refractamount", &x.refractAmount, 0.0f, 3.0f, 1.2f, "", "%.2f");
            ui::Field("fx.ssrstrength", &x.ssrStrength, 0.0f, 1.0f, 0.0f, "", "%.2f");
            ui::Field("fx.ssrdistance", &x.ssrDistance, 16.0f, 512.0f, 128.0f, "u", "%.0f");
            ui::Field("fx.ssrripple", &x.ssrRipple, 0.0f, 1.0f, 0.35f, "", "%.2f");
            ui::EndSection();
        }

        if (ui::Section("fx.oceanlace"))
        {
            ui::Field("fx.lacestrength", &x.laceStrength, 0.0f, 2.0f, 0.55f, "", "%.2f");
            ui::Field("fx.lacescale", &x.laceScale, 0.005f, 0.5f, 0.24f, "/u", "%.3f");
            ui::Field("fx.lacewobble", &x.laceWobble, 0.0f, 3.0f, 0.35f, "", "%.2f");
            ui::Field("fx.lacecrest", &x.laceCrestBoost, 0.0f, 4.0f, 0.55f, "", "%.2f");
            ui::Field("fx.lacedepthfade", &x.laceDepthFade, 0.0f, 0.5f, 0.0f, "/u", "%.3f");
            ui::Field("fx.lacedistfade", &x.laceDistFade, 0.001f, 0.05f, 0.0045f, "/u", "%.4f");
            ui::EndSection();
        }
    }

    // Retail is the sole shipped water style.
    void ApplyRetailWater()
    {
        g_retailProfileVersion = 2.0f;
        // The stylisation full on: soft bounded Fresnel, the reflection mostly repainted, and a
        // third of the scene surviving the medium however deep the water runs.
        w::ShadeFx& x = w::Fx();
        x.styleCartoon = 1.0f;
        x.clarityFloor = 0.30f;
        x.styleRepaint = 0.60f;
        // The reference's own numbers: a tenth off the body at grazing, full daylight worked
        // into sun-facing slopes, and the screen-anchored refraction whose reach follows the
        // thickness of water behind the surface.
        x.styleGrazing = 0.10f;
        x.styleSunDiffuse = 1.0f;
        x.refractRetail = 1.0f;
        x.refractThickness = 2.0f;
        x.refractAmount = 1.2f;
        // Reflection strength at full, with a 128-unit trace limit and a third of
        // the wave normal surviving into the reflected ray.
        x.ssrStrength = 1.0f;
        x.ssrDistance = 128.0f;
        x.ssrRipple = 0.35f;
        x.laceStrength = 0.85f;
        // The stock cadence: the client's own ocean repeats its sheet about once per liquid
        // cell, a shade over four units.
        x.laceScale = 0.24f;
        x.laceWobble = 1.0f;
        x.laceCrestBoost = 1.0f;
        x.laceDepthFade = 0.0f;
        x.laceDistFade = 0.0067f;

        // Everything the reference does not draw, off through its own strength.
        x.causticStrength = 0.0f;
        x.causticSurface = 0.0f;
        x.sssBoost = 0.0f;

        // The shading carries the sea now: grain and ripple up where the geometry stood down.
        x.animStrength = 1.0f;
        x.fbmStrength = 0.35f;
        x.skyStrength = 0.40f;

        w::SurfaceLook& l = w::Look();
        // Weak extinction on purpose: with the clarity floor holding the bottom visible, a strong
        // curve would only decide how fast the water reaches a floor it can no longer pass.
        l.extinction[0] = 0.14f;
        l.extinction[1] = 0.045f;
        l.extinction[2] = 0.030f;
        l.bottomBrightness = 1.15f;
        l.glowStrength = 0.0f;
        l.troughShade = 0.0f;
        l.rippleStrength = 0.8f;
        l.glintStrength = 1.0f;
        l.sunIntensity = 0.9f;
        l.specularPower = 90.0f;

        // A low, long, slow support: presence without sculpture. EVERY shape field is written,
        // not only the ones that differ from the build defaults -- a preset that half-writes the
        // sea inherits whatever the previous preset left, and "Adventure then Retail" would keep
        // the sculpted bands under the painted shading.
        w::SeaResponse& r = w::Response();
        r.calm.windSpeed = 14.0f;
        r.calm.amplitude = 0.20f;
        r.calm.steepness = 0.15f;
        r.calm.directionSpread = 90.0f;
        r.calm.minWavelength = 12.0f;
        r.calm.crestSpacing = 3.0f;
        r.calm.swellLength = 160.0f;
        r.calm.swellShare = 0.20f;
        r.calm.exposure = 1.0f;
        r.storm = r.calm;
        r.storm.windSpeed = 20.0f;
        r.storm.amplitude = 0.40f;
        r.storm.steepness = 0.20f;
        r.storm.directionSpread = 70.0f;

        // The detail field kept for texture, not for waves: a short band, mild alignment, no
        // drama. The lace and the grain carry the surface; this only keeps light moving on it.
        w::OceanTuning& o = w::OceanSettings();
        o.amplitude = 0.15f;
        o.shortest = 1.5f;
        o.longest = 12.0f;
        o.cellSize = 1.0f;
        o.align[0] = 0.60f; o.align[1] = 0.50f; o.align[2] = 0.45f; o.align[3] = 0.40f;
        o.speed[0] = 1.00f; o.speed[1] = 1.05f; o.speed[2] = 1.10f; o.speed[3] = 1.20f;
        o.falloff = 3.0f;
        o.crestLength = 2.0f;
        o.upwind = 0.3f;
        o.timeScale = 1.0f;
        o.chop = 1.0f;
        o.foldBudget = 0.8f;
        o.stokes = 0.2f;
        o.lean = 0.3f;
        o.waveCoupling = 0.3f;
        o.foamThreshold = 0.55f;
        o.foamLife = 3.0f;
        o.foamGain = 0.8f;
        o.strength = 1.0f;
        // Retail must not inherit the experimental 5x-height/2x-chop storm response.
        o.stormAmplitude = 1.25f;
        o.stormChop = 1.0f;
        o.stormFold = 1.0f;
        o.stormFoam = 1.0f;

        // Foam pulled to the shore, where the reference keeps all of its white. Restated in
        // full for the same self-containment reason as the weather above.
        w::FoamShape& f = w::Foam();
        f.breakThreshold = 0.70f;
        f.breakSharpness = 4.0f;
        f.shoreSlopeMin = 0.12f;
        f.shoreSlopeGain = 0.8f;
        f.shoreDepthFade = 0.12f;
        f.shoreStrength = 1.8f;
        f.maskFloor = 0.55f;
        f.maskContrast = 0.45f;
        f.warp = 0.25f;
        f.density = 2.2f;

        w::SnapSea();
    }

    /// The share row: the whole sea as one pasteable token, and the panel's language.
    void DrawProfileRow()
    {
        static char s_key[4096] = {};
        static char s_note[192] = {};
        static bool s_noteBad = false;

        if (ImGui::Button(L("profile.copy")))
        {
            const std::string key = ExportProfile();
            const size_t n = (key.size() < sizeof(s_key) - 1) ? key.size() : sizeof(s_key) - 1;
            std::memcpy(s_key, key.c_str(), n);
            s_key[n] = '\0';
            ImGui::SetClipboardText(s_key);
            std::snprintf(s_note, sizeof(s_note), "%s", L("profile.copied"));
            s_noteBad = false;
        }
        Help(L("profile.copy.help"));

        ImGui::SameLine();
        if (ImGui::Button(L("profile.paste")))
        {
            if (const char* clip = ImGui::GetClipboardText())
            {
                const size_t n = strnlen(clip, sizeof(s_key) - 1);
                std::memcpy(s_key, clip, n);
                s_key[n] = '\0';
            }
            s_noteBad = !ImportProfile(s_key);
            if (s_noteBad)
                std::snprintf(s_note, sizeof(s_note), "%s", L("profile.rejected"));
            else if (g_importCarried == g_importKnown)
                std::snprintf(s_note, sizeof(s_note), "%s", L("profile.applied"));
            else
                // A key from another build. Said with both numbers, because "applied" alone would
                // hide the settings that kept their previous value -- and a setting that stayed
                // behind is invisible.
                std::snprintf(s_note, sizeof(s_note), L("profile.appliedpartial"),
                              (g_importCarried < g_importKnown) ? g_importCarried : g_importKnown,
                              g_importKnown);
        }
        Help(L("profile.paste.help"));

        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        int lang = (Language() == Lang::French) ? 1 : 0;
        if (ImGui::Combo("##lang", &lang, "English\0Francais\0"))
            SetLanguage(lang == 1 ? Lang::French : Lang::English);

        if (s_note[0])
        {
            ImGui::SameLine();
            if (s_noteBad) ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1.0f), "%s", s_note);
            else           ImGui::TextDisabled("%s", s_note);
        }

        // Shown rather than hidden behind the clipboard: a clipboard that did not take is silent,
        // and the field is also where a key arrives when somebody types it in by hand.
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##profilekey", s_key, sizeof(s_key));
    }

    /// The Build window: what a sea is saved, shared and started from. No settings of its own.
    void DrawBuild()
    {
        DrawProfileRow();
        ImGui::Separator();

        // Above the tabs on purpose: it belongs to none of them and applies to all of them, and a
        // save button buried inside one tab is a save button somebody forgets exists.
        if (ImGui::Button("Save")) SaveSettings();
        ImGui::SameLine();
        if (ImGui::Button("Load")) LoadSettings();
        ImGui::SameLine();
        if (ImGui::Button("Reset Retail water")) ApplyRetailWater();
        if (ImGui::BeginItemTooltip())
        {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
            ImGui::TextUnformatted(
                "The sea as the modern client draws its own: geometry nearly still, every visible "
                "wave living in the shading instead.\n\n"
                "Opacity is AUTHORED -- a third of the scene survives the medium however deep the "
                "water runs. The mirror is soft and bounded, and mostly repainted in the water's "
                "own colour. The animated lace sheet rides on top, boiling in place. And what the "
                "reference does not draw is off here too: no caustics, no light through crests, "
                "no trough shading.\n\n"
                "It overwrites the stylisation, the extinction, both weather ends and the detail "
                "field's height. It leaves the palette and the mesh alone -- those belong to your "
                "map. Nothing is written to disk until you press Save.");
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", kSettingsFile);
        if (ImGui::BeginItemTooltip())
        {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
            ImGui::TextUnformatted(
                "Written beside the client, and read back automatically at startup. Everything in "
                "every tab goes in one file.\n\n"
                "A key the file does not carry leaves its field at the default, so a file saved by "
                "an older build still loads into a newer one -- whatever was added since keeps its "
                "default rather than arriving as zero, which for half of these is an invisible sea.");
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }

    }

    /// EVERY SWITCH, walked from the registry rather than written out here.
    ///
    /// A contribution added to the sea appears in this window and in a shared profile on the same
    /// edit. Its own window rather than a strip inside another, because isolating an artefact means
    /// clicking switches WHILE watching the controls that might own it.
    void DrawSwitches()
    {
        if (ImGui::Button(L("btn.allon"))) w::AllOn();
        Help(L("btn.allon.help"));
        ImGui::Separator();

        // Three to a row: there are twenty-two, and a column of twenty-two checkboxes is a list
        // nobody reads.
        int col = 0;
        for (int i = 0; i < static_cast<int>(w::Sw::Count); ++i)
        {
            const w::Sw s = static_cast<w::Sw>(i);
            if (col++ % 3 != 0) ImGui::SameLine(static_cast<float>((col - 1) % 3) * 175.0f + 8.0f);
            w::ui::Switch(s);
        }
    }

    // ---- the workspace --------------------------------------------------------------------------
    //
    // A toolbar and a window per category, rather than one window with a tab strip.
    //
    // A tab strip can only ever show ONE category, and tuning water is a matter of moving a control
    // in one while watching a readout in another -- the detail field against the foam it feeds, the
    // mesh against the cell size it bounds. With tabs that comparison costs a click each way and
    // has to be held in memory across it.
    //
    // Each window carries its own geometry and open state, remembered between sessions by ImGui's
    // ini, so an arrangement somebody built is an arrangement they keep.

    struct Category
    {
        const char* key;      ///< string-table key AND the window's ImGui identity
        void      (*body)();
        bool        open;
    };

    // Ordered by what a sea is BUILT from, outermost first: the weather decides the sea, the sea
    // decides the waves, the waves are drawn by a mesh, and the rest is shading.
    Category g_cats[] = {
        { "tab.build",    &DrawBuild,   true  },
        { "tab.weather",  &TabWeather,  false },
        { "tab.seastate", &TabSeaState, false },
        { "tab.detail",   &TabDetail,   false },
        { "tab.shore",    &TabShore,    false },
        { "tab.mesh",     &TabMesh,     false },
        { "tab.surface",  &TabSurface,  false },
        { "tab.foam",     &TabFoam,     false },
        { "tab.effects",  &TabEffects,  false },
        { "tab.debug",    &TabDebug,    false },
        { "tab.switches", &DrawSwitches, false },
    };
    constexpr int kCatCount = static_cast<int>(sizeof(g_cats) / sizeof(g_cats[0]));

    void DrawAdvanced()
    {
        // The toolbar. Auto-resized and without a scrollbar so it stays the size of its buttons:
        // it is a launcher, and a launcher that needs scrolling has stopped being one.
        ImGui::SetNextWindowSize(ImVec2(0.0f, 0.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("WarcraftXL -- Sea", nullptr,
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar))
        {
            for (int i = 0; i < kCatCount; ++i)
            {
                if (i) ImGui::SameLine();
                // The button reads as pressed while its window is open, so the toolbar is also the
                // answer to "what have I got open behind this".
                const bool on = g_cats[i].open;
                if (on) ImGui::PushStyleColor(ImGuiCol_Button,
                                              ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                if (ImGui::Button(w::ui::Text(g_cats[i].key, "label")))
                    g_cats[i].open = !g_cats[i].open;
                if (on) ImGui::PopStyleColor();
            }

            ImGui::Separator();
            if (ImGui::Button(L("btn.save"))) SaveSettings();
            ImGui::SameLine();
            if (ImGui::Button(L("btn.load"))) LoadSettings();
            ImGui::SameLine();
            if (ImGui::Button(L("btn.reloadlang"))) w::LoadStrings();
            Help(L("btn.reloadlang.help"));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(96.0f);
            int lang = (Language() == Lang::French) ? 1 : 0;
            if (ImGui::Combo("##lang", &lang, "English\0Francais\0"))
                SetLanguage(lang == 1 ? Lang::French : Lang::English);

            // The string table's own health, where somebody editing it is already looking.
            const int miss = w::MissingCount(Language()) + w::UnknownCount();
            if (miss > 0)
                w::ui::Warn("%d strings missing", miss);
        }
        ImGui::End();

        for (int i = 0; i < kCatCount; ++i)
        {
            if (!g_cats[i].open) continue;
            ImGui::SetNextWindowSize(ImVec2(540.0f, 560.0f), ImGuiCond_FirstUseEver);
            // The key is appended as an ImGui id so the TITLE can be translated without the window
            // losing the geometry it was given under the other language.
            char title[128];
            std::snprintf(title, sizeof(title), "%s###%s",
                          w::ui::Text(g_cats[i].key, "label"), g_cats[i].key);
            if (ImGui::Begin(title, &g_cats[i].open))
            {
                w::ui::FilterBox();
                ImGui::Spacing();
                g_cats[i].body();
            }
            ImGui::End();
        }
    }

    bool g_showAdvanced = false;
    std::string g_startingProfile;
    const char* g_simpleNote = "";
    const char* SimpleText(const char* english, const char* french)
    { return Language() == Lang::French ? french : english; }

    // Sliders only write on user input: merely opening this panel preserves custom values,
    // including values outside the simplified range that were authored in Advanced tuning.
    bool SimpleAmount(const char* label, float& value, float maximum)
    {
        float percent = value * (100.0f / maximum);
        if (!ImGui::SliderFloat(label, &percent, 0.0f, 100.0f, "%.0f%%")) return false;
        value = percent * (maximum / 100.0f);
        g_simpleNote = "";
        return true;
    }

    void DrawAll(void*)
    {
        w::ui::NewFrame();
        ImGui::SetNextWindowSize(ImVec2(440.0f, 380.0f), ImGuiCond_FirstUseEver);
        const char* title = SimpleText("Water settings###water-simple", "Reglages de l'eau###water-simple");
        if (ImGui::Begin(title))
        {
            if(w::forever::Panel()) { ImGui::End(); return; }
            ImGui::TextWrapped("%s", w::EnhancementsEnabled()
                ? SimpleText("Enhanced water active. F11 compares with native water.", "Eau amelioree active. F11 compare avec l'eau native.")
                : SimpleText("F11: native water active. Your saved look is preserved.", "F11 : eau native active. Vos reglages sont conserves."));
            ImGui::TextWrapped("%s", w::EnhancementsEnabled()
                ? SimpleText("Adjust the look of the water. Changes appear immediately.", "Ajustez l'apparence de l'eau. Les changements sont immediats.")
                : SimpleText("Changes become visible when you restore enhanced rendering.", "Les changements seront visibles au retour du rendu ameliore."));
            ImGui::Spacing();
            ImGui::SetNextItemWidth(220);
            if (SimpleAmount(SimpleText("Clarity", "Transparence"), w::Fx().clarityFloor, 0.8f))
            {
                w::SetOn(Sw::Bottom, true);
                w::SetOn(Sw::Refraction, true);
            }
            Help(SimpleText("Higher keeps more underwater detail visible, especially in deep water.",
                "Plus eleve : davantage de details visibles sous l'eau, surtout en profondeur."));
            ImGui::SetNextItemWidth(220);
            SimpleAmount(SimpleText("Shore reflections", "Reflets du rivage"), w::Fx().ssrStrength, 1.0f);
            Help(SimpleText("Depth-tested reflections of visible objects. Requires depth capture; off-screen objects use the sky fallback.",
                "Reflets des objets visibles avec test de profondeur. Necessite la capture de profondeur ; hors ecran, le ciel prend le relais."));
            ImGui::SetNextItemWidth(220);
            if (SimpleAmount(SimpleText("Sky reflections", "Reflets du ciel"), w::Fx().skyStrength, 1.0f)) w::SetOn(Sw::Sky, true);
            Help(SimpleText("Sky texture reflected in the surface. Zero keeps the smooth sky colour and sun lighting.",
                "Texture du ciel refletee dans l'eau. Zero conserve la couleur uniforme du ciel et la lumiere solaire."));
            ImGui::SetNextItemWidth(220);
            if (SimpleAmount(SimpleText("Foam", "Ecume"), w::Look().foam, 2.0f)) w::SetOn(Sw::Foam, true);
            Help(SimpleText("Amount of white foam on the water.", "Quantite d'ecume blanche sur l'eau."));
            ImGui::SetNextItemWidth(220);
            if (SimpleAmount(SimpleText("Ripples", "Ondulations"), w::Look().rippleStrength, 2.0f)) w::SetOn(Sw::Ripple, true);
            Help(SimpleText("Small surface detail. This does not change wave height or swimming.",
                "Petits details de surface. Ne change ni la hauteur des vagues ni la nage."));
            ImGui::Separator();
            if (ImGui::Button(SimpleText("Save", "Enregistrer")))
                g_simpleNote = SaveSettings() ? "saved" : "save-failed";
            ImGui::SameLine();
            if (ImGui::Button(SimpleText("Restore starting look", "Retablir l'apparence initiale")))
            {
                const bool restored = ImportProfile(g_startingProfile.c_str());
                if (restored)
                {
                    g_override = false;
                    w::ReleaseForcedConditions();
                    w::LightPreview().enabled = false;
                }
                g_simpleNote = restored ? "restored" : "restore-failed";
            }
            ImGui::TextWrapped("%s", SimpleText("Save keeps your changes for next time. Restore returns to the settings loaded at startup.",
                "Enregistrer conserve vos changements. Retablir reprend les reglages charges au demarrage."));
            if (!std::strcmp(g_simpleNote, "reloaded")) ImGui::TextUnformatted("F12: saved water settings loaded.");
            if (!std::strcmp(g_simpleNote, "saved")) ImGui::TextUnformatted(SimpleText("Saved.", "Enregistre."));
            if (!std::strcmp(g_simpleNote, "restored")) ImGui::TextUnformatted(SimpleText("Starting look restored. Not saved yet.", "Apparence initiale retablie. Pas encore enregistree."));
            if (!std::strcmp(g_simpleNote, "save-failed") || !std::strcmp(g_simpleNote, "restore-failed"))
                ImGui::TextUnformatted(SimpleText("Could not apply that action. See the client log.", "Action impossible. Consultez le journal du client."));
            ImGui::Spacing();
            if (ImGui::CollapsingHeader("Water reflection test (temporary)"))
            {
                auto& preview = w::LightPreview();
                ImGui::Checkbox("Override water light", &preview.enabled);
                ImGui::TextWrapped("Changes the water's test light only. The visible sun and sky stay at their real positions. Not saved; disable to restore normal lighting.");
                if (preview.enabled)
                {
                    ImGui::SliderFloat("Test elevation", &preview.elevation, 5.0f, 80.0f, "%.0f deg");
                    ImGui::SliderFloat("Test direction", &preview.bearingOffset, -180.0f, 180.0f, "%.0f deg");
                    if (ImGui::Button("Low sun (10 deg)"))
                    { preview.elevation = 10.0f; preview.bearingOffset = 0.0f; }
                    ImGui::SameLine();
                    if (ImGui::Button("Use real sun")) preview.enabled = false;
                }
            }
            ImGui::Checkbox(SimpleText("Advanced tuning", "Reglages avances"), &g_showAdvanced);
            if (g_showAdvanced) ImGui::TextWrapped("%s", SimpleText("Detailed controls open separately. Turn this off to hide them all.",
                "Les controles detailles s'ouvrent separement. Decochez pour tous les masquer."));
        }
        ImGui::End();
        if (g_showAdvanced) DrawAdvanced();
    }

    void OnPreviewWorldLeave(void*,const void*) { w::LightPreview().enabled = false; }

    void OnReloadInput(void*,const void* args) {
        const auto* a=static_cast<const wxl::events::InputArgs*>(args);
        static wxl::render::ReloadKey key;
        if(key.Handle(a->message,a->wparam,a->handled)) {
            const bool loaded=LoadSettings();
            g_simpleNote=loaded ? "reloaded" : "restore-failed";
            WLOG_INFO("render-reload: water loaded=%u",unsigned(loaded));
        }
    }
    bool Install()
    {
        // Opened at a size that fits on screen and scrolls, rather than at the width of its widest
        // paragraph. Only the first launch: after that the window's own geometry wins.
        // Own the compact window and optional advanced category windows together.
        wxl::ui::AddWindowOwner("Water", &DrawAll, nullptr);

        // Strings first: everything below can report through the panel, and a panel whose labels
        // are all raw keys is a panel nobody can read the report in.
        w::LoadStrings();
        wxl::events::Subscribe(wxl::events::Event::OnInput,OnReloadInput,nullptr);
        wxl::events::Subscribe(wxl::events::Event::OnWorldLeave,OnPreviewWorldLeave,nullptr);

        // Read at install, so a tuned sea survives a rebuild without anyone having to remember to
        // press anything. Its absence is not a fault -- it just means nothing has been saved yet.
        ApplyRetailWater();
        if (!LoadSettings())
            WLOG_INFO("water: using Retail baseline");

        // Lossless existing profile format includes switches and persisted advanced settings.
        // Keep this snapshot in memory; opening the simplified UI never rewrites a profile.
        g_startingProfile = ExportProfile();

        WLOG_INFO("water: tuning panel registered");
        return true;
    }
}

WXL_REGISTER_FEATURE("water-tuner", wxl::features::water, Install)
