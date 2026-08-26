/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * arch/ampere/qmd_v03.h — Ampere QMD V03 builder.
 *
 * Built NVK-style from Mesa's `Qmd3_0` (src/nouveau/compiler/nak/qmd.rs), the
 * same reference the Hopper V04 builder follows.  Field positions come from
 * NVIDIA's clc6c0qmd.h, which this tree does not ship -- only the Kepler
 * cla0c0qmd.h is in the open kernel modules -- so the NVC6C0_QMDV03_00_*
 * macro names are carried in comments purely so the two can be read together.
 *
 * V03 also serves Ada: AD10x brings ADA_COMPUTE_A but the QMD layout is the
 * Ampere one, so both backends point at mc_arch_ampere_compute.
 *
 * The QMD field setters stay private to qmd_v03.c -- they are how a V03 QMD
 * is assembled, not what a caller asks for.  Callers go through the
 * architecture's compute ops (hal/arch.h), whose build_qmd covers the single
 * launch shape libmc uses.  mc_arch_ampere_compute is that table, and it also
 * carries the sm_80 constant-bank offsets, which differ from Hopper's.
 */
#ifndef MC_ARCH_AMPERE_QMD_V03_H
#define MC_ARCH_AMPERE_QMD_V03_H

#include <stdint.h>

/* Ampere QMD V03 fixed size: NVK's Qmd3_0 is [u32; 64].  Hopper's V04 is
 * [u32; 96] / 384 bytes -- the versions are different sizes, not the same
 * structure with fields moved. */
#define MC_QMD_V03_BYTES             256u

#endif /* MC_ARCH_AMPERE_QMD_V03_H */
