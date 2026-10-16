#include <rex/memory/sync_counters.h>
#include <rex/memory/utils.h>
#include <ps5platform/kernel.h>
#include <ps5platform/shm.h>
extern "C" {
#include "host_kernel.h"
}
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

// The host's real mapping permissions for an address ("rw-p" style), to check
// the tracked answers against the kernel.
static std::string host_perms(const void* address) {
    std::ifstream maps("/proc/self/maps");
    std::string line;
    const auto target = reinterpret_cast<uintptr_t>(address);
    while (std::getline(maps, line)) {
        uintptr_t begin = 0, end = 0; char perms[8] = {};
        if (std::sscanf(line.c_str(), "%lx-%lx %7s", &begin, &end, perms) == 3 && target >= begin && target < end)
            return perms;
    }
    return "?";
}

int main() {
    using namespace rex::memory;
    constexpr size_t size = 0x100000;
    for (int failure : {HOST_CALL_NONE, HOST_CALL_ALLOCATE, HOST_CALL_MAP, HOST_CALL_PROTECT}) {
        auto mapping = CreateFileMappingHandle("test", size, PageAccess::kReadWrite, false);
        assert(mapping != kFileMappingHandleInvalid);
        auto* first = static_cast<uint8_t*>(MapFileView(mapping, nullptr, size, PageAccess::kReadWrite, 0));
        auto* second = static_cast<uint8_t*>(MapFileView(mapping, nullptr, size, PageAccess::kReadWrite, 0));
        assert(first && second && first != second);
        assert(host_direct_allocations() == 0);
        if (failure != HOST_CALL_NONE) {
            // The protection failure needs a non-default alias permission.
            if (failure == HOST_CALL_PROTECT)
                assert(Protect(second, 0x4000, PageAccess::kNoAccess));
            // First map zeroes the backing; fail the first fixed alias map.
            host_fail(failure, failure == HOST_CALL_MAP ? 2 : 1);
            assert(!AllocFixed(first, 0x4000, AllocationType::kCommit, PageAccess::kReadWrite));
            host_fail(HOST_CALL_NONE, 0);
            assert(host_direct_allocations() == 0);
            assert(Protect(second, 0x4000, PageAccess::kReadWrite));
        }
        assert(AllocFixed(first, 0x4000, AllocationType::kCommit, PageAccess::kReadWrite) == first);
        assert(host_direct_allocations() == 1);
        {
            // Protection queries of arena addresses answer from the tracked state
            // without a kernel call: the backed 64 KiB unit reports the alias's
            // access over its four pages, the next unit has no backing and no
            // access, and the answer matches the host kernel model.
            auto& counters = sync_counters();
            const uint64_t kernel_queries = counters.query_syscalls.load();
            size_t length = 0; PageAccess access{};
            assert(QueryProtect(first + 0x123, length, access));
            assert(access == PageAccess::kReadWrite && length == 0x10000 - 0x123);
            assert(QueryProtect(first + 0x10000, length, access));
            assert(access == PageAccess::kNoAccess);
            assert(host_perms(first + 0x123).rfind("rw", 0) == 0);
            assert(host_perms(first + 0x10000).rfind("---", 0) == 0);
            assert(counters.query_syscalls.load() == kernel_queries);
        }
        assert(first[0x1234] == 0 && second[0x1234] == 0);
        first[0x1234] = 0x37;
        assert(second[0x1234] == 0x37);
        assert(AllocFixed(second + 0x10000, 0x4000, AllocationType::kCommit, PageAccess::kReadWrite));
        second[0x12345] = 0x6a;
        assert(first[0x12345] == 0x6a && host_direct_allocations() == 2);
        // The two pages change in one protection call; failing it must leave both
        // pages with their previous access.
        host_fail(HOST_CALL_PROTECT, 1);
        assert(!Protect(first, 0x8000, PageAccess::kReadOnly));
        host_fail(HOST_CALL_NONE, 0);
        first[0x1234] = 0x37;  // First page must be writable again after rollback.
        first[0x5234] = 0x51;
        assert(second[0x5234] == 0x51);
        {
            auto& counters = sync_counters();
            const uint64_t calls = counters.protect_calls.load();
            const uint64_t syscalls = counters.protect_syscalls.load();
            const uint64_t cycles = counters.protect_cycles.load();
            assert(Protect(second, 0x4000, PageAccess::kReadOnly));
            // A repeated change to the same access is tracked without a kernel call.
            assert(Protect(second, 0x4000, PageAccess::kReadOnly));
            assert(counters.protect_calls.load() == calls + 2);
            assert(counters.protect_syscalls.load() == syscalls + 1);
            assert(counters.protect_cycles.load() > cycles);
            // The alias keeps its own access; the query reflects the change and the kernel agrees.
            size_t length = 0; PageAccess access{};
            assert(QueryProtect(second, length, access) && access == PageAccess::kReadOnly && length == 0x4000);
            assert(QueryProtect(first, length, access) && access == PageAccess::kReadWrite);
            assert(host_perms(second).rfind("r--", 0) == 0);
            assert(host_perms(first).rfind("rw", 0) == 0);
        }
        assert(second[0x1234] == 0x37);
        // The single-page write-watch fast path must not publish a failed
        // protection change, and retry must still reach the kernel.
        host_fail(HOST_CALL_PROTECT, 1);
        assert(!Protect(second, 0x4000, PageAccess::kReadWrite));
        host_fail(HOST_CALL_NONE, 0);
        assert(Protect(second, 0x4000, PageAccess::kReadWrite));
        second[0x1234] = 0x37;
        assert(first[0x1234] == 0x37);
        assert(UnmapFileView(mapping, first, size));
        assert(UnmapFileView(mapping, second, size));
        CloseFileMappingHandle(mapping, "test");
        assert(host_direct_allocations() == 0);
        ps5_shm_stats stats{}; ps5_shm_live(&stats);
        assert(stats.objects == 0 && stats.views == 0 && stats.ranges == 0);
    }
    auto* private_range = AllocFixed(nullptr, 0x10000, AllocationType::kReserveCommit, PageAccess::kReadWrite);
    assert(private_range);
    {
        // Addresses outside the arena still go to the kernel (the host model's
        // query is a stub, so only the fallback itself is checked here).
        auto& counters = sync_counters();
        const uint64_t kernel_queries = counters.query_syscalls.load();
        size_t length = 0; PageAccess access{};
        QueryProtect(private_range, length, access);
        assert(counters.query_syscalls.load() == kernel_queries + 1);
    }
    assert(DeallocFixed(private_range, 0, DeallocationType::kRelease));
    assert(host_direct_allocations() == 0);
    std::puts("Sparse guest backing, aliases, protection, tracked queries and allocation failure cleanup passed (host kernel model)");
}
