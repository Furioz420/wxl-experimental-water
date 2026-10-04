// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ViewerLiquid.hpp"
#include "offsets/game/ADT.hpp"
namespace wxl::water
{
    // Read only during the world render interval, after native scene update.
    inline ViewerLiquid ReadViewerLiquid()
    {
        ViewerLiquid liquid;
        liquid.type=reinterpret_cast<wxl::offsets::game::adt::GetViewerLiquidFn>(
            wxl::offsets::game::adt::kGetViewerLiquid)(&liquid.depth);
        return liquid;
    }
}
