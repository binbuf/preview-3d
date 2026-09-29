# T42 — Package, sign, and audit the provider payload closure

## Goal
Ship the provider with an auditable, signed payload: the DLL plus every family runtime it needs,
covered by a closed PE-dependency allowlist, notices, SBOM, signing and a per-file hash manifest.

## Context (read first)
- `docs/design/08-installation-and-registration.md` — dependency/registration ownership.
- `docs/design/02-system-architecture.md` — which libraries may appear in the thumbnail closure.
- `docs/design/adr/0002` (OCCT), `0003` (decoders), `0004` (source sharing) — what the DLL links.
- `packaging/installer/Preview3D.nsi`, `packaging/installer/Create-Installer.ps1`, `packaging/portable/Create-PortableRelease.ps1`, `packaging/portable/THIRD-PARTY-NOTICES.txt` — the existing packaging scripts to extend.

## Scope
- [ ] Stage the provider DLL and every non-system runtime it resolves (e.g. ufbx/lib3mf/zlib/TinyUSDZ/Draco/KTX2/libwebp/meshopt and, for STEP, the constrained OCCT modules) in the installer payload.
- [ ] Extend the PE-dependency closure allowlist to include exactly those modules (plus the known Windows system DLLs) and fail staging on any unresolved or unexpected non-system import.
- [ ] Add the provider's dependency notices and SBOM entries.
- [ ] Wire Authenticode signing for the DLL and a per-file SHA-256 manifest into the release procedure.
- [ ] Prove the DLL imports no viewer/worker/host binary and that the viewer/worker/hosts import no provider-only library.

## Out of scope
- Registration keys (→ T41).
- The portable ZIP scope decision (ADR-016): the provider is an installed feature and remains excluded from the portable archive. A later scope change requires an ADR.
- The full MSI/SBOM gate 7 (viewer program).

## Design notes
- vcpkg autolink places the whole triplet's `lib\*.lib` on the link line; verify the real closure with `dumpbin`, not the manifest.
- Keep the payload allowlist closed (explicit list), never "everything in `$(OutDir)`".
- No signing certificate may be available locally; record the unsigned-engineering state honestly if so.

## Done when
- [ ] The installer stages the provider payload and the closure check passes with no unresolved non-system import.
- [ ] Notices, SBOM entries and the hash manifest include every provider binary.
- [ ] Commands and results are recorded in Hand-off.
- [ ] Hand-off below filled in.

## Hand-off
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_
