# libmc

`libmc` is a small CUDA-runtime-like library built directly on NVIDIA's raw
RM and UVM interfaces. It does not link against `libcuda` or `libcudart`.

The project grew out of the tracing and reverse-engineering work in
[`libcuda-research`](https://github.com/maxime-peim/libcuda-research). This
repository contains the reusable library, its tests, and the exact NVIDIA
source dependency needed to compile it; the research repository remains the
home of the instrumented driver, trace tooling, captures, and protocol notes.

## Compatibility

This revision targets NVIDIA driver **610.43.02**, with backends for
**Hopper**, **Ampere** and **Ada**. Private RM/UVM interfaces are not stable
APIs, so each `libmc` revision pins the matching official
`open-gpu-kernel-modules` source as a submodule. At runtime, `mc_init()`
verifies the loaded driver version before allocating RM objects and returns
`MC_EVERSION` on a mismatch, then identifies the GPU and selects its backend,
returning `MC_EARCH` for a chip it does not support.

Architectures differ in what they can offer, and the difference is in the
silicon rather than in the library. Ampere and Ada have no BAR1 usermode
aperture, so they cannot install a doorbell PTE, so the FB carrier VA space and
every SM-authored path are permanently unavailable there. Ask rather than
assume:

```c
if (mc_feature_supported(ctx, MC_FEAT_XFER_SM))
    mc_memcpy(ctx, dst, src, n, MC_XFER_SM);
```

## Build

```sh
git clone --recurse-submodules --shallow-submodules \
  https://github.com/maxime-peim/libmc.git
cd libmc
make libmc mc-all test-unit
```

The normal build uses the committed per-architecture kernel bytes and does not
require the CUDA toolkit. Regenerating them needs `nvcc` and `cuobjdump`; SASS
does not port across SM versions, so each architecture compiles its own.

The host-only tests run without a GPU, and cover more than the version
contract: chip detection, feature derivation, method streams, QMD layouts and
the USERD address-width rules are all checked on the host. Running the
integration demos requires root access to `/dev/nvidia0`, `/dev/nvidiactl`, and
`/dev/nvidia-uvm`, plus the matching 610.43.02 kernel modules. For example:

```sh
sudo ./bin/mc_demo
```

## Layout

- `mc/include/mc/mc.h` — the public header; the only one consumers include
- `mc/` — library implementation, grouped by layer: `core/` (lifecycle and
  VA-space policy), `driver/` (RM and UVM ioctl surfaces), `hal/` (method
  streams, submission, and the architecture descriptor), `arch/<name>/`
  (per-architecture code — `hopper`, `ampere` (GA100), `ampere_ga10x`, `ada`), `experimental/`
  (research paths), `internal/` (private headers), `kernels/` (device-side
  sources)
- `tests/mc/` — integration demos and benchmarks
- `tests/unit/` — host-only ABI and selection tests
- `tools/run_mc_tests.py` — integration-test matrix runner
- `docs/` — concise architecture and compute-launch guides
- `third_party/open-gpu-kernel-modules/` — pinned NVIDIA source submodule

Start with [`mc/README.md`](mc/README.md) for the API and constraints. The
implementation is intentionally experimental: it writes GPU command streams and
doorbells directly. It has been exercised on H100 SXM5, A100 SXM4 and L4, all
on driver 610.43.02 under Linux 5.15. Those are not interchangeable — the H100
evidence is SXM, not the PCIe card, and each board's PCIe generation sets a
different transfer ceiling.

## License

The `libmc` sources are MIT licensed. The NVIDIA submodule retains NVIDIA's
original license terms. See [`NOTICE`](NOTICE).
