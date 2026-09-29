# Thumbnail provider program overview

## Goal

Deliver the original-MVP **Explorer thumbnail provider**: a single x64 COM DLL that Windows Explorer
loads, out of process, to render a model-derived thumbnail for any supported direct format. The
provider is stream-only, has no GPU device, makes no network or path access, launches no process, and
fails safely to Explorer's generic icon on anything corrupt, unsupported, sidecar-dependent or over
budget.

This program restores the installed original-MVP scope ([ADR-0007](adr/0007-provider-program-scope-and-installer.md)):
the provider, its family runtimes, and its machine-level COM/ShellEx registration ship through the
project's per-machine NSIS installer, and the eventual WiX/MSI adopts the same component identities
and rules. The scope-limited portable ZIP (ADR-016) does not include the provider and does not gain
thumbnails; the general import worker, OpenUSD compatibility host, and STEP host already ship in the
NSIS installer.

This program covers the whole capability, not one family:

- the shared COM/provider foundation (classes, stream backing, adapter routing, sampler, CPU
  rasterizer, deadline and containment);
- every direct family in FR-01 — `.glb`/`.gltf`, `.stl`, `.ply`, `.obj`, `.fbx`, `.3mf`, `.usd`/
  `.usda`/`.usdc`/`.usdz`, and `.step`/`.stp`;
- Shell registration, packaging/signing, adversarial hardening and release qualification.

## Why this program starts with prerequisites

The provider was specified but never built: `thumbnail-provider/dllmain.cpp` is a 21-line stub. Three
things must be settled before adapter work is safe to schedule, and each is an early task here:

1. **Specification conflicts.** The baseline says "seven CLSIDs" and its roster omits STEP, yet
   `.step`/`.stp` is a direct format. It also says OCCT is "never the … thumbnail provider" while the
   STEP plan requires linking OCCT into the DLL, and it disagrees with itself on whether the provider
   decodes Draco/KTX2/WebP/meshopt. See ADR-0001, ADR-0002 and ADR-0003.
2. **Unproven feasibility.** Validation **Spike 8** (`.docs` baseline, now archived) requires
   benchmarking the mesh/point CPU rasterizer inside the *actual* thumbnail surrogate and confirming
   out-of-process load before Gate 6. No prototype exists. See tasks T02/T03.
3. **No shared foundation plan.** Gate 6 was six bullets; the shared DLL is the largest and riskiest
   piece. This document and the roadmap chunk it explicitly.

## Target family roster (eight CLSIDs)

| Family | Extensions | CLSID |
| --- | --- | --- |
| glTF | `.glb`, `.gltf` | `{A592F425-EA68-4C88-BB96-020805D4BE56}` |
| STL | `.stl` | `{BFC86E1A-55C1-4C2D-AA36-3C25DECF30C9}` |
| PLY | `.ply` | `{F4DC6119-E235-4BAC-8089-54EDD84F8492}` |
| OBJ | `.obj` | `{D4722752-C480-4D9C-BEBE-1A9B514A8846}` |
| FBX | `.fbx` | `{FBC218D4-FD2C-41DF-B168-7F3B9E53C84E}` |
| 3MF | `.3mf` | `{D8389A63-8526-454A-9892-72F3149484B9}` |
| USD | `.usd`, `.usda`, `.usdc`, `.usdz` | `{E938BC70-4C08-4446-A15D-EE31576BFB48}` |
| STEP | `.step`, `.stp` | `{6EE961AC-AC3B-4958-A898-E30523FEE79D}` |

All eight CLSIDs above are product identities and **must not be regenerated**. STEP is the one
identity added after the original seven; it is frozen here and in
[`05-thumbnail-provider.md`](05-thumbnail-provider.md) under ADR-0001.

## Component map

```
Explorer  ──loads out of process──▶  Preview3DThumbnailProvider.dll   (this program)
                                        │
                                        ├─ COM core (class factory, lifetime, unload)
                                        ├─ bounded IStream backing + block cache
                                        ├─ family router (CLSID → adapter, never sniff)
                                        ├─ per-family bounded adapter (one per family)
                                        ├─ deterministic geometry sampler
                                        ├─ CPU tile rasterizer → premultiplied BGRA HBITMAP
                                        └─ shared model-core subset (validation, math, fallbacks)
```

The provider shares **source code** with `model-core` (validation helpers, normalized math, material
fallbacks, bounded parsers) but shares **no process, memory or cache** with the viewer or any import
process. It never loads the viewer, `Preview3DImportWorker.exe`, `Preview3DImportHost.exe` or
`Preview3DStepHost.exe`. See ADR-0004.

## Non-goals

- Explorer Preview pane (`IPreviewHandler`), context menus, icon overlays, property handlers.
- GPU rendering, Direct3D devices, or reusing the viewer's renderer inside Explorer.
- Network access, external/sidecar resolution, path recovery, process launch, or persistent caches.
- OpenUSD composition in the provider: the USD adapter uses TinyUSDZ only; composition-dependent
  files fall back to the generic icon (ADR-0003).
- Family viewer release qualification (FBX-007, 3MF-007, USD-009, STEP-008 and the original Gate 3/4
  work). Those gates remain owned by the viewer program; this program builds on the already
  implemented viewer adapters as a policy reference and does not claim their evidence.

## Definition of done

Gate 6 exit criteria (archived `10-delivery-plan.md`): every extension routes to its intended CLSID;
the 750 ms p95 target, cooperative 2 s stop point, 192 MiB accountable scratch cap and 384 MiB
measured process-commit target are qualified; OBJ/glTF sidecars are never opened from a
Shell stream; every CLSID loads into the isolated Shell surrogate rather than `explorer.exe` on a
clean installed machine and no registry value sets `DisableProcessIsolation`; malformed/fuzz/parallel/
unload soak produces no crash, hang, handle leak or persistent thread; `DllCanUnloadNow` semantics and
GDI ownership tests pass. A failed STEP qualification blocks this full eight-family release until a
scope-changing ADR and roadmap update. Registration and signing are completed through the installer
path chosen in T01/T41.
