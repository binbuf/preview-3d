# 0053 — Classify the primary source path before any open

## Status
accepted

## Context
A follow-up audit of the completed security set found the primary-file guards still classified the raw
text with a bare prefix test. `interactive-viewer/src/app/Preview3D.cpp` rejected only paths starting
`\\` unless they started `\\?\`, and `shared/import-broker/src/SourceFileAccess.cpp` had the same
shape. A forward-slash UNC (`//server/share/model.glb`) satisfied neither the `\\` prefix nor the
`\\.\` device test, so it reached `CreateFileW` and made the trusted process initiate an SMB
connection that the *post-open* canonical check (`\\?\UNC\`) then refused; the viewer also admitted
`\\?\UNC\server\...` for the same reason. Separately, `OpenAndCanonicalizeSourceFile` accepted a
relative primary (`foo\bar`), which resolved against the process CWD, unlike `SidecarPathResolver`,
which already requires its references to be relative-but-contained and rejects UNC/device/rooted text
(ADR-0032). Reject-after-open is too late: the network/device I/O and the credentials/name disclosure
that come with it have already happened.

## Decision
- Add a header-only `platform::ClassifySourcePath` (`shared/platform/include/platform/SourcePathPolicy.h`)
  shared by the viewer guard and the broker. It returns `LocalAbsolute`, `RemoteOrDevice`, or
  `Relative`; it normalizes `/` to `\` on a **copy** before classifying, because a forward slash is a
  valid Win32 separator and `//server/share` must be read as a UNC, not a relative path.
- `RemoteOrDevice` covers any leading double separator that is not the extended drive form:
  `\\server\share`, `//server/share`, `\\.\PhysicalDrive0`, `\\?\UNC\...`, `\\?\GLOBALROOT\...`,
  `\\?\Volume{...}\`. The extended drive form `\\?\X:\...` stays accepted.
- The broker requires `LocalAbsolute` and keeps its existing ADS/extra-colon check and its
  `GetDriveTypeW(...) == DRIVE_REMOTE` probe for a mapped network drive; a `Relative` primary is now
  rejected with `UnsafeReference` instead of being resolved against the CWD.
- The viewer's `BeginOpen` rejects anything that is not `LocalAbsolute` before it spawns the import,
  so a crafted path never reaches an import worker or the broker.
- Reject, do not sanitize: the original text is opened unchanged, and the post-open canonical
  containment check is retained as defense in depth.

## Consequences
- A remote or device primary path cannot cause an SMB/device open from the trusted process; a
  forward-slash UNC, an extended UNC, and a relative primary are all refused up front.
- `tests/import-isolation/SourceFileAccessTests.cpp` covers the remote/device forms, a relative
  primary, and a forward-slash absolute local path that must still open; `tests/unit/SafeFileOpsTests.cpp`
  covers the same classification for the viewer guard (`[security]`).
- The viewer reuses the existing `failure.remotePath` string for both the remote and the (practically
  unreachable, shell never supplies one) relative rejection, so no new localization key is introduced.