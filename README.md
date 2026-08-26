# libmc

`libmc` is a small CUDA-runtime-like library built directly on NVIDIA's raw
RM and UVM interfaces. It does not link against `libcuda` or `libcudart`.

The project grew out of the tracing and reverse-engineering work in
[`libcuda-research`](https://github.com/maxime-peim/libcuda-research). This
repository contains the reusable library, its tests, and the exact NVIDIA
source dependency needed to compile it; the research repository remains the
home of the instrumented driver, trace tooling, captures, and protocol notes.

## Compatibility

This revision targets NVIDIA driver **610.43.02** and Hopper/H100. Private
RM/UVM interfaces are not stable APIs, so each `libmc` revision pins the
matching official `open-gpu-kernel-modules` source as a submodule. At runtime,
`mc_init()` verifies the loaded driver version before allocating RM objects and
returns `MC_EVERSION` on a mismatch.

## Build

```sh
git clone --recurse-submodules --shallow-submodules \
  https://github.com/maxime-peim/libmc.git
cd libmc
make libmc mc-all test-unit
```

The normal build uses the committed Hopper kernel bytes and does not require
the CUDA toolkit. `make sm_owner` regenerates those bytes and requires `nvcc`
and `cuobjdump`.

The host-only version-contract tests run without a GPU. Running the integration
demos requires root access to `/dev/nvidia0`, `/dev/nvidiactl`, and
`/dev/nvidia-uvm`, plus the matching 610.43.02 kernel modules. For example:

```sh
sudo ./bin/mc_demo
```

## Layout

- `mc/include/mc/mc.h` — the public header; the only one consumers include
- `mc/` — library implementation, grouped by layer: `core/` (lifecycle and
  VA-space policy), `driver/` (RM and UVM ioctl surfaces), `hal/` (method
  streams and submission), `arch/<name>/` (per-architecture code),
  `experimental/` (research paths), `internal/` (private headers),
  `kernels/` (device-side sources)
- `tests/mc/` — integration demos and benchmarks
- `tests/unit/` — host-only ABI and selection tests
- `tools/run_mc_tests.py` — integration-test matrix runner
- `docs/` — concise architecture and Hopper compute-launch guides
- `third_party/open-gpu-kernel-modules/` — pinned NVIDIA source submodule

Start with [`mc/README.md`](mc/README.md) for the API and constraints. The
implementation is intentionally experimental: it writes GPU command streams
and doorbells directly and has only been exercised on H100 PCIe with Linux
6.8.

## License

The `libmc` sources are MIT licensed. The NVIDIA submodule retains NVIDIA's
original license terms. See [`NOTICE`](NOTICE).
