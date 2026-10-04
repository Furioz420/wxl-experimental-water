// The liquid lattice, subdivided: enough vertices for a wave to have a shape.
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
// WHAT THE MAP'S OWN WATER IS FOR
//   The map declares where water is, how far it extends, which cells of a chunk it covers, and what
//   height it sits at. That -- a mask and a plane -- is everything we want from it, and it is
//   exactly what MH2O carries and what the server also holds. Every other thing the client does with
//   it is replaced.
//
//   Taking the mask as geometry rather than rebuilding it is not a shortcut. A shoreline is placed
//   per cell, so the footprint is ragged by design, and the client's own index run states it exactly.
//   A grid of our own would have to rediscover that, and would be wrong at every coast.
//
// WHY IT HAS TO BE SUBDIVIDED
//   The sheet carries one vertex every ~4 world units: nine per chunk edge over a 33-unit chunk. A
//   twenty-unit wave gets five vertices, which is enough to LEAN a face and not enough to shape one.
//   No amount of tuning moves that ceiling; only vertices do.
//
//   What it does not buy: a breaking wave that curls is not a height field -- its lip overhangs, so
//   two surface points sit above one ground point -- and a denser grid still cannot fold over itself.
//
// TWO MEASURED FACTS ABOUT THE BATCH, both fatal to get wrong:
//   * ONE BATCH HOLDS SEVERAL CHUNKS. The emitter runs once per chunk and they accumulate into one
//     pair of streams -- a batch was observed carrying 306 vertices where a single chunk emits 81.
//   * THE LATTICE IS A TRIANGLE STRIP. Its stock index count was 562, which is not divisible by
//     three, so it cannot be a list whatever the primitive enum happens to look like.

#pragma once

#include <cstdint>

#include "wxl-experimental-water/sea/Query.hpp"

namespace wxl::water
{
    /// The still-water plane under a world point, or `present = false` if no water has been drawn
    /// there this session.
    ///
    /// Recorded as the geometry goes past rather than queried from the map, because the emitter hands
    /// us the flat sheet the map declares -- position and column both -- and that IS the plane. It
    /// persists once seen: a plane does not move, so a record from an earlier rebuild is as good as a
    /// fresh one, which matters because most draws are served from the cache and rebuild nothing.
    bool PlaneAt(float x, float y, WaterPlane& out);

    /// One vertex as the client's emitter left it, in the emitter's own order (rows outer, columns
    /// inner), which is what makes each chunk reconstructible as a grid.
    struct SourceVertex
    {
        float    pos[3];
        float    nrm[3];
        uint32_t col;
        float    uv0[2];
        float    uv1[2];
    };

    /// One chunk's lattice inside the batch's shared vertex pool.
    struct GridSpan { int rows, cols, first; };

    /// Chunks one batch may carry, and source vertices behind them.
    ///
    /// THIS IS THE SEA'S DRAW DISTANCE. A batch that arrives with one chunk more than fits does not
    /// degrade, it FAILS: the overflow flag drops the whole batch back to stock, so the sea would
    /// end at a line rather than fade at one. So the pool is sized to the real ceiling rather than
    /// to a guess -- a batch descriptor states its vertex count in sixteen bits, so no batch can
    /// present more than 65536 source vertices however much water is in view, and matching that
    /// makes the capture unable to be the binding constraint. The cost is static and known, about
    /// three megabytes.
    constexpr int kMaxGrids     = 512;
    constexpr int kMaxPoolVerts = 65536;
    /// Cells one chunk's coverage mask can describe. A chunk's lattice is nine vertices to a side
    /// and so needs 64; a cell beyond the bound is treated as uncovered, which is a hole in the
    /// water rather than a write past the end of anything. Kept tight because the mask array is the
    /// product of this and kMaxGrids, and it is cleared once per batch.
    constexpr int kMaxGridCells = 256;

    /// Everything the emitter wrote for the batch currently being built.
    struct Batch
    {
        int  gridCount = 0;
        int  vertCount = 0;
        bool overflow = false;   ///< a chunk did not fit; the whole batch falls back to stock
        /// THE EMITTER RAN, whether or not a single chunk survived the capture.
        ///
        /// Distinct from `gridCount > 0`, and the distinction is not academic: a batch whose FIRST
        /// chunk is rejected records no grid at all, which is byte for byte what a draw served from
        /// the provider's cache looks like. The two need opposite handling -- a cache answer leaves
        /// the buffers alone, a rejected rebuild has just replaced them with stock geometry -- so
        /// telling them apart by grid count alone silently took the wrong branch on every rejected
        /// shoreline chunk.
        bool emitted = false;
        int  stride = 0;
        int  offPos = 0, offNrm = -1, offCol = -1, offUv0 = -1, offUv1 = -1;
        GridSpan     grids[kMaxGrids];
        SourceVertex v[kMaxPoolVerts];
    };

    /// Opens a frame: renews the allowance of chunk groups we may ask to be rebuilt.
    ///
    /// Rebuilds are how a group reaches the refinement at all -- the provider otherwise answers from
    /// its own cache and never runs the vertex emitter -- and they are rationed because a camera
    /// swinging onto open water brings hundreds of groups into view at once.
    void BeginFrame();

    /// How finely each source cell is split, per side. Live, so the cost can be traded against the
    /// shortest wave the sea is currently carrying.
    ///
    /// ONE FACTOR EVERYWHERE, and grading it by distance is not on offer: a group's refinement is
    /// baked into buffers the surrounding engine caches, and the only way to revise it afterwards is
    /// to have that group re-emitted -- a request measured landing about one time in twenty. Any
    /// distance grading would therefore be computed correctly, applied once, and then frozen for as
    /// long as the group stayed loaded, so water first built far away would stay coarse while you
    /// swam over it.
    ///
    /// What makes one factor affordable is a measurement: a batch peaks at four chunks, which at the
    /// full factor is 16900 output vertices against a ceiling of 63488.
    int  Subdivision();
    void SetSubdivision(int factor);

    /// Chunk groups that may be asked to rebuild in one frame.
    ///
    /// For one thing only: a group never re-emitted while we were watching has never passed through
    /// our hands, so it draws as stock water. This is how many of those first sightings a frame may
    /// chase.
    int  RebuildBudget();
    void SetRebuildBudget(int perFrame);


    /// Finest vertex spacing the refined lattice reaches, in world units -- the source sheet's own
    /// cell over the full subdivision factor, and now simply THE spacing, since every chunk gets the
    /// same factor. This is the yardstick of what the mesh can DRAW: a detail wavelength under a few
    /// of these cannot be restituted by the geometry anywhere, and the panel names those cascades as
    /// shading-only instead of letting them be discovered as seething vertices.
    float FinestVertexSpacing();



    /// Flat allowance added to the bounds the client culls liquid against, in world units, on top
    /// of the sea's own measured excursion.
    ///
    /// The measured part covers where the waves ARE; this covers the fact that the box is built
    /// from this frame's sea and tested against a frustum that has already moved. Raise it if
    /// turning quickly opens holes at the edge of the screen; it costs the chunks that are drawn
    /// and turn out not to have been visible.
    float CullMargin();
    void  SetCullMargin(float margin);

    /// Radius around the camera inside which a water chunk is drawn without being tested at all,
    /// in world units.
    ///
    /// The margin above answers "the box is the wrong SIZE"; this answers "near the camera, the box
    /// is the wrong SHAPE". The bounds are a flat sheet, and a chunk a few units away fills a large
    /// part of the screen -- so a wrong answer there is a hole in the middle of the view rather than
    /// a chunk missing on the horizon. Swimming is the case it is for: the eye is inside the water,
    /// and the surface running past overhead belongs to the chunks whose sheet is closest to
    /// edge-on against the frustum.
    ///
    /// Costs every chunk inside the radius, drawn whether or not it was visible. Zero restores the
    /// plain test.
    float NearAlwaysRadius();
    void  SetNearAlwaysRadius(float radius);

    /// WHERE THIS MAP'S WATER ACTUALLY IS, in depth, measured over the water drawn last frame.
    ///
    /// Every shore setting is a depth, and a depth is the one quantity in this system nobody can
    /// see: standing on a beach, there is no way to know whether the transition set at twelve units
    /// happens a step away or a hundred. These make the panel able to say so, which is the
    /// difference between tuning the shore and guessing at it.
    ///
    /// The histogram is log-spaced -- a beach is under a unit, open ocean is hundreds -- so bucket
    /// edges come from DepthForBucket() rather than from arithmetic at the call site.
    int        DepthBuckets();
    const int* DepthHistogram();
    /// Depth at a bucket's low edge. Takes a float so a curve drawn over the same axis can be
    /// sampled per pixel rather than per bucket, without a second copy of the spacing rule.
    float      DepthForBucket(float bucket);

    /// Starts a fresh batch. Called before the stock GetBuffers, which is what the emitter runs under.
    void BeginBatch();

    /// The batch under construction, appended to by the vertex-emit detour.
    Batch& Current();

    /// Detours the liquid chunk cache's allocator so its buffers are reserved at the subdivided size.
    /// Nothing else can do it: those buffers are created once, sized to the counts requested there,
    /// and every later draw of that chunk group reuses them.
    bool InstallHooks();

    /**
     * @brief Rewrites the just-built liquid lattice as a subdivided one, and restates its batch.
     *
     * Called from the geometry provider's detour after the stock call, where the buffers and the
     * batch descriptor have just been filled. The buffers written here belong to that chunk group's
     * cache entry and to nothing else, so the refinement survives in the cache and every later draw
     * of the group gets it back without rebuilding anything.
     *
     * Writes nothing whenever anything is missing or over capacity, leaving the client's own lattice
     * exactly as it built it: a failure here costs detail and never a draw.
     *
     * @param geom       the geometry factory, whose cached copy of the descriptor is restated too --
     *                   without that, the refinement is drawn once and misdescribed forever after.
     * @param vbOut      the vertex stream, rewritten in place.
     * @param ibOut      likewise for indices. Read FIRST: the client's index run is what states which
     *                   cells of the lattice are actually covered.
     * @param batchDesc  the four-dword batch the draw is about to issue; its counts are rewritten.
     */
    void Densify(void* geom, void** vbOut, void** ibOut, void* batchDesc);

    /**
     * @brief Makes a batch the provider left unwritten harmless, at the last seam before the draw.
     *
     * NOT THIS MODULE'S INVARIANT TO REPAIR, which is why it is neutralised here rather than fixed
     * upstream: the fix would be in the client. Its rebuild path returns WITHOUT filling the
     * descriptor it was handed whenever both views already report valid and GPU-resident -- plain
     * in the disassembly, and reached in the ordinary run of play. What the caller holds is then
     * its own stack, and the device divides a primitive count by a table entry that does not exist
     * for the type it finds there. Nothing downstream of this call can tell that apart from a real
     * batch, so it stops here.
     *
     * @param batchDesc  the four-dword batch the draw is about to issue.
     */
    void EnsureDrawable(void* batchDesc);
}
