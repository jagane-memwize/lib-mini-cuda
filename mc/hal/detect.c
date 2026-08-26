/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * hal/detect.c — identify the GPU and select its backend.
 *
 * Two questions, asked in order:
 *
 *   1. What is this chip?  NV2080_CTRL_CMD_MC_GET_ARCH_INFO returns the
 *      architecture and implementation; the registry maps that to a backend.
 *
 *   2. Does the backend fit?  Every class id the backend is about to allocate
 *      must appear in NV0080_CTRL_CMD_GPU_GET_CLASSLIST_V2.  This second call
 *      is what makes the answer trustworthy rather than merely plausible: a
 *      MIG-partitioned or feature-reduced device can report an architecture we
 *      support while not exposing the classes we need, and without the check
 *      that surfaces later as a bare RM status from some allocation, with
 *      nothing pointing at which class was missing.
 *
 * Unknown silicon is a hard failure.  There is no nearest-backend fallback:
 * the wrong class id in a SET_OBJECT does not degrade gracefully, it produces
 * an Xid attributed to the caller's process.  Everything else about this
 * library is exact -- the pinned submodule, the strict driver-version
 * handshake -- and a soft edge here would be the only guess in it.
 */

#include <stddef.h>
#include <stdlib.h>

#include "nvtypes.h"

#include "hal/arch.h"
#include "hal/detect.h"
#include "hal/registry.h"
#include "internal/log.h"
#include "internal/rm.h"

#define MC_CLASSLIST_MAX 256

static bool classlist_has(const NvU32 *classes, NvU32 count, NvU32 want)
{
    NvU32 i;
    for (i = 0; i < count; i++)
        if (classes[i] == want)
            return true;
    return false;
}

/* Print what the device DOES offer, alongside what the backend wanted.
 *
 * Only on the failure path, and only the classes in the ranges libmc cares
 * about -- a full list is ~100 entries of mostly display and DMA classes that
 * bury the answer.  The engine classes all live in the 0x_0B5 / 0x_0C0 /
 * 0x_06F families, so filtering to those turns "which one should I have
 * asked for?" into a line you can read. */
static void classlist_dump(const mc_arch_desc_t *arch,
                           const NvU32 *classes, NvU32 count)
{
    NvU32 i;

    ERROR_LOG("backend %s wanted: usermode 0x%04x gpfifo 0x%04x copy 0x%04x "
              "compute 0x%04x",
              arch->name, (unsigned)arch->cls.usermode,
              (unsigned)arch->cls.channel_gpfifo,
              (unsigned)arch->cls.dma_copy, (unsigned)arch->cls.compute);

    for (i = 0; i < count; i++)
    {
        /* Low BYTE, not low nibble-triple: the engine families are 0x__B5
         * (copy), 0x__C0 (compute), 0x__6F (channel/GPFIFO) and 0x_561
         * (usermode), so AMPERE_DMA_COPY_B 0xC7B5 masks to 0xB5. */
        NvU32 low = classes[i] & 0xFFu;
        if (low == 0xB5u || low == 0xC0u || low == 0x6Fu
            || (classes[i] & 0xFFFu) == 0x561u)
            ERROR_LOG("  device offers 0x%04x", (unsigned)classes[i]);
    }
}

/* Confirm the device exposes every class this backend will allocate.
 *
 * The compute class is deliberately NOT required: a device without it can
 * still do host-authored copies, and the compute-dependent paths are gated
 * separately.  Failing init over a class only some features need would turn a
 * usable GPU into an unusable one. */
static int verify_classes(int ctl_fd, NvHandle h_client, NvHandle h_device,
                          const mc_arch_desc_t *arch)
{
    NvU32 classes[MC_CLASSLIST_MAX];
    NvU32 count = 0;
    NvU32 i;

    const struct { NvU32 id; const char *what; } required[] = {
        { arch->cls.usermode,       "usermode"       },
        { arch->cls.channel_gpfifo, "channel_gpfifo" },
        { arch->cls.dma_copy,       "dma_copy"       },
        { arch->cls.channel_group,  "channel_group"  },
        { arch->cls.vaspace,        "vaspace"        },
        { arch->cls.virtual_memory, "virtual_memory" },
    };

    if (rm_get_classlist(ctl_fd, h_client, h_device,
                         classes, MC_CLASSLIST_MAX, &count) != 0)
        return -1;

    for (i = 0; i < sizeof(required) / sizeof(required[0]); i++)
        if (!classlist_has(classes, count, required[i].id))
        {
            ERROR_LOG("backend %s expects class 0x%04x (%s) but this device "
                      "does not expose it (%u classes reported)",
                      arch->name, (unsigned)required[i].id, required[i].what,
                      (unsigned)count);
            /* Bringing up a new chip, this is the question immediately after
             * "which class is missing?" -- what does it offer instead.  The
             * A2 (GA107) reported architecture GA100 and then failed here on
             * the copy class, because GA10x carries the _B forms; the answer
             * was in this list.  Cheap to print and only on the failure path. */
            classlist_dump(arch, classes, count);
            return -1;
        }

    if (!classlist_has(classes, count, arch->cls.compute))
        WARN_LOG("backend %s: compute class 0x%04x absent from this device's "
                 "class list; compute-dependent paths will be unavailable",
                 arch->name, (unsigned)arch->cls.compute);

    DEBUG_LOG("class list: %u classes, all %zu required classes present",
              (unsigned)count, sizeof(required) / sizeof(required[0]));
    return 0;
}

int mc_detect_arch(int ctl_fd, NvHandle h_client, NvHandle h_device,
                   NvHandle h_subdevice, const mc_arch_desc_t **out_arch)
{
    const mc_arch_desc_t *arch;
    const char           *forced;
    NvU32                 architecture = 0, implementation = 0, revision = 0;

    if (out_arch == NULL) return -1;
    *out_arch = NULL;

    if (rm_get_arch_info(ctl_fd, h_client, h_subdevice,
                         &architecture, &implementation, &revision) != 0)
        return -1;

    DEBUG_LOG("chip: architecture=0x%x implementation=0x%x revision=0x%x",
              (unsigned)architecture, (unsigned)implementation,
              (unsigned)revision);

    arch = mc_arch_registry_lookup(architecture, implementation);

    /* Bring-up override.  Driving a new chip with an existing backend is how
     * a port starts, so the escape hatch exists -- but it is unsupported by
     * construction, so it warns on every init rather than quietly succeeding. */
    forced = getenv("MC_ARCH_FORCE");
    if (forced != NULL && forced[0] != '\0')
    {
        const mc_arch_desc_t *f = mc_arch_registry_by_name(forced);
        if (f == NULL)
        {
            ERROR_LOG("MC_ARCH_FORCE=%s: no such backend (have: %s)",
                      forced, mc_arch_registry_names());
            return -1;
        }
        WARN_LOG("MC_ARCH_FORCE=%s overrides detected architecture 0x%x/0x%x "
                 "— UNSUPPORTED, for bring-up only", forced,
                 (unsigned)architecture, (unsigned)implementation);
        arch = f;
    }

    if (arch == NULL)
    {
        ERROR_LOG("unsupported GPU: architecture=0x%x implementation=0x%x. "
                  "This build has backends for: %s",
                  (unsigned)architecture, (unsigned)implementation,
                  mc_arch_registry_names());
        return -1;
    }

    if (verify_classes(ctl_fd, h_client, h_device, arch) != 0)
        return -1;

    DEBUG_LOG("architecture backend: %s", arch->name);
    *out_arch = arch;
    return 0;
}
