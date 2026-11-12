/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * arch/blackwell/backend.c — the Blackwell GB20x architecture descriptor,
 * claimed for GB206.
 *
 * What this backend covers
 * ------------------------
 * The host-authored path: VA spaces, carriers, a CE channel, and copies.
 * Compute is deliberately absent -- see "No compute" below -- which the
 * descriptor expresses as a NULL compute pointer and empty kernel blobs.
 * hal/arch.h calls that a normal state rather than an error: the features
 * that need compute report unavailable through mc_feature_supported(), and
 * everything else works.
 *
 * Established on hardware, not inferred
 * -------------------------------------
 * An RTX PRO 2000 Blackwell Generation Laptop GPU (GB206) running driver
 * 620.70.06 reported architecture=0x1b0 implementation=0x6 and the class
 * list below.  The engine classes it offers, read back through the
 * MC_ARCH_FORCE bring-up path that hal/detect.c provides for exactly this
 * purpose:
 *
 *     0xc761  BLACKWELL_USERMODE_A
 *     0xc96f  BLACKWELL_CHANNEL_GPFIFO_A
 *     0xca6f  BLACKWELL_CHANNEL_GPFIFO_B
 *     0xcab5  BLACKWELL_DMA_COPY_B
 *     0xcec0  BLACKWELL_COMPUTE_B
 *     0xa06c  KEPLER_CHANNEL_GROUP_A
 *     0x90f1  FERMI_VASPACE_A
 *     0x50a0  NV50_MEMORY_VIRTUAL
 *
 * Note what is NOT there: BLACKWELL_DMA_COPY_A (0xc9b5) and
 * BLACKWELL_COMPUTE_A (0xcdc0).  GB206 carries the _B forms only, which is
 * the same split arch/ampere_ga10x/backend.c documents for GA10x against
 * GA100 -- and the same reason that backend exists.  A descriptor built from
 * the _A class names would compile, then fail the class-list check at init.
 *
 * Why the implementation mask is narrow
 * -------------------------------------
 * ARCHITECTURE_GB200 (0x1b0) is the family id for all of GB20x, not just the
 * GB200 part: ctrl2080mc.h lists GB200, GB202, GB203, GB205, GB206, GB207,
 * GB20B and GB20C under "valid ARCHITECTURE_GB20x implementation values".
 * Only GB206 (implementation 6) has been run, so only bit 6 is set.
 *
 * That is deliberately stricter than the "mask == 0 means any" default
 * hal/arch.h recommends, and it follows the precedent arch/ampere_ga10x sets
 * by declining GA10B: the class set is what actually matters, and it is not
 * uniform across this family.  GB20B and GB20C in particular bind to a
 * different USERD-address HAL than GB206 does (see the cap note below), which
 * is concrete evidence that GB20x is not one thing.  Widening the mask is a
 * one-bit change -- but it belongs with a run on that part, not ahead of one.
 *
 * No compute
 * ----------
 * GB206 exposes BLACKWELL_COMPUTE_B (0xcec0) and `cls.compute` records that,
 * so the class list is reported accurately and hal/detect.c issues no
 * spurious "compute class absent" warning.  But `compute` is NULL, because
 * two things needed to drive it cannot be established from the pinned tree:
 *
 *   - Method dialect.  class/clcec0.h is a stub -- 30 lines, a class id and
 *     nothing else, no method addresses at all.  hal/method_abi.h can assert
 *     that BLACKWELL_COMPUTE_A (0xcdc0) agrees with the C7C0 dialect the
 *     shared emitters write, and does; it can say nothing about 0xcec0, and
 *     neither can this file.  Guessing that _B inherits _A's dialect is
 *     exactly the kind of assumption CLAUDE.md requires be established from
 *     the headers or a capture first.
 *
 *   - QMD layout.  hal/arch.h records Blackwell as QMD V05.  There is no V05
 *     builder in the tree (only V03 for Ampere and V04 for Hopper) and no
 *     clcec0qmd.h anywhere in the driver source to build one from.
 *
 * A third thing is merely missing rather than unknowable: there is no sm_120
 * cubin for the embedded kernels, so both blobs are NULL.  hal/arch.h
 * anticipates this ("generating a cubin needs nvcc and a toolkit that targets
 * the SM version in question, which is not always to hand").
 *
 * Adding compute later means a V05 QMD builder with golden bits in
 * tests/unit/mc_qmd_test.c, sm_120 cubins, the cb0 offsets read off
 * `cuobjdump --dump-sass` for those cubins, and evidence for 0xcec0's method
 * dialect.  None of it changes this file beyond two pointers.
 */

#include "class/cl50a0.h"        /* NV50_MEMORY_VIRTUAL */
#include "class/cl90f1.h"        /* FERMI_VASPACE_A */
#include "class/cla06c.h"        /* KEPLER_CHANNEL_GROUP_A */
#include "class/clc761.h"        /* BLACKWELL_USERMODE_A */
#include "class/clc96f.h"        /* BLACKWELL_CHANNEL_GPFIFO_A */
#include "class/clcab5.h"        /* BLACKWELL_DMA_COPY_B */
#include "class/clcec0.h"        /* BLACKWELL_COMPUTE_B */

#include "ctrl/ctrl2080/ctrl2080mc.h"  /* ARCHITECTURE_GB200, IMPLEMENTATION_GB206 */

#include "hal/arch.h"

extern const mc_arch_ops_t mc_arch_blackwell_ops;
#include "internal/types.h"

const mc_arch_desc_t mc_arch_blackwell = {
    .name   = "blackwell",
    .family = MC_ARCH_BLACKWELL,

    .rm_architecture = NV2080_CTRL_MC_ARCH_INFO_ARCHITECTURE_GB200,  /* 0x1b0 */
    /* GB206 only; see "Why the implementation mask is narrow" above. */
    .rm_impl_mask    = 1u << NV2080_CTRL_MC_ARCH_INFO_IMPLEMENTATION_GB206,

    .cls = {
        .usermode       = BLACKWELL_USERMODE_A,        /* 0xC761 */
        .channel_gpfifo = BLACKWELL_CHANNEL_GPFIFO_A,  /* 0xC96F */
        /* The _B copy class, because GB206 exposes no _A.  Same situation as
         * GA10x, and the reason that is a separate backend. */
        .dma_copy       = BLACKWELL_DMA_COPY_B,        /* 0xCAB5 */
        /* Recorded for the class-list check only -- `compute` below is NULL,
         * so nothing allocates this. */
        .compute        = BLACKWELL_COMPUTE_B,         /* 0xCEC0 */
        .channel_group  = KEPLER_CHANNEL_GROUP_A,      /* 0xA06C */
        .vaspace        = FERMI_VASPACE_A,             /* 0x90F1 */
        .virtual_memory = NV50_MEMORY_VIRTUAL,         /* 0x50A0 */
    },

    /* Identical to Hopper's, from the same macros in internal/types.h, so the
     * derivations stay attached to the constants they explain.  The VF
     * doorbell in particular is invariant at +0x90 from Ampere through
     * Blackwell; stating it per backend rather than sharing a constant is
     * what tests/unit/mc_arch_desc_test.c checks. */
    .lay = {
        .userd_slot_bytes      = 512,
        .userd_offset          = MC_USERD_OFFSET,
        .vf_doorbell_offset    = MC_VF_DOORBELL_OFFSET,
        .usermode_window_bytes = MC_USERMODE_SIZE,
        .big_page_bytes        = MC_VA_POOL_ALIGN_BYTES,
        .vidmem_align_bytes    = MC_VIDMEM_ALIGN_BYTES,
        .gpfifo_entries        = MC_GPFIFO_ENTRIES,
    },

    .ops     = &mc_arch_blackwell_ops,
    .compute = NULL,     /* see "No compute" above */

    .kernels = {
        /* No sm_120 cubin in this build.  NULL bytes makes the features that
         * launch a kernel unavailable; it does not make anything broken. */
        .sm_owner    = { NULL, 0, 0 },
        .store_dword = { NULL, 0, 0 },
    },

    .cap = {
        /* BLACKWELL_USERMODE_A is RS_OPTIONAL(NV_HOPPER_USERMODE_A_PARAMS) --
         * the same parameter struct Hopper uses, not merely a similar one:
         * src/nvidia/src/kernel/rmapi/resource_list.h:960-964 against
         * :950-954.  That is why ops.c can fill in one struct for both. */
        .usermode_takes_params = true,
        .usermode_bar1_capable = true,

        /* NVC96F has GP_ENTRY0_PB_EXTENDED_BASE_OPERAND, so a pushbuffer at
         * or above 2^40 is addressable. */
        .gpfifo_has_ext_base   = true,

        /* Nvc96fControl publishes GPPut at 0x8c and NO GPGet at all -- the
         * divergence hal/arch.h cites when explaining why the getter is a
         * try_ form.  Confirmed in class/clc96f.h; Nvca6fControl (_B) is the
         * same.  Progress polling must use the semaphore fallback. */
        .userd_exposes_gpget   = false,

        /* GB206 is not in the GB10B branch of the USERD-address HAL, so it
         * takes the GH100 default: NV_RAMRL_ENTRY_CHAN_USERD_PTR_HI_HW is 20
         * bits, reaching 2^52, and USERD may live in sysmem.
         *
         * From the driver's own dispatch at
         * src/nvidia/generated/g_kernel_channel_nvoc.c:1126-1134 -- the
         * GB10B arm names "GB10B | GB10C | GB20B | GB20C" and GB206 is in
         * none of them, so the else arm binds
         * kchannelIsUserdAddrSizeValid_GH100.  (The published swref headers
         * stop at blackwell/gb10b/dev_ram.h; there is no gb20x dev_ram.h to
         * read the field width from directly, which is why the HAL binding is
         * the evidence here rather than the register header.)
         *
         * tests/unit/mc_userd_addr_test.c already encodes this for the
         * family and checks this flag against it. */
        .userd_requires_vidmem = false,
    },
};
