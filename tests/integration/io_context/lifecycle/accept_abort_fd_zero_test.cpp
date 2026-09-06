// Reproduction for the accept-abort descriptor-ownership defect.
//
// EXPECTED TO FAIL until the accept path is fixed. These tests assert the
// contract a caller is entitled to rely on; today bnio violates it. They are
// written red-on-purpose so the fix has an unambiguous target.
//
// ---------------------------------------------------------------------------
// The defect
// ---------------------------------------------------------------------------
// tcp_acceptor::async_accept completes with
// (std::error_code, tcp_socket). The raw accept result travels through the
// native layer as an `int` descriptor and the tcp layer unconditionally wraps
// it into an owning tcp::socket (include/bnio/tcp/async_operations.h:27).
//
// When io_context::stop() aborts an in-flight accept, the native layer
// delivers a *hard-coded* descriptor value of 0 together with
// operation_canceled:
//   include/bnio/detail/bsd/io_context_native_io/common.h:139-141      (BSD)
//   include/bnio/detail/linux/io_context_native_io/common.h:210-212    (Linux)
//
// The tcp layer cannot tell that 0 apart from a real descriptor, so it builds
// tcp::socket(0):
//   * the constructor sets O_NONBLOCK on the process's stdin
//     (include/bnio/tcp/socket.h:47-54),
//   * the destructor closes it (src/posix/tcp.cpp:86 -> close_fd(), which
//     only guards `fd < 0`, src/posix/tcp.cpp:39-47).
//
// The read/write models avoid the analogous problem by clamping with
// std::max(0, result) — harmless there, because a negative result only means
// "no bytes transferred". A descriptor value has no such clamp: 0 is a
// *valid* descriptor, so the same idiom would be useless here.
//
// ---------------------------------------------------------------------------
// Test hygiene
// ---------------------------------------------------------------------------
// Each test runs inside a stdin_guard that snapshots fd 0 and restores it
// afterwards, so a repro that closes stdin cannot perturb the rest of the
// test binary. The executable is registered RUN_SERIAL for the same reason.

#include <bnio/bnio.h>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <bexec/bexec.hpp>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <system_error>
#include <thread>
#include <utility>

namespace {

// 1 = completed via set_value(operation_canceled)
// 2 = completed via set_stopped
// 4 = completed via set_value with some other error code
constexpr int kValueCanceled = 1;
constexpr int kStopped = 2;
constexpr int kOtherValue = 4;

[[nodiscard]] const char* signal_name(int signal) noexcept {
  switch (signal) {
    case kValueCanceled:
      return "set_value(operation_canceled)";
    case kStopped:
      return "set_stopped()";
    case kOtherValue:
      return "set_value(other ec)";
    default:
      return "(no completion)";
  }
}

struct accept_state {
  std::atomic<int> signal{0};
  std::atomic<int> fd{-1};
  std::atomic<bool> open{false};
};

[[nodiscard]] int canceled_or_other(const std::error_code& ec) noexcept {
  return ec == std::make_error_code(std::errc::operation_canceled)
             ? kValueCanceled
             : kOtherValue;
}

// Receiver that lets the delivered tcp::socket be destroyed at the end of
// set_value — the ownership semantics every real accept loop uses.
struct destroying_accept_receiver {
  accept_state* state = nullptr;

  void set_value(std::error_code ec, bnio::tcp_socket socket) noexcept {
    state->fd.store(socket.native_handle(), std::memory_order_relaxed);
    state->open.store(socket.is_open(), std::memory_order_relaxed);
    // `socket` is destroyed on return: ~socket() -> close() -> ::close(0).
    state->signal.store(canceled_or_other(ec), std::memory_order_relaxed);
  }

  void set_stopped() noexcept {
    state->signal.store(kStopped, std::memory_order_relaxed);
  }
};

// Receiver that releases the descriptor instead of closing it, so the
// O_NONBLOCK mutation tcp::socket's constructor applies to fd 0 stays
// observable after the completion returns.
struct releasing_accept_receiver {
  accept_state* state = nullptr;

  void set_value(std::error_code ec, bnio::tcp_socket socket) noexcept {
    const int fd = socket.release();
    state->fd.store(fd, std::memory_order_relaxed);
    state->open.store(fd >= 0, std::memory_order_relaxed);
    state->signal.store(canceled_or_other(ec), std::memory_order_relaxed);
  }

  void set_stopped() noexcept {
    state->signal.store(kStopped, std::memory_order_relaxed);
  }
};

struct op_holder_base {
  virtual ~op_holder_base() = default;
};

template <class Op>
struct op_holder : op_holder_base {
  template <class Sender, class Receiver>
  op_holder(Sender&& sender, Receiver&& receiver)
      : op(bexec::connect(std::forward<Sender>(sender),
                          std::forward<Receiver>(receiver))) {}

  Op op;
};

// Snapshots fd 0 before the test body runs and puts it back afterwards.
// A dup()'d descriptor shares the underlying file description, so O_NONBLOCK
// set through fd 0 is also visible through the copy — the destructor
// therefore restores F_SETFL explicitly, not just the descriptor number.
class stdin_guard {
 public:
  stdin_guard() noexcept
      : saved_(::dup(0)),
        flags_(::fcntl(0, F_GETFL, 0)),
        stat_valid_(::fstat(0, &stat_) == 0) {}

  ~stdin_guard() noexcept { restore(); }

  stdin_guard(const stdin_guard&) = delete;
  stdin_guard& operator=(const stdin_guard&) = delete;

  [[nodiscard]] int flags() const noexcept { return flags_; }

  // True when fd 0 no longer refers to the file it referred to at
  // construction time — i.e. it was closed (and possibly reused by a later
  // open()) rather than merely left open.
  [[nodiscard]] bool replaced() const noexcept {
    if (!stat_valid_) {
      // fd 0 was already closed at construction time, so there is no
      // baseline to compare against and this predicate can never report
      // true — it is inert in exactly the environment where a caller would
      // most want it. Callers must not rely on it alone; pair it with an
      // assertion on the delivered descriptor value.
      return false;
    }
    if (::fcntl(0, F_GETFD, 0) < 0 && errno == EBADF) {
      return true;  // closed and not (yet) reused.
    }
    struct stat current {};
    if (::fstat(0, &current) != 0) {
      return true;
    }
    return current.st_dev != stat_.st_dev || current.st_ino != stat_.st_ino;
  }

  void restore() noexcept {
    if (saved_ < 0) {
      return;
    }
    if (::dup2(saved_, 0) == 0 && flags_ >= 0) {
      (void)::fcntl(0, F_SETFL, flags_);
    }
    ::close(saved_);
    saved_ = -1;
  }

 private:
  // Declaration order is load-bearing: members are initialized in
  // declaration order, and the constructor's init list fills stat_ via
  // fstat while initializing stat_valid_.  Keeping stat_ BEFORE
  // stat_valid_ ensures the default member initializer `stat_ {}` runs
  // first and the fstat snapshot survives; the other order zeroed the
  // snapshot after filling it and made replaced() a tautology.
  struct stat stat_ {};
  int saved_;
  int flags_;
  bool stat_valid_;
};

[[nodiscard]] bool make_listener(bnio::tcp_acceptor& acceptor) noexcept {
  return !acceptor.open(bnio::ip::tcp::v4()) &&
         !acceptor.bind(bnio::ip::endpoint(bnio::ip::address::any_v4(), 0)) &&
         !acceptor.listen(16);
}

void describe(const accept_state& state) noexcept {
  std::printf(
      "    observed: completion=%s delivered_fd=%d delivered_socket_open=%s\n",
      signal_name(state.signal.load()), state.fd.load(),
      state.open.load() ? "yes" : "no");
}

}  // namespace

// Scenario A (the must-fix case): the context is running normally, a legal
// accept is in flight, and another thread calls io_context::stop(). The abort
// path delivers set_value(operation_canceled, 0) and the tcp layer turns that
// 0 into an owning socket over stdin.
TEST(LifecycleAcceptAbortTest, aborted_accept_must_not_close_descriptor_zero) {
  stdin_guard guard;
  if (guard.flags() < 0) {
    GTEST_SKIP() << "fd 0 is closed in this environment; this scenario "
                    "cannot observe the descriptor hand-out (coverage lost)";
  }

  bnio::io_context context;
  if (!context.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  bnio::tcp_acceptor acceptor;
  ASSERT_TRUE(make_listener(acceptor)) << "failed to set up the listener";

  std::thread worker([&context] { (void)context.run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  accept_state state;
  using sender_type =
      decltype(acceptor.async_accept(context.get_post_scheduler(), 0));
  using operation_type = decltype(bexec::connect(std::declval<sender_type>(),
                                                 destroying_accept_receiver{}));
  auto holder = std::make_unique<op_holder<operation_type> >(
      acceptor.async_accept(context.get_post_scheduler(), 0),
      destroying_accept_receiver{&state});
  bexec::start(holder->op);

  // Let the worker register the accept with kqueue so it is genuinely in
  // flight when the stop lands.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  (void)context.stop();
  worker.join();

  describe(state);

  // The abort is allowed to report cancellation through either channel, but
  // it must never hand the caller a descriptor it does not own.
  EXPECT_NE(state.fd.load(), 0)
      << "the aborted accept handed the caller descriptor 0 (stdin); "
         "tcp::socket then closed it";
  EXPECT_FALSE(guard.replaced())
      << "fd 0 was closed by the aborted accept's tcp::socket";

  if (guard.replaced()) {
    std::printf(
        "    REPRODUCED: fd 0 is gone (F_GETFD -> %s) after the aborted "
        "accept's socket was destroyed\n",
        std::strerror(errno));
  }
}

// Scenario B: same abort, but the receiver releases the descriptor so it is
// not closed. This isolates the second half of the damage — tcp::socket's
// constructor flipping stdin to non-blocking mode.
TEST(LifecycleAcceptAbortTest, aborted_accept_must_not_touch_stdin_flags) {
  stdin_guard guard;
  if (guard.flags() < 0) {
    GTEST_SKIP() << "fd 0 is closed in this environment; this scenario "
                    "cannot observe the O_NONBLOCK tamper (coverage lost)";
  }
  // Establish the precondition instead of assuming it. tcp::socket's
  // constructor only sets O_NONBLOCK when it is not already set
  // (include/bnio/tcp/socket.h:50), so a stdin that happens to start
  // non-blocking would make the constructor a no-op and this scenario
  // would go GREEN while the defect is still present. Clear the bit first,
  // then compare against the normalized baseline below — guard.restore()
  // reinstates the original flags, so nothing leaks.
  if ((guard.flags() & O_NONBLOCK) != 0) {
    (void)::fcntl(0, F_SETFL, guard.flags() & ~O_NONBLOCK);
  }
  const int baseline = ::fcntl(0, F_GETFL, 0);

  bnio::io_context context;
  if (!context.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  bnio::tcp_acceptor acceptor;
  ASSERT_TRUE(make_listener(acceptor)) << "failed to set up the listener";

  std::thread worker([&context] { (void)context.run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  accept_state state;
  using sender_type =
      decltype(acceptor.async_accept(context.get_post_scheduler(), 0));
  using operation_type = decltype(bexec::connect(std::declval<sender_type>(),
                                                 releasing_accept_receiver{}));
  auto holder = std::make_unique<op_holder<operation_type> >(
      acceptor.async_accept(context.get_post_scheduler(), 0),
      releasing_accept_receiver{&state});
  bexec::start(holder->op);

  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  (void)context.stop();
  worker.join();

  describe(state);

  EXPECT_NE(state.fd.load(), 0)
      << "the aborted accept handed the caller descriptor 0 (stdin)";

  const int flags_after = ::fcntl(0, F_GETFL, 0);
  ASSERT_GE(flags_after, 0) << "fd 0 was closed: " << std::strerror(errno);
  EXPECT_EQ(flags_after & O_NONBLOCK, baseline & O_NONBLOCK)
      << "the aborted accept changed fd 0's O_NONBLOCK flag "
      << "(before=" << (baseline & O_NONBLOCK)
      << " after=" << (flags_after & O_NONBLOCK) << ")";

  if ((flags_after & O_NONBLOCK) != 0) {
    std::printf("    REPRODUCED: fd 0 was switched to O_NONBLOCK\n");
  }

  // Give the leaked descriptor back; the guard restores fd 0 afterwards.
  if (state.fd.load() == 0) {
    (void)::close(0);
  }
}

// Scenario C: the accept is started *after* stop() has already published the
// stopping state, so publish_io() rejects it and the operation completes
// inline through the same stopped branch. Fully deterministic because no
// worker is running. Whether starting work after stop() is supported is a
// separate question —
// lifecycle/stop_operation_completion_test.cpp::stop_completes_all_schedule_ops
// asserts that operations posted concurrently with stop() must still
// complete, so the path is live either way.
TEST(LifecycleAcceptAbortTest,
     accept_started_after_stop_must_not_close_descriptor_zero) {
  stdin_guard guard;
  if (guard.flags() < 0) {
    GTEST_SKIP() << "fd 0 is closed in this environment; this scenario "
                    "cannot observe the descriptor hand-out (coverage lost)";
  }

  bnio::io_context context;
  if (!context.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  // No worker ever runs: stop() publishes the stopping state and returns.
  (void)context.stop();

  bnio::tcp_acceptor acceptor;
  ASSERT_TRUE(make_listener(acceptor)) << "failed to set up the listener";

  accept_state state;
  using sender_type =
      decltype(acceptor.async_accept(context.get_post_scheduler(), 0));
  using operation_type = decltype(bexec::connect(std::declval<sender_type>(),
                                                 destroying_accept_receiver{}));
  auto holder = std::make_unique<op_holder<operation_type> >(
      acceptor.async_accept(context.get_post_scheduler(), 0),
      destroying_accept_receiver{&state});
  bexec::start(holder->op);

  describe(state);

  EXPECT_EQ(state.signal.load(), kValueCanceled)
      << "the rejected publish should complete inline with operation_canceled";
  EXPECT_NE(state.fd.load(), 0)
      << "the rejected accept handed the caller descriptor 0 (stdin)";
  EXPECT_FALSE(guard.replaced())
      << "fd 0 was closed by an accept started after stop()";
}
