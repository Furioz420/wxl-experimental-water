// The liquid lattice, subdivided.
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

#include "wxl-experimental-water/mesh/Lattice.hpp"

#include "wxl-experimental-water/mesh/Owned.hpp"
#include "wxl-experimental-water/sea/Ocean.hpp"
#include "wxl-experimental-water/sea/Sea.hpp"
#include "common/Log.hpp"
#include "game/World.hpp"
#include "engine/hook/Hook.hpp"
#include "offsets/engine/Gx.hpp"
#include "offsets/engine/Liquid.hpp"

#include <cmath>
#include <cstring>

namespace
{
    namespace liq   = wxl::offsets::engine::liquid;
    namespace gxoff = wxl::offsets::engine::gx;
    using namespace wxl::water;

    /// Refined vertices one batch may produce.
    ///
    /// A HARD CEILING, not a budget, and not ours to raise: the surrounding geometry addresses this
    /// lattice with sixteen-bit indices, so a vertex numbered 65536 cannot be referenced at all.
    ///
    /// Measured headroom against it is large -- a batch peaks at four chunks, which at the full
    /// factor is 16900 vertices -- so the subdivision does not have to be rationed to fit. The guard
    /// in Densify exists for the batch that has never yet been seen, not for the common case.
    constexpr int kMaxOutVerts = 63488;

    /// Flat allowance added to the culling bounds, on top of the sea's measured excursion.
    ///
    /// It buys against LATENCY, not against amplitude: the box is widened with the wave heights of
    /// the frame being built, and tested against a frustum that has already moved by the time the
    /// chunk would be drawn. Turning the camera quickly is the case that fails, and it fails as a
    /// hole at the edge of the screen -- water that was correctly culled a frame ago and should
    /// have been drawn this one. No measurement covers that; a constant does.
    float g_cullMargin = 12.0f;

    /// Radius around the eye inside which a chunk is never culled, in world units.
    ///
    /// A FLAT SLAB IS A BAD STAND-IN FOR NEAR WATER. The bounds describe the still-water plane, and
    /// the further the surface departs from that plane the less the box says about where it is. Far
    /// off, being wrong costs a chunk nobody would have picked out. Close in, one chunk is a large
    /// part of the screen -- and the water the player is standing in is exactly the water they are
    /// looking at, so the same error is a hole in the middle of the view.
    ///
    /// Swimming is where it shows. The eye sits inside the water volume, the surface is a ceiling
    /// running past overhead, and the chunks carrying it are the ones whose slab is closest to
    /// degenerate against the frustum.
    ///
    /// OFF, because the measurement says there is nothing here to fix. Every instance the cull
    /// considers already survives both bound tests -- the nearest considered and the nearest
    /// surviving are the same number -- so this buys overdraw and no water. Kept as a control
    /// because it is the right lever if a map ever does lose near chunks to the bounds, and the
    /// counters in the log are what would say so.
    float g_nearAlways = 0.0f;

    Batch g_batch;
    /// ONE FACTOR FOR EVERY CHUNK AT EVERY DISTANCE. Varying it by distance is not available here:
    /// a group's refinement is baked into buffers the surrounding engine caches and hands back
    /// unchanged for most draws, so whatever factor a chunk is built with is the factor it keeps for
    /// as long as it stays loaded. A uniform one is the only kind that cannot be stale.
    int   g_subdiv = 8;

    /// Column reported where the sea floor could not be resolved.
    ///
    /// Deep, deliberately. Attenuation exists to calm water that is measurably shallow; a failed
    /// measurement is not a shallow one, and defaulting the other way would flatten the whole sea
    /// the moment the terrain grid is unavailable -- which is a far harder fault to recognise than a
    /// wave running slightly too tall over an unresolved patch.
    constexpr float kUnknownDepth = 1000.0f;

    /// WHERE THIS MAP'S WATER ACTUALLY IS, in depth, over the water drawn last frame.
    ///
    /// Measured because the shore settings are all expressed as depths and a depth is the one
    /// quantity in this whole system that cannot be seen. Tuning a transition at twelve units on a
    /// map whose water runs from two to four hundred is guessing, and the panel had no way to say
    /// so. Costs one compare per SOURCE vertex, on a loop that is already touching each of them.
    ///
    /// Last frame's rather than this one's: the panel draws before the water does, so reading the
    /// live counters would show a half-filled histogram that flickers.
    constexpr int kDepthBuckets = 48;
    /// Log-spaced, because the interesting range spans three decades -- a beach is under a unit and
    /// open ocean is hundreds -- and linear buckets would put the entire shore in the first one.
    constexpr float kDepthBucketMax = 400.0f;
    int   g_depthHist[kDepthBuckets] = {};
    int   g_depthLive[kDepthBuckets] = {};
    int   g_depthCount = 0;

    void NoteDepth(float depth)
    {
        if (depth < 0.0f || depth >= kUnknownDepth) return;
        ++g_depthCount;

        // log1p keeps the bucket edges meaningful at both ends: the first few cover tenths of a
        // unit, where the dead zone lives, and the last few cover the deep ocean nobody tunes.
        const float t = std::log(1.0f + depth) / std::log(1.0f + kDepthBucketMax);
        int b = static_cast<int>(t * kDepthBuckets);
        if (b < 0) b = 0;
        if (b >= kDepthBuckets) b = kDepthBuckets - 1;
        ++g_depthLive[b];
    }

    // ---- getting a chunk group refined AT ALL ---------------------------------------------------
    //
    // THE PROVIDER ANSWERS FROM ITS OWN CACHE, and that is what was ending the sea at a line.
    //
    // Refinement rides on the client re-running its vertex emitter: that is the only moment a chunk's
    // vertices pass through our hands. But the provider only does that when it considers its cached
    // buffers stale. Most draws it returns the pair it built earlier and emits nothing -- so the
    // rebuild below is handed a batch with no captured geometry and can do nothing with it.
    //
    // Which means a group we did not happen to catch during the brief window where the client built
    // it stays stock water FOREVER. Not too coarse, not badly lit -- flat, with no wave in it, while
    // the water nearer the camera moves. That is the boundary, and it sits wherever the loading
    // happened to fall rather than at any distance.
    //
    // The provider's own dirty flag is the way back in: set it, and the next draw of that group takes
    // the full rebuild path and comes through the emitter. Once is enough, and this is where our
    // design costs less than it might. The waves live in the VERTEX STAGE, so a refined lattice keeps
    // moving on its own forever -- nothing has to be rebuilt to keep the sea alive, only to make it
    // dense the first time. A CPU-displaced sea would have to force every group on a timer.
    struct GeomNote
    {
        const void* geom;
        uint8_t     state;   ///< 1 asked once, 2 settled -- either way, never asked again
    };
    /// Raised with the hash below. Direct-mapped means every collision is an eviction, and the cost
    /// of a slot is a pointer and a few bytes -- so buying headroom here is cheaper than any of the
    /// ways a wrong answer shows up on screen.
    constexpr int kNoteSlots = 16384;
    GeomNote g_notes[kNoteSlots] = {};

    /// MIXED, NOT SLICED. These are pool allocations: they share their high bits and are spaced by
    /// a stride that is very often a power of two, so a raw window of such a pointer maps whole runs
    /// of distinct groups onto the same slot -- and the table is direct-mapped, so every collision
    /// is an eviction. A multiply-xor spreads the bits that genuinely differ across the table.
    static_assert((kNoteSlots & (kNoteSlots - 1)) == 0, "slot count must be a power of two");
    int NoteSlot(const void* geom)
    {
        uintptr_t h = reinterpret_cast<uintptr_t>(geom);
        h ^= h >> 16;
        h *= 0x2545F491u;
        h ^= h >> 13;
        return static_cast<int>(h & static_cast<uintptr_t>(kNoteSlots - 1));
    }

    /// Rebuilds asked for in one frame.
    ///
    /// FIRST SIGHT ONLY. A group is asked once in its life, so the population this has to cover is a
    /// horizon filling with water nobody has looked at yet -- not one that keeps changing its mind.
    ///
    /// Budgeted because a camera swinging onto open water brings hundreds of those into view at
    /// once, and rebuilding all of them on the frame they appear is a visible stall. Spread over a
    /// few frames it is imperceptible: at this rate a fresh horizon fills in within a fifth of a
    /// second, and the group draws stock water in the meantime rather than nothing.
    ///
    /// Live, because how much water a map puts in view at once is a property of the map.
    int g_forcePerFrame = 8;
    int g_forceBudget = 8;

    /// Why batches are not being refined, counted rather than guessed at.
    ///
    /// The rebuild has five silent exits and the capture has four, and a batch that takes any of them
    /// keeps stock geometry -- correct water, but not ours, and indistinguishable at a glance from
    /// water we never saw. Every hypothesis about the sea's reach is a guess about WHICH of those nine
    /// is firing, so they are counted and reported instead.
    ///
    /// The discrimination that matters: if the declines are all zero and `refined` tracks `batches`,
    /// then everything the emitter hands us is already being rebuilt, and the missing water never
    /// reached this file at all.
    struct Diag
    {
        int batches, refined;
        /// Served from the provider's cache with nothing emitted. Not a fault -- it is the normal
        /// answer for a group already refined, and the signal to ask for a rebuild for one that is not.
        int declEmpty;
        /// DOES THE DIRTY FLAG ACTUALLY FORCE A RE-EMIT? Asked is how many times we set it; honoured
        /// is how many of those groups then came back through the emitter. The whole refinement
        /// design rests on that flag working, and nothing has ever measured it -- so a near-zero
        /// honoured against a large asked says the mechanism is inert, and every symptom blamed
        /// on culling or batching is this instead.
        int asked, honoured;
        /// Batches the overflow guard had to coarsen below the chosen factor. Expected to stay
        /// at zero: anything else is a batch far larger than any yet measured.
        int trimmed;
        int declOverflow, declMismatch, declSize, declPool, declLock;
        int ovGrids, ovPool, ovCells, ovShape;
        int peakGrids, peakSrc, peakOut;
        /// Farthest chunk we actually refined, and farthest we were offered at all. The gap between
        /// the two is the whole question: equal, and the client simply stops handing us water there;
        /// far apart, and we are being handed it and dropping it.
        float peakRefinedDist, peakSeenDist;
        /// Liquid instances the client asked bounds for, and the farthest of them. This is the only
        /// complete population: it is taken before any culling or caching decision, so it answers
        /// how far the client CONSIDERS water at all -- which is a different question from how far
        /// it hands us any, and the one that says whether the edge is ours or its.
        int   boxes;
        float peakBoxDistSq;   ///< squared; rooted where it is reported

        /// WHICH GATE IS THROWING THE NEAR WATER AWAY, counted instead of reasoned about.
        ///
        /// The liquid cull runs three tests in order: the frustum and the occlusion buffer, both on
        /// the box, then a convex-volume test on the sphere. Only the middle of that ladder is
        /// observable, and it is the one that matters: the sphere is fetched ONLY after the two box
        /// gates have passed, so `passedBox` against `boxes` is exactly how many instances they let
        /// through.
        ///
        /// The pair of nearest distances is what answers the question. If the nearest instance
        /// CONSIDERED is a few units away while the nearest one that PASSED is hundreds, the box
        /// gates are eating the near water and the fault is in the bounds. If the two are equal,
        /// culling is innocent and the near water is being dropped after this point -- and every
        /// hypothesis about bounds is looking in the wrong place.
        int   passedBox;
        float nearestBox = 1e9f;
        float nearestPassed = 1e9f;
    };
    Diag g_diag{};
    int  g_diagTick = 0;

    void DiagReport()
    {
        if (++g_diagTick < 600) return;
        g_diagTick = 0;
        WLOG_INFO("water: batches %d refined %d | declined overflow %d mismatch %d size %d pool %d "
                  "lock %d | cached %d | asked %d honoured %d trimmed %d | capture rejects grids %d pool %d cells %d "
                  "shape %d | peak grids %d src %d out %d | reach refined %.0f offered %.0f "
                  "| boxes %d out to %.0f | past box %d, nearest considered %.0f nearest past %.0f "
                  "| owned %d live of %d ever",
                  g_diag.batches, g_diag.refined,
                  g_diag.declOverflow, g_diag.declMismatch, g_diag.declSize,
                  g_diag.declPool, g_diag.declLock,
                  g_diag.declEmpty,
                  g_diag.asked, g_diag.honoured, g_diag.trimmed,
                  g_diag.ovGrids, g_diag.ovPool, g_diag.ovCells, g_diag.ovShape,
                  g_diag.peakGrids, g_diag.peakSrc, g_diag.peakOut,
                  g_diag.peakRefinedDist, g_diag.peakSeenDist,
                  g_diag.boxes, std::sqrt(g_diag.peakBoxDistSq),
                  g_diag.passedBox, g_diag.nearestBox, g_diag.nearestPassed,
                  wxl::water::OwnedCount(), wxl::water::OwnedAllocations());
        g_diag = Diag{};
    }

    /// Asks the provider to rebuild this group once, so its vertices come back through the emitter.
    ///
    /// ONCE, and never again for the same group whatever comes of it. If the rebuild produces a
    /// capture we refine it and it is done; if it produces nothing -- the group draws through a path
    /// we do not hook, or its shape is one the capture rejects -- then asking a second time gives the
    /// same answer at the same price, every draw, forever. A rebuild loop on water that cannot be
    /// refined would cost more than the refinement ever bought.
    void AskForRebuild(void* geom)
    {
        if (!geom || g_forceBudget <= 0) return;

        GeomNote& note = g_notes[NoteSlot(geom)];
        if (note.geom == geom) return;

        // Direct-mapped: a collision evicts a settled group, which will then be asked once more the
        // next time it is drawn. Wasteful, bounded by the budget, and self-correcting -- worth far
        // less complexity than probing, at four thousand slots.
        note.geom = geom;
        note.state = 1;

        *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(geom) + liq::kGeomDirty) = 1u;
        --g_forceBudget;
        ++g_diag.asked;
    }

    /// Records that this group has been through the emitter, so it is never asked again.
    void MarkSettled(void* geom)
    {
        if (!geom) return;
        GeomNote& note = g_notes[NoteSlot(geom)];
        note.geom = geom;
        note.state = 2;
    }

    /// A REBUILD CAME THROUGH AND WE COULD NOT REFINE IT. Puts the cached descriptor back.
    ///
    /// THIS IS THE SPRAY OF TRIANGLES. A successful refinement writes its descriptor into the
    /// group's CACHED copy, because most draws never run the emitter again and that copy is what
    /// gets restored each time. Correct, and it has a mirror image: if the group is re-emitted and
    /// we then DECLINE it, the buffers are freshly stock -- a few hundred vertices -- while the
    /// cached descriptor still claims the tens of thousands of the refinement before it. The draw
    /// then reads that descriptor over those buffers, and every index past the real end lands on
    /// whatever the pool happens to hold: a fan of triangles reaching to garbage positions.
    ///
    /// The descriptor just filled IS the correct stock one, so putting it in the cache is the whole
    /// repair. Every decline below therefore goes through here -- there is no path that leaves a
    /// refined descriptor standing over stock geometry.

    liq::MapChunkBufAllocFn      g_origAlloc  = nullptr;
    liq::ChunkVertexEmitFn       g_origEmit   = nullptr;
    liq::LiquidGetAaBoxFn        g_origAaBox  = nullptr;
    liq::LiquidGetSphereFn       g_origSphere = nullptr;
    liq::MapChunkGatherLiquidFn  g_origGather = nullptr;

    /// The excursion the sea is currently capable of, in world units. One statement of it, so every
    /// bound that has to cover the waves covers the same waves.
    float WaveMargin()
    {
        const float m = wxl::water::Frame().maxDisplacement
                      + wxl::water::OceanMaxRise()
                      + g_cullMargin;
        return (m > 0.0f) ? m : 0.0f;
    }

    /// Widens a map chunk's own bounds for the length of its liquid gather, and puts them back.
    ///
    /// THE OUTERMOST GATE, and the one that was actually taking the water. Widening the per-instance
    /// bounds could never reach it: fail here and the instances are never enumerated, so nothing
    /// that knows it is looking at water is ever asked. That is why the log showed every instance
    /// the cull considered surviving both of its bound tests -- the ones being lost had already been
    /// dropped a level up, and were never counted as considered at all.
    ///
    /// Borrowed and restored rather than widened permanently. These are the TERRAIN's bounds and the
    /// rest of the frame is entitled to them exactly as they are: they decide what the ground draws,
    /// what it collides with and how it sorts, and none of that wants a slab inflated by the height
    /// of a wave. The widening is true only inside the call that gathers liquid.
    void __fastcall HkGatherLiquid(void* chunk, void* edx)
    {
        const float m = WaveMargin();
        if (!chunk || m <= 0.0f) { g_origGather(chunk, edx); return; }

        auto* bounds = reinterpret_cast<float*>(static_cast<uint8_t*>(chunk) + liq::kMapChunkBounds);
        float saved[6];
        std::memcpy(saved, bounds, sizeof(saved));

        bounds[0] -= m; bounds[1] -= m; bounds[2] -= m;
        bounds[3] += m; bounds[4] += m; bounds[5] += m;

        g_origGather(chunk, edx);

        std::memcpy(bounds, saved, sizeof(saved));
    }

    /// Rides the sphere fetch purely to count. See Diag::passedBox for what the count decides.
    float* __fastcall HkGetSphere(void* instance, void* edx)
    {
        float* sphere = g_origSphere(instance, edx);
        if (sphere)
        {
            const float* eye = wxl::water::EyePosition();
            const float dx = sphere[0] - eye[0];
            const float dy = sphere[1] - eye[1];
            const float d = std::sqrt(dx * dx + dy * dy);
            ++g_diag.passedBox;
            if (d < g_diag.nearestPassed) g_diag.nearestPassed = d;
        }
        return sphere;
    }

    /// Widens the bounds the client culls liquid against, to cover where the waves actually go.
    ///
    /// THIS IS WHY THE SEA HAD AN EDGE. The box comes off the flat MH2O sheet, which is exactly where
    /// the water used to be and no longer is: the surface now rises and falls several units either
    /// side of that plane, and it is pushed sideways as well, because a Gerstner crest is pinched
    /// horizontally as much as it is lifted. Every one of those excursions is outside the box.
    ///
    /// So a chunk is culled on the strength of a slab that no longer describes it, and its waves --
    /// which would have been on screen -- go with it. It shows first where the box is closest to the
    /// frustum edge: the sides of the screen, and the long grazing view down the water where a slab
    /// of no thickness leaves the frustum a long way before the crests standing on it do.
    ///
    /// The margin is the frame's OWN maximum excursion rather than a constant, so it follows the
    /// weather -- a calm sea pays for a calm sea. Applied to all three axes: the vertical bound is
    /// the one that matters, and covering the horizontal pinch with the same number costs a slightly
    /// generous box, which errs towards drawing rather than towards this exact bug.
    void __fastcall HkGetAaBox(void* instance, void* edx, float* box)
    {
        g_origAaBox(instance, edx, box);
        if (!box) return;

        // Measured HERE because this is the only place that sees every liquid instance the client
        // considers, culled or not, cached or rebuilt. Everything downstream sees a survivor of a
        // decision already taken, which is why the reach numbers taken there answered a narrower
        // question than the one being asked: they only ever covered the groups re-emitted that
        // window, never the far larger number served from the provider's cache.
        // Taken once: the reach measurement and the near-water rule below both want it, and the
        // sheet's own centre does not move while this function runs.
        const float* eye = wxl::water::EyePosition();
        const float cx = (box[0] + box[3]) * 0.5f - eye[0];
        const float cy = (box[1] + box[4]) * 0.5f - eye[1];
        const float eyeDist2 = cx * cx + cy * cy;

        // Kept SQUARED, and rooted once when the line is written. This runs for every liquid
        // instance the frame considers -- thousands of times a second -- and it exists only to
        // print a number every six hundred batches. A comparison does not need the root.
        ++g_diag.boxes;
        if (eyeDist2 > g_diag.peakBoxDistSq) g_diag.peakBoxDistSq = eyeDist2;
        {
            const float d = std::sqrt(eyeDist2);
            if (d < g_diag.nearestBox) g_diag.nearestBox = d;
        }

        // BOTH LAYERS, and only one of them was counted. The trains' excursion was the whole margin
        // while the detail field was a ripple; it is now the layer carrying every wave anybody
        // looks at, and its rise was outside the box exactly like the trains' used to be. A chunk
        // whose trains happen to be calm was therefore culled with several units of detail standing
        // on it -- which is why the holes survived the first fix and appear worst in a calm.
        //
        // Plus a flat allowance the panel owns. Two things the measured pair cannot cover: the
        // field's SIDEWAYS gather, which is bounded per cascade rather than measured, and the fact
        // that both numbers describe the sea as it is THIS frame while the box is culled against a
        // frustum that will have moved by the time the chunk would be drawn. Turning fast is the
        // case that fails, and a constant is the honest way to buy against it.
        const float margin = WaveMargin();
        if (margin > 0.0f)
        {
            box[0] -= margin; box[1] -= margin; box[2] -= margin;
            box[3] += margin; box[4] += margin; box[5] += margin;
        }

        // NEAR WATER SKIPS THE TEST ENTIRELY, and the way it skips it is to swallow the camera.
        //
        // A box containing the eye cannot be outside the frustum, whatever the frustum is doing --
        // so stretching the near chunks' bounds far enough to reach the eye is an exact statement of
        // "do not cull this", with no invented number to tune against and no effect on how the box
        // reads for anything else. A chunk already containing the eye is left untouched.
        //
        // See g_nearAlways for why the near ones deserve it and the far ones do not. The distance is
        // measured on the SHEET, before the margin widened it, so the radius means the same thing in
        // a storm as in a calm.
        if (g_nearAlways > 0.0f && eyeDist2 < g_nearAlways * g_nearAlways)
        {
            for (int a = 0; a < 3; ++a)
            {
                if (eye[a] < box[a])     box[a]     = eye[a];
                if (eye[a] > box[a + 3]) box[a + 3] = eye[a];
            }
        }

        // AND THE SPHERE IS RESTATED TO MATCH. Widening the box alone was half a fix: a third gate
        // reads the instance's bounding sphere instead, and that sphere was still measured on the
        // flat sheet -- so a chunk could clear both box tests and be dropped anyway, which is why
        // nothing changed when the box grew.
        //
        // Written rather than scaled, so it stays exactly the sphere the box implies however many
        // times this runs. Idempotent by construction, which matters because the value is cached on
        // the instance and this hook sees the same instance every frame.
        if (instance)
        {
            auto* sphere = reinterpret_cast<float*>(static_cast<uint8_t*>(instance) +
                                                    liq::kInstanceSphere);
            const float sx = (box[0] + box[3]) * 0.5f;
            const float sy = (box[1] + box[4]) * 0.5f;
            const float sz = (box[2] + box[5]) * 0.5f;
            const float ex = box[3] - sx;
            const float ey = box[4] - sy;
            const float ez = box[5] - sz;
            sphere[0] = sx;
            sphere[1] = sy;
            sphere[2] = sz;
            sphere[3] = std::sqrt(ex * ex + ey * ey + ez * ez);
        }
    }

    /// The refined lattice's vertex spacing, from the spacing the source sheet arrived with.
    ///
    /// The vertex stage fades each detail cascade against this number, so it has to be a quantity
    /// both sides of a shared chunk edge compute identically -- otherwise the same world point gets
    /// two different amplitudes depending on which chunk drew it, which is a crease in the surface
    /// running exactly along the seam. One factor everywhere makes that automatic.
    float LatticeSpacing(float sourceCell)
    {
        return sourceCell / ((g_subdiv < 1) ? 1.0f : static_cast<float>(g_subdiv));
    }

    template <class Fn>
    Fn Vcall(void* obj, unsigned slot)
    {
        return reinterpret_cast<Fn>((*reinterpret_cast<void***>(obj))[slot]);
    }

    // ---- the plane registry -------------------------------------------------------------------
    // One record per map chunk, written as the geometry goes past. A still-water level is a fact
    // about the map, so nothing here is ever stale and nothing is evicted by age.
    //
    // It MUST probe on collision rather than overwrite. Two chunks that hash together are not
    // alternatives -- both are real water, and a lookup that lands on the wrong one answers "no
    // water here" for a place that has some. Turning the camera brings a fresh batch of chunks into
    // view all at once, so a table that overwrites drops whichever record they collide with, and
    // anything floating on that chunk falls to the flat plane until the chunk is recorded again.
    constexpr float kChunkSize = 33.33333f;
    constexpr int   kPlaneSlots = 8192;
    constexpr int   kPlaneProbe = 16;

    struct PlaneRecord
    {
        int   cx = 0, cy = 0;
        float level = 0.0f;
        float floorZ = 0.0f;
        bool  used = false;
    };
    PlaneRecord g_planes[kPlaneSlots];

    uint32_t PlaneHash(int cx, int cy)
    {
        uint32_t h = static_cast<uint32_t>(cx) * 0x9E3779B9u ^ static_cast<uint32_t>(cy) * 0x85EBCA6Bu;
        h ^= h >> 15;
        h *= 0x2545F491u;
        h ^= h >> 13;
        return h;
    }

    /// Finds the slot holding (cx, cy), or -1. With `claim`, returns a free slot to write into
    /// instead, falling back to the first probed slot only once a whole run is full -- at which
    /// point one record is genuinely being traded for another and the map is bigger than the table.
    int PlaneSlot(int cx, int cy, bool claim)
    {
        const uint32_t h = PlaneHash(cx, cy);
        for (int i = 0; i < kPlaneProbe; ++i)
        {
            const int idx = static_cast<int>((h + static_cast<uint32_t>(i)) & (kPlaneSlots - 1));
            const PlaneRecord& r = g_planes[idx];
            if (r.used && r.cx == cx && r.cy == cy) return idx;
            if (!r.used) return claim ? idx : -1;
        }
        return claim ? static_cast<int>(h & (kPlaneSlots - 1)) : -1;
    }

    void NotePlane(float x, float y, float level, float column)
    {
        const int cx = static_cast<int>(std::floor(x / kChunkSize));
        const int cy = static_cast<int>(std::floor(y / kChunkSize));
        PlaneRecord& r = g_planes[PlaneSlot(cx, cy, true)];
        r.cx = cx;
        r.cy = cy;
        r.level = level;
        r.floorZ = level - column;
        r.used = true;
    }

    /// Terrain height under a world point, through the client's own per-cell query.
    ///
    /// The resolution mirrors the client's walkable-height wrapper exactly: world to grid, grid to
    /// area tile, tile to chunk, chunk to cell. Every step is guarded, because the grid is only
    /// partly resident -- a tile still streaming in is the normal case at the edge of view, not an
    /// error, and it must cost a default rather than a fault.
    bool TerrainHeight(float wx, float wy, float& out)
    {
        if (*reinterpret_cast<const uint32_t*>(liq::kMapBDungeon)) return false;

        const float gx = -(wx - 17066.666f) * 0.24f;
        const float gy = -(wy - 17066.666f) * 0.24f;
        const int cellX = static_cast<int>(std::floor(gx));
        const int cellY = static_cast<int>(std::floor(gy));
        if (cellX < 0 || cellY < 0) return false;

        auto* areas = reinterpret_cast<void**>(liq::kMapAreaTable);
        void* area = areas[((cellX >> 7) & 0x3F) * 0x40 + ((cellY >> 7) & 0x3F)];
        if (!area) return false;
        if (*reinterpret_cast<const uint32_t*>(static_cast<uint8_t*>(area) + 0x70)) return false;

        auto* chunks = reinterpret_cast<void**>(static_cast<uint8_t*>(area) + 0xBC);
        void* chunk = chunks[((cellX >> 3) & 0xF) * 0x10 + ((cellY >> 3) & 0xF)];
        if (!chunk) return false;

        const float pos[3] = { wx, wy, 0.0f };
        float h = 0.0f;
        auto query = reinterpret_cast<liq::MapGetHeightTerrainFn>(liq::kMapGetHeightTerrain);
        if (!query(chunk, pos, cellY & 7, cellX & 7, &h)) return false;

        out = h;
        return true;
    }

    /// Beyond this the distinction stops mattering: everything steeper is a wall.
    constexpr float kMaxSlope = 4.0f;
    /// How far either side the bottom is sampled, in world units.
    constexpr float kSlopeProbe = 3.0f;

    /// How fast the bottom is climbing under a world point.
    ///
    /// Sampled at a FIXED world offset, not from the neighbouring lattice vertices. Differencing the
    /// lattice is free, but it has no neighbour to reach for at the edge of a chunk: the difference
    /// there becomes one-sided over half the span, so the value steps as the chunk boundary is
    /// crossed and the foam draws the boundary. A field the chunk grid cannot see is the only kind
    /// that stays continuous across it.
    ///
    /// Four extra queries per source vertex -- the coarse grid, once, when the chunk's buffers are
    /// built, not per frame.
    float BottomSlopeAt(float wx, float wy)
    {
        float xm = 0.0f, xp = 0.0f, ym = 0.0f, yp = 0.0f;
        if (!TerrainHeight(wx - kSlopeProbe, wy, xm)) return 0.0f;
        if (!TerrainHeight(wx + kSlopeProbe, wy, xp)) return 0.0f;
        if (!TerrainHeight(wx, wy - kSlopeProbe, ym)) return 0.0f;
        if (!TerrainHeight(wx, wy + kSlopeProbe, yp)) return 0.0f;

        const float gx = (xp - xm) * (0.5f / kSlopeProbe);
        const float gy = (yp - ym) * (0.5f / kSlopeProbe);
        const float mag = std::sqrt(gx * gx + gy * gy);
        return (mag > kMaxSlope) ? kMaxSlope : mag;
    }

    /// Reserves a cache entry large enough for the subdivided lattice.
    ///
    /// The multipliers are upper bounds over every grid shape rather than exact figures, which are
    /// not knowable here: this call is handed the batch's totals, while the subdivided counts depend
    /// on each chunk's own rows and columns. Reserving short is not a hazard -- the rebuild measures
    /// the pool it is about to write and declines, costing detail rather than memory.
    /// Rides the client's own liquid allocation for one thing: the memory tag it stamps its buffers
    /// with, so the pair this module allocates for itself is accounted for the same way. The counts
    /// pass through untouched -- the refinement no longer lives in these buffers, so inflating what
    /// the client reserves for its own would only waste the pool it is about to fill with stock
    /// vertices.
    void* __cdecl HkAlloc(int vertexCount, int indexCount, uint32_t tag, uint32_t fmt)
    {
        if (fmt == liq::kVertexFormatLiquid) wxl::water::NoteAllocTag(tag);
        return g_origAlloc(vertexCount, indexCount, tag, fmt);
    }

    /// Captures one chunk's emitted vertices as a grid.
    ///
    /// The write cursors are read before and after the stock call: their advance states both the
    /// vertex count and, through their distance from one another, the layout of the vertex the
    /// client chose for this batch. Nothing is assumed about the format beyond its stride.
    void __fastcall HkEmit(void* chunk, void* edx, const void* mtx, int stride,
                           uint8_t** pos, uint8_t** nrm, uint8_t** col,
                           uint8_t** uv0, uint8_t** uv1)
    {
        uint8_t* basePos = pos ? *pos : nullptr;
        uint8_t* baseNrm = nrm ? *nrm : nullptr;
        uint8_t* baseCol = col ? *col : nullptr;
        uint8_t* baseUv0 = uv0 ? *uv0 : nullptr;
        uint8_t* baseUv1 = uv1 ? *uv1 : nullptr;

        g_origEmit(chunk, edx, mtx, stride, pos, nrm, col, uv0, uv1);

        Batch& bt = g_batch;
        // BEFORE any rejection, including the one below. Reaching this line at all is the fact the
        // rebuild needs: the client has just refilled the buffers from scratch, and that is true
        // whether or not we manage to record what it wrote.
        bt.emitted = true;
        if (bt.overflow) return;

        if (!basePos || stride <= 0)
        {
            bt.overflow = true;
            return;
        }

        const int written = static_cast<int>((*pos - basePos) / stride);
        const auto* c = static_cast<const uint8_t*>(chunk);
        const int rows = *reinterpret_cast<const int*>(c + liq::kChunkRow1)
                       - *reinterpret_cast<const int*>(c + liq::kChunkRow0) + 1;
        const int cols = *reinterpret_cast<const int*>(c + liq::kChunkCol1)
                       - *reinterpret_cast<const int*>(c + liq::kChunkCol0) + 1;

        if (written <= 0 || rows <= 1 || cols <= 1 || rows * cols != written ||
            (rows - 1) * (cols - 1) > kMaxGridCells ||
            bt.gridCount >= kMaxGrids || bt.vertCount + written > kMaxPoolVerts)
        {
            if (bt.gridCount >= kMaxGrids)                   ++g_diag.ovGrids;
            else if (bt.vertCount + written > kMaxPoolVerts) ++g_diag.ovPool;
            else if ((rows - 1) * (cols - 1) > kMaxGridCells) ++g_diag.ovCells;
            else                                             ++g_diag.ovShape;
            bt.overflow = true;
            return;
        }

        bt.stride = stride;
        bt.offPos = 0;
        bt.offNrm = baseNrm ? static_cast<int>(baseNrm - basePos) : -1;
        bt.offCol = baseCol ? static_cast<int>(baseCol - basePos) : -1;
        bt.offUv0 = baseUv0 ? static_cast<int>(baseUv0 - basePos) : -1;
        bt.offUv1 = baseUv1 ? static_cast<int>(baseUv1 - basePos) : -1;

        SourceVertex* dst = bt.v + bt.vertCount;
        for (int k = 0; k < written; ++k)
        {
            const uint8_t* src = basePos + static_cast<size_t>(k) * stride;
            SourceVertex& d = dst[k];
            std::memcpy(d.pos, src, sizeof(d.pos));
            if (bt.offNrm >= 0) std::memcpy(d.nrm, src + bt.offNrm, sizeof(d.nrm));
            if (bt.offCol >= 0) std::memcpy(&d.col, src + bt.offCol, sizeof(d.col));
            if (bt.offUv0 >= 0) std::memcpy(d.uv0, src + bt.offUv0, sizeof(d.uv0));
            if (bt.offUv1 >= 0) std::memcpy(d.uv1, src + bt.offUv1, sizeof(d.uv1));

            // The water column, measured once per SOURCE vertex and interpolated into the refined
            // ones. Doing it per output vertex would multiply the terrain queries by s*s for a field
            // that varies far more slowly than the lattice it is carried on.
            float floorZ = 0.0f;
            d.nrm[0] = TerrainHeight(d.pos[0], d.pos[1], floorZ)
                     ? (d.pos[2] - floorZ) : kUnknownDepth;
            if (d.nrm[0] < 0.0f) d.nrm[0] = 0.0f;
            NoteDepth(d.nrm[0]);

            // What the column cannot say. The column knows how deep it is HERE; the slope knows
            // whether something is standing up out of the water nearby -- a reef, a cliff face, the
            // shelf of a beach. It is the whole shore-foam signal.
            d.nrm[1] = BottomSlopeAt(d.pos[0], d.pos[1]);
        }

        // The plane, recorded from the sheet the client just emitted. Taken at the middle vertex
        // rather than a corner: a corner sits on the chunk boundary and hashes to whichever
        // neighbour rounding picks.
        {
            const SourceVertex& mid = dst[written / 2];
            NotePlane(mid.pos[0], mid.pos[1], mid.pos[2], mid.nrm[0]);
        }

        GridSpan& sp = bt.grids[bt.gridCount++];
        sp.rows  = rows;
        sp.cols  = cols;
        sp.first = bt.vertCount;
        bt.vertCount += written;
    }
}

namespace wxl::water
{
    int   Subdivision() { return g_subdiv; }
    void  SetSubdivision(int factor) { g_subdiv = (factor < 1) ? 1 : (factor > 8 ? 8 : factor); }
    float FinestVertexSpacing()
    {
        // The map sheet's cell: eight cells to a chunk edge. The per-batch value the vertex stage
        // grades against is measured off the real geometry; this one is for the panel, where the
        // question is the map's, not one batch's.
        return (kChunkSize / 8.0f) / static_cast<float>(g_subdiv);
    }

    float CullMargin() { return g_cullMargin; }
    void  SetCullMargin(float margin) { g_cullMargin = (margin < 0.0f) ? 0.0f : margin; }

    float NearAlwaysRadius() { return g_nearAlways; }
    void  SetNearAlwaysRadius(float radius)
    {
        g_nearAlways = (radius < 0.0f) ? 0.0f : ((radius > 400.0f) ? 400.0f : radius);
    }
    Batch& Current() { return g_batch; }

    bool PlaneAt(float x, float y, WaterPlane& out)
    {
        const int cx = static_cast<int>(std::floor(x / kChunkSize));
        const int cy = static_cast<int>(std::floor(y / kChunkSize));
        const int idx = PlaneSlot(cx, cy, false);
        if (idx < 0)
        {
            out = WaterPlane{};
            return false;
        }
        const PlaneRecord& r = g_planes[idx];
        out.present = true;
        out.level = r.level;
        out.floor = r.floorZ;
        return true;
    }

    void BeginBatch()
    {
        g_batch.gridCount = 0;
        g_batch.vertCount = 0;
        g_batch.overflow  = false;
        g_batch.emitted   = false;
        g_batch.stride    = 0;
    }

    void BeginFrame()
    {
        g_forceBudget = g_forcePerFrame;

        // The frame that just ended becomes what the panel reads, and the live counters start over.
        // Published only when something was actually measured: a frame with no water on screen --
        // indoors, or facing away from the sea -- would otherwise blank the histogram somebody is in
        // the middle of reading.
        if (g_depthCount > 0)
        {
            std::memcpy(g_depthHist, g_depthLive, sizeof(g_depthHist));
        }
        std::memset(g_depthLive, 0, sizeof(g_depthLive));
        g_depthCount = 0;
    }

    int  RebuildBudget() { return g_forcePerFrame; }
    void SetRebuildBudget(int perFrame)
    {
        g_forcePerFrame = (perFrame < 1) ? 1 : ((perFrame > 128) ? 128 : perFrame);
    }

    int DepthBuckets() { return kDepthBuckets; }
    const int* DepthHistogram() { return g_depthHist; }

    float DepthForBucket(float bucket)
    {
        // Inverse of NoteDepth's mapping, at the bucket's LOW edge, so a curve drawn against these
        // lines up with the bars rather than sitting half a bucket to the right of them.
        const float t = bucket / static_cast<float>(kDepthBuckets);
        return std::exp(t * std::log(1.0f + kDepthBucketMax)) - 1.0f;
    }


    bool InstallHooks()
    {
        // The geometry provider itself is hooked by the renderer, not here: that call is also the
        // last seam where a shader substitution still wins the deferred flush, and one detour on it
        // is the only way both can happen in a defined order.
        //
        // The cull bounds belong here though, and they come first: they decide what reaches the
        // lattice at all, where everything else decides what is done with what reached it.
        // The map chunk's liquid gather comes FIRST, because it is the gate above all the others:
        // a chunk that fails it never enumerates its water, so nothing below ever sees it.
        bool ok = wxl::hook::Install("Water_LatticeChunkGather", liq::kMapChunkGatherLiquid,
                                     &HkGatherLiquid, &g_origGather);
        ok &= wxl::hook::Install("Water_LatticeCullBounds", liq::kLiquidGetAaBox,
                                     &HkGetAaBox, &g_origAaBox);
        ok &= wxl::hook::Install("Water_LatticeAllocTag", liq::kMapChunkBufAlloc,
                                     &HkAlloc, &g_origAlloc);
        // The two seams where a chunk group lets go of geometry. Our own buffers are released
        // there, which is the whole of their lifetime: a record that outlived its group would hold
        // a pair the engine's free list is entitled to hand to somebody else.
        ok &= InstallGeometryOwnership();
        ok &= wxl::hook::Install("Water_LatticeEmit", liq::kChunkVertexEmit, &HkEmit, &g_origEmit);
        // Diagnostic only: it counts what survived the two bound tests, and changes nothing.
        ok &= wxl::hook::Install("Water_LatticeCullSurvivors", liq::kLiquidGetSphere,
                                 &HkGetSphere, &g_origSphere);
        return ok;
    }

    /// Hands the draw the refinement this group already has, if it has one.
    ///
    /// THE DIVIDEND OF OWNING THE BUFFERS, and it took a crash to notice it was there. When the
    /// refinement lived in the provider's pair, a decline meant the client had just written stock
    /// vertices over it and the only honest answer was to stand aside. The pair is ours now: the
    /// client's emitter cannot reach it, so a batch this module cannot refine THIS time leaves the
    /// previous refinement intact and still describing the same stretch of water. Re-presenting it
    /// is both better looking and safer than standing aside -- it is what makes every decline path
    /// below end in a complete, self-consistent answer rather than in whatever the provider left.
    bool PresentOwned(void* geom, void** vbOut, void** ibOut, void* batchDesc)
    {
        OwnedGeometry* o = FindGeometry(geom);
        if (!o || !o->described) return false;
        *vbOut = o->vb;
        *ibOut = o->ib;
        *static_cast<liq::BatchDesc*>(batchDesc) = o->desc;
        return true;
    }

    void EnsureDrawable(void* batchDesc)
    {
        if (!batchDesc) return;
        auto* bd = static_cast<liq::BatchDesc*>(batchDesc);
        if (bd->primType <= liq::kPrimTypeMax && bd->indexCount >= 3) return;

        // Two indices is the one shape that draws nothing and is still legal arithmetic: a strip
        // yields (indices - 2) primitives, so the device is asked for none rather than for a count
        // it would read as enormous.
        bd->primType    = liq::kPrimTriangleStrip;
        bd->startIndex  = 0;
        bd->indexCount  = 2;
        bd->vertexRange = 0;

        static int said = 0;
        if (said < 3)
        {
            ++said;
            WLOG_WARN("water: the liquid provider returned a batch it never wrote -- that draw is "
                      "skipped (this is the client's own rebuild path, not a fault of the water)");
        }
    }

    void Densify(void* geom, void** vbOut, void** ibOut, void* batchDesc)
    {
        Batch& bt = g_batch;
        const int grids = bt.gridCount;
        // Consumed exactly once, whatever happens: a stale batch reused by the next draw would paint
        // one stretch of water with another's geometry.
        bt.gridCount = 0;

        ++g_diag.batches;
        if (grids > g_diag.peakGrids)       g_diag.peakGrids = grids;
        if (bt.vertCount > g_diag.peakSrc)  g_diag.peakSrc = bt.vertCount;
        DiagReport();

        if (!vbOut || !ibOut || !batchDesc || !*vbOut || !*ibOut) return;

        // NOTHING CAPTURED -- TWO OPPOSITE SITUATIONS WEARING THE SAME FACE, and taking them for one
        // was the torn sheet of water hanging over the shoreline.
        //
        // If the emitter never ran, the provider answered from its cache: the buffers still hold the
        // refinement we put there and the descriptor still describes it, so the correct action is to
        // touch nothing. For a group already refined that is the normal answer; for one that never
        // was, it is the only moment we get to notice, and the dirty flag is the only way back in.
        //
        // If the emitter DID run and we recorded no grid, the client has just overwritten those
        // buffers with a few hundred stock vertices while the cached descriptor still claims the
        // tens of thousands of the last refinement -- and the next draw fans triangles from that
        // descriptor into whatever the pool happens to hold. It is reached whenever the FIRST chunk
        // of a batch is rejected, which is a shoreline chunk almost by definition: those are the
        // ones emitted with a partial footprint, and a partial footprint is exactly what the capture
        // refuses. Hence a fault that only ever appeared near a coast, and stayed once it appeared:
        // the group is asked for its one rebuild, rejected, and never revisited.
        if (grids <= 0)
        {
            // The emitter ran and nothing survived the capture, or it never ran at all because the
            // provider answered from its cache. Either way this batch brought no geometry, and
            // either way the refinement this group already had is still sitting in our buffers.
            if (bt.emitted) ++g_diag.declOverflow; else ++g_diag.declEmpty;
            if (PresentOwned(geom, vbOut, ibOut, batchDesc)) return;

            // Nothing of ours to show: a group that has never been refined asks for the rebuild
            // that is its only way in.
            if (!bt.emitted) AskForRebuild(geom);
            return;
        }

        // It came through the emitter, which is all that was ever wanted from it. Whether the
        // refinement below succeeds is a separate question, and not one another rebuild would answer.
        //
        // Counted BEFORE MarkSettled overwrites the state: a group arriving here while still marked
        // as asked is a dirty-flag request that the client honoured, and that count against
        // g_diag.asked is the only evidence the mechanism works at all.
        {
            const GeomNote& note = g_notes[NoteSlot(geom)];
            if (note.geom == geom && note.state == 1) ++g_diag.honoured;
        }
        MarkSettled(geom);

        // EVERY DECLINE FROM HERE DOWN RE-PRESENTS WHAT THIS GROUP ALREADY HAD. Owning the buffers
        // is what makes that possible: the previous refinement is still in them, whatever this
        // batch turned out to be. Falling back to the provider's own answer is the one thing not
        // worth doing -- it has just been observed handing back a descriptor it never filled.
        if (bt.overflow || bt.stride <= 0)
        { ++g_diag.declOverflow; PresentOwned(geom, vbOut, ibOut, batchDesc); return; }

        auto* bd = static_cast<liq::BatchDesc*>(batchDesc);
        const int stockVerts = static_cast<int>((bd->vertexRange >> 16) & 0xFFFF) + 1;
        const int stockIdx   = static_cast<int>(bd->indexCount);

        // THE INVARIANT. The descriptor states how many vertices this batch uses and is refilled
        // from scratch by every provider call -- unlike the stream's own count field, which is never
        // reset. Rebuilding is only meaningful if the capture accounts for exactly that many: miss a
        // chunk and every chunk after it is rebuilt from the wrong source rows.
        if (stockVerts != bt.vertCount)
        {
            static int mism = 0;
            if (mism < 6)
            {
                ++mism;
                WLOG_WARN("water: batch states %d verts but %d were captured over %d chunk(s) "
                          "-- keeping stock geometry", stockVerts, bt.vertCount, grids);
            }
            ++g_diag.declMismatch;
            PresentOwned(geom, vbOut, ibOut, batchDesc);
            return;
        }

        // ONE FACTOR FOR THE WHOLE BATCH, halved until the result fits.
        //
        // The halving is an OVERFLOW GUARD, not a level of detail. kMaxGrids admits a batch far
        // larger than any yet measured, and such a batch at the full factor would not fit under the
        // sixteen-bit ceiling; coarsening it uniformly keeps that case as coarser water rather than
        // as the whole batch falling back to stock -- which is the failure that ends a sea at a line
        // rather than fading it. In everything measured so far the loop exits on its first pass.
        int factor = (g_subdiv < 1) ? 1 : g_subdiv;
        int verts = 0, idxBound = 0;
        for (;;)
        {
            verts = 0;
            idxBound = 0;
            for (int gi = 0; gi < grids; ++gi)
            {
                const GridSpan& sp = bt.grids[gi];
                const int rows = (sp.rows - 1) * factor + 1;
                const int cols = (sp.cols - 1) * factor + 1;
                verts += rows * cols;
                // An UPPER BOUND: a masked row is emitted as several runs and each carries its own
                // pair of duplicates.
                idxBound += (rows - 1) * (4 * cols + 8);
            }
            if (verts <= kMaxOutVerts || factor <= 1) break;
            factor >>= 1;
        }

        // Even unrefined it does not fit: the batch presents more source water than the index space
        // holds at all, and only stock geometry can draw it.
        if (verts > kMaxOutVerts || idxBound <= 0)
        { ++g_diag.declSize; PresentOwned(geom, vbOut, ibOut, batchDesc); return; }
        if (verts > g_diag.peakOut) g_diag.peakOut = verts;
        if (factor < g_subdiv) ++g_diag.trimmed;

        // How far this batch reached, for the panel. Measured off the eye, because the question
        // it answers is how far out water is being handed to us at all.
        {
            const float* eye = wxl::water::EyePosition();
            for (int gi = 0; gi < grids; ++gi)
            {
                const GridSpan& sp = bt.grids[gi];
                const SourceVertex& mid = bt.v[sp.first + (sp.rows * sp.cols) / 2];
                const float dx = mid.pos[0] - eye[0];
                const float dy = mid.pos[1] - eye[1];
                const float d = std::sqrt(dx * dx + dy * dy);
                if (d > g_diag.peakSeenDist) g_diag.peakSeenDist = d;
            }
        }


        // No capacity check stands here any more. It used to guard a reservation made at the
        // client's allocation, before the grid shapes were known, which could fall short of what
        // the refinement then wanted to write. The pair used below is allocated further down for
        // exactly the two figures just computed, and the allocator matches on exact byte sizes, so
        // "does it fit" is answered by construction rather than tested afterwards.
        void* dev = *reinterpret_cast<void**>(gxoff::kGxDevicePtr);
        if (!dev) { PresentOwned(geom, vbOut, ibOut, batchDesc); return; }

        auto lock    = Vcall<liq::GxBufLockFn>(dev, liq::kGxVtLock);
        auto unlock  = Vcall<liq::GxBufUnlockFn>(dev, liq::kGxVtUnlock);
        auto sizeSet = reinterpret_cast<liq::GxBufSizeSetFn>(liq::kGxBufSizeSet);

        // THE COVERAGE MASK, read out of the client's own index run. Liquid near a shoreline is
        // placed by cell, so a chunk's lattice is full of holes by design. The emitter still writes a
        // vertex for every corner of the whole rectangle -- but the corners of a cell the client
        // never draws are never referenced by it either, and their contents mean nothing. Joining
        // them, as a full lattice does, stitches the surface to values never meant to be read. That
        // is why only shorelines tear: open water carries no mask, so a full lattice is right there
        // by accident.
        //
        // The mask is not looked up in the chunk. The index run IS the mask, exactly, per batch.
        static bool covered[kMaxGrids][kMaxGridCells];
        // Only the rows about to be used. The array is a hundred and thirty kilobytes at this grid
        // count, and clearing all of it for a batch of six chunks would cost more than the rebuild.
        std::memset(covered, 0, sizeof(covered[0]) * static_cast<size_t>(grids));
        {
            const auto* src = static_cast<const uint16_t*>(lock(dev, nullptr, *ibOut));
            if (!src) { ++g_diag.declLock; PresentOwned(geom, vbOut, ibOut, batchDesc); return; }
            for (int t = 0; t + 2 < stockIdx; ++t)
            {
                const uint16_t a = src[t], b = src[t + 1], c = src[t + 2];
                if (a == b || b == c || a == c) continue;   // a degenerate joins, it does not cover
                // Found by bisection, not by scanning. This runs once per index in the batch, so a
                // linear probe makes the mask cost grow as the SQUARE of the water in view -- which
                // did not matter at thirty-two chunks and would be the whole cost of the rebuild at
                // five hundred. The spans are appended in order and never overlap, so the search is
                // exact.
                int lo = 0, hi = grids - 1, gi = -1;
                while (lo <= hi)
                {
                    const int probe = (lo + hi) >> 1;
                    const GridSpan& sp = bt.grids[probe];
                    if (a < sp.first)                              hi = probe - 1;
                    else if (a >= sp.first + sp.rows * sp.cols)    lo = probe + 1;
                    else { gi = probe; break; }
                }
                if (gi >= 0)
                {
                    const GridSpan& sp = bt.grids[gi];
                    const int span = sp.rows * sp.cols;
                    const int la = a - sp.first, lb = b - sp.first, lc = c - sp.first;
                    if (lb < 0 || lb >= span || lc < 0 || lc >= span) continue;
                    // A lattice triangle spans exactly one cell, and that cell is the corner with the
                    // lowest row and the lowest column -- true for both triangles of a quad.
                    int r = la / sp.cols, cc = la % sp.cols;
                    const int rb = lb / sp.cols, cb = lb % sp.cols;
                    const int rc = lc / sp.cols, ccc = lc % sp.cols;
                    if (rb < r) r = rb;
                    if (rc < r) r = rc;
                    if (cb < cc) cc = cb;
                    if (ccc < cc) cc = ccc;
                    if (r < sp.rows - 1 && cc < sp.cols - 1 && (r * (sp.cols - 1) + cc) < kMaxGridCells)
                        covered[gi][r * (sp.cols - 1) + cc] = true;
                }
            }
            unlock(dev, nullptr, *ibOut, 0);
        }

        // FROM HERE THE GEOMETRY IS OURS. Everything above only READ what the client emitted: its
        // vertices, its index run, the coverage they imply. What is written below is a different
        // mesh, and it goes into a buffer pair this module allocated for this group and sized for
        // exactly what is about to be put in it. The client's own pair is left holding the stock
        // geometry it just built, described by its own descriptor -- so no decision it makes later,
        // about caching or skipping or recycling that pair, can be wrong about data of ours.
        OwnedGeometry* owned = AcquireGeometry(geom, verts, idxBound);
        if (!owned) { ++g_diag.declPool; PresentOwned(geom, vbOut, ibOut, batchDesc); return; }
        void* const ourVb = owned->vb;
        void* const ourIb = owned->ib;

        // Restate both views at the subdivided size BEFORE locking: a lock maps exactly the byte
        // count the view carries, so a view still describing the stock lattice hands back a mapping
        // that stops partway through the one being written.
        sizeSet(ourVb, static_cast<uint32_t>(bt.stride), static_cast<uint32_t>(verts));
        sizeSet(ourIb, 2, static_cast<uint32_t>(idxBound));
        auto* vdst = static_cast<uint8_t*>(lock(dev, nullptr, ourVb));
        auto* idst = static_cast<uint16_t*>(lock(dev, nullptr, ourIb));
        if (!vdst || !idst)
        {
            if (vdst) unlock(dev, nullptr, ourVb, 0);
            if (idst) unlock(dev, nullptr, ourIb, 0);
            ++g_diag.declLock;
            PresentOwned(geom, vbOut, ibOut, batchDesc);
            return;
        }

        uint16_t* w = idst;
        int base = 0;

        for (int gi = 0; gi < grids; ++gi)
        {
            const GridSpan& sp = bt.grids[gi];
            const SourceVertex* src = bt.v + sp.first;
            const int   s   = factor;
            const float inv = 1.0f / static_cast<float>(s);
            const int rows = (sp.rows - 1) * s + 1;
            const int cols = (sp.cols - 1) * s + 1;

            // The spacing the refined lattice actually has HERE, measured off the source geometry
            // rather than assumed from the chunk constant, and carried per vertex in the normal's
            // spare component. The vertex stage grades each detail cascade against it: a wave
            // shorter than a few vertex spacings cannot be DRAWN -- each vertex receives a value
            // nearly decorrelated from its neighbour's, which animates as per-vertex noise, not as
            // a travelling wave -- so those cascades leave the geometry locally and live on in the
            // shading, where resolution is the pixel and no lattice exists to alias against.
            const float e0 = src[1].pos[0] - src[0].pos[0];
            const float e1 = src[1].pos[1] - src[0].pos[1];
            const float sourceCell = std::sqrt(e0 * e0 + e1 * e1);

            for (int r = 0; r < rows; ++r)
            {
                const int   r0 = r / s;
                const float fr = static_cast<float>(r - r0 * s) * inv;
                const int   r1 = (r0 + 1 < sp.rows) ? r0 + 1 : r0;
                for (int c = 0; c < cols; ++c)
                {
                    const int   c0 = c / s;
                    const float fc = static_cast<float>(c - c0 * s) * inv;
                    const int   c1 = (c0 + 1 < sp.cols) ? c0 + 1 : c0;

                    const SourceVertex& a = src[r0 * sp.cols + c0];
                    const SourceVertex& b = src[r0 * sp.cols + c1];
                    const SourceVertex& d = src[r1 * sp.cols + c0];
                    const SourceVertex& e = src[r1 * sp.cols + c1];

                    auto bil = [fc, fr](float av, float bv, float dv, float ev) {
                        const float top = av + (bv - av) * fc;
                        const float bot = dv + (ev - dv) * fc;
                        return top + (bot - top) * fr;
                    };
                    // Eased weights for the FIELDS, linear for the POSITION. Straight bilinear is
                    // flat inside a cell and creased at its edges, which is invisible on something
                    // that varies gently and very visible on the water column: over a reef the bottom
                    // can jump metres between two source vertices, and the crease then runs along the
                    // cell boundary as a straight line the sea has no reason to contain. The position
                    // stays linear because a curved patch would part company with the flat sheet next
                    // to it.
                    const float ec = fc * fc * (3.0f - 2.0f * fc);
                    const float er = fr * fr * (3.0f - 2.0f * fr);
                    auto eased = [ec, er](float av, float bv, float dv, float ev) {
                        const float top = av + (bv - av) * ec;
                        const float bot = dv + (ev - dv) * ec;
                        return top + (bot - top) * er;
                    };

                    uint8_t* out = vdst + static_cast<size_t>(base + r * cols + c) * bt.stride;

                    float* p = reinterpret_cast<float*>(out + bt.offPos);
                    for (int k = 0; k < 3; ++k) p[k] = bil(a.pos[k], b.pos[k], d.pos[k], e.pos[k]);

                    if (bt.offNrm >= 0)
                    {
                        // X IS NOT A NORMAL COMPONENT HERE. The liquid sheet's normal is a constant
                        // up-vector that no stage of ours ever reads -- three floats per vertex
                        // sitting unused in a format we cannot widen. The water column has nowhere
                        // else to go and is worth far more than a component of a normal nobody
                        // consults; the vertex stage builds its own normal from the wave table.
                        float* n = reinterpret_cast<float*>(out + bt.offNrm);
                        n[0] = eased(a.nrm[0], b.nrm[0], d.nrm[0], e.nrm[0]);
                        // ... and Y is not one either: it carries the bottom's slope, eased for the
                        // same reason as the column. A creased slope field would draw the foam's edge
                        // along the source cell boundaries instead of along the rock.
                        n[1] = eased(a.nrm[1], b.nrm[1], d.nrm[1], e.nrm[1]);
                        // ... and Z carries the vertex spacing the detail's portability fade grades
                        // against, NEGATED, because the sign is also the only mark saying these
                        // three components mean anything.
                        //
                        // The reasoning that used to sit here -- that a sheet we never wrote is safe
                        // because its X reads as zero column -- had it exactly backwards. A zero
                        // column is not "no wave"; it is the WATERLINE, and the shore ladder answers
                        // the waterline with a fully broken wave. So every sheet the refinement
                        // never reached, and every sheet from the surrounding geometry, was drawn as
                        // solid surf: a white plateau with hard straight edges standing in open
                        // water. A negative Z here and a sign test there is the whole repair.
                        n[2] = -LatticeSpacing(sourceCell);
                    }
                    if (bt.offCol >= 0)
                        *reinterpret_cast<uint32_t*>(out + bt.offCol) = a.col;
                    if (bt.offUv0 >= 0)
                    {
                        float* t = reinterpret_cast<float*>(out + bt.offUv0);
                        t[0] = bil(a.uv0[0], b.uv0[0], d.uv0[0], e.uv0[0]);
                        t[1] = bil(a.uv0[1], b.uv0[1], d.uv0[1], e.uv0[1]);
                    }
                    if (bt.offUv1 >= 0)
                    {
                        float* t = reinterpret_cast<float*>(out + bt.offUv1);
                        t[0] = bil(a.uv1[0], b.uv1[0], d.uv1[0], e.uv1[0]);
                        t[1] = bil(a.uv1[1], b.uv1[1], d.uv1[1], e.uv1[1]);
                    }
                }
            }

            for (int r = 0; r + 1 < rows; ++r)
            {
                const int srcRow = r / s;
                const uint16_t rowTop = static_cast<uint16_t>(base + r * cols);
                const uint16_t rowBot = static_cast<uint16_t>(base + (r + 1) * cols);
                int c = 0;
                while (c + 1 < cols)
                {
                    while (c + 1 < cols && !covered[gi][srcRow * (sp.cols - 1) + c / s]) ++c;
                    if (c + 1 >= cols) break;
                    const int c0 = c;
                    while (c + 1 < cols && covered[gi][srcRow * (sp.cols - 1) + c / s]) ++c;
                    // Each run is its own strip, laid out exactly as the client lays out a full row:
                    // leading duplicate, body, trailing duplicate. The duplicates bridge one run to
                    // the next and keep the winding parity, whether the gap is a masked-out cell, the
                    // end of a chunk, or the end of the batch. The leading duplicate is not
                    // decoration: dropping it shifts the parity of every triangle after it and the
                    // whole surface comes out wound the wrong way.
                    *w++ = static_cast<uint16_t>(rowTop + c0);
                    for (int k = c0; k <= c; ++k)
                    {
                        *w++ = static_cast<uint16_t>(rowTop + k);
                        *w++ = static_cast<uint16_t>(rowBot + k);
                    }
                    *w++ = static_cast<uint16_t>(rowBot + c);
                }
            }
            base += rows * cols;
        }

        const int written = static_cast<int>(w - idst);
        unlock(dev, nullptr, ourVb, 0);
        unlock(dev, nullptr, ourIb, 0);

        // The index view was sized to an upper bound; masked cells mean fewer were written.
        *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(ourIb) + liq::kBufCount) =
            static_cast<uint32_t>(written);
        *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(ourIb) + liq::kBufBytes) =
            static_cast<uint32_t>(written) * 2;

        // Contents are vouched for again: resizing a view clears the flag the draw tests. These are
        // our own views, so this states a fact about our own data rather than making the engine
        // believe something about its.
        *(static_cast<uint8_t*>(ourVb) + liq::kBufValid) = 1;
        *(static_cast<uint8_t*>(ourIb) + liq::kBufValid) = 1;

        // ALL FOUR FIELDS, ALWAYS. A descriptor is published whole or not at all: the type is as
        // much a part of describing this mesh as the counts are, and a field left as the caller
        // happened to leave it is caller stack being handed to a device. The layout written above
        // is strips bridged by duplicated vertices, which is this type.
        liq::BatchDesc desc{};
        desc.primType    = liq::kPrimTriangleStrip;
        desc.startIndex  = 0;
        desc.indexCount  = static_cast<uint32_t>(written);
        desc.vertexRange = static_cast<uint32_t>(verts - 1) << 16;

        // A strip yields (indices - 2) primitives, so anything shorter than a single triangle would
        // ask the device for a negative count. Nothing survived the mask here: describe a batch that
        // draws nothing, and let the group rebuild rather than hand that count over.
        if (written < 3)
        {
            desc.indexCount = 2;
            AskForRebuild(geom);
        }

        // PUBLISHED TOGETHER, TO THE DRAW AND TO THE RECORD. Most draws of this group never reach
        // here again -- the provider answers from its own cache and the emitter never runs -- so the
        // record is what carries the refinement forward, and it is re-presented as a whole: these
        // buffers with this descriptor, never one of them with the other's counterpart.
        owned->desc      = desc;
        owned->described = true;
        *bd    = desc;
        *vbOut = ourVb;
        *ibOut = ourIb;

        ++g_diag.refined;
        if (g_diag.peakSeenDist > g_diag.peakRefinedDist)
            g_diag.peakRefinedDist = g_diag.peakSeenDist;
    }

}
