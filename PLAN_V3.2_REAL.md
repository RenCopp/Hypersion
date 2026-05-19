# Hypersion v3.2 plan — VERDICT: NO SHIPPABLE CANDIDATES, v3.1 REMAINS CURRENT

## Outcome

All four proposed v3.2 candidates were tested against the v3.1-equivalent baseline at TC 5+0.05 conc=6. None produced a shippable ELO delta in clean rebuilds. v3.1 remains the latest verified Hypersion release.

| Phase | Candidate | 30g triage | 200g confirm | Verdict | Build mode |
|---|---|---|---|---|---|
| T1 | Berserk FMR eval damping | -23.2 ± 110.1 | **-24.4 ± 36.8** | REJECT (negative) | incremental — search.cpp only, layout-stable |
| T2 | Obsidian counter-move stage | +11.6 ± 107.2 | **+1.7 ± 38.5** | NEUTRAL (no-ship) | incremental — MovePicker layout change, possibly stale-affected |
| T3 | Alexandria root-only history | +70.4 ± 86.4 | **-5.2 ± 39.1** | REJECT (30g fakeout, regression at 200g) | incremental — Worker struct field add, likely stale |
| T4 | Berserk defender-status capture history | +94.9 → -58.5 ± 114 (clean) | **+45.4 → 0.0 ± 38.9 (clean)** | NEUTRAL after clean rebuild | clean rebuild required; stale-incremental gave false +45.4 |

## Critical lesson confirmed: stale-build SPRT artifacts

When the CaptureHistory struct dimension was bumped from `[12][64][7]` to `[12][64][2][7]` for T4, the incremental `make -j` produced a binary that gave +45.4 ± 39.7 ELO at 200g. After `make clean && make -j`, the **same source code** produced 0.0 ± 38.9 ELO at 200g. The stale build had `.o` files with the old layout linked against the new header — some translation units indexed at the old offsets, some at the new ones, producing a binary that LOOKED like it was performing well at bullet but was actually corrupt.

This matches CLAUDE.md's documented warning: *"Adding new struct fields to `src/eval_params.h` changes the binary ABI. Incremental `make` may NOT relink all object files."* The same rule applies to `src/history.h`, `src/search.h` (Worker), and any header with a struct field add.

**New rule, added to PROTOCOL.md and CLAUDE.md as a result of this session**: any SPRT that follows a struct layout change in a header MUST be preceded by `make clean && make -j`. The bench-determinism test alone (Threads=1 across processes) does NOT catch this — stale builds are still deterministic between processes; they're just running an internally-inconsistent binary.

T2 and T3 also involved layout-affecting header changes (MovePicker member add, Worker field add). Their 200g results (+1.7 and -5.2) may also be stale-build artifacts. The conclusion ("doesn't ship") is robust to that uncertainty — clean builds would either give similar noise or a similar reject. T1 (search.cpp body change only, no layout shift) is the only candidate where the SPRT result was unambiguously valid.

## Lessons applied

1. **Structural ports work IF they add new information** — v3.1's Tier 1 (ContCorrHist) and Tier 2 (ThreatSquareHistory) added genuinely new tables. T2 (counter-move stage), T3 (root-only history), T4 (defender-status capture-history) all RESHUFFLE OR PARTITION existing information — the SEE-vulnerability signal that defender-status would add is already captured by Hypersion's threat-by-lesser move-ordering bonus + 3D captureHist + see_ge gates. The partitioned signal split into smaller buckets, reducing the per-bucket data and net zeroing out.

2. **30g fakeouts are real at any +ELO band** — T3 showed +70.4 at 30g and -5.2 at 200g. T4 showed +94.9 at 30g (stale) and 0.0 at 200g (clean). Both 30g positives were well above the +50 "proceed to Stage 2" threshold; both collapsed. Future contributors: **never ship on 30g alone**.

3. **Clean rebuild before SPRT for every header struct change** — this session's most expensive lesson. The cost of `make clean && make -j` is 60-90 seconds of build time; the cost of a stale-build SPRT is hours of misdirected effort.

## Verified untried features that REMAIN untried (after this session)

All 4 candidates from the original plan were tested and rejected. The "untried features verified by source-grep" list is now exhausted for this round.

For a future v3.2+ attempt, the right direction per v3.1's lessons:
- (a) joint multi-feature SPSA over coordinated parameter groups (NMP + RFP + futility margins as one cluster)
- (b) a different NNUE network (search-NNUE coupling means net changes can compensate for tuning differences)
- (c) coordinated multi-feature ports (e.g., PawnHistory + butterfly weight + bonus formula together — NOT one at a time)

NOT: more single-feature SPSA campaigns or more single-table ports.

## v3.1 remains the shipped release

The binaries under `release/Hypersion-x86-64-avx2.exe` etc. are the canonical v3.1 builds. `src/misc.h::ENGINE_VERSION = "3.1"` is the source-of-truth version string. Bench at Threads=1 NNUE-on depth 13: 1,959,186 nodes (deterministic, matches release stripped binary).
