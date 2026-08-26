/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * internal/feature.h — feature availability, implemented by core/feature.c.
 */
#ifndef MC_INTERNAL_FEATURE_H
#define MC_INTERNAL_FEATURE_H

#include <stdint.h>

#include <mc/mc.h>

#include "hal/arch.h"

/* Features this architecture and this build can provide, with dependencies
 * closed transitively.  Independent of what a caller requested. */
uint64_t mc_features_available(const mc_arch_desc_t *arch);

/* Log every feature's state under MC_VERBOSE. */
void mc_features_log(uint64_t active, uint64_t available);

#endif /* MC_INTERNAL_FEATURE_H */
