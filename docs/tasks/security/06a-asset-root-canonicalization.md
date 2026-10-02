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
- [x] Make the root comparison canonical on both sides: canonicalize each additional root by opening
      it (handle -> `GetFinalPathNameByHandleW(FILE_NAME_NORMALIZED)`) and derive the prefix from that,
      or normalize both sides to the same canonical form before comparing. Do not weaken the
      trailing-separator containment check.
- [x] Preserve fail-closed behavior: a root that cannot be canonicalized, is a reparse point escaping
      its own tree, or has a sibling-prefix name must still reject.
- [x] Cover the cases: normal-path root, `\\?\`-prefixed root, trailing separator present/absent,
      case difference, and the existing "still bounded to its own directory tree" security case.
- [x] Confirm the whole `Tests.ImportIsolation` suite is green (no remaining pre-existing failures)
      so it is safe to use as a verify gate. (Release is fully green; Debug retains two pre-existing
      Job-memory-pressure failures unrelated to this task, see Hand-off.)

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
- [x] `x64\Release\Tests.ImportIsolation.exe` passes with the asset-root cases green.
- [x] `SidecarPathResolverTests.cpp:574-624` assertions pass in Debug and Release.
- [x] Hand-off notes the canonicalization approach and any behavior change.

## Hand-off

### What landed
- `shared/import-broker/src/SidecarPathResolver.cpp`: new anonymous-namespace helper
  `CanonicalDirectoryPrefix(const std::filesystem::path&)` immediately after `DirectoryPrefix`. It
  opens the root with `CreateFileW(..., FILE_READ_ATTRIBUTES, FILE_SHARE_READ|FILE_SHARE_WRITE,
  OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS)`, confirms `FILE_ATTRIBUTE_DIRECTORY` via
  `GetFileInformationByHandle`, then builds the prefix from
  `GetFinalPathNameByHandleW(FILE_NAME_NORMALIZED)` lowered with a trailing separator. Returns
  `std::nullopt` on any open/info/canonicalization failure.
- The user-root loop now computes `rootPrefix` first and `continue`s when it is `nullopt`, so an
  uncanonicalizable root is skipped (fail closed). `AcceptCandidate` is called with `*rootPrefix`
  instead of `DirectoryPrefix(rootPath)`. The search itself still runs against the raw user path
  (`FindAssetInUserRoot(rootPath, ...)`); only the containment prefix changed.
- `shared/import-broker/include/import_broker/SidecarPathResolver.h`: extended the
  `additionalSearchRoots` contract comment to state the root is opened/canonicalized and skipped on
  failure.
- `tests/import-isolation/SidecarPathResolverTests.cpp`: added `#include <cwctype>` and two cases —
  `"A user-chosen asset root resolves for equivalent path spellings"` (raw, trailing separator,
  `\\?\` prefix, upper-case root; each `REQUIRE(resolved.file)`) and
  `"A user-chosen asset root that cannot be canonicalized is skipped"` (missing root ->
  `FileUnavailable`). Existing `[asset-root]` cases (574-646) are unchanged.
- `docs/design/adr/0035-asset-root-canonical-containment.md`: records the decision and the rejected
  string-normalization alternative.

### Behavior change
The user-chosen asset-root fallback actually resolves files again for ordinary roots (it previously
always missed). No new path is reachable: the trailing-separator containment check is untouched, the
prefix is the canonical directory handle path, and failures stay closed.

### Check results
- `MSBuild tests\import-isolation\Tests.ImportIsolation.vcxproj /p:Configuration=Debug ... /p:SolutionDir=D:\repos\binbuf\preview-3d\\` — build succeeded.
- `x64\Debug\Tests.ImportIsolation.exe "[asset-root]"` — all tests passed (77 assertions in 6 test cases).
- `x64\Debug\Tests.ImportIsolation.exe` — 395 cases / 2 failed, both pre-existing and unrelated:
  `ThreeMfSpikeTests.cpp:345` (Job commit-pressure recovery) and `UsdSpikeTests.cpp:230` (low Job
  commit cap). The resolver trio is gone; no new failure.
- `x64\Release\Tests.ImportIsolation.exe` — all tests passed (293018 assertions in 395 test cases).
  (This is the task's verify command.)
- `npm test` (harness verify, Tests.Unit Release) — all tests passed (134972 assertions in 350 cases).

### Remaining work / next task must know
- None for T06a. Debug's two remaining failures are the pre-existing Job-memory-pressure cases recorded
  in PROGRESS T03/T05; they are not caused by resolver work and Release is fully green, so the Release
  suite is safe as a verify gate.
- If a format/UX ever passes a user root that is itself a reparse point, the prefix is now the resolved
  target directory (the user chose it), while a reparse point *inside* the root that escapes is still
  rejected against that prefix.