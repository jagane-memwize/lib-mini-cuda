/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * hal/registry.h — the table of compiled-in architecture backends.
 *
 * Split from detection on purpose: everything here is a pure function of the
 * table, so it is testable without a GPU, without ioctls, and without a
 * context.  hal/detect.c owns the part that has to talk to the driver.
 */
#ifndef MC_HAL_REGISTRY_H
#define MC_HAL_REGISTRY_H

#include <stddef.h>

#include "hal/arch.h"

/* Backends compiled into this build, in match order. */
const mc_arch_desc_t *const *mc_arch_registry(size_t *out_count);

/* Find the backend claiming this (architecture, implementation) pair, or NULL.
 *
 * The architecture is the discriminator; the implementation only narrows
 * within it, because implementation values are namespaced per architecture.
 * A backend with rm_impl_mask == 0 accepts any implementation of its
 * architecture. */
const mc_arch_desc_t *mc_arch_registry_lookup(NvU32 architecture,
                                              NvU32 implementation);

/* Find a backend by name.  Used only by the MC_ARCH_FORCE bring-up override. */
const mc_arch_desc_t *mc_arch_registry_by_name(const char *name);

/* Comma-separated list of compiled-in backend names, for error messages.
 * Returns a pointer to a static buffer. */
const char *mc_arch_registry_names(void);

#endif /* MC_HAL_REGISTRY_H */
