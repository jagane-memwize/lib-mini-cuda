/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * arch/hopper/backend.c — the Hopper (GH100) architecture descriptor.
 *
 * Every value here was previously a literal at its point of use or a macro in
 * internal/types.h.  The layout fields are still initialised FROM those
 * macros rather than re-spelling the numbers, so the derivations that justify
 * them -- in particular the twenty-line argument for why the VF doorbell is
 * at +0x90 regardless of which BAR the window is carved from -- stay attached
 * to the constants they explain instead of being duplicated here.
 *
 * This is what a backend costs: a struct initialiser.  A second architecture
 * that shares Hopper's behaviour is a copy of this file with different class
 * ids.
 */

#include "class/cl50a0.h"        /* NV50_MEMORY_VIRTUAL */
#include "class/cl90f1.h"        /* FERMI_VASPACE_A */
#include "class/cla06c.h"        /* KEPLER_CHANNEL_GROUP_A */
#include "class/clc661.h"        /* HOPPER_USERMODE_A */
#include "class/clc86f.h"        /* HOPPER_CHANNEL_GPFIFO_A */
#include "class/clc8b5.h"        /* HOPPER_DMA_COPY_A */
#include "class/clcbc0.h"        /* HOPPER_COMPUTE_A */

#include "ctrl/ctrl2080/ctrl2080mc.h"  /* ARCHITECTURE_GH100 */

#include "hal/arch.h"

/* Generated from the sources under mc/kernels; see KERNEL_RULE in the Makefile. */
#include "arch/hopper/sm_owner_sass.h"
#include "arch/hopper/doorbell_sass.h"

extern const mc_arch_ops_t    mc_arch_hopper_ops;
extern const mc_compute_ops_t mc_arch_hopper_compute;
#include "internal/types.h"

const mc_arch_desc_t mc_arch_hopper = {
    .name   = "hopper",
    .family = MC_ARCH_HOPPER,

    .rm_architecture = NV2080_CTRL_MC_ARCH_INFO_ARCHITECTURE_GH100,  /* 0x180 */
    /* Any GH100 implementation.  Discrete H100 parts (PCIe, SXM, NVL) all
     * report IMPLEMENTATION_GH100 = 0, and GH100_SOC (Grace Hopper) exposes
     * the same RM class set, so there is nothing to gain by rejecting it --
     * the class list is verified separately, which is the check that
     * actually matters. */
    .rm_impl_mask    = 0,

    .cls = {
        .usermode       = HOPPER_USERMODE_A,        /* 0xC661 */
        .channel_gpfifo = HOPPER_CHANNEL_GPFIFO_A,  /* 0xC86F */
        .dma_copy       = HOPPER_DMA_COPY_A,        /* 0xC8B5 */
        .compute        = HOPPER_COMPUTE_A,         /* 0xCBC0 */
        .channel_group  = KEPLER_CHANNEL_GROUP_A,   /* 0xA06C */
        .vaspace        = FERMI_VASPACE_A,          /* 0x90F1 */
        .virtual_memory = NV50_MEMORY_VIRTUAL,      /* 0x50A0 */
    },

    .lay = {
        /* 512-byte USERD slots; libmc places USERD at slot 16 of its
         * combined GPFIFO+USERD allocation, matching libcuda's placement. */
        .userd_slot_bytes     = 512,
        .userd_offset         = MC_USERD_OFFSET,
        .vf_doorbell_offset   = MC_VF_DOORBELL_OFFSET,
        .usermode_window_bytes = MC_USERMODE_SIZE,
        .big_page_bytes       = MC_VA_POOL_ALIGN_BYTES,
        .vidmem_align_bytes   = MC_VIDMEM_ALIGN_BYTES,
        .gpfifo_entries       = MC_GPFIFO_ENTRIES,
    },

    .ops     = &mc_arch_hopper_ops,
    .compute = &mc_arch_hopper_compute,

    .kernels = {
        .sm_owner = { mc_sm_owner_kernel_sass, MC_SM_OWNER_SASS_LEN,
                      MC_SM_OWNER_REGS },
        .doorbell = { mc_doorbell_kernel_sass, MC_DOORBELL_SASS_LEN,
                      MC_DOORBELL_REGS },
    },

    .cap = {
        /* HOPPER_USERMODE_A is RS_OPTIONAL(NV_HOPPER_USERMODE_A_PARAMS) and
         * libmc allocates both the BAR0 and BAR1 windows; the BAR1 one
         * carries the doorbell PTE the SM-authored paths depend on. */
        .usermode_takes_params = true,
        .usermode_bar1_capable = true,

        /* NVC86F has GP_ENTRY0_PB_EXTENDED_BASE_OPERAND. */
        .gpfifo_has_ext_base   = true,

        /* Nvc86fControl publishes GPGet at 0x88. */
        .userd_exposes_gpget   = true,
    },
};
