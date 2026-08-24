# Hopper compute launch in libmc

libmc can submit a small Hopper kernel without `libcuda`. This document keeps
the end-to-end chain, operational constraints, and experimental evidence in
one place. The exact QMD fields and method words are documented beside their
builders in `mc_compute_qmd.{c,h}` and `mc_compute_demo.c`.

## What the path proves

The public demos establish two increasingly strong claims:

1. `mc_compute_doorbell_kernel()` submits HOPPER_COMPUTE_A work and a single SM
   thread writes a token to a mapped scratch cell.
2. `mc_memcpy(..., MC_XFER_SM)` launches `sm_owner`, which authors another
   channel's copy-engine submission: method stream, GPFIFO entry, USERD
   `GP_PUT`, and BAR1 doorbell.

The second path does not make the SM perform the copy. The SM is the producer
of CE commands; PBDMA fetches them and the copy engine moves the bytes.

## Execution chain

```text
host API
  -> compute-channel pushbuffer
  -> GPFIFO / USERD / VF doorbell
  -> PBDMA fetches HOPPER_COMPUTE_A methods
  -> scheduler launches one Hopper thread from a QMD
  -> thread writes a token, or authors a victim CE submission
  -> victim PBDMA and CE execute the copy
  -> host observes a release semaphore
```

RM creates the compute channel and schedules it. The userspace library builds
the QMD, constant buffer, pushbuffer, and GPFIFO entries. GPU scheduling and
execution remain hardware/firmware responsibilities.

## Descriptor ground truth

The implementation uses Hopper QMD V04. Its field locations are derived from
Mesa NVK's generated QMD definitions and checked against NVIDIA class headers
where available. `mc_compute_qmd.c` centralizes the multiword bit setters and
records why each nonzero field is required.

The embedded kernels deliberately use a very small ABI:

- one grid, one CTA, and one thread;
- a fixed QMD size and a fixed constant-buffer layout;
- committed `sm_90` cubin bytes for normal builds;
- regeneration through `make sm_owner` when CUDA tools are available.

The committed image lets a normal build avoid a CUDA-toolkit dependency. The
Makefile inspects regenerated SASS and rejects code generation that weakens the
system-scope stores used for publication.

## Channel setup and launch

Compute-channel initialization performs one-time cache invalidation and shader
memory-window programming. A launch then:

1. fills CB0 with the kernel arguments;
2. builds a QMD for the embedded program and its mapped data;
3. emits `SET_OBJECT` plus `SEND_PCAS_A`/`SEND_SIGNALING_PCAS2_B` methods;
4. publishes the GPFIFO entry and rings the compute channel;
5. polls a host-visible completion semaphore.

The source comments identify every field and method. Keeping a second table of
the same bit positions here made the old document drift whenever the builder
changed, so those tables are intentionally not duplicated.

## SM-authored copy submission

`sm_owner` receives GPU VAs for a carrier channel's state. One thread writes a
16-dword copy stream, a GPFIFO entry, USERD `GP_PUT`, and finally the BAR1
doorbell. Publication uses system-scope stores and an explicit system fence so
PBDMA cannot observe the doorbell before the command data.

The sysmem carrier places submission state in host memory. The FB carrier puts
the pushbuffer, GPFIFO, and USERD in HBM so the SM's setup writes remain on the
GPU memory fabric; only the final doorbell is MMIO. Completion remains a
sysmem semaphore polled by the host.

The exact store sequence is canonical in `mc/kernels/sm_owner.cu`. Its SASS
validation belongs in the Makefile because compiler lowering, not prose, is
the property that must be enforced.

## Diagnostic evidence

Bring-up produced three useful failure signatures:

- **Xid 31 / scheduler fault near VA 0:** copying a libcuda QMD template also
  copied hidden pointer fields. Building a minimal QMD from named fields
  removed those references.
- **Xid 13 / SM warp exception:** the original shader shared-memory window
  collided with the selected program layout. Programming the window used by
  the current builder removed the fault.
- **Kernel completed but the CPU read its sentinel:** a cached CPU line could
  obscure the GPU write in the scratch experiment. Invalidating the line
  before verification made the observation reliable.

These are experimental observations, not general definitions of those Xids.
The current source comments contain the exact mechanism needed by each fix;
the research [evidence log][findings] preserves broader measurements and
falsified hypotheses.

## Operational constraints

- Hopper/H100 and the pinned 610.43.02 RM/UVM ABI are the supported target.
- Tests need root access to `/dev/nvidia0`, `/dev/nvidiactl`, and
  `/dev/nvidia-uvm`.
- Official modules need no doorbell configuration. If the instrumented
  `libcuda-research` modules are loaded, disable interception before tests that
  issue a physical doorbell write.
- `MC_XFER_SM` requires both allocations in the same carrier VA space.
- Submission is synchronous and returns `MC_ETIMEOUT` when completion does not
  arrive within the library timeout.
- GPU Xids or channel faults must be checked in the kernel log; a timeout alone
  does not attribute the failure.

## Reproduce

```sh
make libmc mc-all test-unit
sudo ./bin/mc_compute_dbell_demo
sudo ./bin/mc_compute_dbell_chain_demo --size 8M
sudo ./bin/mc_sm_owner_demo --size 8M --iters 4
```

Run the repository matrix with:

```sh
sudo python3 tools/run_mc_tests.py --quick
```

After a failure, retain the exact program output and matching Xid lines before
changing the QMD or ordering rules.

## Source navigation

- `mc/mc_compute_qmd.{c,h}` — QMD V04 and CB0 construction.
- `mc/mc_compute_demo.c` — compute-channel setup and host-authored launch.
- `mc/mc_sm_owner.c` — host glue for the SM-authored CE path.
- `mc/kernels/sm_owner.cu` — GPU-side command publication.
- `tests/mc/mc_compute_dbell_demo.c` — scratch-write proof.
- `tests/mc/mc_compute_dbell_chain_demo.c` — SM-to-doorbell chain.
- `tests/mc/mc_sm_owner_demo.c` — end-to-end SM-authored copy.
- [`mc architecture`](mc_architecture.md) — context, VA spaces, and channel roles.
- [GPFIFO reference][gpfifo] — bit-exact entries, methods, USERD, and doorbell.

[findings]: https://github.com/maxime-peim/libcuda-research/blob/main/docs/findings.md
[gpfifo]: https://github.com/maxime-peim/libcuda-research/blob/main/docs/gpfifo_pushbuffer_reference.md
