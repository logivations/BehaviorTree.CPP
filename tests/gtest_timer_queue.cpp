#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

#include "behaviortree_cpp/utils/timer_queue.h"

#ifdef __linux__
#include <dirent.h>
#include <fstream>
#include <set>
#include <string>
#endif

using namespace std::chrono;
using Timer = BT::TimerQueue<>;

namespace
{
// Waits on an idle queue long enough for the worker thread to reach its
// "no timers" wait, so the following operation exercises that path.
void letQueueIdle()
{
  std::this_thread::sleep_for(milliseconds(50));
}
}  // namespace

#ifdef __linux__
namespace
{
std::set<int> threadIds()
{
  std::set<int> ids;
  if(DIR* dir = opendir("/proc/self/task"))
  {
    while(dirent* entry = readdir(dir))
    {
      if(entry->d_name[0] != '.')
      {
        ids.insert(std::stoi(entry->d_name));
      }
    }
    closedir(dir);
  }
  return ids;
}

long contextSwitches(int tid)
{
  std::ifstream status("/proc/self/task/" + std::to_string(tid) + "/status");
  std::string line;
  long total = 0;
  while(std::getline(status, line))
  {
    if(line.rfind("voluntary_ctxt_switches:", 0) == 0 ||
       line.rfind("nonvoluntary_ctxt_switches:", 0) == 0)
    {
      total += std::stol(line.substr(line.find(':') + 1));
    }
  }
  return total;
}
}  // namespace

// An idle TimerQueue must sleep until it gets work. It used to poll every
// 10 ms, i.e. ~100 wake-ups per second per Delay/Timeout/Sleep node.
TEST(TimerQueue, IdleThreadDoesNotWakeUp)
{
  const auto before = threadIds();
  Timer queue;
  int worker = -1;
  for(int id : threadIds())
  {
    if(before.count(id) == 0)
    {
      worker = id;
    }
  }
  ASSERT_NE(worker, -1);

  letQueueIdle();
  const long switches_before = contextSwitches(worker);
  std::this_thread::sleep_for(seconds(1));
  const long switches_after = contextSwitches(worker);

  EXPECT_LE(switches_after - switches_before, 2);
}
#endif

TEST(TimerQueue, TimerAddedToIdleQueueFiresOnTime)
{
  Timer queue;
  letQueueIdle();

  std::promise<steady_clock::time_point> fired;
  const auto start = steady_clock::now();
  queue.add(milliseconds(20), [&](bool aborted) {
    EXPECT_FALSE(aborted);
    fired.set_value(steady_clock::now());
  });

  auto future = fired.get_future();
  ASSERT_EQ(future.wait_for(seconds(1)), std::future_status::ready);
  const auto elapsed = future.get() - start;
  EXPECT_GE(elapsed, milliseconds(20));
  EXPECT_LT(elapsed, milliseconds(35));
}

TEST(TimerQueue, CancelIsPrompt)
{
  Timer queue;
  letQueueIdle();

  std::promise<bool> done;
  const auto id = queue.add(seconds(10), [&](bool aborted) { done.set_value(aborted); });
  letQueueIdle();

  const auto start = steady_clock::now();
  EXPECT_EQ(queue.cancel(id), 1u);
  auto future = done.get_future();
  ASSERT_EQ(future.wait_for(seconds(1)), std::future_status::ready);
  EXPECT_TRUE(future.get());
  EXPECT_LT(steady_clock::now() - start, milliseconds(50));
}

TEST(TimerQueue, DestroyingIdleQueueIsPrompt)
{
  auto queue = std::make_unique<Timer>();
  letQueueIdle();

  const auto start = steady_clock::now();
  queue.reset();
  EXPECT_LT(steady_clock::now() - start, milliseconds(50));
}

TEST(TimerQueue, DestroyingQueueAbortsPendingTimer)
{
  std::atomic_bool aborted = false;
  auto queue = std::make_unique<Timer>();
  queue->add(seconds(10), [&](bool was_aborted) { aborted = was_aborted; });
  letQueueIdle();

  const auto start = steady_clock::now();
  queue.reset();
  EXPECT_LT(steady_clock::now() - start, milliseconds(50));
  EXPECT_TRUE(aborted);
}
