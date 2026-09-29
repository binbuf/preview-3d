# 0009 — Freeze provider interfaces and the shared source boundary

## Status
accepted

## Context
ADR-0004 commits the provider to compiling model-core and product-parser *source* into
`Preview3DThumbnailProvider.dll`, but the interfaces those sources implement were unspecified, so
foundation tasks (T11–T17) and eight family tasks (T21–T34) risked inventing divergent contracts.
The design (05-thumbnail-provider.md, 03-file-formats-and-ingestion.md) fixes the behaviour but not
the C++ names and signatures, and a precise list of which shared/product source files the DLL
compiles was still implicit.

## Decision
Freeze the provider contracts in `thumbnail-provider/` and treat them as the single authority:

- `ProviderTypes.h`, `FamilyAdapter.h`, `GeometrySampler.h`, `CpuRasterizer.h` define the value
  types, the `IFamilyAdapter` lifecycle (bounded `Initialize`/`Parse`/`EnumerateMaterials`/
  `EnumerateGeometry`/`Reset`), the `IGeometrySampler`, the `ICpuRasterizer`, and the abstract
  `BoundedSource`. No third-party parser type appears in any signature; output is
  `preview3d::provider` and `model_core` product types only.
- Allocations go through a per-call context and the T06 `AllocationLedger`; adapters keep no
  process-global mutable cache and own no lasting threads.
- `FamilyRouting.h` is the single CLSID→family table, shared by runtime routing (T11/T13, no
  sniffing) and installer registration (T41), with the thumbnail-handler ShellEx GUID.
- `design/interfaces.md` records the signatures and classifies every shared `model-core`/platform
  and `import-worker` parser file as move, duplicate, or exclude for T07.

`ProviderLimits`, `Deadline` and `AllocationLedger` are named by these headers but defined by T06.

## Consequences
- Foundation and family tasks build against one stable interface; a change to a frozen signature is
  a design change requiring an updated interfaces document.
- The provider compiles only the enumerated subset with no worker/broker/host/viewer header; T07
  proves it and each family task adds only its parser core.
- Registration and routing cannot drift because both read `FamilyRouting.h`; a registration test
  compares installed values against the table.
- The interface headers stay GDI- and Windows-type-free (except where T12/T15 own those boundaries),
  so sampler and rasterizer are unit-testable without Explorer.