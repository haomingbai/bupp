/**
 * @file core.h
 * @brief Core kqueue operations (post, nop).
 */

#pragma once
#ifndef BNIO_ASYNC_IO_BSD_KQUEUE_OPERATIONS_CORE_H_
#define BNIO_ASYNC_IO_BSD_KQUEUE_OPERATIONS_CORE_H_

#include <bnio/async_io/bsd/detail/kqueue_receiver_operation.h>

#include <bexec/query.hpp>
#include <bexec/receiver.hpp>
#include <type_traits>
#include <utility>

namespace bnio::async_io::bsd_native {

/** Operation that posts a receiver completion onto a kqueue_context. */
template <class Receiver>
class kqueue_post_operation : public kqueue_operation_base {
 public:
  /** Creates the operation bound to the context and receiver. */
  kqueue_post_operation(kqueue_context& context, Receiver receiver)
      : context_(&context), receiver_(std::move(receiver)) {}

  kqueue_post_operation(const kqueue_post_operation&) = delete;
  kqueue_post_operation& operator=(const kqueue_post_operation&) = delete;
  kqueue_post_operation(kqueue_post_operation&&) = delete;
  kqueue_post_operation& operator=(kqueue_post_operation&&) = delete;
  ~kqueue_post_operation() noexcept override = default;

  void execute() noexcept override {
    if (stopped_) {
      bexec::set_stopped(std::move(receiver_));
    } else {
      bexec::set_value(std::move(receiver_), std::error_code{});
    }
  }

  /** Records whether the receiver's stop token is canceled and posts the
   *  operation for completion on the context. */
  void start() noexcept {
    auto environment = bexec::get_env(receiver_);
    auto token = bexec::query(environment, bexec::get_stop_token);
    stopped_ = token.stop_requested();
    (void)context_->post(*this);
  }

 private:
  kqueue_context* context_;
  std::remove_cvref_t<Receiver> receiver_;
  bool stopped_ = false;
};

/** Operation that completes through the context without native I/O. */
template <class Receiver>
class kqueue_nop_operation
    : public detail::kqueue_receiver_operation<Receiver> {
 public:
  /** Creates the operation bound to the context and receiver. */
  kqueue_nop_operation(kqueue_context& context, Receiver receiver)
      : detail::kqueue_receiver_operation<Receiver>(context,
                                                    std::move(receiver)) {}

  /** Prepares a nop registration that only wakes the run loop. */
  void prepare(kqueue_helper& helper) noexcept override { helper.prep_nop(); }

  /** Starts the nop through the receiver-operation start path. */
  void start() noexcept { this->start_io(*this); }
};

/** Operation whose caller supplies the kqueue helper preparation function. */
template <class Receiver, class Prepare>
class kqueue_raw_operation
    : public detail::kqueue_receiver_operation<Receiver> {
 public:
  /** Creates the operation with a caller-supplied preparation callable. */
  kqueue_raw_operation(kqueue_context& context, Prepare prepare,
                       Receiver receiver)
      : detail::kqueue_receiver_operation<Receiver>(context,
                                                    std::move(receiver)),
        prepare_(std::move(prepare)) {}

  /** Runs the caller-supplied preparation callable on the helper. */
  void prepare(kqueue_helper& helper) noexcept override { prepare_(helper); }

  /** Starts the operation through the receiver-operation start path. */
  void start() noexcept { this->start_io(*this); }

 private:
  std::remove_cvref_t<Prepare> prepare_;
};

}  // namespace bnio::async_io::bsd_native

#endif  // BNIO_ASYNC_IO_BSD_KQUEUE_OPERATIONS_CORE_H_
