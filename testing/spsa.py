"""SPSA (Simultaneous Perturbation Stochastic Approximation) tuner — skeleton.

UNFINISHED: this is a SKELETON pending engine-side prerequisite work.

Status: requires the engine to expose its search constants as runtime
UCI options. Currently `search.cpp` has them as `constexpr int`. See
`spsa_README.md` for the full conversion guide.

Once those options are wired, this script will:
  1. Read `spsa_params.json` for the tunable parameter list.
  2. For each iteration:
       a. Pick a random perturbation vector.
       b. Spawn cutechess match: candidate (current+perturb) vs candidate (current-perturb)
       c. Score candidate for each parameter via the gradient direction.
  3. Update parameters by alpha * gradient * direction.
  4. Periodically dump tuned values; the user can paste them back into search.cpp.

Reference: Stockfish OpenBench fishtest SPSA — same algorithm, simpler harness.
"""

from __future__ import annotations

import argparse
import json
import random
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


HERE = Path(__file__).resolve().parent


@dataclass
class Param:
    name:    str
    default: float
    minimum: float
    maximum: float
    step:    float       # SPSA c (perturbation size)
    learn:   float       # SPSA a (learning rate)


def load_params(path: Path) -> list[Param]:
    """Load parameter spec from JSON. Schema:
       {"name": "RFP_MARGIN_PER_DEPTH", "default": 240, "min": 100, "max": 400, "step": 10, "learn": 5}
    """
    with path.open() as f:
        data = json.load(f)
    return [Param(p["name"], p["default"], p["min"], p["max"], p["step"], p["learn"])
            for p in data]


def perturb(params: list[Param], current: dict[str, float]) -> tuple[dict, dict, dict]:
    """Generate +delta and -delta perturbations.

    Returns (delta, plus_values, minus_values):
      delta[name]:        +1 or -1 (the random sign vector for this iteration)
      plus_values[name]:  current[name] + step * delta
      minus_values[name]: current[name] - step * delta
    """
    delta = {p.name: random.choice([-1, +1]) for p in params}
    plus  = {p.name: max(p.minimum, min(p.maximum, current[p.name] + p.step * delta[p.name]))
             for p in params}
    minus = {p.name: max(p.minimum, min(p.maximum, current[p.name] - p.step * delta[p.name]))
             for p in params}
    return delta, plus, minus


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--params", type=Path, default=HERE / "spsa_params.json",
                   help="Parameter spec JSON file")
    p.add_argument("--engine", type=Path, required=True,
                   help="Path to engine that supports the tunable UCI options")
    p.add_argument("--iters", type=int, default=10000,
                   help="Number of SPSA iterations")
    p.add_argument("--games-per-iter", type=int, default=8,
                   help="Games per perturbation pair")
    p.add_argument("--output", type=Path, default=HERE / "spsa_out.json")
    args = p.parse_args(argv)

    if not args.params.exists():
        print(f"ERROR: param spec missing: {args.params}", file=sys.stderr)
        print("Create spsa_params.json — see spsa_README.md for format.", file=sys.stderr)
        return 2

    print("SPSA tuner — SKELETON IMPLEMENTATION")
    print()
    print("Prerequisites NOT yet met:")
    print("  1. Engine must expose its constants as UCI options (see spsa_README.md)")
    print("  2. Need a per-iteration cutechess wrapper that sends `setoption` lines")
    print("  3. Need a result aggregator that converts game outcomes into a")
    print("     scalar gradient signal (-1 / 0 / +1 per game)")
    print()
    print("Current state: parameter loading is implemented, the SPSA loop is not.")
    print("Use `testing/sprt.py` for individual change validation in the meantime.")
    print()
    return 1


if __name__ == "__main__":
    sys.exit(main())
