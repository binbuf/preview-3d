# FBX-008: Explorer FBX thumbnail adapter

Status: blocked on the general Gate 6 thumbnail-provider foundation  
Depends on: FBX-007 and a working bounded COM thumbnail provider  
Completes: original-MVP FBX family support

## Objective

Add bounded, stream-only FBX thumbnails to the isolated Explorer COM provider
after its format-neutral COM, sampler, CPU rasterizer, deadline, and surrogate
test infrastructure exists.

This task intentionally does **not** build the entire provider from the current
DLL stub. That is Gate 6 shared infrastructure needed by every format and must
land as its own work before this FBX adapter task begins.

## Context to load

- `design/05-thumbnail-provider.md`
- the completed thumbnail provider's adapter/sampler/raster interfaces
- the viewer `FbxAdapter` and FBX-001 results (policy reference, not code to
  link across the process boundary blindly)
- `thumbnail-provider/Preview3DThumbnailProvider.vcxproj`
- installer registration and actual-surrogate verification tests
- FBX corpus from FBX-007

## Work

1. Implement the fixed FBX CLSID
   `{FBC218D4-FD2C-41DF-B168-7F3B9E53C84E}` through
   `IInitializeWithStream`/`IThumbnailProvider` using the provider's bounded
   adapter interface. Do not sniff into another family or recover a path.
2. Enforce the provider limits: 256 MiB stream maximum, at most 128 MiB
   contiguous backing when required, 192 MiB parser/normalizer scratch,
   2 million triangles inspected, 250,000 rasterized samples, decoded texture
   limits, 750 ms target, and 2 s hard cutoff.
3. Configure the provider-local pinned `ufbx` copy with path/external-file,
   geometry-cache, plug-in, script, and environment-selected codec access
   disabled. Permit only stream-contained geometry and embedded allowlisted
   images. External textures produce neutral fallback; external required
   geometry safely returns the generic icon.
4. Apply the same deterministic static pose and supported skin/blend policy for
   fixtures small enough to evaluate within provider budgets. Preflight counts
   before evaluation and incorporate FBX-001's lack of an evaluation progress
   callback into a conservative deadline/size policy; never allow an unbounded
   library call merely because Shell uses a surrogate.
5. Feed finite evaluated triangles and material colors into the deterministic
   spatial/reservoir sampler and CPU rasterizer. Preserve large components,
   transformed bounds, fixed view, transparent background, and stable output;
   do not create a GPU device or communicate with the viewer/worker/cache.
6. Register the FBX CLSID/extension in the installer without
   `DisableProcessIsolation`, with non-clobber conflict/repair/uninstall rules.
   Add the dependency, license, SBOM, signing, and payload checks for the DLL's
   local `ufbx` copy.
7. Add COM/provider tests for binary/ASCII FBX, static pose, embedded texture,
   external dependency fallback, malformed/over-limit/deadline/OOM cases,
   golden sizes, repeated load/unload, leak counts, and actual Explorer
   surrogate hosting.

## Constraints

- The thumbnail DLL does not launch or IPC to the import worker/viewer and does
  not read the persistent cache.
- Shell surrogate isolation is crash containment, not an AppContainer security
  boundary; bounds, deadlines, fuzzing, and disabled path access remain
  mandatory.
- On any uncertain, over-budget, or failed parse, return the documented HRESULT
  with a null bitmap so Explorer uses its generic icon. Never cache a fabricated
  success image.

## Verification

- COM identity/routing selects the fixed FBX CLSID and returns deterministic
  perceptual goldens at 32, 64, 256, and 512 pixels.
- Malformed, hostile, oversized, non-seekable, timeout, and OOM cases do not
  crash/hang Explorer and leave no GDI/User/private-byte leak.
- The installed CLSID loads in the isolated thumbnail surrogate, not
  `explorer.exe`, and no registration sets `DisableProcessIsolation`.
- Clean install, handler conflict, repair, upgrade, rollback, and uninstall
  preserve unrelated handlers and user defaults.
- Viewer FBX behavior remains independent when thumbnail generation fails.

