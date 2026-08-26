# libmc architecture

This document is a map of the library, not a second copy of its implementation.
The exact ioctl fields, class IDs, method words, ordering rules, and failure
rationale live beside the code that depends on them.

`libmc` talks directly to NVIDIA's private RM and UVM interfaces. It does not
link to `libcuda` or `libcudart`. The current revision is tied to driver
610.43.02 and has been exercised on Hopper/H100 PCIe.

## Layer boundaries

| Layer | Owner | libmc interaction |
|---|---|---|
| Public API | `mc/include/mc/mc.h` | Contexts, allocations, VA lookup, and synchronous copies |
| RM client | `mc/driver/rm.c` | Device, subdevice, VA-space, TSG, channel, engine, memory, mapping, and control objects |
| UVM client | `mc/driver/uvm.c` | GPU and VA-space registration, external ranges, buffer mappings, and the primary channel |
| Submission | `mc/hal/submit.c` | Copy methods, GPFIFO entries, USERD updates, doorbells, semaphores, and timeouts |
| VA-space policy | `mc/core/vaspace.c` | Paper-F1 identity pool and the two carrier VA spaces |
| Compute | `mc/arch/hopper/qmd_v04.*`, `mc/experimental/compute.c` | Hopper QMD construction and compute-channel submission |
| SM-authored copy | `mc/experimental/sm_owner.c`, `mc/kernels/` | An SM thread authors work for a copy-engine channel |

RM owns object creation and engine scheduling. UVM owns the external-range and
primary-channel registration path. PBDMA fetches GPFIFO entries and method
streams; a selected copy engine executes transfers. Completion is observed
through a system-memory semaphore. These boundaries are kept distinct in the
source and during error attribution.

## Context initialization

`mc_init()` performs the following high-level sequence:

1. Verify that the loaded RM version exactly matches the pinned source.
2. Open `/dev/nvidiactl`, `/dev/nvidia0`, and `/dev/nvidia-uvm`.
3. Allocate the RM root, device, subdevice, USERMODE aperture, and VA spaces.
4. Query copy-engine capabilities and select a present, sysmem-capable LCE
   whose runlist is not shared with GR.
5. Create the channel groups and their GPFIFO, USERD, pushbuffer, semaphore,
   and engine objects.
6. Register the UVM-owned VA space and its channel with UVM.
7. Create the sysmem and FB carrier channels, including compute channels used
   by the SM-authored paths.

The source wrappers encode the fields that must agree across the TSG, channel,
and engine allocations. In particular, the selected copy-engine type flows
through all three objects. `mc_init()` returns an error and unwinds partial
state rather than terminating the caller.

## VA spaces and channel roles

| Public selector | Addressing | Channel-resource placement | Intended use |
|---|---|---|---|
| `MC_VAS_UVM` | CPU VA equals GPU VA for host allocations | UVM-managed setup | Ordinary host-authored H2D/D2H |
| `MC_VAS_SYSMEM_CARRIER` | CPU and GPU VAs may differ | Pushbuffer, GPFIFO, USERD, and semaphore in sysmem | Host- or SM-authored copies with a carrier VAS |
| `MC_VAS_FB_CARRIER` | CPU and GPU VAs may differ | Hot submission state in FB; completion semaphore in sysmem | Remove PCIe writes from the SM-authored setup path except the doorbell |

The UVM identity mapping is deliberate. libcuda reserves a process VA window,
maps RM memory into it, and registers the same address as the UVM external
range. libmc mirrors that shape. The experiment that established this
requirement is recorded under [Paper-F1 VA identity][paper-f1].

Carrier mappings use one `NV50_MEMORY_VIRTUAL` object per source allocation.
RM chooses the GPU VA; `mc_gpu_va()` exposes it for a CPU pointer. Pointers from
different VA spaces cannot be mixed in one `mc_memcpy()` call.

## Submission path

A host-authored synchronous copy follows this chain:

1. Validate both pointers, sizes, VA-space membership, and direction.
2. Write the copy-engine method stream into the channel pushbuffer.
3. Publish a GPFIFO entry that points at that stream.
4. Update USERD `GP_PUT`, order the writes, and ring the VF doorbell.
5. Wait for the copy engine's release semaphore or return `MC_ETIMEOUT`.

The bit-exact GPFIFO, method-header, USERD, and doorbell formats are documented
in the research repository's [GPFIFO reference][gpfifo] and implemented with
the relevant SDK macros in `hal/submit.c`.

For `MC_XFER_SM`, a Hopper compute kernel writes the victim CE channel's
pushbuffer, GPFIFO entry, USERD value, and BAR1 doorbell. The host still waits
for completion. The end-to-end compute path and the evidence behind its
ordering rules are summarized in [the compute-launch guide](compute_kernel_launch.md).

## Failure boundaries

- A driver/source mismatch fails before RM objects are allocated.
- A missing suitable copy engine fails during initialization.
- Cross-VA-space pointers, unknown pointers, zero sizes, and transfers larger
  than the CE method field return `MC_EINVAL`.
- Submission completion is bounded by a two-second timeout.
- Fatal GPU faults remain channel/context failures; libmc does not service or
  replay UVM or RM faults.
- The public API is single-threaded and supports one context per process.

`mc_fini()` disables and drains channels before releasing mappings, memory,
engine objects, VA spaces, device objects, and file descriptors. Cleanup is
best-effort so one release failure does not strand the rest of the object tree.

## Evidence and source of truth

The following belong in documentation because comments alone cannot preserve
their experimental provenance:

- trace counts, timings, and capture identities;
- failure rates before and after a change;
- hypotheses that were tested and falsified;
- hardware- and driver-version scope;
- questions that remain unresolved.

Those results live in the research repository's compact [evidence log][findings].
Implementation facts stay in source comments and tests. The pinned NVIDIA
submodule is the ABI ground truth for RM/UVM declarations; it is a dependency,
not a modified copy of the research driver.

## Reproducing the supported path

```sh
git submodule update --init --depth 1
make libmc mc-all test-unit
sudo python3 tools/run_mc_tests.py --quick
```

The integration command requires Hopper/H100, driver 610.43.02, root access to
the NVIDIA device nodes, and loaded `nvidia` plus `nvidia-uvm` modules. Official
modules require no doorbell setting. When using the instrumented modules from
`libcuda-research`, disable their watchpoint interception before running an
SM-issued doorbell experiment.

[findings]: https://github.com/maxime-peim/libcuda-research/blob/main/docs/findings.md
[paper-f1]: https://github.com/maxime-peim/libcuda-research/blob/main/docs/findings.md#paper-f1-va-identity
[gpfifo]: https://github.com/maxime-peim/libcuda-research/blob/main/docs/gpfifo_pushbuffer_reference.md
