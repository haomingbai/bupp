/**
 * @file kqueue_receiver_operation.h
 * @brief Internal kqueue receiver operation state.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_BSD_DETAIL_KQUEUE_RECEIVER_OPERATION_H_
#define BNIO_ASYNC_IO_BSD_DETAIL_KQUEUE_RECEIVER_OPERATION_H_

#include <bnio/async_io/bsd/kqueue_context_base.h>

#include <bexec/query.hpp>
#include <bexec/receiver.hpp>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bnio::async_io::bsd_native::detail {

/**
 * Completion channel selected for receiver delivery.
 *
 * Per completion-semantics contract: set_value(ec, ...) is the universal
 * observable exit (success, recoverable failure, and an abort observed
 * without a canceled stop token). `stopped` marks the stop channel —
 * io_context::stop() aborting inflight I/O, or a stop-token cancel
 * observed at start time — and execute() arbitrates its final signal:
 * a canceled receiver token wins → set_stopped; otherwise
 * set_value(operation_canceled, ...).
 */
enum class kqueue_receiver_completion {
  /** set_value(empty ec, result, flags). */
  value,
  /** set_value(ec, result, flags) where ec carries a recoverable error. */
  value_with_ec,
  /** Stop channel (abort or token-at-start); execute() arbitrates the
   *  final signal. */
  stopped,
};

/** Translates kqueue context results into receiver completion signals. */
template <class Receiver>
class kqueue_receiver_operation : public kqueue_io_operation_base {
 public:
  /**
   * Copy construction is disabled because operations are queued intrusively.
   */
  kqueue_receiver_operation(const kqueue_receiver_operation&) = delete;

  /**
   * Copy assignment is disabled because operations are queued intrusively.
   */
  kqueue_receiver_operation& operator=(const kqueue_receiver_operation&) =
      delete;

  /**
   * Move construction is disabled because operations are queued intrusively.
   */
  kqueue_receiver_operation(kqueue_receiver_operation&&) = delete;

  /**
   * Move assignment is disabled because operations are queued intrusively.
   */
  kqueue_receiver_operation& operator=(kqueue_receiver_operation&&) = delete;

  /**
   * Destroys the operation without delivering an additional signal.
   */
  ~kqueue_receiver_operation() noexcept override = default;

  /**
   * Delivers the selected completion signal to the receiver.
   *
   * The `value` branch re-derives the error from `result`: a kevent may
   * report readiness, then perform_io() returns a negative errno, but
   * completion_ is still `value` because the event handler only updates
   * `result`, not `completion_`. That errno must surface through
   * set_value(ec, ...) rather than being lost.
   */
  void execute() noexcept override {
    switch (completion_) {
      case kqueue_receiver_completion::value:
        if (this->result < 0) {
          bexec::set_value(
              std::move(receiver_),
              std::error_code(-this->result, std::generic_category()),
              this->result, this->flags);
        } else {
          bexec::set_value(std::move(receiver_), std::error_code{},
                           this->result, this->flags);
        }
        break;
      case kqueue_receiver_completion::value_with_ec:
        bexec::set_value(std::move(receiver_), error_, this->result,
                         this->flags);
        break;
      case kqueue_receiver_completion::stopped:
        // Token arbitration decides the stop channel's final signal: a
        // canceled receiver token wins → set_stopped; an abort observed
        // without a canceled token delivers
        // set_value(operation_canceled, 0, 0).
        if (stop_requested()) {
          bexec::set_stopped(std::move(receiver_));
        } else {
          bexec::set_value(std::move(receiver_),
                           std::make_error_code(std::errc::operation_canceled),
                           0, 0);
        }
        break;
    }
  }

  void complete_submit_error(int result_code) noexcept override {
    complete_with_ec(result_code);
  }

  void complete_submit_stopped() noexcept override { complete_with_stopped(); }

 protected:
  /** Creates the operation state bound to the context and receiver. */
  kqueue_receiver_operation(kqueue_context& context, Receiver receiver)
      : context_(&context), receiver_(std::move(receiver)) {}

  /** Returns whether the receiver's associated stop token is canceled. */
  [[nodiscard]] bool stop_requested() const noexcept {
    auto environment = bexec::get_env(receiver_);
    auto token = bexec::query(environment, bexec::get_stop_token);
    return token.stop_requested();
  }

  /** Selects the value channel (empty ec) for a successful start. */
  void complete_with_value() noexcept {
    completion_ = kqueue_receiver_completion::value;
  }

  /**
   * Selects set_value(ec, ...) where ec carries a recoverable error
   * (preparation/registration failure reported through
   * complete_submit_error).
   */
  void complete_with_ec(int result_code) noexcept {
    completion_ = kqueue_receiver_completion::value_with_ec;
    error_ = std::error_code(-result_code, std::generic_category());
  }

  /**
   * Selects the stop channel. Reached via complete_submit_stopped()
   * (io_context::stop() aborts this inflight I/O operation) or via
   * start_io()'s stop-token pre-check. execute()'s token arbitration
   * decides the final signal: token canceled → set_stopped, otherwise
   * set_value(operation_canceled, ...).
   */
  void complete_with_stopped() noexcept {
    completion_ = kqueue_receiver_completion::stopped;
  }

  /**
   * Starts a kqueue operation or posts an immediate completion.
   *
   * A stop-token cancel observed at start() selects the stop channel;
   * execute()'s token arbitration then delivers set_stopped (token
   * canceled) or set_value(operation_canceled, ...) (abort raced the
   * token).
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

  /** Context whose run loop drives this operation. */
  kqueue_context* context_;
  /** Receiver that receives the completion signal. */
  std::remove_cvref_t<Receiver> receiver_;
  /** Completion channel selected before execution; consumed by execute(). */
  kqueue_receiver_completion completion_ = kqueue_receiver_completion::value;
  /** Leading ec for the value_with_ec channel; empty for value. */
  std::error_code error_;
};

}  // namespace bnio::async_io::bsd_native::detail

#endif  // BNIO_ASYNC_IO_BSD_DETAIL_KQUEUE_RECEIVER_OPERATION_H_
