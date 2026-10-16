#include <rex/graphics/shared_memory.h>
#include <cassert>
#include <rex/cvar.h>
#include <cstdio>
namespace rex::memory { size_t page_size() { return 16384; } }
class Residency : public rex::graphics::SharedMemory {
 public:
  explicit Residency(rex::memory::Memory& m):SharedMemory(m){InitializeCommon();}
  uint64_t uploaded_bytes=0;
  void Resident(uint32_t a,uint32_t n,bool gpu=false){MakeRangeValid(a,n,gpu);}
 protected:
  bool UploadRanges(const std::vector<std::pair<uint32_t,uint32_t>>& ranges) override {
   for(auto [page,count]:ranges){uploaded_bytes+=uint64_t(count)<<page_size_log2();MakeRangeValid(page<<page_size_log2(),count<<page_size_log2(),false);}return true;
  }
};
int main(){
 // This checks invalidation widening; content tracking of hot pages is separate.
 assert(rex::cvar::SetFlagByName("shared_memory_dynamic_vertices","false"));
 rex::memory::Memory memory;assert(memory.Initialize());Residency gpu(memory);
 constexpr uint32_t page=16384,base=0x100000;
 for(unsigned i=0;i<256;++i){
  gpu.Resident(0,0x800000);
  uint32_t a=(i+1)*page+137,n=(i%9)*page+500;
  auto r=gpu.MemoryInvalidationCallback(a,n,false);
  assert(r.first<=a && uint64_t(r.first)+r.second>=uint64_t(a)+n);
  assert(r.first>=(a&~uint32_t(0x3ffff)));
  assert(uint64_t(r.first)+r.second<=((uint64_t(a)+n+0x3ffff)&~uint64_t(0x3ffff)));
  assert(!gpu.IsRangeValid(a,n));
 }
 // GPU-written neighbours must never lose validity solely due to widening.
 gpu.Resident(base,0x100000);
 gpu.Resident(base, page,true);gpu.Resident(base+3*page,page,true);
 auto r=gpu.MemoryInvalidationCallback(base+page+1,1,false);
 assert(r.first>=base+page && r.first+r.second<=base+3*page);
 assert(gpu.IsRangeValid(base,page) && gpu.IsRangeValid(base+3*page,page));
 gpu.Resident(base,0x100000);
 r=gpu.MemoryInvalidationCallback(base+page+1,1,true);
 assert(r.first==base+page && r.second==page);
 gpu.Resident(base,0x100000);
 r=gpu.MemoryInvalidationCallback(base+page+1,1,false);
 assert(r.second==0x40000);
 auto before=gpu.uploaded_bytes;
 assert(gpu.RequestRange(base,0x100000));
 assert(gpu.uploaded_bytes-before==0x40000);
 printf("PS5-page bounds, actual write coverage, exact invalidation and GPU-neighbour preservation passed; reupload bytes=%llu\n",(unsigned long long)(gpu.uploaded_bytes-before));
}
