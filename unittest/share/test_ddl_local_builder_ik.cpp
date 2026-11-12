/*
 * Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <new>
#include <thread>

#include "lib/allocator/ob_concurrent_fifo_allocator.h"
#include "lib/hash/ob_hashset.h"
#include "lib/lock/ob_spin_lock.h"
#include "lib/lock/ob_thread_cond.h"
#include "lib/ob_running_mode.h"
#include "lib/profile/ob_trace_id.h"
#include "lib/thread/ob_async_task_queue.h"
#include "lib/thread/thread_pool.h"
#include "lib/utility/ob_mod_define.h"
#include "share/location_cache/ob_location_struct.h"
#include "share/ob_errno.h"
#include "share/ob_thread_pool.h"

// Inspect the reservation under its lock, and fill the companion queue below
// the one-companion admission limit to force a real queue overflow.
#define private public
#include "share/ob_ddl_task_executor.h"
#undef private

using namespace std::chrono_literals;
using namespace oceanbase::common;
using oceanbase::share::ObAsyncTask;
using oceanbase::share::ObDDLLocalBuilder;

namespace
{

constexpr auto WAIT_TIMEOUT = 8s;
constexpr int64_t PARENT = 10;
constexpr int64_t OTHER_PARENT = 11;
constexpr char EXPERIMENT[] = "SEEKDB_IK_SCOPED_OVERLAP_EXPERIMENT";

[[noreturn]] void fail(const char *condition, int line)
{
  std::fprintf(stderr, "line %d: %s\n", line, condition);
  std::fflush(stderr);
  std::_Exit(EXIT_FAILURE);
}

#define CHECK(condition) do { if (!(condition)) { fail(#condition, __LINE__); } } while (false)

class Latch
{
public:
  void signal()
  {
    std::lock_guard<std::mutex> guard(lock_);
    signaled_ = true;
    cond_.notify_all();
  }

  bool wait()
  {
    std::unique_lock<std::mutex> guard(lock_);
    return cond_.wait_for(guard, WAIT_TIMEOUT, [&] { return signaled_; });
  }

private:
  std::mutex lock_;
  std::condition_variable cond_;
  bool signaled_ = false;
};

struct TaskProbe
{
  std::function<int()> run;
  std::atomic<int> attempted_copies{0};
  std::atomic<int> destroyed_copies{0};
  std::atomic<int> calls{0};
};

enum class CopyFailure { NONE, ALLOCATION, DEEP_COPY };

class QueueTask : public ObAsyncTask
{
public:
  explicit QueueTask(TaskProbe &probe, CopyFailure failure = CopyFailure::NONE,
                     bool queue_copy = false)
    : probe_(probe), failure_(failure), queue_copy_(queue_copy)
  {
    // The production ObIndexSSTableBuildTask also delegates retry to its parent.
    set_retry_times(0);
  }

  ~QueueTask() override
  {
    if (queue_copy_) {
      ++probe_.destroyed_copies;
    }
  }

  int process() override
  {
    ++probe_.calls;
    return probe_.run ? probe_.run() : OB_SUCCESS;
  }

  int64_t get_deep_copy_size() const override
  {
    // ObAsyncTaskQueue's allocator rejects a block above its 1 GiB limit
    // before calling the underlying allocator.
    return failure_ == CopyFailure::ALLOCATION ? (2LL << 30) : sizeof(*this);
  }

  ObAsyncTask *deep_copy(char *buf, int64_t size) const override
  {
    ++probe_.attempted_copies;
    if (failure_ == CopyFailure::DEEP_COPY) {
      return nullptr;
    }
    CHECK(size >= static_cast<int64_t>(sizeof(*this)));
    return new (buf) QueueTask(probe_, CopyFailure::NONE, true);
  }

private:
  TaskProbe &probe_;
  CopyFailure failure_;
  bool queue_copy_;
};

void start(ObDDLLocalBuilder &builder, bool pair_enabled = true)
{
  CHECK((pair_enabled ? setenv(EXPERIMENT, "1", 1) : unsetenv(EXPERIMENT)) == 0);
  CHECK(builder.init() == OB_SUCCESS);
  CHECK(builder.ik_pair_enabled() == pair_enabled);
  CHECK(builder.start() == OB_SUCCESS);
}

void stop_and_destroy(ObDDLLocalBuilder &builder)
{
  builder.stop();
  builder.server_module_thread_stop();
  builder.server_module_thread_wait();
  builder.destroy();
}

void assert_reservation(ObDDLLocalBuilder &builder, int64_t parent, bool companion_active)
{
  std::lock_guard<std::mutex> guard(builder.ik_pair_lock_);
  CHECK(builder.ik_primary_parent_ == parent);
  CHECK(builder.ik_companion_active_ == companion_active);
}

template <typename Predicate>
void wait_for_builder_state(ObDDLLocalBuilder &builder, Predicate predicate, const char *failure)
{
  const auto deadline = std::chrono::steady_clock::now() + WAIT_TIMEOUT;
  do {
    {
      std::lock_guard<std::mutex> guard(builder.ik_pair_lock_);
      if (predicate()) {
        return;
      }
    }
    std::this_thread::yield();
  } while (std::chrono::steady_clock::now() < deadline);
  fail(failure, __LINE__);
}

void wait_for_primary_to_finish(ObDDLLocalBuilder &builder)
{
  // A running primary with a cleared parent and active companion is already
  // inside finish_ik_primary(), which releases this mutex only to wait.
  wait_for_builder_state(builder, [&] {
    return builder.ik_primary_parent_ == 0 && builder.ik_primary_running_
        && builder.ik_companion_active_;
  }, "primary never waited for the active companion");
}

void complete_pair(ObDDLLocalBuilder &builder, int64_t parent)
{
  Latch companion_started;
  Latch done;
  TaskProbe primary, companion, sentinel;
  primary.run = [&] {
    CHECK(builder.begin_ik_primary(parent));
    CHECK(companion_started.wait());
    builder.finish_ik_primary();
    return OB_SUCCESS;
  };
  companion.run = [&] {
    CHECK(builder.begin_ik_companion());
    companion_started.signal();
    builder.finish_ik_companion();
    return OB_SUCCESS;
  };
  sentinel.run = [&] {
    CHECK(primary.destroyed_copies == 1 && companion.calls == 1);
    done.signal();
    return OB_SUCCESS;
  };
  QueueTask primary_task(primary), companion_task(companion), sentinel_task(sentinel);
  CHECK(builder.try_push_ik_primary(primary_task, parent));
  CHECK(builder.try_push_ik_companion(companion_task, parent));
  CHECK(builder.push_task(sentinel_task) == OB_SUCCESS);
  CHECK(done.wait());
  // The companion's finish signal can precede its task destructor. Join and
  // drain before destroying the probes referenced by queue copies.
  stop_and_destroy(builder);
  CHECK(companion.destroyed_copies == 1 && sentinel.destroyed_copies == 1);
}

enum class Shutdown { STOP_WAIT, STOP_DESTROY, DESTROY_ONLY };

void queued_shutdown(bool pair_enabled, Shutdown shutdown)
{
  ObDDLLocalBuilder builder;
  start(builder, pair_enabled);
  Latch blocker_entered, release_blocker, companion_entered, companion_rejected;
  TaskProbe blocker, queued, companion;
  blocker.run = [&] {
    blocker_entered.signal();
    CHECK(release_blocker.wait());
    return OB_SUCCESS;
  };
  companion.run = [&] {
    companion_entered.signal();
    CHECK(!builder.begin_ik_companion());
    builder.finish_ik_companion();
    companion_rejected.signal();
    return OB_CANCELED;
  };
  QueueTask blocker_task(blocker), queued_task(queued), companion_task(companion);
  CHECK(builder.push_task(blocker_task) == OB_SUCCESS);
  CHECK(blocker_entered.wait());
  if (pair_enabled) {
    CHECK(builder.try_push_ik_primary(queued_task, PARENT));
    CHECK(builder.try_push_ik_companion(companion_task, PARENT));
    CHECK(companion_entered.wait());
    assert_reservation(builder, PARENT, true);
  } else {
    CHECK(builder.push_task(queued_task) == OB_SUCCESS);
  }

  std::thread destroy_thread;
  if (shutdown == Shutdown::DESTROY_ONLY) {
    destroy_thread = std::thread([&] { builder.destroy(); });
    // The ordinary task keeps destroy() in its join after this notification.
    wait_for_builder_state(builder, [&] { return builder.ik_pair_shutting_down_; },
                           "destroy never closed IK admission");
  } else {
    builder.stop();
    builder.server_module_thread_stop();
  }
  if (pair_enabled) {
    // The primary remains blocked behind the ordinary task at this point.
    CHECK(companion_rejected.wait());
  }
  release_blocker.signal();
  if (destroy_thread.joinable()) {
    destroy_thread.join();
  } else {
    if (shutdown == Shutdown::STOP_WAIT) {
      builder.server_module_thread_wait();
    }
    builder.destroy();
  }
  CHECK(queued.calls == 0);
  CHECK(companion.calls == (pair_enabled ? 1 : 0));
  if (shutdown == Shutdown::STOP_WAIT) {
    CHECK(queued.destroyed_copies == 1);
  }

  start(builder, pair_enabled);
  Latch restarted;
  TaskProbe after_restart;
  after_restart.run = [&] {
    restarted.signal();
    return OB_SUCCESS;
  };
  QueueTask after_restart_task(after_restart);
  CHECK(builder.push_task(after_restart_task) == OB_SUCCESS);
  CHECK(restarted.wait());
  CHECK(queued.destroyed_copies == 1);
  if (pair_enabled) {
    complete_pair(builder, OTHER_PARENT);
  } else {
    stop_and_destroy(builder);
  }
  CHECK(queued.calls == 0);
  CHECK(companion.calls == (pair_enabled ? 1 : 0));
}

void running_shutdown()
{
  ObDDLLocalBuilder builder;
  start(builder);
  Latch companion_started, release_companion, primary_returned;
  TaskProbe primary, companion, sentinel;
  primary.run = [&] {
    CHECK(builder.begin_ik_primary(PARENT));
    CHECK(companion_started.wait());
    builder.finish_ik_primary();
    primary_returned.signal();
    return OB_SUCCESS;
  };
  companion.run = [&] {
    CHECK(builder.begin_ik_companion());
    companion_started.signal();
    CHECK(release_companion.wait());
    builder.finish_ik_companion();
    return OB_SUCCESS;
  };
  QueueTask primary_task(primary), companion_task(companion), sentinel_task(sentinel);
  CHECK(builder.try_push_ik_primary(primary_task, PARENT));
  CHECK(builder.try_push_ik_companion(companion_task, PARENT));
  CHECK(builder.push_task(sentinel_task) == OB_SUCCESS);
  wait_for_primary_to_finish(builder);
  builder.stop();
  builder.server_module_thread_stop();
  CHECK(primary_returned.wait());
  CHECK(sentinel.calls == 0);
  release_companion.signal();
  builder.server_module_thread_wait();
  builder.destroy();
  CHECK(primary.destroyed_copies == 1 && companion.destroyed_copies == 1);
  CHECK(sentinel.calls == 0 && sentinel.destroyed_copies == 1);
  start(builder);
  complete_pair(builder, OTHER_PARENT);
}

void submission_failures()
{
  ObDDLLocalBuilder builder;
  start(builder);
  Latch main_entered, release_main, companion_entered, release_companion, drained, companion_drained;
  TaskProbe main_blocker, companion_blocker, companion_filler, filler, marker;
  main_blocker.run = [&] {
    main_entered.signal();
    CHECK(release_main.wait());
    return OB_SUCCESS;
  };
  companion_blocker.run = [&] {
    companion_entered.signal();
    CHECK(release_companion.wait());
    return OB_SUCCESS;
  };
  marker.run = [&] {
    drained.signal();
    return OB_SUCCESS;
  };
  companion_filler.run = [&] {
    if (companion_filler.calls == 2) {
      companion_drained.signal();
    }
    return OB_SUCCESS;
  };
  QueueTask main_task(main_blocker), companion_blocker_task(companion_blocker);
  QueueTask companion_filler_task(companion_filler), filler_task(filler), marker_task(marker);
  CHECK(builder.push_task(main_task) == OB_SUCCESS);
  CHECK(main_entered.wait());
  CHECK(builder.ik_pair_queue_.push(companion_blocker_task) == OB_SUCCESS);
  CHECK(companion_entered.wait());

  TaskProbe alloc_failure, copy_failure, overflow, reserved_primary;
  QueueTask alloc_task(alloc_failure, CopyFailure::ALLOCATION);
  QueueTask copy_task(copy_failure, CopyFailure::DEEP_COPY);
  QueueTask overflow_task(overflow), reserved_primary_task(reserved_primary);
  CHECK(!builder.try_push_ik_primary(alloc_task, PARENT));
  CHECK(builder.push_task(alloc_task) == OB_ALLOCATE_MEMORY_FAILED);
  assert_reservation(builder, 0, false);
  CHECK(!builder.try_push_ik_primary(copy_task, PARENT));
  CHECK(builder.push_task(copy_task) == OB_ERR_UNEXPECTED);
  assert_reservation(builder, 0, false);

  // A queued primary publishes its reservation while the ordinary task is held.
  reserved_primary.run = [&] {
    CHECK(builder.begin_ik_primary(PARENT));
    builder.finish_ik_primary();
    return OB_SUCCESS;
  };
  CHECK(builder.try_push_ik_primary(reserved_primary_task, PARENT));
  CHECK(!builder.try_push_ik_companion(alloc_task, PARENT));
  CHECK(builder.push_task(alloc_task) == OB_ALLOCATE_MEMORY_FAILED);
  assert_reservation(builder, PARENT, false);
  CHECK(!builder.try_push_ik_companion(copy_task, PARENT));
  CHECK(builder.push_task(copy_task) == OB_ERR_UNEXPECTED);
  assert_reservation(builder, PARENT, false);

  CHECK(builder.ik_pair_queue_.push(companion_filler_task) == OB_SUCCESS);
  CHECK(builder.ik_pair_queue_.push(companion_filler_task) == OB_SUCCESS);
  CHECK(!builder.try_push_ik_companion(overflow_task, PARENT));
  CHECK(overflow.destroyed_copies == 1);
  assert_reservation(builder, PARENT, false);
  CHECK(builder.push_task(overflow_task) == OB_SUCCESS);
  CHECK(builder.push_task(marker_task) == OB_SUCCESS);
  release_companion.signal();
  release_main.signal();
  CHECK(drained.wait());
  CHECK(companion_drained.wait());
  CHECK(overflow.calls == 1 && overflow.destroyed_copies == 2);
  CHECK(alloc_failure.attempted_copies == 0 && alloc_failure.calls == 0);
  CHECK(copy_failure.attempted_copies == 4 && copy_failure.calls == 0);
  assert_reservation(builder, 0, false);

  Latch full_entered, release_full, full_drained;
  TaskProbe full_blocker, full_marker;
  full_blocker.run = [&] {
    full_entered.signal();
    CHECK(release_full.wait());
    return OB_SUCCESS;
  };
  full_marker.run = [&] {
    full_drained.signal();
    return OB_SUCCESS;
  };
  QueueTask full_blocker_task(full_blocker), full_marker_task(full_marker);
  CHECK(builder.push_task(full_blocker_task) == OB_SUCCESS);
  CHECK(full_entered.wait());
  for (int i = 0; i < (4 << 10) - 1; ++i) {
    CHECK(builder.push_task(filler_task) == OB_SUCCESS);
  }
  CHECK(builder.push_task(full_marker_task) == OB_SUCCESS);
  CHECK(!builder.try_push_ik_primary(overflow_task, PARENT));
  CHECK(builder.push_task(overflow_task) == OB_SIZE_OVERFLOW);
  CHECK(overflow.destroyed_copies == 4);
  assert_reservation(builder, 0, false);
  release_full.signal();
  CHECK(full_drained.wait());
  complete_pair(builder, OTHER_PARENT);
  CHECK(overflow.calls == 1);
  CHECK(overflow.attempted_copies == overflow.destroyed_copies);
  CHECK(filler.attempted_copies == filler.destroyed_copies);
  CHECK(companion_filler.attempted_copies == companion_filler.destroyed_copies);
}

void isolation_and_cancellation()
{
  ObDDLLocalBuilder builder;
  start(builder);
  Latch primary_started, release_primary, companion_callback, release_companion, done;
  TaskProbe primary, companion, foreign, sentinel;
  std::atomic<bool> primary_canceled{false};
  std::atomic<bool> companion_canceled{false};
  std::atomic<bool> companion_finished{false};
  primary.run = [&] {
    CHECK(builder.begin_ik_primary(PARENT));
    primary_started.signal();
    CHECK(release_primary.wait());
    CHECK(primary_canceled);
    builder.finish_ik_primary();
    CHECK(companion_finished);
    return OB_CANCELED;
  };
  companion.run = [&] {
    CHECK(builder.begin_ik_companion());
    companion_callback.signal();
    CHECK(release_companion.wait());
    CHECK(companion_canceled);
    companion_finished = true;
    builder.finish_ik_companion();
    return OB_EAGAIN;
  };
  foreign.run = [&] {
    CHECK(companion_finished && primary.destroyed_copies == 1);
    return OB_SUCCESS;
  };
  sentinel.run = [&] {
    CHECK(foreign.destroyed_copies == 1);
    done.signal();
    return OB_SUCCESS;
  };
  QueueTask primary_task(primary), companion_task(companion), foreign_task(foreign), sentinel_task(sentinel);
  CHECK(builder.try_push_ik_primary(primary_task, PARENT));
  CHECK(primary_started.wait());
  CHECK(!builder.try_push_ik_companion(foreign_task, OTHER_PARENT));
  CHECK(!builder.try_push_ik_primary(foreign_task, OTHER_PARENT));
  CHECK(foreign.attempted_copies == 0);
  CHECK(builder.push_task(foreign_task) == OB_SUCCESS);
  CHECK(builder.push_task(sentinel_task) == OB_SUCCESS);
  CHECK(builder.try_push_ik_companion(companion_task, PARENT));
  CHECK(companion_callback.wait());
  primary_canceled = true;
  release_primary.signal();
  wait_for_primary_to_finish(builder);
  CHECK(foreign.calls == 0 && sentinel.calls == 0);
  CHECK(!builder.try_push_ik_primary(foreign_task, OTHER_PARENT));
  companion_canceled = true;
  release_companion.signal();
  CHECK(done.wait());
  CHECK(primary.calls == 1 && companion.calls == 1 && foreign.calls == 1 && sentinel.calls == 1);
  assert_reservation(builder, 0, false);
  complete_pair(builder, OTHER_PARENT);
}

} // namespace

int main(int argc, char **argv)
{
  CHECK(argc == 2);
  oceanbase::lib::ObRunningModeConfig::instance().mini_mode_ = true;
  if (std::strcmp(argv[1], "queued-stop-wait") == 0) {
    queued_shutdown(true, Shutdown::STOP_WAIT);
  } else if (std::strcmp(argv[1], "queued-direct-destroy") == 0) {
    queued_shutdown(true, Shutdown::STOP_DESTROY);
  } else if (std::strcmp(argv[1], "ordinary-direct-destroy") == 0) {
    queued_shutdown(false, Shutdown::STOP_DESTROY);
  } else if (std::strcmp(argv[1], "queued-destroy-only") == 0) {
    queued_shutdown(true, Shutdown::DESTROY_ONLY);
  } else if (std::strcmp(argv[1], "running-stop-wait") == 0) {
    running_shutdown();
  } else if (std::strcmp(argv[1], "submission-failures") == 0) {
    submission_failures();
  } else if (std::strcmp(argv[1], "isolation-cancellation") == 0) {
    isolation_and_cancellation();
  } else {
    fail("unknown test case", __LINE__);
  }
  std::printf("PASS %s\n", argv[1]);
}
