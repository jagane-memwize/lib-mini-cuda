/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * hal/detect.h — GPU identification and backend selection.
 */
#ifndef MC_HAL_DETECT_H
#define MC_HAL_DETECT_H

#include "nvtypes.h"
#include "hal/arch.h"

/* Identify the GPU behind (h_client, h_device, h_subdevice) and select its
 * backend, verifying that the device exposes the classes that backend will
 * allocate.
 *
 * Returns 0 with *out_arch set, or -1 with a diagnostic already logged --
 * unknown architecture, missing class, or a failed query.  Callers map the
 * failure to MC_EARCH.
 *
 * Must run after the subdevice exists (the arch query is subdevice-scoped)
 * and before any architecture-specific object is allocated. */
int mc_detect_arch(int ctl_fd, NvHandle h_client, NvHandle h_device,
                   NvHandle h_subdevice, const mc_arch_desc_t **out_arch);

#endif /* MC_HAL_DETECT_H */
