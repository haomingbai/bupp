/**
 * @file file_common.h
 * @brief Shared helpers for kqueue file read/write operations.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_BSD_KQUEUE_OPERATIONS_FILE_COMMON_H_
#define BNIO_ASYNC_IO_BSD_KQUEUE_OPERATIONS_FILE_COMMON_H_

#include <sys/types.h>

#include <cerrno>
#include <cstdint>
#include <limits>

namespace bnio::async_io::bsd_native {

namespace detail {

/** Converts a positioned read/write syscall result into the operation
 *  result encoding: the transferred byte count on success, or a negative
 *  errno on failure. */
[[nodiscard]] inline int positioned_io_result(ssize_t result) noexcept {
  if (result >= 0) {
    return static_cast<int>(result);
  }
  return -errno;
}

/** Returns whether the offset is representable as off_t. */
[[nodiscard]] inline bool valid_file_offset(std::uint64_t offset) noexcept {
  return offset <=
         static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
}

/** Converts a nonblocking descriptor syscall result into the operation
 *  result encoding: the transferred byte count on success; EINTR, EAGAIN,
 *  and EWOULDBLOCK map to -EAGAIN so the caller retries after readiness;
 *  any other failure maps to a negative errno. */
[[nodiscard]] inline int nonblocking_descriptor_result(
    ssize_t result) noexcept {
  if (result >= 0) {
    return static_cast<int>(result);
  }
  const int error = errno;
  if (error == EINTR || error == EAGAIN || error == EWOULDBLOCK) {
    return -EAGAIN;
  }
  return -error;
}

}  // namespace detail

}  // namespace bnio::async_io::bsd_native

#endif  // BNIO_ASYNC_IO_BSD_KQUEUE_OPERATIONS_FILE_COMMON_H_
