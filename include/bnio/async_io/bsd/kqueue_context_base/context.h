/**
 * @file context.h
 * @brief kqueue_context class declaration.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_BSD_KQUEUE_CONTEXT_BASE_CONTEXT_H_
#define BNIO_ASYNC_IO_BSD_KQUEUE_CONTEXT_BASE_CONTEXT_H_

#include <bnio/async_io/bsd/kqueue_context_base/operation_base.h>
#include <bnio/async_io/bsd/kqueue_context_base/options.h>
#include <bnio/async_io/bsd/kqueue_helper.h>
#include <bnio/async_io/buffer_view.h>
#include <bnio/async_io/descriptor_view.h>
#include <bnio/async_io/dns.h>
#include <bnio/async_io/ip/endpoint.h>
#include <bnio/async_io/random_access_file.h>
#include <bnio/async_io/socket_view.h>
#include <bnio/async_io/time.h>
#include <bnio/base/bsd/kqueue.h>
#include <bnio/export.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace bnio::async_io::bsd_native {

/**
 * Passive readiness event loop backed by a BSD kqueue.
 *
 * Single-owner and non-reentrant: at most one thread may call run() at a
 * time, concurrent calls to run() on the same context are undefined
 * behavior, and calling run() again after the single run loop has reached
 * finished is not supported. The context owns one base::kqueue instance.
 */
class BNIO_EXPORT kqueue_context {
 public:
  /**
   * Monotonic clock used by async I/O scheduling.
   */
  using steady_clock = bnio::async_io::steady_clock;

  /**
   * Clock used as the default async I/O scheduling clock.
   */
  using clock = bnio::async_io::clock;

  /**
   * Wall-clock type for APIs that explicitly need system time.
   */
  using system_clock = bnio::async_io::system_clock;

  /**
   * Canonical async I/O duration.
   */
  using duration = bnio::async_io::duration;

  /**
   * Time point represented with the default async I/O clock.
   */
  using time_point = bnio::async_io::time_point;

  /**
   * System-clock time point represented with async I/O duration precision.
   */
  using system_time_point = bnio::async_io::system_time_point;

  /** Creates a closed context. */
  kqueue_context() noexcept;

  /** Creates a context and attempts to open its kqueue. */
  explicit kqueue_context(const kqueue_context_options& options) noexcept;

  /**
   * Stops and releases the context kqueue.
   */
  ~kqueue_context() noexcept;

  /**
   * Copy construction is disabled because the context owns a kqueue.
   */
  kqueue_context(const kqueue_context&) = delete;

  /**
   * Copy assignment is disabled because the context owns a kqueue.
   */
  kqueue_context& operator=(const kqueue_context&) = delete;

  /**
   * Move construction is disabled because the context owns a kqueue and
   * single-owner run-loop state.
   */
  kqueue_context(kqueue_context&&) = delete;

  /**
   * Move assignment is disabled because the context owns a kqueue and
   * single-owner run-loop state.
   */
  kqueue_context& operator=(kqueue_context&&) = delete;

  /** Initializes the native queue and wakeup event. */
  int queue_init(const kqueue_context_options& options = {}) noexcept;

  /**
   * Releases the native queue and pending internal storage.
   *
   * A context torn down without a clean finish (never run, fatal error,
   * forced close) first runs the same abort-and-deliver path as finish():
   * inflight and queued I/O is aborted and every completion executes
   * synchronously on the calling thread, so each published operation
   * reaches a terminal receiver call. A finished context releases without
   * touching the shared queues its sibling workers still own.
   */
  void queue_exit() noexcept;

  /** Returns whether the native queue is open. */
  [[nodiscard]] bool is_open() const noexcept;

  /** Creates a typed poll sender. */
  [[nodiscard]] auto async_poll(descriptor_view descriptor, unsigned poll_mask);

  /**
   * Creates a streaming read sender whose start performs ::read, advancing
   * the kernel file position.
   */
  [[nodiscard]] auto async_read(descriptor_view descriptor, buffer_view buffer);

  /**
   * Creates a positioned read sender whose start performs @c pread() at the
   * given offset, without observing or advancing the kernel file position.
   */
  [[nodiscard]] auto async_read(random_access_file file, buffer_view buffer,
                                std::uint64_t offset);

  /**
   * Creates a streaming write sender whose start performs ::write,
   * advancing the kernel file position.
   */
  [[nodiscard]] auto async_write(descriptor_view descriptor, const void* data,
                                 std::size_t size);

  /**
   * Creates a positioned write sender whose start performs @c pwrite() at
   * the given offset, without observing or advancing the kernel file
   * position.
   */
  [[nodiscard]] auto async_write(random_access_file file, const void* data,
                                 std::size_t size, std::uint64_t offset);

  /** Creates one nonblocking stream receive sender. */
  [[nodiscard]] auto async_receive(stream_socket_view socket,
                                   buffer_view buffer, int flags = 0);

  /** Creates one nonblocking stream send sender. */
  [[nodiscard]] auto async_send(stream_socket_view socket, const void* data,
                                std::size_t size, int flags = 0);

  /** Creates one nonblocking connected-datagram receive sender. */
  [[nodiscard]] auto async_receive(datagram_socket_view socket,
                                   buffer_view buffer, int flags = 0);

  /** Creates one nonblocking connected-datagram send sender. */
  [[nodiscard]] auto async_send(datagram_socket_view socket, const void* data,
                                std::size_t size, int flags = 0);

  /** Creates one endpoint-aware datagram receive sender. */
  [[nodiscard]] auto async_receive_from(datagram_socket_view socket,
                                        buffer_view buffer,
                                        ip::endpoint& endpoint, int flags = 0);

  /** Creates one endpoint-aware datagram send sender. */
  [[nodiscard]] auto async_send_to(datagram_socket_view socket,
                                   const void* data, std::size_t size,
                                   const ip::endpoint& endpoint, int flags = 0);

  /** Creates one nonblocking accept sender. */
  [[nodiscard]] auto async_accept(stream_socket_view socket, int flags = 0);

  /** Creates one nonblocking connect sender. */
  [[nodiscard]] auto async_connect(stream_socket_view socket,
                                   const ip::endpoint& endpoint);

  /** Creates a DNS sender completed on the context run loop. */
  [[nodiscard]] auto async_resolve(bnio::async_io::dns_query query,
                                   bnio::async_io::dns_result_view result);

  /** Creates a DNS sender from host and service strings. */
  [[nodiscard]] auto async_resolve(std::string_view host,
                                   std::string_view service,
                                   bnio::async_io::dns_result_view result);

  /**
   * Runs posted work and readiness completions until stopped.
   *
   * Once the context has reached finished, calling run() again is not
   * supported; a caller that races a completed run loop past its lifetime
   * is outside the contract.
   */
  void run() noexcept;

  /** Requests run-loop termination. */
  int stop() noexcept;

  /** Returns whether this context is running on the current thread. */
  [[nodiscard]] bool is_in_context() const noexcept;

  /**
   * Selects externally owned shared state for a worker group.
   *
   * A null pointer selects this context's single-threaded local queues. The
   * state must remain valid until this context stops running.
   */
  void set_global_state(kqueue_task_queue_state* state) noexcept;

  /**
   * Returns the shared task queue state this worker is bound to, or null
   * when it has not entered run() yet.
   */
  [[nodiscard]] kqueue_task_queue_state* get_global_state() const noexcept {
    return global_state_;
  }

  /** Wakes one run-loop waiter. */
  void notify_one_waiter() noexcept;

  /** Returns whether this run-loop worker has published a sleeping state. */
  [[nodiscard]] bool is_waiting() const noexcept;

  /**
   * Returns the negative errno that aborted enter_run(), or 0 when the
   * last run() entered its loop normally (or never ran).
   *
   * The kqueue backend always returns 0: its enter_run() has no failure
   * point past io_context::run_native_loop()'s is_open() pre-check, so
   * a kqueue run never needs to report a failed enter. The query exists
   * to give both native backends the same run() error surface.
   */
  [[nodiscard]] int enter_run_error() const noexcept { return 0; }

  /**
   * Posts an operation to the context run loop.
   *
   * Takes the worker-local fast path when the publisher is this run loop's
   * own thread (or in standalone mode); every other publication goes to
   * the shared MPSC CPU queue and wakes a worker.
   *
   * Internal submission API. This function does not gate on the shared
   * life_state; it always enqueues and never refuses work. Lifecycle
   * gating is the caller's job: the io_context layer refuses submissions
   * while stopping so nothing strands, and direct callers must guarantee
   * the context keeps running until the operation reaches a terminal
   * receiver call.
   *
   * @see bnio::io_context::publish_cpu
   */
  int post(kqueue_operation_base& operation) noexcept;

  /**
   * Publishes I/O for passive preparation by the context run loop.
   *
   * Takes the worker-local fast path when the publisher is this run loop's
   * own thread (or in standalone mode); every other publication goes to
   * the shared MPSC I/O queue and wakes a worker.
   *
   * Internal submission API. This function does not gate on the shared
   * life_state; it always enqueues and never refuses work. Lifecycle
   * gating is the caller's job: the io_context layer refuses submissions
   * while stopping so nothing strands, and direct callers must guarantee
   * the context keeps running until the operation reaches a terminal
   * receiver call.
   *
   * @see bnio::io_context::publish_io
   */
  void publish_io(kqueue_io_operation_base& operation) noexcept;

  /**
   * Returns the worker-local task queue state.
   *
   * The state is linked into the shared task queue state's suspend list
   * while the worker sleeps in the native poller. Its CPU queue is owned
   * exclusively by the worker thread (no stealing).
   */
  [[nodiscard]] kqueue_local_task_queue_state* local_state() noexcept {
    return &local_state_;
  }

  /**
   * Fetches a batch of CPU tasks, trying the local queue first, then the
   * shared queue. Stops at the first source that yields tasks so work never
   * accumulates on this thread's stack.
   */
  [[nodiscard]] kqueue_operation_base* fetch_cpu_task() noexcept;

 private:
  struct operation_queue {
    void push(kqueue_operation_base& operation) noexcept;

    void push(kqueue_operation_base* operations) noexcept;

    [[nodiscard]] kqueue_operation_base* pop_all() noexcept;

    kqueue_operation_base* head = nullptr;
  };

  /** Unregisters this worker's local state from the shared list. */
  void unregister_local_state() noexcept;

  /** Lifecycle state for the context run loop. */
  enum class context_state {
    running,
    finishing,
    finished,
  };

  /** Next action selected by the run loop. */
  enum class run_phase {
    run_ready_tasks,
    wait_for_work,
    finish_drain,
    finished,
  };

  /** Applies configuration from options to context member variables. */
  void apply_context_options(const kqueue_context_options& options) noexcept;
  /** Verifies in debug builds that the context is running. */
  void assert_running() const noexcept;

  /**
   * Initialises run-loop state and returns whether the worker may proceed.
   *
   * On failure the caller must exit early after restoring current_context_.
   */
  [[nodiscard]] bool enter_run() noexcept;

  /**
   * Registers one wake-channel read fd with the kqueue as
   * EVFILT_READ | EV_ADD | EV_CLEAR. EV_CLEAR makes the event edge-triggered
   * so it re-fires only on the next write after the channel is drained;
   * `udata` tags the event so process_event() filters it out of operation
   * dispatch. The control result is intentionally ignored.
   */
  void register_wake_poll(int fd, void* udata) noexcept;

  /**
   * Repeatedly drains timer expirations and local CPU tasks until the
   * CPU queue is empty. Used by finish() to drain abort-generated tasks.
   */
  void drain_local_cpu_tasks() noexcept;

  /**
   * Publishes all operations from a local queue to the shared CPU-task
   * queue.
   */
  void push_cpu_tasks(operation_queue& operations) noexcept;
  /** Fetches and executes a batch of CPU tasks. Returns true if work ran. */
  [[nodiscard]] bool run_cpu_batch() noexcept;

  /**
   * Consumes staged I/O tasks after ready CPU work, trying the worker's
   * local queue first and the shared queue only when the local one is
   * empty.
   *
   * Once a stop or close has been requested, the teardown guard at the top
   * of this function routes every popped operation through
   * drain_io_list_complete_stopped() instead of registering it: EV_ADD
   * still succeeds while the kqueue fd is open, so an operation published
   * while finish() drains its queues would otherwise be armed for real,
   * and a receiver that keeps republishing ready I/O would starve the
   * finish() break condition. This implements the not-yet-executed
   * queued-work rule of io_context::stop().
   */
  [[nodiscard]] bool consume_io_tasks() noexcept;

  /** Adds an I/O operation to the inflight doubly-linked list. */
  void add_inflight(kqueue_io_operation_base& operation) noexcept;

  /** Removes an I/O operation from the inflight doubly-linked list. */
  void remove_inflight(kqueue_io_operation_base& operation) noexcept;

  /** Aborts all inflight I/O operations during shutdown. */
  void abort_inflight_io() noexcept;

  /** Completes a linked list of unregistered I/O operations as stopped and
   *  pushes them to the local CPU queue. */
  void drain_io_list_complete_stopped(kqueue_io_operation_base* head) noexcept;

  /**
   * Prepares and registers one I/O operation with kqueue.
   *
   * @return true if the operation was registered and added to the inflight
   *         list; false if it completed immediately (preparation or
   *         registration failure, or a nop task), in which case the caller
   *         must push the operation to the CPU queue.
   */
  [[nodiscard]] bool prepare_and_register_operation(
      kqueue_io_operation_base& operation) noexcept;

  /** Moves due passive-timer completions into the local CPU queue. */
  [[nodiscard]] bool consume_timeout_operations() noexcept;

  /**
   * Prepares one I/O operation through kqueue_helper and returns the
   * helper's error, or -EINVAL when no native action was selected.
   */
  [[nodiscard]] int prepare_io(kqueue_io_operation_base& operation) noexcept;

  /**
   * Publishes the sleeping state and links the worker into the shared
   * suspend list before a blocking wait.
   */
  void begin_wait() noexcept;
  /** Unlinks the worker from the suspend list and clears the sleeping
   *  state after a wait. */
  void end_wait() noexcept;

  /**
   * Wakes a sleeping run loop through the shared wake channel (guarded by
   * the submit lock), falling back to this context's EVFILT_USER
   * NOTE_TRIGGER in standalone mode.
   */
  [[nodiscard]] int trigger_wakeup() noexcept;
  /** Returns the sentinel udata tagging shared wake-channel events. */
  [[nodiscard]] static void* wakeup_user_data() noexcept;
  /** Returns the sentinel udata tagging this context's own local wakeup
   *  events. */
  [[nodiscard]] static void* local_wakeup_user_data() noexcept;

  /** Kind of a kevent udata pointer, used to dispatch collected events. */
  enum class event_udata_kind {
    shared_wake,
    local_wake,
    operation,
  };

  /** Classifies a kevent udata value into a dispatch kind. */
  [[nodiscard]] static event_udata_kind classify_udata(void* udata) noexcept;

  /** Runs ready kevents, CPU tasks, and due timers. */
  [[nodiscard]] run_phase handle_run_ready_tasks() noexcept;
  /** Waits for work when no tasks are immediately ready. */
  [[nodiscard]] run_phase handle_wait_for_work() noexcept;
  /** Drains remaining work during shutdown. */
  [[nodiscard]] run_phase handle_finish_drain() noexcept;
  /** Spins briefly for readiness events or posted tasks before blocking. */
  [[nodiscard]] run_phase spin_for_work() noexcept;
  /** Waits for readiness events from the kqueue. */
  [[nodiscard]] run_phase wait_for_io_work() noexcept;

  /**
   * Fetches due timer operations from the shared heap and computes the wait
   * timeout for the native poller.
   *
   * @param[out] deadline        Timer deadline fetched from the shared heap.
   * @param[out] timeout         Filled with the remaining time until deadline.
   * @param[out] timeout_pointer Points at @p timeout when a deadline was
   *                             fetched; null for an unbounded wait.
   * @return true if work was found (timer operations pushed, a CPU batch ran,
   *         I/O was consumed, or finish was requested) and the caller should
   *         leave the wait.
   */
  [[nodiscard]] bool compute_io_wait_timeout(
      async_io::time_point& deadline, timespec& timeout,
      const timespec*& timeout_pointer) noexcept;

  /** Returns whether the shared state has requested closing. */
  [[nodiscard]] bool closing_requested() const noexcept;
  /** Returns whether stop() has been called on this context. */
  [[nodiscard]] bool stop_requested() const noexcept;
  /** Returns whether the context should leave the running state. */
  [[nodiscard]] bool should_finish() const noexcept;
  /**
   * Drains ready events, CPU tasks, timer aborts, and inflight I/O, then
   * marks the context finished. Every operation already published reaches
   * a terminal receiver call before the run loop exits.
   */
  void finish() noexcept;

  /** Collects ready kevents and dispatches their tasks. Returns true if
   *  work ran; when @p wait is set, blocks for the first event using
   *  @p timeout (null for an unbounded wait). */
  [[nodiscard]] bool collect_ready_events(
      bool wait, const timespec* timeout = nullptr) noexcept;
  /** Collects ready kevents into an operation queue and returns the task
   *  count. */
  [[nodiscard]] unsigned collect_event_tasks(operation_queue& event_tasks,
                                             bool wait,
                                             const timespec* timeout) noexcept;
  /** Dispatches collected event tasks locally or through the shared CPU
   *  queue. */
  void dispatch_event_tasks(operation_queue& event_tasks,
                            unsigned task_count) noexcept;
  /** Handles one collected kevent: wake-channel drains are consumed and
   *  operation events resolve through their registration node. Returns
   *  true when an operation task was produced. */
  [[nodiscard]] bool process_event(const bnio::base::event& event,
                                   operation_queue& tasks) noexcept;
  /**
   * Drains the wake channel identified by `udata`.
   *
   * @return true if `udata` names a wake channel (shared or per-worker) and
   *         the event carries no operation; false for operation udata.
   */
  [[nodiscard]] bool drain_wake_channel(void* udata) noexcept;
  /**
   * Finds the registration node of `operation` matching `filter`, or null.
   */
  [[nodiscard]] kqueue_registration_state* find_fired_node(
      kqueue_io_operation_base& operation, std::int16_t filter) noexcept;
  /**
   * Resolves the result of a fired event on `operation`/`node` (poll mask,
   * kevent errno, write EOF, or the native I/O step with EAGAIN retry).
   * @return true if the operation should be completed; false if it was
   *         re-armed after EAGAIN and must stay inflight.
   */
  [[nodiscard]] bool dispatch_event_result(
      kqueue_io_operation_base& operation, kqueue_registration_state& node,
      const bnio::base::event& event) noexcept;
  /**
   * Performs the native I/O step and returns whether the operation
   * completed. A false return means the operation was re-armed after
   * EAGAIN and must stay inflight.
   */
  [[nodiscard]] bool perform_io_step(kqueue_io_operation_base& operation,
                                     kqueue_registration_state& node) noexcept;
  /** Resolves a fired write event: kevent errno, EOF, or the I/O step. */
  [[nodiscard]] bool dispatch_write_result(
      kqueue_io_operation_base& operation, kqueue_registration_state& node,
      const bnio::base::event& event) noexcept;
  /** Resolves a fired read (or fallback) event: kevent errno or the I/O
   *  step. */
  [[nodiscard]] bool dispatch_read_result(
      kqueue_io_operation_base& operation, kqueue_registration_state& node,
      const bnio::base::event& event) noexcept;

  /** Links every node of the operation into its (ident, filter) wait
   *  queue, rolling back on failure. */
  [[nodiscard]] int register_operation(
      kqueue_io_operation_base& operation) noexcept;
  /** Arms one node's kevent (EV_ADD | EV_RECEIPT) and marks it armed. */
  [[nodiscard]] int arm_registration(kqueue_registration_state& node) noexcept;
  /**
   * Arms the first armable node starting at `candidate`, looping past any
   * node whose arming fails (each such node's operation is failed and
   * detached). `candidate` is the successor of a node that just left its
   * wait queue.
   */
  void arm_queue_head(kqueue_registration_state* candidate) noexcept;
  /** Disarms and unlinks every node of the operation. */
  void unregister_operation(kqueue_io_operation_base& operation) noexcept;
  /** Translates a fired kevent into the subset of the requested poll mask
   *  it satisfies. */
  [[nodiscard]] unsigned poll_result(
      unsigned poll_mask, const bnio::base::event& event) const noexcept;

  /** Attempts to rearm a node after EAGAIN/EWOULDBLOCK.
   *
   * @return true if rearm succeeded (caller should not complete the operation),
   *         false if rearm failed and operation.result has been set.
   */
  [[nodiscard]] bool try_rearm_operation(
      kqueue_io_operation_base& operation,
      kqueue_registration_state& node) noexcept;

  /** @brief Wait-queue helpers backed by the inflight list (no allocation).
   *
   * The wait queues live entirely inside the registration nodes embedded in
   * inflight operations. Finding a queue tail is a linear scan of the
   * inflight list; unlinking a node is O(1) via the doubly-linked wait
   * pointers.
   */
  [[nodiscard]] kqueue_registration_state* find_queue_tail(
      std::uintptr_t ident, std::int16_t filter) const noexcept;
  [[nodiscard]] int append_node(kqueue_registration_state& node) noexcept;
  kqueue_registration_state* unlink_node(
      kqueue_registration_state& node) noexcept;
  /** Tears down an operation whose arming failed: removes all its nodes
   *  (no re-arm), removes it from inflight, and completes it with `result`. */
  void fail_operation(kqueue_io_operation_base& operation, int result) noexcept;

  /** Run-loop lifecycle and run flags. */
  struct run_state {
    /** Overall lifecycle state (running / finishing / finished). */
    std::atomic<context_state> state{context_state::finished};
    /** Whether a run loop is active on this context. */
    std::atomic_bool run_active{false};
    /** Whether this worker has published a sleeping state. */
    std::atomic_bool waiting{false};
    /** Whether queue_init() has completed. */
    bool queue_initialized = false;
  };

  /** Scheduling cursors and sequence counters. */
  struct scheduling_state {
    /** Remaining local inline-completion budget for this round. */
    unsigned local_task_budget = 0;
    /** Monotonic registration sequence; mirrors wait-queue insertion order. */
    std::uint64_t next_registration_sequence = 0;
  };

  bnio::base::kqueue queue_;
  kqueue_context_options options_{};
  run_state run_state_;
  scheduling_state scheduling_state_;

  std::unique_ptr<bnio::base::event[]> event_buffer_;

  static thread_local kqueue_context* current_context_;
  kqueue_task_queue_state* global_state_ = nullptr;
  kqueue_local_task_queue_state local_state_;
  kqueue_io_operation_base* inflight_io_head_ = nullptr;
};

}  // namespace bnio::async_io::bsd_native

#endif  // BNIO_ASYNC_IO_BSD_KQUEUE_CONTEXT_BASE_CONTEXT_H_
