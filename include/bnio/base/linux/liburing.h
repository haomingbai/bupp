/**
 * @file liburing.h
 * @brief liburing capability detection.
 */

#pragma once
#ifndef BNIO_BASE_LINUX_LIBURING_H_
#define BNIO_BASE_LINUX_LIBURING_H_

#if defined(__has_include)
#if __has_include(<linux/openat2.h>)
#include <linux/openat2.h>
#endif
#endif

#include <liburing.h>

#include <cstdint>

namespace bnio::base::detail {

/** Value of IORING_SETUP_COOP_TASKRUN when the kernel headers define it,
 *  or 0 when building against older headers. */
#ifdef IORING_SETUP_COOP_TASKRUN
inline constexpr unsigned io_uring_setup_coop_taskrun =
    IORING_SETUP_COOP_TASKRUN;
#else
inline constexpr unsigned io_uring_setup_coop_taskrun = 0;
#endif

/** Value of IORING_SETUP_SINGLE_ISSUER when the kernel headers define it,
 *  or 0 when building against older headers. */
#ifdef IORING_SETUP_SINGLE_ISSUER
inline constexpr unsigned io_uring_setup_single_issuer =
    IORING_SETUP_SINGLE_ISSUER;
#else
inline constexpr unsigned io_uring_setup_single_issuer = 0;
#endif

/** Value of IORING_SETUP_R_DISABLED when the kernel headers define it,
 *  or 0 when building against older headers. */
#ifdef IORING_SETUP_R_DISABLED
inline constexpr unsigned io_uring_setup_r_disabled = IORING_SETUP_R_DISABLED;
#else
inline constexpr unsigned io_uring_setup_r_disabled = 0;
#endif

/** Stores a 64-bit user data tag in an SQE. */
inline void io_uring_sqe_set_data64(io_uring_sqe* sqe,
                                    std::uint64_t data) noexcept {
  sqe->user_data = data;
}

/** Returns the 64-bit user data tag carried by a CQE. */
[[nodiscard]] inline std::uint64_t io_uring_cqe_get_data64(
    const io_uring_cqe* cqe) noexcept {
  return cqe->user_data;
}

}  // namespace bnio::base::detail

#endif  // BNIO_BASE_LINUX_LIBURING_H_
