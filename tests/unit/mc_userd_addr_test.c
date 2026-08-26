/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * mc_userd_addr_test — USERD physical-address reach, per architecture.
 *
 * A channel's USERD physical address is written into the runlist entry field
 * NV_RAMRL_ENTRY_CHAN_USERD_PTR_HI_HW, whose width is architecture-specific.
 * Where it is too narrow to hold an arbitrary host physical address, USERD
 * cannot live in sysmem, and cap.userd_requires_vidmem says so.
 *
 * That capability bit is a claim about the pinned headers, so this test
 * checks it against them rather than restating it:
 *
 *   1. _Static_asserts pin each architecture's field width to the value in
 *      its own dev_ram.h.  If NVIDIA ever changes one, the build breaks here
 *      instead of the GPU rejecting a channel at runtime.
 *   2. Every backend in the registry is then checked for agreement between
 *      its declared width and its userd_requires_vidmem bit.
 *   3. The two addresses actually measured on an A100 are classified, so the
 *      arithmetic that turns a width into a verdict is exercised on real
 *      data rather than only on round numbers.
 *
 * Evidence for (3), captured 2026-08-26 on a100-jarvis from the instrumented
 * driver's fifo/userd_resolve record (it fires immediately before
 * kchannelIsUserdAddrSizeValid_HAL and prints the aperture):
 *
 *     sysmem USERD  address_space=1  0x101b0174000  -> NV_ERR_INVALID_ADDRESS
 *     vidmem USERD  address_space=2  0x000014202000 -> OK
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "hal/arch.h"
#include "hal/registry.h"

/* The three dev_ram.h headers all define NV_RAMRL_ENTRY_CHAN_USERD_PTR_HI_HW,
 * so they cannot be in scope simultaneously.  Include each, capture the width,
 * and #undef before the next.  The include guards differ per architecture
 * (__ga100_dev_ram_h__ etc.), so the headers themselves do not collide. */

/* SF_MASK, verbatim from
 * src/nvidia/generated/g_gpu_access_nvoc.h:388.  Reproduced rather than
 * included because that is a generated NVOC header that pulls in the whole RM
 * object model, which a host-only test cannot link.  It is copied exactly,
 * not reinterpreted -- the `a:b` field syntax is what makes ?: work here. */
#define MC_SF_MASK(sf) (0xFFFFFFFFu >> (31 - (1 ? sf) + (0 ? sf)))

#include "swref/published/ampere/ga100/dev_ram.h"
static const uint32_t GA100_HI_MASK = MC_SF_MASK(NV_RAMRL_ENTRY_CHAN_USERD_PTR_HI_HW);
_Static_assert(MC_SF_MASK(NV_RAMRL_ENTRY_CHAN_USERD_PTR_HI_HW) == 0xFFu,
               "GA100 USERD_PTR_HI_HW is no longer 8 bits -- "
               "re-derive cap.userd_requires_vidmem for Ampere");
#undef NV_RAMRL_ENTRY_CHAN_USERD_PTR_HI_HW

#include "swref/published/hopper/gh100/dev_ram.h"
static const uint32_t GH100_HI_MASK = MC_SF_MASK(NV_RAMRL_ENTRY_CHAN_USERD_PTR_HI_HW);
_Static_assert(MC_SF_MASK(NV_RAMRL_ENTRY_CHAN_USERD_PTR_HI_HW) == 0xFFFFFu,
               "GH100 USERD_PTR_HI_HW is no longer 20 bits -- "
               "re-derive cap.userd_requires_vidmem for Hopper");
#undef NV_RAMRL_ENTRY_CHAN_USERD_PTR_HI_HW

#include "swref/published/blackwell/gb10b/dev_ram.h"
_Static_assert(MC_SF_MASK(NV_RAMRL_ENTRY_CHAN_USERD_PTR_HI_HW) == 0xFFFFFu,
               "GB10B USERD_PTR_HI_HW is no longer 20 bits -- "
               "re-derive cap.userd_requires_vidmem for Blackwell");
#undef NV_RAMRL_ENTRY_CHAN_USERD_PTR_HI_HW

/* The measured addresses. */
#define MEASURED_SYSMEM_USERD 0x101b0174000ULL   /* rejected on GA100 */
#define MEASURED_VIDMEM_USERD 0x000014202000ULL  /* accepted on GA100 */

static int failures;

#define EXPECT_MSG(cond, ...)                                           \
  do {                                                                  \
    if (!(cond)) {                                                      \
      printf("FAIL %s:%d: ", __FILE__, __LINE__);                       \
      printf(__VA_ARGS__);                                              \
      printf("\n");                                                     \
      failures++;                                                       \
    }                                                                   \
  } while (0)

/* Highest USERD physical address a runlist entry with this HI mask can hold.
 * The field carries the HIGH 32 bits, so the reach is (mask << 32) | 2^32-1.
 * Mirrors kchannelIsUserdAddrSizeValid_GA100, which round-trips userdAddrHi
 * through SF_MASK and compares. */
static uint64_t userd_reach(uint32_t hi_mask)
{
  return ((uint64_t)hi_mask << 32) | 0xFFFFFFFFULL;
}

static bool userd_addr_fits(uint32_t hi_mask, uint64_t addr)
{
  return addr <= userd_reach(hi_mask);
}

static void test_measured_addresses_classify(void)
{
  /* The exact pair captured on the A100.  These are the whole reason the
   * capability bit exists, so they are checked literally. */
  EXPECT_MSG(!userd_addr_fits(GA100_HI_MASK, MEASURED_SYSMEM_USERD),
             "the sysmem USERD address measured failing on GA100 (%#llx) "
             "is computed as fitting",
             (unsigned long long)MEASURED_SYSMEM_USERD);
  EXPECT_MSG(userd_addr_fits(GA100_HI_MASK, MEASURED_VIDMEM_USERD),
             "the vidmem USERD address measured working on GA100 (%#llx) "
             "is computed as not fitting",
             (unsigned long long)MEASURED_VIDMEM_USERD);

  /* The same sysmem address is comfortably in range on Hopper -- which is why
   * the sysmem carrier works there and hid this for so long. */
  EXPECT_MSG(userd_addr_fits(GH100_HI_MASK, MEASURED_SYSMEM_USERD),
             "the sysmem USERD address (%#llx) should fit on Hopper's 20-bit "
             "field but does not",
             (unsigned long long)MEASURED_SYSMEM_USERD);

  /* It overshoots Ampere's limit by only ~6.9 GiB.  A smaller host could land
   * under it and work by luck, which is exactly why placement is a rule
   * rather than something probed at runtime. */
  EXPECT_MSG(MEASURED_SYSMEM_USERD - userd_reach(GA100_HI_MASK) < (16ULL << 30),
             "the measured overshoot is no longer small; re-check the claim "
             "that a sysmem USERD can pass by luck on a smaller host");

  printf("PASS: measured A100 USERD addresses classify as observed\n");
}

static void test_boundary(void)
{
  EXPECT_MSG(userd_addr_fits(GA100_HI_MASK, (1ULL << 40) - 1),
             "GA100: 2^40-1 should be the last address that fits");
  EXPECT_MSG(!userd_addr_fits(GA100_HI_MASK, 1ULL << 40),
             "GA100: 2^40 should not fit");
  EXPECT_MSG(userd_addr_fits(GH100_HI_MASK, (1ULL << 52) - 1),
             "GH100: 2^52-1 should be the last address that fits");
  EXPECT_MSG(!userd_addr_fits(GH100_HI_MASK, 1ULL << 52),
             "GH100: 2^52 should not fit");
  printf("PASS: USERD address reach boundaries\n");
}

/* Width declared by each architecture's own header, keyed by family.  A
 * backend whose family is absent here is one whose dev_ram.h nobody has read
 * yet -- which the test reports rather than skipping silently. */
static bool hi_mask_for_family(mc_arch_family_t f, uint32_t *out_mask)
{
  switch (f) {
    case MC_ARCH_AMPERE:    *out_mask = GA100_HI_MASK; return true;
    /* Ada reuses AMPERE_CHANNEL_GPFIFO_A, so it inherits Ampere's runlist
     * entry format along with the class. */
    case MC_ARCH_ADA:       *out_mask = GA100_HI_MASK; return true;
    case MC_ARCH_HOPPER:    *out_mask = GH100_HI_MASK; return true;
    case MC_ARCH_BLACKWELL: *out_mask = GH100_HI_MASK; return true;
    default:                return false;
  }
}

/* The claim under test: userd_requires_vidmem is set exactly when the field
 * is too narrow to hold a plausible host physical address.
 *
 * "Plausible" needs a threshold, and 2^40 is the honest one: it is where
 * Ampere's field stops, and host memory routinely lives above it (the A100
 * measured here put USERD at 1.007 TiB).  An architecture that can reach past
 * 2^40 has no placement constraint worth encoding; one that cannot, does. */
static void test_registry_agrees_with_headers(void)
{
  const mc_arch_desc_t *const *regs;
  size_t n = 0, i;

  regs = mc_arch_registry(&n);
  EXPECT_MSG(regs != NULL && n > 0, "registry is empty");

  for (i = 0; i < n; i++) {
    const mc_arch_desc_t *a = regs[i];
    uint32_t mask = 0;
    bool need_vidmem;

    if (!hi_mask_for_family(a->family, &mask)) {
      printf("FAIL %s:%d: backend '%s' has no known "
             "NV_RAMRL_ENTRY_CHAN_USERD_PTR_HI_HW width; read its dev_ram.h "
             "and add it\n", __FILE__, __LINE__, a->name);
      failures++;
      continue;
    }

    need_vidmem = (userd_reach(mask) < (1ULL << 40));

    EXPECT_MSG(a->cap.userd_requires_vidmem == need_vidmem,
               "backend '%s': cap.userd_requires_vidmem=%d but its "
               "USERD field reaches %#llx, which implies %d",
               a->name, (int)a->cap.userd_requires_vidmem,
               (unsigned long long)userd_reach(mask), (int)need_vidmem);
  }
  printf("PASS: every backend's userd_requires_vidmem matches its header\n");
}

/* Guard the specific values, so a future edit that flips one has to come
 * back through this file and its evidence. */
static void test_known_backends(void)
{
  const mc_arch_desc_t *amp = mc_arch_registry_by_name("ampere");
  const mc_arch_desc_t *hop = mc_arch_registry_by_name("hopper");

  if (amp != NULL)
    EXPECT_MSG(amp->cap.userd_requires_vidmem,
               "ampere must require vidmem USERD: its runlist entry reaches "
               "only 2^40 and a host address exceeds that");
  if (hop != NULL)
    EXPECT_MSG(!hop->cap.userd_requires_vidmem,
               "hopper must not require vidmem USERD: its runlist entry "
               "reaches 2^52, so the sysmem carrier is fine as-is");
  printf("PASS: ampere requires vidmem USERD, hopper does not\n");
}

int main(void)
{
  test_measured_addresses_classify();
  test_boundary();
  test_registry_agrees_with_headers();
  test_known_backends();

  if (failures) {
    printf("%d USERD address check(s) failed\n", failures);
    return 1;
  }
  return 0;
}
