---
verify: x64\Release\Tests.Unit.exe
---
# T32 — Implement the 3MF thumbnail adapter

## Goal
Render the supported static `.3mf` preview subset in Explorer from a stream, including contained
colors/textures and a bounded Beam Lattice representation, under provider ceilings.

## Context (read first)
- `docs/design/adapters/3mf-thumbnail.md` — the "3MF-008 — Explorer thumbnail adapter" section.
- `docs/design/05-thumbnail-provider.md` — stream ingestion (3MF contained entries) and limits.
- `docs/design/03-file-formats-and-ingestion.md` — 3MF preflight/policy.
- `docs/legacy/design/` 3MF task history and `3MF-001-SPIKE-RESULTS.md` — lib3mf behavior notes.
- `docs/tasks/17-provider-test-harness.md`.

## Scope
- [ ] Route `.3mf` to this adapter via its fixed CLSID; read exclusively from the stream and launch no worker.
- [ ] Reuse the product OPC/ZIP preflight and required-extension policy under provider limits: 256 MiB stream, 128 MiB aggregate expansion, 100:1 ratio, 192 MiB accounted scratch and 384 MiB measured process-commit increase target. Record lib3mf allocations outside allocator accounting.
- [ ] Link the approved bounded 3MF reader (lib3mf) only into the thumbnail DLL; never the viewer.
- [ ] Sample the complete root build deterministically across build items and spatial regions; include a bounded representation of supported lattice geometry and standard materials/colors.
- [ ] Add fixtures and goldens: Core mesh, Production multi-part, Materials color/texture, bounded Beam Lattice, and unsupported-required-extension/over-budget/malformed cases.

## Out of scope
- Slicer-private plate metadata (not needed for a thumbnail).
- Viewer 3MF qualification (3MF-007, viewer program).

## Design notes
- Unsupported required extensions or over-budget content fall back to the generic icon; never render a partial required scene as success.
- Contained PNG/JPEG attachments only; use the shared bounded image decoders.
- Keep lib3mf compatible-mode usage behind the product OPC/XML boundary as in the viewer adapter.

## Done when
- [ ] `x64\Release\Tests.Unit.exe` (the `verify:` command) passes in Debug and Release for supported and fallback cases.
- [ ] The DLL's dependency closure includes lib3mf's runtime but no viewer/worker binary.
- [ ] The family is smoke-checked in Explorer using the T22 procedure.
- [ ] Hand-off below filled in.

## Hand-off
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_
