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
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_