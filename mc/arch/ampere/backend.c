/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * arch/ampere/backend.c — the Ampere (GA100) architecture descriptor.
 *
 * What Ampere cannot do, and why
 * ------------------------------
 * AMPERE_USERMODE_A is RS_NONE (resource_list.h): it takes no allocation
 * parameters at all.  The bBar1Mapping field that selects a BAR1-mapped
 * usermode aperture lives in NV_HOPPER_USERMODE_A_PARAMS, and Ampere has no
 * such struct -- so there is no BAR1 aperture to request, no page to install
 * a doorbell PTE against, and therefore no FB carrier and nothing
 * SM-authored.  That is a property of the silicon on any driver, not a gap
 * to close later.
 *
 * The feature engine derives all of that from cap.usermode_bar1_capable
 * below; nothing here lists features.
 *
 * Compute is deliberately absent for now.  GA100 uses QMD V03, a different
 * bit layout from Hopper's V04 -- not a field shuffle -- and the pinned tree
 * ships no QMD header beyond Kepler's, so V03 positions would have to be
 * transcribed from Mesa NVK exactly as V04's were.  Writing them without that
 * reference in front of us would put unverified bit positions into a
 * structure SKED dereferences, whose failure mode is an Xid rather than an
 * error return.  Until a V03 builder is written against NVK and checked on
 * this hardware, .compute stays NULL and the compute-dependent features are
 * simply unavailable -- which the feature engine already handles and
 * tests/unit/mc_feature_test.c already covers.
 *
 * Everything below is established from the pinned headers; see the header
 * name against each field.
 *
 * KNOWN BLOCKER — channel bring-up does not yet work on this architecture.
 * ----------------------------------------------------------------------
 * NV_RAMRL_ENTRY_CHAN_USERD_PTR_HI_HW is 8 bits on GA100
 * (swref/published/ampere/ga100/dev_ram.h) but 20 bits on GH100, so a
 * channel's USERD must sit below 2^40 here where Hopper allows 2^52.  RM
 * enforces it in kchannelIsUserdAddrSizeValid_GA100 and returns
 * NV_ERR_INVALID_ADDRESS from the channel allocation.
 *
 * libmc's UVM channel places GPFIFO+USERD in vidmem, which on the A100 tested
 * (a passthrough VM) resolves to roughly 1 TiB physical -- fine on Hopper,
 * out of range here.  The fix is a placement decision, not a constant: where
 * USERD may live is now an architecture property.  Rather than guess, the
 * next step is to trace what libcuda does for a channel on Ampere and match
 * it, which is the same method that established every other bit-exact
 * behaviour in this library.
 *
 * Detection, feature derivation and the reduced feature set are all verified
 * working on real A100 silicon; only channel bring-up is outstanding.
 */

#include "class/cl50a0.h"        /* NV50_MEMORY_VIRTUAL */
#include "class/cl90f1.h"        /* FERMI_VASPACE_A */
#include "class/cla06c.h"        /* KEPLER_CHANNEL_GROUP_A */
#include "class/clc561.h"        /* AMPERE_USERMODE_A */
#include "class/clc56f.h"        /* AMPERE_CHANNEL_GPFIFO_A */
#include "class/clc6b5.h"        /* AMPERE_DMA_COPY_A */
#include "class/clc6c0.h"        /* AMPERE_COMPUTE_A */
#include "ctrl/ctrl2080/ctrl2080mc.h"  /* ARCHITECTURE_GA100 */

#include "hal/arch.h"
#include "internal/types.h"

extern const mc_arch_ops_t mc_arch_ampere_ops;

const mc_arch_desc_t mc_arch_ampere = {
    .name   = "ampere",
    .family = MC_ARCH_AMPERE,

    .rm_architecture = NV2080_CTRL_MC_ARCH_INFO_ARCHITECTURE_GA100,  /* 0x170 */
    /* Any GA100 implementation.  The class list is verified separately at
     * detection time, which is the check that actually matters. */
    .rm_impl_mask    = 0,

    .cls = {
        .usermode       = AMPERE_USERMODE_A,        /* 0xC561 */
        .channel_gpfifo = AMPERE_CHANNEL_GPFIFO_A,  /* 0xC56F */
        /* GA100 uses DMA_COPY_A and COMPUTE_A; the GA10x consumer parts use
         * the _B forms (0xC7B5 / 0xC7C0).  A GA10x backend is a separate
         * descriptor, not a wider implementation mask here. */
        .dma_copy       = AMPERE_DMA_COPY_A,        /* 0xC6B5 */
        .compute        = AMPERE_COMPUTE_A,         /* 0xC6C0 */
        .channel_group  = KEPLER_CHANNEL_GROUP_A,   /* 0xA06C */
        .vaspace        = FERMI_VASPACE_A,          /* 0x90F1 */
        .virtual_memory = NV50_MEMORY_VIRTUAL,      /* 0x50A0 */
    },

    .lay = {
        /* Nvc56fControl has the same shape as Hopper's Nvc86fControl --
         * GPGet at 0x88, GPPut at 0x8c -- so the 512-byte slot and the
         * slot-16 placement carry over unchanged. */
        .userd_slot_bytes      = 512,
        .userd_offset          = MC_USERD_OFFSET,
        /* The VF doorbell sits at the same offset inside the window whether
         * that window is carved from BAR0 or BAR1: the base and the register
         * pick up the same virtualRegPhysOffset.  See the derivation on
         * MC_VF_DOORBELL_OFFSET.  Ampere only ever gets the BAR0 window, but
         * the offset within it is unchanged. */
        .vf_doorbell_offset    = MC_VF_DOORBELL_OFFSET,
        .usermode_window_bytes = MC_USERMODE_SIZE,
        .big_page_bytes        = MC_VA_POOL_ALIGN_BYTES,
        .vidmem_align_bytes    = MC_VIDMEM_ALIGN_BYTES,
        .gpfifo_entries        = MC_GPFIFO_ENTRIES,
    },

    .cap = {
        /* RS_NONE: allocate with NULL parameters. */
        .usermode_takes_params = false,
        /* No bBar1Mapping concept exists -- see the file comment. */
        .usermode_bar1_capable = false,
        /* clc56f.h defines no PB_EXTENDED_BASE_OPERAND and no
         * SET_PB_SEGMENT_EXTENDED_BASE opcode (clc86f.h defines both), so a
         * pushbuffer at or above 2^40 is unreachable on this class.  Every
         * address libmc uses sits far below that; write_gp_entry fails loudly
         * rather than silently if one ever does not. */
        .gpfifo_has_ext_base   = false,
        /* Nvc56fControl publishes GPGet at 0x88. */
        .userd_exposes_gpget   = true,
    },

    .ops     = &mc_arch_ampere_ops,
    /* No QMD V03 builder yet -- see the file comment. */
    .compute = NULL,
    /* No cubins: SASS is per-SM-version and these are only useful with
     * compute support, which is absent. */
};
