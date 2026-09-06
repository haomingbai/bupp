/**
 * @file options.h
 * @brief kqueue context options.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_BSD_KQUEUE_CONTEXT_BASE_OPTIONS_H_
#define BNIO_ASYNC_IO_BSD_KQUEUE_CONTEXT_BASE_OPTIONS_H_

#include <cstdint>

namespace bnio::async_io::bsd_native {

/**
 * Options used to initialize a kqueue-backed async I/O context.
 */
struct kqueue_context_options {
  /**
   * Capacity hint retained for parity with the io_uring context options.
   *
   * The context clamps it to at least 1 but does not consult it: the
   * kevent event buffer is sized from event_batch_window, and wait queues
   * live inside the registration nodes of inflight operations.
   */
  unsigned entries = 256;

  /**
   * Maximum number of ready kevents collected in one batch.
   */
  unsigned event_batch_window = 64;

  /**
   * Number of non-blocking event collection rounds before the run loop
   * blocks in kevent().
   */
  unsigned wait_spin_count = 1;

  /**
   * Maximum number of ready event tasks kept on the local queue.
   *
   * Ready batches at or below this count are always dispatched inline to
   * the worker's own CPU queue, whatever local_queue_threshold says.
   */
  unsigned event_inline_completion_threshold = 64;

  /**
   * Upper bound for tasks dispatched to the local queue per ready-tasks
   * pass. When the cumulative local-queue sink exceeds this value,
   * remaining tasks are published to the shared CPU queue instead.
   *
   * 0 (the default) means no limit.
   */
  unsigned local_queue_threshold = 0;

  /**
   * kevent identifier for this context's own EVFILT_USER wakeup trigger,
   * used when no shared wake channel is available (standalone mode).
   */
  std::uintptr_t wakeup_ident = 1;
};

}  // namespace bnio::async_io::bsd_native

#endif  // BNIO_ASYNC_IO_BSD_KQUEUE_CONTEXT_BASE_OPTIONS_H_
