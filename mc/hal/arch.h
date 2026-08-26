/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * hal/arch.h — the per-architecture descriptor.
 *
 * What belongs here
 * -----------------
 * Data, and only data: class ids, layout offsets, alignments, capability
 * bits.  Measured against the pinned headers, the overwhelming majority of
 * what differs between the architectures libmc targets is exactly that --
 * substituting a number, not running different code.  Keeping it as a plain
 * const struct means two backends can be diffed by eye and validated by a
 * host-only test with no GPU present.
 *
 * Behaviour that genuinely differs -- an allocation whose parameter struct
 * has a different *shape*, a QMD whose bit layout is a different version --
 * becomes a function pointer instead.  Those tables land in a later step;
 * resisting the urge to make everything a hook is what keeps most backends
 * nearly free.
 *
 * Capability bits vs. behaviour
 * ----------------------------
 * Where a class simply lacks something (Ampere's channel class has no
 * extended-base pushbuffer opcode; Blackwell's USERD publishes no GPGet),
 * that is a bool here consumed by shared code -- not a separate
 * implementation.  An arch-specific copy of a 40-line function that differs
 * in one branch is how backends silently drift apart.
 */
#ifndef MC_HAL_ARCH_H
#define MC_HAL_ARCH_H

#include <stdbool.h>
#include <stdint.h>

#include "nvtypes.h"

/* Architecture family, as reported by NV2080_CTRL_CMD_MC_GET_ARCH_INFO.
 * Detection lands in a later step; the enum exists now so the descriptor can
 * name what it describes. */
typedef enum {
    MC_ARCH_UNKNOWN = 0,
    MC_ARCH_AMPERE,
    MC_ARCH_ADA,
    MC_ARCH_HOPPER,
    MC_ARCH_BLACKWELL,
} mc_arch_family_t;

/* RM class ids.  Every hClass libmc passes to NV_ESC_RM_ALLOC, plus the two
 * that appear as SET_OBJECT payloads in method streams. */
struct mc_arch_classes {
    NvU32 usermode;         /* HOPPER_USERMODE_A                            */
    NvU32 channel_gpfifo;   /* HOPPER_CHANNEL_GPFIFO_A                      */
    NvU32 dma_copy;         /* HOPPER_DMA_COPY_A   (also SET_OBJECT)        */
    NvU32 compute;          /* HOPPER_COMPUTE_A    (also SET_OBJECT)        */
    NvU32 channel_group;    /* KEPLER_CHANNEL_GROUP_A — stable since Kepler */
    NvU32 vaspace;          /* FERMI_VASPACE_A     — stable since Fermi     */
    NvU32 virtual_memory;   /* NV50_MEMORY_VIRTUAL — stable since NV50      */
};

/* Channel-control and aperture layout. */
struct mc_arch_layout {
    /* USERD slot size for this channel class, and the offset libmc places
     * USERD at inside its combined GPFIFO+USERD allocation.  The offset must
     * be a whole number of slots; the registry validates that. */
    NvU32 userd_slot_bytes;
    NvU32 userd_offset;

    /* Byte offset of the VF doorbell inside the mapped USERMODE window.
     * Invariant at 0x90 from Ampere through Blackwell -- see the derivation
     * on MC_VF_DOORBELL_OFFSET in internal/types.h.  It lives here so a
     * backend must state it rather than inherit it silently, and the
     * registry asserts every backend agrees. */
    NvU32 vf_doorbell_offset;

    NvU64 usermode_window_bytes;  /* VF register aperture size              */
    NvU64 big_page_bytes;         /* UVM big-page size; VA-pool alignment   */
    NvU64 vidmem_align_bytes;     /* NV_MEMORY_ALLOCATION_PARAMS.alignment  */
    NvU32 gpfifo_entries;         /* ring depth; power of two               */
};

/* Things a class either has or does not. */
struct mc_arch_caps {
    /* HOPPER_USERMODE_A and BLACKWELL_USERMODE_A take an optional
     * NV_HOPPER_USERMODE_A_PARAMS; VOLTA/TURING/AMPERE_USERMODE_A are
     * RS_NONE and must be allocated with NULL parameters.  See
     * src/nvidia/src/kernel/rmapi/resource_list.h. */
    bool usermode_takes_params;

    /* Whether a BAR1-mapped usermode aperture can be requested at all.
     * False on Ampere and Ada, which have no bBar1Mapping concept -- and so
     * no BAR1 doorbell PTE, and therefore none of the SM-authored or
     * doorbell-ringing paths. */
    bool usermode_bar1_capable;

    /* Whether the channel class has GP_ENTRY0_PB_EXTENDED_BASE_OPERAND, and
     * so can address a pushbuffer at or above 2^40.  Present on
     * HOPPER/BLACKWELL_CHANNEL_GPFIFO_A, absent on AMPERE_CHANNEL_GPFIFO_A. */
    bool gpfifo_has_ext_base;

    /* Whether USERD publishes a readable GPGet.  False on
     * BLACKWELL_CHANNEL_GPFIFO_A (Nvc96fControl is GPPut-only), which is why
     * progress polling must have a semaphore fallback. */
    bool userd_exposes_gpget;
};

/* Forward declaration: the ops table references the descriptor. */
typedef struct mc_arch_desc mc_arch_desc_t;

/* Channel roles, mirrored from internal/types.h.
 *
 * hal/ must not include internal/types.h -- types.h includes THIS header for
 * the descriptor, so the dependency has to run one way.  The role enum is
 * duplicated as an opaque integer in the ops signature rather than pulling
 * the whole data model in. */
typedef int mc_arch_role_t;

/* ── Behaviour that is not expressible as data ────────────────────────────
 *
 * Four hooks, each justified by a specific divergence in the pinned headers
 * rather than by speculation:
 *
 *   alloc_usermode      HOPPER/BLACKWELL_USERMODE_A take an optional
 *                       NV_HOPPER_USERMODE_A_PARAMS; VOLTA/TURING/AMPERE are
 *                       RS_NONE and must be allocated with NULL.  The
 *                       parameter struct's *shape* differs, so no amount of
 *                       data substitution covers it.
 *
 *   userd_*             USERD's layout belongs to the channel class.  Ampere
 *                       and Hopper agree (GPGet 0x88, GPPut 0x8c), but
 *                       BLACKWELL_CHANNEL_GPFIFO_A's Nvc96fControl publishes
 *                       GPPut and NO GPGet at all, so the getter is a try_
 *                       form and callers need a fallback.
 *
 *   engine_for_role     Which engine a role runs on is a policy decision, not
 *                       a constant: today every CE role shares one non-GRCE
 *                       LCE and compute uses GR0, but a part with a different
 *                       copy-engine population may want to spread roles
 *                       across LCEs.
 *
 * Everything else about an architecture is data -- see mc_arch_desc_t above.
 */
typedef struct {
    /* Allocate the USERMODE aperture object.  `bar1` requests the BAR1
     * mapping and is only meaningful where cap.usermode_bar1_capable. */
    NvHandle (*alloc_usermode)(int ctl_fd, const mc_arch_desc_t *arch,
                               NvHandle root, NvHandle subdevice, bool bar1);

    void     (*userd_set_gpput)(void *userd, uint32_t value);
    uint32_t (*userd_get_gpput)(const void *userd);
    /* False when the class publishes no GPGet.  A caller that treats a false
     * return as "zero" has no correct behaviour on Blackwell. */
    bool     (*userd_try_get_gpget)(const void *userd, uint32_t *out_gpget);

    /* Engine type for a channel role.  `selected_lce` is the engine type
     * chosen by copy-engine selection, offered so a policy can use it. */
    NvU32 (*engine_for_role)(const mc_arch_desc_t *arch,
                             mc_arch_role_t role, NvU32 selected_lce);
} mc_arch_ops_t;

/* The descriptor proper. */
struct mc_arch_desc {
    const char      *name;          /* "hopper" — appears in log lines      */
    mc_arch_family_t family;

    /* How this backend is recognised, from NV2080_CTRL_CMD_MC_GET_ARCH_INFO.
     *
     * `rm_architecture` is the discriminator and must always be tested:
     * implementation values are namespaced PER ARCHITECTURE, so GA100, GH100,
     * AD100, GB100 and GB200 all report implementation 0.  Matching on an
     * implementation alone would claim four other architectures.
     *
     * `rm_impl_mask` narrows within an architecture: bit i set means
     * implementation i is accepted.  Zero means "any implementation of this
     * architecture", which is the right default -- rejecting an unlisted
     * implementation of an architecture we do support trades a working GPU
     * for no safety, since the class set is what actually matters and that is
     * verified separately against the device's class list. */
    NvU32            rm_architecture;
    NvU32            rm_impl_mask;

    struct mc_arch_classes cls;
    struct mc_arch_layout  lay;
    struct mc_arch_caps    cap;

    const mc_arch_ops_t   *ops;
};

/* Backends define one of these.  Ops tables join it in a later step. */
extern const mc_arch_desc_t mc_arch_hopper;

#endif /* MC_HAL_ARCH_H */
