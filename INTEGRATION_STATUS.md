# Integration source status - 2026-10-07

The 176 implementation files in this repository match E:\WXL1.1\extensions\wxl-experimental-water byte-for-byte after CRLF/LF normalization. The source snapshot originated from local WXL integration commit 50c2f0d82a02216b6e1542eb33b49b617ea41d0b; the current integration checkout contains other unrelated uncommitted work, which was not copied here.

The selected local client runtime labeled v1.2.296 has matching core/proxy/water hashes in the DevAPI RC1 package and C:\Eunoia Wxl Mpq. This establishes package alignment, not a fresh build of this repository against the public core or live gameplay acceptance. The public water repository previously contained documentation only; this main-branch source update closes that source gap.

Build requirements and the test/rollback gate are in [README.md](README.md) and [TESTING.md](TESTING.md). Do not publish the vendored game-derived datasets without a separate redistribution review.
