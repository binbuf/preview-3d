# 0022 — OBJ adapter: provider-local ufbx with external access disabled

## Status
accepted

## Context
T24 must render Wavefront `.obj` geometry from the Shell's `IInitializeWithStream` stream. The
format document assigns OBJ to ufbx, and the FBX-008 design requires the provider-local pinned ufbx
copy to have path, geometry-cache, plug-in, script and environment-codec access disabled. The
provider has no filesystem location and no sidecar capability, so `mtllib` and every texture
reference must be ignored rather than resolved, and an OBJ that references a missing `.mtl` must
still render its geometry.

The provider already consumes product-owned source and the frozen adapter contracts, but it did not
consume any vcpkg library; the worker and both test executables do.

## Decision
1. The provider enables the root vcpkg manifest (`VcpkgEnableManifest=true`, static
   `x64-windows-static-md` triplet) and links ufbx the same way the worker and test executables
   already do. The alternative — hand-linking a triplet/configuration-specific `ufbx.lib` path — was
   rejected because it duplicates vcpkg's path resolution and is the pattern used only by the
   separate OCCT STEP host.
2. `ObjAdapter` forces `UFBX_FILE_FORMAT_OBJ`, disables content/extension format detection, sets
   `load_external_files = false` and `ignore_missing_external_files = true`, and installs a deny
   `open_file_cb` as a second line of defence. ufbx's `temp_allocator`/`result_allocator`
   `memory_limit` caps are each half of `kAccountedScratchMaxBytes`.
3. `mtllib`/`usemtl` and textures are never consulted: the adapter emits the neutral material
   (index 1), with a white base when the geometry carries vertex colors.
4. OBJ text is read once into the frozen contiguous view, or a checked ledger-charged backing buffer
   bounded by `kContiguousBackingMaxBytes`; the ufbx `progress_cb` and the geometry loop poll the
   per-call deadline.

## Consequences
- The provider DLL stays import-clean (no viewer/worker/host/core imports; ufbx is static) and the
  security boundary is unchanged: ufbx never sees a path and cannot open one.
- The provider build now restores the root vcpkg manifest even when built alone. Other provider
  foundation tasks are unaffected.
- T31 (FBX) reuses the same ufbx linkage and deny-external-file policy; it adds FBX-specific
  evaluation and embedded-texture handling on top.
- An OBJ whose geometry depends on an external file that the format would otherwise require renders
  neutral geometry rather than failing; only genuinely malformed/empty geometry fails.