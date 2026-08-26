/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * internal/vaspace.h — VA pool and VA-space helpers implemented by
 * core/vaspace.c.
 *
 * Split out of the former mc_internal.h.  The VA pool is the Paper-F1
 * anchor: CPU VA equals GPU VA for UVM host mappings, which is why the
 * reservation is process-global and claimed before anything else can mmap
 * into that range.
 */
#ifndef MC_INTERNAL_VASPACE_H
#define MC_INTERNAL_VASPACE_H

#include <mc/mc.h>

#include "nvtypes.h"
#include "internal/types.h"

int    va_pool_init(void);
void  *va_pool_reserve(NvU64 size, const char *label);
int    mc_va_space_init_uvm(mc_ctx_t *ctx);
int    mc_va_space_init_carrier(mc_ctx_t *ctx);
int    mc_va_space_init_carrier_fb(mc_ctx_t *ctx);

/* Reserve the next free channels[] slot in `vas` for a channel with
 * role `role`.  Returns a pointer to the new slot (caller fills it
 * in) on success, or NULL if the VAS already owns a channel with
 * this role or has no free slot.  Increments vas->channel_count. */
mc_channel_t *mc_vas_add_channel(mc_va_space_t *vas, mc_channel_role_t role);

/* Look up the unique channel in `vas` whose role is `role`.  Returns
 * NULL if no channel with that role exists in this VAS.  O(MC_VAS_CH_MAX)
 * scan; tiny constant. */
mc_channel_t *mc_vas_find_channel(mc_va_space_t *vas,
                                  mc_channel_role_t role);
int    mc_va_space_install_doorbell_pte(mc_ctx_t *ctx, mc_va_space_t *vas);

/* DMA-map an existing hMemory into `vas` via libcuda's per-resource
 * carrier shape: allocate a fresh NV50_MEMORY_VIRTUAL sized to the
 * source, NV04_MAP_MEMORY_DMA with dmaOffset=0, return RM-chosen GPU
 * VA.  Records the carrier in vas->carriers[] for teardown.  Returns
 * 0 on failure. */
NvU64  mc_va_space_dma_map_resource(mc_ctx_t *ctx, mc_va_space_t *vas,
                                    NvHandle h_mem, NvU64 size);

/* Release the per-resource carrier mapping `h_mem` into `vas`:
 * NVOS47-unmap, NV01_FREE the carrier handle, and remove the entry
 * from vas->carriers[].  Used by channel teardown to detach a
 * channel's carriers BEFORE freeing the channel's h_mem (NVOS47 on a
 * freed h_mem returns INVALID_OBJECT_HANDLE).  No-op if h_mem isn't
 * tracked in this VAS.  Multiple matching entries (shouldn't happen
 * but cheap to handle) are all released. */
void   mc_va_space_release_carrier(mc_ctx_t *ctx, mc_va_space_t *vas,
                                   NvHandle h_mem);

NvU64  mc_va_space_alloc_scratch(mc_ctx_t *ctx, mc_va_space_t *vas,
                                 NvU64 size, NvU64 align,
                                 NvHandle *out_h_mem, void **out_cpu);
/* Write-combined scratch — control plane only; see rm_alloc_sysmem_wc_at. */
NvU64  mc_va_space_alloc_scratch_wc(mc_ctx_t *ctx, mc_va_space_t *vas,
                                    NvU64 size, NvU64 align,
                                    NvHandle *out_h_mem, void **out_cpu);
NvU64  mc_va_space_alloc_vidmem (mc_ctx_t *ctx, mc_va_space_t *vas,
                                 NvU64 size, NvU64 align,
                                 NvHandle *out_h_mem);
void   mc_va_space_fini(mc_ctx_t *ctx, mc_vas_t id);

#endif /* MC_INTERNAL_VASPACE_H */
