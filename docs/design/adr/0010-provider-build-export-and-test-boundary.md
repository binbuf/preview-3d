# 0010 — Provider build, export and test integration boundary

## Status
accepted

## Context
T05 turns `thumbnail-provider/Preview3DThumbnailProvider.vcxproj` from a stub into a real DLL project
before the COM foundation (T11–T17) exists. `docs/design/05-thumbnail-provider.md` already mandates the
hardening flags and the two-symbol export surface, and `docs/design/testing-strategy.md` already places
provider unit/COM tests in `Tests.Unit.exe` and reserves `Tests.ProviderHost.exe` for the T17 harness.
What was unspecified is how the project is configured, which mechanism fixes the exports, and how the
early tests reach a DLL whose only surface is two in-proc COM entry points.

## Decision
- The provider applies the product-boundary hardening in its own `.vcxproj`: `/guard:cf` on compile
  (which also enables the linker Guard CF), `/CETCOMPAT`, `/DYNAMICBASE`, `/NXCOMPAT`; `/sdl`, `/W4`
  and warnings-as-errors come from `Directory.Build.props`. Debug uses `/Zi`, not the `/ZI` default,
  because `/guard:cf` rejects Edit and Continue.
- The export surface is a module-definition file with **exactly** `DllGetClassObject` and
  `DllCanUnloadNow`, both `PRIVATE`. `DllRegisterServer`/`DllUnregisterServer` are absent
  (installer-owned registration, ADR-0006/0007). The stub bodies live in `ProviderExports.cpp`; T11
  replaces the bodies but not the signatures or the export set.
- Provider unit/COM tests live in `tests/unit/` (`Tests.Unit.exe`) and reach the DLL at runtime with
  `LoadLibrary`/`GetProcAddress`; `Tests.Unit.vcxproj` carries a build-order-only `ProjectReference`
  to the provider. The two entry points stay `PRIVATE`, so there is no import library to link against —
  this is also how the Shell and the T17 host activate the server.
- The dependency closure (no viewer/worker/host/OpenUSD-core import) is asserted by parsing the PE
  import and export directories in `Tests.Unit.exe`, with a `dumpbin /dependents` script as the
  literal evidence command.
- The T17 COM host harness target name is frozen as `Tests.ProviderHost.exe`; T05 does not create it.

## Consequences
- T11 must keep the `.def` and the two-symbol surface and may not add a self-registration export.
- T11's tests must not assume the T05 stub behavior: `DllGetClassObject` returning
  `CLASS_E_CLASSNOTAVAILABLE` for a *family* CLSID is scaffolding, not the final contract. Only the
  unknown-CLSID and `E_POINTER` results are stable across T11.
- T17 creates `Tests.ProviderHost.exe` under the frozen name and wires it into `Preview3D.slnx`.
- Any new product binary the provider must not import should be added to the closure rule
  (currently: no import whose name starts with `Preview3D`).