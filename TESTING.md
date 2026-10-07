# Selected v1.2.296 client test

The selected local candidate is already installed in C:\Eunoia Wxl Mpq; the three files below matched E:\WXL1.1\publish\packages\devapi-full-20261006-rc1\devapi\downloads\game\2\sd on 2026-10-07. This source update does not replace the active client DLLs.

| Client file | SHA-256 |
| --- | --- |
| WarcraftXL.dll | 73C2CC3FAAC8F6FFC195EDD3CB4AE13143B0961872493DF17B97C3FFC1E852F9 |
| d3d9.dll | D6101E9CCE167225AB5C21DCD173B5E224A0AB3DAA63BFB5A86F053D56194525 |
| extensions/wxl-experimental-water/wxl-experimental-water.dll | 5E9DEC5F8E8429930D011A9DFDA51E7F5A40B86ED5C43B39C99B299F9EDCE75B |

The verified rollback is at C:\Eunoia Wxl Mpq\_wxl_publish_rollback\water-v1296-20261007. Its manifest records hashes for the three DLLs, tuner strings, and four water/fog settings files. The 2026-10-07 startup log reports the water extension loaded and its 456 tuner strings available; visual acceptance remains pending.

Launch the client from this folder, confirm the water module loads, then inspect a shoreline, shallow and deep water, reflections/refraction, foam, and the in-game tuner. Travel between a water-heavy zone and a city, change a water setting and restore it, and exercise a graphics/device reset if available. Watch for black terrain or clothing, state leakage after water draws, lost shadows, a crash, or unexpectedly reduced image quality. Record the client log and screenshots for any failure.

For rollback, close WoW and restore the verified backup of the whole core/proxy/water trio and the two settings files together. Do not replace a single DLL from a different runtime family.
