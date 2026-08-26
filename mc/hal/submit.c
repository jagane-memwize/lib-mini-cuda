/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * hal/submit.c — method stream + GPFIFO entry builders + channel
 * submission primitives.
 *
 * Pure-ish: no fds opened, no handles allocated.  Inputs are
 * already-allocated mc_channel_t fields + the BAR1 doorbell pointer;
 * outputs are written-to GPFIFO/USERD/PB memory.  drain_channel is
 * here too because it's also a "talk to GPU through USERD" primitive
 * (just polling rather than producing).
 */

#define _GNU_SOURCE
#include <emmintrin.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "nvtypes.h"
#include "nvmisc.h"
#include "class/clc8b5.h"        /* HOPPER_DMA_COPY_A + methods */
#include "class/clc86f.h"        /* HOPPER_CHANNEL_GPFIFO_A + GP entry */
#include "class/clc36f.h"        /* NVC36F_DMA_INCR_* method headers */
#include "class/cla06fsubch.h"   /* NVA06F_SUBCHANNEL_COPY_ENGINE */

#include "internal/types.h"
#include "internal/log.h"
#include "hal/methods.h"
#include "internal/submit.h"

/* ── USERD access ─────────────────────────────────────────────────────────
 *
 * USERD's layout belongs to the channel class, not to the library, so this
 * file is the only place that names the class struct.  mc_channel_t carries
 * an untyped `void *userd_cpu` and internal/types.h needs no class header.
 *
 * Layout across the generations that matter here:
 *   AMPERE_CHANNEL_GPFIFO_A    Nvc56fControl   GPGet @0x88, GPPut @0x8c
 *   HOPPER_CHANNEL_GPFIFO_A    Nvc86fControl   GPGet @0x88, GPPut @0x8c
 *   BLACKWELL_CHANNEL_GPFIFO_A Nvc96fControl   GPPut @0x8c, NO GPGet
 *
 * Blackwell dropping GPGet is why the getter is a try_ form: a caller that
 * assumes a readable GPGet has no correct behaviour on that class.
 */
#define USERD(p) ((volatile HopperAControlGPFifo *)(p))

void mc_userd_set_gpput(void *userd, uint32_t value)
{
  USERD(userd)->GPPut = value;
}

uint32_t mc_userd_get_gpput(const void *userd)
{
  return USERD(userd)->GPPut;
}

bool mc_userd_try_get_gpget(const void *userd, uint32_t *out_gpget)
{
  if (userd == NULL || out_gpget == NULL)
    return false;
  *out_gpget = USERD(userd)->GPGet;
  return true;
}


/*
 * Submit work to the GPU.  TWO writes are required on Hopper, in order:
 *
 *   1. Advance GPPut in USERD.  PBDMA reads GPPut (not continuously —
 *      only when the channel is scheduled onto it) to learn how many
 *      GPFIFO entries are pending.  Writing GPPut makes the new entries
 *      officially "submitted" but does not wake up the GPU on its own.
 *
 *   2. Write the channel's work-submit-token to the VF doorbell
 *      (NV_VIRTUAL_FUNCTION_DOORBELL = 0x30090 *relative to the VF
 *      register block*, not to BAR0 — on bare metal the same register
 *      is at BAR0 + 0xBB0090; reached here at +0x90 inside the
 *      USERMODE mapping — see MC_VF_DOORBELL_OFFSET).  This is the
 *      actual wake-up signal: the host scheduler looks up the channel
 *      by token, verifies it's on the runlist, and forwards a
 *      notification to PBDMA.
 *
 * Writing only (1) hangs — GPPut advances but PBDMA is asleep and never
 * wakes.  Writing only (2) dispatches PBDMA to read GPPut and find no
 * new work, wasting a cycle.  Both writes are mandatory, in this order.
 *
 * On x86 both are Write-Combine stores; the sfences flush WC buffers so
 * the GPU observes the writes in causal order.
 */
void ring_doorbell(void *userd,
                          uint32_t new_gp_put, volatile uint32_t *vf_doorbell,
                          uint32_t work_submit_token)
{
  USERD(userd)->GPPut = new_gp_put;
  _mm_sfence();
  *vf_doorbell = work_submit_token;
  _mm_sfence();
}

/* Poll a release-sema cell until *cell == expected, with the standard
 * MC_TIMEOUT_MS budget.  The cell may live in sysmem (host-rung paths,
 * sysmem-carrier paths, FB-carrier sysmem-dst path) or in HBM via a
 * BAR1-aliased CPU mapping (FB-carrier HBM-dst path).  The caller picks
 * which cell + expected payload to poll. */
mc_status_t mc_channel_poll_sema(volatile uint32_t *cell, uint32_t expected,
                                        struct timespec t0)
{
  struct timespec now;
  long elapsed_ms;
  while (*cell != expected)
  {
    volatile int s;
    clock_gettime(CLOCK_MONOTONIC, &now);
    elapsed_ms = (now.tv_sec - t0.tv_sec) * 1000
                 + (now.tv_nsec - t0.tv_nsec) / 1000000;
    if (elapsed_ms > MC_TIMEOUT_MS) return MC_ETIMEOUT;
    for (s = 0; s < MC_POLL_SPIN_ITERATIONS
                && *cell != expected; s++)
      ;
  }
  return MC_OK;
}

mc_status_t mc_channel_arm(mc_channel_t *ch,
                                  uint32_t copy_bytes,
                                  struct timespec *t0_out)
{
  uint32_t entries_used;

  /* Producer-side fence: bytes the caller wrote into pb_cpu must
   * reach DRAM before PBDMA snoops the GPFIFO entry pointing at
   * pb_gpu_va.  See produce -> fence -> publish contract. */
  _mm_sfence();

  /* Clear the release semaphore so the upcoming poll observes a
   * fresh transition, not a stale prior payload. */
  *ch->sema_ptr = 0;
  _mm_sfence();

  if (t0_out) clock_gettime(CLOCK_MONOTONIC, t0_out);

  /* Publish: write the GPFIFO entry pointing at pb_gpu_va. */
  entries_used = write_gp_entry(ch->arch, ch->gpfifo_ring, ch->gp_put,
                                MC_GPFIFO_ENTRIES, ch->pb_gpu_va, 0,
                                copy_bytes);
  /* PBDMA reads USERD GPPut as a ring INDEX (Hopper NVC86F GPPut is
   * the offset into the GPFIFO ring, not a monotonic counter), so
   * the mirror must wrap modulo MC_GPFIFO_ENTRIES every advance.
   * UVM does the same in uvm_channel.c (cpu_put = (cpu_put + 1) %
   * num_gpfifo_entries). */
  ch->gp_put = (ch->gp_put + entries_used) % MC_GPFIFO_ENTRIES;
  _mm_sfence();

  /* Advance USERD GPPut so PBDMA sees the new entry on its next
   * wake event.  ring_doorbell() inside mc_channel_submit() does
   * the same store + sfence + the BAR1 doorbell write; mc_channel_arm
   * stops here and lets a different agent ring. */
  mc_userd_set_gpput(ch->userd_cpu, ch->gp_put);
  _mm_sfence();

  return MC_OK;
}

mc_status_t mc_channel_submit(mc_channel_t *ch,
                                     volatile uint32_t *vf_doorbell,
                                     uint32_t copy_bytes)
{
  uint32_t entries_used;
  struct timespec t0;
  mc_status_t rc;

  /* Producer-side fence (same contract as mc_channel_arm). */
  _mm_sfence();

  *ch->sema_ptr = 0;
  _mm_sfence();

  clock_gettime(CLOCK_MONOTONIC, &t0);

  entries_used = write_gp_entry(ch->arch, ch->gpfifo_ring, ch->gp_put,
                                MC_GPFIFO_ENTRIES, ch->pb_gpu_va, 0,
                                copy_bytes);
  /* See mc_channel_arm: GPPut wraps modulo MC_GPFIFO_ENTRIES. */
  ch->gp_put = (ch->gp_put + entries_used) % MC_GPFIFO_ENTRIES;
  _mm_sfence();

  /* ring_doorbell does USERD GPPut + sfence + BAR1 doorbell + sfence
   * internally — the two writes that submit work on Hopper. */
  ring_doorbell(ch->userd_cpu, ch->gp_put, vf_doorbell, ch->work_submit_token);

  rc = mc_channel_poll_sema(ch->sema_ptr, ch->sema_payload, t0);
  if (rc != MC_OK)
  {
    uint32_t userd_gp_put = ch->userd_cpu ? mc_userd_get_gpput(ch->userd_cpu)
                                          : 0xffffffffu;
    uint32_t userd_gp_get = 0xffffffffu;
    if (ch->userd_cpu)
      (void)mc_userd_try_get_gpget(ch->userd_cpu, &userd_gp_get);
    uint32_t sema_actual  = ch->sema_ptr ? *ch->sema_ptr : 0xffffffffu;
    WARN_LOG("mc_channel_submit: poll failed rc=%d role=%d type=%d vas=%d "
             "h_channel=0x%x gp_put_mirror=0x%x USERD{GPPut=0x%x GPGet=0x%x} "
             "sema_ptr=%p sema_gpu_va=0x%llx sema_actual=0x%x "
             "sema_expected=0x%x pb_cpu=%p pb_gpu_va=0x%llx copy_bytes=%u "
             "entries_used=%u work_token=0x%x",
             rc, (int)ch->role, (int)ch->type, (int)ch->vas_id,
             ch->h_channel, ch->gp_put, userd_gp_put, userd_gp_get,
             (const void *)ch->sema_ptr,
             (unsigned long long)ch->sema_gpu_va, sema_actual,
             ch->sema_payload, (void *)ch->pb_cpu,
             (unsigned long long)ch->pb_gpu_va, copy_bytes, entries_used,
             ch->work_submit_token);
  }
  return rc;
}

/*
 * drain_channel — poll USERD until GPPut == GPGet (all submitted work
 * consumed) or timeout_ms elapses.  Called from mc_fini so the channel
 * is idle before resources are freed.  Teardown-tolerant: WARN and
 * return on timeout; the subsequent rm_channel_disable / channel free
 * will force-preempt anything still in flight.
 *
 * In practice the last mc_memcpy already polled the semaphore to
 * completion, so GPPut should already equal GPGet at entry — this is
 * insurance, not a hot path.
 */
void drain_channel(void *userd, long timeout_ms)
{
  struct timespec t0, now;
  long            elapsed_ms = 0;
  uint32_t        gpget;

  /* A channel class that does not publish GPGet gives us nothing to poll.
   * Draining is insurance rather than a hot path -- the last mc_memcpy
   * already polled its semaphore to completion -- so skip rather than
   * inventing a value. */
  if (!mc_userd_try_get_gpget(userd, &gpget))
    return;

  clock_gettime(CLOCK_MONOTONIC, &t0);
  while (USERD(userd)->GPPut != USERD(userd)->GPGet)
  {
    volatile int s;
    clock_gettime(CLOCK_MONOTONIC, &now);
    elapsed_ms = (now.tv_sec - t0.tv_sec) * 1000
                 + (now.tv_nsec - t0.tv_nsec) / 1000000;
    if (elapsed_ms > timeout_ms)
    {
      WARN_LOG("drain_channel: timeout after %ld ms (GPPut=0x%x GPGet=0x%x)",
               elapsed_ms, USERD(userd)->GPPut, USERD(userd)->GPGet);
      return;
    }
    for (s = 0; s < MC_POLL_SPIN_ITERATIONS
                && USERD(userd)->GPPut != USERD(userd)->GPGet; s++)
      ;
  }
}
