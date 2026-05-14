## Summary

<!-- 1-3 sentences: what does this PR do and why? -->

## Type of change

- [ ] Bug fix (correctness, no behaviour change otherwise)
- [ ] New feature (search heuristic, eval term, build option, etc.)
- [ ] Refactor / cleanup (no functional change)
- [ ] Documentation / community files
- [ ] Tombstone (records a failed experiment per CONTRIBUTING.md)

## Verification

- [ ] `make -j` builds without new warnings
- [ ] `./Hypersion bench 13` (Threads=1) produces the expected node count
- [ ] WAC depth-8 classical: `py testing/wac_runner.py --depth 8 --no-nnue` ≥ 178/198
- [ ] CI Build + CodeQL workflows pass

### For search/eval changes only

- [ ] 200-game SPRT result attached below (TC 5+0.05 conc=2 vs prior baseline)
- [ ] OR PR is labeled `trivial` (no SPRT expected)

```
<!-- paste SPRT summary here: W/L/D, ELO ± CI -->
```

## Bench

```
NNUE bench T1 d=13: <node count>
```

If the bench changed, briefly explain why.

## Notes for reviewer

<!-- anything else: tombstone history, related issues, follow-up work -->
