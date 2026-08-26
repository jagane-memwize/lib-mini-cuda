/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * Host-only tests for the strict RM version handshake.  ioctl is wrapped at
 * link time so these tests neither open NVIDIA device files nor require a GPU.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

#include "nv-ioctl.h"
#include "nv-ioctl-numbers.h"

#include "internal/rm.h"

typedef enum
{
  MOCK_MATCH,
  MOCK_MISMATCH,
  MOCK_UNRECOGNIZED,
  MOCK_RECOGNIZED_ERROR,
} mock_mode_t;

static mock_mode_t mock_mode;
static int         ioctl_calls;
static int         failures;

#define EXPECT(cond)                                                  \
  do                                                                  \
  {                                                                   \
    if (!(cond))                                                      \
    {                                                                 \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                     \
    }                                                                 \
  } while (0)

int __wrap_ioctl(int fd, unsigned long request, ...)
{
  nv_ioctl_rm_api_version_t *p;
  va_list                    args;

  (void)fd;
  ioctl_calls++;
  EXPECT(_IOC_NR(request) == (NV_ESC_CHECK_VERSION_STR & 0xff));
  EXPECT(_IOC_SIZE(request) == sizeof(*p));

  va_start(args, request);
  p = va_arg(args, nv_ioctl_rm_api_version_t *);
  va_end(args);

  EXPECT(p != NULL);
  if (p == NULL)
  {
    errno = EFAULT;
    return -1;
  }

  EXPECT(p->cmd == NV_RM_API_VERSION_CMD_STRICT);
  EXPECT(strcmp(p->versionString, MC_NVIDIA_DRIVER_VERSION) == 0);

  switch (mock_mode)
  {
  case MOCK_MATCH:
    p->reply = NV_RM_API_VERSION_REPLY_RECOGNIZED;
    return 0;

  case MOCK_MISMATCH:
    p->reply = NV_RM_API_VERSION_REPLY_RECOGNIZED;
    snprintf(p->versionString, sizeof(p->versionString), "%s", "610.57.04");
    errno = EINVAL;
    return -1;

  case MOCK_UNRECOGNIZED:
    errno = ENOTTY;
    return -1;

  case MOCK_RECOGNIZED_ERROR:
    p->reply = NV_RM_API_VERSION_REPLY_RECOGNIZED;
    errno    = EIO;
    return -1;
  }

  errno = EINVAL;
  return -1;
}

static void test_match(void)
{
  char actual[NV_RM_API_VERSION_STRING_LENGTH] = {};

  mock_mode   = MOCK_MATCH;
  ioctl_calls = 0;
  EXPECT(rm_check_driver_version(7, MC_NVIDIA_DRIVER_VERSION, actual,
                                 sizeof(actual))
         == MC_RM_VERSION_OK);
  EXPECT(ioctl_calls == 1);
  EXPECT(strcmp(actual, MC_NVIDIA_DRIVER_VERSION) == 0);
}

static void test_mismatch(void)
{
  char actual[NV_RM_API_VERSION_STRING_LENGTH] = {};

  mock_mode   = MOCK_MISMATCH;
  ioctl_calls = 0;
  EXPECT(rm_check_driver_version(7, MC_NVIDIA_DRIVER_VERSION, actual,
                                 sizeof(actual))
         == MC_RM_VERSION_MISMATCH);
  EXPECT(ioctl_calls == 1);
  EXPECT(strcmp(actual, "610.57.04") == 0);
}

static void test_unrecognized(void)
{
  char actual[NV_RM_API_VERSION_STRING_LENGTH] = {};

  mock_mode   = MOCK_UNRECOGNIZED;
  ioctl_calls = 0;
  EXPECT(rm_check_driver_version(7, MC_NVIDIA_DRIVER_VERSION, actual,
                                 sizeof(actual))
         == MC_RM_VERSION_ERROR);
  EXPECT(ioctl_calls == 1);
}

static void test_unrelated_ioctl_error(void)
{
  char actual[NV_RM_API_VERSION_STRING_LENGTH] = {};

  mock_mode   = MOCK_RECOGNIZED_ERROR;
  ioctl_calls = 0;
  EXPECT(rm_check_driver_version(7, MC_NVIDIA_DRIVER_VERSION, actual,
                                 sizeof(actual))
         == MC_RM_VERSION_ERROR);
  EXPECT(ioctl_calls == 1);
  EXPECT(strcmp(actual, MC_NVIDIA_DRIVER_VERSION) == 0);
}

static void test_invalid_input_does_not_issue_ioctl(void)
{
  char oversized[NV_RM_API_VERSION_STRING_LENGTH + 1];

  memset(oversized, 'x', sizeof(oversized));
  oversized[sizeof(oversized) - 1] = '\0';

  ioctl_calls = 0;
  EXPECT(rm_check_driver_version(-1, MC_NVIDIA_DRIVER_VERSION, NULL, 0)
         == MC_RM_VERSION_ERROR);
  EXPECT(rm_check_driver_version(7, oversized, NULL, 0) == MC_RM_VERSION_ERROR);
  EXPECT(ioctl_calls == 0);
}

int main(void)
{
  test_match();
  test_mismatch();
  test_unrecognized();
  test_unrelated_ioctl_error();
  test_invalid_input_does_not_issue_ioctl();

  if (failures != 0)
  {
    fprintf(stderr, "%d version-check assertion(s) failed\n", failures);
    return 1;
  }

  puts("PASS: RM strict version check");
  return 0;
}
