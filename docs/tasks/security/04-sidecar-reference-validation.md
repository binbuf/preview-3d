# T04 — Sidecar reference validation (NUL/control, per-format)

## Goal
A model can no longer smuggle an embedded NUL (or other control character) through a sidecar
reference to defeat the extension allowlist, and dependency resolution is constrained to the
sidecar types the requesting format is allowed to need.

## Context (read first)
- `shared/import-broker/src/SidecarRequestServicer.cpp:17-26` — only length is checked; the raw
  worker bytes become `relativeReferenceUtf8`.
- `shared/import-broker/src/SidecarPathResolver.cpp:24-36` — `Utf8ToWide` preserves embedded NULs;
  `:364-398` rejects `:`/UNC/absolute/`..` but not NUL/control characters; `:396` tests
  `path.extension()`, so `"secret.pdf\0.png"` passes and `CreateFileW(candidate.c_str())` (`:83`)
  truncates at the NUL and opens `secret.pdf`.
- `shared/import-broker/src/SidecarPathResolver.cpp:44-56` — one global extension list regardless of
  the requesting format, though the host knows `request.format`.
- `docs/design/03-file-formats-and-ingestion.md` — per-format sidecar policy; `docs/design/09...md`
  — no-disclosure requirement.
- Tests: `tests/import-isolation/SidecarPathResolverTests.cpp` (security cases at ~`:369-383`),
  `SidecarProtocolTests.cpp`; corpus seeds `interactive-viewer/test-assets/corpus/sidecar-*.gltf`.

## Scope
- [ ] Reject NUL and all control characters (`< 0x20`, `0x7F`) in the reference before/after UTF-8
      decode, in the servicer and again in the resolver (two layers, matching project style).
- [ ] Reject references that are not valid UTF-8 (currently a failed decode returns empty and is
      rejected — make the failure explicit and tested).
- [ ] Make the allowlist per-format: pass the producer kind/format through and allow exactly the
      extension set that format needs (glTF: bin/images; OBJ: mtl/images; USD: layers; 3MF/USDZ
      entries handled in-archive, not as sidecars).
- [ ] Tests: embedded NUL before/after the real extension, C0 controls, DEL, invalid UTF-8, and a
      cross-format request (e.g. glTF asking for `.mtl`) that must be rejected.
- [ ] Add minimized corpus seeds for the NUL cases so the resolver stays covered.

## Out of scope
- Redesigning canonical containment or reparse handling (already sound).
- Archive-entry policy (→ SEC-05).

## Design notes
- Reject, do not sanitize: a reference with control characters is hostile, and silently trimming it
  changes which file is opened.
- Keep the two-layer check: the servicer can reject before any path work; the resolver remains the
  authority even if a future caller bypasses the servicer.
- If per-format policy needs a wire change, raise it in the control-protocol version review rather
  than overloading `reserved` fields.

## Done when
- [ ] `Tests.ImportIsolation` passes with the new cases, including a resolver unit case that would
      have opened a `.pdf` before the fix.
- [ ] Corpus seeds are committed and referenced from the seed list.
- [ ] Hand-off filled in.

## Hand-off

What landed:
- `SidecarRequestServicer.cpp` rejects any raw reference byte `< 0x20` or `0x7F` before any path
  work (first layer), and now takes the trusted `ImportFormat` and forwards it to the resolver.
- `SidecarPathResolver.cpp`: same raw-byte control check, then `Utf8ToWide` uses
  `MB_ERR_INVALID_CHARS` and returns `std::optional<std::wstring>` so invalid UTF-8 is an explicit
  `UnsafeReference` (not an empty-string substitute); the decoded text is checked for control
  characters too. `IsAllowedSidecarExtension(ImportFormat, path)` replaces the one global list:
  Gltf `.bin`+images, Obj `.mtl`+images, Fbx images, Usd `.usd/.usda/.usdc`+images, and
  Stl/Ply/ThreeMf/Step none. `FindAssetInUserRoot` is format-aware. `ImportSession.cpp` passes
  `request.format`.
- New tests: `SidecarPathResolverTests` embedded NUL (including the `secret.pdf\0.png` case that
  opened `secret.pdf` before the fix), C0 controls + DEL, invalid UTF-8, and a cross-format matrix
  (glTF→`.mtl`/`.usdc` rejected, OBJ→`.mtl` accepted/`.bin` rejected, USD→`.usdc` accepted/`.bin`
  rejected, STL→`*` rejected); `SidecarProtocolTests` servicer-layer NUL + invalid UTF-8.
- Corpus: `generate.py` emits `sidecar-nul-extension.gltf` (`approved.bin\0.png`) and
  `sidecar-nul-trailing.gltf` (`approved.bin\0`); `manifest.json`, `Expectations.h`, and
  `qualification-small.json` regenerated; both seeds added to the hostile list in
  `FixtureManifestTests.cpp`.
- Docs: ADR 0032; `docs/design/03-file-formats-and-ingestion.md` Input boundary now names the
  control-character/UTF-8 rejection and the per-format extension policy; `tests/fixtures/README.md`
  sidecar row updated.

Deviations:
- The per-format list is wider than the task's shorthand. The task said "USD: layers", but USD also
  brokers direct image assets (`materials.usda` → brokered `albedo.png`), and FBX brokers textures;
  restricting USD/FBX to layers/`.bin` broke existing FBX and USD tests. The implemented sets are
  Gltf `.bin`+images, Obj `.mtl`+images, Fbx images, Usd layers+images, others none, which matches
  the adapters' actual needs and design/03. ADR 0032 records this.
- No wire/protocol change was needed: the host already knows `request.format`, so the format is
  never taken from worker text. `ResolveSidecarPath`/`ServiceSidecarRequest` gained a trailing
  `ImportFormat format = ImportFormat::Gltf` default to keep existing callers compiling.
- Regenerating the corpus requires the Release `meshoptimizer.dll`, which is not present in this
  checkout; the manifest was regenerated by importing `generate.py` with `meshopt()` stubbed to copy
  the committed `meshopt.glb`. All pre-existing entries were verified byte-identical.

Checks run:
- Debug: `MSBuild tests\import-isolation\Tests.ImportIsolation.vcxproj /p:Configuration=Debug
  /p:Platform=x64 "/p:SolutionDir=D:\repos\binbuf\preview-3d\\"` → success. Full suite 392 cases /
  386 passed / 6 failed — exactly the documented pre-existing set (SidecarPathResolver
  user-asset-root ×3, ThreeMfSpike, OpenUsdHostSpike, UsdSpike). Confirmed pre-existing by
  `git stash` + rebuild (baseline 387 / 381 / 6).
- Release: same `/p:Configuration=Release` → success. Full suite 392 / 389 / 3 — the pre-existing
  resolver trio only.
- `npm test` (Tests.Unit Release, 350 cases) → green.
- `python tests/fixtures/verify.py interactive-viewer/test-assets/corpus` → passes (all manifest
  hashes, including the two new seeds).

Next task must know:
- `Tests.ImportIsolation` is not green at baseline; judge by "no new failures" (see PROGRESS T01–T04).
- Any future format that brokers a new dependency type must extend `IsAllowedSidecarExtension`.
- `.usdz` currently shares `ImportFormat::Usd`, so it would also allow layer sidecars if the adapter
  ever asked; harmless today (entries are in-archive). Noted as a T04 follow-up.