---
verify: x64\Release\Tests.ImportIsolation.exe
---
# T06a — SEC-03b Fix user-chosen asset-root canonicalization

## Goal
The "locate missing assets" user-chosen asset-root fallback actually resolves files again, and
`Tests.ImportIsolation`'s asset-root cases are green, so this suite can gate the broker/worker tasks
after it.

## Context (read first)
- `shared/import-broker/src/SidecarPathResolver.cpp` — the user-root branch calls
  `AcceptCandidate(match, DirectoryPrefix(rootPath), ...)`. `DirectoryPrefix` lowercases the *raw*
  root path (e.g. `C:\Users\...\Temp\p3d1234\`), while `AcceptCandidate` canonicalizes the opened
  candidate with `GetFinalPathNameByHandleW(FILE_NAME_NORMALIZED)`, which returns a `\\?\C:\...`
  form. `sidecarCanonicalPath.compare(0, prefix.size(), prefix)` therefore never matches (audit
  finding F-11), so `resolved.file` is always false for a normal-path root.
- `additionalSearchRoots` come from the trusted UI as ordinary paths
  (`interactive-viewer/src/app/Preview3D.cpp`, missing-asset warning -> user picks a folder).
- Failing cases today: `tests/import-isolation/SidecarPathResolverTests.cpp:574-624`
  (`[sidecar-resolver][asset-root]`), assertions at `:593`, `:608`, `:622`. PROGRESS T03/T04 already
  recorded them as pre-existing.
- `shared/import-broker/include/import_broker/SidecarPathResolver.h` documents the contained-root
  contract this must keep.

## Scope
- [ ] Make the root comparison canonical on both sides: canonicalize each additional root by opening
      it (handle -> `GetFinalPathNameByHandleW(FILE_NAME_NORMALIZED)`) and derive the prefix from that,
      or normalize both sides to the same canonical form before comparing. Do not weaken the
      trailing-separator containment check.
- [ ] Preserve fail-closed behavior: a root that cannot be canonicalized, is a reparse point escaping
      its own tree, or has a sibling-prefix name must still reject.
- [ ] Cover the cases: normal-path root, `\\?\`-prefixed root, trailing separator present/absent,
      case difference, and the existing "still bounded to its own directory tree" security case.
- [ ] Confirm the whole `Tests.ImportIsolation` suite is green (no remaining pre-existing failures)
      so it is safe to use as a verify gate.

## Out of scope
- Per-format sidecar extension policy (→ T04) and control-character rejection (→ T04).
- Redesigning the asset-root UX.

## Design notes
- Keep the change in the broker resolver; the worker still never sees a path.
- Canonicalizing the root is the same technique `SourceFileAccess`/`AcceptCandidate` already use for
  the primary file; reuse it rather than inventing a string normalization.
- If canonicalization of a root fails, skip that root (current fail-closed behavior), not fall back
  to the raw path.

## Done when
- [ ] `x64\Release\Tests.ImportIsolation.exe` passes with the asset-root cases green.
- [ ] `SidecarPathResolverTests.cpp:574-624` assertions pass in Debug and Release.
- [ ] Hand-off notes the canonicalization approach and any behavior change.

## Hand-off
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_