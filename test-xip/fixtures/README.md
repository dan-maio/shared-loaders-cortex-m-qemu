# test-xip/fixtures — XIP-split relinked `.so`

Unlike `../test/fixtures` (single fused RWX blobs), the XIP apps need each
fixture **relinked with the split `R-X | RW` PHDRS layout** (Blocker A), using
`../fixtures-src/ldscripts/xip-split.ld`. `cpp3.so` already has this layout and
is the working proof-of-concept; the rest are regenerated here by the fixture
build. Prebuilt split `.so` land in this directory.
