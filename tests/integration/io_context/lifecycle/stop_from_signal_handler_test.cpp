// Regression test for the v0.1 circuit-break defect: stop() invoked from a
// signal handler running on the worker thread must unblock a run loop that
// is parked in the kernel wait with in-flight I/O and no timers armed.
//
// Two compounding defects made this hang:
//   1. io_context::stop_internal() excluded the calling worker from the wake
//      set (is_in_context() self-exclusion), so a signal-handler stop()
//      never wrote the shared wake channel.
//   2. io_uring_context::wait_for_cqe_event() retried on -EINTR internally,
//      so the run loop never re-evaluated should_finish() after a signal.
//
// The scenario runs in a forked child so a regression hangs the child, not
// the test process; the parent enforces a watchdog timeout and reports a
// clean failure instead of stalling the test runner.

#include <bnio/bnio.h>
#include <gtest/gtest.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <bexec/bexec.hpp>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <memory>
#include <system_error>
#include <thread>
#include <utility>

namespace {

// Child-side state: set before the handler is installed, read only from the
// handler afterwards.
bnio::io_context* g_child_ctx = nullptr;

extern "C" void on_sigusr1(int) noexcept {
  if (g_child_ctx != nullptr) {
    g_child_ctx->stop();
  }
}

struct op_holder_base {
  virtual ~op_holder_base() = default;
};

template <typename Op>
struct op_holder : op_holder_base {
  template <typename Sender, typename Receiver>
  op_holder(Sender&& s, Receiver&& r)
      : op(bexec::connect(std::forward<Sender>(s), std::forward<Receiver>(r))) {
  }
  Op op;
};

// Records how the pending accept completed: 1 = set_value(operation_canceled),
// 2 = set_stopped, 4 = set_value with an unexpected error code.
struct accept_sink {
  std::atomic<int>* outcome;
  void set_value(std::error_code ec, bnio::tcp_socket) noexcept {
    // Contract: context stop aborting inflight I/O completes via
    // set_value(operation_canceled).
    const int result =
        ec == std::make_error_code(std::errc::operation_canceled) ? 1 : 4;
    outcome->store(result, std::memory_order_relaxed);
  }
  void set_stopped() noexcept { outcome->store(2, std::memory_order_relaxed); }
};

[[noreturn]] void child_main() noexcept {
  bnio::io_context ctx;
  if (!ctx.is_open()) {
    _exit(3);
  }

  bnio::tcp_acceptor acp;
  std::error_code ec;
  if ((ec = acp.open(bnio::ip::tcp::v4())) ||
      (ec = acp.bind(bnio::ip::endpoint(bnio::ip::address::any_v4(), 0))) ||
      (ec = acp.listen(16))) {
    _exit(3);
  }

  // Arm one accept that never completes on its own: it keeps the run loop
  // in the no-timeout kernel wait with in-flight I/O, exactly like an idle
  // echo server.
  std::atomic<int> accept_outcome{0};
  using accept_sender_t =
      decltype(acp.async_accept(ctx.get_post_scheduler(), 0));
  using accept_op_t = decltype(bexec::connect(std::declval<accept_sender_t>(),
                                              accept_sink{nullptr}));
  auto holder = std::make_unique<op_holder<accept_op_t>>(
      acp.async_accept(ctx.get_post_scheduler(), 0),
      accept_sink{&accept_outcome});
  bexec::start(holder->op);

  g_child_ctx = &ctx;
  struct sigaction sa {};
  sa.sa_handler = &on_sigusr1;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  if (::sigaction(SIGUSR1, &sa, nullptr) != 0) {
    _exit(3);
  }

  // Deliver SIGUSR1 to the worker (main) thread after it had ample time to
  // park inside the kernel wait.
  const pthread_t worker_tid = pthread_self();
  std::thread signaler([worker_tid] {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    (void)::pthread_kill(worker_tid, SIGUSR1);
  });
  signaler.detach();

  (void)ctx.run();
  // run() returned: the pending accept must have been drained via
  // set_value(operation_canceled) through finish() -> abort_inflight_io().
  _exit(accept_outcome.load(std::memory_order_relaxed) == 1 ? 0 : 2);
}

TEST(LifecycleTest, stop_from_signal_handler_unblocks_kernel_wait) {
  if (!bnio::io_context().is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  const pid_t pid = ::fork();
  ASSERT_NE(pid, -1) << "fork failed: " << std::strerror(errno);
  if (pid == 0) {
    child_main();
  }

  // Watchdog: the child must exit well within this budget. A lost-wakeup
  // regression leaves it parked in io_uring_enter forever.
  constexpr auto budget = std::chrono::seconds(8);
  const auto deadline = std::chrono::steady_clock::now() + budget;
  int status = 0;
  pid_t reaped = 0;
  for (;;) {
    reaped = ::waitpid(pid, &status, WNOHANG);
    if (reaped != 0 || std::chrono::steady_clock::now() >= deadline) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (reaped == 0) {
    (void)::kill(pid, SIGKILL);
    (void)::waitpid(pid, &status, 0);
    FAIL() << "run() did not return after stop() from a signal handler; the "
              "worker stayed parked in the kernel wait (lost wakeup)";
  }
  ASSERT_EQ(reaped, pid) << "waitpid failed: " << std::strerror(errno);
  ASSERT_TRUE(WIFEXITED(status))
      << "child terminated abnormally (status=0x" << std::hex << status << ")";
  EXPECT_EQ(WEXITSTATUS(status), 0)
      << "child exit " << WEXITSTATUS(status)
      << " (2 = run() returned but the pending accept did not complete via "
         "set_value(operation_canceled); 3 = child setup failed)";
}

}  // namespace
