# Experimental Water

This repository is reserved for the WXL module `wxl-experimental-water`. The module currently lives in the local WXL integration checkout; this `main` branch contains **no source or release DLL**. Active water and shared-renderer edits are still being reconciled, so an older file copy would give reviewers a misleading implementation.

## Source publication status (2026-10-06)

The attempted [source refresh](https://github.com/Furioz420/wxl-experimental-water/pull/2) is closed. Its mesh code did not build against the aligned Furioz420/wxl-core `853217d7b0441e95eed2ba092f291dbd6626e6c9`: it references liquid hook types and offsets absent from that core, including `ChunkGeomPurgeFn`, `MapChunkBufFreeFn`, and `LiquidGetAaBoxFn`. Do not fill those offsets with guessed addresses. The exact refresh tip `9cbdc5cdf257032f99d1e182e2a41769208b5e24` and other removed branch tips are preserved in the verified local `wxl-experimental-water.bundle` archive. Publish source after the missing hook contract is established and a Release/Win32 build passes.

## Integration and release checks

The water module covers surface rendering, refraction, volume effects, ride interaction, and tuner text. It depends on matching core D3D9/render/event APIs, the environment support code and shared ImGui integration. The integrated target also deploys `wxl-water-lang.json` when `CLIENT_PATH` is set. Keep the DLL, tuner strings, and any client data as explicitly reviewed package components; do not publish a local patch archive by wildcard.

After the shared renderer API and water changes are committed, copy a pinned source snapshot into a separate review update, build Win32 against that exact core, and test world transitions, water visibility and refraction, device reset, tuner controls, performance, and rollback. Preserve HD quality during optimization. Until those checks pass, this README does not represent a working standalone package.

## Credits and license

Preserve WarcraftXL source notices and the GPL-3.0 license when source is added. Furioz's local integration changes remain attributed in the integration Git history. Client textures and other external assets are not included.
