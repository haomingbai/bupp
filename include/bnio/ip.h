/**
 * @file ip.h
 * @brief Aggregate header for IP address and endpoint types.
 */

#pragma once
#ifndef BNIO_IP_H_
#define BNIO_IP_H_

#include <bnio/async_io/config.h>
#include <bnio/async_io/dns.h>
#include <bnio/async_io/ip/address.h>
#include <bnio/async_io/ip/endpoint.h>
#include <bnio/async_io/ip/tcp.h>
#include <bnio/async_io/ip/udp.h>
#include <bnio/export.h>

namespace bnio {

namespace tcp {
class BNIO_EXPORT socket;
class BNIO_EXPORT acceptor;
}  // namespace tcp

namespace udp {
class BNIO_EXPORT socket;
}  // namespace udp

/**
 * Alias for the owning TCP stream socket type.
 */
using tcp_socket = tcp::socket;

/**
 * Alias for the owning TCP listening socket type.
 */
using tcp_acceptor = tcp::acceptor;

/**
 * Alias for the owning UDP datagram socket type.
 */
using udp_socket = udp::socket;

/**
 * DNS query object used by io_context resolver senders.
 */
using dns_query = async_io::dns_query;

/**
 * DNS result storage view used by io_context resolver senders.
 */
using dns_result_view = async_io::dns_result_view;

/**
 * DNS transport filter used by resolver queries.
 */
using dns_transport = async_io::dns_transport;

/**
 * DNS resolver query flags.
 */
using dns_query_flags = async_io::dns_query_flags;

namespace ip {

/**
 * Alias for the generic IP address type.
 */
using address = async_io::ip::address;

/**
 * Alias for a generic IP endpoint.
 */
using endpoint = async_io::ip::endpoint;

#if defined(BNIO_HAS_ASYNC_IO_IP_ADDRESS_PARSER)
/**
 * Imports the generic IP address parser into bnio::ip.
 */
using async_io::ip::make_addr;

/**
 * Imports the generic IP address parser into bnio::ip.
 */
using async_io::ip::make_address;

/**
 * Imports the IPv4 address parser into bnio::ip.
 */
using async_io::ip::make_v4_address;

/**
 * Imports the IPv6 address parser into bnio::ip.
 */
using async_io::ip::make_v6_address;
#endif

/**
 * Protocol tag and type namespace for TCP over IPv4 or IPv6.
 *
 * The address and endpoint vocabulary types are aliases of async_io value
 * types, while socket owners are provided by the higher-level TCP API.
 */
class BNIO_EXPORT tcp {
 public:
  /**
   * Endpoint type used by TCP.
   */
  using endpoint = bnio::ip::endpoint;

  /**
   * Owning TCP stream socket type.
   */
  using socket = bnio::tcp::socket;

  /**
   * Owning TCP stream socket type.
   */
  using stream = socket;

  /**
   * Owning TCP listening socket type.
   */
  using acceptor = bnio::tcp::acceptor;

  /**
   * Creates an unspecified TCP protocol tag.
   */
  tcp() noexcept = default;

  /**
   * Copies a TCP protocol tag.
   */
  tcp(const tcp&) noexcept = default;

  /**
   * Copies a TCP protocol tag.
   */
  tcp& operator=(const tcp&) noexcept = default;

  /**
   * Moves a TCP protocol tag.
   */
  tcp(tcp&&) noexcept = default;

  /**
   * Moves a TCP protocol tag.
   */
  tcp& operator=(tcp&&) noexcept = default;

  /**
   * Destroys the TCP protocol tag.
   */
  ~tcp() noexcept = default;

  /**
   * Returns a TCP/IPv4 protocol tag.
   */
  static tcp v4() noexcept { return tcp(async_io::ip::tcp::v4()); }

  /**
   * Returns a TCP/IPv6 protocol tag.
   */
  static tcp v6() noexcept { return tcp(async_io::ip::tcp::v6()); }

  /**
   * Returns the IP version associated with this protocol tag.
   */
  [[nodiscard]] bnio::ip::address::version version() const noexcept {
    return protocol_.version();
  }

  /**
   * Returns the async_io protocol tag represented by this value.
   */
  [[nodiscard]] async_io::ip::tcp async_io_protocol() const noexcept {
    return protocol_;
  }

 private:
  /**
   * Creates a TCP protocol tag from an async_io protocol tag.
   */
  explicit tcp(async_io::ip::tcp protocol) noexcept : protocol_(protocol) {}

  async_io::ip::tcp protocol_{};
};

/**
 * Protocol tag and type namespace for UDP over IPv4 or IPv6.
 */
class BNIO_EXPORT udp {
 public:
  /**
   * Endpoint type used by UDP.
   */
  using endpoint = bnio::ip::endpoint;

  /**
   * Owning UDP datagram socket type.
   */
  using socket = bnio::udp::socket;

  /**
   * Creates an unspecified UDP protocol tag.
   */
  udp() noexcept = default;

  /**
   * Copies a UDP protocol tag.
   */
  udp(const udp&) noexcept = default;

  /**
   * Copies a UDP protocol tag.
   */
  udp& operator=(const udp&) noexcept = default;

  /**
   * Moves a UDP protocol tag.
   */
  udp(udp&&) noexcept = default;

  /**
   * Moves a UDP protocol tag.
   */
  udp& operator=(udp&&) noexcept = default;

  /**
   * Destroys the UDP protocol tag.
   */
  ~udp() noexcept = default;

  /**
   * Returns a UDP/IPv4 protocol tag.
   */
  static udp v4() noexcept { return udp(async_io::ip::udp::v4()); }

  /**
   * Returns a UDP/IPv6 protocol tag.
   */
  static udp v6() noexcept { return udp(async_io::ip::udp::v6()); }

  /**
   * Returns the IP version associated with this protocol tag.
   */
  [[nodiscard]] bnio::ip::address::version version() const noexcept {
    return protocol_.version();
  }

  /**
   * Returns the async_io protocol tag represented by this value.
   */
  [[nodiscard]] async_io::ip::udp async_io_protocol() const noexcept {
    return protocol_;
  }

 private:
  explicit udp(async_io::ip::udp protocol) noexcept : protocol_(protocol) {}

  async_io::ip::udp protocol_{};
};

}  // namespace ip

}  // namespace bnio

#endif  // BNIO_IP_H_
