// T11/T16 module lifetime counters (see ModuleLifetime.h).
//
// Free of the provider precompiled header, COM and Windows types so the same
// source compiles into Preview3DThumbnailProvider.dll and Tests.Unit.exe.

#include "ModuleLifetime.h"

#include <atomic>

namespace preview3d::provider::ModuleLifetime {
namespace {

std::atomic<std::uint32_t> g_liveObjects{0};
std::atomic<std::uint32_t> g_locks{0};
std::atomic<std::uint32_t> g_activeCalls{0};

} // namespace

void AddObject() noexcept { g_liveObjects.fetch_add(1, std::memory_order_relaxed); }
void ReleaseObject() noexcept { g_liveObjects.fetch_sub(1, std::memory_order_acq_rel); }
void AddLock() noexcept { g_locks.fetch_add(1, std::memory_order_relaxed); }
void ReleaseLock() noexcept { g_locks.fetch_sub(1, std::memory_order_acq_rel); }
void AddActiveCall() noexcept { g_activeCalls.fetch_add(1, std::memory_order_relaxed); }
void ReleaseActiveCall() noexcept { g_activeCalls.fetch_sub(1, std::memory_order_acq_rel); }

std::uint32_t LiveObjects() noexcept
{
    return g_liveObjects.load(std::memory_order_acquire);
}
std::uint32_t Locks() noexcept { return g_locks.load(std::memory_order_acquire); }
std::uint32_t ActiveCalls() noexcept
{
    return g_activeCalls.load(std::memory_order_acquire);
}

bool CanUnloadNow() noexcept
{
    return g_liveObjects.load(std::memory_order_acquire) == 0 &&
           g_locks.load(std::memory_order_acquire) == 0 &&
           g_activeCalls.load(std::memory_order_acquire) == 0;
}

} // namespace preview3d::provider::ModuleLifetime