/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * arch/ampere_ga10x/backend.c — the Ampere GA10x architecture descriptor.
 *
 * Why GA10x is a separate backend from GA100
 * ------------------------------------------
 * Both report NV2080_CTRL_MC_ARCH_INFO_ARCHITECTURE_GA100 (0x170).  They are
 * the same architecture in RM's taxonomy and a different one in the classes
 * they expose: GA100 carries AMPERE_DMA_COPY_A (0xC6B5) and AMPERE_COMPUTE_A
 * (0xC6C0), while the GA10x parts carry the _B forms, 0xC7B5 and 0xC7C0.
 * Nothing in the architecture id distinguishes them, so `implementation`
 * does -- this is the first pair of backends where rm_impl_mask is load-
 * bearing rather than documentation, and where "any implementation of this
 * architecture" would be wrong.
 *
 * Established on hardware, not inferred.  An A2 (GA107) reported
 * architecture=0x170 implementation=0x7, was claimed by the GA100 backend
 * because its mask was 0, and then failed the class-list check on the copy
 * class.  The class list it did offer contained 0xC7B5 and 0xC7C0 alongside
 * AMPERE_USERMODE_A and AMPERE_CHANNEL_GPFIFO_A, which is exactly this
 * descriptor.
 *
 * Everything else follows GA100, and the shared pieces are shared:
 *
 *   - Same AMPERE_USERMODE_A and AMPERE_CHANNEL_GPFIFO_A, so mc_arch_ampere_ops
 *     applies by class identity rather than by resemblance -- one header
 *     each, not two that happen to agree.  Same argument as arch/ada.
 *   - Same QMD V03, so mc_arch_ampere_compute applies.  The compute *class*
 *     differs (0xC7C0 against 0xC6C0) but the QMD layout does not; those are
 *     independent axes, and here the class libmc allocates happens to equal
 *     the method dialect the shared emitters write, which on Hopper it does
 *     not.
 *   - Same cb0 offsets: sm_86 places kernel arguments at 0x160/0x164/0x168
 *     with REG 8, identical to sm_80 and sm_89.  Checked with cuobjdump on
 *     the generated cubin rather than assumed from the family.
 *   - No BAR1 usermode aperture, so no doorbell PTE, no FB carrier, nothing
 *     SM-authored.  See arch/ampere/backend.c for the resource_list.h
 *     evidence; AMPERE_USERMODE_A is the same class here.
 *   - USERD must come from vidmem: GA102-GA107 bind to
 *     kchannelIsUserdAddrSizeValid_GA100 through the chip HAL
 *     (g_kernel_channel_nvoc.c:1353), so the 8-bit runlist address field and
 *     its 2^40 limit apply exactly as on GA100.
 *
 * GA10B (0x0B, the Orin SoC part) is deliberately NOT claimed.  It is a Tegra
 * device with a different memory topology, and no hardware was available to
 * check that its class set matches.  Add it to the mask only alongside a run
 * on one.
 */

#include "class/cl50a0.h"        /* NV50_MEMORY_VIRTUAL */
#include "class/cl90f1.h"        /* FERMI_VASPACE_A */
#include "class/cla06c.h"        /* KEPLER_CHANNEL_GROUP_A */
#include "class/clc561.h"        /* AMPERE_USERMODE_A */
#include "class/clc56f.h"        /* AMPERE_CHANNEL_GPFIFO_A */
#include "class/clc7b5.h"        /* AMPERE_DMA_COPY_B */
#include "class/clc7c0.h"        /* AMPERE_COMPUTE_B */
#include "ctrl/ctrl2080/ctrl2080mc.h"  /* ARCHITECTURE_GA100, IMPLEMENTATION_GA10x */

#include "hal/arch.h"
#include "internal/types.h"

#include "store_dword_sass.h"

/* Shared with GA100 by class identity, and by QMD version, respectively. */
extern const mc_arch_ops_t     mc_arch_ampere_ops;
extern const mc_compute_ops_t  mc_arch_ampere_compute;

/* The consumer/entry GA10x dies libmc claims.  Spelled out per part rather
 * than as a range: the gaps are real (there is no 0x1 or 0x5) and a range
 * would silently absorb whatever NVIDIA numbers next. */
#define MC_GA10X_IMPL_MASK                                            \
    ((1u << NV2080_CTRL_MC_ARCH_INFO_IMPLEMENTATION_GA102) |          \
     (1u << NV2080_CTRL_MC_ARCH_INFO_IMPLEMENTATION_GA103) |          \
     (1u << NV2080_CTRL_MC_ARCH_INFO_IMPLEMENTATION_GA104) |          \
     (1u << NV2080_CTRL_MC_ARCH_INFO_IMPLEMENTATION_GA106) |          \
     (1u << NV2080_CTRL_MC_ARCH_INFO_IMPLEMENTATION_GA107))

const mc_arch_desc_t mc_arch_ampere_ga10x = {
    .name   = "ampere_ga10x",
    .family = MC_ARCH_AMPERE,

    .rm_architecture = NV2080_CTRL_MC_ARCH_INFO_ARCHITECTURE_GA100,  /* 0x170 */
    .rm_impl_mask    = MC_GA10X_IMPL_MASK,

    .cls = {
        .usermode       = AMPERE_USERMODE_A,        /* 0xC561 */
        .channel_gpfifo = AMPERE_CHANNEL_GPFIFO_A,  /* 0xC56F */
        /* The _B forms.  This is the whole reason the backend exists. */
        .dma_copy       = AMPERE_DMA_COPY_B,        /* 0xC7B5 */
        .compute        = AMPERE_COMPUTE_B,         /* 0xC7C0 */
        .channel_group  = KEPLER_CHANNEL_GROUP_A,   /* 0xA06C */
        .vaspace        = FERMI_VASPACE_A,          /* 0x90F1 */
        .virtual_memory = NV50_MEMORY_VIRTUAL,      /* 0x50A0 */
    },

    .lay = {
        /* Nvc56fControl, the same channel-control block GA100 uses. */
        .userd_slot_bytes      = 512,
        .userd_offset          = MC_USERD_OFFSET,
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
        .userd_requires_vidmem = true,
    },

    .ops     = &mc_arch_ampere_ops,
    .compute = &mc_arch_ampere_compute,

    .kernels = {
        /* Compiled for sm_86 from kernels/store_dword.cu. */
        .store_dword = { mc_store_dword_kernel_sass,
                         MC_STORE_DWORD_SASS_LEN,
                         MC_STORE_DWORD_REGS },
        /* No sm_owner cubin: no BAR1 aperture, so nothing to ring. */
    },
};
