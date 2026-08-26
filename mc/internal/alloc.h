/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * internal/alloc.h — the allocation table, implemented by core/alloc.c.
 *
 * The table is what lets mc_free take the opaque pointer a user holds and
 * recover the RM handle, GPU VA, size, VAS and is_device bit recorded when
 * the buffer was created.
 */
#ifndef MC_INTERNAL_ALLOC_H
#define MC_INTERNAL_ALLOC_H

#include <mc/mc.h>

#include "internal/types.h"

/* Look up an mc_alloc_t slot by user pointer.  Returns NULL if `ptr`
 * was not returned by mc_malloc_* on this context (or `ptr` is NULL).
 * Cross-file callers (e.g. the chain-doorbell functions in
 * experimental/) use this to validate user-supplied pointer pairs
 * before dispatching a submission. */
mc_alloc_t *alloc_table_lookup(mc_ctx_t *ctx, const void *ptr);

/* Release the UVM external ranges recorded for a registered-host slot.
 * mc_host_register may split one user range into several segments, so
 * teardown walks slot->uvm_segment_base[0 .. uvm_segment_count).  Used by
 * mc_host_unregister and by mc_fini's sweep over any slots the caller
 * never released. */
void mc_free_registered_uvm_ranges(mc_ctx_t *ctx, const mc_alloc_t *slot);

#endif /* MC_INTERNAL_ALLOC_H */
