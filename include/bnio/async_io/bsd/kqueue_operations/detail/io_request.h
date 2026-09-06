/**
 * @file io_request.h
 * @brief Internal kqueue I/O request types.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_BSD_KQUEUE_OPERATIONS_DETAIL_IO_REQUEST_H_
#define BNIO_ASYNC_IO_BSD_KQUEUE_OPERATIONS_DETAIL_IO_REQUEST_H_

#include <bnio/async_io/bsd/kqueue_context_base.h>

#include <bexec/completion_signatures.hpp>
#include <bexec/query.hpp>
#include <bexec/receiver.hpp>
#include <cerrno>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bnio::async_io::bsd_native::detail {

/** Returns whether the receiver's associated stop token is canceled. */
template <class Receiver>
[[nodiscard]] bool stop_requested(const Receiver& receiver) noexcept {
  auto environment = bexec::get_env(receiver);
  auto token = bexec::query(environment, bexec::get_stop_token);
  return token.stop_requested();
}

/** Returns whether the result is a transient would-block (-EAGAIN or
 *  -EWOULDBLOCK) requiring another readiness round. */
[[nodiscard]] inline bool should_wait(int result) noexcept {
  return result == -EAGAIN || result == -EWOULDBLOCK;
}

/** Returns whether the operation must wait for readiness after @p result.
 *  Prefers the request's own should_wait() when it provides one; otherwise
 *  falls back to the errno-based check. */
template <class Request>
[[nodiscard]] bool should_wait(Request& request, int result) noexcept {
  if constexpr (requires { request.should_wait(result); }) {
    return request.should_wait(result);
  } else {
    return should_wait(result);
  }
}

/** Concept that detects whether a request can perform an immediate nonblocking
 *  I/O call before registering with kqueue for readiness. */
template <class Request>
concept has_start_io = requires(Request& req) {
  { req.start_io() } -> std::convertible_to<int>;
};

/** Operation for a request whose native call is attempted before readiness.
 *
 *  Per completion-semantics contract:
 *  - `stopped_` marks the stop channel: it is set by
 *    complete_submit_stopped() (io_context::stop() aborting this inflight
 *    operation) or by start() when the user's stop_token is already
 *    requested. execute() arbitrates the final signal: token canceled →
 *    set_stopped; otherwise set_value(operation_canceled, ...).
 *  - A negative `result` from perform_io() / kevent reports a recoverable
 *    errno through set_value(ec, ...).
 */
template <class Request, class Receiver>
class kqueue_ready_io_operation : public kqueue_io_operation_base {
 public:
  /**
   * Creates the operation state bound to the context, request, and
   * receiver.
   */
  kqueue_ready_io_operation(kqueue_context& context, Request request,
                            Receiver receiver)
      : context_(&context),
        request_(std::move(request)),
        receiver_(std::move(receiver)) {}

  /**
   * Copy construction is disabled because operations are queued intrusively.
   */
  kqueue_ready_io_operation(const kqueue_ready_io_operation&) = delete;

  /**
   * Copy assignment is disabled because operations are queued intrusively.
   */
  kqueue_ready_io_operation& operator=(const kqueue_ready_io_operation&) =
      delete;

  /**
   * Move construction is disabled because operations are queued intrusively.
   */
  kqueue_ready_io_operation(kqueue_ready_io_operation&&) = delete;

  /**
   * Move assignment is disabled because operations are queued intrusively.
   */
  kqueue_ready_io_operation& operator=(kqueue_ready_io_operation&&) = delete;

  /** Delegates native registration preparation to the request. */
  void prepare(kqueue_helper& helper) noexcept override {
    request_.prepare(helper);
  }

  /** Records the submission failure errno as the completion result. */
  void complete_submit_error(int result_code) noexcept override {
    result = result_code;
  }

  /** Selects the stop channel for an aborting io_context::stop(). */
  void complete_submit_stopped() noexcept override { stopped_ = true; }

  /** Returns true: this operation performs the native I/O itself. */
  [[nodiscard]] bool owns_io_step() const noexcept override { return true; }

  /** Performs one bounded nonblocking I/O call through the request. */
  [[nodiscard]] int perform_io() noexcept override {
    return request_.perform_io();
  }

  /** Starts the operation: checks the stop token, attempts an immediate
   *  nonblocking I/O step, and otherwise publishes the operation for
   *  passive preparation by the run loop. */
  void start() noexcept {
    if (detail::stop_requested(receiver_)) {
      stopped_ = true;
      (void)context_->post(*this);
      return;
    }

    if (try_complete_immediate()) {
      return;
    }

    context_->publish_io(*this);
  }

  /**
   * Delivers the completion.
   *
   * `stopped_` marks the stop channel (io_context::stop() abort, or a
   * stop-token cancel observed at start()); execute() arbitrates the
   * final signal: token canceled → set_stopped, otherwise
   * set_value(operation_canceled, -1, 0). All other completions deliver
   * the real result through set_value(ec, result, flags) untouched.
   *
   * The aborted result is -1 and not 0: an aborted operation produced no
   * result at all, and a descriptor-yielding request (accept) forwards
   * `result` verbatim, so 0 here would hand the caller ownership of a real
   * descriptor — the process's stdin. Byte-count requests clamp with
   * std::max(0, result) and still observe 0.
   */
  void execute() noexcept override {
    if (stopped_) {
      if (detail::stop_requested(receiver_)) {
        bexec::set_stopped(std::move(receiver_));
      } else {
        request_.set_value(std::move(receiver_),
                           std::make_error_code(std::errc::operation_canceled),
                           -1, 0);
      }
      return;
    }
    std::error_code ec;
    if (result < 0) {
      ec = std::error_code(-result, std::generic_category());
    }
    request_.set_value(std::move(receiver_), ec, result, flags);
  }

 private:
  /** Attempts nonblocking I/O before registering with kqueue for readiness.
   *
   *  If the request supports immediate I/O (satisfies has_start_io), this
   *  method calls request_.start_io() immediately.  When the call does not
   *  return EAGAIN/EWOULDBLOCK the operation is completed inline via the CPU
   *  queue (post) and this function returns true.  Otherwise it returns false
   *  so that the caller can register the operation with kqueue.
   */
  [[nodiscard]] bool try_complete_immediate() noexcept {
    if constexpr (has_start_io<Request>) {
      result = request_.start_io();
      flags = 0;
      if (detail::should_wait(request_, result)) {
        return false;
      }
      (void)context_->post(*this);
      return true;
    } else {
      return false;
    }
  }

  /**
   * Context whose run loop drives this operation.
   */
  kqueue_context* context_;

  /**
   * Request that prepares the kqueue registration, performs the native
   * I/O step, and translates the completion into set_value arguments.
   */
  Request request_;

  /**
   * Receiver completed by this operation.
   */
  std::remove_cvref_t<Receiver> receiver_;

  /**
   * Marks the stop channel (io_context::stop() abort, or a stop-token
   * cancel observed at start()); execute()'s token arbitration decides
   * the final signal.
   */
  bool stopped_ = false;
};

/** Sender for a readiness-backed nonblocking request. */
template <class Request>
class kqueue_ready_io_sender {
 public:
  /** Completion signatures forwarded from the request type. */
  using completion_signatures = typename Request::completion_signatures;

  /** Creates a sender bound to the context and request. */
  kqueue_ready_io_sender(kqueue_context& context, Request request) noexcept
      : context_(&context), request_(std::move(request)) {}

  /** Connects the sender to a receiver, moving the request into the
   *  created operation state. */
  template <class Receiver>
  auto connect(Receiver receiver) && {
    return kqueue_ready_io_operation<Request, std::remove_cvref_t<Receiver>>(
        *context_, std::move(request_), std::move(receiver));
  }

  /** Connects the sender to a receiver, copying the request into the
   *  created operation state. */
  template <class Receiver>
    requires std::copy_constructible<Request>
  auto connect(Receiver receiver) const& {
    return kqueue_ready_io_operation<Request, std::remove_cvref_t<Receiver>>(
        *context_, request_, std::move(receiver));
  }

 private:
  kqueue_context* context_;
  Request request_;
};

}  // namespace bnio::async_io::bsd_native::detail

#endif  // BNIO_ASYNC_IO_BSD_KQUEUE_OPERATIONS_DETAIL_IO_REQUEST_H_
