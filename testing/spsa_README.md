# SPSA tuning — setup notes

SPSA (Simultaneous Perturbation Stochastic Approximation) lets you tune
many search constants at once by running games with each parameter
slightly perturbed and updating in the gradient direction. It runs on
CPU, no GPU needed, and continues unattended for days.

## Status: scaffolding only

The harness skeleton is here (`spsa.py` is a placeholder), but the
engine itself does NOT yet expose its tuning constants via UCI options.
That's the prerequisite work that needs to land before SPSA does
anything useful.

## Prerequisite: convert search constants to runtime UCI options

Currently `src/search.cpp` has a block of `constexpr int` values. To
make them tunable, change them to global `int` (or `inline int`) and
register UCI `option name X type spin default V min A max B` for each.
The existing `Options` struct in `uci.cpp` is the place to add them.

Candidate constants that move ELO when re-tuned (in priority order):

| Name                       | Default | Min | Max  | Step | Source line     |
|---------------------------|--------:|----:|-----:|-----:|-----------------|
| RFP_MARGIN_PER_DEPTH      | 240     | 100 |  400 |   10 | search.cpp:119  |
| RAZOR_MARGIN_BASE         | 720     | 300 | 1200 |   20 | search.cpp:120  |
| RAZOR_MARGIN_PER_DEPTH    | 390     | 200 |  600 |   10 | search.cpp:121  |
| FUTIL_MARGIN_PER_DEPTH    | 330     | 200 |  500 |   10 | search.cpp:122  |
| FUTIL_MARGIN_BASE         | 390     | 200 |  600 |   10 | search.cpp:123  |
| SEE_QUIET_MARGIN          | -180    |-300 |  -50 |    5 | search.cpp:124  |
| SEE_CAPT_MARGIN           | -300    |-500 | -100 |    5 | search.cpp:125  |
| NMP_EVAL_BETA_DIV         | 600     | 300 |  900 |   10 | search.cpp:126  |
| PROBCUT_MARGIN            | 600     | 300 |  900 |   10 | search.cpp:127  |
| ASPIRATION_DELTA0         | 51      |  10 |  150 |    2 | search.cpp:128  |
| LMR_DIVISOR  (×100)       | 190     | 150 |  220 |    2 | search.cpp:53   |

(LMR_DIVISOR is the float divisor in `init()` — the engine would store
it as `int / 100.0`.)

## SPSA workflow once UCI options exist

1. Set baseline: take a clean release build, rename to
   `dist\Hypersion_spsa_baseline.exe`.
2. Define search space in `spsa_params.json`.
3. Run `py testing/spsa.py --hours 48` — it will spawn cutechess matches
   with `setoption name X value V` for each perturbed run.
4. Apply final tuned values back into the constants and rebuild.
5. SPRT the SPSA result against the baseline at long TC.

## Why we're deferring this

Three reasons:
1. The constants block is constexpr; converting them to runtime options
   touches 20+ lines and requires a UCI option spec for each.
2. SPSA needs a mature SPRT harness (`sprt.py` ✓ present) plus an
   evaluator wrapper that swaps options between candidates. Not yet
   built.
3. Search refinements (LMR/NMP) just changed in the current branch.
   Tuning on a moving baseline is wasteful — wait until the next
   architectural changes settle, then SPSA.

## Estimated ELO from SPSA

Once the prerequisites are in place, expect +10–30 ELO over a 1–2 week
unattended run, depending on how stale the manually-tuned constants
were. This is a slow-burn improvement — easy to set up once and then
let cook in the background.
