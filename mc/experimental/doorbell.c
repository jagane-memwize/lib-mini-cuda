/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * experimental/doorbell.c — host-authored CE-doorbell research paths.
 *
 * mc_dbell_demo_ring has the copy engine write a token to the BAR1
 * doorbell aperture through a GPU MMU PTE, and mc_memcpy_gpu_doorbell_ce
 * chains that into a real transfer.  Both are chain-of-evidence demos for
 * the doorbell path rather than part of the supported copy API.
 *
 * Moved out of the lifecycle layer so the research paths are separable
 * from the code that has to work.
 */

#define _GNU_SOURCE
#include <emmintrin.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "nvtypes.h"
#include "nvmisc.h"
#include "class/clc8b5.h"        /* HOPPER_DMA_COPY_A + methods */
#include "class/clc86f.h"        /* NVC86F_SET_OBJECT + GP entry */
#include "class/clc36f.h"        /* NVC36F_DMA_INCR_* method headers */
#include "class/cla06fsubch.h"   /* NVA06F_SUBCHANNEL_COPY_ENGINE */

#include <mc/mc_experimental.h>

#include "internal/types.h"
#include "internal/log.h"
#include "internal/rm.h"
#include "internal/submit.h"
#include "internal/vaspace.h"
#include "internal/alloc.h"
#include "internal/compute.h"
#include "hal/methods.h"

/*
 * mc_dbell_demo_ring — submit a 4-byte CE LAUNCH_DMA on the DMA
 * channel that copies `token` from a sysmem cell into the BAR1
 * doorbell page at GPU VA (carrier VAS dbell_gpu_va + 0x90).  The CE
 * engine — not the host — issues the MMIO write.
 *
 * Method stream (mirrors mc_write_transfer_methods):
 *   SET_OBJECT      → HOPPER_DMA_COPY_A on subch 4
 *   OFFSET_IN_*     ← DMA channel's token_cell_gpu_va     (sysmem source)
 *   OFFSET_OUT_*    ← carrier vas->dbell_gpu_va + 0x90    (BAR1 destination)
 *   LINE_LENGTH_IN  ← 4 bytes
 *   SET_SEMAPHORE_* + LAUNCH_DMA(NON_PIPELINED|FLUSH_ENABLE|REL_ONE_WORD)
 *
 * The destination GPU VA resolves through the GPU MMU PTE that
 * mc_va_space_install_doorbell_pte installed via NV04_MAP_MEMORY_DMA →
 * usrmodeGetMemInterMapParams_IMPL substituting pBar1VF.  The CE's
 * write egresses through the GPU's host interface as a 4-byte PCIe
 * MWr that lands on the H100's BAR1 doorbell register.
 */
mc_status_t mc_dbell_demo_ring(mc_ctx_t *ctx, uint32_t token)
{
  mc_channel_t         *ch;
  struct mc_dma_extras *ex;
  NvU64                 dbell_gpu_va;
  uint32_t             *pb_end;
  uint32_t              copy_bytes;

  if (ctx == NULL) return MC_EINVAL;
  ch           = mc_vas_find_channel(&ctx->vas[MC_VAS_SYSMEM_CARRIER],
                                     MC_ROLE_HOST_DMA);
  dbell_gpu_va = ctx->vas[MC_VAS_SYSMEM_CARRIER].dbell_gpu_va;
  if (ch == NULL || !ch->h_channel || !dbell_gpu_va) return MC_EINTERNAL;
  ex           = &ch->x.dma;

  /* Pre-fill the source cell with the token value the GPU will copy. */
  *ex->token_cell = token;

  ch->sema_payload++;
  if (ch->sema_payload == 0) ch->sema_payload = 1;

  pb_end = mc_write_transfer_methods(
      ctx->arch,
      ch->pb_cpu,
      ex->token_cell_gpu_va,                          /* src: sysmem token cell */
      dbell_gpu_va + MC_VF_DOORBELL_OFFSET,           /* dst: BAR1 +0x90 */
      4,                                              /* 4 bytes */
      ch->sema_gpu_va,
      ch->sema_payload);
  copy_bytes = (uint32_t)((pb_end - ch->pb_cpu) * sizeof(uint32_t));

  /* Two writes per submission: host's doorbell ring (kicks dma_ch's
   * PBDMA inside mc_channel_submit -> ring_doorbell) and the GPU-side
   * MMIO write the CE op then performs to BAR1+0x90.  The latter is
   * the point of this demo. */
  return mc_channel_submit(ch, ctx->vf_doorbell, copy_bytes);
}

/*
 * mc_memcpy_gpu_doorbell_ce — D2H whose UVM-channel doorbell is
 * rung by the GPU's CE engine on the DMA channel, not the host.
 *
 * Chain (top to bottom is wall-clock time):
 *
 *   host:     arm UVM-channel CE pushbuffer (HBM→DRAM + sema rel)
 *             write UVM-channel GPFIFO entry
 *             advance UVM-channel USERD GPPut       ← but NO doorbell ring
 *             arm DMA-channel CE pushbuffer (token→UVM-ch doorbell GPU VA)
 *             write DMA-channel GPFIFO entry
 *             ring DMA-channel doorbell (host MMIO)
 *             poll DMA-channel sema  ╮
 *                                    │  DMA-channel CE LAUNCH_DMA writes
 *                                    │  the UVM channel's work_submit_token
 *                                    │  to the BAR1 doorbell — this is the
 *                                    │  GPU-issued ring.
 *                                    ╯
 *             DMA-channel sema fires → CE op done, doorbell write committed
 *             poll UVM-channel sema  ╮
 *                                    │  UVM channel's PBDMA, woken by the
 *                                    │  GPU-issued doorbell, runs the
 *                                    │  HBM→DRAM CE LAUNCH_DMA.
 *                                    ╯
 *             UVM-channel sema fires → bytes are in dst_host
 *
 * Verification: the caller compares dst_host with expected bytes.
 * The host did NOT ring the UVM channel's doorbell; the only path
 * that could have woken its PBDMA is the DMA-channel CE's MMIO
 * write to the BAR1 doorbell GPU VA.  No libcuda, no watchpoint
 * shadow involvement, no kernel hooks beyond what was already
 * needed for UVM + DMA mapping.
 */
mc_status_t mc_memcpy_gpu_doorbell_ce(mc_ctx_t *ctx, void *dst,
                                      const void *src, size_t n)
{
  const mc_alloc_t     *src_slot;
  const mc_alloc_t     *dst_slot;
  mc_channel_t         *prim;
  mc_channel_t         *dma;
  struct mc_dma_extras *dma_ex;
  NvU64                 dbell_gpu_va;
  uint32_t             *prim_pb_end;
  uint32_t              prim_copy_bytes;
  uint32_t             *sec_pb_end;
  uint32_t              sec_copy_bytes;
  struct timespec       t0;
  mc_status_t           rc;

  if (ctx == NULL || dst == NULL || src == NULL) return MC_EINVAL;
  if (n == 0 || n > MC_MAX_TRANSFER_SIZE) return MC_EINVAL;

  /* Direction-agnostic validation, mirroring mc_memcpy: both pointers
   * must come from mc_malloc_* on this ctx, share a VAS, and be large
   * enough for n.  The chain primary is the UVM channel, so both
   * pointers must live in MC_VAS_UVM. */
  src_slot = alloc_table_lookup(ctx, src);
  dst_slot = alloc_table_lookup(ctx, dst);
  if (src_slot == NULL || dst_slot == NULL)
  {
    WARN_LOG("mc_memcpy_gpu_doorbell_ce: src=%p dst=%p — at least one "
             "pointer not in alloc table; both must come from mc_malloc_* "
             "on this ctx", src, dst);
    return MC_EINVAL;
  }
  if (src_slot->vas != MC_VAS_UVM || dst_slot->vas != MC_VAS_UVM)
  {
    WARN_LOG("mc_memcpy_gpu_doorbell_ce: src VAS=%d dst VAS=%d; both "
             "buffers must live in MC_VAS_UVM (the chain primary is the "
             "UVM CE channel)", (int)src_slot->vas, (int)dst_slot->vas);
    return MC_EINVAL;
  }
  if (src_slot->size < n || dst_slot->size < n)
  {
    WARN_LOG("mc_memcpy_gpu_doorbell_ce: transfer size %zu exceeds buffer "
             "(src=%llu, dst=%llu)", n,
             (unsigned long long)src_slot->size,
             (unsigned long long)dst_slot->size);
    return MC_EINVAL;
  }

  prim         = mc_vas_find_channel(&ctx->vas[MC_VAS_UVM], MC_ROLE_UVM_CE);
  dma          = mc_vas_find_channel(&ctx->vas[MC_VAS_SYSMEM_CARRIER],
                                     MC_ROLE_HOST_DMA);
  dbell_gpu_va = ctx->vas[MC_VAS_SYSMEM_CARRIER].dbell_gpu_va;
  if (prim == NULL || dma == NULL || !dma->h_channel || !dbell_gpu_va)
    return MC_EINTERNAL;
  dma_ex       = &dma->x.dma;

  /* Arm primary's submission: build pushbuffer + write GPFIFO entry +
   * advance USERD GPPut, but DO NOT ring vf_doorbell.  src/dst are
   * the user-supplied GPU VAs in either order — the CE method stream
   * is direction-agnostic; H2D vs D2H comes from which slot was
   * mc_malloc_host vs mc_malloc_device.  mc_channel_arm captures t0
   * before publish so the eventual poll starts its budget here. */
  prim->sema_payload++;
  if (prim->sema_payload == 0) prim->sema_payload = 1;

  prim_pb_end = mc_write_transfer_methods(
      ctx->arch,
      prim->pb_cpu,
      src_slot->gpu_va, dst_slot->gpu_va,
      (uint32_t)n, prim->sema_gpu_va, prim->sema_payload);
  prim_copy_bytes = (uint32_t)((prim_pb_end - prim->pb_cpu) * sizeof(uint32_t));

  rc = mc_channel_arm(prim, prim_copy_bytes, &t0);
  if (rc != MC_OK) return rc;

  /* Arm + ring the DMA channel's CE op: copy 4 bytes from
   * token_cell (pre-filled with the UVM channel's work_submit_token)
   * to dbell_gpu_va + 0x90 — the same physical BAR1 doorbell page
   * the host would write to ring the UVM channel.  When this CE op
   * executes, its MMIO write to BAR1+0x90 wakes the UVM channel's
   * PBDMA. */
  *dma_ex->token_cell = prim->work_submit_token;

  dma->sema_payload++;
  if (dma->sema_payload == 0) dma->sema_payload = 1;

  sec_pb_end = mc_write_transfer_methods(
      ctx->arch,
      dma->pb_cpu,
      dma_ex->token_cell_gpu_va,                          /* src */
      dbell_gpu_va + MC_VF_DOORBELL_OFFSET,               /* dst: UVM ch doorbell */
      4,
      dma->sema_gpu_va, dma->sema_payload);
  sec_copy_bytes = (uint32_t)((sec_pb_end - dma->pb_cpu) * sizeof(uint32_t));

  /* mc_channel_submit on dma rings the DMA channel's doorbell, polls
   * dma's sema until the CE op completes (LAUNCH_DMA's FLUSH_ENABLE
   * guarantees the BAR1 write has committed by then). */
  rc = mc_channel_submit(dma, ctx->vf_doorbell, sec_copy_bytes);
  if (rc != MC_OK) return rc;

  /* Then poll primary's sema (HBM->DRAM bytes flushed), charging
   * elapsed time against the same budget that started before the
   * first submission. */
  return mc_channel_poll_sema(prim->sema_ptr, prim->sema_payload, t0);
}
