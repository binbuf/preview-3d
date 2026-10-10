// T06 allocation-failure injection for the provider host (see the header).
//
// Only this executable's own allocations are replaced; the built provider DLL
// loaded by the COM cases keeps the CRT allocator. The replacement mirrors the
// CRT allocator (`_malloc_dbg`/`_free_dbg` under a debug build, `malloc`/`free`
// otherwise) unless armed, so it is invisible to the golden, parallel and leak
// cases and never mixes heap families with the rest of the module.

#include "FaultInjectingAllocator.h"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

#ifdef _DEBUG
#include <crtdbg.h>
#include <xmemory>
#endif

namespace {

std::atomic<bool> g_failAllocations{false};

// Never fail the tiny iterator-debug container-proxy allocation. A default
// `std::vector`/`std::string`/`std::unordered_map` under `_ITERATOR_DEBUG_LEVEL`
// allocates its proxy in a `noexcept` constructor, so a `std::bad_alloc` there
// would terminate before reaching the adapter stage boundary. The proxy is
// exactly `sizeof(std::_Container_proxy)`; every product-owned adapter
// allocation the test targets is larger. A release build has no debug proxy.
#ifdef _DEBUG
constexpr std::size_t kMinFailingAllocation = sizeof(std::_Container_proxy);
#else
constexpr std::size_t kMinFailingAllocation = 0;
#endif

void* Allocate(std::size_t size)
{
    if (size > kMinFailingAllocation && preview3d::test::AllocationFailureArmed()) {
        throw std::bad_alloc{};
    }
    if (size == 0) {
        size = 1;
    }
#ifdef _DEBUG
    if (void* memory = _malloc_dbg(size, _NORMAL_BLOCK, __FILE__, __LINE__)) {
        return memory;
    }
#else
    if (void* memory = std::malloc(size)) {
        return memory;
    }
#endif
    throw std::bad_alloc{};
}

void Release(void* memory) noexcept
{
#ifdef _DEBUG
    _free_dbg(memory, _NORMAL_BLOCK);
#else
    std::free(memory);
#endif
}

} // namespace

void* operator new(std::size_t size) { return Allocate(size); }
void* operator new[](std::size_t size) { return Allocate(size); }

void operator delete(void* memory) noexcept { Release(memory); }
void operator delete[](void* memory) noexcept { Release(memory); }
void operator delete(void* memory, std::size_t) noexcept { Release(memory); }
void operator delete[](void* memory, std::size_t) noexcept { Release(memory); }

namespace preview3d::test {

bool AllocationFailureArmed() noexcept
{
    return g_failAllocations.load(std::memory_order_relaxed);
}

void SetAllocationFailureArmed(bool armed) noexcept
{
    g_failAllocations.store(armed, std::memory_order_relaxed);
}

} // namespace preview3d::test