// Production timer queue and GPU signal, real threads and deadlines.
#include "../../.deps/rexglue-sdk/src/core/timer_queue.cpp"
#include <rex/system/gpu_write_signal.h>
#include <future>
#include <vector>
#include <ctime>
#include <cstdio>
namespace rex::thread { void set_current_thread_name(std::string_view) {} }
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
int main() {
 using namespace rex::thread;
 // An earlier publication interrupts a wait on a distant deadline.
 auto late = QueueTimerOnce([](void*){assert(false);},nullptr,Clock::now()+1h);
 std::this_thread::sleep_for(5ms);
 std::promise<void> fired;
 auto early=QueueTimerOnce([&](void*){fired.set_value();},nullptr,Clock::now()+2ms);
 assert(fired.get_future().wait_for(500ms)==std::future_status::ready);
 late.lock()->Disarm();
 // Cancellation waits for an executing callback, then prevents reuse.
 std::promise<void> entered,release;
 auto go=release.get_future().share();
 auto busy=QueueTimerRecurring([&](void*){entered.set_value();go.wait();},nullptr,Clock::now()+2ms,1ms);
 entered.get_future().wait();auto hold=busy.lock();assert(hold);
 auto cancel=std::async(std::launch::async,[hold]{hold->Disarm();});
 assert(cancel.wait_for(5ms)==std::future_status::timeout);
 release.set_value();assert(cancel.wait_for(500ms)==std::future_status::ready);
 // Self-disarm cannot wait on itself.
 std::atomic<unsigned> calls=0;
 std::weak_ptr<TimerQueueWaitItem> self;
 std::promise<void> self_done;
 self=QueueTimerRecurring([&](void*){++calls;self.lock()->Disarm();self_done.set_value();},nullptr,Clock::now()+20ms,1ms);
 assert(self_done.get_future().wait_for(500ms)==std::future_status::ready);
 std::this_thread::sleep_for(10ms);assert(calls==1);
 // Concurrent producers publish more than a ring's capacity without losses.
 std::atomic<unsigned> count=0;std::vector<std::thread> producers;
 for(int p=0;p<4;++p)producers.emplace_back([&]{for(int i=0;i<400;++i)QueueTimerOnce([&](void*){++count;},nullptr,Clock::now());});
 for(auto& p:producers)p.join();
 auto deadline=Clock::now()+2s;while(count<1600 && Clock::now()<deadline)std::this_thread::sleep_for(1ms);
 assert(count==1600);
 // Destroy an idle worker blocked with a distant timer: sentinel must wake it.
 auto start=Clock::now();
 {TimerQueue queue;queue.QueueTimer(std::make_shared<TimerQueueWaitItem>([](void*){},nullptr,&queue,Clock::now()+1h,Clock::duration::zero()));std::this_thread::sleep_for(2ms);}
 assert(Clock::now()-start<500ms);
 const auto cpu_start=std::clock();std::this_thread::sleep_for(100ms);
 const double cpu_ms=1000.0*(std::clock()-cpu_start)/CLOCKS_PER_SEC;
 printf("Idle timer CPU over 100 ms: %.3f ms\n",cpu_ms);
 using namespace rex::system;
 auto seq=GpuWriteSequence();SignalGpuWrite();start=Clock::now();WaitForGpuWrite(seq,100ms);
 assert(Clock::now()-start<50ms); // signal before wait cannot be lost
 seq=GpuWriteSequence();start=Clock::now();WaitForGpuWrite(seq,2ms);
 assert(Clock::now()-start>=1ms);assert(Clock::now()-start<500ms);
 // Wait/check/signal races; timeout is long enough to reveal a lost wakeup.
 for(int i=0;i<300;++i){
  seq=GpuWriteSequence();std::promise<void> started;
  auto waiter=std::async(std::launch::async,[&]{started.set_value();WaitForGpuWrite(seq,1s);});
  started.get_future().wait();SignalGpuWrite();
  assert(waiter.wait_for(300ms)==std::future_status::ready);
 }
 seq=GpuWriteSequence();std::vector<std::future<void>> waiters;
 for(int i=0;i<8;++i)waiters.push_back(std::async(std::launch::async,[seq]{WaitForGpuWrite(seq,1s);}));
 SignalGpuWrite();for(auto& waiter:waiters)assert(waiter.wait_for(300ms)==std::future_status::ready);
 puts("PASS: earlier deadlines, cancellation, self-disarm, concurrent publication, shutdown, bounded GPU waits and wakeup races");
}
