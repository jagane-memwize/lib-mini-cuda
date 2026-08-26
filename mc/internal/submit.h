/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * internal/submit.h — method-stream builders and channel submission
 * primitives implemented by hal/submit.c.
 *
 * Split out of the former mc_internal.h.  The submission order
 * (pushbuffer -> GPFIFO entry -> USERD GPPut -> doorbell) and its fence
 * discipline are properties of the implementations behind these
 * declarations; see hal/submit.c.
 */
#ifndef MC_INTERNAL_SUBMIT_H
#define MC_INTERNAL_SUBMIT_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>           /* struct timespec */

#include <mc/mc.h>

#include "internal/types.h"

/* USERD GPPut bump + BAR1 doorbell ring, in that order, fenced.
 * The method-stream and GPFIFO-entry emitters live in hal/methods.h. */
void     ring_doorbell(const mc_arch_desc_t *arch, void *userd,
                       uint32_t new_gp_put,
                       volatile uint32_t *vf_doorbell,
                       uint32_t work_submit_token);

/* Wait for the channel to drain (USERD GPGet catches up to GPPut)
 * or timeout_ms elapses.  Used during channel teardown. */
void     drain_channel(mc_channel_t *ch, long timeout_ms);


/* Channel submission primitives — see comments above the
 * implementations in hal/submit.c for the produce -> fence -> publish
 * contract. */
mc_status_t mc_channel_submit   (mc_channel_t *ch,
                                 volatile uint32_t *vf_doorbell,
                                 uint32_t copy_bytes);
mc_status_t mc_channel_arm      (mc_channel_t *ch, uint32_t copy_bytes,
                                 struct timespec *t0_out);
mc_status_t mc_channel_poll_sema(volatile uint32_t *cell, uint32_t expected,
                                 struct timespec t0);

#endif /* MC_INTERNAL_SUBMIT_H */
