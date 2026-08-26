/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * mc_arch_desc_test — invariants every architecture descriptor must satisfy.
 *
 * A descriptor is data, so it can be checked without a GPU.  That matters:
 * most of what a backend gets wrong is a mistyped constant, and the failure
 * mode for a wrong class id or a misplaced doorbell offset is an Xid on
 * hardware nobody may have to hand.
 *
 * Several of these are invariants CLAUDE.md states in prose.  Stating them
 * here as well means a backend that violates one fails the build rather than
 * the GPU.  As backends land, each is added to `all_backends` below and
 * inherits the whole set.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hal/arch.h"

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

static const mc_arch_desc_t *const all_backends[] = {
  &mc_arch_hopper,
};
#define N_BACKENDS (sizeof(all_backends) / sizeof(all_backends[0]))

static void check_one(const mc_arch_desc_t *a)
{
  /* Every class id libmc passes to NV_ESC_RM_ALLOC must be populated.  A zero
   * here means the backend forgot a field, and RM would reject the alloc with
   * a status that says nothing about which class was missing. */
  EXPECT_MSG(a->cls.usermode       != 0, "%s: cls.usermode is zero", a->name);
  EXPECT_MSG(a->cls.channel_gpfifo != 0, "%s: cls.channel_gpfifo is zero", a->name);
  EXPECT_MSG(a->cls.dma_copy       != 0, "%s: cls.dma_copy is zero", a->name);
  EXPECT_MSG(a->cls.compute        != 0, "%s: cls.compute is zero", a->name);
  EXPECT_MSG(a->cls.channel_group  != 0, "%s: cls.channel_group is zero", a->name);
  EXPECT_MSG(a->cls.vaspace        != 0, "%s: cls.vaspace is zero", a->name);
  EXPECT_MSG(a->cls.virtual_memory != 0, "%s: cls.virtual_memory is zero", a->name);

  EXPECT_MSG(a->name != NULL && a->name[0] != '\0', "backend has no name");
  EXPECT_MSG(a->family != MC_ARCH_UNKNOWN, "%s: family is UNKNOWN", a->name);

  /* CLAUDE.md: "The work-submit doorbell is at +0x90 in the mapped USERMODE
   * window."  The offset is a descriptor field so a backend must state it
   * rather than inherit it silently -- but every architecture from Ampere
   * through Blackwell picks up the same virtualRegPhysOffset for both the
   * window base and the register, so the value is the same for all of them.
   * A backend disagreeing is far more likely to be a typo than a discovery;
   * if it ever is a discovery, this assert is the place to record it. */
  EXPECT_MSG(a->lay.vf_doorbell_offset == 0x90,
             "%s: vf_doorbell_offset is 0x%x, expected 0x90",
             a->name, a->lay.vf_doorbell_offset);

  /* USERD must start on a slot boundary: RM derives the channel's USERD slot
   * from this offset, and a misaligned one silently points the channel at
   * another channel's control area. */
  EXPECT_MSG(a->lay.userd_slot_bytes != 0, "%s: userd_slot_bytes is zero", a->name);
  if (a->lay.userd_slot_bytes != 0)
    EXPECT_MSG(a->lay.userd_offset % a->lay.userd_slot_bytes == 0,
               "%s: userd_offset 0x%x is not a multiple of the %u-byte slot",
               a->name, a->lay.userd_offset, a->lay.userd_slot_bytes);

  /* PBDMA masks the GPFIFO put index modulo the ring size, so a non-power-of-
   * two depth wraps to the wrong slot rather than failing loudly. */
  EXPECT_MSG(a->lay.gpfifo_entries != 0
             && (a->lay.gpfifo_entries & (a->lay.gpfifo_entries - 1)) == 0,
             "%s: gpfifo_entries %u is not a power of two",
             a->name, a->lay.gpfifo_entries);

  /* The USERD region has to fit inside the allocation that also holds the
   * GPFIFO ring at offset 0. */
  EXPECT_MSG((uint64_t)a->lay.userd_offset
                 >= (uint64_t)a->lay.gpfifo_entries * 8u,
             "%s: userd_offset 0x%x overlaps the %u-entry GPFIFO ring",
             a->name, a->lay.userd_offset, a->lay.gpfifo_entries);

  EXPECT_MSG(a->lay.big_page_bytes != 0
             && (a->lay.big_page_bytes & (a->lay.big_page_bytes - 1)) == 0,
             "%s: big_page_bytes is not a power of two", a->name);
  EXPECT_MSG(a->lay.vidmem_align_bytes != 0
             && (a->lay.vidmem_align_bytes & (a->lay.vidmem_align_bytes - 1)) == 0,
             "%s: vidmem_align_bytes is not a power of two", a->name);
  EXPECT_MSG(a->lay.usermode_window_bytes != 0,
             "%s: usermode_window_bytes is zero", a->name);

  /* Every hook must be present.  These are called without a NULL check on
   * the hot path -- a missing one is a null dereference inside submission,
   * which is a far worse failure than a build error. */
  EXPECT_MSG(a->ops != NULL, "%s: has no ops table", a->name);
  if (a->ops != NULL) {
    EXPECT_MSG(a->ops->alloc_usermode      != NULL, "%s: ops.alloc_usermode is NULL", a->name);
    EXPECT_MSG(a->ops->userd_set_gpput     != NULL, "%s: ops.userd_set_gpput is NULL", a->name);
    EXPECT_MSG(a->ops->userd_get_gpput     != NULL, "%s: ops.userd_get_gpput is NULL", a->name);
    EXPECT_MSG(a->ops->userd_try_get_gpget != NULL, "%s: ops.userd_try_get_gpget is NULL", a->name);
    EXPECT_MSG(a->ops->engine_for_role     != NULL, "%s: ops.engine_for_role is NULL", a->name);
  }

  /* A backend claiming its USERD publishes GPGet must actually return one,
   * or drain_channel silently takes the weaker semaphore path forever. */
  if (a->ops != NULL && a->ops->userd_try_get_gpget != NULL) {
    uint32_t probe_userd[64] = {0};
    uint32_t got = 0xffffffffu;
    bool     ok  = a->ops->userd_try_get_gpget(probe_userd, &got);
    EXPECT_MSG(ok == a->cap.userd_exposes_gpget,
               "%s: cap.userd_exposes_gpget says %d but the accessor returns %d",
               a->name, (int)a->cap.userd_exposes_gpget, (int)ok);
    /* NULL arguments must be rejected rather than dereferenced. */
    EXPECT_MSG(a->ops->userd_try_get_gpget(NULL, &got) == false,
               "%s: userd_try_get_gpget accepted a NULL USERD", a->name);
    EXPECT_MSG(a->ops->userd_try_get_gpget(probe_userd, NULL) == false,
               "%s: userd_try_get_gpget accepted a NULL out-pointer", a->name);
  }

  /* A BAR1-mapped usermode window can only be requested where the class
   * actually takes parameters -- bBar1Mapping lives in that struct.  A
   * backend claiming BAR1 capability without parameters would allocate a
   * BAR0 window and then install a doorbell PTE against the wrong aperture. */
  if (a->cap.usermode_bar1_capable)
    EXPECT_MSG(a->cap.usermode_takes_params,
               "%s: claims BAR1 usermode but not usermode_takes_params",
               a->name);
}

/* Two backends must never claim the same identity, or detection becomes
 * order-dependent. */
static void check_distinct(void)
{
  size_t i, j;
  for (i = 0; i < N_BACKENDS; i++)
    for (j = i + 1; j < N_BACKENDS; j++) {
      EXPECT_MSG(all_backends[i]->family != all_backends[j]->family,
                 "backends %s and %s share family %d",
                 all_backends[i]->name, all_backends[j]->name,
                 (int)all_backends[i]->family);
      EXPECT_MSG(strcmp(all_backends[i]->name, all_backends[j]->name) != 0,
                 "two backends are both named %s", all_backends[i]->name);
    }
}

int main(void)
{
  size_t i;
  for (i = 0; i < N_BACKENDS; i++)
    check_one(all_backends[i]);
  check_distinct();

  if (failures) {
    printf("%d architecture-descriptor invariant(s) violated\n", failures);
    return 1;
  }
  printf("PASS: architecture descriptors (%zu backend%s)\n",
         N_BACKENDS, N_BACKENDS == 1 ? "" : "s");
  return 0;
}
