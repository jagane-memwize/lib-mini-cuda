# libmc architecture

This document is a map of the library, not a second copy of its implementation.
The exact ioctl fields, class IDs, method words, ordering rules, and failure
rationale live beside the code that depends on them.

`libmc` talks directly to NVIDIA's private RM and UVM interfaces. It does not
link to `libcuda` or `libcudart`. The current revision is tied to driver
610.43.02 and ships backends for Hopper, Ampere and Ada, exercised on H100
SXM5, A100 SXM4 and L4 respectively.

## Layer boundaries

| Layer | Owner | libmc interaction |
|---|---|---|
| Public API | `mc/include/mc/mc.h` | Contexts, allocations, VA lookup, and synchronous copies |
| RM client | `mc/driver/rm.c` | Device, subdevice, VA-space, TSG, channel, engine, memory, mapping, and control objects |
| UVM client | `mc/driver/uvm.c` | GPU and VA-space registration, external ranges, buffer mappings, and the primary channel |
| Submission | `mc/hal/submit.c` | Copy methods, GPFIFO entries, USERD updates, doorbells, semaphores, and timeouts |
| VA-space policy | `mc/core/vaspace.c` | Paper-F1 identity pool and the two carrier VA spaces |
| Lifecycle | `mc/core/context.c`, `mc/core/channel.c` | Context init/fini and per-kind channel bring-up |
| Allocation | `mc/core/alloc.c`, `mc/core/memcpy.c` | Allocation table, allocators, and copy dispatch |
| Architecture | `mc/hal/arch.h`, `mc/hal/{detect,registry}.c`, `mc/arch/<name>/` | Chip detection, the per-architecture descriptor, and the behaviour hooks a descriptor cannot express as data |
| Features | `mc/core/feature.c` | What this GPU and build actually provide, derived from the descriptor |
| Compute | `mc/arch/<name>/qmd_v0*.c`, `mc/hal/cbuf.c`, `mc/experimental/compute.c` | QMD construction per version, constant-buffer filling, and compute-channel submission |
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
3. Allocate the RM root, device and subdevice, then identify the GPU and
   select its architecture backend, verifying the device exposes that
   backend's classes. An unsupported chip returns `MC_EARCH` before any
   architecture-specific object is allocated.
4. Allocate the USERMODE aperture and VA spaces.
5. Query copy-engine capabilities and select a present, sysmem-capable LCE
   whose runlist is not shared with GR.
6. Create the channel groups and their GPFIFO, USERD, pushbuffer, semaphore,
   and engine objects.
7. Register the UVM-owned VA space and its channel with UVM.
8. Derive which features this GPU and build can provide, intersect that with
   the caller's requested mask, and create only the VA spaces and channels
   those features need. `mc_init()` requests everything available;
   `mc_init_ex()` takes an explicit mask, and `MC_FEATURES_BASIC` brings up
   neither carrier VA space. On Ampere and Ada the FB carrier and every
   SM-authored channel are never created at all, because the silicon cannot
   support them.

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

For `MC_XFER_SM`, a compute kernel writes the victim CE channel's pushbuffer,
GPFIFO entry, USERD value, and BAR1 doorbell. The host still waits for
completion. This needs a BAR1 usermode aperture and so is Hopper-only; Ampere
and Ada report `MC_FEAT_XFER_SM` as unavailable. The end-to-end compute path
and the evidence behind its ordering rules are summarized in
[the compute-launch guide](compute_kernel_launch.md).

## Failure boundaries

- A driver/source mismatch fails before RM objects are allocated.
- An unsupported GPU architecture, or one whose device does not expose the
  backend's classes, fails with `MC_EARCH` before any architecture-specific
  object is allocated.
- A missing suitable copy engine fails during initialization.
- An operation whose feature this GPU or build does not provide returns
  `MC_EUNSUPPORTED` at the call, rather than failing inside submission.
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

The integration command requires a supported GPU, driver 610.43.02, root access
to the NVIDIA device nodes, and loaded `nvidia` plus `nvidia-uvm` modules.
Official modules require no doorbell setting. When using the instrumented
modules from `libcuda-research`, disable their watchpoint interception before
running an SM-issued doorbell experiment.

The matrix asks the library what the GPU provides and reports cells needing an
absent feature as `SKIP`. A smaller number of executed cells on Ampere or Ada
is the expected result, not a regression — compare the failure count, which
should be zero everywhere.

[findings]: https://github.com/maxime-peim/libcuda-research/blob/main/docs/findings.md
[paper-f1]: https://github.com/maxime-peim/libcuda-research/blob/main/docs/findings.md#paper-f1-va-identity
[gpfifo]: https://github.com/maxime-peim/libcuda-research/blob/main/docs/gpfifo_pushbuffer_reference.md
