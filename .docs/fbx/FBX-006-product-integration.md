# FBX-006: viewer, activation, and installer integration

Status: blocked on FBX-005  
Depends on: FBX-003 through FBX-005  
Unblocks: FBX-007

## Objective

Expose the completed FBX adapter through every viewer activation route and the
current NSIS/Open With integration, without claiming Explorer thumbnails.

## Context to load

- `interactive-viewer/src/app/{Preview3D,ActiveInstance,D3D12ImportBridge}.{h,cpp}`
- `interactive-viewer/src/platform/{ShellIntegration,OpenWithCache}.{h,cpp}`
- `interactive-viewer/src/ui/InfoPanel.cpp`
- `tests/unit/ActiveInstanceTests.cpp`
- `tests/app-smoke/{activation,recovery,metadata,progressive}.py`
- `packaging/installer/Preview3D.nsi`
- `packaging/installer/Reset-Preview3DTestAssociations.ps1`
- `packaging/{installer/INSTALLER-README.txt,portable/PORTABLE-README.txt}`
- root and interactive-viewer READMEs

## Work

1. Route `.fbx` case-insensitively to `ImportFormat::Fbx` in the D3D12 import
   bridge. Add the correct source-format label, stages, typed user errors, and
   redacted Copy details. Preserve prior content on failure/cancel.
2. Enable command-line paths, secondary activation, `IFileOpenDialog`, the
   existing one-file drag/drop path, retry/open-another, and relevant help/About
   text. Update all closed
   extension lists together so no surface disagrees.
3. Consume node/instance/deformed mesh/material results in the existing
   progressive upload/render path. Keep input/present responsive, publish only
   validated fence-complete resources, frame verified transformed bounds, and
   show accurate mesh/node/material/triangle/animation/skin/bone metadata.
4. Add `.fbx` to the current ShellIntegration supported-extension set and
   bounded Open With cache behavior. This is application integration only, not
   a thumbnail handler.
5. Add a dedicated FBX ProgID and capabilities/OpenWith registration to NSIS,
   with matching uninstall and test-association cleanup. Do not take the user's
   current default. Verify adversarial path quoting and association refresh.
6. Update portable/installer manifests, support limits, notices, SBOM inputs,
   and README claims. The existing `ufbx` notice must describe FBX and OBJ use.
   Keep explicit text that Explorer thumbnails remain deferred.
7. Add app/unit tests for uppercase extension, all activation surfaces,
   unsupported/malformed/recoverable errors, cancel/replace, metadata, static
   posed rendering, and a later valid reopen. Replace old tests that expect FBX
   rejection with acceptance plus a distinct unsupported-format fixture.

## Constraints

- Do not expose `.fbx` until the worker path from FBX-005 is present and green.
- Do not register the fixed FBX thumbnail CLSID or imply thumbnail support; the
  provider remains a separate Gate 6 deliverable.
- Installer changes follow current post-MVP NSIS policy and must not silently
  overwrite protected defaults or unrelated handlers.

## Verification

- Direct path, dialog, drop, and secondary activation open binary and ASCII FBX;
  cancel/failure never poisons a subsequent open.
- Static posed geometry, instances, materials, textures, bounds, and metadata
  appear correctly while UI/render heartbeat gates remain satisfied.
- Debug and Release solutions, Unit, ImportIsolation, and applicable app-smoke
  suites pass.
- Unsigned engineering portable and NSIS targets build, include the required
  worker dependency/license closure, and have no unresolved non-system import.
- Install/uninstall tests show `.fbx` in Open With/Default Apps without changing
  the user's selected default and without registering a thumbnail handler.
