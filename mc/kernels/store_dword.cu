/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * kernels/store_dword.cu — the minimal "SM writes one dword" kernel.
 *
 * One thread performs one system-scope store to whatever address it is given.
 * That is the whole kernel, and the generality is the point: the destination
 * is a parameter, so the same kernel serves two quite different purposes.
 *
 *   - Pointed at a BAR1 doorbell register, it demonstrates that an SM thread's
 *     store can ring a doorbell and wake a channel's PBDMA.  That needs a BAR1
 *     usermode aperture, so it is Hopper-only today.
 *   - Pointed at ordinary mapped memory, it is the minimal payload proving a
 *     kernel launches at all: that the architecture's QMD is well-formed, SKED
 *     accepts it, and the constant-bank argument offsets are right.  That is
 *     what makes MC_FEAT_COMPUTE_LAUNCH meaningful, and it is why Ampere and
 *     Ada carry this cubin despite having no doorbell to ring.
 *
 * It was previously called doorbell.cu, which named only the first use and so
 * implied that an arch/ampere/doorbell_sass.h meant Ampere could ring one --
 * the exact opposite of the truth.
 *
 * `volatile` is load-bearing, not decoration.  It forces the store to survive
 * to a real STG rather than being folded away, and it lowers to
 * STG.E.STRONG.SYS -- a system-scope strong-ordered store.  A weaker scope
 * would leave the write sitting in the SM's L1 where the host interface never
 * sees it: a doorbell would never be rung, and a store to host-visible memory
 * would never be observed by the CPU.
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
 *   LDC R5, c[0x0][0x218]              ; token (sm_90; sm_80/89 use 0x168)
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
void mc_store_dword_kernel(volatile unsigned int *dst, unsigned int token)
{
    *dst = token;
}
