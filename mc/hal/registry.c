/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * hal/registry.c — the compiled-in backend table and lookups over it.
 *
 * The table is an explicit array rather than a linker section or a set of
 * constructors.  That costs one line per backend and buys three things: it is
 * greppable, it survives -fvisibility=hidden and --gc-sections, and the match
 * order is written down instead of being whatever order the linker chose.
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "hal/arch.h"
#include "hal/registry.h"

static const mc_arch_desc_t *const g_backends[] = {
    &mc_arch_hopper,
};
#define N_BACKENDS (sizeof(g_backends) / sizeof(g_backends[0]))

const mc_arch_desc_t *const *mc_arch_registry(size_t *out_count)
{
    if (out_count) *out_count = N_BACKENDS;
    return g_backends;
}

const mc_arch_desc_t *mc_arch_registry_lookup(NvU32 architecture,
                                              NvU32 implementation)
{
    size_t i;

    for (i = 0; i < N_BACKENDS; i++)
    {
        const mc_arch_desc_t *a = g_backends[i];

        /* Architecture first, always.  Implementation values repeat across
         * architectures -- GA100, GH100, AD100, GB100 and GB200 all report 0
         * -- so a match that considered implementation alone, or first, would
         * hand an H100 to the Ampere backend. */
        if (a->rm_architecture != architecture)
            continue;

        /* mask == 0 means "any implementation of this architecture". */
        if (a->rm_impl_mask != 0)
        {
            if (implementation >= 32)
                continue;                    /* unrepresentable in the mask */
            if ((a->rm_impl_mask & (1u << implementation)) == 0)
                continue;
        }
        return a;
    }
    return NULL;
}

const mc_arch_desc_t *mc_arch_registry_by_name(const char *name)
{
    size_t i;

    if (name == NULL) return NULL;
    for (i = 0; i < N_BACKENDS; i++)
        if (strcmp(g_backends[i]->name, name) == 0)
            return g_backends[i];
    return NULL;
}

const char *mc_arch_registry_names(void)
{
    static char buf[256];
    size_t      i;
    int         n = 0;

    buf[0] = '\0';
    for (i = 0; i < N_BACKENDS && n >= 0 && (size_t)n < sizeof(buf) - 1; i++)
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%s%s",
                      i ? ", " : "", g_backends[i]->name);
    return buf;
}
