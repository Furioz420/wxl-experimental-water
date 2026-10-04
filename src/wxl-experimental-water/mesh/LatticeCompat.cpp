// Split-core compatibility surface for the experimental Water renderer.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#include "wxl-experimental-water/mesh/Lattice.hpp"

#include "common/Log.hpp"

#include <algorithm>
#include <cmath>

namespace
{
    constexpr float kStockCell = 33.333333f / 8.0f;
    int   g_subdivision = 1;
    int   g_rebuildBudget = 0;
    float g_cullMargin = 0.0f;
    float g_nearAlways = 0.0f;
    wxl::water::Batch g_batch;
    int g_depthHistogram[1] = {};
}

namespace wxl::water
{
    bool PlaneAt(float, float, WaterPlane&) { return false; }

    void BeginFrame() {}

    int Subdivision() { return g_subdivision; }
    void SetSubdivision(int factor) { g_subdivision = std::clamp(factor, 1, 8); }

    int RebuildBudget() { return g_rebuildBudget; }
    void SetRebuildBudget(int perFrame) { g_rebuildBudget = std::max(perFrame, 0); }

    float FinestVertexSpacing()
    {
        return kStockCell / static_cast<float>(std::max(g_subdivision, 1));
    }

    float CullMargin() { return g_cullMargin; }
    void SetCullMargin(float margin) { g_cullMargin = std::max(margin, 0.0f); }

    float NearAlwaysRadius() { return g_nearAlways; }
    void SetNearAlwaysRadius(float radius) { g_nearAlways = std::max(radius, 0.0f); }

    int DepthBuckets() { return 1; }
    const int* DepthHistogram() { return g_depthHistogram; }
    float DepthForBucket(float bucket) { return std::max(bucket, 0.0f); }

    void BeginBatch()
    {
        // The stock-lattice path does not populate the capture payload. Reset its
        // validity metadata only: Batch{} clears and copies almost 3 MB for every
        // visible chunk batch, even though neither array is consumed here.
        g_batch.gridCount = 0;
        g_batch.vertCount = 0;
        g_batch.overflow = false;
        g_batch.emitted = false;
        g_batch.stride = 0;
        g_batch.offPos = 0;
        g_batch.offNrm = g_batch.offCol = g_batch.offUv0 = g_batch.offUv1 = -1;
    }
    Batch& Current() { return g_batch; }

    bool InstallHooks()
    {
        WLOG_WARN("water: split-core canary uses the stock liquid lattice; refinement and ride hooks "
                  "are disabled until their offsets are independently verified");
        return true;
    }

    void Densify(void*, void**, void**, void*) {}
    void EnsureDrawable(void*) {}
}
