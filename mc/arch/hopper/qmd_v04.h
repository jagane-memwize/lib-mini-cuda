/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * arch/hopper/qmd_v04.h — Hopper QMD V04 builder for mc's
 * compute-doorbell-write kernel.
 *
 * Built from zero, NVK-style.  See Mesa NVK
 * `src/nouveau/compiler/nak/qmd.rs` (Qmd4_0 + fill_qmd) for the
 * ground-truth recipe.  The previous implementation captured a
 * libcuda QMD verbatim and patched it; that approach left
 * libcuda-process-specific pointer fragments in the QMD that SKED
 * dereferenced as VA 0 (Xid 31).  This rewrite starts from a
 * zero-initialised 384-byte buffer and only sets the fields NVK
 * proves are required.
 *
 * Field positions come from Mesa NVK's `clcbc0qmd.h`; this tree ships
 * only the Kepler `cla0c0qmd.h`, so the NVCBC0_QMDV04_00_* macro names
 * are kept purely so the two can be read side by side.
 *
 * Usage (single-thread (1,1,1) grid running our doorbell-write SASS):
 *
 *   uint8_t *qmd  = vidmem_alloc(MC_QMD_BYTES, 0x100);
 *   uint8_t *cb0  = vidmem_alloc(mc_cb0_alloc_bytes(compute), 0x100);
 *   uint8_t *sass = vidmem_alloc(roundup(MC_STORE_DWORD_SASS_LEN, 256), 0x100);
 *   memcpy(sass, mc_store_dword_kernel_sass, MC_STORE_DWORD_SASS_LEN);
 *
 *   mc_qmd_init(qmd);
 *   mc_qmd_set_global_size(qmd, 1, 1, 1);
 *   mc_qmd_set_local_size (qmd, 1, 1, 1);
 *   mc_qmd_set_prog_addr  (qmd, sass_gpu_va);
 *   mc_qmd_set_register_count(qmd, 8);
 *   mc_qmd_set_barrier_count (qmd, 0);
 *   mc_qmd_set_slm_size      (qmd, 0);
 *   mc_qmd_set_smem_size     (qmd, 0);
 *   mc_qmd_set_cbuf(qmd, 0, cb0_gpu_va, mc_cb0_alloc_bytes(compute));
 *
 *   mc_cb0_init(cb0);
 *   mc_cb0_set_args(cb0, dst_gpu_va, token);
 *
 *   // Then issue the launch pushbuffer: SET_OBJECT(HOPPER_COMPUTE_A) +
 *   // a few cache-invalidate / window methods (one-time per channel) +
 *   // SEND_PCAS_A(qmd_gpu_va >> 8) + SEND_SIGNALING_PCAS2_B(0xa).
 */
#ifndef MC_COMPUTE_QMD_H
#define MC_COMPUTE_QMD_H

#include <stdint.h>

/* Hopper QMD V04 fixed size (96 dwords). */
#define MC_QMD_BYTES                 384u

/* CB0 carries kernel arguments at SASS-defined offsets:
 *   c[0x0][0x208] = global memdesc (must read 0)
 *   c[0x0][0x210] = dst_gpu_va low 32 bits
 *   c[0x0][0x214] = dst_gpu_va high 32 bits
 *   c[0x0][0x218] = token
 * The SM front-end fetches the whole CB0 once into local memory,
 * so any bytes past 0x21c are unread but the buffer must be at
 * least 0x21c bytes.  We round up to 0x100 = 256 (NVK's
 * min_cbuf_alignment for Hopper is 0x100 on this class). */
/* The QMD field setters are private to qmd_v04.c: they are how a V04 QMD
 * is assembled, not what a caller asks for.  Callers go through the
 * architecture's compute ops (hal/arch.h), whose build_qmd covers the one
 * launch shape libmc uses.  mc_arch_hopper_compute is that table. */

/* CB0 is built by the shared hal/cbuf.c (declared in internal/compute.h),
 * parameterised by the cb0_* offsets in mc_arch_hopper_compute.  Those
 * offsets are sm_90's -- 0x210/0x214/0x218 for the arguments, with the
 * global memdesc at 0x208 which must stay zero for raw-VA mode -- and they do
 * NOT carry to other SM versions.  See hal/arch.h. */

/* The mc_store_dword_kernel SASS now lives in the generated
 * arch/hopper/store_dword_sass.h, built from kernels/store_dword.cu.
 * Include that header directly where the bytes are needed. */

#endif /* MC_COMPUTE_QMD_H */
