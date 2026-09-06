/**
 * @file io_uring_receiver_operation.h
 * @brief Internal io_uring receiver operation state.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_LINUX_DETAIL_IO_URING_RECEIVER_OPERATION_H_
#define BNIO_ASYNC_IO_LINUX_DETAIL_IO_URING_RECEIVER_OPERATION_H_

#include <bnio/async_io/linux/io_uring_context_base.h>

#include <bexec/query.hpp>
#include <bexec/receiver.hpp>
#include <cerrno>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bnio::async_io::linux_native {

namespace detail {

/**
 * Completion channel selected for receiver delivery.
 *
 * Per completion-semantics contract: set_value(ec, ...) is the universal
 * observable exit (success, cancel, recoverable failure); set_stopped is
 * reserved exclusively for stop-token cancellation observed in the
 * receiver environment.
 */
enum class io_uring_receiver_completion {
  /**
   * Complete the receiver with set_value(empty ec, result, flags).
   */
  value,

  /**
   * Complete the receiver with set_value(ec, result, flags) where ec
   * carries a recoverable error (cancel or SQE preparation failure).
   */
  value_with_ec,

  /**
   * Completion marked as stopped; the final signal is decided by
   * execute()'s token arbitration: a cancelled receiver stop token
   * completes with set_stopped, an abort without one (io_context::stop()
   * aborting this op) completes with set_value(operation_canceled, 0, 0).
   * Marked by complete_submit_stopped() and by start_io()'s stop-token
   * pre-check.
   */
  stopped,
};

/**
 * Base operation that translates io_uring completions into receiver signals.
 */
template <class Receiver>
class io_uring_receiver_operation : public io_uring_io_operation_base {
 public:
  /**
   * Copy construction is disabled because operations are queued intrusively.
   */
  io_uring_receiver_operation(const io_uring_receiver_operation&) = delete;

  /**
   * Copy assignment is disabled because operations are queued intrusively.
   */
  io_uring_receiver_operation& operator=(const io_uring_receiver_operation&) =
      delete;

  /**
   * Move construction is disabled because operations are queued intrusively.
   */
  io_uring_receiver_operation(io_uring_receiver_operation&&) = delete;

  /**
   * Move assignment is disabled because operations are queued intrusively.
   */
  io_uring_receiver_operation& operator=(io_uring_receiver_operation&&) =
      delete;

  /**
   * Destroys the operation without delivering an additional signal.
   */
  ~io_uring_receiver_operation() noexcept override = default;

  /**
   * Delivers the selected completion signal to the receiver.
   *
   * Both `value` and `value_with_ec` exit through set_value; only the
   * leading std::error_code differs (empty for success). `stopped`
   * runs the token arbitration: set_stopped is emitted if and only if
   * the receiver's stop token is cancelled (including a token that
   * raced and won against io_context::stop()); an abort without a
   * cancelled token exits through set_value(operation_canceled, 0, 0).
   */
  void execute() noexcept override {
    switch (completion_) {
      case io_uring_receiver_completion::value:
        // The CQE handler only updates result/flags; it does not
        // reclassify completion_. When result < 0 (-errno), ec must be
        // re-derived from result, otherwise the error would be masked.
        if (result < 0) {
          bexec::set_value(std::move(receiver_),
                           std::error_code(-result, std::generic_category()),
                           result, flags);
        } else {
          bexec::set_value(std::move(receiver_), std::error_code{}, result,
                           flags);
        }
        break;
      case io_uring_receiver_completion::value_with_ec:
        bexec::set_value(std::move(receiver_), error_, result, flags);
        break;
      case io_uring_receiver_completion::stopped:
        // Token arbitration: set_stopped is emitted only when the
        // receiver's stop token is cancelled; an io_context::stop()
        // abort without a cancelled token delivers
        // set_value(operation_canceled, 0, 0) instead.
        if (stop_requested()) {
          bexec::set_stopped(std::move(receiver_));
        } else {
          bexec::set_value(std::move(receiver_),
                           std::error_code(ECANCELED, std::generic_category()),
                           0, 0);
        }
        break;
    }
  }

  void complete_submit_error(int result_code) noexcept override {
    complete_with_ec(result_code);
  }

  void complete_submit_stopped() noexcept override { complete_with_stopped(); }

  /** Returns whether a -EAGAIN CQE re-submits this operation instead of
   *  terminating it. Opted in by read/write-class operations through
   *  enable_eagain_rearm(). */
  [[nodiscard]] bool rearm_on_eagain() const noexcept override {
    return rearm_on_eagain_;
  }

 protected:
  /**
   * Creates a receiver operation associated with an io_uring context.
   */
  io_uring_receiver_operation(io_uring_context& context, Receiver receiver)
      : context_(&context), receiver_(std::move(receiver)) {}

  /**
   * Returns whether the receiver environment has requested cancellation.
   */
  [[nodiscard]] bool stop_requested() const noexcept {
    auto env = bexec::get_env(receiver_);
    auto token = bexec::query(env, bexec::get_stop_token);
    return token.stop_requested();
  }

  /**
   * Opts this operation into -EAGAIN re-submission. Read/write-class
   * operations on sockets and files call this from their constructor so a
   * transient would-block CQE is re-submitted through the I/O queue
   * (mirroring kqueue_context::perform_io_step()) instead of being
   * delivered as a terminal error.
   */
  void enable_eagain_rearm() noexcept { rearm_on_eagain_ = true; }

  /**
   * Selects set_value(empty ec, ...) completion for this operation.
   */
  void complete_with_value() noexcept {
    completion_ = io_uring_receiver_completion::value;
  }

  /**
   * Selects set_value(ec, ...) completion where ec carries a recoverable
   * error (SQE preparation failure reported through
   * complete_submit_error). Stop-token cancellation no longer routes
   * here; start_io() marks those completions stopped.
   */
  void complete_with_ec(int result_code) noexcept {
    completion_ = io_uring_receiver_completion::value_with_ec;
    error_ = std::error_code(-result_code, std::generic_category());
  }

  /**
   * Marks this completion stopped; execute()'s token arbitration decides
   * between set_stopped (cancelled receiver stop token) and
   * set_value(operation_canceled, 0, 0) (io_context::stop() abort).
   *
   * Reached via complete_submit_stopped() (context aborting this
   * inflight I/O operation) and via start_io()'s stop-token pre-check.
   */
  void complete_with_stopped() noexcept {
    completion_ = io_uring_receiver_completion::stopped;
  }

  /**
   * Starts an io_uring operation or posts an immediate completion.
   *
   * A stop token cancelled at start marks the completion stopped;
   * execute()'s token arbitration then delivers set_stopped.
   * io_context::stop() aborts are delivered through
   * set_value(operation_canceled, ...) unless the stop token won the
   * race.
   */
  template <class Operation>
  void start_io(Operation& operation) noexcept {
    if (stop_requested()) {
      complete_with_stopped();
      (void)context_->post(operation);
      return;
    }

    complete_with_value();
    context_->publish_io(operation);
  }

  /**
   * Context that passively prepares and completes the operation.
   */
  io_uring_context* context_;

  /**
   * Receiver completed by this operation.
   */
  std::remove_cvref_t<Receiver> receiver_;

  /**
   * Completion channel selected before execute runs.
   */
  io_uring_receiver_completion completion_ =
      io_uring_receiver_completion::value;

  /**
   * Error delivered as the leading argument of set_value when
   * completion_ is value_with_ec. Empty for the value channel.
   */
  std::error_code error_;

  /**
   * Whether a -EAGAIN CQE re-submits this operation (read/write-class
   * operations only; see enable_eagain_rearm()).
   */
  bool rearm_on_eagain_ = false;
};

}  // namespace detail

}  // namespace bnio::async_io::linux_native

#endif  // BNIO_ASYNC_IO_LINUX_DETAIL_IO_URING_RECEIVER_OPERATION_H_
