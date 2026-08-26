/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * internal/compute.h — the SM-authored submission helper and the
 * allocation-table lookup shared across translation units.
 *
 * Split out of the former mc_internal.h.
 */
#ifndef MC_INTERNAL_COMPUTE_H
#define MC_INTERNAL_COMPUTE_H

#include <stdint.h>

#include <mc/mc.h>

#include "internal/types.h"

/* Library-internal: invoked from core/memcpy.c::mc_memcpy when
 * the caller passes MC_XFER_SM.  Both gpu_va arguments must be valid
 * GPU VAs in the chosen carrier VAS; the caller is responsible for
 * alloc-table lookup and validation.
 *
 * `vas` selects which carrier to author into.  The submitter looks
 * up `(compute_ch, victim_ch)` from that VAS's role table:
 *   MC_VAS_SYSMEM_CARRIER → (COMPUTE, HOST_DMA)
 *   MC_VAS_FB_CARRIER     → (COMPUTE, SM_VICTIM_DMA)
 *
 * Functionally: launches the sm_owner kernel on the compute channel.
 * A single SM thread authors the entire CE-channel submission for
 * the victim channel — pushbuffer methods, GPFIFO entry, USERD
 * GP_PUT, BAR1 doorbell — then PBDMA wakes and runs the CE copy.
 *
 * Sema synchronization (unified across both VAS / both directions):
 * the active release-sema cell is always sysmem-resident, the host
 * polls it as ground truth after the compute report sema fires.
 * Compute-sema-fire only proves the SM kernel exited (the doorbell
 * was rung); the sysmem-sema poll proves PBDMA actually retired the
 * CE LAUNCH_DMA.  PCIe producer-side ordering of MWrs from one
 * Requester ID into sysmem gives the data-then-sema guarantee for
 * D2H; LAUNCH_DMA's FLUSH_ENABLE gives MRd-CplD-into-HBM-then-sema
 * for H2D.
 *
 * Returns MC_OK on success, MC_ETIMEOUT if the compute kernel itself
 * doesn't exit within MC_TIMEOUT_MS, MC_EHANG if the kernel exited
 * but the victim's CE LAUNCH_DMA never released the DMA sema. */
mc_status_t mc_sm_owner_submit(mc_ctx_t *ctx, mc_vas_t vas, uint64_t src_gpu_va,
                               uint64_t dst_gpu_va, uint32_t size_bytes);

/* Look up an mc_alloc_t slot by user pointer.  Returns NULL if `ptr`
 * was not returned by mc_malloc_* on this context (or `ptr` is NULL).
 * Cross-file callers (e.g. the chain-doorbell functions in
 * experimental/compute.c) use this to validate user-supplied pointer
 * pairs before dispatching a submission. */
mc_alloc_t *alloc_table_lookup(mc_ctx_t *ctx, const void *ptr);

#endif /* MC_INTERNAL_COMPUTE_H */
