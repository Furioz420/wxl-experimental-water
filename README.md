# Experimental Water for WarcraftXL

This repository contains the integrated wxl-experimental-water extension source. It supplies water surface, refraction, foam, shoreline, weather and fog rendering, plus the in-game tuning panel and localized tuner strings. The extension uses WarcraftXL's render and device lifecycle; it is built as a Win32 DLL inside a compatible wxl-core checkout.

## Build and deploy

Place this repository at extensions/wxl-experimental-water in the matching core source tree. Configure the core for Win32, then build the wxl-experimental-water target in Release. The core's extension discovery reads shared.cmake, target.cmake, and vfog.cmake. The matching core excludes the donor-only Lattice.cpp, Owned.cpp, and Ride.cpp translation units from this target while retaining their source for provenance. Do not enable those files without the lifecycle hooks they require.

Deploy wxl-experimental-water.dll to extensions/wxl-experimental-water/ and src/wxl-experimental-water/lang/wxl-water-lang.json to the client as wxl-water-lang.json. Keep its WarcraftXL core and D3D9 proxy from the same tested runtime set; mixing release families has not been validated. The client also reads wxl-forever-water.ini and wxl-vfog-effects.ini for saved settings.

The selected local v1.2.296 client candidate and its exact hashes, test steps, and rollback are recorded in [TESTING.md](TESTING.md). A source commit or successful build alone does not establish in-game acceptance.

## Credits and data

WarcraftXL contributors and Furioz developed the local integration. The vendored CoAVolFog subset comes from [jealous-sound/coa-vfog](https://github.com/jealous-sound/coa-vfog), revision 4e31ddf53a326ba9c0ad03488b4c30f7ce23c969; its original license, README, notices, and file-level provenance are retained under vendor/coa-vfog/. The repository uses GPL-3.0 and retains upstream's additional permission text. fogdata.bin and waterdata.bin are derived from game data; review redistribution rights separately before creating a public binary or source release containing them.
