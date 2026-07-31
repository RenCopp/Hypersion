# Hypersion master improvement roadmap

**Prepared:** 2026-07-31
**Local authority:** audited checkout, branch `main`, HEAD `afc10b8`
**Public GitHub:** `RenCopp/Hypersion`, `main` at `356012d`
**Scope:** master plan plus local progress tracking

**Resource constraint (user decision):** custom NNUE generation/training and the
large diagnostic-telemetry program are deferred. Codex has no access to OpenAI's
internal training clusters or supercomputers for this repository. The active
roadmap must fit the user's existing local machine and available time.

## Executive decision

Hypersion should not begin another search-heuristic campaign yet.

The local engine is 40 commits ahead of GitHub, the public contribution guide
references a testing protocol that is not published, the advertised version and
bench signatures disagree with the current binary, and the repository contains
conflicting claims about whether `Threads >= 2` is stable. These are release and
measurement risks: they can invalidate Elo conclusions even when the underlying
engine change is sound.

The improvement program should therefore run in this order:

1. Establish one reproducible source-of-truth release.
2. Make the test harness publishable and statistically reliable.
3. Resolve lazy-SMP stability and UCI lifecycle races.
4. Re-baseline the current engine at the time controls the user actually plays.
5. Make low-cost correctness, build, and thread-scaling improvements.
6. Resume only narrowly selected search experiments under multi-TC gates.

The project history demonstrates that most literal Stockfish/Berserk/RubiChess
ports interact badly with Hypersion's search scale and local optimum. Without a
custom NNUE campaign, the realistic objective is a smaller but trustworthy gain
from reliability, SMP, performance, and carefully selected search changes.

## Evidence snapshot

### Repository drift

- Local `main` is 40 commits ahead of `origin/main`.
- Local HEAD is `afc10b8` (`R58b SHIP: NMP_EVAL_BETA_DIV 330 -> 250`).
- GitHub `main` ends at `356012d` (`Bump ENGINE_VERSION to 3.1`).
- The public repository therefore does not contain the v3.2-v3.6-era audit,
  diagnostic, tournament, and shipped-search work present locally.
- `src/eval_params.h` was initially marked modified by line endings only; it is
  now clean with no semantic parameter edit.
- Tags visible locally are `v1.0`, `v2`, `v2.1`, `v3.0`, and `v3.5`; `v3.5` is
  not represented on public GitHub because the containing commits are unpushed.

### Version and benchmark drift

- The clean local executable now reports `Hypersion 3.6.0-dev`.
- `src/misc.h`, the Makefile, README, and UCI metadata now agree on the
  development version and validated Threads=2 default.
- Git history contains a prior v3.1 version bump and later consolidation back
  to v3.0, while local tags and documents discuss v3.5/v3.6.
- A clean AVX2 rebuild establishes **1,875,591 nodes** as the deterministic
  Threads=1 NNUE bench signature at depth 13.
- `testing/BENCH_SIGNATURE` is now the single source of truth used by the smoke,
  regression, and `make verify` gates.
- Five consecutive bench runs within `make verify` matched the signature exactly.

### Test publication gap

- `.gitignore` now exposes the small reusable harness while continuing to ignore
  local match output, large suites, tuning data, and generated artifacts.
- The public `CONTRIBUTING.md` tells contributors to read
  `testing/PROTOCOL.md` and run scripts from `testing/`, but those files do not
  exist on GitHub.
- The core smoke/regression/SPRT scripts, protocol, README, and benchmark
  signature are staged locally for publication; they remain unavailable to
  external contributors until the local commits are intentionally pushed.
- Logs, PGNs, large datasets, binaries, and secrets should remain ignored; the
  scripts, protocols, small deterministic suites, and schemas should be tracked.

### SMP contradiction

- The public README says lazy-SMP works and `Threads=2` is a safe default.
- `src/uci.cpp` also defaults to 2 threads and calls it verified.
- `testing/README.md` says lazy-SMP is unstable and produces disconnects with
  `Threads>=2`; test scripts therefore force `Threads=1`.
- Until a repeatable stress test resolves this contradiction, the safe public
  default is one thread.

### Current strength evidence

The local history shows real gains, but most are bullet-heavy:

- R52 complexity-aware LMR: about +19 Elo at 400 games.
- R53 scale-corrected hindsight reduction: about +14 Elo at 400 games.
- R54 reverse-futility floor: about +40 Elo pooled at 400 games.
- R58 NMP eval/beta scaling: about +17 Elo at 400 games.
- R58b further NMP divisor change: about +12 Elo at 400 games.
- The v3.6 audit estimated a roughly 176-Elo gap to the reference-engine field
  before R58b, but this estimate is time-control and opponent-pool dependent.

Recent high-confidence rejections must be treated as closed unless a materially
new mechanism is proposed:

- R60 `ttCapture` LMR adjustment with cutoff-count guard: rejected.
- R61 SF18 double/triple singular-extension margins: rejected.
- R62 RubiChess adaptive history extension: rejected at all tested magnitudes.
- PawnHistory, LowPlyHistory, six-deep continuation history, root history,
  dedicated countermove stages, correction-value LMR, multi-source correction
  history, threat pruning, and multiple isolated eval additions are already
  tombstoned.

## Program principles

1. **One source of truth.** Every release has one commit, version, tag, bench
   signature, network pair, release note, and checksum set.
2. **Paired testing.** Every opening is played with colors reversed. Seeds,
   openings, engine options, binaries, CPU affinity, and concurrency are saved.
3. **Separate time-control objectives.** Bullet, blitz, and LTC are distinct
   products. A bullet win that loses at LTC is not a universal engine win.
4. **No 30-game shipping decisions.** Triage can stop disasters; it cannot prove
   a gain. Confirmation and replication are mandatory.
5. **Profile before optimizing.** NPS, cache behavior, branch misses, TT hit
   rates, pruning rates, and NNUE refresh cost must identify the bottleneck.
6. **Mechanism before magnitude.** A rejected mechanism is not revived by
   trying many nearby constants unless diagnostics show why the old test fired
   in the wrong regime.
7. **Correctness is not judged only by Elo.** UCI compliance, legal moves,
   repetition, rule-50 behavior, tablebase correctness, and crash freedom have
   their own release gates.
8. **Keep tombstones machine-searchable.** Every rejection records base/candidate
   SHAs, options, TC, concurrency, openings, seed, W/L/D, Elo, confidence, and
   links to raw artifacts.

## Phase 0 - Freeze and reconcile the project

**Priority:** blocking
**Effort:** 0.5-1 day
**Expected Elo:** none; protects every later measurement

### Work

1. Preserve the current local state before cleanup:
   - record HEAD `afc10b8` and binary hashes;
   - archive current NNUE hashes and UCI option output;
   - retain the line-ending-only working-tree change until deliberately fixed.
2. Decide the next public version. Recommended: call the reconciled release
   `v3.6.0` or `v3.7.0`; do not continue calling materially different binaries
   `3.0`.
3. Normalize line endings through `.gitattributes` (`*.cpp`, `*.h`, `*.md`,
   `Makefile`, YAML, and Python) so Windows tooling cannot dirty entire files.
4. Reconcile version sources:
   - one version file or generated constant;
   - UCI identity, Makefile, release filenames, changelog, and tag derive from it.
5. Review the 40 local commits for accidental secrets, giant artifacts, and
   private machine paths before publishing.
6. Choose publication history:
   - preserve all experiment commits if research traceability is the priority;
   - or create a curated release branch with shipped changes and a separate
     archive tag for the full experimental history.
7. Update GitHub issue #2 to reflect what is now shipped, rejected, or still open.
8. Close or supersede stale PR #1 after determining whether it still contains
   unique review value.

### Gate

- Clean working tree after intentional EOL normalization.
- `git status` clean, version consistent everywhere, no secret files tracked.
- GitHub and local `main` identify the same commit before new development.

## Phase 1 - Publish a reproducible validation kit

**Priority:** blocking
**Effort:** 2-4 days

### Repository layout

Track a small, stable validation subset:

```text
tests/
  README.md
  protocol.md
  smoke_test.py
  uci_state_test.py
  perft_positions.epd
  bench_positions.epd
  endgame_positions.epd
  sprt.py
  result.schema.json
tools/
  release/
  profiling/
artifacts/              # ignored
datasets/               # ignored
```

Do not publish bulk PGNs, raw logs, downloaded engines, proprietary opening
books, Lichess credentials, or NNUE binaries in Git history.

### Canonical baselines

1. Perform a clean build from HEAD for every supported architecture.
2. Record:
   - compiler and version;
   - exact compile/link flags;
   - CPU model and enabled ISA;
   - NNUE SHA-256 hashes;
   - Threads, Hash, Syzygy, book, persistence, and analysis-mode settings;
   - bench nodes and median time across at least seven runs.
3. Generate `bench-baselines.json` keyed by engine commit, architecture,
   compiler family, network hashes, and bench depth.
4. Make `make verify` read this data or generate an explicit error explaining
   why no baseline applies. Do not keep unexplained hard-coded node counts in
   multiple documents.

### Statistical protocol revision

1. Keep 30-game runs as disaster filters only.
2. Use actual SPRT boundaries for confirmation, with a maximum game cap large
   enough to decide small gains.
3. Require at least one independent replication (new seed/opening offset) for
   gains below roughly +15 Elo.
4. Preserve color-paired openings and report pentanomial statistics where the
   harness supports them.
5. Run null tests regularly: identical binaries should not systematically
   accept H1.
6. Split result tracks:
   - bullet: current fast TC;
   - blitz: a middle control such as 10+0.1 or a chosen project standard;
   - LTC: 60+0.6 or the chosen project standard;
   - memory-sensitive: lower concurrency and pinned CPU affinity.
7. Save a machine-readable manifest beside every result.

### CI expansion

Minimum pull-request CI:

- Linux GCC release build.
- Linux Clang release build.
- Linux ASan+UBSan debug build.
- Windows MSYS2 release build.
- x86-64 baseline, AVX2, and BMI2 compile checks.
- UCI handshake and option parsing.
- legal bestmove smoke tests.
- deterministic Threads=1 bench.
- standard and Chess960 perft suites.
- endgame and tablebase-free regression cases.
- CodeQL/static analysis.

Nightly or scheduled CI:

- ThreadSanitizer where supported.
- randomized UCI state-machine stress.
- fuzzed FEN/UCI parsing.
- deeper perft.
- compiler/architecture matrix including AVX-VNNI and AVX-512 compile tests.
- optional NNUE tests using a downloaded, hash-verified network cache.

### Gate

- A new contributor can clone GitHub and run documented tests without access to
  the creator's local `testing/` directory.
- README and CONTRIBUTING contain no broken internal links.
- The canonical clean-build bench passes in CI and locally.

## Phase 2 - Resolve lazy-SMP and UCI lifecycle correctness

**Priority:** critical
**Effort:** 3-7 days

### Immediate safety action

Until this phase passes, set the documented and UCI default to `Threads=1`.
Users may opt into more threads, but the engine should not claim two threads are
safe while local documentation says they disconnect.

### Reproduction harness

**Progress (2026-07-31):** the bounded cross-platform harness is now in
`testing/test_uci_lifecycle.py`. It deterministically reproduced a `Clear Hash`
deadlock after `go infinite` and an access violation during shared-resource
reconfiguration. UCI now stops and joins workers before TT, NNUE, Syzygy,
persistence, or runtime-tuning mutations. A second regression found that EOF
during `go infinite` waited forever; EOF now uses the same stop-and-join path as
clean shutdown. The five-scenario suite includes deterministic fixed-seed mixed
command ordering. Release validation passed 500 resize/new-game cycles plus 500
mixed sequences. ASan+UBSan passed 200+200 corresponding cycles with classical
fallback and 50+50 with NNUE enabled, all without a sanitizer report. The
Makefile's parallel debug-build race is also repaired and the matching Clang64
toolchain is installed. The 10,000-search/thread-count long-soak gate below is
now complete. Bounded fixed-node and paired-match scaling evidence was added on
2026-07-31, allowing the default to return to Threads=2.

The harness now asserts exactly one syntactically legal `bestmove` per `go`.
An option-contract audit also found that Threads, MultiPV, and Move Overhead did
not enforce their advertised spin bounds; all three setters now clamp to their
published ranges. The six-case release suite passed 200+200 lifecycle/mixed
cycles, and the same suite passed 100+100 cycles under ASan+UBSan.

The fixed-thread soak gate is now implemented in `testing/test_smp_soak.py`.
Release mode passed **10,000 searches each** at Threads 1, 2, 4, and 8 (40,000
total), with exactly 40,000 legal-form bestmoves and no disconnect or lifecycle
failure. ASan+UBSan passed a further 1,000 searches at each count. CI runs a
100-search-per-count sentinel. This completes the lifecycle portion of the SMP
gate; the bounded strength/scaling evidence immediately below completes the
remaining default-policy requirement.

The follow-up scaling gate measured median fixed-node throughput of 494k, 1.07M,
1.53M, and 2.48M NPS at Threads 1, 2, 4, and 8 respectively. Threads=2 then
beat Threads=1 in two independent 20-game paired-opening bullet samples:
14-4-2 and 12-5-3 (combined 26 wins, 9 losses, 5 draws). This is bounded
validation rather than a broad strength campaign, but it confirms both direction
and reproducibility strongly enough to restore the default to Threads=2.

The remaining UCI input-boundary audit reproduced two additional Windows access
violations: malformed FEN could drive `Position::set` outside the board array,
and histories beyond 2,047 moves overran the global `StateInfo` chain. UCI now
strictly validates six-field FEN before parsing and compacts an overlong history
through the current FEN before continuing. Both regressions pass in release and
ASan+UBSan builds.

Build a stress runner that performs thousands of randomized sequences:

- `uci`, `isready`, `ucinewgame`;
- `position` with startpos and arbitrary legal FENs;
- `go depth`, `go nodes`, `go movetime`, clock-based `go`, and `go infinite`;
- `stop`, `ponderhit`, immediate second `go`, EOF, and `quit`;
- `setoption Threads`, `Hash`, `Clear Hash`, NNUE files, and persistence between
  searches;
- Threads 1, 2, 4, 8, and repeated resize cycles.

Assertions:

- exactly one legal `bestmove` per completed search;
- no output after final shutdown;
- no deadlock, disconnect, crash, invalid move, or use-after-free;
- clean exit within a fixed timeout;
- helper workers stop and join before mutable shared resources are resized.

### Code audit targets

1. `ThreadPool::start`, `stop_all`, `wait_all`, and `set_size` ordering.
2. Main-worker/helper publication of global stop and ponder flags.
3. TT resize/clear and history clear while workers may still be active.
4. `go` received while a prior search is active.
5. `isready` behavior during expensive initialization or file loading.
6. Position/state copying and stack lifetimes across worker launches.
7. Shared TT entry accesses and any C++ data races tolerated by design.
8. Persistent correction-history load/save during game transitions.
9. Main-thread output ownership and helper suppression.

### Performance gate

After correctness, measure scaling at fixed wall-clock and fixed-node modes:

- Threads 1 -> 2 -> 4 -> 8;
- NPS, depth reached, Elo, TT hit rate, and stop latency;
- single-engine and concurrent-match workloads.

Do not keep extra threads enabled merely because aggregate NPS rises; they must
improve playing strength and remain stable.

### Gate

- Sanitizers clean.
- At least 10,000 mixed UCI searches at each supported thread count with no
  disconnect or lifecycle failure.
- Multi-thread strength/scaling report published.
- README, code comments, and UCI defaults agree.

## Phase 3 - Large telemetry program (deferred by user)

**Status:** deferred; not part of the active execution queue
**Reason:** it requires development time, long data-collection runs, and storage

The following remains a reference design only. Do not implement it unless the
user explicitly reactivates this phase or a future bug cannot be diagnosed with
small, temporary counters.

Potential compile-time or UCI-debug telemetry:

### Search counters

- nodes by depth, ply, PV/non-PV, cut/all node;
- branching factor and move index of cutoffs;
- TT probe/hit/cutoff/replacement rates;
- null-move attempts, cutoffs, verification searches, and zugzwang rejects;
- RFP, futility, razoring, ProbCut, SEE, LMP, and history-pruning firings;
- LMR base reduction and every additive adjustment distribution;
- singular-extension attempts and outcomes;
- qsearch move counts, SEE rejects, and stalemate/repetition outcomes;
- NNUE big/small network usage, full refreshes, incremental updates, and Finny
  hit/miss rates;
- time-manager stop reason and unused/overrun time.

### Error-analysis pipeline

For sampled self-play and field games, save per move:

- FEN, side, phase, material class, move, search depth, nodes, score, and PV;
- score from a stronger reference at a fixed budget;
- score swing and whether it persists for several plies;
- color, opening family, game result, and clock state;
- Syzygy WDL/DTZ where available.

Produce dashboards for:

- White/Black asymmetry with confidence intervals;
- 1.e4-as-Black results;
- middlegame drift versus endgame conversion;
- flag-outs and time overspend;
- eval residual by phase and material;
- tactical misses grouped by pruning mechanism.

### Gate

- Every proposed search change states which measured failure mode it addresses.
- Experiments can report how often the changed condition fired and at which
  depths/TCs.

## Phase 4 - Re-baseline playing strength

**Priority:** high
**Effort:** 2-4 machine-days, mostly unattended

Build a stable field with versioned binaries and equal settings:

- Hypersion current release candidate;
- its immediately previous release;
- Stockfish reference;
- Berserk, Obsidian, RubiChess, Alexandria, and one mid-strength control;
- identical Threads, Hash, Syzygy policy, ponder policy, and opening pairs.

Run separate bullet, blitz, and LTC gauntlets. Report:

- Elo with confidence intervals;
- W/L/D and pentanomial data;
- per-color and per-opening results;
- crash/timeout/illegal-move counts;
- average depth, nodes, NPS, and time usage;
- version and binary hashes.

This becomes the baseline for all remaining phases. The old `-176`, `-266`,
or `-300` gap figures should be treated as historical until reproduced against
the reconciled release.

**Bounded progress (2026-07-31):** paired-opening v3.6-v3.5 sentinels completed
at bullet (8-6-6), blitz (2-6-2), and LTC (4-0-2). Their confidence intervals
are too wide for a release Elo claim, but no consistent catastrophic regression
appeared. A larger field campaign belongs to the explicitly excluded large-data
track; raw local PGN/log artifacts remain ignored.

## Phase 5 - Hypersion-specific NNUE program (deferred by user)

**Status:** deferred; not part of the active execution queue
**Effort if reactivated:** 3-8 weeks depending on compute
**Expected Elo:** uncertain; no gain is assumed in the active roadmap

Codex cannot use OpenAI's private model-training supercomputers for Hypersion.
This phase would require user-provided local/cloud GPU resources or a future
community contributor who volunteers the compute. The material below is kept
only so the project has a technically coherent path if resources appear later.

### 5.1 Loader and format hardening

Before training:

- verify architecture/version/hash metadata, not filenames alone;
- print clear diagnostics for wrong architecture or corrupt files;
- expose loaded network hashes in UCI/debug output;
- add known-position inference tests against a reference implementation;
- retain the current SF18 network as a frozen baseline.

### 5.2 Data generator

Add a dedicated generator rather than scraping tournament PGNs:

- output a format accepted by the chosen training pipeline;
- use fixed, recorded engine commit and network hashes;
- randomize openings while preserving color symmetry;
- filter terminal, duplicate, illegal, and trivially tablebase-resolved samples;
- include score, ply, result/WDL target, side to move, rule-50 state, and move;
- sample positions to avoid overrepresenting quiet adjacent plies;
- support deterministic shards and resumable generation;
- validate every shard with checksums and schema/version metadata.

### 5.3 Progressive corpus

Use staged investment:

1. **Pilot:** 5-10 million positions to prove the pipeline and loader.
2. **Candidate:** 50-100 million balanced positions.
3. **Mature:** hundreds of millions or more if validation continues improving.

Mix data sources deliberately:

- Hypersion self-play at bullet, blitz, and LTC search budgets;
- positions near observed Hypersion mistakes;
- balanced public high-quality data where licensing permits;
- endgame/tablebase examples;
- tactical oversampling capped to prevent distribution distortion.

### 5.4 Training strategy

First preserve the existing HalfKAv2_hm + FullThreats architecture so inference
code and search remain comparable. Train multiple seeds and use a held-out set
that is split by game, not random adjacent positions.

Evaluate:

- training/validation loss and calibration;
- color symmetry;
- saturation/overflow statistics after quantization;
- tactical suites and endgame sets;
- fixed-node self-play against the frozen SF18 net;
- bullet, blitz, and LTC SPRT;
- NPS and accumulator-refresh cost.

Do not tune search constants while selecting the first network. Lock the best
network first, then open a new search-tuning baseline.

### 5.5 Promotion gate

A network ships only if it:

- beats the current network at two time controls;
- does not create a major color asymmetry;
- passes legal/correctness/quantization tests;
- has reproducible training metadata and an immutable hash;
- retains or improves endgame and tactical behavior.

## Phase 6 - NNUE and CPU performance engineering

**Priority:** high after instrumentation
**Effort:** 2-4 weeks

### Candidate A: incremental FullThreats

The local audit notes that threat features are not fully incremental. Port the
dirty-threat diff/update mechanism while preserving exact feature indices and
inference output.

Gate:

- byte/exact score equality over a large randomized position corpus;
- identical legal search behavior at fixed nodes where expected;
- measured reduction in full refreshes and NNUE time;
- NPS gain on AVX2 and AVX-VNNI;
- memory-sensitive match at low concurrency.

### Candidate B: sparse L1 inference

Prototype non-zero activation tracking and sparse accumulation for AVX2 first.
Add AVX-VNNI/AVX-512 only after the scalar/AVX2 reference is correct.

Gate:

- exact quantized outputs versus dense inference;
- wins on representative positions, not just a synthetic microbenchmark;
- code-size and cache behavior measured under concurrent matches;
- no regression on CPUs where sparsity overhead exceeds savings.

### Candidate C: thread-local layout and cache isolation

Profile Worker history tables, Finny cache, TT, and NNUE accumulators. Consider:

- cache-line alignment and false-sharing removal;
- reducing per-thread table footprint where hit-rate data supports it;
- NUMA-aware TT placement for large systems;
- huge pages as an opt-in feature;
- prefetch distance tuning;
- cold-path separation to reduce instruction-cache pressure.

### Candidate D: architecture support

- make the existing `x86-64` target real and test it; current BUILDING docs say
  it is missing while the Makefile already contains it;
- add compile-only checks for AVX-VNNI and AVX-512;
- evaluate ARM64/NEON only after x86 release discipline is stable;
- consider runtime dispatch later, but keep separate binaries if it remains
  simpler and faster.

## Phase 7 - Search optimization after the network is locked

**Priority:** conditional
**Effort:** ongoing

### What not to repeat

Do not retry R60-R62, PawnHistory, LowPlyHistory, root history, six-deep
continuation history, correction-value LMR, multi-source correction history,
or previously rejected eval additions without a new diagnostic mechanism.

### 7.1 Joint tune the proven cluster

The most defensible first campaign is a coordinated tune of currently shipped
interacting parameters:

- R52 complexity threshold;
- R53 hindsight threshold;
- R54 RFP floor;
- R58/R58b NMP eval-beta divisor;
- only directly interacting LMR/NMP terms supported by existing experiment
  evidence or small temporary counters.

Rules:

- tune at actual time controls, not fixed-node proxies alone;
- use a multi-objective score across bullet and LTC, or maintain explicit
  separate profiles rather than hiding large behavior changes behind an
  internal 500 ms threshold;
- validate the final joint candidate against the pre-tune baseline at every
  target TC;
- replicate small gains.

### 7.2 Current-reference delta audit

Create a fresh semantic inventory against the current chosen reference engine,
but classify every difference before coding:

- correctness fix;
- search mechanism absent;
- parameter-only difference;
- network-architecture coupling;
- performance-only change;
- already tombstoned equivalent.

Only correctness fixes and evidence-supported missing mechanisms enter the
experiment queue. Literal constants must be normalized by eval/history scales
and, where cheap to measure, firing-rate distributions—not a single global ratio.

### 7.3 Correctness branch

Maintain correctness fixes separately from Elo experiments:

- qsearch stalemate handling;
- rule-50 edge cases;
- repetition and upcoming-repetition tests;
- Chess960 castling round trips;
- Syzygy probe fallbacks and the Fathom KX-vs-K workaround;
- illegal/corrupt FEN handling.

Correctness changes should not be deleted merely because their Elo effect is
neutral. If a correct behavior is measurably expensive, optimize the mechanism
or document a standards tradeoff explicitly.

## Phase 8 - Time management and game-quality work

**Priority:** medium-high
**Effort:** 1-2 weeks

1. Replace anecdotal flag-out diagnosis with a corpus of clock traces.
2. Record stop reason, optimum/maximum budget, elapsed time, score volatility,
   best-move stability, node distribution, and remaining clock.
3. Build deterministic time-manager unit tests for:
   - zero increment;
   - network overhead;
   - first moves out of book;
   - ponderhit;
   - sudden score collapse;
   - forced moves;
   - low-time panic;
   - increment-only survival.
4. Tune time management independently for local GUI play and Lichess latency.
5. Keep `Move Overhead` user-configurable and document realistic bot values.
6. Validate time-manager changes on flag rate and game Elo at bullet, then
   confirm they do not waste time at LTC.

**Bounded progress (2026-07-31):** `make test_timeman` now asserts exact budgets
for movetime, overhead, sudden death, low-time panic, increment, ponder, explicit
moves-to-go, and all non-clock search modes. Large clock-trace collection remains
deferred with the telemetry program.

## Phase 9 - Endgames, tablebases, and practical play

**Priority:** medium

- Build a curated endgame regression set from actual Hypersion losses.
- Report conversion rate, DTZ regret, move count, and rule-50 outcome.
- Test Syzygy on/off and 3/4/5/6-piece configurations where available.
- Track the KQK helper-API hang against its actual Pyrrhic lineage; retain the
  narrow defensive workaround until a full dependency migration is justified.
- Separate classical-eval endgame work from NNUE-shipping work: classical WAC
  gains do not establish NNUE Elo gains.
- Revisit opening-book policy only after testing with book disabled and paired
  openings, so book variance does not masquerade as search strength.

**Progress (2026-07-31):** the portable `test_endgame_conversion.py` records
start DTZ, rule-50 feasibility, move count, clock remainder, and outcome. With
local 3-5-piece Syzygy tables, all 5 feasible KQK/KRK/KBPvK/KBNvK wins converted;
one KRK position with `rule50=85` and `DTZ=23` was correctly classified as an
unavoidable rule-50 draw. Latest Pyrrhic master returns normally on the original
KQK reproducer, so no stale upstream issue was filed. Hypersion's older helper
layer still retains the KQK-only skip; KRK now receives DTZ guidance.

## Phase 10 - Release and community engineering

**Priority:** high once Phase 0-2 pass

### Release automation

Produce from a clean tagged commit:

- Windows x86-64, AVX2, BMI2, AVX-VNNI, and AVX-512 binaries;
- Linux equivalents where supported;
- validated x86-64 baseline;
- SHA-256 checksums;
- source archive;
- network download manifest with hashes;
- UCI option dump;
- benchmark manifest;
- release notes generated from structured experiment records.

**Local progress (2026-07-31):** `tools/build_release.py` now checks the shared
version, refuses dirty release inputs by default, clean-builds all five portable
x86-64 variants, strips them, records the commit/toolchain/bench/UCI/network and
book metadata, writes `SHA256SUMS`, creates a clean-commit source archive, and
restores the default development build. All five variants have compiled
successfully on Windows MSYS2. Publication from a clean tagged commit remains
an explicit external release action.

### GitHub presentation

Update the public page to show:

- current, unambiguous version;
- actual supported platforms and CPU requirements;
- honest thread-stability status;
- current strength numbers with TC, opponent pool, games, and confidence;
- separate bullet and LTC claims;
- reproducible build/test commands;
- roadmap links and contribution-ready issues.

Create scoped issues for Phase 0-6 work. Each issue should state evidence,
definition of done, required tests, and files likely involved.

## Active six-week execution sequence (resource-constrained)

| Week | Focus | Exit condition |
|---|---|---|
| 1 | Phase 0 repository reconciliation | Local/GitHub source and version agree |
| 2 | Phase 1 publishable tests + bench baseline | Clean clone passes documented suite |
| 3 | Phase 2 SMP reproduction and safety fixes | Stress harness explains disconnects |
| 4 | Phase 2 SMP verification + CI | Threads policy backed by data |
| 5 | Phase 4 compact field re-baseline | Bullet/blitz/LTC gaps measured cleanly |
| 6 | Low-cost correctness/performance candidate | Multi-TC result or release candidate |

## Stop conditions

Stop a line of work when any of these holds:

- two independent confirmations show a clear regression;
- the mechanism duplicates an existing tombstone without new evidence;
- a bullet gain is offset by an equal or larger LTC loss and no explicit
  bullet-only product profile is desired;
- the candidate depends on an unreproducible build or stale binary;
- performance improves in a microbenchmark but loses under realistic
  concurrent matches;
- a network cannot be reproduced from recorded data/config/checkpoints;
- three consecutive experiments in one family fail after sound diagnostics.

## Active task queue

- [x] Add scoped `.gitattributes` rules without rewriting legacy source files.
- [x] Resolve the line-ending-only `src/eval_params.h` diff without changing
  evaluation parameters.
- [x] Apply the canonical `3.6.0-dev` version across source/build/docs.
- [x] Stage the non-secret core testing harness for publication under `testing/`.
- [x] Clean-build HEAD and establish one benchmark signature.
- [x] Change the default to Threads=1 until SMP validation passes.
- [x] Restore Threads=2 after lifecycle, throughput, and replicated paired-match
  validation passed.
- [x] Add the bounded UCI lifecycle regression and repair the reproduced
  deadlock/access violation.
- [x] Complete randomized sanitizer and 10,000-search-per-count SMP lifecycle
  validation for Threads 1/2/4/8.
- [x] Add and locally validate checksummed five-architecture release tooling.
- [x] Align portable build documentation and CI with baseline, GCC, Clang, and
  sanitizer coverage.
- [x] Measure bounded multi-thread playing-strength and wall-clock scaling;
  restore the validated Threads=2 default.
- Reconcile/publish the 40 local commits and update GitHub issue #2.
- [x] Run the explicitly approved compact bullet/blitz/LTC v3.6-v3.5 baseline;
  retain raw results locally and avoid overclaiming the small samples.

## Definition of success

The roadmap succeeds when Hypersion has:

- a public repository matching the strongest local engine;
- reproducible releases and test results;
- stable, measured multi-thread behavior;
- truthful bullet/blitz/LTC strength claims;
- performance improvements proven under realistic workloads;
- a smaller experiment queue containing only mechanisms not already disproven.

The first milestone is not an Elo number. It is a trustworthy v3.x baseline.
Once that exists, every Elo gained afterward becomes believable and durable.
