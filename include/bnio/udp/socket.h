/**
 * @file socket.h
 * @brief RAII UDP datagram socket owner.
 */

#pragma once
#ifndef BNIO_UDP_SOCKET_H_
#define BNIO_UDP_SOCKET_H_

#include <bnio/async_io/socket_view.h>
#include <bnio/buffer.h>
#include <bnio/export.h>
#include <bnio/ip.h>
#include <fcntl.h>
#include <sys/socket.h>

#include <cstddef>
#include <system_error>

namespace bnio::udp {

/**
 * Move-only RAII owner for a native UDP datagram socket.
 *
 * Every datagram operation transfers exactly one datagram: senders and
 * receivers preserve message boundaries and never use stream-style
 * write-all or read-all retries. Call connect(endpoint) to set a default
 * peer, then use async_send() and async_receive(); use async_send_to()
 * and async_receive_from() to address each datagram explicitly.
 */
class BNIO_EXPORT socket {
 public:
  /**
   * Native datagram socket descriptor type.
   */
  using native_handle_type = async_io::datagram_socket_view::native_handle_type;

  /**
   * Creates a closed socket owner.
   */
  socket() noexcept = default;

  /**
   * Takes ownership of an existing native datagram socket descriptor.
   *
   * The underlying descriptor is set to non-blocking mode so that the
   * kqueue backend can skip per-operation fcntl calls.
   */
  explicit socket(native_handle_type fd) noexcept : fd_(fd) {
    if (fd_ >= 0) {
      const int flags = ::fcntl(fd_, F_GETFL, 0);
      if (flags >= 0 && (flags & O_NONBLOCK) == 0) {
        (void)::fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
      }
    }
  }
  ~socket() noexcept;

  socket(const socket&) = delete;
  socket& operator=(const socket&) = delete;

  /**
   * Moves ownership of the descriptor from @p other, leaving @p other
   * closed.
   */
  socket(socket&& other) noexcept;

  /**
   * Moves ownership of the descriptor from @p other, closing any descriptor
   * this socket held and leaving @p other closed.
   */
  socket& operator=(socket&& other) noexcept;

  /**
   * Returns the wrapped native datagram socket descriptor.
   */
  [[nodiscard]] native_handle_type native_handle() const noexcept {
    return fd_;
  }

  /**
   * Returns the wrapped native datagram socket descriptor (alias of
   * native_handle()).
   */
  [[nodiscard]] native_handle_type get_native_handle() const noexcept {
    return native_handle();
  }

  /**
   * Returns whether the socket holds an open descriptor.
   */
  [[nodiscard]] bool is_open() const noexcept { return fd_ >= 0; }

  /**
   * Returns a non-owning datagram socket view of this socket.
   */
  [[nodiscard]] async_io::datagram_socket_view view() const noexcept {
    return async_io::datagram_socket_view(fd_);
  }

  /**
   * Opens a UDP datagram socket for the given address family (for example
   * AF_INET or AF_INET6). An already open socket is left unchanged.
   *
   * @return An empty error_code on success, or the errno that made socket
   *         creation fail.
   */
  [[nodiscard]] std::error_code open(int family = AF_INET) noexcept;

  /**
   * Opens a UDP datagram socket for @p protocol's IP version.
   *
   * @return An empty error_code on success, EAFNOSUPPORT when @p protocol
   *         has no IP version, or the errno that made socket creation fail.
   */
  [[nodiscard]] std::error_code open(ip::udp protocol) noexcept;

  /**
   * Binds the socket to an IP endpoint.
   */
  [[nodiscard]] std::error_code bind(const ip::endpoint& endpoint) noexcept;

  /** Sets the default peer used by send/receive operations. */
  [[nodiscard]] std::error_code connect(const ip::endpoint& endpoint) noexcept;

  /**
   * Closes the socket and returns any error reported by ::close. The socket
   * is left closed either way.
   */
  [[nodiscard]] std::error_code close() noexcept;

  /**
   * Releases ownership of the socket descriptor to the caller, leaving this
   * socket closed.
   *
   * @return The descriptor previously owned by this socket, or -1 when the
   *         socket was not open.
   */
  [[nodiscard]] native_handle_type release() noexcept;

  /**
   * Takes ownership of an existing datagram socket descriptor, closing any
   * descriptor currently held.
   *
   * Like the owning constructor, the descriptor is set to non-blocking mode
   * so that the kqueue backend can skip per-operation fcntl calls.
   */
  void assign(native_handle_type fd) noexcept;

  /**
   * Shuts down socket send and/or receive operations.
   */
  [[nodiscard]] std::error_code shutdown(int how) noexcept;

  /**
   * Enables or disables address reuse on the socket.
   */
  [[nodiscard]] std::error_code set_reuse_address(bool enabled) noexcept;

  /**
   * Stores the locally bound endpoint into @p endpoint.
   */
  [[nodiscard]] std::error_code local_endpoint(
      ip::endpoint& endpoint) const noexcept;

  /**
   * Stores the connected peer endpoint into @p endpoint.
   */
  [[nodiscard]] std::error_code remote_endpoint(
      ip::endpoint& endpoint) const noexcept;

  /**
   * Creates a sender that transfers exactly one datagram from @p buffer to
   * the socket's default peer through @p scheduler. The default peer is
   * set with connect(endpoint).
   *
   * The byte storage behind @p buffer must remain valid until the
   * operation completes. Completion delivers
   * set_value(std::error_code, bytes transferred) or set_stopped().
   */
  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_send(Scheduler scheduler, Buffer&& buffer,
                                int flags = 0);

  /**
   * Creates a sender that receives exactly one datagram into @p buffer
   * from the socket's default peer through @p scheduler. The default peer
   * is set with connect(endpoint).
   *
   * The byte storage behind @p buffer must remain valid until the
   * operation completes. Completion delivers
   * set_value(std::error_code, bytes transferred) or set_stopped().
   */
  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_receive(Scheduler scheduler, Buffer&& buffer,
                                   int flags = 0);

  /**
   * Creates a sender that sends one datagram from @p buffer to @p endpoint
   * through @p scheduler, without requiring a connected socket.
   *
   * The byte storage behind @p buffer must remain valid until the
   * operation completes. Completion delivers
   * set_value(std::error_code, bytes transferred) or set_stopped().
   */
  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_send_to(Scheduler scheduler, Buffer&& buffer,
                                   const ip::endpoint& endpoint, int flags = 0);

  /**
   * Creates a sender that receives one datagram into @p buffer through
   * @p scheduler and stores the source endpoint into @p endpoint.
   *
   * The byte storage behind @p buffer must remain valid until the
   * operation completes. @p endpoint is output storage and must remain
   * alive until the operation completes. Completion delivers
   * set_value(std::error_code, bytes transferred) or set_stopped().
   */
  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_receive_from(Scheduler scheduler, Buffer&& buffer,
                                        ip::endpoint& endpoint, int flags = 0);

 private:
  native_handle_type fd_ = -1;
};

}  // namespace bnio::udp

#endif  // BNIO_UDP_SOCKET_H_
