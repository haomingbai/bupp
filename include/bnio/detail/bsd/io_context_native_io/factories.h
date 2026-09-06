/**
 * @file factories.h
 * @brief BSD native I/O sender and request factories.
 *
 * Adapts the shared io_context I/O surface to layer-2 kqueue request
 * objects, which own the native nonblocking socket calls and retry them
 * after the corresponding filter fires.
 */

#ifndef BNIO_DETAIL_BSD_IO_CONTEXT_NATIVE_IO_FACTORIES_H_
#ifndef BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#include <bnio/io_context.h>
#else
#define BNIO_DETAIL_BSD_IO_CONTEXT_NATIVE_IO_FACTORIES_H_

#include <bnio/async_io/random_access_file.h>
#include <bnio/detail/bsd/io_context_native_io/common.h>
#include <bnio/detail/bsd/io_context_native_io/file_factories.h>

namespace bnio::detail {

/** Creates one objectized nonblocking stream receive request. */
[[nodiscard]] inline auto make_stream_read_request(
    async_io::stream_socket_view socket, mutable_buffer buffer, int flags) {
  return async_io::bsd_native::kqueue_receive_request(socket.native_handle(),
                                                      buffer.view(), flags);
}

/** Creates one objectized nonblocking stream send request. */
[[nodiscard]] inline auto make_stream_write_request(
    async_io::stream_socket_view socket, const_buffer buffer, int flags) {
  return async_io::bsd_native::kqueue_send_request(
      socket.native_handle(), buffer.data(), buffer.size(), flags);
}

/** Creates one objectized nonblocking connected-datagram receive request. */
[[nodiscard]] inline auto make_datagram_receive_request(
    async_io::datagram_socket_view socket, mutable_buffer buffer, int flags) {
  return async_io::bsd_native::kqueue_receive_request(socket.native_handle(),
                                                      buffer.view(), flags);
}

/** Creates one objectized nonblocking connected-datagram send request. */
[[nodiscard]] inline auto make_datagram_send_request(
    async_io::datagram_socket_view socket, const_buffer buffer, int flags) {
  return async_io::bsd_native::kqueue_send_request(
      socket.native_handle(), buffer.data(), buffer.size(), flags);
}

/** Creates one endpoint-aware datagram receive request. */
[[nodiscard]] inline auto make_datagram_receive_from_request(
    async_io::datagram_socket_view socket, mutable_buffer buffer,
    ip::endpoint& endpoint, int flags) {
  return async_io::bsd_native::kqueue_receive_from_request(
      socket, buffer.view(), endpoint, flags);
}

/** Creates one endpoint-aware datagram send request. */
[[nodiscard]] inline auto make_datagram_send_to_request(
    async_io::datagram_socket_view socket, const_buffer buffer,
    const ip::endpoint& endpoint, int flags) {
  return async_io::bsd_native::kqueue_send_to_request(
      socket, buffer.data(), buffer.size(), endpoint, flags);
}

/** Creates one objectized nonblocking accept request. */
[[nodiscard]] inline auto make_accept_request(
    async_io::stream_socket_view socket, int flags) {
  return async_io::bsd_native::kqueue_accept_request(socket, flags);
}

/** Creates one objectized nonblocking connect request. */
[[nodiscard]] inline auto make_connect_request(
    async_io::stream_socket_view socket, const ip::endpoint& endpoint) {
  return async_io::bsd_native::kqueue_connect_request(socket, endpoint);
}

/** Creates the descriptor-poll sender for the context. */
[[nodiscard]] inline auto make_poll_sender(io_context& context,
                                           async_io::descriptor_view descriptor,
                                           unsigned poll_mask) {
  return native_poll_sender(context, descriptor, poll_mask);
}

}  // namespace bnio::detail

#endif  // BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#endif  // BNIO_DETAIL_BSD_IO_CONTEXT_NATIVE_IO_FACTORIES_H_
