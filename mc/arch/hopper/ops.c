/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * arch/hopper/ops.c — the Hopper behaviour hooks.
 *
 * Four functions, each covering a divergence the descriptor cannot express as
 * data.  Everything else about Hopper is in backend.c as a struct initialiser.
 *
 * This file is also the only place in the library that names Hopper's USERD
 * struct: the layout belongs to the channel class, so it lives with the class,
 * not in the shared submission code.
 */

#include <stdbool.h>
#include <stdint.h>

#include "nvos.h"
#include "class/cl2080_notification.h"  /* NV2080_ENGINE_TYPE_GR0 */
#include "class/clc86f.h"               /* HopperAControlGPFifo */

#include "hal/arch.h"
#include "internal/rm.h"
#include "internal/types.h"

/* ── USERMODE allocation ──────────────────────────────────────────────────
 *
 * HOPPER_USERMODE_A is RS_OPTIONAL(NV_HOPPER_USERMODE_A_PARAMS), so the
 * parameters carry the BAR0-vs-BAR1 choice:
 *
 *   bar1 == false: BAR0 mapping.  Allocated for structural parity with
 *                  libcuda (which allocates both) but never written after
 *                  setup.
 *   bar1 == true:  BAR1 mapping.  The doorbell libmc actually rings, and the
 *                  aperture the carrier VA spaces install a PTE against.
 *
 * Both expose the VF doorbell at the same offset within the window.
 *
 * Pre-Hopper classes are RS_NONE and take no parameters at all, which is why
 * this is a hook rather than a descriptor field -- there is no struct to fill
 * in differently, there is no struct.
 */
static NvHandle hopper_alloc_usermode(int ctl_fd, const mc_arch_desc_t *arch,
                                      NvHandle root, NvHandle subdevice,
                                      bool bar1)
{
    NV_HOPPER_USERMODE_A_PARAMS params = {
        .bBar1Mapping = bar1,
        .bPriv        = NV_FALSE,
    };
    return rm_alloc(ctl_fd, root, subdevice, arch->cls.usermode, &params);
}

/* ── USERD access ─────────────────────────────────────────────────────────
 *
 * Nvc86fControl publishes GPGet at 0x88 and GPPut at 0x8c.  Ampere's
 * Nvc56fControl is identical; Blackwell's Nvc96fControl drops GPGet
 * entirely, which is why the getter is a try_ form in the interface.
 */
#define USERD(p) ((volatile HopperAControlGPFifo *)(p))

static void hopper_userd_set_gpput(void *userd, uint32_t value)
{
    USERD(userd)->GPPut = value;
}

static uint32_t hopper_userd_get_gpput(const void *userd)
{
    return USERD(userd)->GPPut;
}

static bool hopper_userd_try_get_gpget(const void *userd, uint32_t *out_gpget)
{
    if (userd == NULL || out_gpget == NULL)
        return false;
    *out_gpget = USERD(userd)->GPGet;
    return true;
}

/* ── Engine policy ────────────────────────────────────────────────────────
 *
 * Compute runs on GR0 (runlist 0).  Every copy-engine role shares the single
 * non-GRCE LCE that copy-engine selection picked.
 *
 * Sharing one LCE across all CE roles is deliberate rather than incidental:
 * Hopper allows multiple channels per engine, and the roles are used
 * sequentially by a single-threaded API, so spreading them buys nothing.  A
 * part whose copy-engine population makes that a poor choice can return a
 * different engine per role without any caller changing.
 */
static NvU32 hopper_engine_for_role(const mc_arch_desc_t *arch,
                                    mc_arch_role_t role, NvU32 selected_lce)
{
    (void)arch;
    if ((mc_channel_role_t)role == MC_ROLE_COMPUTE)
        return NV2080_ENGINE_TYPE_GR0;
    return selected_lce;
}

const mc_arch_ops_t mc_arch_hopper_ops = {
    .alloc_usermode      = hopper_alloc_usermode,
    .userd_set_gpput     = hopper_userd_set_gpput,
    .userd_get_gpput     = hopper_userd_get_gpput,
    .userd_try_get_gpget = hopper_userd_try_get_gpget,
    .engine_for_role     = hopper_engine_for_role,
};
