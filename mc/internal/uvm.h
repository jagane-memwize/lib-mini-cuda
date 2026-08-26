/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * internal/uvm.h — UVM ioctl wrappers implemented by driver/uvm.c.
 *
 * Split out of the former mc_internal.h.  UVM-managed memory follows the
 * UVM external-range path; there is deliberately no NV04_MAP_MEMORY_DMA
 * route here.
 */
#ifndef MC_INTERNAL_UVM_H
#define MC_INTERNAL_UVM_H

#include "nvtypes.h"
#include "nvCpuUuid.h"      /* NV_UUID_LEN */

int      uvm_setup(int ctl_fd, int dev_fd, NvHandle h_client,
                   NvHandle h_subdevice, NvHandle h_vaspace,
                   NvU8 out_inst_uuid[NV_UUID_LEN], int *out_uvm_fd);
int      uvm_register_channel(int uvm_fd, int dev_fd, NvHandle h_client,
                              const NvU8 inst_uuid[NV_UUID_LEN],
                              NvHandle h_channel);
NvU64    uvm_map_buffer(int uvm_fd, int dev_fd, NvHandle h_client,
                        const NvU8 inst_uuid[NV_UUID_LEN], NvHandle h_memory,
                        NvU64 size, const char *label);
NvU64    uvm_map_buffer_at(int uvm_fd, int dev_fd, NvHandle h_client,
                           const NvU8 inst_uuid[NV_UUID_LEN], NvHandle h_memory,
                           void *want_va, NvU64 size, const char *label);
int      uvm_map_buffer_range_at(int uvm_fd, int dev_fd, NvHandle h_client,
                                 const NvU8 inst_uuid[NV_UUID_LEN],
                                 NvHandle h_memory, NvU64 base, NvU64 size,
                                 NvU64 offset, const char *label);
void     uvm_free_range(int uvm_fd, NvU64 base, const char *label);
void     uvm_unregister_channel(int uvm_fd, NvHandle h_client,
                                NvHandle h_channel);
void     uvm_unmap_buffer(int uvm_fd, const NvU8 inst_uuid[NV_UUID_LEN],
                          NvU64 gpu_va, NvU64 size, const char *label);
void     uvm_unregister_gpu_vaspace(int uvm_fd,
                                    const NvU8 inst_uuid[NV_UUID_LEN]);
void     uvm_unregister_gpu(int uvm_fd, const NvU8 inst_uuid[NV_UUID_LEN]);

#endif /* MC_INTERNAL_UVM_H */
