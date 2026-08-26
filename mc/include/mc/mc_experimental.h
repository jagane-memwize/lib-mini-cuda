/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * mc_experimental.h — research entrypoints.
 *
 * These are chain-of-evidence demonstrations for particular paths the library
 * can drive: a copy engine writing a doorbell register, an SM thread writing
 * one, an SM thread authoring an entire channel submission.  They exist to
 * establish that something is possible and to keep it working, not to be
 * built on.
 *
 * Nothing here is covered by the stability the rest of the API implies.
 * Signatures may change, entrypoints may be withdrawn, and a build with
 * MC_ENABLE_EXPERIMENTAL=0 does not contain them at all.  Every one of them
 * also depends on GPU capabilities that are absent on some supported
 * architectures -- Ampere and Ada have no BAR1 usermode aperture, so no
 * doorbell PTE, so none of the doorbell paths -- which is why each names the
 * feature to check with mc_feature_supported() before calling.
 */
#ifndef MC_EXPERIMENTAL_H
#define MC_EXPERIMENTAL_H

/* Checks the VALUE, not just definedness: a build that explicitly sets
 * MC_ENABLE_EXPERIMENTAL=0 has no such entrypoints to declare. */
#if !defined(MC_ENABLE_EXPERIMENTAL) || !MC_ENABLE_EXPERIMENTAL
#  error "mc_experimental.h is not part of libmc's stable API. \
Define MC_ENABLE_EXPERIMENTAL to opt in, and note that these entrypoints \
may change or disappear in any revision."
#endif

#include <mc/mc.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Doorbell-write demo: submit a CE LAUNCH_DMA on the DMA channel
 * (whose non-UVM VAS holds a BAR1 doorbell PTE) that copies 4 bytes
 * from a sysmem cell into the BAR1 doorbell page at +0x90.  The cell
 * is pre-filled with `token`, so the GPU's CE engine — not the host —
 * issues the MMIO write that lands on the doorbell.
 *
 * Verification is the release semaphore, not the kernel watchpoint.  The
 * watchpoint is an x86 hardware breakpoint on the diverted userspace
 * mapping, so it only sees *CPU* writes to the doorbell; a CE-issued MMIO
 * write reaches BAR1 without the CPU touching it and is invisible there by
 * construction.  What proves the write happened is that the CE ran the
 * pushbuffer to completion: this call returns MC_OK only once the
 * CE-emitted release semaphore has fired.
 *
 * Synchronous: returns only after that semaphore fires (or MC_ETIMEOUT
 * after TIMEOUT_MS).
 *
 * `token` is the value the GPU writes to the doorbell.  0xDEADBEEF is
 * a safe choice: it's not a valid Hopper work-submit-token, so PBDMA
 * looks up its (chid, runlist) decoding, finds nothing, and silently
 * drops.  Note the kernel watchpoint does NOT see this write: the store
 * is issued by the GPU's Copy Engine, and an x86 debug register traps
 * only CPU accesses.  What proves the write happened is the demo's own
 * release semaphore. */
MC_API mc_status_t mc_dbell_demo_ring(mc_ctx_t *ctx, uint32_t token);

/* End-to-end-verifiable copy whose UVM-channel doorbell ring is
 * issued by the GPU itself, not the host.  Direction is inferred from
 * the alloc table: pass any matched (host, device) pair returned by
 * mc_malloc_* on this context.  Both pointers MUST live in
 * MC_VAS_UVM (the primary channel is the UVM CE channel) — otherwise
 * MC_EINVAL.
 *
 * Sequence (D2H phrasing; H2D swaps src/dst at the LAUNCH_DMA level
 * but the rest is identical):
 *   1. Arm the UVM channel: build a CE pushbuffer at its pb_cpu
 *      (src→dst via OFFSET_IN/OFFSET_OUT), write the GPFIFO entry,
 *      advance USERD GPPut.  DO NOT ring the UVM channel's
 *      vf_doorbell.
 *   2. Build a tiny pushbuffer on the DMA channel that copies the
 *      UVM channel's work_submit_token (pre-filled into a sysmem
 *      token cell mapped into the DMA channel's VAS) to
 *      dbell_gpu_va + 0x90 — i.e. the BAR1 USERMODE_A doorbell as
 *      seen by the DMA channel's GPU MMU.
 *   3. Ring the DMA channel's doorbell from the host.  PBDMA wakes
 *      the DMA channel, its CE writes 4 bytes to BAR1 doorbell,
 *      which (on real hardware) wakes the UVM channel's PBDMA,
 *      which runs the queued copy, which fires the UVM channel's
 *      release semaphore.
 *   4. Poll the DMA channel's sema (CE op finished) then the UVM
 *      channel's sema (bytes flushed through PCIe per
 *      LAUNCH_DMA_FLUSH_ENABLE=TRUE).
 *
 * Verification (D2H): the host buffer contains the bytes from the
 * device buffer iff the UVM channel's doorbell got rung.  The host
 * did not ring it.  Only possible source of the ring is the DMA
 * channel's CE-emitted MMIO write reaching real BAR1.  Comparing
 * the host buffer vs. expected bytes is therefore direct proof —
 * outside libcuda, outside the watchpoint shadow path, just CPU
 * loads from sysmem.
 *
 * Verification (H2D): the device buffer contains the bytes from the
 * host buffer iff the UVM channel's doorbell got rung.  The CPU
 * cannot read the device buffer directly, so the natural follow-up
 * is an *untimed* mc_memcpy(MC_XFER_HOST) read-back into a scratch
 * host buffer; the same chain-of-evidence argument then applies to
 * those readback bytes.
 *
 * Returns MC_OK on full chain success, MC_ETIMEOUT if either sema
 * doesn't fire within TIMEOUT_MS, MC_EINVAL on invalid inputs (NULL,
 * unknown pointers, mismatched VAS, non-UVM VAS, undersized buffer). */
MC_API mc_status_t mc_memcpy_gpu_doorbell_ce(mc_ctx_t *ctx, void *dst,
                                      const void *src, size_t n);

/* Submit a HOPPER_COMPUTE_A kernel launch on the compute channel
 * (which cohabits the DMA channel's non-UVM VAS).  The launched
 * kernel is the embedded mc_doorbell_kernel:
 *
 *   __global__ void mc_doorbell_kernel(volatile uint32_t *dst,
 *                                       uint32_t token)
 *   { *dst = token; }
 *
 * `dst_gpu_va` must be valid in the carrier VAS (i.e. allocated and
 * DMA-mapped through it).  Two intended call patterns:
 *
 *   - sysmem cell: pass the GPU VA of any DMA-mapped sysmem region.
 *     The host can read the cell back directly to verify the SM
 *     thread ran.
 *
 *   - BAR1 doorbell: pass `dbell_gpu_va + 0x90` so the SM thread
 *     issues an MMIO write to the H100's BAR1 doorbell register.
 *     Verifying this reached real BAR1 requires the chain-of-evidence
 *     pattern; see mc_memcpy_gpu_doorbell_sm.
 *
 * Synchronous: returns only after the kernel's release semaphore
 * fires (or MC_ETIMEOUT after MC_TIMEOUT_MS = 2 s).
 *
 * The first call also submits a one-time per-channel setup
 * pushbuffer (SET_OBJECT(HOPPER_COMPUTE_A), cache invalidates,
 * SHADER_*_MEMORY_WINDOW), built NVK-style — see
 * mc_write_compute_setup_methods.  Subsequent calls skip the setup. */
MC_API mc_status_t mc_compute_doorbell_kernel(mc_ctx_t *ctx, uint64_t dst_gpu_va,
                                       uint32_t token);

/* Expose a small, GPU-MMU-mapped scratch dword that lives in the
 * compute channel's VAS — useful as the `dst` for a host-visible
 * mc_compute_doorbell_kernel run.  Caller gets a host-readable CPU
 * pointer + the matching GPU VA the SM thread will dereference.  The
 * scratch lives for the lifetime of the mc_ctx and is not
 * refcounted — used as a one-shot test cell.
 *
 * Backed by a dedicated BAR1-aliased vidmem allocation (one RM hMemory,
 * one GPU MMU PTE) mapped into the carrier VAS during mc_init and
 * freed by mc_fini.  The returned pointer/VA target a dword in the
 * middle of the region, so callers can also write a few sentinel
 * dwords on either side of the target without touching unrelated
 * memory. */
MC_API mc_status_t mc_compute_get_scratch(mc_ctx_t *ctx, volatile uint32_t **cpu_ptr,
                                   uint64_t *gpu_va);

/* End-to-end-verifiable copy whose UVM-channel doorbell is rung by
 * an SM thread of a HOPPER_COMPUTE_A kernel — the GPU compute engine
 * itself, with no copy engine in the loop.  Direction is inferred
 * from the alloc table: pass any matched (host, device) pair
 * returned by mc_malloc_* on this context.  Both pointers MUST live
 * in MC_VAS_UVM — otherwise MC_EINVAL.
 *
 * Identical contract and verification framing as
 * mc_memcpy_gpu_doorbell_ce, but the doorbell write is issued by the
 * compute kernel's STG.E.SYS to dbell_gpu_va + 0x90 instead of by a
 * DMA-channel CE LAUNCH_DMA op.
 *
 * Sequence:
 *   1. Arm the UVM channel: build a CE pushbuffer at its pb_cpu
 *      (src→dst), write the GPFIFO entry, advance USERD GPPut.  DO
 *      NOT ring the UVM channel's vf_doorbell.
 *   2. Launch mc_compute_doorbell_kernel(ctx, dbell_gpu_va + 0x90,
 *                                         uvm_work_submit_token).
 *      A single SM thread runs `*p = token` — that store is a 4-byte
 *      MMIO write to BAR1's USERMODE_A VF_DOORBELL.
 *   3. Compute kernel's release semaphore fires (kernel exited; STG
 *      flushed through L2 to BAR1).
 *   4. Poll the UVM channel's sema (bytes flushed).
 *
 * Verification (D2H): the host buffer contains the bytes from the
 * device buffer iff the UVM channel's doorbell got rung.  The host
 * did NOT ring it.  No CE was used to ring it either.  The only
 * remaining possible source of the ring is the SM thread's MMIO
 * store.  Comparing the host buffer vs. expected bytes is proof —
 * no copy engine, no host doorbell, just a compute kernel writing
 * to BAR1.
 *
 * Verification (H2D): same caveat as mc_memcpy_gpu_doorbell_ce — the
 * load lands in the device buffer; verify by an *untimed*
 * mc_memcpy(MC_XFER_HOST) read-back into a scratch host buffer.
 *
 * Returns MC_OK on full chain success, MC_ETIMEOUT if either sema
 * doesn't fire within MC_TIMEOUT_MS, MC_EINVAL on invalid inputs. */
MC_API mc_status_t mc_memcpy_gpu_doorbell_sm(mc_ctx_t *ctx, void *dst,
                                      const void *src, size_t n);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MC_EXPERIMENTAL_H */
