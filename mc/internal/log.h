/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * internal/log.h — library-wide logging and the fatal-assert macro.
 *
 * Split out of the former mc_internal.h so every translation unit can log
 * without pulling in the full context layout.
 */
#ifndef MC_INTERNAL_LOG_H
#define MC_INTERNAL_LOG_H

#include <stdio.h>          /* fprintf, fputc — used in log macros */
#include <stdlib.h>         /* abort — used in CHECK macro */

/* Verbose logging gate (env: MC_VERBOSE).  Cached after first call.
 * Defined in driver/rm.c (always linked). */
int mc_debug(void);

/* Library-wide log macros.  All call sites expand to direct fprintf;
 * mc_debug() gates DEBUG output but the macro expands unconditionally
 * so log-string formatting cost vanishes when MC_VERBOSE is unset.
 *
 * Kept in this header (rather than per-file) so every TU sees the
 * same definition; otherwise a copy in each .c file would drift. */
#define DEBUG_LOG(...)                                       \
  do                                                         \
  {                                                          \
    if (mc_debug())                                          \
    {                                                        \
      fprintf(stderr, "[mc] DEBUG: " __VA_ARGS__);    \
      fputc('\n', stderr);                                   \
    }                                                        \
  } while (0)
#define WARN_LOG(...)                                        \
  do                                                         \
  {                                                          \
    fprintf(stderr, "[mc] WARN: " __VA_ARGS__);       \
    fputc('\n', stderr);                                     \
  } while (0)
#define ERROR_LOG(...)                                       \
  do                                                         \
  {                                                          \
    fprintf(stderr, "[mc] ERROR: " __VA_ARGS__);      \
    fputc('\n', stderr);                                     \
  } while (0)

/* Library-internal fatal check.  Library users should not have their
 * process aborted by library code, so this is used sparingly:
 * we log and abort only for invariant violations that indicate a bug
 * (asserts in a stricter language).  Setup/ioctl failures return
 * mc_status_t instead. */
#define CHECK(cond, ...)                                              \
  do                                                                  \
  {                                                                   \
    if (!(cond))                                                      \
    {                                                                 \
      fprintf(stderr, "[mc] FATAL %s:%d: ", __FILE__, __LINE__); \
      fprintf(stderr, __VA_ARGS__);                                   \
      fputc('\n', stderr);                                            \
      abort();                                                        \
    }                                                                 \
  } while (0)

#endif /* MC_INTERNAL_LOG_H */
