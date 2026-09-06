/**
 * @file async_operations.h
 * @brief Internal UDP async operation implementations.
 */

#pragma once
#ifndef BNIO_DETAIL_UDP_ASYNC_OPERATIONS_H_
#define BNIO_DETAIL_UDP_ASYNC_OPERATIONS_H_

#include <bnio/async_io/socket_view.h>
#include <bnio/buffer.h>
#include <bnio/ip.h>

#include <bexec/completion_signatures.hpp>
#include <bexec/detail/manual_lifetime.hpp>
#include <bexec/receiver.hpp>
#include <bexec/sender.hpp>
#include <cstddef>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bnio::detail {

/**
 * Operation state that performs one datagram receive through a scheduler
 * and forwards the result to a parent receiver.
 *
 * The operation owns the buffer holder so the receive storage stays alive
 * until completion; on success the holder commits the received byte count
 * before the parent receiver is completed. With From true the child is a
 * receive-from operation that also reports the source endpoint through the
 * caller-provided pointer.
 */
template <class Scheduler, class Holder, bool From, class Receiver>
class udp_receive_operation {
 public:
  /** Stored scheduler type. */
  using scheduler_type = std::remove_cvref_t<Scheduler>;

  /** Parent receiver type. */
  using receiver_type = std::remove_cvref_t<Receiver>;

  /**
   * Receiver adapted onto the scheduler's child sender.
   *
   * It exposes the parent receiver's environment and translates the child
   * completion into the parent receiver's completion, committing received
   * bytes on success.
   */
  class child_receiver {
   public:
    /**
     * Environment type forwarded from the parent receiver.
     */
    // The env type depends only on the operation's Receiver template
    // parameter; naming it explicitly keeps get_env's return type available
    // while the operation class is still incomplete (child operation types
    // are computed inside its own definition). A deduced decltype(auto)
    // return would force the body — and its operation_->receiver_ access —
    // to be instantiated too early.
    using env_type = decltype(bexec::get_env(std::declval<receiver_type&>()));

    /** Binds the child receiver to its parent operation. */
    explicit child_receiver(udp_receive_operation& operation) noexcept
        : operation_(&operation) {}

    /** Returns the parent receiver's environment. */
    [[nodiscard]] env_type get_env() const noexcept {
      return bexec::get_env(operation_->receiver_);
    }

    /**
     * Commits the received bytes on success and completes the parent
     * receiver with @p ec and @p size.
     */
    void set_value(std::error_code ec, std::size_t size) noexcept {
      if (!ec) operation_->holder_.commit(size);
      bexec::set_value(std::move(operation_->receiver_), ec, size);
    }

    /** Completes the parent receiver as stopped. */
    void set_stopped() noexcept {
      bexec::set_stopped(std::move(operation_->receiver_));
    }

   private:
    udp_receive_operation* operation_;
  };

  /**
   * Creates the scheduler's child sender for this receive.
   *
   * @param[in,out] scheduler Scheduler providing the datagram receive
   *                          senders.
   * @param[in]     socket    Datagram socket to receive on.
   * @param[in]     buffer    Buffer that receives the datagram bytes.
   * @param[in]     endpoint  Source endpoint output; only read when From
   *                          is true.
   * @param[in]     flags     Receive flags passed to the scheduler.
   * @return The child sender selected by From.
   */
  static auto make_child_sender(scheduler_type& scheduler,
                                async_io::datagram_socket_view socket,
                                mutable_buffer buffer, ip::endpoint* endpoint,
                                int flags) {
    if constexpr (From) {
      return scheduler.async_receive_from(socket, buffer, *endpoint, flags);
    } else {
      return scheduler.async_receive(socket, buffer, flags);
    }
  }

  /** Child sender type produced by make_child_sender(). */
  using child_sender_type = decltype(make_child_sender(
      std::declval<scheduler_type&>(),
      std::declval<async_io::datagram_socket_view>(),
      std::declval<mutable_buffer>(), static_cast<ip::endpoint*>(nullptr), 0));

  /** Operation state type of the connected child sender and receiver. */
  using child_operation_type = decltype(bexec::connect(
      std::declval<child_sender_type>(), std::declval<child_receiver>()));

  /**
   * Creates the operation state and connects the child sender to a child
   * receiver.
   *
   * @param[in] scheduler Scheduler used for the child receive.
   * @param[in] socket    Datagram socket to receive on.
   * @param[in] holder    Buffer holder owning the receive storage.
   * @param[in] endpoint  Source endpoint output; may be null only when
   *                      From is false. When set, the pointee must remain
   *                      alive until the operation completes.
   * @param[in] flags     Receive flags.
   * @param[in] receiver  Parent receiver to complete.
   */
  udp_receive_operation(scheduler_type scheduler,
                        async_io::datagram_socket_view socket, Holder holder,
                        ip::endpoint* endpoint, int flags, Receiver receiver)
      : scheduler_(std::move(scheduler)),
        socket_(socket),
        holder_(std::move(holder)),
        endpoint_(endpoint),
        flags_(flags),
        receiver_(std::move(receiver)) {
    child_operation_.emplace_from([this] {
      return bexec::connect(
          make_child_sender(scheduler_, socket_, bnio::buffer(holder_.view()),
                            endpoint_, flags_),
          child_receiver(*this));
    });
  }

  udp_receive_operation(const udp_receive_operation&) = delete;
  udp_receive_operation& operator=(const udp_receive_operation&) = delete;
  udp_receive_operation(udp_receive_operation&&) = delete;
  udp_receive_operation& operator=(udp_receive_operation&&) = delete;

  /** Starts the child receive operation. */
  void start() noexcept { bexec::start(*child_operation_); }

 private:
  scheduler_type scheduler_;
  async_io::datagram_socket_view socket_;
  Holder holder_;
  ip::endpoint* endpoint_;
  int flags_;
  receiver_type receiver_;
  bexec::detail::manual_lifetime<child_operation_type> child_operation_;
};

/**
 * Sender that performs one datagram receive through a scheduler.
 *
 * With From true the receive is endpoint-aware (receive-from); otherwise
 * it uses the socket's connected peer.
 */
template <class Scheduler, class Holder, bool From>
class udp_receive_sender {
 public:
  /** Completes with set_value(std::error_code, std::size_t) or
   *  set_stopped(). */
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  /**
   * Creates a sender that receives on @p socket through @p scheduler.
   *
   * @p holder keeps the receive storage alive until completion. With From
   * true, @p endpoint receives the source endpoint of each datagram and
   * its pointee must remain alive until the operation completes.
   */
  udp_receive_sender(Scheduler scheduler, async_io::datagram_socket_view socket,
                     Holder holder, ip::endpoint* endpoint, int flags)
      : scheduler_(std::move(scheduler)),
        socket_(socket),
        holder_(std::move(holder)),
        endpoint_(endpoint),
        flags_(flags) {}

  /**
   * Connects this sender to @p receiver and returns the operation state.
   */
  template <class Receiver>
  auto connect(Receiver receiver) && {
    return udp_receive_operation<Scheduler, Holder, From,
                                 std::remove_cvref_t<Receiver>>(
        std::move(scheduler_), socket_, std::move(holder_), endpoint_, flags_,
        std::move(receiver));
  }

 private:
  Scheduler scheduler_;
  async_io::datagram_socket_view socket_;
  Holder holder_;
  ip::endpoint* endpoint_;
  int flags_;
};

/**
 * Operation state that performs one datagram send through a scheduler and
 * forwards the result to a parent receiver.
 *
 * The operation owns the buffer holder so the datagram bytes stay alive
 * until completion. With To true the child is a send-to operation targeting
 * the stored endpoint; otherwise it sends on the socket's connected peer.
 */
template <class Scheduler, class Holder, bool To, class Receiver>
class udp_send_operation {
 public:
  /** Stored scheduler type. */
  using scheduler_type = std::remove_cvref_t<Scheduler>;

  /** Parent receiver type. */
  using receiver_type = std::remove_cvref_t<Receiver>;

  /**
   * Receiver adapted onto the scheduler's child sender.
   *
   * It exposes the parent receiver's environment and forwards the child
   * completion to the parent receiver.
   */
  class child_receiver {
   public:
    /**
     * Environment type forwarded from the parent receiver.
     */
    // The env type depends only on the operation's Receiver template
    // parameter; naming it explicitly keeps get_env's return type available
    // while the operation class is still incomplete (child operation types
    // are computed inside its own definition). A deduced decltype(auto)
    // return would force the body — and its operation_->receiver_ access —
    // to be instantiated too early.
    using env_type = decltype(bexec::get_env(std::declval<receiver_type&>()));

    /** Binds the child receiver to its parent operation. */
    explicit child_receiver(udp_send_operation& operation) noexcept
        : operation_(&operation) {}

    /** Returns the parent receiver's environment. */
    [[nodiscard]] env_type get_env() const noexcept {
      return bexec::get_env(operation_->receiver_);
    }

    /** Completes the parent receiver with @p ec and @p size. */
    void set_value(std::error_code ec, std::size_t size) noexcept {
      bexec::set_value(std::move(operation_->receiver_), ec, size);
    }

    /** Completes the parent receiver as stopped. */
    void set_stopped() noexcept {
      bexec::set_stopped(std::move(operation_->receiver_));
    }

   private:
    udp_send_operation* operation_;
  };

  /**
   * Creates the scheduler's child sender for this send.
   *
   * @param[in,out] scheduler Scheduler providing the datagram send senders.
   * @param[in]     socket    Datagram socket to send on.
   * @param[in]     buffer    Datagram bytes to send.
   * @param[in]     endpoint  Destination endpoint; only read when To is
   *                          true.
   * @param[in]     flags     Send flags passed to the scheduler.
   * @return The child sender selected by To.
   */
  static auto make_child_sender(scheduler_type& scheduler,
                                async_io::datagram_socket_view socket,
                                const_buffer buffer,
                                const ip::endpoint& endpoint, int flags) {
    if constexpr (To) {
      return scheduler.async_send_to(socket, buffer, endpoint, flags);
    } else {
      return scheduler.async_send(socket, buffer, flags);
    }
  }

  /** Child sender type produced by make_child_sender(). */
  using child_sender_type = decltype(make_child_sender(
      std::declval<scheduler_type&>(),
      std::declval<async_io::datagram_socket_view>(),
      std::declval<const_buffer>(), std::declval<const ip::endpoint&>(), 0));

  /** Operation state type of the connected child sender and receiver. */
  using child_operation_type = decltype(bexec::connect(
      std::declval<child_sender_type>(), std::declval<child_receiver>()));

  /**
   * Creates the operation state and connects the child sender to a child
   * receiver.
   *
   * @param[in] scheduler Scheduler used for the child send.
   * @param[in] socket    Datagram socket to send on.
   * @param[in] holder    Buffer holder owning the datagram bytes.
   * @param[in] endpoint  Destination endpoint; ignored when To is false.
   * @param[in] flags     Send flags.
   * @param[in] receiver  Parent receiver to complete.
   */
  udp_send_operation(scheduler_type scheduler,
                     async_io::datagram_socket_view socket, Holder holder,
                     ip::endpoint endpoint, int flags, Receiver receiver)
      : scheduler_(std::move(scheduler)),
        socket_(socket),
        holder_(std::move(holder)),
        endpoint_(endpoint),
        flags_(flags),
        receiver_(std::move(receiver)) {
    child_operation_.emplace_from([this] {
      return bexec::connect(
          make_child_sender(scheduler_, socket_,
                            const_buffer(holder_.data(), holder_.size()),
                            endpoint_, flags_),
          child_receiver(*this));
    });
  }

  udp_send_operation(const udp_send_operation&) = delete;
  udp_send_operation& operator=(const udp_send_operation&) = delete;
  udp_send_operation(udp_send_operation&&) = delete;
  udp_send_operation& operator=(udp_send_operation&&) = delete;

  /** Starts the child send operation. */
  void start() noexcept { bexec::start(*child_operation_); }

 private:
  scheduler_type scheduler_;
  async_io::datagram_socket_view socket_;
  Holder holder_;
  ip::endpoint endpoint_;
  int flags_;
  receiver_type receiver_;
  bexec::detail::manual_lifetime<child_operation_type> child_operation_;
};

/**
 * Sender that performs one datagram send through a scheduler.
 *
 * With To true the send targets the stored endpoint (send-to); otherwise
 * it uses the socket's connected peer.
 */
template <class Scheduler, class Holder, bool To>
class udp_send_sender {
 public:
  /** Completes with set_value(std::error_code, std::size_t) or
   *  set_stopped(). */
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  /**
   * Creates a sender that sends on @p socket through @p scheduler.
   *
   * @p holder keeps the datagram bytes alive until completion. With To
   * true, @p endpoint is the datagram's destination.
   */
  udp_send_sender(Scheduler scheduler, async_io::datagram_socket_view socket,
                  Holder holder, ip::endpoint endpoint, int flags)
      : scheduler_(std::move(scheduler)),
        socket_(socket),
        holder_(std::move(holder)),
        endpoint_(endpoint),
        flags_(flags) {}

  /**
   * Connects this sender to @p receiver and returns the operation state.
   */
  template <class Receiver>
  auto connect(Receiver receiver) && {
    return udp_send_operation<Scheduler, Holder, To,
                              std::remove_cvref_t<Receiver>>(
        std::move(scheduler_), socket_, std::move(holder_), endpoint_, flags_,
        std::move(receiver));
  }

 private:
  Scheduler scheduler_;
  async_io::datagram_socket_view socket_;
  Holder holder_;
  ip::endpoint endpoint_;
  int flags_;
};

}  // namespace bnio::detail

#endif  // BNIO_DETAIL_UDP_ASYNC_OPERATIONS_H_
