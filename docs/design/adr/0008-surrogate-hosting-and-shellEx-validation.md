# 0008 — Thumbnail handlers run in the DllHost surrogate; extension-level ShellEx is validated

## Status
accepted

## Context
T03 (SPIKE-8b) had to prove, before foundation work commits to the design, that a thumbnail handler
is actually loaded out-of-process by the Shell, that the proposed extension-level `ShellEx` mapping
resolves when another application is the file type's default, and that the T02 rasterizer meets its
time/commit budgets inside that host. ADR-0006 marked the extension-level location provisional
"until T03 proves it".

## Decision
The provider relies on the Shell's default out-of-process thumbnail hosting: a plain
`InprocServer32`/`ThreadingModel=Apartment` registration is hosted in `DllHost.exe`, and no
`DisableProcessIsolation` value is ever written. T41 registers the validated extension-level key
`HKLM\Software\Classes\<extension>\shellex\{E357FCCD-A995-4576-B01F-234630154E96} = {family-clsid}`;
the handler does not require the extension to have an open-association ProgID, and a third-party
default ProgID does not shadow it. Per-user (`HKCU\Software\Classes`) and machine-level
(`HKLM\Software\Classes`) registrations resolve identically for the Shell, so the same shape is used
at both scopes. The surrogate loads the provider DLL from the absolute path stored in
`InprocServer32`, not from `PATH` or the current directory.

## Consequences
- T41 writes the extension-level `ShellEx` key and the CLSID/`InprocServer32`/`AppID` shape; it must
  not add a per-handler isolation opt-out. This supersedes the "provisional until T03" wording in
  ADR-0006 and `08-installation-and-registration.md`.
- T42 must keep the provider DLL at the stable absolute install path recorded in `InprocServer32`;
  the payload cannot rely on the surrogate's DLL search path.
- T44 verifies the installed handler loads in `DllHost.exe` (not `explorer.exe`) with no
  `DisableProcessIsolation`, using the same `IThumbnailCache`/module-identity method as T03.
- Crash containment comes from this surrogate boundary; it is not a security boundary (ADR-0005).