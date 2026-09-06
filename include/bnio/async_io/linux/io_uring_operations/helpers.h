/**
 * @file helpers.h
 * @brief io_uring operation helper types.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_LINUX_IO_URING_OPERATIONS_HELPERS_H_
#define BNIO_ASYNC_IO_LINUX_IO_URING_OPERATIONS_HELPERS_H_

#include <bnio/async_io/time.h>
#include <bnio/base/linux/submission_queue_entry.h>
#include <linux/io_uring.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace bnio::async_io::linux_native {

namespace detail {

/**
 * Converts a chrono duration to Linux's io_uring timeout format.
 */
template <class Rep, class Period>
[[nodiscard]] constexpr __kernel_timespec to_kernel_timespec(
    std::chrono::duration<Rep, Period> value) noexcept {
  auto seconds = std::chrono::duration_cast<std::chrono::seconds>(value);
  auto nanoseconds =
      std::chrono::duration_cast<std::chrono::nanoseconds>(value - seconds);

  if (nanoseconds.count() < 0) {
    --seconds;
    nanoseconds += std::chrono::seconds(1);
  }

  __kernel_timespec result{};
  result.tv_sec = static_cast<decltype(result.tv_sec)>(seconds.count());
  result.tv_nsec = static_cast<decltype(result.tv_nsec)>(nanoseconds.count());
  return result;
}

/**
 * Converts a chrono time point's epoch duration to Linux's timeout format.
 */
template <class Clock, class Duration>
[[nodiscard]] constexpr __kernel_timespec to_kernel_timespec(
    std::chrono::time_point<Clock, Duration> value) noexcept {
  return to_kernel_timespec(value.time_since_epoch());
}

/**
 * Timeout storage and SQE preparation helper.
 */
class timeout_request {
 public:
  /**
   * Creates a zero-duration timeout request.
   */
  timeout_request() noexcept = default;

  /**
   * Creates a timeout request from a chrono duration.
   */
  explicit timeout_request(bnio::async_io::duration timeout) noexcept {
    reset(timeout);
  }

  /**
   * Replaces the stored relative timeout.
   */
  void reset(bnio::async_io::duration timeout) noexcept {
    timeout_ = to_kernel_timespec(timeout);
  }

  /**
   * Prepares a timeout SQE using the stored timeout.
   */
  void prepare_timeout(bnio::base::submission_queue_entry& sqe, unsigned count,
                       unsigned flags) noexcept {
    sqe.prep_timeout(&timeout_, count, flags);
  }

  /**
   * Prepares a timeout update SQE using the stored timeout.
   */
  void prepare_timeout_update(bnio::base::submission_queue_entry& sqe,
                              std::uint64_t user_data,
                              unsigned flags) noexcept {
    sqe.prep_timeout_update(&timeout_, user_data, flags);
  }

 private:
  __kernel_timespec timeout_{};
};

/**
 * Clamps a buffer size to the unsigned maximum accepted by io_uring SQE
 * length fields.
 */
[[nodiscard]] constexpr unsigned bounded_io_size(std::size_t size) noexcept {
  constexpr auto max_size =
      static_cast<std::size_t>(std::numeric_limits<unsigned>::max());
  return static_cast<unsigned>(size > max_size ? max_size : size);
}

}  // namespace detail

}  // namespace bnio::async_io::linux_native

#endif  // BNIO_ASYNC_IO_LINUX_IO_URING_OPERATIONS_HELPERS_H_
