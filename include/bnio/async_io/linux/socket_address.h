/**
 * @file socket_address.h
 * @brief Sockaddr storage wrapper for io_uring.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_LINUX_SOCKET_ADDRESS_H_
#define BNIO_ASYNC_IO_LINUX_SOCKET_ADDRESS_H_

#include <bnio/async_io/ip/endpoint.h>
#include <bnio/export.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <optional>

namespace bnio::async_io::linux_native {

/** Linux-native socket address storage for an IP endpoint. */
class BNIO_EXPORT socket_address {
 public:
  /** Creates an empty address that holds no endpoint. */
  socket_address() noexcept;

  /** Stores @p endpoint as a native sockaddr value. */
  explicit socket_address(const ip::endpoint& endpoint) noexcept;

  /** Copies the stored address bytes. */
  socket_address(const socket_address&) noexcept = default;

  /** Copies the stored address bytes. */
  socket_address& operator=(const socket_address&) noexcept = default;

  /** Moves the stored address bytes. */
  socket_address(socket_address&&) noexcept = default;

  /** Moves the stored address bytes. */
  socket_address& operator=(socket_address&&) noexcept = default;

  /** Destroys the address storage. */
  ~socket_address() noexcept = default;

  /** Returns whether an endpoint was stored. */
  [[nodiscard]] bool valid() const noexcept;

  /**
   * Returns the stored address family (AF_INET or AF_INET6), or AF_UNSPEC
   * when no endpoint was stored.
   */
  [[nodiscard]] int family() const noexcept;

  /**
   * Returns a pointer to the native sockaddr for socket calls, or nullptr
   * when no endpoint was stored.
   */
  [[nodiscard]] sockaddr* data() noexcept;

  /**
   * Returns a pointer to the native sockaddr for socket calls, or nullptr
   * when no endpoint was stored.
   */
  [[nodiscard]] const sockaddr* data() const noexcept;

  /**
   * Returns the byte length to pass alongside data() to socket calls, or
   * zero when no endpoint was stored.
   */
  [[nodiscard]] socklen_t size() const noexcept;

 private:
  sockaddr_storage storage_{};
  socklen_t size_ = 0;
};

/**
 * Converts a native sockaddr into an IP endpoint.
 *
 * @param[in] address Native socket address bytes.
 * @param[in] size    Byte length of @p address.
 * @return The converted endpoint, or std::nullopt when @p address is null,
 *         @p size is too small for the address family it claims, or the
 *         family is neither AF_INET nor AF_INET6.
 */
[[nodiscard]] BNIO_EXPORT std::optional<ip::endpoint> make_endpoint(
    const sockaddr* address, socklen_t size) noexcept;

}  // namespace bnio::async_io::linux_native

#endif  // BNIO_ASYNC_IO_LINUX_SOCKET_ADDRESS_H_
