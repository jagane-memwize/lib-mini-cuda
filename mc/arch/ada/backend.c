/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * arch/ada/backend.c — the Ada (AD10x) architecture descriptor.
 *
 * Ada is not a self-contained generation
 * --------------------------------------
 * The pinned tree ships ADA_COMPUTE_A (0xC9C0) and ADA_A (0xC997) and nothing
 * else under that name: there is no ADA_CHANNEL_GPFIFO and no ADA_DMA_COPY.
 * Ada drives AMPERE_USERMODE_A, AMPERE_CHANNEL_GPFIFO_A and AMPERE_DMA_COPY_B,
 * and brings only its own compute class.  So this file is a descriptor and
 * nothing else -- there is no arch/ada/ops.c.
 *
 * Sharing mc_arch_ampere_ops here is not the "identical today" shortcut that
 * arch/ampere/ops.c deliberately refuses.  Ampere spells out its own USERD
 * accessors because Nvc56fControl and Nvc86fControl are two headers that
 * happen to agree, and agreement is not a guarantee.  Ada allocates *the same
 * class* -- 0xC561 and 0xC56F, one header each, not two -- so there is no
 * second definition that could drift.  The ops are correct here by identity,
 * and the hooks already take the descriptor, so alloc_usermode reports "ada"
 * rather than "ampere" when it refuses a BAR1 request.
 *
 * Capabilities follow from that reuse:
 *
 *   - AMPERE_USERMODE_A is RS_NONE (resource_list.h:907-915), so no BAR1
 *     usermode aperture exists, so no doorbell PTE, so no FB carrier and
 *     nothing SM-authored.  Same reasoning as Ampere, same silicon-level
 *     conclusion; see arch/ampere/backend.c.
 *   - clc56f.h defines no PB_EXTENDED_BASE_OPERAND, so no ext base.
 *   - Nvc56fControl publishes GPGet at 0x88.
 *   - USERD must come from vidmem.  Ada ships no dev_ram.h definition of
 *     NV_RAMRL_ENTRY_CHAN_USERD_PTR_HI_HW at all -- only ga100, gh100 and
 *     gb10b define it -- and picks up GA100's 8-bit check through the chip
 *     HAL: g_kernel_channel_nvoc.c:1353 binds GA100, GA102-GA107 and
 *     AD102-AD107 together to kchannelIsUserdAddrSizeValid_GA100.  So the 2^40
 *     limit measured on the A100 applies here too.  See the field comment in
 *     hal/arch.h and tests/unit/mc_userd_addr_test.c.
 *
 * Compute is absent, as on Ampere.  ADA_COMPUTE_A is listed in .cls so
 * detection verifies it against the device class list, but no QMD builder
 * exists: Ada uses the same V03 layout family as Ampere, which has to be
 * transcribed from Mesa NVK rather than written from memory.  Until then
 * .compute stays NULL and the feature engine withholds every compute-dependent
 * feature.  One V03 builder will serve both architectures.
 *
 * That leaves host transfers on the UVM and sysmem-carrier VA spaces, which is
 * the same reduced set Ampere delivers.
 */

#include "class/cl50a0.h"        /* NV50_MEMORY_VIRTUAL */
#include "class/cl90f1.h"        /* FERMI_VASPACE_A */
#include "class/cla06c.h"        /* KEPLER_CHANNEL_GROUP_A */
#include "class/clc561.h"        /* AMPERE_USERMODE_A */
#include "class/clc56f.h"        /* AMPERE_CHANNEL_GPFIFO_A */
#include "class/clc7b5.h"        /* AMPERE_DMA_COPY_B */
#include "class/clc9c0.h"        /* ADA_COMPUTE_A */
#include "ctrl/ctrl2080/ctrl2080mc.h"  /* ARCHITECTURE_AD100 */

#include "hal/arch.h"
#include "internal/types.h"

/* Shared with Ampere by class identity -- see the file comment. */
extern const mc_arch_ops_t mc_arch_ampere_ops;

const mc_arch_desc_t mc_arch_ada = {
    .name   = "ada",
    .family = MC_ARCH_ADA,

    .rm_architecture = NV2080_CTRL_MC_ARCH_INFO_ARCHITECTURE_AD100,  /* 0x190 */
    /* Any AD10x implementation.  The class list is verified separately at
     * detection time, which is the check that actually matters. */
    .rm_impl_mask    = 0,

    .cls = {
        /* Ampere's classes, allocated unchanged -- Ada defines none of its
         * own for these roles. */
        .usermode       = AMPERE_USERMODE_A,        /* 0xC561 */
        .channel_gpfifo = AMPERE_CHANNEL_GPFIFO_A,  /* 0xC56F */
        /* The _B copy class, not GA100's _A: AD10x follows the GA10x consumer
         * parts here.  clc9b5.h is BLACKWELL_DMA_COPY_A (guard
         * __gb100_clc9b5_h__), not an Ada class, despite the numbering. */
        .dma_copy       = AMPERE_DMA_COPY_B,        /* 0xC7B5 */
        /* The one class Ada brings itself. */
        .compute        = ADA_COMPUTE_A,            /* 0xC9C0 */
        .channel_group  = KEPLER_CHANNEL_GROUP_A,   /* 0xA06C */
        .vaspace        = FERMI_VASPACE_A,          /* 0x90F1 */
        .virtual_memory = NV50_MEMORY_VIRTUAL,      /* 0x50A0 */
    },

    .lay = {
        /* Nvc56fControl, the same channel-control block Ampere uses, so the
         * 512-byte slot and slot-16 placement carry over unchanged. */
        .userd_slot_bytes      = 512,
        .userd_offset          = MC_USERD_OFFSET,
        /* Invariant from Ampere through Blackwell; Ada only ever gets the
         * BAR0 window, but the offset within it is unchanged.  See the
         * derivation on MC_VF_DOORBELL_OFFSET. */
        .vf_doorbell_offset    = MC_VF_DOORBELL_OFFSET,
        .usermode_window_bytes = MC_USERMODE_SIZE,
        .big_page_bytes        = MC_VA_POOL_ALIGN_BYTES,
        .vidmem_align_bytes    = MC_VIDMEM_ALIGN_BYTES,
        .gpfifo_entries        = MC_GPFIFO_ENTRIES,
    },

    .cap = {
        .usermode_takes_params = false,
        .usermode_bar1_capable = false,
        .gpfifo_has_ext_base   = false,
        .userd_exposes_gpget   = true,
        /* AD10x binds to kchannelIsUserdAddrSizeValid_GA100 -- 8 bits, 2^40.
         * See the file comment. */
        .userd_requires_vidmem = true,
    },

    .ops     = &mc_arch_ampere_ops,
    /* No QMD V03 builder yet -- see the file comment. */
    .compute = NULL,
    /* No cubins: only useful with compute support, which is absent. */
};
