/**
 * @file instances.h
 * @brief I/O CPO instance definitions.
 */

#pragma once
#ifndef BNIO_IO_CONTEXT_CPO_INSTANCES_H_
#define BNIO_IO_CONTEXT_CPO_INSTANCES_H_

#include <bnio/io_context_cpo/connection.h>
#include <bnio/io_context_cpo/poll.h>
#include <bnio/io_context_cpo/read.h>
#include <bnio/io_context_cpo/resolve.h>
#include <bnio/io_context_cpo/write.h>

namespace bnio {

/**
 * CPO object instance for the read-all async_read operation.
 */
inline constexpr async_read_t async_read{};

/**
 * CPO object instance for one read attempt (async_read_some).
 */
inline constexpr async_read_some_t async_read_some{};

/**
 * CPO object instance for the write-all async_write operation.
 */
inline constexpr async_write_t async_write{};

/**
 * CPO object instance for one write attempt (async_write_some).
 */
inline constexpr async_write_some_t async_write_some{};

/**
 * CPO object instance for accepting one connection from an acceptor.
 */
inline constexpr async_accept_t async_accept{};

/**
 * CPO object instance for connecting a stream to an endpoint.
 */
inline constexpr async_connect_t async_connect{};

/**
 * CPO object instance for descriptor polling.
 */
inline constexpr async_poll_t async_poll{};

/**
 * CPO object instance for DNS resolution into caller-provided result
 * storage.
 */
inline constexpr async_resolve_t async_resolve{};

}  // namespace bnio

#endif  // BNIO_IO_CONTEXT_CPO_INSTANCES_H_
