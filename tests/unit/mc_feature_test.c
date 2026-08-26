/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * mc_feature_test — feature availability derived from an architecture.
 *
 * The interesting cases are the ones no hardware here can exercise.  Ampere
 * and Ada have no BAR1 usermode aperture, so they can never install a
 * doorbell PTE, so the FB carrier and every SM-authored path are permanently
 * unavailable on them -- a property of the silicon, not of any driver.  That
 * is the whole reason feature availability exists, and it can be tested now,
 * against synthetic descriptors, long before either backend is written.
 *
 * Using synthetic descriptors rather than the real ones is deliberate: the
 * question is whether the DERIVATION is right for a given shape of hardware,
 * not what today's single backend happens to claim.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <mc/mc.h>

#include "hal/arch.h"
#include "internal/feature.h"

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

#define HAS(mask, f) (((mask) & MC_FEAT_BIT(f)) != 0)

/* A QMD builder need not do anything for these tests; only its presence is
 * consulted, as the marker that the architecture has compute support. */
static void stub_build_qmd(uint8_t *q, uint64_t p, uint32_t r,
                           uint64_t c, uint32_t n)
{ (void)q; (void)p; (void)r; (void)c; (void)n; }

static const mc_compute_ops_t stub_compute = {
    .qmd_bytes = 384, .qmd_align = 256, .cbuf_align = 0x100,
    .version_major = 4, .build_qmd = stub_build_qmd,
};

/* Stand-in cubins.  Only presence and length are consulted here. */
static const uint8_t stub_sass[16];
#define STUB_KERNELS                                            \
    { .sm_owner = { stub_sass, sizeof(stub_sass), 32 },         \
      .doorbell = { stub_sass, sizeof(stub_sass), 8 } }

/* Hopper-shaped: BAR1 aperture, compute support, both cubins present. */
static const mc_arch_desc_t desc_full = {
    .name = "synthetic-full",
    .cap = { .usermode_bar1_capable = true },
    .compute = &stub_compute,
    .kernels = STUB_KERNELS,
};

/* Ampere/Ada-shaped: compute, but no BAR1 usermode aperture. */
static const mc_arch_desc_t desc_no_bar1 = {
    .name = "synthetic-no-bar1",
    .cap = { .usermode_bar1_capable = false },
    .compute = &stub_compute,
    .kernels = STUB_KERNELS,
};

/* A device whose class list lacked the compute class. */
static const mc_arch_desc_t desc_no_compute = {
    .name = "synthetic-no-compute",
    .cap = { .usermode_bar1_capable = true },
    .compute = NULL,
    .kernels = STUB_KERNELS,
};

/* A backend written before its cubins were generated: full capability on
 * paper, no compiled kernels.  SASS does not port across SM versions, so this
 * is the normal state of a new backend and must degrade rather than break. */
static const mc_arch_desc_t desc_no_cubins = {
    .name = "synthetic-no-cubins",
    .cap = { .usermode_bar1_capable = true },
    .compute = &stub_compute,
    /* .kernels left zeroed */
};

static void test_full_architecture(void)
{
  uint64_t a = mc_features_available(&desc_full);
  EXPECT_MSG(HAS(a, MC_FEAT_XFER_HOST),      "full: xfer_host missing");
  EXPECT_MSG(HAS(a, MC_FEAT_HOST_REGISTER),  "full: host_register missing");
  EXPECT_MSG(HAS(a, MC_FEAT_CARRIER_SYSMEM), "full: carrier_sysmem missing");
  EXPECT_MSG(HAS(a, MC_FEAT_CARRIER_FB),     "full: carrier_fb missing");
  EXPECT_MSG(HAS(a, MC_FEAT_COMPUTE_LAUNCH), "full: compute_launch missing");
  EXPECT_MSG(HAS(a, MC_FEAT_CE_DOORBELL_RING), "full: ce_doorbell_ring missing");
#if MC_ENABLE_EXPERIMENTAL
  EXPECT_MSG(HAS(a, MC_FEAT_XFER_SM), "full: xfer_sm missing in an experimental build");
#else
  EXPECT_MSG(!HAS(a, MC_FEAT_XFER_SM), "full: xfer_sm present in a stable build");
#endif
  printf("PASS: a fully capable architecture offers everything\n");
}

/* The case the whole mechanism exists for. */
static void test_no_bar1_loses_doorbell_paths(void)
{
  uint64_t a = mc_features_available(&desc_no_bar1);

  /* Copies still work.  This is the point: an Ampere backend is useful. */
  EXPECT_MSG(HAS(a, MC_FEAT_XFER_HOST),      "no-bar1: lost xfer_host");
  EXPECT_MSG(HAS(a, MC_FEAT_HOST_REGISTER),  "no-bar1: lost host_register");
  EXPECT_MSG(HAS(a, MC_FEAT_CARRIER_SYSMEM), "no-bar1: lost carrier_sysmem");
  EXPECT_MSG(HAS(a, MC_FEAT_COMPUTE_LAUNCH), "no-bar1: lost compute_launch");

  /* Everything that needs a doorbell PTE is gone. */
  EXPECT_MSG(!HAS(a, MC_FEAT_CE_DOORBELL_RING),
             "no-bar1: still offers ce_doorbell_ring without a BAR1 aperture");
  EXPECT_MSG(!HAS(a, MC_FEAT_CARRIER_FB),
             "no-bar1: still offers carrier_fb without a BAR1 aperture");

  /* And so is SM-authored submission, which depends on the doorbell path --
   * this one comes from dependency closure rather than a direct rule, so it
   * is the check that proves the closure works. */
  EXPECT_MSG(!HAS(a, MC_FEAT_XFER_SM),
             "no-bar1: still offers xfer_sm, whose dependency is unavailable");
  printf("PASS: no BAR1 aperture removes every doorbell-dependent feature\n");
}

static void test_no_compute(void)
{
  uint64_t a = mc_features_available(&desc_no_compute);
  EXPECT_MSG(HAS(a, MC_FEAT_XFER_HOST), "no-compute: lost xfer_host");
  EXPECT_MSG(!HAS(a, MC_FEAT_COMPUTE_LAUNCH),
             "no-compute: offers compute_launch with no QMD builder");
  EXPECT_MSG(!HAS(a, MC_FEAT_XFER_SM),
             "no-compute: offers xfer_sm with no QMD builder");
  /* A doorbell ring by the copy engine needs no compute at all. */
  EXPECT_MSG(HAS(a, MC_FEAT_CE_DOORBELL_RING),
             "no-compute: lost ce_doorbell_ring, which does not need compute");
  printf("PASS: absent compute support removes only compute-dependent features\n");
}

static void test_missing_cubins(void)
{
  uint64_t a = mc_features_available(&desc_no_cubins);

  /* Copies never needed a kernel. */
  EXPECT_MSG(HAS(a, MC_FEAT_XFER_HOST),      "no-cubins: lost xfer_host");
  EXPECT_MSG(HAS(a, MC_FEAT_CARRIER_SYSMEM), "no-cubins: lost carrier_sysmem");
  EXPECT_MSG(HAS(a, MC_FEAT_CARRIER_FB),     "no-cubins: lost carrier_fb");

  /* Anything that launches a kernel needs its bytes. */
  EXPECT_MSG(!HAS(a, MC_FEAT_COMPUTE_LAUNCH),
             "no-cubins: offers compute_launch with no doorbell cubin");
  EXPECT_MSG(!HAS(a, MC_FEAT_XFER_SM),
             "no-cubins: offers xfer_sm with no sm_owner cubin");
  printf("PASS: absent cubins remove only the features that launch them\n");
}

static void test_dependency_closure_is_total(void)
{
  const mc_arch_desc_t *descs[] = { &desc_full, &desc_no_bar1, &desc_no_compute,
                                    &desc_no_cubins };
  size_t d;

  /* No feature may ever be available while something it depends on is not.
   * Checked over every shape rather than spot-checked, because the closure
   * is iterative and an ordering mistake in the table would show up as one
   * specific feature surviving. */
  for (d = 0; d < sizeof(descs) / sizeof(descs[0]); d++) {
    uint64_t a = mc_features_available(descs[d]);
    if (HAS(a, MC_FEAT_XFER_SM)) {
      EXPECT_MSG(HAS(a, MC_FEAT_COMPUTE_LAUNCH),
                 "%s: xfer_sm without compute_launch", descs[d]->name);
      EXPECT_MSG(HAS(a, MC_FEAT_CE_DOORBELL_RING),
                 "%s: xfer_sm without ce_doorbell_ring", descs[d]->name);
    }
    if (HAS(a, MC_FEAT_COMPUTE_LAUNCH))
      EXPECT_MSG(HAS(a, MC_FEAT_CARRIER_SYSMEM),
                 "%s: compute_launch without carrier_sysmem", descs[d]->name);
    if (HAS(a, MC_FEAT_CE_DOORBELL_RING))
      EXPECT_MSG(HAS(a, MC_FEAT_CARRIER_SYSMEM),
                 "%s: ce_doorbell_ring without carrier_sysmem", descs[d]->name);
  }
  printf("PASS: dependency closure holds for every architecture shape\n");
}

static void test_names_and_bounds(void)
{
  unsigned f;
  for (f = 0; f < MC_FEAT_COUNT; f++) {
    const char *n = mc_feature_name((mc_feature_t)f);
    EXPECT_MSG(n != NULL && n[0] != '\0' && strcmp(n, "invalid") != 0,
               "feature %u has no name", f);
  }
  /* Out-of-range must be reported, not indexed. */
  EXPECT_MSG(strcmp(mc_feature_name((mc_feature_t)MC_FEAT_COUNT), "invalid") == 0,
             "out-of-range feature id was not rejected");
  EXPECT_MSG(mc_features_available(NULL) == 0,
             "a NULL architecture offered features");
  printf("PASS: feature names and bounds\n");
}

int main(void)
{
  test_full_architecture();
  test_no_bar1_loses_doorbell_paths();
  test_no_compute();
  test_missing_cubins();
  test_dependency_closure_is_total();
  test_names_and_bounds();

  if (failures) {
    printf("%d feature check(s) failed\n", failures);
    return 1;
  }
  return 0;
}
