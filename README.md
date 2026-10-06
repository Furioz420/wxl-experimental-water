# Experimental Water

This repository contains the `wxl-experimental-water` client extension source: surface and refraction rendering, atmosphere and volume effects, and the water tuning panel. It builds as a WarcraftXL extension inside the WXL core checkout.

## Source publication status (2026-10-06)

The [first source refresh](https://github.com/Furioz420/wxl-experimental-water/pull/2) did not build because three donor translation units depend on liquid hook types and offsets absent from the aligned core (`ChunkGeomPurgeFn`, `MapChunkBufFreeFn`, `LiquidGetAaBoxFn`, and related lifecycle hooks). The integrated client already runs the stock-lattice `LatticeCompat.cpp` adapter instead. `shared.cmake` now makes that selection inside this repository, so a standalone checkout builds the same feature set. The donor refinement, geometry ownership, and visual ride source remains available for later verified integration; it is not part of this DLL. Do not fill missing offsets with guessed addresses.

The extension built in Release/Win32 against Furioz420/wxl-core `853217d7b0441e95eed2ba092f291dbd6626e6c9` with Visual Studio 2026 on 2026-10-06. Its built DLL is the same size as the working client DLL and differs in only 15 bytes; this is a binary compatibility observation, not an in-game acceptance test.

## Integration and release checks

The module depends on matching core D3D9/render/event APIs, the environment support code, and shared ImGui integration. The `wxl-water-lang.json` tuner strings belong beside the client executable; `target.cmake` copies them when `CLIENT_PATH` is set. Keep the DLL, tuner strings, and any client data as explicitly reviewed package components; do not publish a local patch archive by wildcard.

Before publishing a binary release, test world transitions, water visibility and refraction, device reset, tuner controls, performance, and rollback with the exact built DLL and client data. Preserve HD quality during optimization. The source build alone does not establish those runtime results.

## Credits and license

WarcraftXL source notices and the GPL-3.0 license are preserved. Furioz's local integration changes are attributed in the Git history. Client textures and other external assets are not included.
