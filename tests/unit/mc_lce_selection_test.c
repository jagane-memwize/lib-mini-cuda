/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * Host-only tests for copy-engine selection.  NV_ESC_RM_CONTROL is wrapped so
 * the real RM and a GPU are not required.
 */

#include <stdarg.h>
#include <stdint.h>         /* uintptr_t — used to stage the caps table */
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

#include "nvos.h"
#include "nv-ioctl-numbers.h"
#include "nv_escape.h"
#include "class/cl2080_notification.h"
#include "ctrl/ctrl2080/ctrl2080ce.h"

#include "internal/log.h"
#include "internal/rm.h"

static NV2080_CTRL_CE_GET_ALL_CAPS_PARAMS mock_caps;
static int failures;
static int ioctl_calls;

#define EXPECT(cond)                                                  \
  do                                                                  \
  {                                                                   \
    if (!(cond))                                                      \
    {                                                                 \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                     \
    }                                                                 \
  } while (0)

#define SET_CAP(tbl, cap) ((tbl)[(1 ? cap)] |= (0 ? cap))

int __wrap_ioctl(int fd, unsigned long request, ...)
{
  NVOS54_PARAMETERS *p;
  va_list             args;

  (void)fd;
  ioctl_calls++;
  EXPECT(_IOC_NR(request) == (NV_ESC_RM_CONTROL & 0xff));
  EXPECT(_IOC_SIZE(request) == sizeof(*p));

  va_start(args, request);
  p = va_arg(args, NVOS54_PARAMETERS *);
  va_end(args);

  EXPECT(p != NULL);
  if (p == NULL) return -1;
  EXPECT(p->cmd == NV2080_CTRL_CMD_CE_GET_ALL_CAPS);
  EXPECT(p->paramsSize == sizeof(mock_caps));

  memcpy((void *)(uintptr_t)p->params, &mock_caps, sizeof(mock_caps));
  p->status = NV_OK;
  return 0;
}

static void reset_caps(void)
{
  memset(&mock_caps, 0, sizeof(mock_caps));
  ioctl_calls = 0;
}

static void test_low_index(void)
{
  reset_caps();
  mock_caps.present = 1ULL << 2;
  SET_CAP(mock_caps.capsTbl[2], NV2080_CTRL_CE_CAPS_CE_SYSMEM);
  EXPECT(pick_non_grce_lce(7, 1, 2) == NV2080_ENGINE_TYPE_COPY2);
  EXPECT(ioctl_calls == 1);
}

static void test_discontinuous_copy10_id(void)
{
  reset_caps();
  mock_caps.present = 1ULL << 10;
  SET_CAP(mock_caps.capsTbl[10], NV2080_CTRL_CE_CAPS_CE_SYSMEM);
  EXPECT(pick_non_grce_lce(7, 1, 2) == NV2080_ENGINE_TYPE_COPY10);
  EXPECT(NV2080_ENGINE_TYPE_COPY10 != NV2080_ENGINE_TYPE_COPY0 + 10);
}

static void test_filters_grce_and_non_sysmem(void)
{
  reset_caps();
  mock_caps.present = (1ULL << 1) | (1ULL << 2) | (1ULL << 4);
  SET_CAP(mock_caps.capsTbl[2], NV2080_CTRL_CE_CAPS_CE_SYSMEM);
  SET_CAP(mock_caps.capsTbl[2], NV2080_CTRL_CE_CAPS_CE_GRCE);
  SET_CAP(mock_caps.capsTbl[4], NV2080_CTRL_CE_CAPS_CE_SYSMEM);
  EXPECT(pick_non_grce_lce(7, 1, 2) == NV2080_ENGINE_TYPE_COPY4);
}

static void test_no_suitable_engine(void)
{
  reset_caps();
  mock_caps.present = (1ULL << 1) | (1ULL << 2);
  SET_CAP(mock_caps.capsTbl[2], NV2080_CTRL_CE_CAPS_CE_SYSMEM);
  SET_CAP(mock_caps.capsTbl[2], NV2080_CTRL_CE_CAPS_CE_GRCE);
  EXPECT(pick_non_grce_lce(7, 1, 2) == (NvU32)-1);
}

int main(void)
{
  test_low_index();
  test_discontinuous_copy10_id();
  test_filters_grce_and_non_sysmem();
  test_no_suitable_engine();

  if (failures != 0)
  {
    fprintf(stderr, "%d LCE-selection assertion(s) failed\n", failures);
    return 1;
  }

  puts("PASS: copy-engine selection");
  return 0;
}
