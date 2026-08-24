# SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
# SPDX-License-Identifier: MIT
#
# libmc Makefile — builds the library and its example artefacts:
#
#   bin/mc_demo          — D2H transfer on top of libmc, no CUDA library.
#   lib/libmc.so         — the mc library itself (see mc/mc.h).
#
# New .c files dropped in tests/mc/ are picked up automatically.
#
# Quick start:
#
#   make                   — build libmc and every tests/mc/*.c binary.
#   make mc-all            — every tests/mc/*.c binary.
#   make mc_demo           — libmc demo (no CUDA toolchain needed).
#   make libmc             — library only.
#   make check-nvidia-source — validate the pinned driver-source dependency.
#   make test-unit         — host-only unit tests (no GPU required).
#   make help              — print this summary.
#   make clean             — remove every produced artefact.
#
# Override toolchain or flags from the command line:
#
#   make CC=clang mc_demo
#   make NVCC=/opt/cuda/bin/nvcc sm_owner
#   make CFLAGS='-O0 -g -fsanitize=address' mc_demo

# ── Toolchain ──────────────────────────────────────────────────────────────
CC        ?= gcc
NVCC      ?= $(shell command -v nvcc      2>/dev/null || echo /usr/local/cuda/bin/nvcc)
CUOBJDUMP ?= $(shell command -v cuobjdump 2>/dev/null || echo /usr/local/cuda/bin/cuobjdump)
REPO_ROOT := $(CURDIR)

include compat/nvidia.mk

# libmc consumes private RM/UVM ABI definitions from the exact official
# driver release it targets.  The submodule is the default; NVIDIA_SRC lets a
# developer reuse another checkout of that same release without weakening the
# version contract.
NVIDIA_SRC ?= $(REPO_ROOT)/third_party/open-gpu-kernel-modules
NVIDIA_VERSION_MK := $(NVIDIA_SRC)/version.mk

# ── Flags ──────────────────────────────────────────────────────────────────
# Warnings on, but allow unused helpers: the sources keep a few reference
# implementations around as documentation.
# libmc talks to the driver directly, so it needs the pinned driver's SDK and
# UVM headers.  MC_NVIDIA_DRIVER_VERSION is also compiled into the strict
# runtime compatibility check performed before any RM object is allocated.
CFLAGS       ?= -O2 -g -Wall -Wno-unused-function
MC_CPPFLAGS  := -DMC_NVIDIA_DRIVER_VERSION=\"$(MC_NVIDIA_VERSION)\"
SDK_INC     := -I$(NVIDIA_SRC)/src/common/sdk/nvidia/inc \
                -I$(NVIDIA_SRC)/src/common/sdk/nvidia/inc/class \
                -I$(NVIDIA_SRC)/src/common/sdk/nvidia/inc/alloc \
                -I$(NVIDIA_SRC)/src/nvidia/arch/nvalloc/unix/include \
                -I$(NVIDIA_SRC)/src/common/inc \
                -I$(NVIDIA_SRC)/src/nvidia/inc \
                -I$(NVIDIA_SRC)/kernel-open/nvidia-uvm \
                -I$(NVIDIA_SRC)/kernel-open/common/inc

# sm_owner kernel build flags.  -O2 is required for nvcc to fold our
# DRF-style header constants into immediate operands; -O0 leaves them
# as runtime register loads which still works but obscures the SASS.
# We pin sm_90 explicitly because Hopper-specific instructions (the
# .sys store scope) are sm_90+. -lineinfo helps cuobjdump --dump-sass
# annotate output but is harmless if the user has no debug interest.
NVCC_KERNEL_FLAGS ?= -arch=sm_90 --cubin -O2 -lineinfo -Imc

# ── Artefacts ──────────────────────────────────────────────────────────────
# Auto-discovered from source directories — add a file, it builds automatically.
# Auto-discover test programs.  Files ending in _helpers.c are
# shared-helper translation units (mc_test_helpers.c etc.), NOT
# stand-alone tests — exclude them from binary discovery and link
# them into every test instead.
MC_ALL_SRCS := $(wildcard tests/mc/*.c)
MC_HELPERS  := $(filter %_helpers.c,$(MC_ALL_SRCS))
MC_SRCS     := $(filter-out %_helpers.c,$(MC_ALL_SRCS))
MC_BINS     := $(patsubst tests/mc/%.c,bin/%,$(MC_SRCS))

LIBMC := lib/libmc.so
MC_LIB_SRCS := mc/mc_core.c mc/mc_rm.c mc/mc_uvm.c mc/mc_submit.c \
               mc/mc_vaspace.c mc/mc_compute_qmd.c \
               mc/mc_compute_demo.c mc/mc_sm_owner.c
UNIT_TEST_BINS := bin/mc_rm_version_test bin/mc_init_version_test

BINS := $(MC_BINS)
LIBS := $(LIBMC)

.DEFAULT_GOAL := all
.PHONY: all bins libs mc-all clean help libmc
.PHONY: check-nvidia-source test-unit
.PHONY: $(notdir $(BINS))

all: $(BINS) $(LIBS)
bins: $(BINS)
libs: $(LIBS)
mc-all: $(MC_BINS)

# Friendly aliases without the directory prefix (auto-generated).
$(foreach b,$(BINS),$(eval $(notdir $b): $b))

libmc: $(LIBMC)
test-unit: $(UNIT_TEST_BINS)
	@for test_bin in $(UNIT_TEST_BINS); do ./$$test_bin || exit; done

# Refuse to compile against a missing or different private driver ABI.  The
# runtime repeats this check against the loaded nvidia.ko before initialization.
check-nvidia-source:
	@if [ ! -f "$(NVIDIA_VERSION_MK)" ]; then \
	  echo "ERROR: NVIDIA driver sources not found at $(NVIDIA_SRC)" >&2; \
	  echo "From the repository root, run:" >&2; \
	  echo "  git submodule update --init --depth 1 third_party/open-gpu-kernel-modules" >&2; \
	  exit 1; \
	fi
	@actual=$$(sed -n 's/^NVIDIA_VERSION[[:space:]]*=[[:space:]]*//p' \
	                    "$(NVIDIA_VERSION_MK)" | head -n 1); \
	if [ "$$actual" != "$(MC_NVIDIA_VERSION)" ]; then \
	  echo "ERROR: libmc targets NVIDIA $(MC_NVIDIA_VERSION)," >&2; \
	  echo "       but NVIDIA_SRC contains $${actual:-an unknown version}." >&2; \
	  exit 1; \
	fi

# ── Output directories (order-only prereqs so parallel builds don't race) ─
bin lib:
	mkdir -p $@

# ── Build rules ────────────────────────────────────────────────────────────

# Generic rule for every tests/mc/*.c — links libmc
# and pulls in the test-side helper translation units (mc_test_helpers.c
# etc.).  No SDK headers needed; just public mc.h + the helper
# headers.  Future tests added to tests/mc/ pick up the helpers
# automatically.
bin/%: tests/mc/%.c $(MC_HELPERS) $(LIBMC) | bin
	$(CC) $(CFLAGS) -Imc -Itests/mc -o $@ \
	    $< $(MC_HELPERS) -L./lib -lmc \
	    -Wl,-rpath,'$$ORIGIN/../lib'

# ── sm_owner kernel: .cu -> .cubin -> .text bytes -> generated .h ─────────
#
# The sm_owner kernel (mc/kernels/sm_owner.cu) is a single-thread Hopper
# kernel that authors an entire CE-channel submission from the SM:
# pushbuffer methods, GPFIFO entry, USERD GPPut advance, BAR1 doorbell.
# libmc embeds it as a hex array (mc_sm_owner_kernel_sass[]) consumed
# by mc_sm_owner.c.
#
# The build is two-stage: nvcc compiles the .cu to a cubin, then the
# .text.sm_owner_kernel bytes are pulled out into a raw binary and xxd -i
# wraps them into a C-array source file that we transform into a header.
# The extraction is done by hand with readelf + dd rather than objcopy, for
# the reason given below.
#
# We commit the generated header (mc/mc_sm_owner_sass.h) so a fresh
# checkout builds without nvcc on the host.  The header is regenerated
# whenever sm_owner.cu changes; reviewers see the SASS-bytes diff in
# git, which is the only honest review surface for hand-managed SASS.
mc/mc_sm_owner_sass.h: mc/kernels/sm_owner.cu
	@echo "  NVCC    mc/kernels/sm_owner.cu (-> cubin)"
	$(NVCC) $(NVCC_KERNEL_FLAGS) -o mc/kernels/sm_owner.cubin $<
	@echo "  EXTRACT .text.sm_owner_kernel"
	@# objcopy's BFD doesn't have a backend for NVIDIA CUDA ELFs
	@# (e_machine = 190 / 0xBE), so we parse the ELF section table
	@# with readelf -SW (wide format, no name truncation) and dd
	@# the bytes out by file offset/size.  Columns 5/6 are hex
	@# offset/size without 0x prefix.
	@OFF=$$(readelf -SW mc/kernels/sm_owner.cubin                         \
	        | awk '/\.text\.sm_owner_kernel/ {print "0x"$$5; exit}');     \
	 SZ=$$(readelf  -SW mc/kernels/sm_owner.cubin                         \
	        | awk '/\.text\.sm_owner_kernel/ {print "0x"$$6; exit}');     \
	 if [ -z "$$OFF" ] || [ -z "$$SZ" ]; then                             \
	   echo "ERROR: could not locate .text.sm_owner_kernel in cubin" >&2; \
	   exit 1;                                                            \
	 fi;                                                                  \
	 dd if=mc/kernels/sm_owner.cubin of=mc/kernels/sm_owner.text.bin      \
	    bs=1 skip=$$(printf '%d' $$OFF) count=$$(printf '%d' $$SZ) status=none
	@# Sanity-check: the kernel must emit at least 19 system-scope
	@# strong-ordered global stores (16 × STG.E.STRONG.SYS for the
	@# PB method dwords + 1 × STG.E.64.STRONG.SYS for the GPFIFO
	@# entry + 1 × STG.E.STRONG.SYS for USERD GPPut + 1 × for the
	@# BAR1 doorbell).  If NVCC ever weakens any of these to plain
	@# STG.E.SYS or coalesces them, PBDMA could observe PB / GPFIFO
	@# / USERD writes out of order.  PTX-level we requested
	@# `st.relaxed.sys.global` which has lowered to STG.E.STRONG.SYS
	@# (or STG.E.64.STRONG.SYS for u64 stores) on every cuda toolkit
	@# we've tested (sm_90, CUDA 13.x), but the lowering isn't an
	@# ABI guarantee — fail the build loudly if it changes rather
	@# than discovering it as an intermittent runtime failure.
	@COUNT=$$($(CUOBJDUMP) --dump-sass mc/kernels/sm_owner.cubin             \
	          | grep -cE 'STG\.E(\.[0-9]+)?\.STRONG\.SYS');               \
	 if [ "$$COUNT" -lt 19 ]; then                                        \
	   echo "ERROR: sm_owner.cubin contains only $$COUNT STG.E.*STRONG.SYS"\
	        " instructions; expected >= 19 (16 PB + GPFIFO + GPPut +"     \
	        " doorbell).  NVCC may have weakened a store; re-check"       \
	        " sm_owner.cu inline PTX and cuobjdump --dump-sass." >&2;     \
	   exit 1;                                                            \
	 fi
	@# The optional on-GPU DMA-sema poll loop (compiled in, disabled at
	@# run time — the host passes dma_sema_poll_va = 0) is `__threadfence_system()`
	@# + `ld.global.cg`, which lowers to MEMBAR.SC.SYS + LDG.E.STRONG.GPU
	@# on Hopper.  The LDG bypasses L1 (.cg cache class), the MEMBAR
	@# provides system-scope ordering — the combination is what makes
	@# the SM observe PBDMA's CE-released sema (proven by the N=1000
	@# stability sweep; see libcuda-research docs/findings.md §14).
	@# Earlier `.relaxed.sys` lowering to LDG.E.STRONG.SYS spun on
	@# stale L1 forever.
	@# Gate on both the GPU-scope LDG inside the loop AND at least one
	@# MEMBAR.SC.SYS — losing either would silently break the
	@# cross-engine handshake.
	@LCOUNT=$$($(CUOBJDUMP) --dump-sass mc/kernels/sm_owner.cubin            \
	          | grep -cE 'LDG\.E(\.[0-9]+)?\.STRONG\.GPU');                \
	 if [ "$$LCOUNT" -lt 1 ]; then                                        \
	   echo "ERROR: sm_owner.cubin contains 0 LDG.E.*STRONG.GPU"          \
	        " instructions; the FB DMA-sema poll loop's"                  \
	        " ld.global.cg did not lower to a GPU-scope strong load." >&2;\
	   exit 1;                                                            \
	 fi
	@MCOUNT=$$($(CUOBJDUMP) --dump-sass mc/kernels/sm_owner.cubin            \
	          | grep -cE 'MEMBAR\.SC\.SYS');                               \
	 if [ "$$MCOUNT" -lt 1 ]; then                                        \
	   echo "ERROR: sm_owner.cubin contains 0 MEMBAR.SC.SYS"              \
	        " instructions; the per-iter __threadfence_system() in the"   \
	        " poll loop did not lower to a system-scope memory barrier." >&2; \
	   exit 1;                                                            \
	 fi
	@echo "  GEN     $@"
	@(  echo '/*';                                                        \
	    echo ' * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.'; \
	    echo ' * SPDX-License-Identifier: MIT';                           \
	    echo ' *';                                                        \
	    echo ' * mc_sm_owner_sass.h — auto-generated by the libmc Makefile';\
	    echo ' *';                                                        \
	    echo ' * Source: mc/kernels/sm_owner.cu';                         \
	    echo ' * Compiled with: $(NVCC) $(NVCC_KERNEL_FLAGS)';            \
	    echo ' *';                                                        \
	    echo ' * DO NOT EDIT BY HAND.  To regenerate, modify sm_owner.cu';\
	    echo ' * and run `make mc/mc_sm_owner_sass.h` from the repository root.'; \
	    echo ' */';                                                       \
	    echo '#ifndef MC_SM_OWNER_SASS_H';                                \
	    echo '#define MC_SM_OWNER_SASS_H';                                \
	    echo '';                                                          \
	    echo '#include <stdint.h>';                                       \
	    echo '';                                                          \
	    SIZE=$$(stat -c '%s' mc/kernels/sm_owner.text.bin);               \
	    REGS=$$($(CUOBJDUMP) --dump-resource-usage                        \
	            mc/kernels/sm_owner.cubin 2>&1                            \
	            | sed -n 's/.*REG:\([0-9]*\).*/\1/p' | head -1);          \
	    echo "/* SASS byte length and register count, harvested from cubin. */"; \
	    echo "#define MC_SM_OWNER_SASS_LEN ((uint32_t)$$SIZE)";          \
	    echo "#define MC_SM_OWNER_REGS     ((uint32_t)$$REGS)";          \
	    echo '';                                                          \
	    echo "static const uint8_t mc_sm_owner_kernel_sass[] = {";       \
	    xxd -i < mc/kernels/sm_owner.text.bin;                            \
	    echo "};";                                                        \
	    echo '';                                                          \
	    echo "#endif /* MC_SM_OWNER_SASS_H */";                           \
	) > $@
	@rm -f mc/kernels/sm_owner.text.bin
	@echo "  OK      $@ ($$(stat -c '%s' $@) bytes)"

# Convenience target so `make sm_owner` does the right thing.
.PHONY: sm_owner
sm_owner: mc/mc_sm_owner_sass.h

# libmc: the ioctl + submission machinery behind a small
# CUDA-runtime-like public API (see mc/mc.h).  Uses SDK_INC for the pinned SDK headers;
# compiled -fPIC and linked -shared.  mc_internal.h is a prereq
# only for rebuild tracking — public consumers include mc.h.
#
# Sources (one TU per topical band):
#   mc_rm.c            — RM ioctl wrappers (alloc/free/map/control + typed
#                        object allocators).  Hosts mc_debug() too.
#   mc_uvm.c           — UVM ioctl wrappers (initialize, register/unregister,
#                        map/unmap external allocation, register channel).
#   mc_submit.c        — method-stream + GPFIFO entry builders +
#                        mc_channel_arm/submit/poll_sema + drain_channel.
#   mc_vaspace.c       — VA pool (Paper-F1 anchor) + mc_va_space_* helpers.
#   mc_core.c          — lifecycle: per-kind channel init/fini + mc_init/
#                        mc_fini + mc_malloc/mc_free + mc_memcpy + the two
#                        CE-doorbell demos.
#   mc_compute_qmd.c   — Hopper QMD V04 builder + embedded SASS.
#   mc_compute_demo.c  — HOPPER_COMPUTE_A demos (mc_compute_doorbell_kernel,
#                        mc_memcpy_gpu_doorbell_sm, mc_compute_get_scratch).
#   mc_sm_owner.c      — SM-authored CE submission helper.  Library-internal
#                        (declared in mc_internal.h); reached through the
#                        public API via mc_memcpy(..., MC_XFER_SM).
#                        Embeds the sm_owner kernel from mc_sm_owner_sass.h.
$(LIBMC): $(MC_LIB_SRCS) \
                 mc/mc.h mc/mc_internal.h \
                 mc/mc_compute_qmd.h mc/mc_sm_owner_sass.h \
                 compat/nvidia.mk | lib check-nvidia-source
	$(CC) $(CFLAGS) $(MC_CPPFLAGS) -fPIC $(SDK_INC) -Imc -shared -o $@ \
	    $(MC_LIB_SRCS)

bin/mc_rm_version_test: tests/unit/mc_rm_version_test.c mc/mc_rm.c \
                        mc/mc.h mc/mc_internal.h compat/nvidia.mk \
                        | bin check-nvidia-source
	$(CC) $(CFLAGS) $(MC_CPPFLAGS) $(SDK_INC) -Imc -o $@ \
	    tests/unit/mc_rm_version_test.c mc/mc_rm.c -Wl,--wrap=ioctl

bin/mc_init_version_test: tests/unit/mc_init_version_test.c $(MC_LIB_SRCS) \
                          mc/mc.h mc/mc_internal.h compat/nvidia.mk \
                          | bin check-nvidia-source
	$(CC) $(CFLAGS) $(MC_CPPFLAGS) $(SDK_INC) -Imc -o $@ \
	    tests/unit/mc_init_version_test.c $(MC_LIB_SRCS) \
	    -Wl,--wrap=open -Wl,--wrap=ioctl -Wl,--wrap=close

# ── Utility ────────────────────────────────────────────────────────────────
clean:
	rm -rf bin lib

help:
	@echo "Targets:"
	@echo "  all              build everything (bins + libs)"
	@echo "  mc-all           build every tests/mc/*.c binary"
	@echo "  bins             build everything under bin/"
	@echo "  libs             build everything under lib/"
	@echo "  libmc            build $(LIBMC) — the mc library"
	@echo "  check-nvidia-source  validate NVIDIA_SRC is driver $(MC_NVIDIA_VERSION)"
	@echo "  test-unit        run host-only unit tests"
	@echo "  sm_owner         regenerate mc/mc_sm_owner_sass.h from the kernel"
	@echo "  clean            remove bin/ and lib/ directories"
	@echo "  help             this message"
	@echo
	@echo "Discovered test binaries (mc-all):"
	@$(foreach b,$(MC_BINS),echo "  $(notdir $b)";)
	@echo
	@echo "Each binary can also be built by name, e.g. 'make mc_demo'."
	@echo
	@echo "Overridable variables:"
	@echo "  CC         C compiler          (default: $(CC))"
	@echo "  CFLAGS     C compile flags     (default: $(CFLAGS))"
	@echo "  NVCC       CUDA compiler used only by sm_owner (default: $(NVCC))"
	@echo "  CUOBJDUMP  SASS inspection tool (default: $(CUOBJDUMP))"
	@echo "  NVIDIA_SRC matching driver tree (default: pinned submodule)"
