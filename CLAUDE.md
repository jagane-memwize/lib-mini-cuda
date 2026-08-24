# libmc contributor guide

`libmc` is the standalone userspace library extracted from
[`libcuda-research`](https://github.com/maxime-peim/libcuda-research). This
repository owns the library, integration demos, unit tests, and its exact
driver-source dependency. The research repository owns the instrumented
driver, captures, trace tools, and protocol notes.

## Compatibility contract

- This revision targets NVIDIA driver 610.43.02 and Hopper/H100.
- `third_party/open-gpu-kernel-modules` is pinned to the matching official
  source. Do not silently build against another driver revision.
- Private RM and UVM interfaces are version-specific. A new supported driver
  version should update the pin, generated assumptions, tests, and release
  documentation together.

## Implementation invariants

- Use `NVOS64_PARAMETERS`, not the legacy NVOS21 form, for RM allocation.
- Build NVIDIA bit fields with the SDK `DRF_*` macros. Do not replace them with
  handwritten shifts and masks.
- Format `NvU64` values with `NvU64_fmtx`; use `ULL` for 64-bit constants.
- UVM-managed memory follows the UVM external-range path. Do not substitute a
  non-UVM `NV04_MAP_MEMORY_DMA` route.
- UVM host mappings obey the Paper-F1 invariant: CPU VA equals GPU VA. Preserve
  the anchored VA-pool design.
- `FERMI_VASPACE_A` used with UVM must be externally owned.
- Submission order is pushbuffer, GPFIFO entry, USERD GPPut, then doorbell.
  Preserve the existing fence discipline.
- The work-submit doorbell is at `+0x90` in the mapped USERMODE window.
- The CE channel deliberately selects a non-GRCE LCE and uses its COPY engine
  consistently for the TSG and channel. Do not reintroduce the historical RM
  engine-lookup patch as a prerequisite.
- Changes to `mc/kernels/sm_owner.cu` must retain the Makefile's generated-SASS
  assertions. Review the disassembly when store or fence code changes.
- Before adding an ioctl or changing a GPU command bit field, establish its
  value from the pinned driver headers or a checked-in research capture.

## Verification

Run the host build and unit gates with:

```sh
make libmc mc-all test-unit
```

The integration matrix (`sudo python3 tools/run_mc_tests.py`) requires an H100
host with the matching loaded driver. Official 610.43.02 modules are sufficient.
When using the instrumented `libcuda-research` modules for an `MC_XFER_SM` run,
load them with `nv_dbell_disable_intercept=1`.

Keep architecture claims linked to the evidence in `libcuda-research`; do not
present an experimental workaround as a current requirement after the code path
has changed.
