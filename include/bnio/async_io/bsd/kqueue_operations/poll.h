/**
 * @file poll.h
 * @brief kqueue poll operations.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_BSD_KQUEUE_OPERATIONS_POLL_H_
#define BNIO_ASYNC_IO_BSD_KQUEUE_OPERATIONS_POLL_H_

#include <bnio/async_io/bsd/detail/kqueue_receiver_operation.h>
#include <bnio/async_io/descriptor_view.h>

#include <bexec/completion_signatures.hpp>
#include <bexec/query.hpp>
#include <bexec/receiver.hpp>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bnio::async_io::bsd_native {

/** Prepared kqueue poll request reusable by higher abstraction layers. */
class kqueue_poll_request {
 public:
  /** Creates a prepared poll request for the descriptor and event mask. */
  kqueue_poll_request(descriptor_view descriptor, unsigned poll_mask) noexcept
      : descriptor_(descriptor), poll_mask_(poll_mask) {}

  /** Stages the poll-add registration on the helper. */
  void prepare(kqueue_helper& helper) const noexcept {
    helper.prep_poll_add(descriptor_.native_handle(), poll_mask_);
  }

 private:
  descriptor_view descriptor_;
  unsigned poll_mask_;
};

/** Raw poll operation that reports result and native flags. */
template <class Receiver>
class kqueue_poll_operation
    : public detail::kqueue_receiver_operation<Receiver> {
 public:
  /** Creates the operation state bound to the context, descriptor, mask,
   *  and receiver. */
  kqueue_poll_operation(kqueue_context& context, descriptor_view descriptor,
                        unsigned poll_mask, Receiver receiver)
      : detail::kqueue_receiver_operation<Receiver>(context,
                                                    std::move(receiver)),
        request_(descriptor, poll_mask) {}

  /** Delegates native registration preparation to the poll request. */
  void prepare(kqueue_helper& helper) noexcept override {
    request_.prepare(helper);
  }

  /** Starts the poll through the receiver-operation start path. */
  void start() noexcept { this->start_io(*this); }

 private:
  kqueue_poll_request request_;
};

/** Operation state used by the typed kqueue poll sender. */
template <class Receiver>
class kqueue_poll_sender_operation : public kqueue_io_operation_base {
 public:
  /** Creates the operation state bound to the context, descriptor, mask,
   *  and receiver. */
  kqueue_poll_sender_operation(kqueue_context& context,
                               descriptor_view descriptor, unsigned poll_mask,
                               Receiver receiver)
      : context_(&context),
        request_(descriptor, poll_mask),
        receiver_(std::move(receiver)) {}

  kqueue_poll_sender_operation(const kqueue_poll_sender_operation&) = delete;
  kqueue_poll_sender_operation& operator=(const kqueue_poll_sender_operation&) =
      delete;
  kqueue_poll_sender_operation(kqueue_poll_sender_operation&&) = delete;
  kqueue_poll_sender_operation& operator=(kqueue_poll_sender_operation&&) =
      delete;

  void prepare(kqueue_helper& helper) noexcept override {
    request_.prepare(helper);
  }

  void complete_submit_error(int result_code) noexcept override {
    completion_ = completion_kind::value_with_ec;
    error_ = std::error_code(-result_code, std::generic_category());
  }

  void complete_submit_stopped() noexcept override {
    completion_ = completion_kind::stopped;
  }

  /**
   * Starts the poll or posts an immediate completion.
   *
   * A stop-token cancel observed at start() selects the stop channel;
   * execute()'s token arbitration then delivers set_stopped (token
   * canceled) or set_value(operation_canceled, ...) (abort raced the
   * token).
   */
  void start() noexcept {
    if (stop_requested()) {
      completion_ = completion_kind::stopped;
      (void)context_->post(*this);
      return;
    }

    completion_ = completion_kind::value;
    context_->publish_io(*this);
  }

  /**
   * Delivers the typed poll completion.
   *
   * The `value` branch preserves the `result < 0` guard: a kevent may
   * report a negative errno while completion_ is still `value`; that
   * errno must surface through set_value(ec, ...) rather than being lost.
   *
   * The ready mask is meaningful only on success; every other channel
   * delivers `0U`. The errno is already carried by `ec`, so nothing is lost,
   * and a negative result is never converted to `unsigned` (that would wrap
   * to ~4.29e9 and make a caller's `mask & POLLIN` test trip on a failure).
   */
  void execute() noexcept override {
    switch (completion_) {
      case completion_kind::value:
        if (result < 0) {
          bexec::set_value(std::move(receiver_),
                           std::error_code(-result, std::generic_category()),
                           0U);
        } else {
          bexec::set_value(std::move(receiver_), std::error_code{},
                           static_cast<unsigned>(result));
        }
        break;
      case completion_kind::value_with_ec:
        bexec::set_value(std::move(receiver_), error_, 0U);
        break;
      case completion_kind::stopped:
        // Token arbitration decides the stop channel's final signal: a
        // canceled receiver token wins → set_stopped; an abort observed
        // without a canceled token delivers
        // set_value(operation_canceled, 0).
        if (stop_requested()) {
          bexec::set_stopped(std::move(receiver_));
        } else {
          bexec::set_value(std::move(receiver_),
                           std::make_error_code(std::errc::operation_canceled),
                           0U);
        }
        break;
    }
  }

 private:
  enum class completion_kind {
    value,
    value_with_ec,
    stopped,
  };

  [[nodiscard]] bool stop_requested() const noexcept {
    auto environment = bexec::get_env(receiver_);
    auto token = bexec::query(environment, bexec::get_stop_token);
    return token.stop_requested();
  }

  kqueue_context* context_;
  kqueue_poll_request request_;
  std::remove_cvref_t<Receiver> receiver_;
  completion_kind completion_ = completion_kind::value;
  std::error_code error_;
};

/** Sender returned by kqueue_context::async_poll. */
class kqueue_poll_sender {
 public:
  /**
   * set_value(ec, unsigned) is the universal observable exit (success,
   * recoverable failure, and an abort observed without a canceled stop
   * token); set_stopped is delivered when the receiver's stop token is
   * observed canceled at the completion point.
   */
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, unsigned), bexec::set_stopped_t()>;

  /** Creates a typed poll sender for the descriptor and event mask. */
  kqueue_poll_sender(kqueue_context& context, descriptor_view descriptor,
                     unsigned poll_mask) noexcept
      : context_(&context), descriptor_(descriptor), poll_mask_(poll_mask) {}

  /** Connects the sender to a receiver, creating the poll operation
   *  state. */
  template <class Receiver>
  auto connect(Receiver receiver) const {
    return kqueue_poll_sender_operation<std::remove_cvref_t<Receiver>>(
        *context_, descriptor_, poll_mask_, std::move(receiver));
  }

 private:
  kqueue_context* context_;
  descriptor_view descriptor_;
  unsigned poll_mask_;
};

/** @cond BNIO_DETAIL */

inline auto kqueue_context::async_poll(descriptor_view descriptor,
                                       unsigned poll_mask) {
  return kqueue_poll_sender(*this, descriptor, poll_mask);
}

/** @endcond */

}  // namespace bnio::async_io::bsd_native

#endif  // BNIO_ASYNC_IO_BSD_KQUEUE_OPERATIONS_POLL_H_
