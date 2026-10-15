#include <rex/memory/utils.h>
#include <ps5platform/shm.h>
extern "C" {
#include "host_kernel.h"
}
#include <cassert>
#include <cstdint>
#include <cstdio>

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
        assert(Protect(second, 0x4000, PageAccess::kReadOnly));
        assert(second[0x1234] == 0x37);
        assert(UnmapFileView(mapping, first, size));
        assert(UnmapFileView(mapping, second, size));
        CloseFileMappingHandle(mapping, "test");
        assert(host_direct_allocations() == 0);
        ps5_shm_stats stats{}; ps5_shm_live(&stats);
        assert(stats.objects == 0 && stats.views == 0 && stats.ranges == 0);
    }
    auto* private_range = AllocFixed(nullptr, 0x10000, AllocationType::kReserveCommit, PageAccess::kReadWrite);
    assert(private_range);
    assert(DeallocFixed(private_range, 0, DeallocationType::kRelease));
    assert(host_direct_allocations() == 0);
    std::puts("Sparse guest backing, aliases, protection and allocation failure cleanup passed (host kernel model)");
}
