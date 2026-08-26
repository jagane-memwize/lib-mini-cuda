/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * core/context.c — library lifecycle: mc_init and mc_fini.
 *
 * mc_init verifies the loaded RM version against the pinned source before
 * allocating anything, opens the device nodes, reserves the Paper-F1 VA
 * pool, allocates the RM object tree, selects a copy engine, and brings up
 * the VA spaces and their channels.  mc_fini unwinds in strict LIFO order,
 * draining channels before releasing the mappings and handles they use.
 *
 * Cleanup is best-effort: one release failure must not strand the rest of
 * the object tree.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "nvtypes.h"                   /* NvU32 / NvU64 / NvHandle */
#include "nv-ioctl.h"                  /* NV_RM_API_VERSION_STRING_LENGTH */
#include "class/cl2080_notification.h" /* NV2080_ENGINE_TYPE_GR0 */

#include "internal/types.h"
#include "internal/log.h"
#include "internal/rm.h"
#include "internal/uvm.h"
#include "internal/submit.h"
#include "internal/vaspace.h"
#include "internal/alloc.h"
#include "internal/channel.h"
#include "hal/arch.h"
#include "hal/detect.h"

/* One line summarising every channel mc_init brought up.  Kept out of mc_init
 * so the six lookups it needs do not land in that function's scope. */
static void mc_log_init_summary(mc_ctx_t *ctx)
{
  mc_channel_t *uvm_ch  = mc_vas_find_channel(&ctx->vas[MC_VAS_UVM],
                                              MC_ROLE_UVM_CE);
  mc_channel_t *sys_dma = mc_vas_find_channel(&ctx->vas[MC_VAS_SYSMEM_CARRIER],
                                              MC_ROLE_HOST_DMA);
  mc_channel_t *sys_cmp = mc_vas_find_channel(&ctx->vas[MC_VAS_SYSMEM_CARRIER],
                                              MC_ROLE_COMPUTE);
  mc_channel_t *fb_dma  = mc_vas_find_channel(&ctx->vas[MC_VAS_FB_CARRIER],
                                              MC_ROLE_HOST_DMA);
  mc_channel_t *fb_smv  = mc_vas_find_channel(&ctx->vas[MC_VAS_FB_CARRIER],
                                              MC_ROLE_SM_VICTIM_DMA);
  mc_channel_t *fb_cmp  = mc_vas_find_channel(&ctx->vas[MC_VAS_FB_CARRIER],
                                              MC_ROLE_COMPUTE);

  DEBUG_LOG("mc_init ok: client=0x%x device=0x%x uvm=0x%x "
            "sys{dma=0x%x compute=0x%x} fb{dma=0x%x sm_victim=0x%x compute=0x%x}",
            ctx->h_client, ctx->h_device,
            uvm_ch  ? uvm_ch->h_channel  : 0,
            sys_dma ? sys_dma->h_channel : 0,
            sys_cmp ? sys_cmp->h_channel : 0,
            fb_dma  ? fb_dma->h_channel  : 0,
            fb_smv  ? fb_smv->h_channel  : 0,
            fb_cmp  ? fb_cmp->h_channel  : 0);
}

mc_status_t mc_init(mc_ctx_t **out_ctx)
{
  mc_ctx_t              *ctx;
  NvU32                  lce_engine_type;
  mc_status_t            fail_status = MC_EIOCTL;
  mc_rm_version_result_t version_result;
  char                    loaded_version[NV_RM_API_VERSION_STRING_LENGTH] = {};

  if (out_ctx == NULL) return MC_EINVAL;
  *out_ctx = NULL;

  ctx = calloc(1, sizeof(*ctx));
  if (ctx == NULL) { ERROR_LOG("calloc ctx"); return MC_EALLOC; }
  ctx->ctl_fd = ctx->dev_fd = ctx->uvm_fd = -1;

  /* Open only the control device first.  A private ABI mismatch must be
   * rejected before /dev/nvidia0 is opened, the VA pool is reserved, or any
   * RM object/channel is allocated. */
  ctx->ctl_fd = open(MC_CONTROL_DEV_PATH, O_RDWR | O_CLOEXEC);
  if (ctx->ctl_fd < 0)
  {
    ERROR_LOG("open %s: %s", MC_CONTROL_DEV_PATH, strerror(errno));
    goto fail;
  }

  version_result = rm_check_driver_version(ctx->ctl_fd,
                                           MC_NVIDIA_DRIVER_VERSION,
                                           loaded_version,
                                           sizeof(loaded_version));
  if (version_result == MC_RM_VERSION_MISMATCH)
  {
    ERROR_LOG("NVIDIA driver version mismatch: libmc targets %s, loaded %s",
              MC_NVIDIA_DRIVER_VERSION,
              loaded_version[0] != '\0' ? loaded_version : "unknown");
    fail_status = MC_EVERSION;
    goto fail;
  }
  if (version_result != MC_RM_VERSION_OK)
  {
    ERROR_LOG("NVIDIA driver version check failed");
    goto fail;
  }

  ctx->dev_fd = open(MC_DEVICE_DEV_PATH, O_RDWR | O_CLOEXEC);
  if (ctx->dev_fd < 0)
  {
    ERROR_LOG("open %s: %s", MC_DEVICE_DEV_PATH, strerror(errno));
    goto fail;
  }
  if (rm_register_client_fd(ctx->ctl_fd, ctx->dev_fd) != 0) goto fail;

  /* VA pool up-front — Paper F1 is non-negotiable. */
  if (va_pool_init() != 0) goto fail;

  /* Root / device / subdevice. */
  ctx->h_client    = rm_alloc_root(ctx->ctl_fd);
  ctx->h_device    = rm_alloc_device(ctx->ctl_fd, ctx->h_client);
  ctx->h_subdevice = rm_alloc_subdevice(ctx->ctl_fd, ctx->h_client,
                                        ctx->h_device);
  if (!ctx->h_client || !ctx->h_device || !ctx->h_subdevice) goto fail;

  /* Identify the GPU and select its backend.  This is the earliest point it
   * can run -- the architecture query is subdevice-scoped -- and it must run
   * before rm_alloc_usermode below, which is the first allocation that names
   * an architecture-specific class.
   *
   * Everything allocated above is architecture-invariant (NV01_ROOT,
   * NV01_DEVICE_0, NV20_SUBDEVICE_0), so a rejected chip unwinds through the
   * ordinary failure path having touched nothing chip-specific.  Note this
   * does mean detection runs after some RM objects exist, unlike the driver
   * version check, which still runs before /dev/nvidia0 is even opened. */
  if (mc_detect_arch(ctx->ctl_fd, ctx->h_client, ctx->h_device,
                     ctx->h_subdevice, &ctx->arch) != 0)
  {
    fail_status = MC_EARCH;
    goto fail;
  }

  /* Hold boost clocks for the life of the client, as libcuda does —
   * otherwise a copy-only workload runs the CE at the idle SM clock.
   * See rm_perf_boost. */
  rm_perf_boost(ctx->ctl_fd, ctx->h_client, ctx->h_subdevice);

  /* Engine selection: a non-GRCE LCE drives the TSG / channel / CE. */
  lce_engine_type = pick_non_grce_lce(ctx->ctl_fd, ctx->h_client,
                                      ctx->h_subdevice);
  if (lce_engine_type == (NvU32)-1) goto fail;

  /* HOPPER_USERMODE_A BAR0 + BAR1.  Both must be alive before the
   * carrier VAS so we can install the BAR1 doorbell PTE. */
  ctx->h_usermode_bar0 = rm_alloc_usermode(ctx->ctl_fd, ctx->arch, ctx->h_client,
                                           ctx->h_subdevice, false);
  if (!ctx->h_usermode_bar0) goto fail;
  ctx->usermode_bar0_cpu = rm_map_memory(ctx->ctl_fd, MC_DEVICE_DEV_PATH,
                                         ctx->h_client, ctx->h_subdevice,
                                         ctx->h_usermode_bar0, 0,
                                         MC_USERMODE_SIZE, 0);
  if (ctx->usermode_bar0_cpu == NULL) goto fail;

  ctx->h_usermode_bar1 = rm_alloc_usermode(ctx->ctl_fd, ctx->arch, ctx->h_client,
                                           ctx->h_subdevice, true);
  if (!ctx->h_usermode_bar1) goto fail;
  ctx->usermode_bar1_cpu = rm_map_memory(ctx->ctl_fd, MC_DEVICE_DEV_PATH,
                                         ctx->h_client, ctx->h_subdevice,
                                         ctx->h_usermode_bar1, 0,
                                         MC_USERMODE_SIZE, 0);
  if (ctx->usermode_bar1_cpu == NULL) goto fail;
  ctx->vf_doorbell = (volatile uint32_t *)(ctx->usermode_bar1_cpu
                                           + MC_VF_DOORBELL_OFFSET);

  /* Sysmem carrier VAS (DMA + compute channels) + its BAR1 doorbell
   * PTE.  Brought up before the channels themselves so neither has
   * to embed VAS handles in its extras. */
  if (mc_va_space_init_carrier(ctx) != 0) goto fail;
  if (mc_va_space_install_doorbell_pte(ctx, &ctx->vas[MC_VAS_SYSMEM_CARRIER])
      != 0)
    goto fail;

  /* FB carrier VAS (HOST_DMA + SM_VICTIM_DMA + COMPUTE channels with
   * FB-resident, BAR1-aliased channel resources) + its own BAR1
   * doorbell PTE.  Channels here exist for the SM-author hot path
   * where every byte of the submission protocol is FB↔GPU-L2 and the
   * only PCIe MWr is the doorbell. */
  if (mc_va_space_init_carrier_fb(ctx) != 0) goto fail;
  if (mc_va_space_install_doorbell_pte(ctx, &ctx->vas[MC_VAS_FB_CARRIER])
      != 0)
    goto fail;

  /* UVM channel (CE; mc_memcpy/d2h). */
  if (mc_uvm_channel_init(ctx, lce_engine_type) != 0) goto fail;

  /* Sysmem-carrier channels: HOST_DMA (also reused as the SM victim
   * for the existing sysmem SM-author path) + COMPUTE. */
  if (mc_dma_channel_init(ctx, MC_VAS_SYSMEM_CARRIER, MC_ROLE_HOST_DMA,
                          lce_engine_type) != 0) goto fail;
  if (mc_compute_channel_init(ctx, MC_VAS_SYSMEM_CARRIER) != 0) goto fail;

  /* FB-carrier channels: HOST_DMA (host-author CE on FB-resident PB),
   * SM_VICTIM_DMA (CE channel reserved as the SM author's victim),
   * COMPUTE.  All three CE channels reuse the same non-GRCE LCE
   * engine type — Hopper allows multiple channels per engine. */
  if (mc_dma_channel_init(ctx, MC_VAS_FB_CARRIER, MC_ROLE_HOST_DMA,
                          lce_engine_type) != 0) goto fail;
  if (mc_dma_channel_init(ctx, MC_VAS_FB_CARRIER, MC_ROLE_SM_VICTIM_DMA,
                          lce_engine_type) != 0) goto fail;
  if (mc_compute_channel_init(ctx, MC_VAS_FB_CARRIER) != 0) goto fail;

  mc_log_init_summary(ctx);
  *out_ctx = ctx;
  return MC_OK;

fail:
  mc_fini(ctx);
  return fail_status;
}

/*
 * mc_fini — orchestrator.  Tears down the three channels in reverse
 * dependency order (compute cohabits dma's VAS so it must go first;
 * dma is independent of UVM; the UVM channel goes last among the
 * channels), THEN reaps any user allocations the caller didn't free
 * (after every channel is drained + disabled, so no in-flight GPU
 * work can possibly reference user PTEs being unmapped — Paper F1
 * invariant), THEN releases process-global state (USERMODE_A
 * mappings + RM handles, fds, ctx).
 *
 * Best-effort: every sub-step checks its own precondition so partial-
 * init states from mc_init failures are safe to pass in (mc_init's
 * fail path calls mc_fini on a half-built context).  Safe against
 * a NULL argument.
 */
void mc_fini(mc_ctx_t *ctx)
{
  int i;

  if (ctx == NULL) return;

  /* Channels: reverse-dependency order, FB carrier first (its compute
   * channel may still be authoring submissions to its SM_VICTIM_DMA),
   * then sysmem carrier, then UVM channel.  Each *_fini drains +
   * disables its channel before unmapping anything, so no GPU work
   * survives. */
  mc_compute_channel_fini(ctx, MC_VAS_FB_CARRIER);
  mc_dma_channel_fini    (ctx, MC_VAS_FB_CARRIER, MC_ROLE_SM_VICTIM_DMA);
  mc_dma_channel_fini    (ctx, MC_VAS_FB_CARRIER, MC_ROLE_HOST_DMA);
  mc_compute_channel_fini(ctx, MC_VAS_SYSMEM_CARRIER);
  mc_dma_channel_fini    (ctx, MC_VAS_SYSMEM_CARRIER, MC_ROLE_HOST_DMA);
  mc_uvm_channel_fini(ctx);

  /* VA spaces: free both carriers (each with its own BAR1 doorbell
   * PTE) and the UVM-managed VAS.  All channel buffers were unmapped
   * above. */
  mc_va_space_fini(ctx, MC_VAS_FB_CARRIER);
  mc_va_space_fini(ctx, MC_VAS_SYSMEM_CARRIER);
  mc_va_space_fini(ctx, MC_VAS_UVM);

  /* Now safe to reap any leftover user allocations: every channel is
   * disabled, so the GPU can't be referencing these PTEs.  This used
   * to run BEFORE channel disable — that violated the F1 invariant
   * (uvm_unmap on an active channel intermittently wedges the GPU). */
  for (i = 0; i < MC_ALLOC_TABLE_SLOTS; i++)
  {
    if (ctx->allocs[i].ptr == NULL) continue;
    /* GPU-mapping teardown gated on VAS.  See mc_free for rationale.
     *
     * Note: by the time we reach this loop the VA spaces themselves
     * have already been freed by mc_va_space_fini above (which
     * NVOS47-unmaps every per-resource carrier).  rm_free_handle on
     * the alloc's h_mem is sufficient here since the parent VAS is
     * already released.  (mc_free runs while the VAS is still live
     * and does the full release-carrier path; this loop only catches
     * user-leaked allocs.) */
    if (ctx->allocs[i].is_registered)
      mc_free_registered_uvm_ranges(ctx, &ctx->allocs[i]);
    else if (ctx->allocs[i].vas == MC_VAS_UVM
        && ctx->uvm_fd >= 0 && ctx->allocs[i].gpu_va)
      uvm_unmap_buffer(ctx->uvm_fd, ctx->gpu_inst_uuid, ctx->allocs[i].gpu_va,
                       ctx->allocs[i].size, "user");
    if (!ctx->allocs[i].is_device && !ctx->allocs[i].is_registered)
      munmap(ctx->allocs[i].ptr, ctx->allocs[i].size);
    if (ctx->allocs[i].h_mem)
      rm_free_handle(ctx->ctl_fd, ctx->h_client, ctx->h_device,
                     ctx->allocs[i].h_mem, "user_mem");
    ctx->allocs[i].ptr = NULL;
  }

  /* Process-global mappings + RM handles. */
  if (ctx->usermode_bar1_cpu)
    munmap((void *)ctx->usermode_bar1_cpu, MC_USERMODE_SIZE);
  if (ctx->usermode_bar0_cpu)
    munmap((void *)ctx->usermode_bar0_cpu, MC_USERMODE_SIZE);

  if (ctx->ctl_fd >= 0 && ctx->h_client)
  {
    if (ctx->h_usermode_bar1)
      rm_free_handle(ctx->ctl_fd, ctx->h_client, ctx->h_subdevice,
                     ctx->h_usermode_bar1, "h_usermode_bar1");
    if (ctx->h_usermode_bar0)
      rm_free_handle(ctx->ctl_fd, ctx->h_client, ctx->h_subdevice,
                     ctx->h_usermode_bar0, "h_usermode_bar0");
    if (ctx->h_subdevice)
      rm_free_handle(ctx->ctl_fd, ctx->h_client, ctx->h_client,
                     ctx->h_subdevice, "h_subdevice");
    if (ctx->h_device)
      rm_free_handle(ctx->ctl_fd, ctx->h_client, ctx->h_client, ctx->h_device,
                     "h_device");
    rm_free_handle(ctx->ctl_fd, ctx->h_client, ctx->h_client, ctx->h_client,
                   "h_client");
  }

  /* Close fds (uvm_fd first — its release does a final TLB flush). */
  if (ctx->uvm_fd >= 0) close(ctx->uvm_fd);
  if (ctx->dev_fd >= 0) close(ctx->dev_fd);
  if (ctx->ctl_fd >= 0) close(ctx->ctl_fd);

  free(ctx);
}
