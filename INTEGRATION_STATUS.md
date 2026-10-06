# Integration source and build status - 2026-10-06

This repository contains the `wxl-experimental-water` extension maintained under Furioz420.
This repository provides source, not an installable client release.

## Provenance

Source: `50c2f0d82a02216b6e1542eb33b49b617ea41d0b` from the local WXL integration workspace. Existing repository
licenses, attribution, packaging, and independently maintained files are retained.
No game client, database dump, or server credentials are included.

## Build and installation requirements

Build this extension inside a compatible WXL core with the matching extension APIs.
Standalone CI, release packaging, and client acceptance remain separate gates.
Validate the matching client installation before publishing a binary release.

## Aligned source build - 2026-10-06

`shared.cmake` selects the stock-topology `LatticeCompat.cpp` adapter used by the
existing client and excludes `mesh/Lattice.cpp`, `mesh/Owned.cpp`, and
`world/Ride.cpp`. Those donor units require liquid lifecycle hooks that have not
been verified in the v1.2.296-aligned core. The Release/Win32
`wxl-experimental-water` target built successfully against
Furioz420/wxl-core `853217d7b0441e95eed2ba092f291dbd6626e6c9` with
Visual Studio 2026. The built DLL has the same size as the working client DLL
and differs in 15 bytes; client gameplay acceptance is still pending.
