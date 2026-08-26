/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * core/feature.c — what this GPU, in this build, can actually do.
 *
 * Availability is DERIVED from the architecture descriptor rather than
 * declared per backend.  A backend that listed its own feature set would be a
 * second place to get it wrong: the descriptor already says whether the class
 * has a BAR1 usermode aperture and whether there is a compute builder, and
 * every feature follows from facts like those.  Adding a backend therefore
 * cannot forget to update a feature list, because there is no feature list.
 *
 * Dependencies are declared once, in the table below, and closed transitively
 * -- MC_FEAT_XFER_SM needs compute, compute needs the sysmem carrier -- so a
 * feature never appears available while something it is built on is missing.
 */

#include <stdbool.h>
#include <stdint.h>

#include <mc/mc.h>

#include "hal/arch.h"
#include "internal/feature.h"
#include "internal/log.h"
#include "internal/types.h"

struct feature_info {
    const char *name;
    uint64_t    depends;     /* features that must also be available */
};

static const struct feature_info g_features[MC_FEAT_COUNT] = {
    [MC_FEAT_XFER_HOST]       = { "xfer_host",        0 },
    [MC_FEAT_HOST_REGISTER]   = { "host_register",    0 },
    [MC_FEAT_CARRIER_SYSMEM]  = { "carrier_sysmem",   0 },
    [MC_FEAT_CARRIER_FB]      = { "carrier_fb",       0 },
    [MC_FEAT_COMPUTE_LAUNCH]  = { "compute_launch",
                                  MC_FEAT_BIT(MC_FEAT_CARRIER_SYSMEM) },
    /* SM-authored submission runs a compute kernel that writes another
     * channel's doorbell, so it needs both compute and the doorbell path. */
    [MC_FEAT_XFER_SM]         = { "xfer_sm",
                                  MC_FEAT_BIT(MC_FEAT_COMPUTE_LAUNCH)
                                  | MC_FEAT_BIT(MC_FEAT_CE_DOORBELL_RING) },
    [MC_FEAT_CE_DOORBELL_RING]= { "ce_doorbell_ring",
                                  MC_FEAT_BIT(MC_FEAT_CARRIER_SYSMEM) },
};

const char *mc_feature_name(mc_feature_t f)
{
    if ((unsigned)f >= MC_FEAT_COUNT || g_features[f].name == NULL)
        return "invalid";
    return g_features[f].name;
}

/* What the hardware and the build can provide, before anything is requested. */
uint64_t mc_features_available(const mc_arch_desc_t *arch)
{
    uint64_t avail;
    unsigned pass, f;

    if (arch == NULL) return 0;

    /* Copies and host registration need nothing beyond a channel, which every
     * supported architecture has. */
    avail = MC_FEAT_BIT(MC_FEAT_XFER_HOST)
          | MC_FEAT_BIT(MC_FEAT_HOST_REGISTER)
          | MC_FEAT_BIT(MC_FEAT_CARRIER_SYSMEM);

    /* Everything that rings a doorbell from the GPU needs a BAR1-mapped
     * usermode aperture to install a PTE against.  Ampere and Ada have no
     * such aperture -- the class takes no parameters and there is no
     * bBar1Mapping concept -- so these are permanently unavailable there,
     * regardless of driver. */
    if (arch->cap.usermode_bar1_capable)
        avail |= MC_FEAT_BIT(MC_FEAT_CE_DOORBELL_RING)
               | MC_FEAT_BIT(MC_FEAT_CARRIER_FB);

    /* Compute needs a QMD builder for this architecture. */
    if (arch->compute != NULL)
        avail |= MC_FEAT_BIT(MC_FEAT_COMPUTE_LAUNCH);

#if MC_ENABLE_EXPERIMENTAL
    /* SM-authored submission runs a specific compiled kernel.  SASS does not
     * port across SM versions, so a backend without a cubin for it cannot
     * offer the feature no matter what else it supports -- and a backend
     * being written before its cubin exists is a normal intermediate state,
     * not an error. */
    if (arch->compute != NULL && arch->kernels.sm_owner.bytes != NULL
        && arch->kernels.sm_owner.len != 0)
        avail |= MC_FEAT_BIT(MC_FEAT_XFER_SM);
#endif

    /* Likewise the store-dword kernel.  A QMD builder with no kernel to launch
     * cannot be exercised, so compute_launch means "can launch a kernel", and
     * that needs the bytes.  Note this blob is NOT about doorbells despite what
     * its old name suggested: every architecture with compute carries it,
     * including those that can never ring one. */
    if (arch->kernels.store_dword.bytes == NULL || arch->kernels.store_dword.len == 0)
        avail &= ~MC_FEAT_BIT(MC_FEAT_COMPUTE_LAUNCH);

    /* Close over dependencies.  Iterating to a fixed point rather than
     * assuming the table is topologically ordered means the table can be
     * written in whatever order reads best. */
    for (pass = 0; pass < MC_FEAT_COUNT; pass++)
    {
        uint64_t before = avail;
        for (f = 0; f < MC_FEAT_COUNT; f++)
            if ((avail & MC_FEAT_BIT(f))
                && (g_features[f].depends & ~avail) != 0)
                avail &= ~MC_FEAT_BIT(f);
        if (avail == before) break;
    }
    return avail;
}

bool mc_feature_supported(const mc_ctx_t *ctx, mc_feature_t f)
{
    if (ctx == NULL || (unsigned)f >= MC_FEAT_COUNT) return false;
    return (ctx->features & MC_FEAT_BIT(f)) != 0;
}

void mc_features_log(uint64_t active, uint64_t available)
{
    unsigned f;
    if (!mc_debug()) return;
    for (f = 0; f < MC_FEAT_COUNT; f++)
    {
        const char *state = (active & MC_FEAT_BIT(f))    ? "on"
                          : (available & MC_FEAT_BIT(f)) ? "available, not requested"
                                                         : "unavailable";
        DEBUG_LOG("feature %-18s %s", mc_feature_name((mc_feature_t)f), state);
    }
}
