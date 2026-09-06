#include <bnio/bnio.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

// Receiver whose set_value() sets an atomic flag.
struct join_flag_recv {
  std::atomic<bool>* flag = nullptr;
  void set_value() noexcept {
    if (flag) flag->store(true, std::memory_order_release);
  }
};

// Receiver for schedule operations.  Classifies terminal calls: an op
// racing the join either completes normally (accepted before the reject
// point) or aborts via set_value(operation_canceled); set_stopped is a
// contract violation.
struct schedule_recv {
  std::atomic<int>* counter = nullptr;  // every terminal call
  std::atomic<int>* ok = nullptr;       // set_value({}) — ran before the stop
  std::atomic<int>* canceled = nullptr;  // set_value(operation_canceled)
  std::atomic<int>* stopped = nullptr;   // set_stopped (contract violation)
  void set_value(std::error_code ec) noexcept {
    if (counter) counter->fetch_add(1, std::memory_order_relaxed);
    if (ok && ec == std::error_code{}) {
      ok->fetch_add(1, std::memory_order_relaxed);
    }
    if (canceled && ec == std::make_error_code(std::errc::operation_canceled)) {
      canceled->fetch_add(1, std::memory_order_relaxed);
    }
  }
  void set_stopped() noexcept {
    if (counter) counter->fetch_add(1, std::memory_order_relaxed);
    if (stopped) stopped->fetch_add(1, std::memory_order_relaxed);
  }
};

// Heap-based holders so async operations outlive the test loop.
struct op_holder_base {
  virtual ~op_holder_base() = default;
};

template <typename Op>
struct op_holder : op_holder_base {
  Op op;
  template <typename Sender, typename Receiver>
  explicit op_holder(Sender&& s, Receiver&& r)
      : op(bexec::connect(std::forward<Sender>(s), std::forward<Receiver>(r))) {
  }
};

// Test 1: join_completes_sender
// Create io_context, start a worker, join() completes and stops the context.
TEST(LifecycleTest, join_completes_sender) {
  auto ctx = std::make_unique<bnio::io_context>();
  if (!ctx->is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  std::thread worker([&ctx]() { ctx->run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  std::atomic<bool> completed{false};
  auto sender = ctx->join();
  auto op = bexec::connect(sender, join_flag_recv{&completed});
  bexec::start(op);

  worker.join();

  EXPECT_TRUE(completed.load(std::memory_order_acquire));
}

// Test 2: join_multi_thread
// Multiple threads call join() concurrently; all must complete.
TEST(LifecycleTest, join_multi_thread) {
  auto ctx = std::make_unique<bnio::io_context>();
  if (!ctx->is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  constexpr int N = 4;
  std::vector<std::atomic<bool>> completed(N);
  std::vector<std::thread> joiners;

  std::thread worker([&ctx]() { ctx->run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  for (int i = 0; i < N; ++i) {
    joiners.emplace_back([&ctx, &completed, i]() {
      auto sender = ctx->join();
      auto op = bexec::connect(sender, join_flag_recv{&completed[i]});
      bexec::start(op);
    });
  }

  for (auto& t : joiners) {
    t.join();
  }

  worker.join();

  for (int i = 0; i < N; ++i) {
    EXPECT_TRUE(completed[i].load(std::memory_order_acquire));
  }
}

// Test 3: join_races_with_schedule
// Schedule ops during join should complete (set_value or set_stopped).
TEST(LifecycleTest, join_races_with_schedule) {
  auto ctx = std::make_unique<bnio::io_context>();
  if (!ctx->is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  constexpr int N = 100;
  std::atomic<int> completed{0};
  std::atomic<int> stopped{0};
  std::vector<std::unique_ptr<op_holder_base>> ops;
  ops.reserve(N);

  std::thread worker([&ctx]() { ctx->run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  auto sched = ctx->get_post_scheduler();
  using Sender = decltype(sched.schedule());
  using Op =
      decltype(bexec::connect(std::declval<Sender>(), schedule_recv{nullptr}));

  // Post N schedule operations on the heap so they outlive the test loop.
  for (int i = 0; i < N; ++i) {
    auto sender = sched.schedule();
    auto h = std::make_unique<op_holder<Op>>(
        sender, schedule_recv{&completed, nullptr, &stopped});
    bexec::start(h->op);
    ops.push_back(std::move(h));
  }

  // Give the scheduler a chance to start processing.
  std::this_thread::sleep_for(std::chrono::milliseconds(5));

  // Join while schedule ops are in-flight.
  std::atomic<bool> join_done{false};
  auto join_sender = ctx->join();
  auto join_op = bexec::connect(join_sender, join_flag_recv{&join_done});
  bexec::start(join_op);

  worker.join();

  EXPECT_TRUE(join_done.load(std::memory_order_acquire));
  EXPECT_EQ(N, completed.load(std::memory_order_relaxed));
  // Whatever the race outcome, no operation may complete via set_stopped.
  EXPECT_EQ(stopped.load(std::memory_order_relaxed), 0);
}

// Test 4: double_join
// Calling join() twice: first completes the stop, second completes immediately.
TEST(LifecycleTest, double_join) {
  auto ctx = std::make_unique<bnio::io_context>();
  if (!ctx->is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  std::thread worker([&ctx]() { ctx->run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  std::atomic<bool> first_done{false};
  auto sender1 = ctx->join();
  auto op1 = bexec::connect(sender1, join_flag_recv{&first_done});
  bexec::start(op1);

  worker.join();

  EXPECT_TRUE(first_done.load(std::memory_order_acquire));

  // Second join should complete immediately since stopped_ is already true.
  std::atomic<bool> second_done{false};
  auto sender2 = ctx->join();
  auto op2 = bexec::connect(sender2, join_flag_recv{&second_done});
  bexec::start(op2);

  EXPECT_TRUE(second_done.load(std::memory_order_acquire));
}

// Test 5: stop_then_join
// Calling stop() then join(): join completes immediately.
TEST(LifecycleTest, stop_then_join) {
  auto ctx = std::make_unique<bnio::io_context>();
  if (!ctx->is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  std::thread worker([&ctx]() { ctx->run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  ctx->stop();
  worker.join();

  // Join after stop() should complete immediately.
  std::atomic<bool> join_done{false};
  auto sender = ctx->join();
  auto op = bexec::connect(sender, join_flag_recv{&join_done});
  bexec::start(op);

  EXPECT_TRUE(join_done.load(std::memory_order_acquire));
}

// Test 6: join_then_stop
// Start join() then call stop() — both should succeed.
TEST(LifecycleTest, join_then_stop) {
  auto ctx = std::make_unique<bnio::io_context>();
  if (!ctx->is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  std::thread worker([&ctx]() { ctx->run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  std::atomic<bool> join_done{false};
  auto join_sender = ctx->join();
  auto join_op = bexec::connect(join_sender, join_flag_recv{&join_done});
  bexec::start(join_op);

  // Give join a head start to CAS the life_state.
  std::this_thread::sleep_for(std::chrono::milliseconds(5));

  // stop() should observe life_state already == stopping and spin on stopped_.
  int stop_ret = ctx->stop();

  worker.join();

  EXPECT_TRUE(join_done.load(std::memory_order_acquire));
  EXPECT_EQ(stop_ret, 0);
}

// Test 7: is_stopped_after_join
// Verify is_stopped() returns true after join completes.
TEST(LifecycleTest, is_stopped_after_join) {
  auto ctx = std::make_unique<bnio::io_context>();
  if (!ctx->is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  std::thread worker([&ctx]() { ctx->run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  std::atomic<bool> completed{false};
  auto sender = ctx->join();
  auto op = bexec::connect(sender, join_flag_recv{&completed});
  bexec::start(op);

  worker.join();

  EXPECT_TRUE(completed.load(std::memory_order_acquire));
  EXPECT_TRUE(ctx->is_stopped());
}

}  // namespace
