/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * mc_methods_test — golden-byte tests for the GPU method-stream emitters.
 *
 * Every word libmc sends to PBDMA comes out of hal/methods.c.  A wrong bit in
 * one of them is not a test failure, it is an Xid: the GPU faults and the
 * process dies, or worse, a copy silently reads the wrong address.  So the
 * emitters are pinned here against byte-exact expected output.
 *
 * The golden values were captured from the implementation at the commit that
 * introduced this test, which was validated on an H100 (driver 610.43.02)
 * with the full 51-cell integration matrix passing.  They are a regression
 * fence, not an independent derivation -- their authority comes from that
 * hardware run.
 *
 * Why this exists: the integration matrix is the only other check on these
 * encodings, it needs a GPU, and its bandwidth numbers carry ~8% run-to-run
 * noise, so it cannot resolve a small behavioural change.  This test is
 * deterministic and runs on any host.
 *
 * If a change here is intentional, re-capture the goldens AND re-run the
 * integration matrix -- do not update one without the other.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hal/methods.h"

static int failures;

/* Report PASS only if this check actually added no failures.  Printing an
 * unconditional PASS at the end of a test function makes a failing run claim
 * both FAIL and PASS for the same check. */
#define BEGIN_CHECK() int failures_at_entry = failures
#define END_CHECK(name)                                                 \
  do {                                                                  \
    if (failures == failures_at_entry) printf("PASS: %s\n", (name));    \
  } while (0)

#define EXPECT(cond)                                                    \
  do {                                                                  \
    if (!(cond)) {                                                      \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
      failures++;                                                       \
    }                                                                   \
  } while (0)

/* Compare an emitted stream against its golden, reporting the first
 * differing dword rather than just "not equal". */
static void expect_stream(const char *what,
                          const uint32_t *got, size_t got_n,
                          const uint32_t *want, size_t want_n)
{
  size_t i;
  if (got_n != want_n) {
    printf("FAIL %s: emitted %zu dwords, expected %zu\n", what, got_n, want_n);
    failures++;
    return;
  }
  for (i = 0; i < want_n; i++) {
    if (got[i] != want[i]) {
      printf("FAIL %s: dword %zu is 0x%08x, expected 0x%08x\n",
             what, i, got[i], want[i]);
      failures++;
      return;
    }
  }
}

/* ── Copy-engine transfer stream ──────────────────────────────────────────
 * Two LAUNCH_DMAs: the copy itself (0x182 = NON_PIPELINED | FLUSH_ENABLE |
 * physical layouts) then a semaphore-only release (0xc). */
static void test_transfer_methods(void)
{
  BEGIN_CHECK();
  static const uint32_t want[] = {
    0x20018000, 0x0000c8b5,             /* SET_OBJECT(HOPPER_DMA_COPY_A)   */
    0x20028100, 0x01223344, 0x55667788, /* OFFSET_IN_UPPER/LOWER           */
    0x20028102, 0x01aabbcc, 0xddeeff00, /* OFFSET_OUT_UPPER/LOWER          */
    0x20018106, 0x00001000,             /* LINE_LENGTH_IN = 0x1000         */
    0x200180c0, 0x00000182,             /* LAUNCH_DMA (the copy)           */
    0x20038090, 0x00fef00d, 0xbaadf00d, 0x5a5a5a5a, /* SET_SEMAPHORE_A/B/C */
    0x200180c0, 0x0000000c,             /* LAUNCH_DMA (semaphore release)  */
  };
  uint32_t pb[64];
  uint32_t *end = mc_write_transfer_methods(pb,
                                            0x1122334455667788ULL,
                                            0x99aabbccddeeff00ULL,
                                            0x1000,
                                            0xcafef00dbaadf00dULL,
                                            0x5a5a5a5a);
  expect_stream("transfer", pb, (size_t)(end - pb),
                want, sizeof(want) / sizeof(want[0]));

  /* The high bits of each 64-bit address must land in the _UPPER method and
   * be masked to the field width -- an unmasked shift here is exactly the
   * kind of error that produces an out-of-range DMA rather than a fault. */
  EXPECT(pb[3] == 0x01223344);   /* src >> 32, masked */
  EXPECT(pb[6] == 0x01aabbcc);   /* dst >> 32, masked */
  END_CHECK("CE transfer method stream");
}

/* ── Compute setup ────────────────────────────────────────────────────────*/
static void test_compute_setup(void)
{
  BEGIN_CHECK();
  static const uint32_t want[] = {
    0x20012000, 0x0000cbc0,             /* SET_OBJECT(HOPPER_COMPUTE_A)     */
    0x200120a6, 0x00000000,             /* INVALIDATE_SKED_CACHES           */
    0x20012509, 0x00000000,             /* INVALIDATE_SAMPLER_CACHE_NO_WFI  */
    0x20012091, 0x00000000,             /* INVALIDATE_TEXTURE_HEADER_...    */
    0x200220a8, 0x00000004, 0x00000000, /* SHARED_MEMORY_WINDOW = 16 GiB    */
    0x200221ec, 0x00000000, 0xff000000, /* LOCAL_MEMORY_WINDOW = 0xff<<24   */
  };
  uint32_t pb[64];
  uint32_t *end = mc_compute_write_setup(pb);
  expect_stream("compute_setup", pb, (size_t)(end - pb),
                want, sizeof(want) / sizeof(want[0]));

  /* Hopper requires a non-zero, 4 GiB-aligned shared-memory window; a zero
   * here silently turns every global store into an SMEM access. */
  EXPECT(pb[9] == 0x00000004 && pb[10] == 0x00000000);
  END_CHECK("compute setup method stream");
}

/* ── Compute launch, both cache-invalidation modes ────────────────────────
 * The invalidating form must be exactly the base form with a two-dword
 * prefix: if it ever diverges further, the two call sites are no longer
 * emitting the same launch and the shared builder is lying. */
static void test_compute_launch(void)
{
  BEGIN_CHECK();
  static const uint32_t want_base[] = {
    0x200120ad, 0x12345678,             /* SEND_PCAS_A(qmd >> 8)            */
    0x200120b0, 0x0000000a,             /* SEND_SIGNALING_PCAS2_B           */
    0x200426c0, 0x00000000, 0xfeedface, /* SET_REPORT_SEMAPHORE_A/B/C       */
    0x11223344, 0x10000000,             /* payload, control (RELEASE)       */
  };
  static const uint32_t want_inval_prefix[] = {
    0x200125a6, 0x00001010,             /* INVALIDATE_SHADER_CACHES_NO_WFI  */
  };
  uint32_t base[64], inval[64];
  size_t   base_n, inval_n, i;

  base_n  = (size_t)(mc_compute_write_launch(base, 0x1234567800ULL,
                                             0xfeedfaceULL, 0x11223344,
                                             false) - base);
  inval_n = (size_t)(mc_compute_write_launch(inval, 0x1234567800ULL,
                                             0xfeedfaceULL, 0x11223344,
                                             true) - inval);

  expect_stream("compute_launch(no invalidate)", base, base_n,
                want_base, sizeof(want_base) / sizeof(want_base[0]));

  EXPECT(inval_n == base_n + 2);
  if (inval_n == base_n + 2) {
    EXPECT(inval[0] == want_inval_prefix[0]);
    EXPECT(inval[1] == want_inval_prefix[1]);
    for (i = 0; i < base_n; i++)
      EXPECT(inval[i + 2] == base[i]);
  }
  END_CHECK("compute launch method stream (both invalidation modes)");
}

/* ── GPFIFO entry ─────────────────────────────────────────────────────────
 * A pushbuffer VA at or above 2^40 needs a SET_PB_SEGMENT_EXTENDED_BASE
 * opcode ahead of the real entry, so the builder consumes two ring slots.
 * AMPERE_CHANNEL_GPFIFO_A has no such opcode, which is why this path
 * becomes a capability question once backends land. */
static void test_gp_entry(void)
{
  BEGIN_CHECK();
  volatile uint32_t ring[32];
  uint32_t used;

  memset((void *)ring, 0, sizeof(ring));
  used = write_gp_entry(ring, 0, 512, 0x10000ULL, 0, 0x80);
  EXPECT(used == 1);
  /* GP_ENTRY0 carries the VA's low bits shifted right by 2. */
  EXPECT(ring[0] == (0x10000u >> 2 << 2));
  EXPECT(ring[1] != 0);

  memset((void *)ring, 0, sizeof(ring));
  used = write_gp_entry(ring, 3, 512, 0x0000ABCDEF000000ULL, 0x40, 0x80);
  EXPECT(used == 2);          /* extended-base opcode + the entry itself */
  EXPECT(ring[6] == 0x0000ab00);
  EXPECT(ring[7] == 0x00000004);
  END_CHECK("GPFIFO entry encoding (incl. extended base)");
}

int main(void)
{
  test_transfer_methods();
  test_compute_setup();
  test_compute_launch();
  test_gp_entry();
  if (failures) {
    printf("%d method-encoding check(s) failed\n", failures);
    return 1;
  }
  return 0;
}
