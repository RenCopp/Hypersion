# Building Hypersion

## Requirements

- **g++ 12+** with C++20 support (clang 15+ also works with `CXX=clang++`)
- **GNU make**
- **AVX2-capable x86_64 CPU** for the default build (Haswell, 2013, or
  newer). Older CPUs need a different `ARCH=` (see below).
- Linux, macOS, or Windows.

On **Windows**, install MSYS2 from <https://www.msys2.org>, then
`pacman -S mingw-w64-x86_64-gcc make`.

## Quick build

From the project root:

```
make build              # default release: -O3 -flto -march=haswell + AVX2
```

The output binary is `Hypersion` (Linux/macOS) or `Hypersion.exe` (Windows).

## Architecture targets

Pick the one that matches your CPU. From oldest to newest:

| `ARCH=` | Targets | When to use |
|---|---|---|
| `x86-64-avx2` | Haswell+ (2013) | **default** — works on most modern CPUs |
| `x86-64-bmi2` | Zen 3+ / Ice Lake+ | adds fast PEXT for slider attacks |
| `x86-64-avxvnni` | Alder Lake+ | adds AVX-VNNI dpbusd intrinsics for FC dot products |
| `x86-64-avx512` | Skylake-X+ / Zen 4+ | full AVX-512 |
| `native` | local CPU | auto-detect via `-march=native`; not portable |

```
make build ARCH=x86-64-bmi2
```

Hypersion does not currently have a non-AVX2 (SSE2-only) target. If your
CPU lacks AVX2 you'll need to add one — the SIMD primitives in
`src/nnue.cpp` already have an SSE2 fallback path, but the build flags
need wiring.

## Other targets

| `make` target | Description |
|---|---|
| `make build` | default release build |
| `make` | alias for `build` |
| `make debug` | -O0 with `-fsanitize=address,undefined` |
| `make bench` | release build then run the deterministic bench |
| `make profile` | 2-pass PGO build (slow — ~5 min on Windows MinGW) |
| `make clean` | remove `obj/` and the binary |
| `make tuner` | build the Texel tuner at `tools/tuner/` |
| `make pgn_to_positions` | build the SAN-PGN-to-positions tool |
| `make help` | print these targets |

## Verifying the build

After `make build`, run the bench:

```
./Hypersion bench
```

Expected output ends with three lines like:

```
===========================
Total time : 1700 ms
Nodes      : 635067
Nodes/sec  : 370000
===========================
```

The **node count is deterministic** — if your `make build` matches the
released binary, you'll see exactly `635067` nodes at depth 13 (default
bench depth, NNUE on). Different node counts indicate either a build
flag mismatch or a search-affecting change.

If the NNUE files are missing, the bench still runs but on the classical
evaluator and the node count differs — `make bench` is only deterministic
once both `.nnue` files are present alongside the binary.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| `error: '__m256i' was not declared` | CPU/build flags don't include AVX2 — try `ARCH=native` or check your toolchain |
| `cannot open nn-...nnue` warning | NNUE files not present; engine falls back to classical eval (still works, much weaker) |
| `lto-wrapper.exe: serial compilation` | harmless GCC LTO message |
| Bench hangs forever | usually a debug build; use `make build` not `make debug` |
| `version mismatch` on NNUE load | your `.nnue` file is from a different SF format. Use the exact filenames listed in [NNUE.md](NNUE.md) |
