/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * core/memcpy.c — mc_memcpy: the public synchronous copy entry point.
 *
 * One entry point covers both directions.  Each pointer is looked up in
 * the allocation table, so the function already knows which side is device
 * and which is host, and which VA space the pair belongs to.  Dispatch is
 * (VAS, agent) -> channel role: the host-authored arm writes the method
 * stream itself, while MC_XFER_SM hands the submission to an SM thread via
 * mc_sm_owner_submit.
 */

#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "nvtypes.h"

#include "internal/types.h"
#include "internal/log.h"
#include "internal/submit.h"
#include "hal/methods.h"
#include "internal/vaspace.h"
#include "internal/alloc.h"
#include "internal/compute.h"

/* mc_memcpy casts size_t n to uint32_t for the method
 * stream and the SM-owner CB0.  The size gate above each cast is
 * `n > MC_MAX_TRANSFER_SIZE`, so n <= MC_MAX_TRANSFER_SIZE always.
 * If MC_MAX_TRANSFER_SIZE is ever raised above UINT32_MAX (e.g. to
 * support chunked >4 GiB transfers), the cast will silently
 * truncate and the GPU will copy a tiny size while reading/writing
 * far past the buffer — a memory-corruption foot-gun.  Trip the
 * build before that ships. */
_Static_assert(MC_MAX_TRANSFER_SIZE <= UINT32_MAX,
               "(uint32_t)n cast in mc_memcpy would truncate; "
               "raise MC_MAX_TRANSFER_SIZE only with a chunking loop");

/* ── mc_memcpy ─────────────────────────────
 *
 * Both src and dst arguments are user pointers returned by mc_malloc_*.
 * mc_memcpy looks them up in the allocation table, validates
 * they share a VAS (else MC_EINVAL), then dispatches to the agent
 * named by `agent`.
 *
 *   MC_XFER_HOST: host writes the matching channel's pushbuffer (UVM-
 *                 VAS → UVM channel, CARRIER-VAS → DMA channel) and
 *                 rings its doorbell.  Translates CPU pointers to GPU
 *                 VAs via the alloc table.
 *
 *   MC_XFER_SM:   launches the sm_owner kernel on the compute channel;
 *                 a single SM thread authors the entire DMA-channel
 *                 submission (PB + GPFIFO entry + USERD GPPut + BAR1
 *                 doorbell) from device code.  Both buffers must live
 *                 in a carrier VA space — MC_VAS_SYSMEM_CARRIER or
 *                 MC_VAS_FB_CARRIER (the SM kernel only addresses a
 *                 carrier).
 *
 * Why translation matters: for UVM-VAS host allocs the user's CPU
 * pointer happens to equal the GPU VA (Paper F1) — so passing it
 * directly to mc_write_transfer_methods would have worked.  For
 * CARRIER-VAS host allocs the CPU and GPU VAs are different.  The
 * alloc-table lookup gives us the right GPU VA in both cases.
 *
 * The sema_payload counter increments monotonically per submission per
 * channel; if it wraps to 0 we bump it to 1 so the payload == 0
 * "cleared" sentinel stays unambiguous.
 */
mc_status_t mc_memcpy(mc_ctx_t *ctx, const void *dst_ptr, const void *src_ptr,
                      size_t n, mc_xfer_t agent)
{
  const mc_alloc_t *src_slot;
  const mc_alloc_t *dst_slot;
  mc_channel_t     *ch;
  uint32_t         *pb_end;
  uint32_t          copy_bytes;
  mc_status_t       rc;

  if (ctx == NULL) return MC_EINVAL;
  if (n == 0 || n > MC_MAX_TRANSFER_SIZE) return MC_EINVAL;
  if (agent != MC_XFER_HOST && agent != MC_XFER_SM) return MC_EINVAL;

  src_slot = alloc_table_lookup(ctx, src_ptr);
  dst_slot = alloc_table_lookup(ctx, dst_ptr);
  if (src_slot == NULL || dst_slot == NULL)
  {
    WARN_LOG("mc_memcpy: src=%p dst=%p — at least one pointer not in "
             "alloc table; both must come from mc_malloc_* on this ctx",
             src_ptr, dst_ptr);
    return MC_EINVAL;
  }
  if (src_slot->vas != dst_slot->vas)
  {
    WARN_LOG("mc_memcpy: src VAS=%d dst VAS=%d mismatch; both buffers "
             "must live in the same VAS",
             (int)src_slot->vas, (int)dst_slot->vas);
    return MC_EINVAL;
  }
  if (src_slot->size < n || dst_slot->size < n)
  {
    WARN_LOG("mc_memcpy: transfer size %zu exceeds buffer (src=%llu, dst=%llu)",
             n, (unsigned long long)src_slot->size,
             (unsigned long long)dst_slot->size);
    return MC_EINVAL;
  }

  if (agent == MC_XFER_SM)
  {
    /* The SM-owner kernel only addresses a carrier VAS — it walks
     * that VAS's PDB and submits onto the VAS's victim DMA channel.
     * UVM-VAS pointers would dereference invalid GPU VAs from the
     * SM thread's perspective. */
    if (src_slot->vas == MC_VAS_UVM)
    {
      WARN_LOG("mc_memcpy(MC_XFER_SM): UVM-VAS pointers not supported "
               "(SM kernel only addresses carrier VAS)");
      return MC_EINVAL;
    }
    return mc_sm_owner_submit(ctx, src_slot->vas, src_slot->gpu_va,
                              dst_slot->gpu_va, (uint32_t)n);
  }

  /* MC_XFER_HOST: pick the channel by (vas, role).  UVM-VAS routes
   * through the UVM CE channel; carrier-VAS routes through the
   * carrier's HOST_DMA channel.  Both channels' sema state is
   * independent; the library is single-threaded so alternating
   * mc_memcpy calls don't race. */
  ch = (src_slot->vas == MC_VAS_UVM)
       ? mc_vas_find_channel(&ctx->vas[MC_VAS_UVM], MC_ROLE_UVM_CE)
       : mc_vas_find_channel(&ctx->vas[src_slot->vas], MC_ROLE_HOST_DMA);
  if (ch == NULL || !ch->h_channel) return MC_EINTERNAL;

  ch->sema_payload++;
  if (ch->sema_payload == 0) ch->sema_payload = 1;

  pb_end = mc_write_transfer_methods(ch->arch, ch->pb_cpu,
                                     src_slot->gpu_va, dst_slot->gpu_va,
                                     (uint32_t)n,
                                     ch->sema_gpu_va, ch->sema_payload);
  copy_bytes = (uint32_t)((pb_end - ch->pb_cpu) * sizeof(uint32_t));

  rc = mc_channel_submit(ch, ctx->vf_doorbell, copy_bytes);
  if (rc != MC_OK)
  {
    WARN_LOG("mc_memcpy: submit failed rc=%d agent=%d vas=%d bytes=%zu "
             "src_ptr=%p src_gpu_va=0x%llx dst_ptr=%p dst_gpu_va=0x%llx "
             "h_channel=0x%x sema_payload=0x%x",
             rc, (int)agent, (int)src_slot->vas, n,
             src_ptr, (unsigned long long)src_slot->gpu_va,
             dst_ptr, (unsigned long long)dst_slot->gpu_va,
             ch->h_channel, ch->sema_payload);
  }
  return rc;
}
