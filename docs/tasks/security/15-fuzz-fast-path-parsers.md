---
verify: x64\Release\Tests.Unit.exe "~[graphics]"
---

# T15 — Fuzz targets: STL, PLY, OBJ

## Goal
The three product-owned fast-path parsers have ASan/libFuzzer targets seeded from real fixtures, so
manual bounds/stride/tokenization bugs become minimized, permanent regression cases.

## Context (read first)
- `tests/fuzz/README.md` and the four existing projects (`StepFuzz`, `ThreeMfFuzz`, `UsdFuzz`,
  `FbxFuzz`) define the conventions: standalone libFuzzer + ASan (`/fsanitize=fuzzer`,
  `EnableASAN=true`), `LLVMFuzzerTestOneInput`, immutable seeds with a preparer script that refuses
  a non-empty output directory, no GPU/window/filesystem.
- Parser code to drive: `shared/parser-core/src/StlParserCore.cpp`, `PlyParserCore.cpp`,
  `AsciiTokenizer.cpp`; `import-worker/src/StlAdapter.cpp`, `PlyAdapter.cpp`, `ObjAdapter.cpp`
  (OBJ/MTL goes through ufbx; drive the adapter with a bounded virtual sidecar map).
- `SECURITY.md:39-40` and `docs/design/09-quality-performance-and-security.md:177-192` list these
  boundaries as in scope and promise targets.
- Fixtures are generated, not committed (`tests/fixtures/README.md`); seed preparers must generate
  or read small bounded samples.

## Scope
- [ ] Add `StlFuzz` covering ASCII/binary detection, tokenization, facet/index normalization, and
      the adapter's bounded scan path.
- [ ] Add `PlyFuzz` covering header parsing, endianness, scalar/list properties, skips, and
      point/mesh normalization (ASCII and binary).
- [ ] Add `ObjFuzz` driving the OBJ/MTL adapter with an in-memory sidecar map (deny/allow hooks as
      the adapter expects); cover MTL tokenization, missing sidecars, and hostile references.
- [ ] Add a seed preparer per target following the existing script pattern; minimize and commit any
      finding as an immutable seed.
- [ ] Document each target in `tests/fuzz/README.md` (build + run commands, caps, what is not
      instrumented) and record a bounded smoke run in the Hand-off.
- [ ] Wire the new targets into the CI fuzz-smoke step from SEC-13 (or leave a precise hook the
      SEC-13 owner can call).

## Out of scope
- glTF and compressed codecs (→ SEC-16); provider adapters (→ SEC-17).
- Fixing findings: minimize, add as regression fixtures, and file a fix task (unless the fix is
  small enough to land here).

## Design notes
- Keep each target's input cap small (1–2 MiB) as the existing targets do, so an in-process run is
  bounded without the AppContainer/Job.
- Do not add the targets to `Preview3D.slnx`; they are explicitly built sanitizer targets.
- Reuse the shared parser code directly where possible to maximize coverage per engine cycle.

## Done when
- [ ] Each target builds with ASan and completes a bounded run (`-max_total_time`, `-rss_limit_mb`)
      against its seeds with no findings.
- [ ] `tests/fuzz/README.md` documents all three.
- [ ] Hand-off lists seeds added, runtimes, and any findings filed.

## Hand-off
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_