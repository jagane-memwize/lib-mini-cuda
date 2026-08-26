/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * arch/ampere/ops.c — the Ampere behaviour hooks.
 *
 * Only one of the four genuinely differs from Hopper.  The USERD accessors
 * are identical because Nvc56fControl and Nvc86fControl have the same layout,
 * and the engine policy is the same shape; both are spelled out here rather
 * than shared, because "identical today" is a fact about two class headers
 * rather than a guarantee, and a backend that quietly inherited another's
 * accessor would be wrong the moment one of them changed.
 */

#include <stdbool.h>
#include <stdint.h>

#include "nvos.h"
#include "class/cl2080_notification.h"  /* NV2080_ENGINE_TYPE_GR0 */
#include "class/clc56f.h"               /* AmpereAControlGPFifo */

#include "hal/arch.h"
#include "internal/log.h"
#include "internal/rm.h"
#include "internal/types.h"

/* AMPERE_USERMODE_A is RS_NONE: no allocation parameters exist for it.
 *
 * This is the one hook Ampere genuinely needs.  A descriptor field cannot
 * express it -- the difference is not a value inside a parameter struct, it
 * is the absence of the struct.
 *
 * `bar1` is therefore meaningless here and must be false; the caller only
 * ever requests a BAR1 window where cap.usermode_bar1_capable says it can,
 * and this backend says it cannot.  Requesting one anyway is a bring-up bug,
 * so it is reported rather than silently ignored -- a BAR0 window returned in
 * answer to a BAR1 request would surface much later as a doorbell PTE mapped
 * against the wrong aperture.
 */
static NvHandle ampere_alloc_usermode(int ctl_fd, const mc_arch_desc_t *arch,
                                      NvHandle root, NvHandle subdevice,
                                      bool bar1)
{
    if (bar1)
    {
        ERROR_LOG("%s: BAR1 usermode requested, but AMPERE_USERMODE_A takes "
                  "no parameters and has no BAR1 mapping", arch->name);
        return 0;
    }
    return rm_alloc(ctl_fd, root, subdevice, arch->cls.usermode, NULL);
}

/* Nvc56fControl: GPGet at 0x88, GPPut at 0x8c -- the same layout Hopper has. */
#define USERD(p) ((volatile AmpereAControlGPFifo *)(p))

static void ampere_userd_set_gpput(void *userd, uint32_t value)
{
    USERD(userd)->GPPut = value;
}

static uint32_t ampere_userd_get_gpput(const void *userd)
{
    return USERD(userd)->GPPut;
}

static bool ampere_userd_try_get_gpget(const void *userd, uint32_t *out_gpget)
{
    if (userd == NULL || out_gpget == NULL)
        return false;
    *out_gpget = USERD(userd)->GPGet;
    return true;
}

/* Compute on GR0, every copy role on the selected non-GRCE LCE -- the same
 * policy as Hopper.  The GR0 arm is live: Ampere, Ada and GA10x all have a
 * compute backend and all three share this function through
 * mc_arch_ampere_ops. */
static NvU32 ampere_engine_for_role(const mc_arch_desc_t *arch,
                                    mc_arch_role_t role, NvU32 selected_lce)
{
    (void)arch;
    if ((mc_channel_role_t)role == MC_ROLE_COMPUTE)
        return NV2080_ENGINE_TYPE_GR0;
    return selected_lce;
}

const mc_arch_ops_t mc_arch_ampere_ops = {
    .alloc_usermode      = ampere_alloc_usermode,
    .userd_set_gpput     = ampere_userd_set_gpput,
    .userd_get_gpput     = ampere_userd_get_gpput,
    .userd_try_get_gpget = ampere_userd_try_get_gpget,
    .engine_for_role     = ampere_engine_for_role,
};
