/**
 * @file types.h
 * @brief DNS type aliases.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_DNS_TYPES_H_
#define BNIO_ASYNC_IO_DNS_TYPES_H_

#include <bnio/async_io/ip/address.h>

#include <cstddef>

namespace bnio::async_io {

/**
 * Default maximum host bytes stored by dns_query, excluding the trailing nul.
 */
inline constexpr std::size_t default_dns_host_capacity = 255;

/**
 * Default maximum service bytes stored by dns_query, excluding the trailing
 * nul.
 */
inline constexpr std::size_t default_dns_service_capacity = 32;

/**
 * Transport filter used by DNS address resolution.
 */
enum class dns_transport {
  /**
   * Do not restrict the resolver by socket type or protocol.
   */
  any,

  /**
   * Return stream-oriented TCP endpoints.
   */
  tcp,

  /**
   * Return datagram-oriented UDP endpoints.
   */
  udp,
};

/**
 * Portable resolver query flags.
 */
enum class dns_query_flags : unsigned {
  /**
   * No resolver flags.
   */
  none = 0,

  /**
   * Return wildcard addresses suitable for binding when host is empty.
   */
  passive = 1U << 0U,

  /**
   * Request the canonical name when the platform resolver supports it.
   */
  canonical_name = 1U << 1U,

  /**
   * Treat the host as a numeric address string.
   */
  numeric_host = 1U << 2U,

  /**
   * Treat the service as a numeric port string.
   */
  numeric_service = 1U << 3U,
};

/**
 * Combines two resolver flag sets into their union.
 */
[[nodiscard]] constexpr dns_query_flags operator|(
    dns_query_flags lhs, dns_query_flags rhs) noexcept {
  return static_cast<dns_query_flags>(static_cast<unsigned>(lhs) |
                                      static_cast<unsigned>(rhs));
}

/**
 * Intersects two resolver flag sets.
 */
[[nodiscard]] constexpr dns_query_flags operator&(
    dns_query_flags lhs, dns_query_flags rhs) noexcept {
  return static_cast<dns_query_flags>(static_cast<unsigned>(lhs) &
                                      static_cast<unsigned>(rhs));
}

/**
 * Returns whether @p flags contains @p flag.
 */
[[nodiscard]] constexpr bool has_dns_query_flag(dns_query_flags flags,
                                                dns_query_flags flag) noexcept {
  return (flags & flag) != dns_query_flags::none;
}

/**
 * Non-owning query view consumed by platform resolver implementations.
 */
struct dns_query_view {
  /**
   * Nul-terminated host name, numeric address, or nullptr for an empty host.
   */
  const char* host = nullptr;

  /**
   * Nul-terminated service name, decimal port, or nullptr for an empty service.
   */
  const char* service = nullptr;

  /**
   * Requested address family.
   */
  ip::address::version address_version = ip::address::version::unspecified;

  /**
   * Requested transport filter.
   */
  dns_transport transport = dns_transport::tcp;

  /**
   * Portable resolver flags.
   */
  dns_query_flags flags = dns_query_flags::none;

  /**
   * Whether the owning query fit inside its fixed buffers.
   */
  bool valid = true;
};

}  // namespace bnio::async_io

#endif  // BNIO_ASYNC_IO_DNS_TYPES_H_
