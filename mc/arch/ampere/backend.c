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
 * Compute uses QMD V03, built against Mesa NVK's Qmd3_0 -- see
 * arch/ampere/qmd_v03.c.  V03 is a different layout from Hopper's V04 rather
 * than a field shuffle: a different size, different bit positions throughout,
 * an extra SM_GLOBAL_CACHING_ENABLE, unshifted constant-buffer addresses, and
 * no QMD_TYPE field at all.  The sm_80 cubin's constant-bank offsets differ
 * from sm_90's too, and travel with the compute ops.
 *
 * Note this does NOT bring back anything doorbell-driven.  compute_launch
 * means a kernel can be launched and its stores land; MC_FEAT_CE_DOORBELL_RING
 * and MC_FEAT_XFER_SM still depend on the BAR1 aperture above and remain
 * unavailable here.
 *
 * Everything below is established from the pinned headers; see the header
 * name against each field.
 *
 * USERD placement
 * ---------------
 * NV_RAMRL_ENTRY_CHAN_USERD_PTR_HI_HW is 8 bits on GA100
 * (swref/published/ampere/ga100/dev_ram.h:28) against 20 on GH100, so a
 * channel's USERD must sit below 2^40 here where Hopper reaches 2^52.  A host
 * physical address does not reliably do that, so USERD cannot live in sysmem
 * -- cap.userd_requires_vidmem below, which core/channel.c honours when
 * allocating the GPFIFO+USERD buffer.
 *
 * Established by tracing, not inference: libcuda on this hardware puts USERD
 * in vidmem for every channel it creates, and the instrumented driver's
 * fifo/userd_resolve record shows libmc's sysmem placement resolving to
 * 1.007 TiB and being rejected while a vidmem one at 322 MiB is accepted.
 * See the field comment in hal/arch.h and tests/unit/mc_userd_addr_test.c,
 * which machine-checks the widths against the headers.
 *
 * Detection, feature derivation, the reduced feature set and host transfers
 * on the UVM and sysmem-carrier VA spaces are all verified on real A100
 * silicon.
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

#include "doorbell_sass.h"

extern const mc_arch_ops_t mc_arch_ampere_ops;
extern const mc_compute_ops_t mc_arch_ampere_compute;

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
        /* NV_RAMRL_ENTRY_CHAN_USERD_PTR_HI_HW is 8 bits here against 20 on
         * GH100/GB10B, so USERD must sit below 2^40 -- which a host physical
         * address does not reliably do.  See the field comment in hal/arch.h
         * for the header evidence and the measured addresses. */
        .userd_requires_vidmem = true,
    },

    .ops     = &mc_arch_ampere_ops,
    .compute = &mc_arch_ampere_compute,

    .kernels = {
        /* Compiled for sm_80 from kernels/doorbell.cu; the build asserts the
         * generated SASS still contains its system-scope store. */
        .doorbell = { mc_doorbell_kernel_sass,
                      MC_DOORBELL_SASS_LEN,
                      MC_DOORBELL_REGS },
        /* No sm_owner cubin: that kernel authors a submission and rings the
         * BAR1 doorbell, which this architecture has no aperture for.  The
         * feature engine withholds MC_FEAT_XFER_SM from cap.usermode_bar1_capable
         * regardless, so the blob would be dead weight. */
    },
};
