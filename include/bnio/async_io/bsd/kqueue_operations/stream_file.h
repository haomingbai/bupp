/**
 * @file stream_file.h
 * @brief kqueue streaming descriptor read/write operations.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_BSD_KQUEUE_OPERATIONS_STREAM_FILE_H_
#define BNIO_ASYNC_IO_BSD_KQUEUE_OPERATIONS_STREAM_FILE_H_

#include <bnio/async_io/bsd/kqueue_operations/detail/io_request.h>
#include <bnio/async_io/bsd/kqueue_operations/detail/native_io.h>
#include <bnio/async_io/bsd/kqueue_operations/file_common.h>
#include <bnio/async_io/buffer_view.h>
#include <bnio/async_io/descriptor_view.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <utility>

namespace bnio::async_io::bsd_native {

/**
 * A streaming descriptor read. Regular files run ::read inline, advancing
 * the kernel file position; other descriptors are switched to O_NONBLOCK
 * and wait for kqueue readiness. The fstat classification is kept only to
 * select those two behaviors.
 */
class kqueue_stream_file_read_request {
 public:
  /** Completion signals: set_value(ec, bytes) or set_stopped(). */
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  /** Constructs the request from the descriptor and receive buffer. */
  kqueue_stream_file_read_request(descriptor_view descriptor,
                                  buffer_view buffer) noexcept
      : descriptor_(descriptor), buffer_(buffer) {}

  /** Registers the descriptor for read readiness with @p helper. */
  void prepare(kqueue_helper& helper) noexcept {
    helper.prep_read(descriptor_.native_handle());
  }

  /** Validates the buffer and attempts one immediate read. */
  [[nodiscard]] int start_io() noexcept {
    if (buffer_.size > 0 && buffer_.data == nullptr) {
      return -EFAULT;
    }
    return perform_io();
  }

  /** Classifies the descriptor once via fstat, then performs one read:
   *  inline for regular files, nonblocking otherwise. */
  [[nodiscard]] int perform_io() noexcept {
    if (!resolved_) {
      struct stat status {};
      if (::fstat(descriptor_.native_handle(), &status) != 0) {
        return -errno;
      }
      regular_file_ = S_ISREG(status.st_mode);
      if (!regular_file_) {
        const int nonblocking =
            detail::set_descriptor_nonblocking(descriptor_.native_handle());
        if (nonblocking < 0) {
          return nonblocking;
        }
      }
      resolved_ = true;
    }
    if (regular_file_) {
      ssize_t result;
      do {
        result = ::read(descriptor_.native_handle(), buffer_.data,
                        detail::bounded_io_size(buffer_.size));
      } while (result < 0 && errno == EINTR);
      return detail::positioned_io_result(result);
    }
    return detail::nonblocking_descriptor_result(
        ::read(descriptor_.native_handle(), buffer_.data,
               detail::bounded_io_size(buffer_.size)));
  }

  /** Returns whether a retryable result should wait for kqueue readiness;
   *  regular files never wait. */
  [[nodiscard]] bool should_wait(int result) const noexcept {
    return !regular_file_ && detail::should_wait(result);
  }

  /** Delivers the byte count to @p receiver, clamping negative results to
   *  zero. */
  template <class Receiver>
  void set_value(Receiver&& receiver, std::error_code ec, int result,
                 unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver), ec,
                     static_cast<std::size_t>(std::max(0, result)));
  }

 private:
  descriptor_view descriptor_;
  buffer_view buffer_;
  bool regular_file_ = false;
  bool resolved_ = false;
};

/**
 * A streaming descriptor write. Regular files run ::write inline, advancing
 * the kernel file position; other descriptors are switched to O_NONBLOCK
 * and wait for kqueue readiness.
 */
class kqueue_stream_file_write_request {
 public:
  /** Completion signals: set_value(ec, bytes) or set_stopped(). */
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  /** Constructs the request from the descriptor, data, and size. */
  kqueue_stream_file_write_request(descriptor_view descriptor, const void* data,
                                   std::size_t size) noexcept
      : descriptor_(descriptor), data_(data), size_(size) {}

  /** Registers the descriptor for write readiness with @p helper. */
  void prepare(kqueue_helper& helper) noexcept {
    helper.prep_write(descriptor_.native_handle());
  }

  /** Validates the buffer and attempts one immediate write. */
  [[nodiscard]] int start_io() noexcept {
    if (size_ > 0 && data_ == nullptr) {
      return -EFAULT;
    }
    return perform_io();
  }

  /** Classifies the descriptor once via fstat, then performs one write:
   *  inline for regular files, nonblocking otherwise. */
  [[nodiscard]] int perform_io() noexcept {
    if (!resolved_) {
      struct stat status {};
      if (::fstat(descriptor_.native_handle(), &status) != 0) {
        return -errno;
      }
      regular_file_ = S_ISREG(status.st_mode);
      if (!regular_file_) {
        const int nonblocking =
            detail::set_descriptor_nonblocking(descriptor_.native_handle());
        if (nonblocking < 0) {
          return nonblocking;
        }
      }
      resolved_ = true;
    }
    if (regular_file_) {
      ssize_t result;
      do {
        result = ::write(descriptor_.native_handle(), data_,
                         detail::bounded_io_size(size_));
      } while (result < 0 && errno == EINTR);
      return detail::positioned_io_result(result);
    }
    return detail::nonblocking_descriptor_result(::write(
        descriptor_.native_handle(), data_, detail::bounded_io_size(size_)));
  }

  /** Returns whether a retryable result should wait for kqueue readiness;
   *  regular files never wait. */
  [[nodiscard]] bool should_wait(int result) const noexcept {
    return !regular_file_ && detail::should_wait(result);
  }

  /** Delivers the byte count to @p receiver, clamping negative results to
   *  zero. */
  template <class Receiver>
  void set_value(Receiver&& receiver, std::error_code ec, int result,
                 unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver), ec,
                     static_cast<std::size_t>(std::max(0, result)));
  }

 private:
  descriptor_view descriptor_;
  const void* data_;
  std::size_t size_;
  bool regular_file_ = false;
  bool resolved_ = false;
};

/** Sender returned by kqueue_context::async_read for streaming
 *  descriptors. */
using kqueue_stream_file_read_sender =
    detail::kqueue_ready_io_sender<kqueue_stream_file_read_request>;

/** Sender returned by kqueue_context::async_write for streaming
 *  descriptors. */
using kqueue_stream_file_write_sender =
    detail::kqueue_ready_io_sender<kqueue_stream_file_write_request>;

/** @cond BNIO_DETAIL */

inline auto kqueue_context::async_read(descriptor_view descriptor,
                                       buffer_view buffer) {
  return kqueue_stream_file_read_sender(
      *this, kqueue_stream_file_read_request(descriptor, buffer));
}

inline auto kqueue_context::async_write(descriptor_view descriptor,
                                        const void* data, std::size_t size) {
  return kqueue_stream_file_write_sender(
      *this, kqueue_stream_file_write_request(descriptor, data, size));
}

/** @endcond */

/** Streaming read that forwards the raw (ec, result, flags) completion. */
class kqueue_raw_stream_file_read_request
    : public kqueue_stream_file_read_request {
 public:
  /** Inherits the base class constructor. */
  using kqueue_stream_file_read_request::kqueue_stream_file_read_request;

  /** Completion signals: set_value(ec, native result, flags) or
   *  set_stopped(). */
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::error_code, int,
                                                      unsigned),
                                   bexec::set_stopped_t()>;

  /** Forwards the raw native result and flags to @p receiver unchanged. */
  template <class Receiver>
  void set_value(Receiver&& receiver, std::error_code ec, int result,
                 unsigned flags) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver), ec, result, flags);
  }
};

/** Streaming write that forwards the raw (ec, result, flags) completion. */
class kqueue_raw_stream_file_write_request
    : public kqueue_stream_file_write_request {
 public:
  /** Constructs the request from the descriptor and buffer view. */
  kqueue_raw_stream_file_write_request(descriptor_view descriptor,
                                       buffer_view buffer)
      : kqueue_stream_file_write_request(descriptor, buffer.data, buffer.size) {
  }

  /** Completion signals: set_value(ec, native result, flags) or
   *  set_stopped(). */
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::error_code, int,
                                                      unsigned),
                                   bexec::set_stopped_t()>;

  /** Forwards the raw native result and flags to @p receiver unchanged. */
  template <class Receiver>
  void set_value(Receiver&& receiver, std::error_code ec, int result,
                 unsigned flags) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver), ec, result, flags);
  }
};

/** Low-level streaming read operation backed by an objectized request. */
template <class Receiver>
class kqueue_read_operation
    : public detail::kqueue_ready_io_operation<
          kqueue_raw_stream_file_read_request, Receiver> {
 public:
  /** Constructs the operation from its context, descriptor, buffer, and
   *  receiver. */
  kqueue_read_operation(kqueue_context& context, descriptor_view descriptor,
                        buffer_view buffer, Receiver receiver)
      : detail::kqueue_ready_io_operation<kqueue_raw_stream_file_read_request,
                                          Receiver>(
            context, kqueue_raw_stream_file_read_request(descriptor, buffer),
            std::move(receiver)) {}
};

/** Low-level streaming write operation backed by an objectized request. */
template <class Receiver>
class kqueue_write_operation
    : public detail::kqueue_ready_io_operation<
          kqueue_raw_stream_file_write_request, Receiver> {
 public:
  /** Constructs the operation from its context, descriptor, buffer, and
   *  receiver. */
  kqueue_write_operation(kqueue_context& context, descriptor_view descriptor,
                         buffer_view buffer, Receiver receiver)
      : detail::kqueue_ready_io_operation<kqueue_raw_stream_file_write_request,
                                          Receiver>(
            context, kqueue_raw_stream_file_write_request(descriptor, buffer),
            std::move(receiver)) {}
};

}  // namespace bnio::async_io::bsd_native

#endif  // BNIO_ASYNC_IO_BSD_KQUEUE_OPERATIONS_STREAM_FILE_H_
