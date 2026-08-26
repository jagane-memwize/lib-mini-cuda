/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * arch/ampere/qmd_v03.c — NVK-style Ampere QMD V03 builder.
 *
 * Mirrors Mesa NVK's `Qmd3_0` (src/nouveau/compiler/nak/qmd.rs), the same
 * reference arch/hopper/qmd_v04.c was built from.  Bit ranges are named after
 * the NVC6C0_QMDV03_00_* macros in NVIDIA's clc6c0qmd.h, which does NOT ship
 * in the open kernel modules tree -- that tree carries only the Kepler
 * cla0c0qmd.h -- so the ranges here follow the header Mesa publishes and the
 * macro names are kept so the two can be read side by side.
 *
 * V03 is not V04 with fields moved.  The differences are structural, and each
 * one below is a fact taken from the reference rather than an adaptation:
 *
 *   - The QMD is 64 dwords (256 bytes), against V04's 96 dwords (384).
 *   - SM_GLOBAL_CACHING_ENABLE must be set.  V04's `new()` does not set it;
 *     V03's does (qmd.rs, Qmd3_0::new).
 *   - Constant buffer addresses are stored UNSHIFTED.  NVK instantiates V03
 *     with `qmd_impl_set_cbuf!(..., NONE, SHIFTED4)` against V04's
 *     `(SHIFTED6, SHIFTED4)`, so the address is written whole and only the
 *     size is >>4.
 *   - The register-count field is REGISTER_COUNT_V, not REGISTER_COUNT.
 *   - There is NO QMD_TYPE field.  V04 has one and libmc sets it to GRID_CTA;
 *     clc6c0qmd.h has no QMDV03_00_QMD_TYPE at all (only an unrelated
 *     QMDV02_03_DEPENDENT_QMD_TYPE), so it must not be written here.
 *   - Nothing is where it was.  CONSTANT_BUFFER_ADDR_LOWER(0) is at bit 1024
 *     against V04's 1536; PROGRAM_ADDRESS_LOWER is at 1536 against 1216;
 *     CONSTANT_BUFFER_VALID(i) is a run of single bits from 640 with stride 1
 *     against V04's stride 4 from 416.
 *
 * The bit-range writer is duplicated from qmd_v04.c rather than shared.  It is
 * fifteen lines, and sharing it would create a header whose only purpose is to
 * couple two QMD versions that NVIDIA is free to diverge further -- the same
 * reasoning arch/ampere/ops.c gives for spelling out its own USERD accessors.
 */

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "hal/arch.h"
#include "qmd_v03.h"

/* ── Bit-range write helper ─────────────────────────────────────────
 * Writes `value` into the QMD at bits [hi:lo] inclusive (MW(hi:lo)
 * semantics), handling ranges that span two 32-bit dwords.  The QMD is
 * viewed as little-endian dwords, as both the SDK macros and NVK do.
 */
static void qmd_set_bits(uint8_t *qmd, unsigned hi, unsigned lo, uint64_t value)
{
    unsigned width = hi - lo + 1;
    assert(width <= 32);

    uint64_t mask = (width == 64) ? ~0ULL : ((1ULL << width) - 1ULL);
    value &= mask;

    uint32_t *qw = (uint32_t *)qmd;

    unsigned dw_lo  = lo / 32;
    unsigned bit_lo = lo % 32;
    unsigned bit_hi = hi - dw_lo * 32;        /* relative to dw_lo */

    if (bit_hi < 32) {
        uint32_t dw_mask = (uint32_t)(mask << bit_lo);
        qw[dw_lo] = (qw[dw_lo] & ~dw_mask) | (uint32_t)((value & mask) << bit_lo);
    } else {
        unsigned lo_bits = 32 - bit_lo;
        uint32_t lo_mask = (uint32_t)(((1ULL << lo_bits) - 1ULL) << bit_lo);
        uint32_t hi_mask = (uint32_t)((1ULL << (width - lo_bits)) - 1ULL);

        qw[dw_lo] = (qw[dw_lo] & ~lo_mask)
                    | (uint32_t)((value & ((1ULL << lo_bits) - 1ULL)) << bit_lo);
        qw[dw_lo + 1] = (qw[dw_lo + 1] & ~hi_mask)
                    | (uint32_t)((value >> lo_bits) & hi_mask);
    }
}

/* ── QMD V03 field bit positions (from clc6c0qmd.h, NVC6C0_QMDV03_00_*) ─
 * Every range below is quoted verbatim from that header. */
/* SM_GLOBAL_CACHING_ENABLE              MW(134:134)                     */
#define SM_GLOBAL_CACHING_BIT     134u
/* API_VISIBLE_CALL_LIMIT                MW(378:378)                     */
#define API_VIS_CALL_LIMIT_BIT    378u
#define API_VIS_CALL_LIMIT_NO_CHECK 1u
/* SAMPLER_INDEX                         MW(382:382)                     */
#define SAMPLER_INDEX_BIT         382u
#define SAMPLER_INDEX_INDEPENDENTLY 0u
/* CTA_RASTER_WIDTH                      MW(415:384)                     */
#define GRID_W_HI                 415u
#define GRID_W_LO                 384u
/* CTA_RASTER_HEIGHT                     MW(431:416)                     */
#define GRID_H_HI                 431u
#define GRID_H_LO                 416u
/* CTA_RASTER_DEPTH                      MW(463:448)                     */
#define GRID_D_HI                 463u
#define GRID_D_LO                 448u
/* SHARED_MEMORY_SIZE                    MW(561:544)                     */
#define SMEM_SIZE_HI              561u
#define SMEM_SIZE_LO              544u
/* QMD_VERSION (the minor version)       MW(579:576)                     */
#define QMD_MINOR_VERSION_HI      579u
#define QMD_MINOR_VERSION_LO      576u
/* QMD_MAJOR_VERSION                     MW(583:580)                     */
#define QMD_MAJOR_VERSION_HI      583u
#define QMD_MAJOR_VERSION_LO      580u
/* CTA_THREAD_DIMENSION0/1/2             MW(607:592)/(623:608)/(639:624)  */
#define CTA_DIM0_HI               607u
#define CTA_DIM0_LO               592u
#define CTA_DIM1_HI               623u
#define CTA_DIM1_LO               608u
#define CTA_DIM2_HI               639u
#define CTA_DIM2_LO               624u
/* CONSTANT_BUFFER_VALID(i)              MW((640+i*1):(640+i*1))          */
#define CB_VALID_BIT(i)           (640u + (i) * 1u)
/* REGISTER_COUNT_V                      MW(656:648)                     */
#define REGISTER_COUNT_HI         656u
#define REGISTER_COUNT_LO         648u
/* SHADER_LOCAL_MEMORY_LOW_SIZE          MW(759:736)                     */
#define SLM_LOW_SIZE_HI           759u
#define SLM_LOW_SIZE_LO           736u
/* BARRIER_COUNT                         MW(767:763)                     */
#define BARRIER_COUNT_HI          767u
#define BARRIER_COUNT_LO          763u
/* CONSTANT_BUFFER_ADDR_LOWER(i)         MW((1055+i*64):(1024+i*64))      */
#define CB_ADDR_LO_HI(i)          (1055u + (i) * 64u)
#define CB_ADDR_LO_LO(i)          (1024u + (i) * 64u)
/* CONSTANT_BUFFER_ADDR_UPPER(i)         MW((1072+i*64):(1056+i*64))      */
#define CB_ADDR_HI_HI(i)          (1072u + (i) * 64u)
#define CB_ADDR_HI_LO(i)          (1056u + (i) * 64u)
/* CONSTANT_BUFFER_SIZE_SHIFTED4(i)      MW((1087+i*64):(1075+i*64))      */
#define CB_SIZE_HI(i)             (1087u + (i) * 64u)
#define CB_SIZE_LO(i)             (1075u + (i) * 64u)
/* PROGRAM_ADDRESS_LOWER                 MW(1567:1536)                   */
#define PROG_ADDR_LO_HI           1567u
#define PROG_ADDR_LO_LO           1536u
/* PROGRAM_ADDRESS_UPPER                 MW(1584:1568)                   */
#define PROG_ADDR_HI_HI           1584u
#define PROG_ADDR_HI_LO           1568u
/* SHADER_LOCAL_MEMORY_HIGH_SIZE         MW(1623:1600)                   */
#define SLM_HIGH_SIZE_HI          1623u
#define SLM_HIGH_SIZE_LO          1600u

/* ── Field setters ─────────────────────────────────────────────────── */

static void mc_qmd_init(uint8_t *qmd)
{
    memset(qmd, 0, MC_QMD_V03_BYTES);

    /* NVK qmd_init!: major, minor, then two enum defaults. */
    qmd_set_bits(qmd, QMD_MAJOR_VERSION_HI, QMD_MAJOR_VERSION_LO, 3);
    qmd_set_bits(qmd, QMD_MINOR_VERSION_HI, QMD_MINOR_VERSION_LO, 0);
    qmd_set_bits(qmd, API_VIS_CALL_LIMIT_BIT, API_VIS_CALL_LIMIT_BIT,
                 API_VIS_CALL_LIMIT_NO_CHECK);
    qmd_set_bits(qmd, SAMPLER_INDEX_BIT, SAMPLER_INDEX_BIT,
                 SAMPLER_INDEX_INDEPENDENTLY);

    /* Qmd3_0::new sets this on top of qmd_init!; Qmd4_0::new does not.
     * Without it the SM may service the kernel's global accesses from a
     * non-coherent path, which for a kernel whose entire purpose is a
     * system-scope store is exactly the wrong behaviour. */
    qmd_set_bits(qmd, SM_GLOBAL_CACHING_BIT, SM_GLOBAL_CACHING_BIT, 1);

    /* No QMD_TYPE here: V03 has no such field.  See the file comment. */
}

static void mc_qmd_set_barrier_count(uint8_t *qmd, uint8_t n)
{
    qmd_set_bits(qmd, BARRIER_COUNT_HI, BARRIER_COUNT_LO, n);
}

static void mc_qmd_set_global_size(uint8_t *qmd, uint32_t w, uint32_t h, uint32_t d)
{
    qmd_set_bits(qmd, GRID_W_HI, GRID_W_LO, w);
    qmd_set_bits(qmd, GRID_H_HI, GRID_H_LO, h);
    qmd_set_bits(qmd, GRID_D_HI, GRID_D_LO, d);
}

static void mc_qmd_set_local_size(uint8_t *qmd, uint16_t w, uint16_t h, uint16_t d)
{
    qmd_set_bits(qmd, CTA_DIM0_HI, CTA_DIM0_LO, w);
    qmd_set_bits(qmd, CTA_DIM1_HI, CTA_DIM1_LO, h);
    qmd_set_bits(qmd, CTA_DIM2_HI, CTA_DIM2_LO, d);
}

static void mc_qmd_set_prog_addr(uint8_t *qmd, uint64_t prog_gpu_va)
{
    /* qmd_impl_set_prog_addr_64!(clc6c0, QMDV03_00, NONE): no shift.
     * UPPER is 17 bits here (MW(1584:1568)) against V04's 25, so a program
     * VA must fit in 49 bits.  Every VA libmc hands out is far below that;
     * assert rather than silently truncate if that ever changes. */
    assert((prog_gpu_va >> 49) == 0 && "program VA exceeds QMD V03's 49-bit field");
    qmd_set_bits(qmd, PROG_ADDR_LO_HI, PROG_ADDR_LO_LO, (uint32_t)prog_gpu_va);
    qmd_set_bits(qmd, PROG_ADDR_HI_HI, PROG_ADDR_HI_LO, prog_gpu_va >> 32);
}

static void mc_qmd_set_register_count(uint8_t *qmd, uint8_t n)
{
    qmd_set_bits(qmd, REGISTER_COUNT_HI, REGISTER_COUNT_LO, n);
}

static void mc_qmd_set_slm_size(uint8_t *qmd, uint32_t bytes)
{
    /* qmd_impl_set_slm_size! with size_suffix=NONE: round up to 16, write
     * LOW_SIZE = bytes and HIGH_SIZE = 0. */
    uint32_t rounded = (bytes + 0xFu) & ~0xFu;
    qmd_set_bits(qmd, SLM_HIGH_SIZE_HI, SLM_HIGH_SIZE_LO, 0);
    qmd_set_bits(qmd, SLM_LOW_SIZE_HI, SLM_LOW_SIZE_LO, rounded);
}

static void mc_qmd_set_smem_size(uint8_t *qmd, uint32_t bytes)
{
    /* qmd_impl_set_smem_size_bounded! rounds up to 0x100, writes
     * SHARED_MEMORY_SIZE, then MIN/MAX/TARGET_SM_CONFIG_SHARED_MEM_SIZE from
     * gv100_get_hw_smem_sizes.  Both embedded kernels use no shared memory,
     * so the size is 0 and the three SM_CONFIG fields are left at 0 -- the
     * same simplification qmd_v04.c makes, with the same precondition
     * asserted rather than assumed. */
    uint32_t rounded = (bytes + 0xFFu) & ~0xFFu;
    assert(rounded == 0 && "non-zero SMEM not implemented; port gv100_get_hw_smem_sizes if needed");
    qmd_set_bits(qmd, SMEM_SIZE_HI, SMEM_SIZE_LO, rounded);
}

static void mc_qmd_set_cbuf(uint8_t *qmd, uint8_t idx, uint64_t addr, uint32_t size)
{
    /* qmd_impl_set_cbuf!(clc6c0, QMDV03_00, NONE, SHIFTED4): the address is
     * written UNSHIFTED -- unlike V04, which stores it >>6 -- and only the
     * size is >>4.  ADDR_UPPER is 17 bits, so the address must fit in 49. */
    if (idx >= 8) return;
    assert((size & 0x0F) == 0);
    assert((addr >> 49) == 0 && "cbuf VA exceeds QMD V03's 49-bit field");

    uint32_t size_s4 = size >> 4;

    qmd_set_bits(qmd, CB_ADDR_LO_HI(idx), CB_ADDR_LO_LO(idx), (uint32_t)addr);
    qmd_set_bits(qmd, CB_ADDR_HI_HI(idx), CB_ADDR_HI_LO(idx), addr >> 32);
    qmd_set_bits(qmd, CB_SIZE_HI(idx),    CB_SIZE_LO(idx),    size_s4);
    qmd_set_bits(qmd, CB_VALID_BIT(idx),  CB_VALID_BIT(idx),  1);
}

/* ── The operation the library actually needs ─────────────────────────────
 *
 * Same shape as Hopper's: one QMD, a single CTA of a single thread, running
 * `prog_gpu_va` with constant buffer 0 bound at `cb0_gpu_va`. */
static void ampere_build_qmd(uint8_t *qmd,
                             uint64_t prog_gpu_va, uint32_t register_count,
                             uint64_t cb0_gpu_va,  uint32_t cb0_bytes)
{
    mc_qmd_init(qmd);
    mc_qmd_set_global_size   (qmd, 1, 1, 1);
    mc_qmd_set_local_size    (qmd, 1, 1, 1);
    mc_qmd_set_prog_addr     (qmd, prog_gpu_va);
    mc_qmd_set_register_count(qmd, (uint8_t)register_count);
    mc_qmd_set_barrier_count (qmd, 0);
    mc_qmd_set_slm_size      (qmd, 0);
    mc_qmd_set_smem_size     (qmd, 0);
    mc_qmd_set_cbuf          (qmd, 0, cb0_gpu_va, cb0_bytes);
}

const mc_compute_ops_t mc_arch_ampere_compute = {
    .qmd_bytes     = MC_QMD_V03_BYTES,
    .qmd_align     = 256,
    .cbuf_align    = 0x100,
    .version_major = 3,
    .version_minor = 0,
    /* From `cuobjdump --dump-sass` of the sm_80 cubin: MOV R2, c[0x0][0x160]
     * / MOV R3, c[0x0][0x164] for dst and IMAD.MOV.U32 R5, ..., c[0x0][0x168]
     * for the token.  CONSTANT[0] is 364 bytes.  These are NOT Hopper's
     * offsets -- nvcc moved the kernel-parameter base between SM versions. */
    .cb0_bytes      = 0x16Cu,
    .cb0_off_dst_lo = 0x160u,
    .cb0_off_dst_hi = 0x164u,
    .cb0_off_token  = 0x168u,
    .build_qmd     = ampere_build_qmd,
};
