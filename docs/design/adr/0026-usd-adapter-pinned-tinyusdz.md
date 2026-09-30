# 0026 — USD/USDZ adapter: stream-only product adapter over the pinned TinyUSDZ reader

## Status
accepted

## Context
T33 must render the supported static `.usd`/`.usda`/`.usdc`/`.usdz` preview subset in Explorer from the
Shell's `IInitializeWithStream` stream, never the viewer and never the OpenUSD compatibility host
(USD-010). The worker already routes the same subset through a pinned TinyUSDZ 0.9.1 reader plus a
product-owned USDZ preflight; the provider has no compatibility process and no second-chance path, so
composition-dependent input is simply the generic icon. TinyUSDZ has no allocation or progress callback,
and the frozen `VertexSample`/`MaterialPayload` contracts have no texture sampling. Separately, linking the
static TinyUSDZ and lib3mf libraries into one module (the provider DLL, `Tests.Unit.exe`,
`Tests.ProviderHost.exe`) exposed an ODR/COMDAT collision: TinyUSDZ's vendored `fast_float` and lib3mf's
vcpkg `fast_float` both emit `fast_float::from_chars_advanced<double>` with incompatible
`parse_options_t` layouts, and the linker's arbitrary pick corrupted TinyUSDZ's ASCII parse.

## Decision
1. **Reader boundary:** the provider compiles the pinned TinyUSDZ 0.9.1 static reader directly (the same
   overlay-vendored library the worker uses), fed only the bounded in-memory stream. No file path,
   worker, host, cache or network access is ever performed. The translation unit stays PCH/COM/GDI-free
   so the DLL and both test hosts compile the exact shipped source.
2. **Container sniffing:** the extension never selects the encoding. ZIP local header → USDZ, the crate
   magic `PXR-USDC` → USDC, a leading `#usda` (after an optional UTF-8 BOM and whitespace) → USDA,
   anything else is malformed. `.usd` is classified by bytes; a suffix mismatch cannot fabricate a route.
3. **Archive boundary:** a USDZ stream is validated by the worker's product `UsdzZipPreflight`
   (`InspectUsdz`, compiled source-not-state into the provider) under provider ceilings — 256 MiB stream,
   128 MiB aggregate expansion, 100:1 ratio, 4096 entries, 32-level normalized paths, stored-only,
   checked non-overlapping offsets and CRC. Nothing is extracted; entries remain views into the brokered
   stream.
4. **Composition and external assets fail closed:** `load_assets`, `do_composition`, `load_sublayers`,
   `load_references` and `load_payloads` stay disabled. A wildcard resolver resolves only names present in
   the USDZ entry map; any other request becomes `UnsafeReference`. Every composition arc (sublayers,
   references, payloads, inherits, specializes, variants, clips, instanceable) is classified before
   conversion and returns `UnsupportedComposition` (generic icon). The provider never recovers a path,
   starts the compatibility host, reads the derived cache, or reaches the network.
5. **Static policy:** the `UsdPreviewSurface`/display-color values are normalized into the frozen
   `MaterialPayload`; finite sampled triangles carry double-precision world transforms, purpose/visibility
   and point-instancer expansion under the shared caps. Skeletal bindings are stripped so the authored rest
   pose previews. Contained textures are recorded but not decoded — the frozen payload has no texture slot,
   so an absent or external image never fabricates geometry, and a required-only geometry omission fails
   closed rather than claim success.
6. **fast_float ABI alignment:** the TinyUSDZ overlay port (now `0.9.1#3`) patches
   `src/ascii-parser-basetype.cc` to include the vcpkg-pinned `fast_float` and links `FastFloat::fast_float`
   into the static target, so TinyUSDZ and lib3mf share one `fast_float` ABI. Rebuilding TinyUSDZ was
   preferred over a consumer-side guard, which could not keep both libraries correct in one module.

## Consequences
- The provider is import-clean (TinyUSDZ static; no viewer/worker/host/core imports) and stream-only; the
  crash-containment boundary is unchanged. The DLL still exports exactly the two COM entry points.
- Because there is no compatibility host in the provider, a scene the viewer would compose through OpenUSD
  is deliberately the generic icon here; USD-010 records Explorer thumbnails as a stream-only subset.
- The `fast_float` alignment is a dependency-build decision: advancing the TinyUSDZ pin must re-check that
  the port still builds against the vcpkg `fast_float` revision lib3mf uses.
- TinyUSDZ has no allocation callback, so its in-process allocations stay outside the product ledger
  (bounded by the stream/archive caps and followed by T51 peak-commit measurement).
- Contained textures remain structurally recorded but undecoded; a future texture-sampling task must add a
  bounded decoder and revisit this ADR.