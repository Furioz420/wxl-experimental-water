// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d9.h>
namespace wxl::water {
// These textures encode slopes, not display colors. The caller owns snapshot
// and restoration of sampler state around the water draw.
inline void BindLinearNormalSamplers(IDirect3DDevice9* device) {
    const unsigned stages[]{3,4,5,8,9,10,11};
    for (unsigned stage:stages) device->SetSamplerState(stage,D3DSAMP_SRGBTEXTURE,FALSE);
}
}
