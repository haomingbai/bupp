/**
 * @file class.h
 * @brief io_context class declaration.
 */

#pragma once
#ifndef BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#define BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_

#include <bnio/async_io/random_access_file.h>
#include <bnio/async_io/socket_view.h>
#include <bnio/async_io/time.h>
#include <bnio/buffer.h>
#include <bnio/detail/posix/io_context/native_context.h>
#include <bnio/detail/posix/io_context/options.h>
#include <bnio/detail/posix/io_context/steady_timer.h>
#include <bnio/detail/posix/io_context/timer_types.h>
#include <bnio/export.h>
#include <bnio/io_context_cpo.h>
#include <bnio/ip.h>

#include <atomic>
#include <bexec/completion_signatures.hpp>
#include <bexec/query.hpp>
#include <bexec/receiver.hpp>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>

#include "bnio/async_io/dns/query.h"

namespace bnio {

enum class ssl_handshake_type;

namespace detail {
class stream_file_write_all_state;
class random_access_write_all_state;
class socket_write_all_state;
template <class Request, class Control, class Receiver>
class native_io_operation;
template <class Receiver>
class native_poll_operation;
template <class Receiver>
class resolve_operation;
}  // namespace detail

/**
 * High-level asynchronous I/O context for the configured native backend.
 *
 * io_context is the event-loop owner and scheduler factory:
 *
 * 1. Event loop host — run() drives the selected io_uring or kqueue loop;
 *    each thread calling run() creates its own native context.
 * 2. Scheduler factory — produces dispatch and post schedulers.
 * 3. Passive I/O backend — publishes scheduler I/O to the running worker's
 *    own queue or, for every other producer, to the shared queue that a
 *    worker drains on its owning native-context thread.
 *
 * io_context adapts the non-owning async_io views into sender-returning
 * operations. Higher-level stream owners build on top of these view-level
 * operations instead of being known by io_context.
 *
 * The context is non-movable: it owns native workers, the timer heap, and
 * its synchronization resources, and it must outlive every operation
 * submitted on it.
 */
class BNIO_EXPORT io_context {
 public:
  /** I/O operation passively consumed by one native run-loop worker. */
  using operation_base = detail::native_io_operation_base;

  /**
   * Monotonic clock used by this context.
   */
  using steady_clock = async_io::steady_clock;

  /**
   * Default clock used by this context.
   */
  using clock = async_io::clock;

  /**
   * Default duration type used by this context.
   */
  using duration = async_io::duration;

  /**
   * Default time point type used by this context.
   */
  using time_point = async_io::time_point;

  /**
   * Scheduling policy used by io_context scheduler handles.
   */
  enum class schedule_kind {
    /**
     * Complete schedule() inline when start() runs on the context thread.
     */
    dispatch,

    /**
     * Always post schedule() completion through the context run loop.
     */
    post,
  };

  /**
   * Sender returned by io_context schedulers' schedule() member.
   */
  template <schedule_kind Kind>
  class schedule_sender {
   public:
    /**
     * Completion signatures produced by the scheduler sender.
     */
    using completion_signatures =
        bexec::completion_signatures<bexec::set_value_t(std::error_code),
                                     bexec::set_stopped_t()>;

    /**
     * Creates a schedule sender bound to context.
     */
    explicit schedule_sender(io_context& context) noexcept
        : context_(&context) {}

    /**
     * Operation state for a scheduler schedule() sender.
     */
    template <class Receiver>
    class operation : public detail::native_operation_base {
     public:
      /**
       * Creates an operation bound to the context and receiver.
       */
      operation(io_context& context, Receiver receiver)
          : context_(&context), receiver_(std::move(receiver)) {}

      /**
       * Copy construction is disabled because operations are queued
       * intrusively.
       */
      operation(const operation&) = delete;

      /**
       * Copy assignment is disabled because operations are queued intrusively.
       */
      operation& operator=(const operation&) = delete;

      /**
       * Move construction is disabled because operations are queued
       * intrusively.
       */
      operation(operation&&) = delete;

      /**
       * Move assignment is disabled because operations are queued intrusively.
       */
      operation& operator=(operation&&) = delete;

      /**
       * Starts the schedule operation.
       */
      void start() noexcept {
        if constexpr (Kind == schedule_kind::dispatch) {
          if (context_->is_in_context()) {
            complete();
            return;
          }
        }

        // The submission critical section (locked state check + enqueue)
        // is ordered against stop()'s state transition by the submit lock.
        // If the context is already stopping, publish_cpu() rejects the
        // enqueue and we complete inline so the operation never strands.
        if (!context_->publish_cpu(*this)) {
          complete();
        }
      }

      /**
       * Delivers the schedule completion.
       */
      void execute() noexcept override { complete(); }

     private:
      void complete() noexcept {
        // Abort delivery is decided by token arbitration at the observation
        // point: a cancelled receiver stop token wins and delivers
        // set_stopped(); without token cancellation, an io_context::stop()
        // abort delivers set_value(operation_canceled); a normal completion
        // delivers set_value({}) with the real result untouched.
        auto env = bexec::get_env(receiver_);
        auto token = bexec::query(env, bexec::get_stop_token);
        if (token.stop_requested()) {
          bexec::set_stopped(std::move(receiver_));
          return;
        }
        if (context_->is_stopped()) {
          bexec::set_value(std::move(receiver_),
                           std::make_error_code(std::errc::operation_canceled));
          return;
        }
        bexec::set_value(std::move(receiver_), std::error_code{});
      }

      io_context* context_;
      Receiver receiver_;
    };

    template <class Receiver>
    /**
     * Connects the schedule sender to a receiver.
     */
    [[nodiscard]] auto connect(Receiver receiver) const {
      return operation<std::remove_cvref_t<Receiver>>(*context_,
                                                      std::move(receiver));
    }

   private:
    io_context* context_;
  };

  /**
   * Copyable scheduler handle produced by io_context.
   */
  template <schedule_kind Kind>
  class basic_scheduler {
   public:
    /**
     * Concrete sender type returned by schedule().
     */
    using schedule_sender_type = schedule_sender<Kind>;

    /**
     * Copies a scheduler handle.
     */
    basic_scheduler(const basic_scheduler&) noexcept = default;

    /**
     * Assigns a scheduler handle.
     */
    basic_scheduler& operator=(const basic_scheduler&) noexcept = default;

    /**
     * Returns a sender that completes according to this scheduler's policy.
     */
    [[nodiscard]] schedule_sender_type schedule() const noexcept {
      return schedule_sender_type(*context_);
    }

    /**
     * Returns the context that owns this scheduler.
     */
    [[nodiscard]] io_context& context() const noexcept { return *context_; }

    /**
     * Creates a sender that reads the whole buffer from a socket.
     */
    [[nodiscard]] auto async_read(async_io::stream_socket_view socket,
                                  mutable_buffer buffer, int flags = 0) const;

    /**
     * Creates a sender for one socket read operation.
     */
    [[nodiscard]] auto async_read_some(async_io::stream_socket_view socket,
                                       mutable_buffer buffer,
                                       int flags = 0) const;

    /**
     * Creates a sender that writes the whole buffer to a socket.
     */
    [[nodiscard]] auto async_write(async_io::stream_socket_view socket,
                                   const_buffer buffer, int flags = 0) const;

    /**
     * Creates a sender for one socket write operation without retrying short
     * writes.
     */
    [[nodiscard]] auto async_write_some(async_io::stream_socket_view socket,
                                        const_buffer buffer,
                                        int flags = 0) const;

    /**
     * Creates a sender that performs one datagram receive on a connected
     * socket.
     */
    [[nodiscard]] auto async_receive(async_io::datagram_socket_view socket,
                                     mutable_buffer buffer,
                                     int flags = 0) const;

    /**
     * Creates a sender that performs one datagram send on a connected
     * socket.
     */
    [[nodiscard]] auto async_send(async_io::datagram_socket_view socket,
                                  const_buffer buffer, int flags = 0) const;

    /**
     * Creates a sender that performs one datagram receive and stores the
     * source endpoint into @p endpoint.
     */
    [[nodiscard]] auto async_receive_from(async_io::datagram_socket_view socket,
                                          mutable_buffer buffer,
                                          ip::endpoint& endpoint,
                                          int flags = 0) const;

    /**
     * Creates a sender that performs one datagram send to @p endpoint,
     * without requiring a connected socket.
     */
    [[nodiscard]] auto async_send_to(async_io::datagram_socket_view socket,
                                     const_buffer buffer,
                                     const ip::endpoint& endpoint,
                                     int flags = 0) const;
    /**
     * Creates a sender that reads the whole buffer from a descriptor,
     * advancing the kernel file position.
     */
    [[nodiscard]] auto async_read(async_io::descriptor_view descriptor,
                                  mutable_buffer buffer) const;

    /**
     * Creates a sender for one streaming descriptor read operation.
     */
    [[nodiscard]] auto async_read_some(async_io::descriptor_view descriptor,
                                       mutable_buffer buffer) const;

    /**
     * Creates a sender that writes the whole buffer to a descriptor,
     * advancing the kernel file position.
     */
    [[nodiscard]] auto async_write(async_io::descriptor_view descriptor,
                                   const_buffer buffer) const;

    /**
     * Creates a sender for one streaming descriptor write operation without
     * retrying short writes.
     */
    [[nodiscard]] auto async_write_some(async_io::descriptor_view descriptor,
                                        const_buffer buffer) const;

    /**
     * Creates a sender that reads the whole buffer from a random access
     * file at an explicit offset.
     */
    [[nodiscard]] auto async_read(async_io::random_access_file file,
                                  mutable_buffer buffer,
                                  std::uint64_t offset) const;

    /**
     * Creates a sender for one random access read operation at an explicit
     * offset.
     */
    [[nodiscard]] auto async_read_some(async_io::random_access_file file,
                                       mutable_buffer buffer,
                                       std::uint64_t offset) const;

    /**
     * Creates a sender that writes the whole buffer to a random access file
     * at an explicit offset.
     */
    [[nodiscard]] auto async_write(async_io::random_access_file file,
                                   const_buffer buffer,
                                   std::uint64_t offset) const;

    /**
     * Creates a sender for one random access write operation at an explicit
     * offset without retrying short writes.
     */
    [[nodiscard]] auto async_write_some(async_io::random_access_file file,
                                        const_buffer buffer,
                                        std::uint64_t offset) const;

    /**
     * Creates a sender that accepts one connection.
     */
    [[nodiscard]] auto async_accept(async_io::stream_socket_view socket,
                                    int flags = 0) const;

    /**
     * Creates a sender that connects a socket to an endpoint.
     */
    [[nodiscard]] auto async_connect(async_io::stream_socket_view socket,
                                     const ip::endpoint& endpoint) const;

    /**
     * Creates a sender that waits for descriptor events.
     */
    [[nodiscard]] auto async_poll(async_io::descriptor_view descriptor,
                                  unsigned poll_mask) const;

    /**
     * Creates a sender that resolves a DNS query into caller-provided storage.
     */
    [[nodiscard]] auto async_resolve(async_io::dns_query query,
                                     async_io::dns_result_view result) const;

    /**
     * Creates a sender that resolves a host and service into caller-provided
     * storage.
     */
    [[nodiscard]] auto async_resolve(std::string_view host,
                                     std::string_view service,
                                     async_io::dns_result_view result) const;

    /**
     * Compares whether two scheduler handles refer to the same context.
     */
    friend bool operator==(basic_scheduler lhs, basic_scheduler rhs) noexcept {
      return lhs.context_ == rhs.context_;
    }

   private:
    friend class io_context;

    explicit basic_scheduler(io_context& context) noexcept
        : context_(&context) {}

    io_context* context_;
  };

  /**
   * Sender returned by io_context::join().
   */
  class join_sender {
   public:
    /** Completes with set_value() when the context has fully stopped. */
    using completion_signatures =
        bexec::completion_signatures<bexec::set_value_t()>;

    /** Creates a join sender bound to @p context. */
    explicit join_sender(io_context& context) noexcept : context_(&context) {}

    /**
     * Operation state that stops the context and waits for it to finish.
     */
    template <class Receiver>
    class operation {
     public:
      /** Creates an operation bound to the context and receiver. */
      operation(io_context& context, Receiver receiver)
          : context_(&context), receiver_(std::move(receiver)) {}

      operation(const operation&) = delete;
      operation& operator=(const operation&) = delete;
      operation(operation&&) = delete;
      operation& operator=(operation&&) = delete;

      /**
       * Stops the context on the elected thread, or waits for the already
       * stopping thread, then completes the receiver with set_value().
       */
      void start() noexcept {
        if (context_->begin_stop()) {
          // This thread is responsible for stopping.
          context_->stop_internal();
          context_->lifecycle_.stopped.store(true, std::memory_order_release);
          bexec::set_value(std::move(receiver_));
        } else {
          // Another thread is already stopping or the context is stopped.
          while (
              !context_->lifecycle_.stopped.load(std::memory_order_acquire)) {
            std::this_thread::yield();
          }
          bexec::set_value(std::move(receiver_));
        }
      }

     private:
      io_context* context_;
      Receiver receiver_;
    };

    /**
     * Connects the join sender to @p receiver and returns the operation
     * state.
     */
    template <class Receiver>
    [[nodiscard]] auto connect(Receiver receiver) const {
      return operation<std::remove_cvref_t<Receiver>>(*context_,
                                                      std::move(receiver));
    }

   private:
    io_context* context_;
  };

  /**
   * Scheduler with Asio dispatch-like schedule() semantics.
   */
  using dispatch_scheduler = basic_scheduler<schedule_kind::dispatch>;

  /**
   * Scheduler with Asio post-like schedule() semantics.
   */
  using post_scheduler = basic_scheduler<schedule_kind::post>;

  /**
   * Creates a context with default options.
   */
  io_context() noexcept;

  /**
   * Creates a context with explicit options.
   */
  explicit io_context(const io_context_options& options) noexcept;

  /** Releases worker, timer, and native context resources. */
  ~io_context() noexcept;

  /**
   * Copy construction is disabled because the context owns a native context
   * and synchronization resources.
   */
  io_context(const io_context&) = delete;

  /**
   * Copy assignment is disabled because the context owns a native context
   * and synchronization resources.
   */
  io_context& operator=(const io_context&) = delete;

  /**
   * Move construction is disabled because the context owns synchronization
   * resources and timer state.
   */
  io_context(io_context&&) = delete;

  /**
   * Move assignment is disabled because the context owns synchronization
   * resources and timer state.
   */
  io_context& operator=(io_context&&) = delete;

  /**
   * Returns whether the context is open.  The POSIX backend is selected
   * at compile time, so this always returns true; runtime availability of
   * the native backend is probed lazily by run(), which reports failure
   * through its returned error_code.
   */
  [[nodiscard]] bool is_open() const noexcept;

  /**
   * Runs the context event loop on the calling thread until the context
   * stops. Each call to run() creates one native context for this thread.
   *
   * @return An empty error_code for a normal run, operation_canceled when
   *         the context stopped (including a stop requested before or
   *         during this call), or the backend's error as
   *         std::error_code(-errno, std::generic_category()) when the
   *         native run loop could not be entered (for example the ring
   *         could not be enabled or a wake poll could not be armed).
   *         Whatever was already published when the enter failed is still
   *         delivered to its receivers before run() returns.
   */
  std::error_code run() noexcept;

  /**
   * Requests the context event loop to stop.
   *
   * Work published before the stopping state was elected still completes.
   * Inflight I/O and not-yet-executed queued work is aborted and reports
   * set_value(operation_canceled), and so does every pending timer wait;
   * a cancelled receiver stop token wins and reports set_stopped()
   * instead. The elected stopping thread drains the shared queues, so no
   * published work is silently dropped.
   */
  int stop() noexcept;

  /**
   * Returns a sender that stops the context (if not already done) and waits
   * for it to fully finish.  If multiple threads call join(), exactly one
   * performs the actual stop and the rest spin-wait.
   *
   * The sender completes with set_value() when all workers have exited.
   */
  [[nodiscard]] join_sender join() noexcept;

  /**
   * Returns whether the current thread is running this context.
   */
  [[nodiscard]] bool is_in_context() const noexcept;

  /**
   * Returns whether stop() has been requested for this context.
   *
   * schedule_sender's completion arbitration consults this after the
   * receiver's stop token: a cancelled token delivers set_stopped(), a
   * stopped context without token cancellation delivers
   * set_value(operation_canceled), and otherwise the real result is
   * delivered untouched.
   */
  [[nodiscard]] bool is_stopped() const noexcept {
    return global_state_.life_state.load(std::memory_order_acquire) != 0 ||
           lifecycle_.stopped.load(std::memory_order_acquire);
  }

  /**
   * Returns whether eager immediate I/O completion is enabled for this
   * context.
   *
   * Set once at construction from io_context_options::enable_immediate_io;
   * immutable afterwards, so native I/O operations can read it in start()
   * as a single non-atomic bool load.
   */
  [[nodiscard]] bool enable_immediate_io() const noexcept {
    return immutable_flags_.enable_immediate_io;
  }

  /**
   * Returns a scheduler whose schedule() may complete inline on the context
   * thread.
   */
  [[nodiscard]] dispatch_scheduler get_dispatch_scheduler() noexcept;

  /**
   * Returns a scheduler whose schedule() always posts through the context loop.
   */
  [[nodiscard]] post_scheduler get_post_scheduler() noexcept;

 private:
  friend class steady_timer;
  friend class detail::timer_operation_base;
  friend class detail::stream_file_write_all_state;
  friend class detail::random_access_write_all_state;
  friend class detail::socket_write_all_state;
  template <class Receiver>
  friend class detail::timer_wait_operation;
  template <class Request, class Control, class Receiver>
  friend class detail::native_io_operation;
  template <class Receiver>
  friend class detail::native_poll_operation;
  template <class Receiver>
  friend class detail::resolve_operation;
  friend class join_sender;

  /**
   * Creates a sender that reads bytes from a non-owning stream socket
   * view and completes with bytes transferred.
   */
  [[nodiscard]] auto async_read(async_io::stream_socket_view socket,
                                mutable_buffer buffer, int flags = 0);

  /**
   * Creates a sender for one socket read operation through a non-owning
   * stream socket view.
   */
  [[nodiscard]] auto async_read_some(async_io::stream_socket_view socket,
                                     mutable_buffer buffer, int flags = 0);

  /**
   * Creates a sender that writes the whole buffer through a non-owning
   * stream socket view.
   */
  [[nodiscard]] auto async_write(async_io::stream_socket_view socket,
                                 const_buffer buffer, int flags = 0);

  /**
   * Creates a sender for one write operation through a non-owning
   * stream socket view.
   */
  [[nodiscard]] auto async_write_some(async_io::stream_socket_view socket,
                                      const_buffer buffer, int flags = 0);

  /**
   * Creates a sender that reads bytes from a file descriptor, advancing the
   * kernel file position.
   */
  [[nodiscard]] auto async_read(async_io::descriptor_view descriptor,
                                mutable_buffer buffer);

  /**
   * Creates a sender for one streaming descriptor read operation.
   */
  [[nodiscard]] auto async_read_some(async_io::descriptor_view descriptor,
                                     mutable_buffer buffer);

  /**
   * Creates a sender that writes the whole buffer to a file descriptor,
   * advancing the kernel file position.
   */
  [[nodiscard]] auto async_write(async_io::descriptor_view descriptor,
                                 const_buffer buffer);

  /**
   * Creates a sender for one streaming write operation to a file descriptor.
   */
  [[nodiscard]] auto async_write_some(async_io::descriptor_view descriptor,
                                      const_buffer buffer);

  /**
   * Creates a sender that reads bytes from a random access file at an
   * explicit offset.
   */
  [[nodiscard]] auto async_read(async_io::random_access_file file,
                                mutable_buffer buffer, std::uint64_t offset);

  /**
   * Creates a sender for one random access read operation at an explicit
   * offset.
   */
  [[nodiscard]] auto async_read_some(async_io::random_access_file file,
                                     mutable_buffer buffer,
                                     std::uint64_t offset);

  /**
   * Creates a sender that writes the whole buffer to a random access file at
   * an explicit offset.
   */
  [[nodiscard]] auto async_write(async_io::random_access_file file,
                                 const_buffer buffer, std::uint64_t offset);

  /**
   * Creates a sender for one positioned write operation to a random access
   * file.
   */
  [[nodiscard]] auto async_write_some(async_io::random_access_file file,
                                      const_buffer buffer,
                                      std::uint64_t offset);

  /**
   * Creates a sender that performs one datagram receive on a connected
   * socket.
   */
  [[nodiscard]] auto async_receive(async_io::datagram_socket_view socket,
                                   mutable_buffer buffer, int flags = 0);

  /**
   * Creates a sender that performs one datagram send on a connected
   * socket.
   */
  [[nodiscard]] auto async_send(async_io::datagram_socket_view socket,
                                const_buffer buffer, int flags = 0);

  /**
   * Creates a sender that performs one datagram receive and stores the
   * source endpoint into @p endpoint.
   */
  [[nodiscard]] auto async_receive_from(async_io::datagram_socket_view socket,
                                        mutable_buffer buffer,
                                        ip::endpoint& endpoint, int flags = 0);

  /**
   * Creates a sender that performs one datagram send to @p endpoint,
   * without requiring a connected socket.
   */
  [[nodiscard]] auto async_send_to(async_io::datagram_socket_view socket,
                                   const_buffer buffer,
                                   const ip::endpoint& endpoint, int flags = 0);
  /**
   * Creates a sender that accepts one connection from a non-owning
   * listening socket view.
   */
  [[nodiscard]] auto async_accept(async_io::stream_socket_view socket,
                                  int flags = 0);

  /**
   * Creates a sender that connects a non-owning stream socket view.
   */
  [[nodiscard]] auto async_connect(async_io::stream_socket_view socket,
                                   const ip::endpoint& endpoint);

  /**
   * Creates a sender that waits for events on a file descriptor.
   */
  [[nodiscard]] auto async_poll(async_io::descriptor_view descriptor,
                                unsigned poll_mask);

  /**
   * Creates a sender that resolves a DNS query into caller-provided result
   * storage on the context run loop.
   */
  [[nodiscard]] auto async_resolve(async_io::dns_query query,
                                   async_io::dns_result_view result);

  /**
   * Creates a sender that resolves a host and service into caller-provided
   * result storage on the context run loop.
   */
  [[nodiscard]] auto async_resolve(std::string_view host,
                                   std::string_view service,
                                   async_io::dns_result_view result);

  /**
   * Publishes an operation for passive native submission by a worker.
   *
   * Takes the worker-local fast path when this thread is a worker of this
   * context (see running_worker_native()): the operation is pushed onto
   * that worker's own I/O queue without taking submit_lock, which also
   * keeps it on the worker that owns the connection. Otherwise it falls
   * through to the shared path, which mirrors the old behaviour: check the
   * shutdown state under the submit lock and, only while the context is
   * not stopping, enqueue the operation and wake a sleeping worker as
   * needed. The wake-channel write is bound to the submit lock (see
   * wake_locked), so it can never race the destructor's close.
   *
   * @return true if the operation was published; false if the context is
   *         already stopping and the operation was NOT enqueued — the
   *         caller must complete it inline (set_stopped), mirroring the
   *         abort path. Only the shared path can return false: the local
   *         path always publishes, because the publisher is the thread
   *         that will drain that queue (see running_worker_native()).
   */
  [[nodiscard]] bool publish_io(operation_base& operation) noexcept;

  /**
   * Publishes CPU work for execution by one context run-loop worker.
   *
   * Takes the worker-local fast path when this thread is a worker of this
   * context (see running_worker_native()), posting to that worker's own
   * queue without taking submit_lock. Otherwise it falls through to the
   * shared path, which checks the shutdown state under the submit lock
   * and, only while the context is not stopping, enqueues the operation
   * and wakes a sleeping worker as needed. publish_cpu assumes the publish
   * happens against a running context; the in-lock state check makes that
   * assumption explicit and atomic with the enqueue.
   *
   * @return true if the operation was published (enqueued, or posted to the
   *         worker-local queue); false if the context is already stopping
   *         and the operation was NOT enqueued — the caller must complete
   *         it inline (e.g. set_stopped) so it never strands.
   *
   * Only state-involving work (check, enqueue, wake decision) runs inside
   * the critical section; the caller executes the operation afterwards.
   */
  [[nodiscard]] bool publish_cpu(
      detail::native_operation_base& operation) noexcept;

  /**
   * Returns the native context this thread is running as a worker of, or
   * nullptr when this thread is not a worker of THIS io_context.
   *
   * Shared by the publish_io() and publish_cpu() local fast paths.
   * current_worker_native_ is only non-null while this thread is inside
   * io_context::run(), which sets it on entry and restores it on every
   * return path, so a non-null bound pointer also guarantees the native
   * context is alive for the duration of the call.
   *
   * The bound-state check matters for nested run(): an outer worker's
   * handler may post to a different context, and current_worker_native_
   * then still points at the OUTER worker's native context. Posting to
   * the wrong local queue would strand the operation — the inner run loop
   * never drains the outer worker's local queue — so both publish paths
   * fall through to the shared queue for any context other than this one.
   *
   * A non-null result is also what makes the local path safe to take
   * without the lock and without the life_state check, even while the
   * context is already stopping: the caller is the worker that will drain
   * the queue. Native finish() keeps calling consume_io_tasks() →
   * abort_inflight_io() until both I/O queues stay empty, and
   * abort_inflight_io() completes the leftovers with stopped.
   */
  [[nodiscard]] detail::native_context* running_worker_native() const noexcept;

  /** Wakes one worker only when every published worker is sleeping. */
  void wake_one_if_all_workers_sleeping() noexcept;

  // Internal wake helpers. The *_locked variants require the caller to
  // already hold global_state_.submit_lock; they are used by the publish
  // paths so the wake-channel write stays bound to the submit lock without
  // re-locking. wake_one_sleeping_locked() prefers a directed wake of a
  // single sleeping worker via its per-worker channel and falls back to the
  // shared broadcast channel when nobody is suspended. The public
  // wake_one_if_all_workers_sleeping acquires the lock itself and re-checks
  // the shutdown state first: after ~io_context has published the terminal
  // state and closed the channel under the same lock, a wake would write to
  // a closed fd.
  void wake_locked() noexcept;
  void wake_one_sleeping_locked() noexcept;

  /**
   * Returns whether a worker may enter the run loop: the context is not
   * stopping.
   */
  [[nodiscard]] bool can_start_run() const noexcept;

  /**
   * Decrements the running worker count on every run() return path.
   */
  void release_worker_slot() noexcept;

  /**
   * Drives the native run loop for one worker: constructs the native
   * context, binds it to the shared state, and blocks in ctx.run().
   */
  std::error_code run_native_loop() noexcept;

  /**
   * Registers a timer slot with this context, inserting it into the active
   * time heap or the inactive list according to its expiry.
   */
  void register_timer(detail::timer_slot& timer) noexcept;

  /**
   * Unregisters a timer slot, removing it from its heap/list container.
   * Pending waits are detached and staged on the timer-ready list with
   * canceled completion, delivering set_value(operation_canceled).
   */
  void unregister_timer(detail::timer_slot& timer) noexcept;

  /**
   * Detaches the timer's submitted head-linked queue and marks every
   * detached wait canceled. The timer's active/inactive state and expiry
   * are left unchanged.
   *
   * @return The number of canceled waits.
   */
  [[nodiscard]] std::size_t cancel_timer(detail::timer_slot& timer) noexcept;

  /**
   * Replaces the timer's expiry: removes the slot from its current
   * container, stores the new expiry, detaches the submitted waits as
   * canceled, and reinserts the slot into the active heap or the inactive
   * list according to the new time.
   *
   * @return The number of detached waits.
   */
  [[nodiscard]] std::size_t set_timer_expiry(detail::timer_slot& timer,
                                             time_point expiry) noexcept;

  /**
   * Returns the timer's stored expiry. The saved value is authoritative
   * even for an unregistered slot, which no worker or context can mutate.
   */
  [[nodiscard]] time_point timer_expiry(
      const detail::timer_slot& timer) const noexcept;

  /**
   * Starts a wait on the given timer. An active timer links the operation
   * at its submitted head; an inactive (expired) timer stages an immediate
   * completion on the timer-ready list without touching the shared CPU
   * queue.
   */
  void start_timer_wait(detail::timer_operation_base& operation,
                        detail::timer_slot& timer) noexcept;

  // *_locked variants of the timer entry points. The caller must already
  // hold timers_.mutex. Each returns whether a sleeping worker must be
  // woken (the public wrapper wakes on the caller's behalf); the entry
  // points that also return a cancellation count combine both.
  [[nodiscard]] bool register_timer_locked(detail::timer_slot& timer,
                                           time_point now) noexcept;

  [[nodiscard]] bool unregister_timer_locked(
      detail::timer_slot& timer) noexcept;

  [[nodiscard]] std::pair<std::size_t, bool> cancel_timer_locked(
      detail::timer_slot& timer) noexcept;

  [[nodiscard]] std::pair<std::size_t, bool> set_timer_expiry_locked(
      detail::timer_slot& timer, time_point expiry, time_point now) noexcept;

  [[nodiscard]] bool start_timer_wait_locked(
      detail::timer_operation_base& operation,
      detail::timer_slot& timer) noexcept;

  [[nodiscard]] bool try_fetch_timeout_operations(
      time_point& deadline,
      detail::native_operation_base*& operations) noexcept;
  [[nodiscard]] static bool try_fetch_timeout_operations_thunk(
      void* state, time_point& deadline,
      detail::native_operation_base*& operations) noexcept;

  // Stage helpers for try_fetch_timeout_operations().
  // drain_expired_timers_locked runs inside the timers_.mutex critical section;
  // reverse_ready_operations runs after it is released.
  void drain_expired_timers_locked(time_point now) noexcept;

  void reverse_ready_operations(
      detail::timer_operation_base* ready,
      detail::native_operation_base*& operations) noexcept;

  /** Aborts all pending timer waits for io_context::stop().
   *
   *  Drains every active and inactive timer slot's submitted queue, sets
   *  each operation's completion to timer_completion_kind::canceled (a
   *  context-stop abort is not token cancellation; execute() delivers it
   *  as set_value(operation_canceled)), and pushes them to timers_.ready.
   *  Called by begin_stop() BEFORE
   *  life_state is published so that workers entering finish() observe a
   *  fully populated timers_.ready — no worker can drain the ready list
   *  before the abort has staged the operations.
   */
  void abort_pending_timer_waits() noexcept;

  /**
   * Waits for every other worker to observe the stopping state and exit.
   * Timer waits were already aborted by begin_stop() before life_state
   * was published, so this function only spins on the wake channel until
   * global_state_.running_workers drops to zero (or one, when called from
   * a worker), then drains the shared queues that no worker will drain
   * (see drain_shared_queues_for_stop()).
   */
  int stop_internal() noexcept;

  /**
   * Drains and delivers the shared queues after the stopping thread has
   * waited for running_workers to reach zero.
   *
   * Concurrency contract: begin_stop() published life_state = 1 inside
   * global_state_.submit_lock, and every shared-path publish checks
   * life_state and enqueues inside the same lock. The two critical
   * sections are therefore fully serialized — an operation enqueued
   * before the store is visible to this drain, and any publish after the
   * store observes the stopping state, does not enqueue, and completes
   * inline at its caller. With running_workers at zero no concurrent
   * consumer exists either: run() increments running_workers before any
   * check, so a late run() that observes life_state != 0 exits without
   * touching the queues, and earlier workers drained the shared queues in
   * finish() before releasing their slot. The drain owns the queues
   * exclusively; the loop runs until a full pass finds every source empty
   * and then returns. On the normal multi-worker stop the last worker's
   * finish() already emptied everything, so this pass is a no-op.
   *
   * Delivery never prepares or submits SQEs/kevents: CPU operations run
   * inline (their execute() arbitration reports
   * set_value(operation_canceled) for work the context aborted), I/O
   * operations are marked stopped (result = -ECANCELED +
   * complete_submit_stopped()) and executed inline, and the timer waits
   * staged on timers_.ready by abort_pending_timer_waits() are taken
   * under timers_.mutex and executed inline. Receiver callbacks run
   * synchronously on the stopping thread.
   */
  void drain_shared_queues_for_stop() noexcept;

  /**
   * Elects this thread as the stopping thread, aborts pending timer
   * waits, and publishes the stopping state inside the submit-path lock.
   *
   * abort_pending_timer_waits() runs BEFORE taking submit_lock so that
   * the release-store of life_state happens after the abort in program
   * order.  Any worker whose acquire-load of life_state observes the
   * non-zero value is therefore guaranteed to see a fully populated
   * timers_.ready when it enters finish().
   *
   * Ordering the state transition against publish_cpu()'s
   * check-state + enqueue critical section (same lock) is what binds
   * enqueue to state: operations that passed the check are drained before
   * workers exit, and operations that see the stopping state do not
   * enqueue. The election CAS on lifecycle_.stop_requested keeps exactly one
   * stopping thread while stop() and join() share this path.
   *
   * @return true if this thread owns the stop, false if another thread
   *         already owns it (the caller must wait for lifecycle_.stopped).
   */
  [[nodiscard]] bool begin_stop() noexcept;

  /**
   * Consumes the timer's lock-protected list of submitted wait operations.
   */
  [[nodiscard]] detail::timer_operation_queue take_timer_operations_locked(
      detail::timer_slot& timer) noexcept;

  /**
   * Stages the given detached wait operations on the timer-ready list with
   * the given completion kind. The list is drained only by a native
   * worker's passive timer check (or by the stopping thread).
   */
  void enqueue_timer_operations_locked(
      detail::timer_operation_base* operations,
      detail::timer_completion_kind completion) noexcept;

  /**
   * Stages a single wait operation's completion kind onto the timer-ready
   * list.
   */
  void queue_timer_completion(
      detail::timer_operation_base& operation,
      detail::timer_completion_kind completion) noexcept;

  detail::native_task_queue_state global_state_;
  platform_io_context_options native_options_;

  /**
   * Configuration fixed once at construction and read-only afterwards.
   *
   * enable_immediate_io comes from io_context_options and is written
   * exactly once by the constructor before the context is shared with
   * other threads; it is read as a plain bool on the hot path.
   */
  struct immutable_flags {
    explicit immutable_flags(bool eager_immediate_io) noexcept
        : enable_immediate_io(eager_immediate_io) {}

    /** Eager immediate I/O switch, fixed at construction (see options.h). */
    bool enable_immediate_io = true;
  } immutable_flags_;

  /**
   * Lifecycle state shared between the run()/stop()/join() paths.
   */
  struct lifecycle_state {
    /** Stop-thread election flag. Exactly one thread wins the CAS and then
     *  publishes life_state inside the submit-path lock (see begin_stop()). */
    std::atomic<int> stop_requested{0};

    /** Signalled when the context has fully stopped. Join() waiters spin
     *  on this flag. Independent from life_state to avoid torn reads. */
    std::atomic_bool stopped{false};
  } lifecycle_;

  detail::timer_state_data timers_;

  static thread_local io_context* current_context_;
  static thread_local detail::native_context* current_worker_native_;
};

}  // namespace bnio

#include <bnio/detail/posix/io_context/native_io.h>

#endif  // BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
