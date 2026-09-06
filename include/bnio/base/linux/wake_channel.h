/**
 * @file wake_channel.h
 * @brief Linux eventfd-backed wake channel.
 */

#pragma once
#ifndef BNIO_BASE_LINUX_WAKE_CHANNEL_H_
#define BNIO_BASE_LINUX_WAKE_CHANNEL_H_

#include <bnio/export.h>

namespace bnio::base {

/**
 * Cross-thread wake channel backed by eventfd(2).
 *
 * On Linux, a single eventfd serves as both the read and write end.
 * io_context writes to signal workers; each worker submits
 * IORING_POLL_ADD on the read side before sleeping.
 *
 * @warning A single write wakes ALL workers whose io_uring rings have
 * a pending IORING_POLL_ADD on this eventfd (minor thundering herd).
 * The per-worker overhead is one @c read returning EAGAIN plus one
 * pop_cpu_all() CAS — negligible for typical 4–8 worker concurrency.
 * During stop(), waking all workers is the desired behaviour.
 */
class BNIO_EXPORT wake_channel {
 public:
  /** Creates a closed wake channel. */
  wake_channel() noexcept;

  /** Closes the eventfd if it is open. */
  ~wake_channel() noexcept;

  wake_channel(const wake_channel&) = delete;
  wake_channel& operator=(const wake_channel&) = delete;

  /** Moves ownership of the eventfd. */
  wake_channel(wake_channel&& other) noexcept;

  /** Moves ownership of the eventfd. */
  wake_channel& operator=(wake_channel&& other) noexcept;

  /**
   * Opens an eventfd in non-blocking, close-on-exec mode.
   *
   * @return 0 on success, or a negative errno on failure.
   */
  int open() noexcept;

  /** Closes the eventfd if it is open. */
  void close() noexcept;

  /** Returns whether the eventfd is open. */
  [[nodiscard]] bool is_open() const noexcept;

  /** Returns the fd workers should register read interest on. */
  [[nodiscard]] int read_fd() const noexcept;

  /**
   * Writes one unit to the eventfd counter to wake workers.
   *
   * This is a best-effort signal — EAGAIN (counter saturated) is
   * treated as success because workers are already awake in that
   * case.
   *
   * @return 0 on success, or a negative errno (other than EAGAIN).
   */
  int wake() noexcept;

  /**
   * Drains all pending wake notifications from the eventfd.
   *
   * Called by the worker on waking to reset the counter so future
   * edge-triggered polling can detect new signals.
   *
   * @return 0 on success, or a negative errno.
   */
  int drain() noexcept;

 private:
  int fd_ = -1;
};

}  // namespace bnio::base

#endif  // BNIO_BASE_LINUX_WAKE_CHANNEL_H_
