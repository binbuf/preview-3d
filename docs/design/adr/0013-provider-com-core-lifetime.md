# 0013 — Provider COM core and lifetime export shape

## Status
accepted

## Context
T05 froze the provider's two-symbol `PRIVATE` export surface and its test boundary
(ADR-0010); T04 froze the CLSID→family routing table (`thumbnail-provider/FamilyRouting.h`,
ADR-0009). T11 must turn the stub `DllGetClassObject`/`DllCanUnloadNow` bodies into a real
in-proc COM server while `IInitializeWithStream` (T12), `IThumbnailProvider` (T13) and
active-call accounting (T16) do not exist yet. design/05 ("Threading and unload") fixes the
unload contract: `DllCanUnloadNow` returns `S_OK` only when live objects, factory locks and
active calls are all zero.

## Decision
- One `IClassFactory` per routed family CLSID, built in `thumbnail-provider/ComCore.{h,cpp}`.
  The family is selected from the CLSID string alone through `RouteForClsid()` (the factory
  formats the `GUID` locally; no `StringFromGUID2`/ole32 import). An unknown CLSID returns
  `CLASS_E_CLASSNOTAVAILABLE`; no content is ever sniffed or a path recovered.
- The factory supports `IID_IUnknown`/`IID_IClassFactory`; any other `riid` returns
  `E_NOINTERFACE`, a null `ppv` returns `E_POINTER`. It rejects aggregation
  (`pUnkOuter != nullptr`) with `CLASS_E_NOAGGREGATION` before an instance is constructed.
- Lifetime is explicit atomic module/object/lock/active-call counters. A factory or object
  holds one object reference for its lifetime; `LockServer(TRUE/FALSE)` adds/releases a lock;
  `ActiveCallGuard` brackets a bounded call. `DllCanUnloadNow` is `S_OK` only when all three
  counts are zero. Destruction is `noexcept`. The counters are pure reference bookkeeping, not
  a cache or model data.
- The created object is a `ProviderObject` shell implementing `IUnknown` only. T12 adds
  `IInitializeWithStream` and T13 adds `IThumbnailProvider` to the same class; T16 wraps
  `GetThumbnail` in `ActiveCallGuard`. `DllMain` remains side-effect-free: it records the
  module handle and calls `DisableThreadLibraryCalls`.
- The `.def` and the exact two-`PRIVATE`-export surface are unchanged; there is still no
  `DllRegisterServer`/`DllUnregisterServer`.

## Consequences
- T12/T13 extend `ComCore.cpp`'s `ProviderObject`, not a new object type, so the same
  instance carries the whole adapter lifecycle.
- T16 owns the active-call placement on `GetThumbnail`; the counter and guard already exist.
- T17's `Tests.ProviderHost.exe` activates via `DllGetClassObject` → `IClassFactory` →
  `CreateInstance`, exactly the shape the Shell uses.
- T41 must keep installed CLSIDs identical to `FamilyRouting.h`; the factory routes only from
  that table.
- `Tests.Unit.exe` `[provider][com]` cases load the built DLL and cover routing, identity,
  aggregation rejection, refcount, lock server and unload; only the unknown-CLSID,
  `E_POINTER` and `E_NOINTERFACE` results are stable across T12/T13.