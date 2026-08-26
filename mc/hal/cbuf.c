/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * hal/cbuf.c — constant buffer 0 for the embedded kernels.
 *
 * Both kernels libmc embeds take the same two arguments, a destination VA and
 * a token, so building their CB0 is: zero the buffer, write three dwords.
 * Only *where* those dwords go varies, and that is descriptor data
 * (mc_compute_ops_t.cb0_off_*) rather than behaviour, so one implementation
 * serves every architecture.
 *
 * The offsets are properties of the compiled SASS, not of the QMD version:
 * nvcc places kernel parameters at 0x210 on sm_90 but 0x160 on sm_80.  A
 * backend must read its own out of `cuobjdump --dump-sass`.
 *
 * Everything outside the argument dwords stays zero.  On sm_90 that matters
 * beyond tidiness: the SASS loads a global memory descriptor from
 * c[0x0][0x208] and addresses the store through it, so a stale non-zero value
 * there sends the store to the wrong place.  Zeroing selects raw-VA mode.
 */

#include <stdint.h>
#include <string.h>

#include "hal/arch.h"
#include "internal/compute.h"

void mc_cb0_init(const mc_compute_ops_t *c, uint8_t *cb0)
{
    if (c == NULL || cb0 == NULL) return;
    memset(cb0, 0, mc_cb0_alloc_bytes(c));
}

void mc_cb0_set_args(const mc_compute_ops_t *c, uint8_t *cb0,
                     uint64_t dst_gpu_va, uint32_t token)
{
    if (c == NULL || cb0 == NULL) return;
    *(uint32_t *)(cb0 + c->cb0_off_dst_lo) = (uint32_t)dst_gpu_va;
    *(uint32_t *)(cb0 + c->cb0_off_dst_hi) = (uint32_t)(dst_gpu_va >> 32);
    *(uint32_t *)(cb0 + c->cb0_off_token ) = token;
}
