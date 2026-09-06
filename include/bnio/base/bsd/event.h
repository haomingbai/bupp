/**
 * @file event.h
 * @brief Wrapper for a kevent structure.
 */

#pragma once
#ifndef BNIO_BASE_BSD_EVENT_H_
#define BNIO_BASE_BSD_EVENT_H_

#include <bnio/export.h>
#include <sys/event.h>

#include <cstdint>
#include <type_traits>

namespace bnio::base {

/**
 * Value wrapper for a native kqueue event.
 *
 * @see kevent
 */
class BNIO_EXPORT event {
 public:
  /**
   * Creates a zero-initialized event.
   */
  event() noexcept;

  /**
   * Creates an event from the native kevent fields.
   *
   * @see EV_SET
   */
  event(uintptr_t ident, int16_t filter, uint16_t flags, uint32_t fflags,
        intptr_t data, void* udata) noexcept;

  /**
   * Wraps an existing native kevent value.
   */
  explicit event(const struct kevent& raw_event) noexcept;

  /**
   * Sets all native kevent fields.
   *
   * @see EV_SET
   */
  void set(uintptr_t ident, int16_t filter, uint16_t flags, uint32_t fflags,
           intptr_t data, void* udata) noexcept;

  /**
   * Returns the wrapped native event.
   */
  [[nodiscard]] struct kevent* raw() noexcept;

  /**
   * Returns the wrapped native event.
   */
  [[nodiscard]] const struct kevent* raw() const noexcept;

  /**
   * Returns the native event identifier.
   */
  [[nodiscard]] uintptr_t ident() const noexcept;

  /**
   * Returns the native event filter.
   */
  [[nodiscard]] int16_t filter() const noexcept;

  /**
   * Returns the native event flags.
   */
  [[nodiscard]] uint16_t flags() const noexcept;

  /**
   * Returns the native filter-specific flags.
   */
  [[nodiscard]] uint32_t fflags() const noexcept;

  /**
   * Returns the native event data field.
   */
  [[nodiscard]] intptr_t data() const noexcept;

  /**
   * Returns the native user data pointer.
   */
  [[nodiscard]] void* udata() const noexcept;

  /**
   * Sets the native event identifier.
   */
  void set_ident(uintptr_t ident) noexcept;

  /**
   * Sets the native event filter.
   */
  void set_filter(int16_t filter) noexcept;

  /**
   * Sets the native event flags.
   */
  void set_flags(uint16_t flags) noexcept;

  /**
   * Sets the native filter-specific flags.
   */
  void set_fflags(uint32_t fflags) noexcept;

  /**
   * Sets the native event data field.
   */
  void set_data(intptr_t data) noexcept;

  /**
   * Sets the native user data pointer.
   */
  void set_udata(void* udata) noexcept;

  /**
   * Returns whether the event has the EV_ERROR flag.
   */
  [[nodiscard]] bool has_error() const noexcept;

  /**
   * Returns whether the event has the EV_EOF flag.
   */
  [[nodiscard]] bool has_eof() const noexcept;

 private:
  struct kevent event_ {};
};

static_assert(std::is_standard_layout_v<event>);
static_assert(sizeof(event) == sizeof(struct kevent));
static_assert(alignof(event) == alignof(struct kevent));

}  // namespace bnio::base

#endif  // BNIO_BASE_BSD_EVENT_H_
