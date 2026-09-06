/**
 * @file native_io.h
 * @brief Shared native descriptor helpers for kqueue operations.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_BSD_KQUEUE_OPERATIONS_DETAIL_NATIVE_IO_H_
#define BNIO_ASYNC_IO_BSD_KQUEUE_OPERATIONS_DETAIL_NATIVE_IO_H_

#include <fcntl.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstddef>

namespace bnio::async_io::bsd_native::detail {

/** Clamps the requested byte count to the maximum the native I/O syscalls
 *  accept (INT_MAX). */
[[nodiscard]] inline std::size_t bounded_io_size(std::size_t size) noexcept {
  return std::min(size, static_cast<std::size_t>(INT_MAX));
}

/** Enables O_NONBLOCK on the descriptor if not already set.
 *
 *  @return 0 on success, or a negative errno on failure.
 */
[[nodiscard]] inline int set_descriptor_nonblocking(int descriptor) noexcept {
  const int flags = ::fcntl(descriptor, F_GETFL, 0);
  if (flags < 0) {
    return -errno;
  }
  if ((flags & O_NONBLOCK) != 0) {
    return 0;
  }
  return ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0 ? 0 : -errno;
}

}  // namespace bnio::async_io::bsd_native::detail

#endif  // BNIO_ASYNC_IO_BSD_KQUEUE_OPERATIONS_DETAIL_NATIVE_IO_H_
