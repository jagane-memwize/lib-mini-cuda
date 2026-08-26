# SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
# SPDX-License-Identifier: MIT
#
# libmc Makefile — builds the library and its example artefacts:
#
#   bin/mc_demo          — D2H transfer on top of libmc, no CUDA library.
#   lib/libmc.so         — the mc library itself (see mc/include/mc/mc.h).
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

# Include roots.  -Imc/include exposes only the public <mc/mc.h>; -Imc is the
# library-internal root, so a TU says #include "internal/types.h" or
# "arch/hopper/qmd_v04.h" and the layer it is reaching into is visible at the
# include site.  Consumers get MC_PUBLIC_INC alone and never see internal/.
MC_PUBLIC_INC := -Imc/include
MC_INC        := $(MC_PUBLIC_INC) -Imc
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
LIBMC_SONAME := libmc.so.0
LIBMC_MAP    := mc/libmc.map
# Sources, grouped by layer.  core/ and hal/ are architecture-neutral;
# driver/ wraps the RM and UVM ioctl surfaces; arch/<name>/ holds
# per-architecture code; experimental/ holds the research paths.
MC_CORE_SRCS := mc/core/context.c mc/core/alloc.c mc/core/channel.c \
                mc/core/memcpy.c mc/core/vaspace.c
MC_HAL_SRCS  := mc/hal/submit.c
MC_DRV_SRCS  := mc/driver/rm.c mc/driver/uvm.c
MC_ARCH_SRCS := mc/arch/hopper/qmd_v04.c
MC_EXP_SRCS  := mc/experimental/compute.c mc/experimental/sm_owner.c \
                mc/experimental/doorbell.c

MC_LIB_SRCS := $(MC_CORE_SRCS) $(MC_HAL_SRCS) $(MC_DRV_SRCS) \
               $(MC_ARCH_SRCS) $(MC_EXP_SRCS)

# Per-translation-unit objects.  The library used to be built by a single
# compiler invocation over every source; that does not scale as the tree grows
# (architecture backends, a HAL layer, split-up core), and it recompiles
# everything on any edit.
BUILD_DIR   := build
MC_LIB_OBJS := $(patsubst mc/%.c,$(BUILD_DIR)/%.o,$(MC_LIB_SRCS))

# Automatic header-dependency tracking.  Replaces the hand-maintained header
# prerequisite list on the link rule, which silently goes stale every time a
# new internal header appears.
DEPFLAGS := -MMD -MP

# Library-only compile flags.  MC_BUILDING_LIBMC turns on the MC_API
# visibility attribute in mc.h; -fvisibility=hidden makes every other symbol
# library-private.  See mc/libmc.map for the link-time half of the contract.
MC_LIB_CPPFLAGS := -DMC_BUILDING_LIBMC
MC_LIB_CFLAGS   := -fPIC -fvisibility=hidden

# Unit tests are auto-discovered like tests/mc.  Each test may override the
# symbols it interposes and the library sources it links; the defaults suit a
# test that wraps ioctl and links the whole library.
UNIT_SRCS      := $(wildcard tests/unit/*.c)
UNIT_TEST_BINS := $(patsubst tests/unit/%.c,bin/%,$(UNIT_SRCS))
UNIT_WRAP_DEFAULT := ioctl
UNIT_SRCS_DEFAULT := $(MC_LIB_SRCS)

# Per-test overrides.  A test with no entry here gets the defaults above.
UNIT_WRAP_mc_rm_version_test    := ioctl
UNIT_SRCS_mc_rm_version_test    := mc/driver/rm.c
UNIT_WRAP_mc_lce_selection_test := ioctl
UNIT_SRCS_mc_lce_selection_test := mc/driver/rm.c
UNIT_WRAP_mc_init_version_test  := open ioctl close

# Resolve a test's wrap set / source list, falling back to the defaults.
unit_wrap = $(if $(UNIT_WRAP_$(1)),$(UNIT_WRAP_$(1)),$(UNIT_WRAP_DEFAULT))
unit_srcs = $(if $(UNIT_SRCS_$(1)),$(UNIT_SRCS_$(1)),$(UNIT_SRCS_DEFAULT))

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
# `|| exit $$?` — a bare `exit` would return the shell's last status rather
# than the failing test's, which can mask a failure as success.
test-unit: $(UNIT_TEST_BINS)
	@for test_bin in $(UNIT_TEST_BINS); do ./$$test_bin || exit $$?; done

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
bin lib $(BUILD_DIR):
	mkdir -p $@

# ── Build rules ────────────────────────────────────────────────────────────

# Generic rule for every tests/mc/*.c — links libmc
# and pulls in the test-side helper translation units (mc_test_helpers.c
# etc.).  No SDK headers needed; just public mc.h + the helper
# headers.  Future tests added to tests/mc/ pick up the helpers
# automatically.
bin/%: tests/mc/%.c $(MC_HELPERS) $(LIBMC) | bin
	$(CC) $(CFLAGS) $(MC_PUBLIC_INC) -Itests/mc -o $@ \
	    $< $(MC_HELPERS) -L./lib -lmc \
	    -Wl,-rpath,'$$ORIGIN/../lib'

# ── sm_owner kernel: .cu -> .cubin -> .text bytes -> generated .h ─────────
#
# The sm_owner kernel (mc/kernels/sm_owner.cu) is a single-thread Hopper
# kernel that authors an entire CE-channel submission from the SM:
# pushbuffer methods, GPFIFO entry, USERD GPPut advance, BAR1 doorbell.
# libmc embeds it as a hex array (mc_sm_owner_kernel_sass[]) consumed
# by experimental/sm_owner.c.
#
# The build is two-stage: nvcc compiles the .cu to a cubin, then the
# .text.sm_owner_kernel bytes are pulled out into a raw binary and xxd -i
# wraps them into a C-array source file that we transform into a header.
# The extraction is done by hand with readelf + dd rather than objcopy, for
# the reason given below.
#
# We commit the generated header (mc/arch/hopper/sm_owner_sass.h) so a fresh
# checkout builds without nvcc on the host.  The header is regenerated
# whenever sm_owner.cu changes; reviewers see the SASS-bytes diff in
# git, which is the only honest review surface for hand-managed SASS.
mc/arch/hopper/sm_owner_sass.h: mc/kernels/sm_owner.cu
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
	@# stability sweep; see libcuda-research docs/findings.md under
	@# "FB-carrier and cross-engine ordering").
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
	    echo ' * sm_owner_sass.h — auto-generated by the libmc Makefile';\
	    echo ' *';                                                        \
	    echo ' * Source: mc/kernels/sm_owner.cu';                         \
	    echo ' * Compiled with: $(NVCC) $(NVCC_KERNEL_FLAGS)';            \
	    echo ' *';                                                        \
	    echo ' * DO NOT EDIT BY HAND.  To regenerate, modify sm_owner.cu';\
	    echo ' * and run `make mc/arch/hopper/sm_owner_sass.h` from the repository root.'; \
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
sm_owner: mc/arch/hopper/sm_owner_sass.h

# libmc: the ioctl + submission machinery behind a small
# CUDA-runtime-like public API (see mc/include/mc/mc.h).  Uses SDK_INC for the pinned SDK headers;
# compiled -fPIC and linked -shared.  mc/internal/*.h is a prereq
# only for rebuild tracking — public consumers include mc.h.
#
# Sources, one TU per concern:
#   driver/rm.c              RM ioctl wrappers (alloc/free/map/control + typed
#                            object allocators).  Hosts mc_debug() too.
#   driver/uvm.c             UVM ioctl wrappers (initialize, register/unregister,
#                            map/unmap external allocation, register channel).
#   hal/submit.c             method-stream + GPFIFO entry builders +
#                            mc_channel_arm/submit/poll_sema + drain_channel.
#                            The only TU that names the USERD class struct.
#   core/context.c           mc_init / mc_fini.
#   core/channel.c           per-kind channel bring-up and teardown.
#   core/alloc.c             allocation table + mc_malloc_* / mc_free /
#                            mc_host_register / mc_gpu_va.
#   core/memcpy.c            mc_memcpy and its VAS/agent dispatch.
#   core/vaspace.c           VA pool (Paper-F1 anchor) + mc_va_space_* helpers.
#   arch/hopper/qmd_v04.c    Hopper QMD V04 builder + embedded SASS.
#   experimental/compute.c   HOPPER_COMPUTE_A demos (mc_compute_doorbell_kernel,
#                            mc_memcpy_gpu_doorbell_sm, mc_compute_get_scratch).
#   experimental/doorbell.c  host-authored CE-doorbell demos
#                            (mc_dbell_demo_ring, mc_memcpy_gpu_doorbell_ce).
#   experimental/sm_owner.c  SM-authored CE submission helper.  Library-internal
#                            (declared in internal/compute.h); reached through
#                            the public API via mc_memcpy(..., MC_XFER_SM).
#                            Embeds arch/hopper/sm_owner_sass.h.
# Per-TU compile.  Header prerequisites come from the generated .d files
# (DEPFLAGS), so adding an internal header needs no Makefile edit.
$(BUILD_DIR)/%.o: mc/%.c compat/nvidia.mk | $(BUILD_DIR) check-nvidia-source
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(DEPFLAGS) $(MC_CPPFLAGS) $(MC_LIB_CPPFLAGS) \
	    $(MC_LIB_CFLAGS) $(SDK_INC) $(MC_INC) -c -o $@ $<

# The version script is the authority on what libmc exports; mc/include/mc/mc.h's MC_API
# markers must list the same set.  Without it, every non-static symbol in every
# TU lands in the public ABI.
$(LIBMC): $(MC_LIB_OBJS) $(LIBMC_MAP) | lib
	$(CC) $(CFLAGS) $(MC_LIB_CFLAGS) -shared -o $@ $(MC_LIB_OBJS) \
	    -Wl,--version-script,$(LIBMC_MAP) -Wl,-soname,$(LIBMC_SONAME)

# Generic unit-test rule.  Each test links the library sources it needs
# directly (not the .so) and interposes libc entry points with --wrap, so the
# whole suite runs without a GPU, without root, and without /dev/nvidia*.
# Adding tests/unit/foo.c is enough to get bin/foo built and run.
.SECONDEXPANSION:
bin/%: tests/unit/%.c $$(call unit_srcs,$$*) compat/nvidia.mk \
       | bin check-nvidia-source
	$(CC) $(CFLAGS) $(MC_CPPFLAGS) $(SDK_INC) $(MC_INC) -o $@ \
	    $< $(call unit_srcs,$*) \
	    $(foreach w,$(call unit_wrap,$*),-Wl,--wrap=$(w))

# Guard the export contract: the MC_API markers in mc/include/mc/mc.h, the version script,
# and the symbols actually exported by the .so must all agree.  Losing an
# MC_API silently un-exports a public function; forgetting `local: *` silently
# publishes every internal helper.  Neither shows up as a build failure, so
# check it explicitly.
.PHONY: check-abi
check-abi: $(LIBMC)
	@nm -D --defined-only $(LIBMC) | awk '{print $$3}' | sed 's/@@.*//' \
	    | grep '^mc_' | sort -u > $(BUILD_DIR)/.abi-actual
	@grep '^MC_API' mc/include/mc/mc.h \
	    | sed -E 's/.*[ *](mc_[a-z_]+)\(.*/\1/' | sort -u > $(BUILD_DIR)/.abi-expected
	@if ! diff -u $(BUILD_DIR)/.abi-expected $(BUILD_DIR)/.abi-actual; then \
	  echo "ERROR: exported ABI does not match the MC_API set in mc/include/mc/mc.h." >&2; \
	  echo "       Reconcile mc/include/mc/mc.h, mc/libmc.map, and the definitions." >&2; \
	  exit 1;                                                                 \
	fi
	@echo "  OK      ABI: $$(wc -l < $(BUILD_DIR)/.abi-actual) exported symbols match mc/include/mc/mc.h"

# ── Utility ────────────────────────────────────────────────────────────────
clean:
	rm -rf bin lib $(BUILD_DIR)

# Header dependencies generated by DEPFLAGS.  Absent on a clean tree, which is
# why this is `-include`.
-include $(MC_LIB_OBJS:.o=.d)

help:
	@echo "Targets:"
	@echo "  all              build everything (bins + libs)"
	@echo "  mc-all           build every tests/mc/*.c binary"
	@echo "  bins             build everything under bin/"
	@echo "  libs             build everything under lib/"
	@echo "  libmc            build $(LIBMC) — the mc library"
	@echo "  check-nvidia-source  validate NVIDIA_SRC is driver $(MC_NVIDIA_VERSION)"
	@echo "  test-unit        run host-only unit tests (auto-discovered)"
	@echo "  check-abi        verify exported symbols match mc.h's MC_API set"
	@echo "  sm_owner         regenerate mc/arch/hopper/sm_owner_sass.h from the kernel"
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
