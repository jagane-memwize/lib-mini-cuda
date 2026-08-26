/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * internal/channel.h — per-kind channel bring-up and teardown,
 * implemented by core/channel.c and driven by core/context.c's
 * mc_init / mc_fini.
 *
 * A channel's `role` is what the library dispatches on; its `type` is the
 * RM allocation shape used to bring it up.  The two are orthogonal — see
 * mc_channel_role_t / mc_channel_type_t in internal/types.h.
 *
 * The selected copy-engine type flows unchanged through the TSG, the
 * channel, and the CE object; RM cross-checks it at each step, so the
 * lce_engine_type argument threaded through these functions must be the
 * one pick_non_grce_lce returned.
 */
#ifndef MC_INTERNAL_CHANNEL_H
#define MC_INTERNAL_CHANNEL_H

#include <mc/mc.h>

#include "nvtypes.h"
#include "internal/types.h"

/* CE channel inside a carrier VAS.  `role` distinguishes the host-authored
 * channel from the SM-authored victim; a VAS may own at most one of each. */
int  mc_dma_channel_init(mc_ctx_t *ctx, mc_vas_t slot,
                         mc_channel_role_t role, NvU32 lce_engine_type);
void mc_dma_channel_fini(mc_ctx_t *ctx, mc_vas_t slot,
                         mc_channel_role_t role);

/* Compute channel inside a carrier VAS, plus the two kernel modules
 * (mc_doorbell_kernel and sm_owner_kernel) staged into it. */
int  mc_compute_channel_init(mc_ctx_t *ctx, mc_vas_t slot);
void mc_compute_channel_fini(mc_ctx_t *ctx, mc_vas_t slot);

/* The UVM-registered CE channel — the one mc_memcpy uses for MC_VAS_UVM.
 * Its resources are vidmem-backed and registered with UVM rather than
 * mapped through a carrier. */
int  mc_uvm_channel_init(mc_ctx_t *ctx, NvU32 lce_engine_type);
void mc_uvm_channel_fini(mc_ctx_t *ctx);

#endif /* MC_INTERNAL_CHANNEL_H */
