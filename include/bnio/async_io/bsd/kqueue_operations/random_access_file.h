/**
 * @file random_access_file.h
 * @brief kqueue positioned read/write operations on random access files.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_BSD_KQUEUE_OPERATIONS_RANDOM_ACCESS_FILE_H_
#define BNIO_ASYNC_IO_BSD_KQUEUE_OPERATIONS_RANDOM_ACCESS_FILE_H_

#include <bnio/async_io/bsd/kqueue_operations/detail/io_request.h>
#include <bnio/async_io/bsd/kqueue_operations/detail/native_io.h>
#include <bnio/async_io/bsd/kqueue_operations/file_common.h>
#include <bnio/async_io/buffer_view.h>
#include <bnio/async_io/random_access_file.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <utility>

namespace bnio::async_io::bsd_native {

/**
 * A positioned read on a random access file: start performs pread at the
 * given offset without observing or advancing the kernel file position.
 * The caller guarantees a random access file, so no fstat dispatch happens
 * and the operation never waits on kqueue. Offsets beyond off_t are
 * rejected with EOVERFLOW before entering the kernel.
 */
class kqueue_random_access_read_request {
 public:
  /** Completion signals: set_value(ec, bytes) or set_stopped(). */
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  /** Constructs the request from the file, buffer, and byte offset. */
  kqueue_random_access_read_request(random_access_file file, buffer_view buffer,
                                    std::uint64_t offset) noexcept
      : file_(file), buffer_(buffer), offset_(offset) {}

  /** Prepares a read step for the file descriptor with @p helper. */
  void prepare(kqueue_helper& helper) noexcept {
    helper.prep_read(file_.native_handle());
  }

  /** Validates the buffer and performs the positioned read. */
  [[nodiscard]] int start_io() noexcept {
    if (buffer_.size > 0 && buffer_.data == nullptr) {
      return -EFAULT;
    }
    return perform_io();
  }

  /** Performs one positioned read at the stored offset; offsets beyond
   *  off_t are rejected with -EOVERFLOW before entering the kernel. */
  [[nodiscard]] int perform_io() noexcept {
    if (!detail::valid_file_offset(offset_)) {
      return -EOVERFLOW;
    }

    ssize_t result;
    do {
      result = ::pread(file_.native_handle(), buffer_.data,
                       detail::bounded_io_size(buffer_.size),
                       static_cast<off_t>(offset_));
    } while (result < 0 && errno == EINTR);
    return detail::positioned_io_result(result);
  }

  /** Positioned I/O is synchronous on a random access file: never wait. */
  [[nodiscard]] bool should_wait(int) const noexcept { return false; }

  /** Delivers the byte count to @p receiver, clamping negative results to
   *  zero. */
  template <class Receiver>
  void set_value(Receiver&& receiver, std::error_code ec, int result,
                 unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver), ec,
                     static_cast<std::size_t>(std::max(0, result)));
  }

 private:
  random_access_file file_;
  buffer_view buffer_;
  std::uint64_t offset_;
};

/**
 * A positioned write on a random access file: start performs pwrite at
 * the given offset without observing or advancing the kernel file position.
 * Never waits on kqueue; offsets beyond off_t are rejected with EOVERFLOW
 * before entering the kernel.
 */
class kqueue_random_access_write_request {
 public:
  /** Completion signals: set_value(ec, bytes) or set_stopped(). */
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  /** Constructs the request from the file, data, size, and byte offset. */
  kqueue_random_access_write_request(random_access_file file, const void* data,
                                     std::size_t size,
                                     std::uint64_t offset) noexcept
      : file_(file), data_(data), size_(size), offset_(offset) {}

  /** Prepares a write step for the file descriptor with @p helper. */
  void prepare(kqueue_helper& helper) noexcept {
    helper.prep_write(file_.native_handle());
  }

  /** Validates the buffer and performs the positioned write. */
  [[nodiscard]] int start_io() noexcept {
    if (size_ > 0 && data_ == nullptr) {
      return -EFAULT;
    }
    return perform_io();
  }

  /** Performs one positioned write at the stored offset; offsets beyond
   *  off_t are rejected with -EOVERFLOW before entering the kernel. */
  [[nodiscard]] int perform_io() noexcept {
    if (!detail::valid_file_offset(offset_)) {
      return -EOVERFLOW;
    }

    ssize_t result;
    do {
      result =
          ::pwrite(file_.native_handle(), data_, detail::bounded_io_size(size_),
                   static_cast<off_t>(offset_));
    } while (result < 0 && errno == EINTR);
    return detail::positioned_io_result(result);
  }

  /** Positioned I/O is synchronous on a random access file: never wait. */
  [[nodiscard]] bool should_wait(int) const noexcept { return false; }

  /** Delivers the byte count to @p receiver, clamping negative results to
   *  zero. */
  template <class Receiver>
  void set_value(Receiver&& receiver, std::error_code ec, int result,
                 unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver), ec,
                     static_cast<std::size_t>(std::max(0, result)));
  }

 private:
  random_access_file file_;
  const void* data_;
  std::size_t size_;
  std::uint64_t offset_;
};

/** Sender returned by kqueue_context::async_read for random access files.
 */
using kqueue_random_access_read_sender =
    detail::kqueue_ready_io_sender<kqueue_random_access_read_request>;

/** Sender returned by kqueue_context::async_write for random access
 *  files. */
using kqueue_random_access_write_sender =
    detail::kqueue_ready_io_sender<kqueue_random_access_write_request>;

/** @cond BNIO_DETAIL */

inline auto kqueue_context::async_read(random_access_file file,
                                       buffer_view buffer,
                                       std::uint64_t offset) {
  return kqueue_random_access_read_sender(
      *this, kqueue_random_access_read_request(file, buffer, offset));
}

inline auto kqueue_context::async_write(random_access_file file,
                                        const void* data, std::size_t size,
                                        std::uint64_t offset) {
  return kqueue_random_access_write_sender(
      *this, kqueue_random_access_write_request(file, data, size, offset));
}

/** @endcond */

}  // namespace bnio::async_io::bsd_native

#endif  // BNIO_ASYNC_IO_BSD_KQUEUE_OPERATIONS_RANDOM_ACCESS_FILE_H_
