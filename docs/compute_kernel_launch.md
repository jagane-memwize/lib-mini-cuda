# Compute launch in libmc

libmc can submit a small kernel without `libcuda`. This document keeps the
end-to-end chain, operational constraints, and experimental evidence in one
place. The exact QMD fields and method words are documented beside their
builders in `arch/<name>/qmd_v0*.{c,h}` and `experimental/compute.c`.

## Two claims, with different reach

The public demos establish two increasingly strong claims, and they do **not**
apply to the same hardware:

1. `mc_compute_doorbell_kernel()` submits compute work and a single SM thread
   writes a token to a mapped scratch cell. This is `MC_FEAT_COMPUTE_LAUNCH`,
   available on **Hopper, Ampere and Ada**.
2. `mc_memcpy(..., MC_XFER_SM)` launches `sm_owner`, which authors another
   channel's copy-engine submission: method stream, GPFIFO entry, USERD
   `GP_PUT`, and BAR1 doorbell. This is `MC_FEAT_XFER_SM`, and it is
   **Hopper-only**.

The second is not a gap waiting to be closed. Authoring a submission ends in
ringing a doorbell, which needs a BAR1-mapped usermode aperture to install a
PTE against. `AMPERE_USERMODE_A` is `RS_NONE` — it takes no allocation
parameters, and the `bBar1Mapping` field that selects a BAR1 window exists only
in `NV_HOPPER_USERMODE_A_PARAMS`. Ampere and Ada have no such aperture on any
driver, so they have no doorbell PTE, no FB carrier, and nothing SM-authored.

Note also what claim 2 does *not* say. The SM does not perform the copy. It is
the producer of CE commands; PBDMA fetches them and the copy engine moves the
bytes.

Query rather than assume:

```c
if (mc_feature_supported(ctx, MC_FEAT_XFER_SM))
    mc_memcpy(ctx, dst, src, n, MC_XFER_SM);
```

## Execution chain

```text
host API
  -> compute-channel pushbuffer
  -> GPFIFO / USERD / VF doorbell
  -> PBDMA fetches the architecture's compute-class methods
  -> scheduler launches one thread from a QMD
  -> thread writes a token, or authors a victim CE submission (Hopper)
  -> victim PBDMA and CE execute the copy
  -> host observes a release semaphore
```

RM creates the compute channel and schedules it. The userspace library builds
the QMD, constant buffer, pushbuffer, and GPFIFO entries. GPU scheduling and
execution remain hardware/firmware responsibilities.

## Descriptor ground truth

The QMD version belongs to the architecture, and the versions differ
structurally rather than cosmetically:

| | QMD V03 (Ampere, Ada) | QMD V04 (Hopper) |
|---|---|---|
| Size | 256 bytes (64 dwords) | 384 bytes (96 dwords) |
| `SM_GLOBAL_CACHING_ENABLE` | set at init | not set |
| Constant-buffer address | stored unshifted | stored `>>6` |
| Register-count field | `REGISTER_COUNT_V` | `REGISTER_COUNT` |
| `QMD_TYPE` | field does not exist | set to `GRID_CTA` |

Nothing is where it was between the two: the constant-buffer address moves from
bit 1024 to 1536, the program address from 1536 to 1216, and
`CONSTANT_BUFFER_VALID(i)` from stride 1 at bit 640 to stride 4 at bit 416.
Treating one as the other with fields shuffled produces a QMD that SKED
dereferences into the wrong memory — an Xid, not an error return.

**Provenance.** No QMD header past Kepler's `cla0c0qmd.h` ships in the pinned
`open-gpu-kernel-modules` tree, so neither builder can cite it. Both follow
Mesa NVK — `src/nouveau/compiler/nak/qmd.rs` for the recipe (`Qmd3_0`,
`Qmd4_0`, and the `qmd_impl_*` macros) and the `clcXc0qmd.h` headers Mesa
publishes for the bit ranges. The `NVC6C0_QMDV03_00_*` / `NVCBC0_QMDV04_00_*`
macro names are carried in comments purely so the two can be read side by side.
Because that is a transcription rather than a pinned dependency,
`tests/unit/mc_qmd_test.c` pins the result with golden dwords: it compares
every non-zero dword of a QMD built from distinctive inputs against a frozen
table, independent of the builders' own bit-range macros, so a moved field or
one field clobbering another fails on the host.

**Constant-bank offsets are a separate axis.** Where kernel arguments sit in
CB0 is a property of the compiled SASS, not of the QMD version — nvcc places
them at `0x210` on sm_90 and `0x160` on sm_80. They are descriptor data
(`mc_compute_ops_t.cb0_*`) and `hal/cbuf.c` fills CB0 from them, since the
procedure is identical everywhere and only the offsets differ. Read a new
architecture's out of `cuobjdump --dump-sass`; never carry another's over.
sm_89 happens to match sm_80 for these kernels, which was checked rather than
assumed.

The embedded kernels deliberately use a very small ABI:

- one grid, one CTA, and one thread;
- a fixed QMD size and constant-buffer layout per architecture;
- committed cubin bytes per architecture, so a normal build needs no CUDA
  toolkit;
- regeneration through the Makefile when CUDA tools are available.

The Makefile inspects regenerated SASS and rejects code generation that weakens
the system-scope stores used for publication. Those assertions are per
architecture, so a new backend declares what its own disassembly must contain.

## Channel setup and launch

Compute-channel initialization performs one-time cache invalidation and shader
memory-window programming. A launch then:

1. fills CB0 with the kernel arguments, at the architecture's offsets;
2. builds a QMD for the embedded program and its mapped data;
3. emits `SET_OBJECT` plus `SEND_PCAS_A`/`SEND_SIGNALING_PCAS2_B` methods;
4. publishes the GPFIFO entry and rings the compute channel;
5. polls a host-visible completion semaphore.

The compute *class* and the compute *method dialect* are independent axes.
Hopper allocates `HOPPER_COMPUTE_A` (`0xCBC0`) but is driven with `NVC7C0_*`
methods, and `0xC7C0` is `AMPERE_COMPUTE_B` — it inherits the Ampere-B dialect.
The descriptor models the two separately; never derive one from the other.

The source comments identify every field and method. Keeping a second table of
the same bit positions here made the old document drift whenever the builder
changed, so those tables are intentionally not duplicated. The V03/V04 table
above is the exception: it records *differences between versions*, which no
single builder's comments can state.

## SM-authored copy submission (Hopper)

`sm_owner` receives GPU VAs for a carrier channel's state. One thread writes a
16-dword copy stream, a GPFIFO entry, USERD `GP_PUT`, and finally the BAR1
doorbell. Publication uses system-scope stores and an explicit system fence so
PBDMA cannot observe the doorbell before the command data.

The sysmem carrier places submission state in host memory. The FB carrier puts
the pushbuffer, GPFIFO, and USERD in HBM so the SM's setup writes remain on the
GPU memory fabric; only the final doorbell is MMIO. Completion remains a sysmem
semaphore polled by the host.

The exact store sequence is canonical in `mc/kernels/sm_owner.cu`. Its SASS
validation belongs in the Makefile because compiler lowering, not prose, is the
property that must be enforced.

One arch assumption still lives here: `experimental/sm_owner.c`'s `patch_cb0`
hardcodes `0x210` for the kernel's parameter slot. That is correct for sm_90
and unreachable elsewhere, since this path needs the BAR1 aperture — but a
BAR1-capable non-Hopper backend will need those offsets moved into the compute
ops beside the store-dword kernel's.

## Diagnostic evidence

Bring-up produced four useful failure signatures:

- **Xid 31 / scheduler fault near VA 0:** copying a libcuda QMD template also
  copied hidden pointer fields. Building a minimal QMD from named fields
  removed those references.
- **Xid 13 / SM warp exception:** the original shader shared-memory window
  collided with the selected program layout. Programming the window used by the
  current builder removed the fault.
- **Kernel completed but the CPU read its sentinel:** a cached CPU line could
  obscure the GPU write in the scratch experiment. Invalidating the line before
  verification made the observation reliable.
- **`NV_ERR_INVALID_ADDRESS` from channel allocation on Ampere:** unrelated to
  compute, but it gated reaching it. A channel's USERD must fit the runlist
  entry's address field, which is 8 bits wide on GA100 against 20 on GH100, so
  USERD must live in vidmem there. See the architecture guide.

These are experimental observations, not general definitions of those Xids. The
current source comments contain the exact mechanism needed by each fix; the
research [evidence log][findings] preserves broader measurements and falsified
hypotheses.

## Operational constraints

- The pinned 610.43.02 RM/UVM ABI is the supported target. Compute launch works
  on Hopper, Ampere and Ada; `MC_XFER_SM` and the doorbell paths are
  Hopper-only.
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
sudo ./bin/mc_compute_dbell_demo                       # any supported GPU
sudo ./bin/mc_compute_dbell_chain_demo --size 8M       # Hopper only
sudo ./bin/mc_sm_owner_demo --size 8M --iters 4        # Hopper only
```

The two Hopper-only demos decline cleanly elsewhere rather than failing
obscurely. Run the repository matrix with:

```sh
sudo python3 tools/run_mc_tests.py --quick
```

It reports cells needing an absent feature as `SKIP`, so fewer executed cells
on Ampere or Ada is the expected result; the failure count should be zero
everywhere.

After a failure, retain the exact program output and matching Xid lines before
changing the QMD or ordering rules.

## Source navigation

- `mc/arch/hopper/qmd_v04.{c,h}` — QMD V04 (Hopper).
- `mc/arch/ampere/qmd_v03.{c,h}` — QMD V03 (Ampere, shared by Ada).
- `mc/hal/cbuf.c` — constant buffer 0, from the descriptor's offsets.
- `mc/experimental/compute.c` — compute-channel setup and host-authored launch.
- `mc/experimental/sm_owner.c` — host glue for the SM-authored CE path.
- `mc/kernels/sm_owner.cu` — GPU-side command publication.
- `mc/kernels/store_dword.cu` — the minimal one-store kernel; the payload
  behind both the doorbell experiment and every compute-launch proof.
- `tests/unit/mc_qmd_test.c` — golden QMD bits for both versions, no GPU needed.
- `tests/mc/mc_compute_dbell_demo.c` — scratch-write proof.
- `tests/mc/mc_compute_dbell_chain_demo.c` — SM-to-doorbell chain.
- `tests/mc/mc_sm_owner_demo.c` — end-to-end SM-authored copy.
- [`mc architecture`](mc_architecture.md) — context, VA spaces, and channel roles.
- [GPFIFO reference][gpfifo] — bit-exact entries, methods, USERD, and doorbell.

[findings]: https://github.com/maxime-peim/libcuda-research/blob/main/docs/findings.md
[gpfifo]: https://github.com/maxime-peim/libcuda-research/blob/main/docs/gpfifo_pushbuffer_reference.md
