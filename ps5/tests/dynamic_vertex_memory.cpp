// Real SharedMemory with simulated PS5 16 KiB pages and a CPU upload sink.
// Dynamic tracking works per page (no 4 KiB upload units).
#include <rex/graphics/shared_memory.h>
#include <cassert>
#include <rex/cvar.h>
#include <cstdio>
#include <cstring>
#include <unordered_map>
namespace rex::memory { size_t page_size() { return 16384; } }
class Residency : public rex::graphics::SharedMemory {
 public:
  explicit Residency(rex::memory::Memory& m):SharedMemory(m){InitializeCommon();}
  void Reinitialize(){ShutdownCommon();InitializeCommon();}
  uint64_t bytes=0;
  bool fail=false, invalidate_during_upload=false;
  std::unordered_map<uint32_t, uint8_t> copied;
  bool Vertex(uint32_t address, uint32_t length=4096) {
    const std::pair<uint32_t,uint32_t> range{address,length};
    return RequestRanges(&range,1,true);
  }
 protected:
  bool UploadRanges(const std::vector<std::pair<uint32_t,uint32_t>>& ranges) override {
    if(fail) return false;
    for(auto [unit,count]:ranges) {
      const auto start=unit<<page_size_log2(), length=count<<page_size_log2();
      MakeRangeValid(start,length,false);
      bytes+=length;
      for(uint32_t i=0;i<count;++i) copied[unit+i]=*memory().TranslatePhysical<uint8_t*>((unit+i)<<page_size_log2());
    }
    if(invalidate_during_upload) {
      invalidate_during_upload=false;
      MemoryInvalidationCallback(ranges.front().first<<page_size_log2(),16384,true);
    }
    return true;
  }
};
int main(){
 rex::memory::Memory memory;assert(memory.Initialize());
 const auto allocation=memory.SystemHeapAlloc(0x84000,0x4000,rex::memory::kSystemHeapPhysical);
 assert(allocation);
 const uint32_t base=(memory.GetPhysicalAddress(allocation)+0x3fff)&~uint32_t(0x3fff);
 auto* data=memory.TranslatePhysical<uint8_t*>(base);
 std::memset(data,0,0x80000);
 Residency gpu(memory);
 auto heat=[&](uint32_t a){
   for(int i=0;i<3;++i){assert(gpu.Vertex(a));gpu.MemoryInvalidationCallback(a,16384,false);}
   assert(gpu.NeedsVertexContentCheck(a,4096));
 };
 heat(base);
 assert(gpu.Vertex(base));
 assert(!gpu.IsRangeValid(base,4096));
 const auto before=gpu.bytes;
 assert(gpu.Vertex(base));assert(gpu.bytes==before);
 // Real contents change without a callback: must upload on the next draw.
 assert(rex::memory::Protect(data,0x80000,rex::memory::PageAccess::kReadWrite));
 data[0]=42;
 assert(gpu.Vertex(base));assert(gpu.bytes==before+16384);
 assert(gpu.copied[base>>14]==42);
 assert(gpu.Vertex(base));assert(gpu.bytes==before+16384);
 // A failed copy must not publish its hash or suppress the retry.
 data[0]=43;gpu.fail=true;assert(!gpu.Vertex(base));gpu.fail=false;
 assert(gpu.Vertex(base));assert(gpu.copied[base>>14]==43);
 // Overlapping streams in the same (already uploaded, unchanged) dynamic page
 // merge and need no copy; a change anywhere in the page uploads it once.
 std::pair<uint32_t,uint32_t> batch[]={{base,4096},{base+8192,4096},{base,4096}};
 auto batch_before=gpu.bytes;
 assert(gpu.RequestRanges(batch,3,true));assert(gpu.bytes==batch_before);
 data[8192]=7;
 assert(gpu.RequestRanges(batch,3,true));assert(gpu.bytes==batch_before+16384);
 assert(gpu.RequestRanges(batch,3,true));assert(gpu.bytes==batch_before+16384);
 // Texture/index access elsewhere in the same 16 KiB page pins the page.
 assert(gpu.RequestRange(base+4096,4096));
 assert(!gpu.NeedsVertexContentCheck(base,4096));
 assert(gpu.Vertex(base));assert(gpu.IsRangeValid(base,4096));
 for(int i=0;i<5;++i)gpu.MemoryInvalidationCallback(base,16384,false);
 assert(!gpu.NeedsVertexContentCheck(base,4096));
 // GPU-written neighbour and registered watch also prevent dynamic tracking.
 const auto b=base+0x10000;
 heat(b);gpu.RangeWrittenByGpu(b+4096,4096);
 assert(!gpu.NeedsVertexContentCheck(b,4096));
 assert(gpu.IsRangeValid(b+4096,4096));
 assert(gpu.Vertex(b));assert(gpu.IsRangeValid(b+4096,4096));
 const auto c=base+0x20000;heat(c);
 auto watch=gpu.WatchMemoryRange(c+4096,4096,
   [](const auto&,void*,void*,uint64_t,bool){},nullptr,nullptr,0);
 assert(!gpu.NeedsVertexContentCheck(c,4096));gpu.UnwatchMemoryRange(watch);
 // Exact invalidation during upload must not commit a now-obsolete hash.
 const auto d=base+0x30000;heat(d);
 gpu.invalidate_during_upload=true;assert(gpu.Vertex(d));
 assert(!gpu.NeedsVertexContentCheck(d,4096));
 assert(!gpu.IsRangeValid(d,4096));assert(gpu.Vertex(d));
 const auto e=base+0x40000;heat(e);assert(gpu.Vertex(e));
 gpu.InvalidateAllPages();assert(!gpu.NeedsVertexContentCheck(e,4096));
 assert(gpu.Vertex(e));assert(gpu.IsRangeValid(e,4096));
 const auto f=base+0x50000;heat(f);assert(gpu.Vertex(f));
 assert(rex::cvar::SetFlagByName("shared_memory_dynamic_vertices","false"));
 assert(gpu.Vertex(f));assert(gpu.IsRangeValid(f,4096));
 assert(!gpu.NeedsVertexContentCheck(f,4096));
 assert(rex::cvar::SetFlagByName("shared_memory_dynamic_vertices","true"));
 const auto g=base+0x60000;heat(g);assert(gpu.Vertex(g));
 gpu.Reinitialize();heat(g);
 const auto restart_before=gpu.bytes;assert(gpu.Vertex(g));
 assert(gpu.bytes==restart_before+16384);
 puts("PASS: 16 KiB pages, changed/unchanged dynamic contents, failed upload retry, batched ranges, host-wide texture/index pinning, GPU data, watches, invalidation during upload and cache reset");
}
