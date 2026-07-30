# ============================================================================
#  Hypersion chess engine — Makefile
#  Builds a UCI engine executable.
#
#  Targets:
#    make            (alias for `make build`)
#    make build      release build with -O3 -flto -march=native
#    make debug      -O0 -g -fsanitize=address,undefined
#    make profile    2-pass PGO (instrument, run bench, optimize)
#    make bench      build then run `./Hypersion bench`
#    make clean      remove build artifacts
#
#  Architecture:
#    ARCH=x86-64-avx2     (default, widely supported on modern CPUs)
#    ARCH=x86-64-bmi2     (Zen3+/Ice Lake+ — uses PEXT for sliders)
#    ARCH=x86-64-avx512   (Ice Lake/Zen4+)
#
#  On Windows MSYS2:  pacman -S mingw-w64-x86_64-gcc make
#  For `make debug`:  pacman -S mingw-w64-clang-x86_64-clang mingw-w64-clang-x86_64-compiler-rt
#  On Linux:          apt install g++ make
# ============================================================================

ENGINE   = Hypersion
VERSION  = 3.6.0-dev
CXX      = g++
CC       = gcc
STD      = -std=c++20
ARCH    ?= x86-64-avx2

SRCDIR   = src
OBJDIR   = obj
BINDIR   = .

SOURCES   = $(wildcard $(SRCDIR)/*.cpp)
OBJECTS   = $(patsubst $(SRCDIR)/%.cpp,$(OBJDIR)/%.o,$(SOURCES))

# Fathom (Syzygy) — bundled C sources, compiled with the C compiler.
FATHOM_DIR     = $(SRCDIR)/fathom
FATHOM_SOURCES = $(FATHOM_DIR)/tbprobe.c
FATHOM_OBJECTS = $(patsubst $(FATHOM_DIR)/%.c,$(OBJDIR)/fathom_%.o,$(FATHOM_SOURCES))

# -----------------------------------------------------------------------------
# Architecture-specific flags
# -----------------------------------------------------------------------------
ifeq ($(ARCH),x86-64-avx2)
    ARCH_FLAGS = -march=haswell -DUSE_POPCNT -DUSE_AVX2
endif
# x86-64 baseline (no AVX2 / no BMI2). Targets very old CPUs (pre-2013
# Intel, pre-2015 AMD) — runs but with much lower NPS due to no SIMD.
# For users on old hardware who can't run the AVX2 build.
ifeq ($(ARCH),x86-64)
    ARCH_FLAGS = -march=x86-64 -msse2
endif
ifeq ($(ARCH),x86-64-bmi2)
    ARCH_FLAGS = -march=haswell -mbmi2 -DUSE_POPCNT -DUSE_AVX2 -DUSE_PEXT
endif
# AVX-VNNI: 256-bit VNNI dpbusd intrinsics (Intel Alder Lake+, Zen 4+ E-cores
# do NOT have it). Adds ~15-25 % NNUE-FC speed via _mm256_dpbusd_epi32.
# Use this for Intel 12th/13th/14th gen consumer CPUs (no AVX-512), Zen 4+.
# **+29.6 +/- 35.6 ELO @ 200g 5+0.05 conc=2** vs avx2 build (post-cutoffCnt
# baseline) on i7-14700F. Earlier conc=6 testing showed -45 ELO and was
# rejected — that result was a cache-contention artefact (cutechess #630),
# not a real regression. Re-test with conc=2 if you suspect this.
ifeq ($(ARCH),x86-64-avxvnni)
    ARCH_FLAGS = -march=alderlake -mavxvnni -mbmi2 -DUSE_POPCNT -DUSE_AVX2 -DUSE_PEXT -DUSE_AVXVNNI
endif
ifeq ($(ARCH),x86-64-avx512)
    ARCH_FLAGS = -march=skylake-avx512 -DUSE_POPCNT -DUSE_AVX2 -DUSE_AVX512 -DUSE_PEXT
endif
ifeq ($(ARCH),native)
    ARCH_FLAGS = -march=native -DUSE_POPCNT
endif

# -----------------------------------------------------------------------------
# Common flags
# -----------------------------------------------------------------------------
WARN     = -Wall -Wextra -Wcast-qual -Wshadow -pedantic -Wno-unused-parameter
# -MMD -MP generates per-object dependency files (.d) so make rebuilds the
# right .o files when headers change. Without this, modifying a header
# (e.g. timeman.h) only rebuilds the .o whose .cpp directly references it
# via the compile-target rule — leaving stale .o files for indirect users.
# At -flto link time this surfaces as a "type 'struct X' violates the C++
# One Definition Rule [-Wodr]" warning, requiring `make clean` to clear.
# With -MMD -MP, dep tracking is automatic and `make -j` Just Works.
COMMON   = $(STD) $(WARN) $(ARCH_FLAGS) -pthread -MMD -MP
RELEASE  = -O3 -DNDEBUG -flto=auto -fno-exceptions
# NOTE: tried -funroll-loops; bench showed +9% NPS but 200g 5+0.05 match
# regressed -22.6 ELO. Likely cause: aggressive unrolling expands the
# instruction footprint, hurting i-cache hit rate when 8 cutechess
# games run concurrently. Bench (single-thread) is misleading here.
#
# NOTE: Game-workload PGO (-fprofile-generate / -fprofile-use cycle, see
# testing/pgo_build.py) is in the same family of "expanded code footprint"
# pessimization. Tested 2026-05-07 at conc=2 (per memory-aggressive
# protocol): -40.1 +/- 38.0 ELO @ 200g. Re-tested 2026-05-09 post-A2-v2
# + A3 ship (different hot path, fresh profile from 30 selfplay games):
# -6.9 +/- 37.1 ELO @ 200g (W=57 L=61 D=82). Confirmed tombstone.
# Game-PGO consistently hurts or no-ops on Hypersion. Future contributor
# wanting to retry should pair with i-cache-aware build flags
# (-falign-functions, -falign-loops alignment to L1I line size, or
# explicit `__attribute__((cold))` annotations on rare error paths)
# to compensate for the footprint expansion.
DEBUG    = -O0 -g3 -fsanitize=address,undefined -fno-omit-frame-pointer
DEBUG_LDFLAGS = -pthread -fsanitize=address,undefined
DEBUG_FATHOM_CFLAGS = -O0 -g3 -fsanitize=address,undefined -fno-omit-frame-pointer -DTB_NO_HELPER_API

ifeq ($(OS),Windows_NT)
    SANITIZER_CXX ?= /clang64/bin/clang++
    SANITIZER_CC  ?= /clang64/bin/clang
    SANITIZER_RUNTIME = libclang_rt.asan_dynamic-x86_64.dll.a
else
    SANITIZER_CXX ?= $(CXX)
    SANITIZER_CC  ?= $(CC)
endif

CXXFLAGS ?= $(COMMON) $(RELEASE)
LDFLAGS  += -pthread -flto=auto

# Windows: static-link libstdc++ / libgcc so the .exe is portable.
# Bump default stack to 16 MB — qsearch can recurse deep with large MovePicker buffers on stack.
ifeq ($(OS),Windows_NT)
    LDFLAGS += -static -static-libstdc++ -static-libgcc -Wl,--stack,16777216
    DEBUG_LDFLAGS += -Wl,--stack,16777216
    EXE     := .exe
else
    EXE     :=
endif

TARGET = $(BINDIR)/$(ENGINE)$(EXE)

# -----------------------------------------------------------------------------
# Targets
# -----------------------------------------------------------------------------
.PHONY: all build debug profile bench clean help tuner release test_timeman
all: build

build: $(TARGET)

# Strip debug info + symbol table for distribution. Reduces .exe size
# significantly on MinGW (~15-25 % typical), no perf impact. Used for
# release tarballs / GitHub releases. Run AFTER `make` (or `make profile`).
release: build
	strip --strip-all $(TARGET)
	@echo "Stripped $(TARGET) for distribution"

# ----- Texel tuner ----------------------------------------------------------
# Loads labeled positions, reports MSE on Hypersion's classical eval.
# See tools/tuner/README.md for the full Texel workflow + data extraction.
TUNER_TARGET = tuner$(EXE)
TUNER_SRCS   = tools/tuner/tuner.cpp \
               src/bitboard.cpp src/position.cpp src/movegen.cpp \
               src/zobrist.cpp src/misc.cpp src/evaluate.cpp
tuner: $(TUNER_TARGET)
$(TUNER_TARGET): $(TUNER_SRCS)
	$(CXX) $(STD) $(WARN) $(ARCH_FLAGS) -O3 -DNDEBUG -fno-exceptions -fopenmp \
	    -o $@ $(TUNER_SRCS) -static -static-libstdc++ -static-libgcc -Wl,--stack,16777216 -fopenmp
	@echo "Built $@"

# ----- PGN-to-positions extractor (for the tuner) --------------------------
PGN2POS_TARGET = pgn_to_positions$(EXE)
PGN2POS_SRCS   = tools/tuner/pgn_to_positions.cpp \
                 src/bitboard.cpp src/position.cpp src/movegen.cpp \
                 src/zobrist.cpp src/misc.cpp
pgn_to_positions: $(PGN2POS_TARGET)
$(PGN2POS_TARGET): $(PGN2POS_SRCS)
	$(CXX) $(STD) $(WARN) $(ARCH_FLAGS) -O3 -DNDEBUG -fno-exceptions \
	    -o $@ $(PGN2POS_SRCS) -static -static-libstdc++ -static-libgcc -Wl,--stack,16777216
	@echo "Built $@"

debug:
ifeq ($(OS),Windows_NT)
	@runtime_path=`$(SANITIZER_CXX) --print-file-name=$(SANITIZER_RUNTIME)`; \
	 if [ "$$runtime_path" = "$(SANITIZER_RUNTIME)" ]; then \
	   echo "[!!] sanitizer runtime missing: $(SANITIZER_RUNTIME)"; \
	   echo "[!!] install it with: pacman -S mingw-w64-clang-x86_64-compiler-rt"; \
	   exit 1; \
	 fi
endif
	$(MAKE) clean
	$(MAKE) $(TARGET) CXX="$(SANITIZER_CXX)" CC="$(SANITIZER_CC)" \
	    CXXFLAGS="$(COMMON) $(DEBUG) $(SANITIZER_INCLUDE_FLAGS)" \
	    LDFLAGS="$(DEBUG_LDFLAGS)" \
	    FATHOM_CFLAGS="$(DEBUG_FATHOM_CFLAGS) $(SANITIZER_INCLUDE_FLAGS)"
ifeq ($(OS),Windows_NT)
	@echo "Run the sanitizer binary with /clang64/bin on PATH."
endif

$(TARGET): $(OBJECTS) $(FATHOM_OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $(OBJECTS) $(FATHOM_OBJECTS)
	@echo "Built $@"

$(OBJDIR)/%.o: $(SRCDIR)/%.cpp | $(OBJDIR)
	$(CXX) $(CXXFLAGS) -I$(FATHOM_DIR) -c -o $@ $<

FATHOM_CFLAGS ?= -O3 -DNDEBUG -flto -DTB_NO_HELPER_API
$(OBJDIR)/fathom_%.o: $(FATHOM_DIR)/%.c | $(OBJDIR)
	$(CC) $(FATHOM_CFLAGS) -MMD -MP -I$(FATHOM_DIR) -c -o $@ $<

$(OBJDIR):
	@mkdir -p $(OBJDIR)

# Deterministic unit tests for clock-budget arithmetic. Kept separate from the
# engine binary so edge cases can assert exact optimum/maximum values.
TIMEMAN_TEST_TARGET = $(OBJDIR)/timeman_test$(EXE)
test_timeman: | $(OBJDIR)
	$(CXX) $(STD) $(WARN) -O2 -I$(SRCDIR) -o $(TIMEMAN_TEST_TARGET) \
	    testing/timeman_test.cpp src/timeman.cpp src/misc.cpp -pthread
	./$(TIMEMAN_TEST_TARGET)

bench: build
	./$(TARGET) bench

# PGO: build with instrumentation, run bench to collect profile, rebuild using
# the profile. Bench shows ~5-15 % NPS gain on top of the LTO release build,
# and depth-18 single-thread search can run up to ~38 % faster.
#
# DEPLOYMENT NOTE: PGO is a real win for single-instance production use
# (lichess-bot, analysis tools) but REGRESSES at high concurrent-process
# load. Tested at concurrency=6 cutechess match against non-PGO base:
#   30g  : +34.9 +/- 96.0 ELO  (lucky positive tail)
#   61g  : -45.8 +/- 75.6 ELO  (regression-to-mean made true direction visible)
# Same i-cache pressure pattern as -funroll-loops (which also bench-faster /
# concurrent-match-slower). Use PGO build for production binaries; do NOT
# use it as the SPRT testing baseline.
PGO_DIR = ./pgo
profile:
	$(MAKE) clean
	$(MAKE) build CXXFLAGS="$(COMMON) -O3 -DNDEBUG -fprofile-generate=$(PGO_DIR) -fprofile-correction" \
	              LDFLAGS="-pthread -static -static-libstdc++ -static-libgcc -Wl,--stack,16777216 -fprofile-generate=$(PGO_DIR) -fprofile-correction"
	@mkdir -p $(PGO_DIR)
	./$(TARGET) bench 13
	./$(TARGET) bench 11
	$(MAKE) clean
	$(MAKE) build CXXFLAGS="$(COMMON) $(RELEASE) -fprofile-use=$(PGO_DIR) -fprofile-correction -Wno-missing-profile -Wno-coverage-mismatch -Wno-error=coverage-mismatch" \
	              LDFLAGS="-pthread -flto -static -static-libstdc++ -static-libgcc -Wl,--stack,16777216 -fprofile-use=$(PGO_DIR) -fprofile-correction"
	@rm -rf $(PGO_DIR)
	@echo "PGO build complete: $(TARGET)"

clean:
	@rm -rf $(OBJDIR) $(TARGET)
	@echo "Cleaned."

# -MMD generates one .d file per .o with the same stem. Pull them all in.
# Wrapped in conditional include so a missing .d (e.g. first build) isn't
# an error.
DEPS = $(OBJECTS:.o=.d) $(FATHOM_OBJECTS:.o=.d)
-include $(DEPS)

# Strict bench determinism gate. Bench at Threads=1 should be 100 %
# deterministic. The canonical depth-13 node count lives in the tracked
# testing/BENCH_SIGNATURE file and must be refreshed intentionally whenever
# an accepted search change shifts the tree.
# Earlier session work hit a stale-obj-file issue where incremental
# builds produced non-deterministic binaries — a clean rebuild fixed it.
# If verify fails here, run `make clean && make -j` and try again before
# investigating real search-behavior changes.
BENCH_SIGNATURE_FILE ?= testing/BENCH_SIGNATURE
verify: build
	@echo "Running bench 5x (Threads=1, NNUE on, depth 13)..."
	@for i in 1 2 3 4 5; do \
	    printf 'setoption name Threads value 1\nisready\nbench 13\nquit\n' | ./$(TARGET) 2>&1 \
	      | grep -E "^Nodes searched ?:" | sed "s/^/  Run $$i: /"; \
	  done
	@expected=$$(cat "$(BENCH_SIGNATURE_FILE)" 2>/dev/null); \
	 if [ -z "$$expected" ]; then \
	   echo "[!!] missing bench signature: $(BENCH_SIGNATURE_FILE)"; \
	   exit 1; \
	 fi; \
	 n=$$(printf 'setoption name Threads value 1\nisready\nbench 13\nquit\n' | ./$(TARGET) 2>&1 \
	      | awk '/^Nodes searched ?:/ { n = $$NF } END { print n }'); \
	 if [ "$$n" = "$$expected" ]; then \
	   echo "[OK] bench=$$n matches expected $$expected"; \
	 else \
	   echo "[!!] bench=$$n differs from expected $$expected"; \
	   echo "[!!] If you didn't change src/, run \`make clean && make -j\`."; \
	   exit 1; \
	 fi

help:
	@echo "Hypersion Makefile targets:"
	@echo "  make build    - release build (default)"
	@echo "  make debug    - debug build with sanitizers"
	@echo "  make profile  - PGO build"
	@echo "  make bench    - build and run bench"
	@echo "  make verify   - build + bench, fail if signature drifts"
	@echo "  make test_timeman - run deterministic clock-budget unit tests"
	@echo "  make clean    - remove build artifacts"
	@echo ""
	@echo "Architecture selection: ARCH=x86-64|x86-64-avx2|x86-64-bmi2|x86-64-avxvnni|x86-64-avx512|native"
