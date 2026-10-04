// Liquid geometry this module owns: allocation, lookup, and the two release seams.
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

#include "wxl-experimental-water/mesh/Owned.hpp"

#include "common/Log.hpp"
#include "engine/hook/Hook.hpp"
#include "game/Binding.hpp"

#include <cstdint>
#include <unordered_map>

namespace
{
    namespace liq = wxl::offsets::engine::liquid;

    liq::ChunkGeomPurgeFn      g_origPurge = nullptr;
    liq::ChunkGeomDestructorFn g_origDtor  = nullptr;

    std::unordered_map<void*, wxl::water::OwnedGeometry> g_owned;

    /// The tag the client stamps its own liquid buffers with, mirrored from its own allocation so
    /// ours are accounted for and freed exactly like the engine's. It passes a literal zero at its
    /// one call site, which is a tag like any other -- treating "no tag yet" as a reason to stand
    /// down refused every allocation this module ever asked for.
    uint32_t g_allocTag = 0;

    /// Pairs asked of the engine since boot. Read next to the live count: the two together say
    /// whether the release detours are firing, which nothing else in the game would reveal.
    int g_allocations = 0;

    // SIZES ARE ROUNDED UP, AND IT IS NOT A DETAIL OF ACCOUNTING.
    //
    // A miss on the free list does not hand out memory, it calls PoolCreate twice -- two real GPU
    // buffers -- and the list only ever matches an EXACT pair of byte sizes. The refined counts
    // vary continuously with the shape of the grids in the batch, so asking for them verbatim
    // supplies a nearly unbounded set of sizes: every group creates pools no other group can reuse,
    // and a camera swinging across open water creates them by the dozen. What that looks like from
    // the game is everything that needs a buffer flickering out and back -- doodads, the sky, water
    // chunks, the interface -- because they are the ones asking for one at the same moment.
    //
    // The client's own counts come from a small set of grid shapes, which is exactly why its free
    // list works. Rounding puts ours back into a small set too: the common batch lands in one or
    // two classes and its buffers are handed round instead of being rebuilt.
    constexpr int kVertexStep = 2048;
    constexpr int kIndexStep  = 4096;

    int RoundUp(int value, int step) { return ((value + step - 1) / step) * step; }

    void* AllocPair(int vertexCount, int indexCount)
    {
        ++g_allocations;
        return wxl::game::Native<liq::MapChunkBufAllocFn>(liq::kMapChunkBufAlloc)(
            vertexCount, indexCount, g_allocTag, liq::kVertexFormatLiquid);
    }

    void FreePair(void* chunkBuf)
    {
        wxl::game::Native<liq::MapChunkBufFreeFn>(liq::kMapChunkBufFree)(chunkBuf);
    }

    void ReleaseRecord(void* geom)
    {
        auto it = g_owned.find(geom);
        if (it == g_owned.end()) return;
        if (it->second.chunkBuf) FreePair(it->second.chunkBuf);
        g_owned.erase(it);
    }

    /// CChunkGeomFactory::Purge -- the group drops its geometry but goes on living, so the record
    /// goes with the geometry and the next refinement builds a fresh one.
    void __fastcall HkPurge(void* geom)
    {
        ReleaseRecord(geom);
        g_origPurge(geom);
    }

    /// The factory destructor. Released before the original runs, while the key is still a group.
    void* __fastcall HkDtor(void* geom, void* edx, uint8_t freeFlags)
    {
        ReleaseRecord(geom);
        return g_origDtor(geom, edx, freeFlags);
    }
}

namespace wxl::water
{
    void NoteAllocTag(uint32_t tag) { g_allocTag = tag; }

    int OwnedCount()       { return static_cast<int>(g_owned.size()); }
    int OwnedAllocations() { return g_allocations; }

    OwnedGeometry* FindGeometry(void* geom)
    {
        if (!geom) return nullptr;
        auto it = g_owned.find(geom);
        return (it == g_owned.end()) ? nullptr : &it->second;
    }

    OwnedGeometry* AcquireGeometry(void* geom, int vertexCount, int indexCount)
    {
        if (!geom || vertexCount <= 0 || indexCount <= 0) return nullptr;

        // Rounded before anything else, so the capacity kept on the record is the class the pair
        // really belongs to: a group whose next batch is a little larger then keeps the same pair
        // instead of trading it for another of nearly the same size.
        vertexCount = RoundUp(vertexCount, kVertexStep);
        indexCount  = RoundUp(indexCount, kIndexStep);

        OwnedGeometry& o = g_owned[geom];
        if (o.chunkBuf && o.vertexCap >= vertexCount && o.indexCap >= indexCount) return &o;

        // A pair sized for less than the group now needs is returned whole rather than grown: the
        // free list is keyed on exact byte sizes, so a released pair is immediately reusable by the
        // next group that wants those dimensions, and a group's geometry settles after its first
        // refinement anyway.
        if (o.chunkBuf)
        {
            FreePair(o.chunkBuf);
            o = OwnedGeometry{};
        }

        void* pair = AllocPair(vertexCount, indexCount);
        if (!pair)
        {
            // Said out loud, and early. Refusing here is invisible from the game -- the water
            // simply draws stock, exactly as it does when everything is fine but nothing is in
            // range -- so a refusal that happens EVERY time reads as "no crash" rather than as a
            // feature that never ran once.
            static int said = 0;
            if (said < 3)
            {
                ++said;
                WLOG_WARN("water: no buffer pair for %d verts / %d indices -- this group draws "
                          "stock water", vertexCount, indexCount);
            }
            g_owned.erase(geom);
            return nullptr;
        }
        o.chunkBuf  = pair;
        o.vb        = *reinterpret_cast<void**>(static_cast<uint8_t*>(pair) + liq::kChunkBufVertex);
        o.ib        = *reinterpret_cast<void**>(static_cast<uint8_t*>(pair) + liq::kChunkBufIndex);
        o.vertexCap = vertexCount;
        o.indexCap  = indexCount;
        o.described = false;
        if (!o.vb || !o.ib)
        {
            FreePair(pair);
            g_owned.erase(geom);
            return nullptr;
        }
        return &o;
    }

    bool InstallGeometryOwnership()
    {
        bool ok = wxl::hook::Install("Water_ChunkGeomPurge", liq::kChunkGeomPurge,
                                     &HkPurge, &g_origPurge);
        ok &= wxl::hook::Install("Water_ChunkGeomDestroy", liq::kChunkGeomDestructor,
                                 &HkDtor, &g_origDtor);
        return ok;
    }
}
