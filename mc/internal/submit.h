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

/* Build the NVC8B5 method stream for one CE copy.  Returns the
 * pushbuffer-end pointer (caller computes byte length as
 * (end - pb) * sizeof(uint32_t)). */
uint32_t *mc_write_transfer_methods(uint32_t *pb,
                                    uint64_t src_va, uint64_t dst_va,
                                    uint32_t nbytes,
                                    uint64_t sema_va, uint32_t sema_payload);

/* GPFIFO-entry builder + USERD GPPut bump.  Both used by submit
 * primitives but are useful standalone for tests/diagnostics. */
uint32_t write_gp_entry(volatile uint32_t *gpfifo_ring,
                        uint32_t gp_put_index, uint32_t ring_entries,
                        uintptr_t pb_cpu_va, uint32_t pb_offset,
                        uint32_t method_bytes);
void     ring_doorbell(void *userd,
                       uint32_t new_gp_put,
                       volatile uint32_t *vf_doorbell,
                       uint32_t work_submit_token);

/* Wait for the channel to drain (USERD GPGet catches up to GPPut)
 * or timeout_ms elapses.  Used during channel teardown. */
void     drain_channel(void *userd, long timeout_ms);

/* USERD field accessors.
 *
 * USERD is a per-channel-class structure, so its layout belongs to the
 * channel class rather than to the library: HOPPER_CHANNEL_GPFIFO_A uses
 * HopperAControlGPFifo, AMPERE_CHANNEL_GPFIFO_A uses AmpereAControlGPFifo
 * (identical layout), and BLACKWELL_CHANNEL_GPFIFO_A uses Nvc96fControl,
 * which exposes GPPut and *no GPGet at all*.  Keeping the cast inside
 * hal/submit.c means mc_channel_t can hold an untyped `void *userd_cpu`
 * and internal/types.h needs no class header.
 *
 * mc_userd_try_get_gpget returns false when the class does not expose
 * GPGet; callers must have a fallback rather than assuming a value.  It
 * always succeeds today and exists so the Blackwell case has a shape to
 * land in.
 */
void     mc_userd_set_gpput(void *userd, uint32_t value);
uint32_t mc_userd_get_gpput(const void *userd);
bool     mc_userd_try_get_gpget(const void *userd, uint32_t *out_gpget);

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
