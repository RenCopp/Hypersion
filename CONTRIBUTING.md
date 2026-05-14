# Contributing to Hypersion

Thanks for the interest. This document describes how to build, test,
and propose changes to Hypersion. Read `CLAUDE.md` at the repo root
for the agent-oriented overview and `testing/PROTOCOL.md` for the
SPRT testing rules.

## Quick start (build)

Requires MSYS2 MinGW64 on Windows or `g++ 12+` on Linux.

```
make -j                      # default: AVX2 release build
./Hypersion bench 13         # quick correctness + NPS check
```

Run `setoption name Threads value 1` before `bench` if you want a
deterministic node count (1,273,328 nodes T1 d=13 is the v3.1
baseline).

## Where the code lives

- `src/` — engine (UCI, search, eval, NNUE, syzygy, book, ...)
- `tools/tuner/` — Texel-style classical-eval tuner + PGN-to-positions extractor
- `testing/` — SPRT runner, tactical suites, IMPROVEMENT_LOG, plans (gitignored)
- `.github/workflows/` — CI (Build + CodeQL)
- `src/fathom/` — vendored Syzygy probe library (BSD, basil00/Fathom)

## How to test a change

1. **Build**: `make -j` succeeds with no warnings.
2. **Bench preservation**: `./Hypersion bench 13` outputs
   `Nodes searched : 1273328` (or the PR explains why bench shifted).
3. **WAC tactical**: `py testing/wac_runner.py --depth 8 --no-nnue`
   solves ≥ 178/198 (current baseline is 184).
4. **SPRT** for any ELO-affecting search change: 200 games at TC
   5+0.05 conc=2 vs the immediate previous release.
   See `testing/PROTOCOL.md` for the full protocol.

## Tombstoning failed experiments

If an experiment regresses, leave a source-inline tombstone comment
with the measured ELO ± CI, sample size, and reason. The repo has
many such tombstones (`grep -rn "NOTE: tested" src/`). This prevents
the same regression from being rediscovered repeatedly.

## Style

- C++20, `-Wall -Wextra -Wcast-qual -Wshadow -pedantic`.
- 4-space indent, no tabs.
- `clang-format` not enforced; match the surrounding file style.
- ASCII source — no emoji or non-ASCII characters in code.
- No new files unless they significantly improve organisation.

## Reporting bugs

Use the **Bug report** issue template. Include:
- Hypersion version (`uci` command output) and architecture
- Operating system + compiler
- Reproduction: minimal UCI command sequence
- Expected vs actual behaviour

For security vulnerabilities, use **GitHub's Private Vulnerability
Reporting** (see `SECURITY.md`), not a public issue.

## Pull requests

1. Fork + branch from `main`
2. Make the change + commit with descriptive message
3. Run the test matrix above
4. Push + open a PR using the template
5. CI must pass (Build + CodeQL workflows)

For small changes (typo, comment, doc), label the PR `trivial` so
SPRT confirmation isn't expected.

For non-trivial search/eval changes, include the SPRT result in the
PR description. PRs without SPRT data on search/eval changes will be
asked to provide one before merge.

## Areas where help is welcome

Per the current `README.md` project status:
- **NNUE retrain** on Hypersion self-play data — the
  highest-leverage open task. Requires GPU + multi-week effort.
- **PR audit** for SF-master features added since SF18 that haven't
  been tested in Hypersion yet.
- **Test infrastructure** improvements (e.g., a proper cutechess
  Docker image for CI gauntlets).

## License

Contributions are accepted under the same license as the project
(see `LICENSE`).
