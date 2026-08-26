/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * hal/methods.h — the GPU method-stream emitters.
 *
 * Everything here is pure encoding: given a pushbuffer cursor and some
 * addresses, produce method words.  Nothing opens a file descriptor, touches
 * a channel, or fences.  The submission mechanics that consume these streams
 * (produce -> fence -> publish, USERD, doorbell) live in hal/submit.c.
 *
 * These are the ONLY functions in libmc that emit GPU method words.  Keeping
 * that true is what makes the architecture work tractable: when a class id or
 * a method encoding has to vary per architecture, there is exactly one place
 * to thread it through.  hal/method_abi.h carries the compile-time proof that
 * the encodings really are shared across the classes we target.
 */
#ifndef MC_HAL_METHODS_H
#define MC_HAL_METHODS_H

#include <stdbool.h>
#include <stdint.h>

#include "hal/arch.h"

/* Build the copy-engine method stream for one CE copy.  Returns the
 * pushbuffer-end pointer (caller computes byte length as
 * (end - pb) * sizeof(uint32_t)). */
uint32_t *mc_write_transfer_methods(const mc_arch_desc_t *arch, uint32_t *pb,
                                    uint64_t src_va, uint64_t dst_va,
                                    uint32_t nbytes,
                                    uint64_t sema_va, uint32_t sema_payload);

/* Write one GPFIFO entry describing a pushbuffer segment.  Returns the number
 * of ring entries consumed (more than one when an extended-base opcode is
 * needed for a pushbuffer above 2^40). */
uint32_t write_gp_entry(const mc_arch_desc_t *arch,
                        volatile uint32_t *gpfifo_ring,
                        uint32_t gp_put_index, uint32_t ring_entries,
                        uintptr_t pb_cpu_va, uint32_t pb_offset,
                        uint32_t method_bytes);

/* Per-channel one-time compute setup: bind the compute class to its
 * subchannel and program the shader memory windows.  Callers gate this on
 * mc_compute_extras.setup_done — the binding is sticky for the channel. */
uint32_t *mc_compute_write_setup(const mc_arch_desc_t *arch, uint32_t *pb);

/* Launch one QMD and release a report semaphore when it retires.
 *
 * `invalidate_caches` prepends INVALIDATE_SHADER_CACHES_NO_WFI(CONSTANT,
 * GLOBAL_DATA).  Callers that reuse one CB0 GPU VA and patch its contents
 * between launches MUST pass true: the SM front-end's constant cache is keyed
 * by GPU VA, so without it the second launch reads the first launch's
 * arguments.  The observed symptom is iteration 0 succeeding and iteration 1
 * hanging, because the kernel re-reads a stale gp_put and its USERD store
 * becomes a no-op that never wakes PBDMA.
 *
 * Callers whose arguments are identical on every launch may pass false. */
uint32_t *mc_compute_write_launch(const mc_arch_desc_t *arch, uint32_t *pb,
                                  uint64_t qmd_gpu_va,
                                  uint64_t sema_va,
                                  uint32_t sema_payload,
                                  bool     invalidate_caches);

#endif /* MC_HAL_METHODS_H */
