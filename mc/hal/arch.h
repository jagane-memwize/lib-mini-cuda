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

    /* Whether a channel's USERD must be allocated from vidmem rather than
     * sysmem, because the runlist entry cannot hold an arbitrary host
     * physical address.
     *
     * NV_RAMRL_ENTRY_CHAN_USERD_PTR_HI_HW holds the HIGH 32 bits of the USERD
     * physical address, and its width is architecture-specific:
     *
     *     ampere/ga100/dev_ram.h:28     (7+1*32):(0+1*32)   ->  8 bits, 2^40
     *     hopper/gh100/dev_ram.h:28    (19+1*32):(0+1*32)   -> 20 bits, 2^52
     *     blackwell/gb10b/dev_ram.h:28 (19+1*32):(0+1*32)   -> 20 bits, 2^52
     *
     * kchannelIsUserdAddrSizeValid_GA100 round-trips both halves through
     * SF_MASK and RM returns NV_ERR_INVALID_ADDRESS (0x1e) from the channel
     * allocation when the address does not fit.
     *
     * A host physical address routinely exceeds 2^40, so on Ampere sysmem is
     * not a safe home for USERD; vidmem is, because FB physical addresses are
     * GPU-local and start near zero.  Measured on an A100 (2026-08-26) with
     * the instrumented driver's fifo/userd_resolve record:
     *
     *     sysmem USERD  address_space=1  0x101b0174000 (1.007 TiB)  -> 0x1e
     *     vidmem USERD  address_space=2  0x000014202000 (322 MiB)   -> OK
     *
     * The sysmem case overshoots 2^40 by only ~6.9 GiB, so a host with less
     * RAM could land under the limit and work by luck -- which is why this is
     * a placement rule rather than something to detect at runtime.  libcuda
     * sidesteps it entirely by never putting USERD in sysmem: on the same box
     * all 20 of its channels shared one vidmem NV01_MEMORY_LOCAL_USER
     * allocation.  That is also the recipe already described in
     * core/vaspace.c (GPFIFO + USERD in FB, pushbuffer + semaphore in
     * sysmem).
     *
     * Hopper and Blackwell can address the full 2^52 physical range from the
     * runlist entry, so their sysmem carrier needs no special handling and is
     * left exactly as it was. */
    bool userd_requires_vidmem;
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

/* ── Embedded device kernels ──────────────────────────────────────────────
 *
 * SASS is not portable across SM versions, so each architecture carries its
 * own compiled bytes for each kernel.  A NULL `bytes` means this build has no
 * cubin for that kernel on that architecture -- which is a normal state, not
 * an error: it makes the features that need the kernel unavailable, and
 * everything else still works.  That matters because generating a cubin needs
 * nvcc and a toolkit that targets the SM version in question, which is not
 * always to hand when a backend is first written.
 */
typedef struct {
    const uint8_t *bytes;
    uint32_t       len;
    uint32_t       regs;   /* register count the QMD must request */
} mc_kernel_blob_t;

typedef struct {
    mc_kernel_blob_t sm_owner;     /* SM authors an entire CE submission   */
    /* One thread, one system-scope store to a given address.  Named for what
     * it does rather than for one use of it: pointed at a BAR1 doorbell it
     * rings one, pointed at ordinary memory it is the minimal proof that a
     * kernel launches at all.  Only the second use is available on an
     * architecture without a BAR1 usermode aperture, which is why Ampere and
     * Ada carry this blob and no sm_owner blob. */
    mc_kernel_blob_t store_dword;
} mc_arch_kernels_t;

/* ── Compute (QMD) ────────────────────────────────────────────────────────
 *
 * The QMD is the structure SKED reads to launch a kernel, and its bit layout
 * is versioned: Hopper is V04, Ampere V03, Blackwell V05.  These are
 * different layouts rather than a field shuffle, so a backend supplies a
 * whole builder.
 *
 * The interface is the OPERATION, not the field setters.  libmc needs exactly
 * one thing from a QMD -- "launch this program, single CTA, single thread,
 * with constant buffer 0 bound here" -- and the individual setters are an
 * internal decomposition of that.  Hooking at setter granularity would force
 * every backend to expose the same decomposition even where its QMD version
 * does not decompose the same way, and would multiply nine hooks by four
 * architectures for no gain.
 *
 * A NULL compute pointer means the architecture has no compute support in
 * this build; compute-dependent features are unavailable rather than broken.
 */
typedef struct {
    uint32_t qmd_bytes;     /* size of a QMD image                          */
    uint32_t qmd_align;     /* required alignment of that image             */
    uint32_t cbuf_align;    /* required alignment of a bound constant buffer */
    uint8_t  version_major;
    uint8_t  version_minor;

    /* Constant-bank-0 layout for the embedded kernels.
     *
     * These are data, not behaviour, so they sit here rather than behind a
     * hook -- but note they are NOT properties of the QMD version.  nvcc
     * places kernel parameters at a different constant-bank offset per SM
     * version, so they are properties of the compiled SASS:
     *
     *     sm_90   dst 0x210/0x214   token 0x218   (CONSTANT[0] 540 bytes)
     *     sm_80   dst 0x160/0x164   token 0x168   (CONSTANT[0] 364 bytes)
     *
     * They live in this table because a backend already selects both its QMD
     * version and its cubins together, and the two must agree.  Read them off
     * `cuobjdump --dump-sass` for the arch's cubin -- never assume another
     * architecture's offsets carry over.
     *
     * cb0_bytes is what the kernel actually reads; callers round it up to
     * cbuf_align before allocating and binding. */
    uint32_t cb0_bytes;
    uint32_t cb0_off_dst_lo;
    uint32_t cb0_off_dst_hi;
    uint32_t cb0_off_token;

    /* Build a complete QMD for a single-CTA, single-thread launch.
     * `qmd` must be qmd_bytes long and qmd_align-aligned. */
    void (*build_qmd)(uint8_t *qmd,
                      uint64_t prog_gpu_va, uint32_t register_count,
                      uint64_t cb0_gpu_va,  uint32_t cb0_bytes);
} mc_compute_ops_t;

/* Bytes to allocate for a constant buffer 0: what the kernel reads, rounded
 * up to the alignment the class requires for a bound cbuf. */
static inline uint32_t mc_cb0_alloc_bytes(const mc_compute_ops_t *c)
{
    return (c->cb0_bytes + c->cbuf_align - 1u) & ~(c->cbuf_align - 1u);
}

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

    const mc_arch_ops_t     *ops;
    const mc_compute_ops_t  *compute;   /* NULL: no compute support */
    mc_arch_kernels_t        kernels;
};

/* Compiled-in backends.  Each is declared unconditionally -- a declaration
 * costs nothing -- while hal/registry.c decides membership from the
 * MC_HAVE_ARCH_<NAME> defines the build passes for whatever MC_ARCHS names. */
extern const mc_arch_desc_t mc_arch_ampere;        /* GA100 die   */
extern const mc_arch_desc_t mc_arch_ampere_ga10x;  /* GA102-GA107 */
extern const mc_arch_desc_t mc_arch_ada;
extern const mc_arch_desc_t mc_arch_hopper;
extern const mc_arch_desc_t mc_arch_blackwell;

#endif /* MC_HAL_ARCH_H */
