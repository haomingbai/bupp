/**
 * @file system.h
 * @brief Platform detection and feature macros.
 */

#pragma once
#ifndef BNIO_CONFIG_SYSTEM_H_
/** Include guard. */
#define BNIO_CONFIG_SYSTEM_H_

#if defined(__linux__)
/** Defined when compiling for Linux. */
#define BNIO_SYSTEM_LINUX 1
/** Defined when compiling for any POSIX-compliant system. */
#define BNIO_SYSTEM_POSIX 1
#elif defined(__APPLE__) && defined(__MACH__)
/** Defined when compiling for Apple platforms (macOS, iOS, ...). */
#define BNIO_SYSTEM_DARWIN 1
/** Defined when compiling for any BSD system. */
#define BNIO_SYSTEM_BSD 1
/** Defined when compiling for any POSIX-compliant system. */
#define BNIO_SYSTEM_POSIX 1
#elif defined(__FreeBSD__)
/** Defined when compiling for FreeBSD. */
#define BNIO_SYSTEM_FREEBSD 1
/** Defined when compiling for any BSD system. */
#define BNIO_SYSTEM_BSD 1
/** Defined when compiling for any POSIX-compliant system. */
#define BNIO_SYSTEM_POSIX 1
#elif defined(_WIN32)
/** Defined when compiling for Windows. */
#define BNIO_SYSTEM_WINDOWS 1
#else
/**
 * Fallback defined when the target platform is not recognised; bnio's
 * native I/O backends are unavailable in this configuration.
 */
#define BNIO_SYSTEM_UNKNOWN 1
#endif

#endif  // BNIO_CONFIG_SYSTEM_H_
