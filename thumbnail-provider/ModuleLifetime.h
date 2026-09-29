#pragma once

// T11/T16 provider module lifetime bookkeeping.
//
// design/05-thumbnail-provider.md ("Threading and unload") and ADR-0013 fix the
// unload contract: DllCanUnloadNow returns S_OK only when live objects,
// class-factory locks and in-flight bounded calls are all zero.
//
// This header owns only the counters and the RAII `ActiveCallGuard`. It is
// deliberately PCH/COM/Windows-free so Tests.Unit.exe can compile the same
// source and prove the active-call gate directly, and so it carries no policy
// beyond reference counting. It is the file T11 extracted the counters from
// (originally inline in ComCore.cpp); the definitions moved to
// ModuleLifetime.cpp for the shared test seam.
//
// The counters are the one process-global mutable state the DLL owns. They are
// pure reference bookkeeping -- not a cache and not model data. An in-flight
// bounded call holds `AddActiveCall` for its duration, so a caller that releases
// its last object while a method is still running cannot unload the module out
// from under the call.

#include <cstdint>

namespace preview3d::provider {

namespace ModuleLifetime {

// A COM object (provider object or class factory) holds one object reference
// for its lifetime.
void AddObject() noexcept;
void ReleaseObject() noexcept;

// IClassFactory::LockServer(TRUE) adds one lock; FALSE releases it.
void AddLock() noexcept;
void ReleaseLock() noexcept;

// A bounded COM call (Initialize/GetThumbnail) holds one active call for its
// duration.
void AddActiveCall() noexcept;
void ReleaseActiveCall() noexcept;

// Introspection for tests and diagnostics. Live values, not snapshots.
std::uint32_t LiveObjects() noexcept;
std::uint32_t Locks() noexcept;
std::uint32_t ActiveCalls() noexcept;

// The single predicate behind DllCanUnloadNow: true only when object, lock and
// active-call counts are all zero.
bool CanUnloadNow() noexcept;

} // namespace ModuleLifetime

// RAII marker for an in-flight bounded call. Applying it for the whole body of
// Initialize/GetThumbnail keeps the module loaded even if the caller releases
// every external reference concurrently.
class ActiveCallGuard {
public:
    ActiveCallGuard() noexcept { ModuleLifetime::AddActiveCall(); }
    ActiveCallGuard(const ActiveCallGuard&) = delete;
    ActiveCallGuard& operator=(const ActiveCallGuard&) = delete;
    ActiveCallGuard(ActiveCallGuard&&) = delete;
    ActiveCallGuard& operator=(ActiveCallGuard&&) = delete;
    ~ActiveCallGuard() noexcept { ModuleLifetime::ReleaseActiveCall(); }
};

} // namespace preview3d::provider