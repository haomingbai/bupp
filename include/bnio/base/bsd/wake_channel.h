/**
 * @file wake_channel.h
 * @brief BSD pipe-backed wake channel.
 */

#pragma once
#ifndef BNIO_BASE_BSD_WAKE_CHANNEL_H_
#define BNIO_BASE_BSD_WAKE_CHANNEL_H_

#include <bnio/export.h>

namespace bnio::base {

/**
 * Cross-thread wake channel backed by pipe(2).
 *
 * On BSD, a pipe provides separate read and write file descriptors.
 * io_context writes to the write end to signal workers; each worker
 * registers EVFILT_READ | EV_CLEAR on the read end before sleeping.
 *
 * @warning A single write wakes ALL workers whose kqueues have
 * EVFILT_READ registered on the read end (minor thundering herd).
 * The per-worker overhead is one @c read() returning EAGAIN plus one
 * pop_cpu_all() CAS — negligible for typical 4–8 worker concurrency.
 * During stop(), waking all workers is the desired behaviour.
 */
class BNIO_EXPORT wake_channel {
 public:
  /** Creates a closed wake channel. */
  wake_channel() noexcept;

  /** Closes both pipe ends if they are open. */
  ~wake_channel() noexcept;

  wake_channel(const wake_channel&) = delete;
  wake_channel& operator=(const wake_channel&) = delete;

  /** Moves ownership of both pipe ends. */
  wake_channel(wake_channel&& other) noexcept;

  /** Moves ownership of both pipe ends. */
  wake_channel& operator=(wake_channel&& other) noexcept;

  /**
   * Opens a pipe with non-blocking read and write ends.
   *
   * @return 0 on success, or a negative errno on failure.
   */
  int open() noexcept;

  /** Closes both pipe ends if they are open. */
  void close() noexcept;

  /** Returns whether the pipe is open. */
  [[nodiscard]] bool is_open() const noexcept;

  /** Returns the read-end fd workers register EVFILT_READ on. */
  [[nodiscard]] int read_fd() const noexcept;

  /** Returns the write-end fd io_context writes to for wake. */
  [[nodiscard]] int write_fd() const noexcept;

  /**
   * Writes one byte to the pipe to wake workers.
   *
   * @return 0 on success, -EAGAIN if the pipe buffer is full and
   *         no data was written, or another negative errno on failure.
   */
  int wake() noexcept;

  /**
   * Drains all pending wake bytes from the pipe.
   *
   * Called by a worker on waking to empty the pipe so future
   * edge-triggered EVFILT_READ can detect new signals.
   *
   * @return 0 on success, or a negative errno.
   */
  int drain() noexcept;

 private:
  int read_fd_ = -1;
  int write_fd_ = -1;
};

}  // namespace bnio::base

#endif  // BNIO_BASE_BSD_WAKE_CHANNEL_H_
