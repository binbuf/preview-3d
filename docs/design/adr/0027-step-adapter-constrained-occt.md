# 0027 — The STEP thumbnail adapter links a dedicated static OCCT closure

## Status
accepted

## Context
T34 requires the STEP/STP thumbnail to use the "separately built, explicitly
limited OCCT STEP/XDE/tessellation adapter" of [ADR-0002](0002-occt-linked-into-thumbnail-adapter.md),
linked only into the thumbnail DLL. The environment only had a **dynamic**
(`x64-windows`) OCCT built for the AppContainer STEP host, while the provider and
its test executables consume the repository-root `x64-windows-static-md` manifest.
The root `Directory.Build.props` forbids upstream runtime DLLs in the release
closure, and the root manifest deliberately does not contain OCCT so the viewer,
general worker and either import host can never link it.

## Decision
Build a **dedicated static OCCT closure** from the same constrained overlay port
(`packaging/vcpkg-ports/opencascade`, `default-features=false`) for
`x64-windows-static-md`, declared by the isolated manifest
`thumbnail-provider/step-occt/`. That tree is consumed only by
`Preview3DThumbnailProvider.dll`, `Tests.Unit.exe` and `Tests.ProviderHost.exe`;
it is not added to the repository-root manifest, so the STEP host keeps its own
separate (dynamic) OCCT closure and no other binary links OCCT.

- Consumers define `OCCT_STATIC_BUILD` so `Standard_EXPORT` is empty and the
  static OCCT symbols are not re-exported from the provider DLL; the Debug
  configuration links the `debug/lib` variants so the CRT/iterator levels match.
- The adapter reuses the STEP host's product-owned Part-21 admission scanner
  (source-not-state, [ADR-0004](0004-share-source-not-state.md)) **before** any
  OCCT call, rejects external documents and over-budget input, and feeds OCCT a
  bounded seekable stream over the T12 `BoundedSource` (never a path).
- OCCT opaque calls (read/transfer and meshing/extraction) run under the T16
  exception/structured-exception containment boundary; a contained fault is a
  typed failure, never a fabricated thumbnail.
- A fixed low-detail deterministic tessellation/sample policy is used under the
  stricter provider ceilings (256 MiB stream, 192 MiB accounted scratch, reduced
  triangle cap).

## Consequences
- The provider DLL grows by the constrained OCCT static closure (~24 MB Release)
  and imports only `WS2_32/ADVAPI32/USER32` plus the CRT — no OCCT DLL and no
  product/worker/host import; the dependency-closure check stays green.
- The release payload/SBOM (T42) and the measured process-commit target (T51)
  now include this closure. T51 must confirm the 384 MiB process-commit increase
  on a real STEP corpus; the committed fixtures are small and measure ~18 ms and
  well under 1 MiB of commit per render in Release.
- OCCT's `XCAFApp_Application` singleton is created once per process but every
  document is per call and closed on `Reset`; the adapter keeps no lasting
  per-call state.