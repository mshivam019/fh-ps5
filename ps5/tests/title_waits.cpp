// Exercise production hooks at their ABI boundary; originals preserve callbacks
// and timeout/kick decisions, while unrelated calls must not be delayed.
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/cvar.h>
#include <rex/thread.h>
#include <rex/system/gpu_write_signal.h>
#include <cassert>
#include <vector>
#include <chrono>
#include <thread>
#include <cstdio>
using namespace std::chrono_literals;
extern "C" REX_FUNC(sub_82A6D7D8);
extern "C" REX_FUNC(sub_829F04A8);
static unsigned sleep_calls=0, idle_originals=0, fence_originals=0;
namespace rex::thread { void Sleep(std::chrono::microseconds duration) {assert(duration==200us);++sleep_calls;} }
extern "C" REX_FUNC(__imp__sub_82A6D7D8) {++idle_originals;ctx.r3.u64=0xc0;}
extern "C" REX_FUNC(__imp__sub_829F04A8) {++fence_originals;ctx.r3.u64=77;}
int main(){
 PPCContext ctx{};std::vector<uint8_t> mem(65536);auto* base=mem.data();
 auto idle=[&](uint32_t delay,uint32_t alert,uint32_t lr){ctx.r3.u64=delay;ctx.r4.u64=alert;ctx.lr=lr;sub_82A6D7D8(ctx,base);assert(ctx.r3.u64==0xc0);};
 idle(0,1,0x8259FBEC);assert(sleep_calls==1);
 idle(1,1,0x8259FBEC);idle(0,0,0x8259FBEC);idle(0,1,0x12345678);assert(sleep_calls==1);
 assert(rex::cvar::SetFlagByName("forza_render_idle_sleep_us","0"));idle(0,1,0x8259FBEC);assert(sleep_calls==1 && idle_originals==5);
 auto put=[&](uint32_t at,uint32_t v){*reinterpret_cast<uint32_t*>(base+at)=__builtin_bswap32(v);};
 constexpr uint32_t object=0x100,device=0x1000,progress=0x5000;
 put(object,device);put(device+11024,progress);put(progress,10);
 auto fence=[&]{ctx.r3.u64=object;sub_829F04A8(ctx,base);assert(ctx.r3.u64==77);};
 fence();std::this_thread::sleep_for(100us);
 auto before=std::chrono::steady_clock::now();fence();
 auto elapsed=std::chrono::steady_clock::now()-before;assert(elapsed>=500us && elapsed<100ms);
 // Signal racing the wait releases it without replacing the original body.
 std::thread signal([]{std::this_thread::sleep_for(100us);rex::system::SignalGpuWrite();});fence();signal.join();
 // Progress, disabled device and disabled option all retain original results.
 put(progress,11);fence();base[device+11069]=2;fence();
 assert(rex::cvar::SetFlagByName("forza_block_on_gpu_fence","false"));base[device+11069]=0;fence();
 assert(fence_originals==6);
 puts("PASS: exact idle-call filter, disabled controls, bounded fence wait, progress wakeup and original guest results");
}
