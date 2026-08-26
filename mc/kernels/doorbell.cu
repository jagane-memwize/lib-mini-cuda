/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * kernels/doorbell.cu — the minimal "SM writes one dword" kernel.
 *
 * One thread performs one system-scope store.  That is the whole kernel, and
 * the point: it is the smallest possible demonstration that an SM thread's
 * store can land on a BAR1 doorbell register and wake a channel's PBDMA.
 * mc_compute_doorbell_kernel and mc_memcpy_gpu_doorbell_sm both launch it.
 *
 * `volatile` is load-bearing, not decoration.  It forces the store to survive
 * to a real STG rather than being folded away, and it lowers to
 * STG.E.STRONG.SYS -- a system-scope strong-ordered store.  A weaker scope
 * would leave the write sitting in the SM's L1 where the host interface never
 * sees it, so the doorbell would never be rung and PBDMA would never wake.
 *
 * Build (matching the Makefile rule):
 *   nvcc -arch=sm_90 --cubin -O0
 *
 * -O0 rather than the -O2 used for sm_owner.cu: this kernel has nothing to
 * fold, and -O0 keeps the generated SASS a literal transcription of the
 * source, which is what makes the committed bytes reviewable.
 *
 * Expected SASS:
 *   LDC R1, c[0x0][0x28]
 *   LDC R5, c[0x0][0x218]              ; token
 *   ULDC.64 UR4, c[0x0][0x208]         ; global memdesc
 *   LDC.64 R2, c[0x0][0x210]           ; dst
 *   STG.E.STRONG.SYS desc[UR4][R2.64], R5
 *   EXIT
 *   BRA self
 *   (NOP padding)
 *
 * Resources: REG=8, STACK=0, SHARED=0, LOCAL=0.
 *
 * This file was reconstructed from that documented disassembly: the embedded
 * bytes predate it and had no source in the tree, so they could neither be
 * regenerated nor ported to another architecture.  The build asserts the
 * reconstruction reproduces them byte-for-byte.
 */

extern "C" __global__
void mc_doorbell_kernel(volatile unsigned int *dst, unsigned int token)
{
    *dst = token;
}
