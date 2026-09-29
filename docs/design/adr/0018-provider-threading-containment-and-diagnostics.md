# 0018 — Provider threading, containment and path-redacted diagnostics

## Status
accepted

## Context
T11 put the module/object/lock/active-call counters and `ActiveCallGuard` inline in
`ComCore.cpp`, so `Tests.Unit.exe` could not exercise the unload gate without loading the DLL, and
T13 had not yet wrapped `GetThumbnail` in a guard. design/05 ("Threading and unload",
"Security and robustness") and ADR-0005 require in-flight calls to keep `DllCanUnloadNow` at
`S_FALSE`, a cooperative deadline checkpoint at bounded parser/sampler/raster intervals, a
last-resort exception/structured-exception HRESULT boundary at the COM boundary (without claiming an
in-call interrupt), and diagnostic events that are path-redacted and disabled unless troubleshooting
is enabled. The frozen adapter methods are `noexcept`, so an uncontained third-party throw would call
`std::terminate` before any COM-level boundary could translate it.

## Decision
- **Lifetime.** Move `ModuleLifetime` and `ActiveCallGuard` into
  `thumbnail-provider/ModuleLifetime.{h,cpp}` (PCH/COM/Windows-free) with added
  `LiveObjects()/Locks()/ActiveCalls()` introspection; hold an `ActiveCallGuard` for the whole body
  of both `Initialize` and `GetThumbnail`. Containment's only thread-local is a trivial
  `std::uint32_t`, which registers no TLS destructor and cannot keep the module alive.
- **Deadline.** Add a `Deadline::Checkpoint()` between every pipeline stage; the T12 source and T15
  rasterizer keep their per-unit polls, and adapters (T21–T34) poll `AdapterInput::deadline` in their
  own loops. `RunContained` records the real elapsed time of an uninterruptible call and rejects a
  completed overrun afterwards as `ERROR_TIMEOUT`; it never interrupts the call.
- **Containment.** Add `thumbnail-provider/Containment.{h,cpp}` (`RunContained`), compiled
  `/EHsc` (so `catch (...)` does not swallow structured exceptions). `std::bad_alloc` -> `E_OUTOFMEMORY`;
  any other C++ exception or a contained structured exception -> `E_FAIL`; stack overflow, breakpoint
  and single-step are not swallowed. It is a last-resort boundary, not a correctness mechanism.
- **Diagnostics.** Add `thumbnail-provider/Diagnostics.{h,cpp}`: numeric/enum events only (no field a
  path, name or model text could travel in), emitted only while enabled explicitly or through
  `PREVIEW3D_THUMBNAIL_DIAGNOSTICS=1`; off by default.
- **Rejected:** a thread pool or lasting worker (the Shell owns call scheduling); an in-call
  interrupt or hard wall-clock wall (not possible for an opaque library call); `catch (...)` alone
  (under /EHsc it misses SEH, and under the default model it swallows it); a string-bearing
  diagnostic event (a redaction step can be forgotten; removing the field removes the class).

## Consequences
- `Tests.Unit.exe` compiles the same three PCH-free sources and covers the active-call unload gate,
  injected C++/SEH faults, the after-the-fact overrun rejection and the default-off, path-free
  diagnostics (`[provider][threading]`).
- Adapters must route third-party calls through `RunContained`; `RunThumbnailPipeline` and the DIB
  boundary already do. A contained fault is reported, so the underlying fault is still fuzzed/fixed
  (T43) rather than treated as handled.
- The guard and containment add only the existing kernel32/CRT imports; the two-symbol `PRIVATE`
  export surface and the dependency-closure rule are unchanged.
- Activating diagnostics in a shipping build (registry/installer or a signed diagnostics build)
  remains installer/T52 work; the provider only provides the tested gate.