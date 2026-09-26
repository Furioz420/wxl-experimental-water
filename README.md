# Experimental Water

This repository is reserved for the WXL module `wxl-experimental-water`. The module currently lives in the WXL v1.1 integration checkout; this documentation PR contains **no source or release DLL**. Active water and shared-renderer edits are still being reconciled, so an older file copy would give reviewers a misleading implementation.

## Integration and release checks

The water module covers surface rendering, refraction, volume effects, ride interaction, and tuner text. It depends on matching core D3D9/render/event APIs, the environment support code and shared ImGui integration. The integrated target also deploys `wxl-water-lang.json` when `CLIENT_PATH` is set. Keep the DLL, tuner strings, and any client data as explicitly reviewed package components; do not publish a local patch archive by wildcard.

After the shared renderer API and water changes are committed, copy a pinned source snapshot into a separate review update, build Win32 against that exact core, and test world transitions, water visibility and refraction, device reset, tuner controls, performance, and rollback. Preserve HD quality during optimization. Until those checks pass, this README does not represent a working standalone package.

## Credits and license

Preserve WarcraftXL source notices and the GPL-3.0 license when source is added. Furioz's local integration changes remain attributed in the integration Git history. Client textures and other external assets are not included.
