---
verify: x64\Release\Tests.Unit.exe
---
# T11 — Implement the COM core and lifetime exports

## Goal
Give the DLL a correct, unloadable COM implementation: a class factory per family CLSID, and
`DllGetClassObject`/`DllCanUnloadNow` (no self-registration export) with explicit module,
object and lock reference counts.

## Context (read first)
- `docs/design/05-thumbnail-provider.md` — "COM classes and extension assignment", "Threading and unload", and "Security and robustness".
- `docs/design/adr/0005-safety-bounded-parsing-not-surrogate.md` — lifetime must support surrogate contention.
- `docs/design/adr/0006-registration-through-installer.md` — registration identities; self-registration must stay consistent with T41.
- `thumbnail-provider/dllmain.cpp` — the current stub to replace.
- `docs/tasks/04-freeze-provider-interfaces.md` — the frozen CLSID→family routing table.

## Scope
- [ ] Implement `IClassFactory` (with `IClassFactory::LockServer`) selecting the adapter family from the CLSID without sniffing content.
- [ ] Implement `DllGetClassObject` and `DllCanUnloadNow` against the routing table. Do **not** implement/export `DllRegisterServer`/`DllUnregisterServer`; registration is installer-owned (ADR-0006/0007) and a self-registration export would bypass the non-clobber rules.
- [ ] Keep `DllMain` side-effect-free (record module handle only; no COM/registration work).
- [ ] Implement explicit module/object/lock refcounts so `DllCanUnloadNow` returns `S_OK` only when objects, factory locks and active calls are all zero; make destructors `noexcept`.
- [ ] Ensure aggregate/contained objects are rejected and unknown CLSIDs return `CLASS_E_CLASSNOTAVAILABLE`.
- [ ] Add tests: identity, `QueryInterface`, aggregation rejection, refcount, lock server, unload.

## Out of scope
- `IInitializeWithStream`/`IThumbnailProvider` bodies (→ T12–T13).
- Machine registration execution (→ T41).

## Design notes
- An object is apartment-affine; the Shell owns call scheduling — do not create a lasting thread pool.
- No process-global mutable caches. Registration identity strings come from the T01 roster.
- `DllMain` must not call `CoCreateInstance`, load libraries, or create threads.

## Done when
- [ ] Provider COM tests for identity/refcount/lock/unload pass in Debug and Release.
- [ ] `dumpbin /exports` shows exactly the two expected exports (`DllGetClassObject`, `DllCanUnloadNow`).
- [ ] Hand-off below filled in.

## Hand-off

**Landed.** The provider's COM core now lives in `thumbnail-provider/ComCore.h`/`ComCore.cpp`:
one `IClassFactory` per routed family CLSID (family chosen from the CLSID string alone via
`RouteForClsid`; the `GUID` is formatted locally so no ole32/`StringFromGUID2` import is added),
explicit atomic module/object/lock/active-call counts behind `ModuleLifetime::CanUnloadNow()`,
and a `ProviderObject` shell implementing `IUnknown` only. `ProviderExports.cpp` is the stable
two-symbol surface (`DllGetClassObject` → `GetClassObject`, `DllCanUnloadNow` → `CanUnloadNow ?
S_OK : S_FALSE`); `dllmain.cpp` records the module handle and calls `DisableThreadLibraryCalls`
and nothing else. New `tests/unit/ProviderComTests.cpp` (`[provider][com]`) loads the built DLL
and covers per-CLSID routing, identity, `QueryInterface`, aggregation rejection, refcounts,
`LockServer` and unload. Decision: `docs/design/adr/0013-provider-com-core-lifetime.md`; design
updates in `docs/design/05-thumbnail-provider.md` ("Threading and unload") and
`docs/design/interfaces.md` ("COM core and lifetime").

**Deviations.** The created object is a shell implementing only `IUnknown`; T12/T13 must add
`IInitializeWithStream`/`IThumbnailProvider` to the same `ProviderObject` class (out of scope
here). `CreateInstance` wraps object creation in `ActiveCallGuard` so the active-call counter is
live from T11; T16 still owns placing a guard around `GetThumbnail`. Family selection converts
the incoming `REFCLSID` to the canonical upper-case string and reuses the frozen table rather
than duplicating GUID constants. The `.def` and exact two-`PRIVATE`-export surface are unchanged;
no self-registration export exists.

**Checks (run by hand, foreground).**
- `msbuild tests\unit\Tests.Unit.vcxproj /p:SolutionDir=<repo root>\ /p:Configuration=Release|Debug
  /p:Platform=x64` → provider + `Tests.Unit.exe` build clean, 0 warnings.
- `x64\Release\Tests.Unit.exe` → **146 cases / 76347 assertions, all passed** (was 140/76245).
  `x64\Debug\Tests.Unit.exe` → 146 / 76434, all passed. `[provider]` filter → 17 cases green in
  both (Release 2313 / Debug 1923 assertions).
- `dumpbin /exports x64\Release\Preview3DThumbnailProvider.dll` → exactly `DllCanUnloadNow`,
  `DllGetClassObject` (2 names).
- `tests\unit\check-provider-dependency-closure.ps1 -Configuration Release|Debug` → exit 0; no
  `Preview3D*` import (Release now also imports the CRT `api-ms-win-crt-heap/string` shims;
  Debug adds `MSVCP140D.dll`, both CRT-only, still no product binary).
- `msbuild Preview3D.slnx /p:Configuration=Release /p:Platform=x64 /m` → provider and `Tests.Unit`
  build; the **only** errors remain the pre-existing, unrelated `compatibility-host-step` OCCT
  `x64-windows-static-md` `C1083 BRepBndLib.hxx` / `BRepMesh_IncrementalMesh.hxx` gap from T04.

**Next must know.** T12 implements `IInitializeWithStream` on `ComCore.cpp`'s `ProviderObject`
and T13 `IThumbnailProvider`; do not create a second object type. T16 wraps `GetThumbnail` in
`ActiveCallGuard` (the counter/guard already exist). T17 activates via
`DllGetClassObject`→`IClassFactory`→`CreateInstance` (the same sequence the Shell uses). T41 must
keep installed CLSIDs identical to `FamilyRouting.h`, which is the factory's only routing source.
Tests may rely only on the stable results (unknown CLSID → `CLASS_E_CLASSNOTAVAILABLE`, null
out-param → `E_POINTER`, unsupported IID → `E_NOINTERFACE`), not on which object interfaces the
factory exposes beyond `IUnknown`/`IClassFactory`.