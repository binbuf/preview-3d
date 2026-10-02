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
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_