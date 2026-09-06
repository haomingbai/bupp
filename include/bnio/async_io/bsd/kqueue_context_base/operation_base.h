/**
 * @file operation_base.h
 * @brief Base classes for kqueue operations.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_BSD_KQUEUE_CONTEXT_BASE_OPERATION_BASE_H_
#define BNIO_ASYNC_IO_BSD_KQUEUE_CONTEXT_BASE_OPERATION_BASE_H_

#include <bnio/async_io/time.h>
#include <bnio/base/wake_channel.h>
#include <bnio/export.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace bnio::async_io::bsd_native {

class kqueue_helper;
class kqueue_operation_base;
class kqueue_io_operation_base;

/**
 * Native action selected for a kqueue operation.
 *
 * Chosen by prepare() through kqueue_helper and consumed by the run loop
 * when registering the operation and dispatching a fired readiness event.
 */
enum class kqueue_task : std::uint8_t {
  /** No action selected; the operation cannot be registered yet. */
  none,
  /** Completes without a native registration (prep_nop). */
  nop,
  /** One EVFILT_READ registration (prep_read). */
  read,
  /** One EVFILT_WRITE registration (prep_write). */
  write,
  /** EVFILT_READ and/or EVFILT_WRITE per the original poll mask
   *  (prep_poll_add). */
  poll,
};

/**
 * Complete per-(ident, filter) registration state for one kevent.
 *
 * An I/O operation owns up to two of these (EVFILT_READ and EVFILT_WRITE
 * for poll requests). Each node is linked into exactly one wait queue
 * keyed by (ident, filter), so a single poll operation can wait on both
 * filters simultaneously.
 */
struct kqueue_registration_state {
  /** Back-pointer to the owning operation; also the kevent udata value. */
  kqueue_io_operation_base* operation = nullptr;

  /** Descriptor ident shared by every node of the owning operation. */
  std::uintptr_t ident = 0;

  /** Native filter (EVFILT_READ / EVFILT_WRITE). */
  std::int16_t filter = 0;

  /** Native action selected by prepare(). */
  kqueue_task task = kqueue_task::none;

  /** Original poll mask for poll tasks. */
  unsigned poll_mask = 0;

  /** Whether this node currently owns an armed kevent in the kernel. */
  bool armed = false;

  /** Monotonic allocation order; mirrors wait-queue insertion order. */
  std::uint64_t sequence = 0;

  /** Intrusive wait-queue links for the (ident, filter) list. */
  kqueue_registration_state* wait_next = nullptr;
  /** Reverse link paired with wait_next for the (ident, filter) list. */
  kqueue_registration_state* wait_prev = nullptr;
};

/** Per-worker local CPU and I/O queues, and wake channel.
 *
 * Both local queues are owned exclusively by their worker thread: they are
 * pushed only by the worker itself (or, in standalone mode, before/after
 * a run on the same thread) and popped only by the worker during fetch.
 * No remote thread ever touches them — work stealing was removed — so the
 * heads are plain pointers and push/pop need no atomics.
 *
 * The local I/O queue is safe without a wakeup for the same reason: an
 * operation reaches it only from a callback running on this worker's own
 * run loop, so the publisher is the thread that will drain it. That is
 * what distinguishes it from the shared I/O queue, which is MPSC and
 * therefore needs every publication to pair the push with a wakeup of a
 * possibly sleeping worker.
 *
 * The node is linked into the shared state's suspend list only while the
 * worker sleeps in the native poller, so a publisher can wake it.  A
 * remote thread touches the node only while holding that list's lock,
 * which also keeps the owning worker from being destroyed (UAF
 * protection).
 */
struct BNIO_EXPORT kqueue_local_task_queue_state {
  /** Pushes a single task onto the local CPU queue (LIFO head insert). */
  void push_cpu(kqueue_operation_base& operation) noexcept;

  /** Pushes a linked list of tasks (order preserved relative to the caller's
   *  reversed FIFO). */
  void push_cpu(kqueue_operation_base* operations) noexcept;

  /** Removes the whole CPU queue in bulk; used by fetch. */
  [[nodiscard]] kqueue_operation_base* pop_cpu_all() noexcept;

  /** Head of the intrusive local CPU queue; nodes chain through next. */
  kqueue_operation_base* cpu_head = nullptr;

  /** Pushes a single I/O task onto the local I/O queue (LIFO head insert).
   */
  void push_io(kqueue_io_operation_base& operation) noexcept;

  /** Pushes a linked list of I/O tasks (the caller's order is preserved
   *  relative to the reversed FIFO the consumer re-establishes). */
  void push_io(kqueue_io_operation_base* operations) noexcept;

  /** Removes the whole I/O queue in bulk; used by consume_io_tasks(). */
  [[nodiscard]] kqueue_io_operation_base* pop_io_all() noexcept;

  /** Head of the intrusive local I/O queue; nodes chain through io_next. */
  kqueue_io_operation_base* io_head = nullptr;

  /** Doubly-linked list links for the shared suspend list. */
  kqueue_local_task_queue_state* prev = nullptr;
  /** Forward link for the shared suspend list (see prev). */
  kqueue_local_task_queue_state* next = nullptr;

  /** Per-worker wake channel for directed wakeups. */
  bnio::base::wake_channel wake_channel_;
};

/** One doubly-linked list of sleeping worker local states guarded by its
 * own lock.
 *
 * The list head and its guard lock are kept together so every operation on
 * the list takes exactly the lock that owns it; the lock also guards node
 * lifetime (a node is unlinked under it before its owner is destroyed). */
struct BNIO_EXPORT kqueue_worker_state_list {
  /** Guards the list and the lifetime of its nodes. */
  std::mutex lock;
  /** Head of the intrusive list. */
  kqueue_local_task_queue_state* head = nullptr;
  /** Round-robin cursor; points into the list so repeated wake-ups rotate
   *  fairly (wake_one_sleeping()). */
  kqueue_local_task_queue_state* cursor = nullptr;
};

/** Shared MPSC CPU/I/O queues and worker-group lifecycle state. */
struct BNIO_EXPORT kqueue_task_queue_state {
  /**
   * Signature of the shared timeout heap's non-blocking fetch entry point:
   * fetches due timer operations and reports whether a deadline exists.
   */
  using try_fetch_timeout_fn = bool (*)(void*, async_io::time_point&,
                                        kqueue_operation_base*&) noexcept;

  /** Atomically pushes one operation onto the shared CPU queue. */
  void push_cpu(kqueue_operation_base& operation) noexcept;

  /** Atomically removes and returns the entire shared CPU queue. */
  [[nodiscard]] kqueue_operation_base* pop_cpu_all() noexcept;

  /** Atomically pushes one I/O operation onto the shared I/O queue. */
  void push_io(kqueue_io_operation_base& operation) noexcept;

  /** Atomically removes and returns the entire shared I/O queue. */
  [[nodiscard]] kqueue_io_operation_base* pop_io_all() noexcept;

  /** Pops the whole shared I/O queue and delivers every operation through
   *  the stop channel inline: result = -ECANCELED, complete_submit_stopped(),
   *  then execute().  The caller owns the receiver-callback context; this
   *  must never run while holding submit_lock.  Traverses with `io_next` —
   *  the field push_io() links with.
   *  @return true when at least one operation was delivered. */
  [[nodiscard]] bool drain_io_stopped() noexcept;

  /**
   * Wakes exactly one sleeping worker by writing its per-worker wake
   * channel. Takes only the suspend list's lock: a node on the suspend
   * list is alive (the owner unregisters under the same lock before its
   * context is destroyed), so the write cannot race a close.
   *
   * @return true if a worker was woken; false if nobody is sleeping.
   */
  [[nodiscard]] bool wake_one_sleeping() noexcept;

  /** Head of the shared MPSC CPU queue; nodes chain through next. */
  std::atomic<kqueue_operation_base*> cpu_head{nullptr};
  /** Head of the shared MPSC I/O queue; nodes chain through io_next. */
  std::atomic<kqueue_io_operation_base*> io_head{nullptr};
  /** Workers not blocked in the native poller (active workers). Incremented
   *  by enter_run(), decremented by begin_wait(), restored by end_wait().
   *  Feeds the wake fast path and the all-sleeping timer wake check. */
  std::atomic<std::size_t> awake_workers{0};
  /** Total workers currently inside io_context::run() (active + suspended +
   *  entering). Incremented by io_context::run() before any check,
   *  decremented on every run() return path; the native backends never touch
   *  it. Feeds the wake fast path. */
  std::atomic<std::size_t> running_workers{0};

  /** Sleeping worker local states (directed-wake targets), guarded by its
   *  own lock. */
  kqueue_worker_state_list workers;

  /** Lifecycle flag: 0 = running, 1 = stopping. */
  std::atomic<int> life_state{0};
  /** Opaque pointer to the shared lazy timer heap; created and owned by
   *  io_context. */
  void* timeout_heap = nullptr;
  /** Fetch entry point into the shared timeout heap; set by io_context
   *  together with timeout_heap. */
  try_fetch_timeout_fn try_fetch_timeout_operations = nullptr;

  /** Shared wake channel owned by io_context.
   *
   * io_context creates the channel once and writes to it to wake
   * workers during shutdown.  Each worker's kqueue_context registers
   * EVFILT_READ | EV_CLEAR on the read end before sleeping.  In normal
   * operation a single worker is woken directly via its per-worker
   * channel (see wake_one_sleeping()); this shared channel remains the
   * broadcast path used by stop() and as a fallback.
   */
  bnio::base::wake_channel wake_channel_;

  /** Submit-path lock shared by publish_cpu() and begin_stop().
   *
   * Both the "check state + enqueue + wake" submission critical section and
   * the stop-path state transition run inside this lock, so an operation
   * that observed the context as running is guaranteed to be drained by the
   * last worker's finish(), and an operation that observes the stopping
   * state never enqueues into a queue that may no longer be drained.
   */
  std::mutex submit_lock;
};

/**
 * Base class for operations scheduled by a kqueue_context.
 *
 * Descriptor and readiness metadata intentionally live in the context, so
 * CPU-only operations do not carry unused native I/O fields.
 */
class BNIO_EXPORT kqueue_operation_base {
 public:
  /** Intrusive next pointer used by context task queues. */
  kqueue_operation_base* next = nullptr;

  /**
   * Completion result copied from the readiness event or the native I/O
   * step; a negative errno reports a recoverable failure.
   */
  int result = 0;

  /**
   * Native kevent flags copied from the readiness notification.
   */
  unsigned flags = 0;

  /** Creates an unlinked operation base. */
  kqueue_operation_base() noexcept = default;

  /** Copy construction is disabled because operations are queued
   *  intrusively. */
  kqueue_operation_base(const kqueue_operation_base&) = delete;

  /** Copy assignment is disabled because operations are queued
   *  intrusively. */
  kqueue_operation_base& operator=(const kqueue_operation_base&) = delete;

  /** Move construction is disabled because operations are queued
   *  intrusively. */
  kqueue_operation_base(kqueue_operation_base&&) = delete;

  /** Move assignment is disabled because operations are queued
   *  intrusively. */
  kqueue_operation_base& operator=(kqueue_operation_base&&) = delete;

  /** Destroys the operation base. */
  virtual ~kqueue_operation_base() = default;

  /** Completes the operation on the context run loop. */
  virtual void execute() noexcept = 0;
};

/** I/O operation passively prepared by a kqueue_context run loop. */
class BNIO_EXPORT kqueue_io_operation_base : public kqueue_operation_base {
 public:
  kqueue_io_operation_base() noexcept = default;
  kqueue_io_operation_base(const kqueue_io_operation_base&) = delete;
  kqueue_io_operation_base& operator=(const kqueue_io_operation_base&) = delete;
  kqueue_io_operation_base(kqueue_io_operation_base&&) = delete;
  kqueue_io_operation_base& operator=(kqueue_io_operation_base&&) = delete;
  ~kqueue_io_operation_base() override = default;

  /**
   * Intrusive link used by the local and shared I/O queues. An operation is
   * either queued or inflight, never both, so this field doubles as the
   * queue link while io_next/io_prev together carry the inflight
   * doubly-linked list.
   */
  kqueue_io_operation_base* io_next = nullptr;

  /** Reverse link for the inflight doubly-linked list. */
  kqueue_io_operation_base* io_prev = nullptr;

  /**
   * Per-(ident, filter) registration state filled by the run loop.
   *
   * A poll request occupies two entries (READ + WRITE); single-filter
   * operations occupy one. `registration_count` is set by prepare_io()
   * before the operation is registered with the kqueue.
   */
  std::array<kqueue_registration_state, 2> registrations{};
  /** Number of valid entries in registrations (0-2). */
  std::uint8_t registration_count = 0;

  /** Describes the native registration after the run loop takes this task. */
  virtual void prepare(kqueue_helper& helper) noexcept = 0;

  /** Selects error completion when preparation or registration fails. */
  virtual void complete_submit_error(int result) noexcept = 0;

  /** Selects set_stopped completion when io_context::stop() aborts inflight
   * I/O. */
  virtual void complete_submit_stopped() noexcept = 0;

  /**
   * Returns whether this operation owns the syscall performed after a
   * readiness notification.
   *
   * Objectized layer-2 read/write requests override this hook. The context
   * itself never issues their native I/O calls.
   */
  [[nodiscard]] virtual bool owns_io_step() const noexcept { return false; }

  /** Performs one bounded nonblocking syscall after readiness. */
  [[nodiscard]] virtual int perform_io() noexcept { return 0; }
};

inline void kqueue_local_task_queue_state::push_cpu(
    kqueue_operation_base& operation) noexcept {
  // Owner-only access (no stealing), so this is a plain LIFO head insert.
  operation.next = cpu_head;
  cpu_head = &operation;
}

inline void kqueue_local_task_queue_state::push_cpu(
    kqueue_operation_base* operations) noexcept {
  while (operations != nullptr) {
    kqueue_operation_base* operation = operations;
    operations = operations->next;
    operation->next = nullptr;
    push_cpu(*operation);
  }
}

inline kqueue_operation_base*
kqueue_local_task_queue_state::pop_cpu_all() noexcept {
  kqueue_operation_base* operations = cpu_head;
  cpu_head = nullptr;
  return operations;
}

inline void kqueue_local_task_queue_state::push_io(
    kqueue_io_operation_base& operation) noexcept {
  // Owner-only access (no stealing), so this is a plain LIFO head insert.
  operation.io_next = io_head;
  io_head = &operation;
}

inline void kqueue_local_task_queue_state::push_io(
    kqueue_io_operation_base* operations) noexcept {
  while (operations != nullptr) {
    kqueue_io_operation_base* operation = operations;
    operations = operations->io_next;
    operation->io_next = nullptr;
    push_io(*operation);
  }
}

inline kqueue_io_operation_base*
kqueue_local_task_queue_state::pop_io_all() noexcept {
  kqueue_io_operation_base* operations = io_head;
  io_head = nullptr;
  return operations;
}

inline void kqueue_task_queue_state::push_cpu(
    kqueue_operation_base& operation) noexcept {
  kqueue_operation_base* head = cpu_head.load(std::memory_order_relaxed);
  do {
    operation.next = head;
  } while (!cpu_head.compare_exchange_weak(
      head, &operation, std::memory_order_release, std::memory_order_relaxed));
}

inline void kqueue_task_queue_state::push_io(
    kqueue_io_operation_base& operation) noexcept {
  kqueue_io_operation_base* head = io_head.load(std::memory_order_relaxed);
  do {
    operation.io_next = head;
  } while (!io_head.compare_exchange_weak(
      head, &operation, std::memory_order_release, std::memory_order_relaxed));
}

inline kqueue_operation_base* kqueue_task_queue_state::pop_cpu_all() noexcept {
  return cpu_head.exchange(nullptr, std::memory_order_acquire);
}

inline kqueue_io_operation_base*
kqueue_task_queue_state::pop_io_all() noexcept {
  return io_head.exchange(nullptr, std::memory_order_acquire);
}

inline bool kqueue_task_queue_state::drain_io_stopped() noexcept {
  kqueue_io_operation_base* head = pop_io_all();
  if (head == nullptr) {
    return false;
  }
  // The shared I/O queue chains through io_next (see push_io()).
  while (head != nullptr) {
    kqueue_io_operation_base* next = head->io_next;
    head->io_next = nullptr;
    head->result = -ECANCELED;
    head->complete_submit_stopped();
    // No worker exists in the stop-drain path: deliver inline instead of
    // re-queueing onto a local CPU queue, or the operation would never
    // be fetched again.
    head->execute();
    head = next;
  }
  return true;
}

/** Links a local state at the head of a worker list. Caller holds the
 *  list's lock. */
inline void kqueue_link_local_state(
    kqueue_worker_state_list& list,
    kqueue_local_task_queue_state* node) noexcept {
  node->prev = nullptr;
  node->next = list.head;
  if (list.head != nullptr) {
    list.head->prev = node;
  }
  list.head = node;
}

/** Unlinks a local state from its worker list. Caller holds the list's
 *  lock. */
inline void kqueue_unlink_local_state(
    kqueue_worker_state_list& list,
    kqueue_local_task_queue_state* node) noexcept {
  if (node->prev != nullptr) {
    node->prev->next = node->next;
  } else {
    list.head = node->next;
  }
  if (node->next != nullptr) {
    node->next->prev = node->prev;
  }
  node->prev = nullptr;
  node->next = nullptr;
}

/** Returns whether the node is currently linked into the given list.
 *  Caller holds the list's lock. */
inline bool kqueue_local_state_in_list(
    const kqueue_worker_state_list& list,
    const kqueue_local_task_queue_state* node) noexcept {
  return node->prev != nullptr || list.head == node;
}

inline bool kqueue_task_queue_state::wake_one_sleeping() noexcept {
  kqueue_worker_state_list& suspend = workers;
  std::lock_guard<std::mutex> guard(suspend.lock);
  // Validate the saved cursor is still in the list; otherwise restart from
  // the head. The cursor advances so repeated wake-ups rotate fairly.
  kqueue_local_task_queue_state* start = suspend.head;
  if (suspend.cursor != nullptr) {
    kqueue_local_task_queue_state* scan = start;
    while (scan != nullptr && scan != suspend.cursor) {
      scan = scan->next;
    }
    if (scan != nullptr) {
      start = suspend.cursor;
    } else {
      suspend.cursor = nullptr;
    }
  }
  if (start == nullptr) {
    return false;
  }
  (void)start->wake_channel_.wake();
  suspend.cursor = start->next;
  return true;
}

}  // namespace bnio::async_io::bsd_native

#endif  // BNIO_ASYNC_IO_BSD_KQUEUE_CONTEXT_BASE_OPERATION_BASE_H_
