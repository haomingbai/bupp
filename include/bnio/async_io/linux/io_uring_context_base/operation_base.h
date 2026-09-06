/**
 * @file operation_base.h
 * @brief Base classes for io_uring operations.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_LINUX_IO_URING_CONTEXT_BASE_OPERATION_BASE_H_
#define BNIO_ASYNC_IO_LINUX_IO_URING_CONTEXT_BASE_OPERATION_BASE_H_

#include <bnio/async_io/time.h>
#include <bnio/base/wake_channel.h>
#include <bnio/export.h>

#include <atomic>
#include <cstddef>
#include <mutex>

namespace bnio::base {
class submission_queue_entry;
}

namespace bnio::async_io::linux_native {

class io_uring_operation_base;
class io_uring_io_operation_base;

/** Per-worker local CPU and I/O queues, and wake channel.
 *
 * Both local queues are owned exclusively by their worker thread: they are
 * pushed only by the worker itself and popped only by the worker during
 * fetch. No remote thread ever touches them — work stealing was removed —
 * so the heads are plain pointers and push/pop need no atomics.
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
struct BNIO_EXPORT io_uring_local_task_queue_state {
  /** Pushes one operation onto the local CPU queue as its new LIFO head. */
  void push_cpu(io_uring_operation_base& operation) noexcept;

  /** Pushes a linked list of tasks (order preserved relative to the caller's
   *  reversed FIFO). */
  void push_cpu(io_uring_operation_base* operations) noexcept;

  /** Removes the whole CPU queue in bulk; used by fetch. */
  [[nodiscard]] io_uring_operation_base* pop_cpu_all() noexcept;

  /** Head of the local CPU task queue. */
  io_uring_operation_base* cpu_head = nullptr;

  /** Pushes one I/O operation onto the local I/O queue as its new LIFO
   *  head; links through the inherited next field. */
  void push_io(io_uring_io_operation_base& operation) noexcept;

  /** Pushes a linked list of I/O tasks (the caller's order is preserved
   *  relative to the reversed FIFO the consumer re-establishes). */
  void push_io(io_uring_io_operation_base* operations) noexcept;

  /** Removes the whole I/O queue in bulk; used by consume_io_tasks(). */
  [[nodiscard]] io_uring_io_operation_base* pop_io_all() noexcept;

  /** Head of the local I/O queue. */
  io_uring_io_operation_base* io_head = nullptr;

  /** Previous node in the shared suspend list. */
  io_uring_local_task_queue_state* prev = nullptr;
  /** Next node in the shared suspend list. */
  io_uring_local_task_queue_state* next = nullptr;

  /** Per-worker wake channel for directed wakeups. */
  bnio::base::wake_channel wake_channel_;
};

/** One doubly-linked list of sleeping worker local states guarded by its
 * own lock.
 *
 * The list head and its guard lock are kept together so every operation on
 * the list takes exactly the lock that owns it; the lock also guards node
 * lifetime (a node is unlinked under it before its owner is destroyed). */
struct BNIO_EXPORT io_uring_worker_state_list {
  /** Guards the list and the lifetime of its nodes. */
  std::mutex lock;
  /** Head of the intrusive list. */
  io_uring_local_task_queue_state* head = nullptr;
  /** Round-robin cursor; points into the list so repeated wake-ups rotate
   *  fairly (wake_one_sleeping()). */
  io_uring_local_task_queue_state* cursor = nullptr;
};

/** Shared MPSC CPU/I/O queues and worker-group lifecycle state. */
struct BNIO_EXPORT io_uring_task_queue_state {
  /** Non-blocking fetch entry point into the shared lazy timer heap.
   *
   *  Invoked with the opaque heap pointer stored in @p timeout_heap; the
   *  second argument receives the earliest remaining deadline and the
   *  third receives the linked list of operations whose timers expired,
   *  ready to be pushed onto a CPU queue.
   *
   *  @return true when the fetch ran; false when another worker was
   *          already draining the heap.
   */
  using try_fetch_timeout_fn = bool (*)(void*, async_io::time_point&,
                                        io_uring_operation_base*&) noexcept;

  /** Atomically pushes one operation onto the shared CPU queue as its new
   *  head (lock-free MPSC with release publication). */
  void push_cpu(io_uring_operation_base& operation) noexcept;

  /** Atomically removes and returns the whole shared CPU queue. */
  [[nodiscard]] io_uring_operation_base* pop_cpu_all() noexcept;

  /** Atomically pushes one I/O operation onto the shared I/O queue as its
   *  new head; the publisher must pair the push with a wakeup of a
   *  possibly sleeping worker. */
  void push_io(io_uring_io_operation_base& operation) noexcept;

  /** Atomically removes and returns the whole shared I/O queue. */
  [[nodiscard]] io_uring_io_operation_base* pop_io_all() noexcept;

  /** Pops the whole shared I/O queue and delivers every operation through
   *  the stop channel inline: result = -ECANCELED, complete_submit_stopped(),
   *  then execute().  The caller owns the receiver-callback context; this
   *  must never run while holding submit_lock.  Traverses with `next` —
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

  /** Head of the shared MPSC CPU task queue. */
  std::atomic<io_uring_operation_base*> cpu_head{nullptr};
  /** Head of the shared MPSC I/O task queue. */
  std::atomic<io_uring_io_operation_base*> io_head{nullptr};
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
  io_uring_worker_state_list workers;

  /** Context lifecycle state: 0 while running, 1 once stop() has begun. */
  std::atomic<int> life_state{0};

  /** Opaque pointer to the shared lazy timer heap; created and owned by
   *  io_context. */
  void* timeout_heap = nullptr;
  /** Non-blocking fetch entry point for the timer heap; set by io_context
   *  when the heap is created and cleared when it is destroyed. */
  try_fetch_timeout_fn try_fetch_timeout_operations = nullptr;

  /** Shared wake channel owned by io_context.
   *
   * io_context creates the channel once and writes to it to wake
   * workers during shutdown.  Each worker's io_uring_context registers
   * read interest (IORING_POLL_ADD) before sleeping.  In normal
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
 * Base class for operations scheduled by an io_uring_context.
 */
class BNIO_EXPORT io_uring_operation_base {
 public:
  /**
   * Intrusive next pointer used by context task queues.
   */
  io_uring_operation_base* next = nullptr;

  /**
   * Completion result copied from the CQE.
   *
   * @see io_uring_cqe
   */
  int result = 0;

  /**
   * Completion flags copied from the CQE.
   *
   * @see io_uring_cqe
   */
  unsigned flags = 0;

  /**
   * Creates an unlinked operation base.
   */
  io_uring_operation_base() noexcept = default;

  /**
   * Copy construction is disabled because operations are queued intrusively.
   */
  io_uring_operation_base(const io_uring_operation_base&) = delete;

  /**
   * Copy assignment is disabled because operations are queued intrusively.
   */
  io_uring_operation_base& operator=(const io_uring_operation_base&) = delete;

  /**
   * Move construction is disabled because operations are queued intrusively.
   */
  io_uring_operation_base(io_uring_operation_base&&) = delete;

  /**
   * Move assignment is disabled because operations are queued intrusively.
   */
  io_uring_operation_base& operator=(io_uring_operation_base&&) = delete;

  /**
   * Destroys the operation base.
   */
  virtual ~io_uring_operation_base() = default;

  /**
   * Completes the operation on the context run loop.
   */
  virtual void execute() noexcept = 0;
};

/** I/O operation passively prepared by an io_uring_context run loop. */
class BNIO_EXPORT io_uring_io_operation_base : public io_uring_operation_base {
 public:
  io_uring_io_operation_base() noexcept = default;
  io_uring_io_operation_base(const io_uring_io_operation_base&) = delete;
  io_uring_io_operation_base& operator=(const io_uring_io_operation_base&) =
      delete;
  io_uring_io_operation_base(io_uring_io_operation_base&&) = delete;
  io_uring_io_operation_base& operator=(io_uring_io_operation_base&&) = delete;
  ~io_uring_io_operation_base() override = default;

  /** Fills one SQE after the run loop takes this operation from the queue. */
  virtual void prepare(bnio::base::submission_queue_entry& sqe) noexcept = 0;

  /** Selects the completion delivered when SQE preparation fails. */
  virtual void complete_submit_error(int result) noexcept = 0;

  /** Selects set_stopped completion when io_context::stop() aborts inflight
   * I/O. */
  virtual void complete_submit_stopped() noexcept = 0;

  /** Returns whether a CQE carrying -EAGAIN is a transient would-block
   *  outcome that must be re-submitted through the I/O queue instead of
   *  being delivered as a terminal error. Read/write-class operations on
   *  sockets and files return true, mirroring kqueue_context::
   *  perform_io_step(); poll/nop/timeout-class operations keep the default
   *  (false). */
  [[nodiscard]] virtual bool rearm_on_eagain() const noexcept { return false; }

  /** Next operation in the inflight doubly-linked list. */
  io_uring_io_operation_base* io_next = nullptr;
  /** Previous operation in the inflight doubly-linked list. */
  io_uring_io_operation_base* io_prev = nullptr;
};

inline void io_uring_local_task_queue_state::push_cpu(
    io_uring_operation_base& operation) noexcept {
  // Owner-only access (no stealing), so this is a plain LIFO head insert.
  operation.next = cpu_head;
  cpu_head = &operation;
}

inline void io_uring_local_task_queue_state::push_cpu(
    io_uring_operation_base* operations) noexcept {
  while (operations != nullptr) {
    io_uring_operation_base* operation = operations;
    operations = operations->next;
    operation->next = nullptr;
    push_cpu(*operation);
  }
}

inline io_uring_operation_base*
io_uring_local_task_queue_state::pop_cpu_all() noexcept {
  io_uring_operation_base* operations = cpu_head;
  cpu_head = nullptr;
  return operations;
}

inline void io_uring_local_task_queue_state::push_io(
    io_uring_io_operation_base& operation) noexcept {
  // Owner-only access (no stealing), so this is a plain LIFO head insert.
  // The I/O queue links through the inherited `next` field: `io_next` and
  // `io_prev` belong to the inflight doubly-linked list.
  operation.next = io_head;
  io_head = &operation;
}

inline void io_uring_local_task_queue_state::push_io(
    io_uring_io_operation_base* operations) noexcept {
  while (operations != nullptr) {
    io_uring_io_operation_base* operation = operations;
    operations = static_cast<io_uring_io_operation_base*>(operations->next);
    operation->next = nullptr;
    push_io(*operation);
  }
}

inline io_uring_io_operation_base*
io_uring_local_task_queue_state::pop_io_all() noexcept {
  io_uring_io_operation_base* operations = io_head;
  io_head = nullptr;
  return operations;
}

inline void io_uring_task_queue_state::push_cpu(
    io_uring_operation_base& operation) noexcept {
  io_uring_operation_base* head = cpu_head.load(std::memory_order_relaxed);
  do {
    operation.next = head;
  } while (!cpu_head.compare_exchange_weak(
      head, &operation, std::memory_order_release, std::memory_order_relaxed));
}

inline io_uring_operation_base*
io_uring_task_queue_state::pop_cpu_all() noexcept {
  return cpu_head.exchange(nullptr, std::memory_order_acquire);
}

inline void io_uring_task_queue_state::push_io(
    io_uring_io_operation_base& operation) noexcept {
  io_uring_io_operation_base* head = io_head.load(std::memory_order_relaxed);
  do {
    operation.next = head;
  } while (!io_head.compare_exchange_weak(
      head, &operation, std::memory_order_release, std::memory_order_relaxed));
}

inline io_uring_io_operation_base*
io_uring_task_queue_state::pop_io_all() noexcept {
  return io_head.exchange(nullptr, std::memory_order_acquire);
}

inline bool io_uring_task_queue_state::drain_io_stopped() noexcept {
  io_uring_io_operation_base* head = pop_io_all();
  if (head == nullptr) {
    return false;
  }
  // The shared I/O queue chains through the inherited `next` field (see
  // push_io()); the inflight list's io_next/io_prev are unrelated here.
  while (head != nullptr) {
    io_uring_io_operation_base* next =
        static_cast<io_uring_io_operation_base*>(head->next);
    head->next = nullptr;
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
inline void io_uring_link_local_state(
    io_uring_worker_state_list& list,
    io_uring_local_task_queue_state* node) noexcept {
  node->prev = nullptr;
  node->next = list.head;
  if (list.head != nullptr) {
    list.head->prev = node;
  }
  list.head = node;
}

/** Unlinks a local state from its worker list. Caller holds the list's
 *  lock. */
inline void io_uring_unlink_local_state(
    io_uring_worker_state_list& list,
    io_uring_local_task_queue_state* node) noexcept {
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
inline bool io_uring_local_state_in_list(
    const io_uring_worker_state_list& list,
    const io_uring_local_task_queue_state* node) noexcept {
  return node->prev != nullptr || list.head == node;
}

inline bool io_uring_task_queue_state::wake_one_sleeping() noexcept {
  io_uring_worker_state_list& suspend = workers;
  std::lock_guard<std::mutex> guard(suspend.lock);
  // Validate the saved cursor is still in the list; otherwise restart from
  // the head. The cursor advances so repeated wake-ups rotate fairly.
  io_uring_local_task_queue_state* start = suspend.head;
  if (suspend.cursor != nullptr) {
    io_uring_local_task_queue_state* scan = start;
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

}  // namespace bnio::async_io::linux_native

#endif  // BNIO_ASYNC_IO_LINUX_IO_URING_CONTEXT_BASE_OPERATION_BASE_H_
