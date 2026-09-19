# 3MF-001 spike fixtures

The `*.base64` files decode to binary 3MF packages. Keeping the transport as
text makes the small feasibility corpus reviewable by the repository tooling;
tests decode to temporary files and pass only inherited handles to the real
AppContainer worker.

The original six source packages came from the lib3mf 2.5.0 test corpus at
upstream commit `64bb454d1fcb53effa57d3cef752a10d740d41a2` and are covered
by [`LIB3MF-LICENSE.txt`](LIB3MF-LICENSE.txt). The static Production derivative
described below is product-authored from that source.

| Fixture | Upstream path | Decoded bytes | SHA-256 |
| --- | --- | ---: | --- |
| `core-box.3mf.base64` | `Tests/TestFiles/CPP_UnitTests/Box.3mf` | 1,680 | `E2633A8C2E014D5F8C612F13F64F48D4E722972C0FB0923795FA83DE5AEE24F6` |
| `nested-components.3mf.base64` | `Tests/TestFiles/Production/HierarchicalComponent.3mf` | 1,398 | `B5FA98899B990C6BCFB615992D8A21739F6B722A05B4070EC08D591BD277B965` |
| `production-boxes.3mf.base64` | `Tests/TestFiles/Production/2ProductionBoxes.3mf` | 16,568 | `CC479831E02D01F4696719773B2F4BAEB61BD9F2D3AC3B37CDD9F9FD2078DD97` |
| `materials-texture.3mf.base64` | `Tests/TestFiles/CPP_UnitTests/Texture.3mf` | 24,582 | `52F787357062DA8BFBB14C1B5258A11513717B4BEB072B00AA0161AA76A34B36` |
| `beam-lattice.3mf.base64` | `Tests/TestFiles/BeamLattice/Box_Simple.3mf` | 1,730 | `6814EF817D4845B76717BB33F56E06168758F34A5F0B6AF94DC76F5CDCE0E045` |
| `beam-representation.3mf.base64` | `Tests/TestFiles/BeamLattice/Box_Attributes_Positive.3mf` | 1,566 | `28248E56B8590EA7E2C33CC375CFA9CDDA89EB98B31AE22EE5072E19D058C8BF` |

`nested-components` removes unused extension namespace declarations that make
lib3mf strict mode reject the upstream package, then repacks it deterministically;
its Core/Production geometry and component relationships are unchanged. The
test also builds a deterministic 128-level Core component package at runtime so
the checked-in corpus stays small.

`production-boxes` declares Slice as required in every model part. The shipping
viewer does not support Slice, so 3MF-007 retains it as a rejection sample.
`static-production.3mf.base64` is a deterministic product-authored repackage
of that source with `requiredextensions="s p"` changed to `"p"` in all three
model parts. Its Slice payloads are optional and the standard Production build
remains unchanged. The decoded file is 16,164 bytes with SHA-256
`2efcd56f2d5cd3bb09b66cf902b401a9dd17ada4396b65658ada531a4637044e`.
The transformation is reproducible with `make_slice_optional` and
`rewrite_package` in `../3mf/verify.py`; the committed base64 is the frozen
test input and verification never rewrites it.
