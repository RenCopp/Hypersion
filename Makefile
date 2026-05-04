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
#  On Linux:          apt install g++ make
# ============================================================================

ENGINE   = Hypersion
VERSION  = 0.1
CXX      = g++
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
ifeq ($(ARCH),x86-64-bmi2)
    ARCH_FLAGS = -march=haswell -mbmi2 -DUSE_POPCNT -DUSE_AVX2 -DUSE_PEXT
endif
# AVX-VNNI: 256-bit VNNI dpbusd intrinsics (Intel Alder Lake+, Zen 4+ E-cores
# do NOT have it). Adds ~15-25 % NNUE-FC speed via _mm256_dpbusd_epi32.
# Use this for Intel 12th/13th/14th gen consumer CPUs (no AVX-512), Zen 4+.
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
COMMON   = $(STD) $(WARN) $(ARCH_FLAGS) -pthread
RELEASE  = -O3 -DNDEBUG -flto -fno-exceptions
DEBUG    = -O0 -g3 -fsanitize=address,undefined -fno-omit-frame-pointer

CXXFLAGS ?= $(COMMON) $(RELEASE)
LDFLAGS  += -pthread -flto

# Windows: static-link libstdc++ / libgcc so the .exe is portable.
# Bump default stack to 16 MB — qsearch can recurse deep with large MovePicker buffers on stack.
ifeq ($(OS),Windows_NT)
    LDFLAGS += -static -static-libstdc++ -static-libgcc -Wl,--stack,16777216
    EXE     := .exe
else
    EXE     :=
endif

TARGET = $(BINDIR)/$(ENGINE)$(EXE)

# -----------------------------------------------------------------------------
# Targets
# -----------------------------------------------------------------------------
.PHONY: all build debug profile bench clean help tuner
all: build

build: $(TARGET)

# ----- Texel tuner ----------------------------------------------------------
# Loads labeled positions, reports MSE on Hypersion's classical eval.
# See tools/tuner/README.md for the full Texel workflow + data extraction.
TUNER_TARGET = tuner$(EXE)
TUNER_SRCS   = tools/tuner/tuner.cpp \
               src/bitboard.cpp src/position.cpp src/movegen.cpp \
               src/zobrist.cpp src/misc.cpp src/evaluate.cpp
tuner: $(TUNER_TARGET)
$(TUNER_TARGET): $(TUNER_SRCS)
	$(CXX) $(STD) $(WARN) $(ARCH_FLAGS) -O3 -DNDEBUG -fno-exceptions \
	    -o $@ $(TUNER_SRCS) -static -static-libstdc++ -static-libgcc -Wl,--stack,16777216
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

debug: CXXFLAGS = $(COMMON) $(DEBUG)
debug: LDFLAGS += -fsanitize=address,undefined
debug: clean $(TARGET)

$(TARGET): $(OBJECTS) $(FATHOM_OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $(OBJECTS) $(FATHOM_OBJECTS)
	@echo "Built $@"

$(OBJDIR)/%.o: $(SRCDIR)/%.cpp | $(OBJDIR)
	$(CXX) $(CXXFLAGS) -I$(FATHOM_DIR) -c -o $@ $<

$(OBJDIR)/fathom_%.o: $(FATHOM_DIR)/%.c | $(OBJDIR)
	gcc -O3 -DNDEBUG -flto -DTB_NO_HELPER_API -I$(FATHOM_DIR) -c -o $@ $<

$(OBJDIR):
	@mkdir -p $(OBJDIR)

bench: build
	./$(TARGET) bench

# PGO: build with instrumentation, run bench to collect profile, rebuild using
# the profile. Typically a 5–10 % nps gain on top of the LTO release build.
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

help:
	@echo "Hypersion Makefile targets:"
	@echo "  make build    - release build (default)"
	@echo "  make debug    - debug build with sanitizers"
	@echo "  make profile  - PGO build"
	@echo "  make bench    - build and run bench"
	@echo "  make clean    - remove build artifacts"
	@echo ""
	@echo "Architecture selection: ARCH=x86-64-avx2|x86-64-bmi2|x86-64-avx512|native"
