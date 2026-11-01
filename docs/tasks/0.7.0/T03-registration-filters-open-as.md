---
timeoutMin: 240
verify: x64\Release\Tests.Unit.exe "[open-as],[format-installer]"
---
# T03 — Catalog-driven registration, filters and Open As

## Goal
The format catalog from T02 becomes the single source for the open-dialog
filter, single-instance acceptance, Shell/Open With discovery and installer
associations, and the user can override a failed detection with **Open As** or
`--format`. A valid model with a wrong or missing suffix becomes openable
instead of producing an extension error.

## Context (read first)
- `docs/proposal.md` §3.1 — Open As and generated metadata.
- `interactive-viewer/src/app/D3D12ImportBridge.cpp:425-441` — classifier from
  T02; this task consumes its candidate/reason result.
- `interactive-viewer/src/app/Preview3D.cpp:5633-5690` — `CommandLineToArgvW`
  parsing for developer/test switches.
- `interactive-viewer/src/app/Preview3D.cpp:2225-2238` — hard-coded open-dialog
  filter strings.
- `interactive-viewer/src/app/ActiveInstance.cpp:323-326` — hard-coded
  accepted-extension list.
- `interactive-viewer/src/platform/ShellIntegration.cpp:40-43` — hard-coded
  Open With extension array.
- `packaging/installer/Preview3D.nsi` — `RegisterExtension` calls and ProgIDs.
- `docs/design/08-installation-and-registration.md` — association rules.

## Scope
- [x] Add `--format <stable-name>`, `--skip-content-check` and
  `--list-formats` to the command-line parser. `--format` is an explicit
  importer choice; `--skip-content-check` trusts the extension only for
  candidate selection and still goes through the normal worker validation.
- [x] Add an **Open As** action to the failure/open flow: a small list of
  catalog formats (stable name + display name), localized. Selecting one
  re-runs the import with the forced format and reports the detected format in
  the info panel/diagnostics.
- [x] Build the open-dialog filter and the “Supported 3D models” string from the
  catalog. Do not hand-maintain the list.
- [x] Use the catalog for `ActiveInstance` acceptance and
  `ShellIntegration`'s extension array. Keep behavior identical for the
  existing extensions.
- [x] Update installer registration without duplicating the list:
  - either generate a small `.nsh` include from the catalog at build time, or
  - keep the explicit NSIS calls and add a test that reads `Preview3D.nsi` and
    asserts every catalog extension has a `RegisterExtension` and the expected
    ProgID.
  A generated include is preferred if it can be done without a new build tool.
- [x] Add a test `[format-installer]` that fails when the catalog and installer
  disagree, including the uninstall section.
- [x] Add `Tests.Unit` cases for `--format` parsing, Open As selection and
  extension/content mismatch reporting. Keep the test independent of the GPU.
- [x] Update `README.md` and `docs/FORMAT-SUPPORT.md` only if the existing
  format list text changes; new formats are not part of this task.

## Out of scope
- Adding IGES/BREP/XBF/FCStd/DAE/3DS/OFF/AMF (→ T08–T10).
- Changing the association *defaults* or overwriting the user's choices; the
  installer remains non-clobbering.
- Thumbnail registration for new formats (later provider work).
- A full “recent files” or drag-and-drop redesign.

## Design notes
- Open As is a **user override**, not a silent sniffing fallback. If content
  and extension disagree, show both candidates and let the user choose; do not
  silently pick one when the content match is weak.
- A forced format must still be validated by the worker/host. It cannot bypass
  budgets, path policy or the wire validator.
- `--list-formats` output should be stable, machine-readable-ish text (one
  `stable-name<TAB>extensions<TAB>display-name` per line) so docs/tests can use
  it.
- Localize display names; stable names are ASCII and never localized.
- Keep the existing extension labels in `SourceFormatLabel` for diagnostics.
- The installer test should be cheap and run from the repo root; do not invoke
  `makensis` in `verify`.

## Done when
- [x] `x64\Release\Tests.Unit.exe "[open-as],[format-installer]"` passes.
- [x] A GLB renamed `.bin` opens through Open As with the glTF importer, and a
  deliberately mis-suffixed file can still be rejected if the user chooses the
  wrong format.
- [x] `Preview3D.exe --list-formats` lists the catalog and matches the installer
  test.
- [x] `Preview3D.exe --format gltf <valid-glb>` opens without relying on the
  suffix; `--format step <glb>` fails typed rather than crashing.
- [x] The open dialog and single-instance path still accept all existing
  formats.
- [x] Hand-off lists any catalog/installer drift found and the exact generated
  or checked artifact.

## Hand-off
_(filled in by the implementing session: CLI grammar, Open As UI entry point,
installer generation/check approach, and the extension/content mismatch policy
chosen.)_

- **CLI grammar**: `--format <stable-name>` (also `--format=<name>`) accepts a
  case-insensitive catalog stable name and only formats with a shipped importer;
  `--skip-content-check` selects by suffix only; `--list-formats` writes
  `stable-name<TAB>extensions<TAB>display-name` per directly registered catalog
  entry, prints to stdout/`CONOUT$`, and exits 0 before window or single-instance
  setup. `--format` is carried into the first open and into Retry/Open As;
  `--skip-content-check` remains process-wide.
- **Open As UI**: the failure card gained an `Open as…` button
  (`ID_VIEW_OPEN_AS`, accessibility `Control::ErrorOpenAs`). Its menu lists the
  detected and suffix candidates first, then every importable catalog format as
  localized display name + stable name. Choosing one re-runs the import with a
  forced descriptor. The failure path reports the detected candidate and the
  successful forced open appends `Opened as X. Detected Y.` to the warning and
  diagnostics.
- **Installer approach**: explicit NSIS calls are retained (no new build tool).
  `[format-installer]` reads `packaging/installer/Preview3D.nsi`, parses the
  `PROGID_*` defines, and asserts the exact `RegisterExtension`,
  `UnregisterExtension`, `RegisterProgId` and `DeleteRegKey` lines for every
  catalog extension, in both directions, including the uninstall section.
  No catalog/installer drift was found.
- **Mismatch policy**: a known suffix keeps the existing extension-only open
  path (behaviour for existing formats is unchanged). Content-only evidence or a
  cross-family mismatch never opens silently; it produces the typed
  "choose a format" failure and exposes both candidates through Open As. A
  forced format still passes the worker/host's budgets, path policy and wire
  validator; `--format step <glb>` therefore fails typed.
- **Checks run**:
  - `x64\Release\Tests.Unit.exe "[open-as],[format-installer]"` — 179
    assertions in 6 cases, pass.
  - `x64\Release\Tests.ImportIsolation.exe "[open-as]"` — 7 assertions,
    pass (real `tri_tight.glb` renamed `.bin` opens as Glb; forced Step fails
    typed).
  - `Preview3D.slnx /t:Preview3D` Release builds.
  - `Preview3D.exe --list-formats` (stdout redirected) — 12 catalog lines;
    matches the installer set.
- **Docs**: `docs/design/03-file-formats-and-ingestion.md`,
  `docs/design/08-installation-and-registration.md`, and
  `docs/design/adr/0058-catalog-driven-open-as-installer-drift.md`.
