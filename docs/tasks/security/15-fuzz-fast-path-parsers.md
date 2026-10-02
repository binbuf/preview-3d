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
Changed:
- `tests/fuzz/StlFuzz.cpp`, `tests/fuzz/StlFuzz.vcxproj` — drives the real
  `import_worker::ImportStl` (ASCII/binary detection, binary facet scan, ASCII
  `AsciiTokenizer` walk, facet normalization, bounded writer) plus a direct
  `parser_core::StlParserCore` domain.
- `tests/fuzz/PlyFuzz.cpp`, `tests/fuzz/PlyFuzz.vcxproj` — drives the real
  `import_worker::ImportPly` (binary LE/BE and ASCII paths, header/scalar/list
  walks, polygon fan triangulation, writer) plus a direct
  `parser_core::PlyParserCore` domain.
- `tests/fuzz/ObjFuzz.cpp`, `tests/fuzz/ObjFuzz.vcxproj` — drives the pinned
  `ufbx` OBJ/MTL parser (OBJ primary with a single in-memory virtual MTL
  sidecar, and MTL-as-primary) with a bounded normalized scene/material walk.
- `tests/fuzz/prepare_{stl,ply,obj}_seeds.py` — deterministic generated seeds
  that refuse a non-empty output directory. 12 STL / 12 PLY / 9 OBJ+MTL seeds.
- `tests/fuzz/README.md` — STL, PLY, OBJ/MTL sections (build + run commands,
  caps, what is not instrumented).
- `.github/workflows/ci.yml` — added STL/PLY/OBJ entries to the `fuzz-smoke`
  matrix (one target per runner, matching the existing STEP/3MF shape).
- `docs/design/adr/0044-fuzz-fast-path-parser-boundaries.md` (new);
  `docs/design/09-quality-performance-and-security.md` (target names);
  `docs/security/PROGRESS.md` (T15 facts).

Deviations:
- `ObjFuzz` drives the pinned `ufbx` library and its OBJ/MTL options/callbacks
  directly rather than linking `ObjAdapter.cpp`. This matches the design doc's
  "OBJ/MTL and FBX adapter options/callbacks" wording and the `FbxFuzz`
  precedent, and avoids pulling SEC-16 compressed-image decoders into this
  target. The adapter's wire-emission/image stages stay covered by the real
  provider/worker lanes. The in-memory sidecar also gives MTL tokenization
  deeper coverage than the adapter could (its MTL path requires a mapped-file
  `MappingLease`, which cannot be synthesized from memory).
- `StlFuzz`/`PlyFuzz` link the real adapters directly; because the adapters
  reference `ChunkBatchSink::PublishBatch` even with a null sink, the projects
  also compile `ChunkBatchSink.cpp` and `ControlChannelIo.cpp` and build with
  no vcpkg dependency. `ObjFuzz` uses the root vcpkg manifest (header-only
  `ufbx`).

Check results:
- Builds: `StlFuzz.vcxproj`, `PlyFuzz.vcxproj`, `ObjFuzz.vcxproj` all build
  Release x64 with ASan + libFuzzer via
  `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe`
  and `/p:SolutionDir=<root>\`; `ObjFuzz` additionally with
  `/p:VcpkgRoot=...\VC\vcpkg`. Exes land in `tests\fuzz\x64\Release\`.
- Bounded runs (fresh seeds under `TestResults/security-t15/`, 30 s each, exit
  0, no ASan finding):
  - StlFuzz: 41,338 units, 1,333 exec/s, 808 new units, peak RSS 335 MB.
  - PlyFuzz: 28,875 units, 931 exec/s, 741 new units, peak RSS 339 MB.
  - ObjFuzz: 212,803 units, 6,864 exec/s, 627 new units, peak RSS 403 MB.
  All with `-max_total_time=30 -timeout=5 -rss_limit_mb=1024 -max_len=2097160`.
- Seed preparers refuse a non-empty directory (exit 1) — verified.
- `x64\Release\Tests.Unit.exe "~[graphics]"` — 307 cases / 131,544 assertions
  green (no production code changed).

Next task must know:
- SEC-16 extends the same `fuzz-smoke` matrix and then promotes it into the
  required gate. Keep one target per runner: the fuzz projects share
  `tests\fuzz\x64\Release` as an intermediate directory (MSB8028).
- A finding should be minimized into the relevant `prepare_*_seeds.py` output
  as a new immutable seed and committed with the fix.