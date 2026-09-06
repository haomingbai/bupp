/**
 * @file udp.h
 * @brief Aggregate header for bnio UDP support.
 */

#pragma once
#ifndef BNIO_UDP_H_
#define BNIO_UDP_H_

#include <bnio/async_io/dns/types.h>
#include <bnio/ip.h>
#include <bnio/udp/async_operations.h>  // IWYU pragma: export
#include <bnio/udp/socket.h>            // IWYU pragma: export

#include <cstdint>
#include <string_view>

namespace bnio::udp {

/**
 * Creates a dns_query with the UDP transport and the address-family filter
 * already set, ready to pass to async_resolve().
 */
[[nodiscard]] inline dns_query make_resolve_query(
    std::string_view host, std::string_view service,
    ip::udp protocol = {}) noexcept {
  dns_query query(host, service);
  query.set_transport(dns_transport::udp);
  query.set_address_version(protocol.version());
  return query;
}

/**
 * Creates a dns_query with the UDP transport and the address-family filter
 * already set, resolving a numeric port as the service, ready to pass to
 * async_resolve().
 */
[[nodiscard]] inline dns_query make_resolve_query(
    std::string_view host, std::uint16_t port, ip::udp protocol = {}) noexcept {
  dns_query query(host, port);
  query.set_transport(dns_transport::udp);
  query.set_address_version(protocol.version());
  return query;
}

}  // namespace bnio::udp

#endif  // BNIO_UDP_H_
