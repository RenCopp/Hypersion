#!/usr/bin/env python3
"""Build reproducible Hypersion binaries and write checksummed metadata."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_ARCHES = (
    "x86-64",
    "x86-64-avx2",
    "x86-64-bmi2",
    "x86-64-avxvnni",
    "x86-64-avx512",
)
ASSET_NAMES = (
    "nn-c288c895ea92.nnue",
    "nn-37f18f62d772.nnue",
    "Perfect2023.bin",
)


def run(command: list[str], *, env: dict[str, str], capture: bool = False) -> str:
    result = subprocess.run(
        command,
        cwd=ROOT,
        env=env,
        check=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.STDOUT if capture else None,
    )
    return result.stdout if capture else ""


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def read_version() -> str:
    makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
    header = (ROOT / "src" / "misc.h").read_text(encoding="utf-8")
    make_match = re.search(r"^VERSION\s*=\s*(\S+)\s*$", makefile, re.MULTILINE)
    header_match = re.search(r'ENGINE_VERSION\s*=\s*"([^"]+)"', header)
    if not make_match or not header_match:
        raise RuntimeError("could not read version from Makefile and src/misc.h")
    if make_match.group(1) != header_match.group(1):
        raise RuntimeError(
            f"version mismatch: Makefile={make_match.group(1)} "
            f"src/misc.h={header_match.group(1)}"
        )
    return make_match.group(1)


def git_metadata(env: dict[str, str]) -> dict[str, object]:
    git = shutil.which("git", path=env.get("PATH"))
    if git is None:
        return {"commit": None, "dirty": None}
    try:
        commit = run([git, "rev-parse", "HEAD"], env=env, capture=True).strip()
        status = run(
            [git, "status", "--porcelain", "--untracked-files=normal"],
            env=env,
            capture=True,
        )
    except (OSError, subprocess.CalledProcessError):
        return {"commit": None, "dirty": None}
    return {"commit": commit, "dirty": bool(status.strip())}


def create_source_archive(destination: Path, version: str, env: dict[str, str]) -> dict[str, object]:
    git = shutil.which("git", path=env.get("PATH"))
    if git is None:
        raise RuntimeError("git is required to create the source archive")
    run(
        [
            git,
            "archive",
            "--format=tar.gz",
            f"--prefix=Hypersion-{version}/",
            f"--output={destination}",
            "HEAD",
        ],
        env=env,
    )
    return {
        "file": destination.name,
        "size": destination.stat().st_size,
        "sha256": sha256(destination),
    }


def tool_environment() -> tuple[dict[str, str], str, str, str]:
    env = os.environ.copy()
    make = os.environ.get("MAKE", "make")
    compiler = "g++"
    strip = "strip"
    if os.name == "nt":
        msys = Path(os.environ.get("MSYS2_ROOT", r"C:\msys64"))
        mingw_bin = msys / "mingw64" / "bin"
        usr_bin = msys / "usr" / "bin"
        if mingw_bin.is_dir() and usr_bin.is_dir():
            env["PATH"] = os.pathsep.join((str(mingw_bin), str(usr_bin), env["PATH"]))
            make = os.environ.get("MAKE", str(usr_bin / "make.exe"))
            compiler = str(mingw_bin / "g++.exe")
            strip = str(mingw_bin / "strip.exe")
    return env, make, compiler, strip


def capture_uci(binary: Path, env: dict[str, str]) -> dict[str, object]:
    result = subprocess.run(
        [str(binary), "--no-nnue-default"],
        cwd=ROOT,
        env=env,
        input="uci\nquit\n",
        check=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=30,
    ).stdout
    identity = next((line for line in result.splitlines() if line.startswith("id name ")), None)
    options = [line for line in result.splitlines() if line.startswith("option name ")]
    if identity is None or not options:
        raise RuntimeError("built binary did not return complete UCI metadata")
    return {"identity": identity, "options": options}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--architectures", nargs="+", default=list(DEFAULT_ARCHES))
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--allow-dirty", action="store_true")
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--no-restore", action="store_true")
    args = parser.parse_args()

    env, make, compiler, strip = tool_environment()
    version = read_version()
    output_dir = (args.output_dir or ROOT / "dist" / f"Hypersion-{version}").resolve()
    git = git_metadata(env)
    if git["dirty"] and not args.allow_dirty:
        parser.error("working tree is dirty; commit/stash it or pass --allow-dirty")

    output_dir.mkdir(parents=True, exist_ok=True)
    expected = [
        output_dir / f"Hypersion-{version}-{arch}{'.exe' if os.name == 'nt' else ''}"
        for arch in args.architectures
    ]
    source_path = output_dir / f"Hypersion-{version}-source.tar.gz"
    metadata_paths = [
        output_dir / "release-manifest.json",
        output_dir / "SHA256SUMS",
        source_path,
    ]
    collisions = [path for path in expected + metadata_paths if path.exists()]
    if collisions and not args.force:
        parser.error(f"output exists (use --force): {collisions[0]}")

    compiler_line = run([compiler, "--version"], env=env, capture=True).splitlines()[0]
    target = ROOT / ("Hypersion.exe" if os.name == "nt" else "Hypersion")
    builds: list[dict[str, object]] = []

    try:
        for arch, destination in zip(args.architectures, expected):
            print(f"\n== Building {arch} ==", flush=True)
            run([make, "clean"], env=env)
            build_command = [make, "-j", "build", f"ARCH={arch}"]
            run(build_command, env=env)
            shutil.copy2(target, destination)
            run([strip, "--strip-all", str(destination)], env=env)
            builds.append(
                {
                    "architecture": arch,
                    "file": destination.name,
                    "size": destination.stat().st_size,
                    "sha256": sha256(destination),
                    "command": f"make -j build ARCH={arch}",
                }
            )

        uci_binary = expected[0]
        for arch, binary in zip(args.architectures, expected):
            if arch in ("x86-64", "x86-64-avx2"):
                uci_binary = binary
                break
        uci = capture_uci(uci_binary, env)

        assets = []
        for name in ASSET_NAMES:
            path = ROOT / name
            assets.append(
                {
                    "file": name,
                    "present": path.is_file(),
                    "size": path.stat().st_size if path.is_file() else None,
                    "sha256": sha256(path) if path.is_file() else None,
                }
            )

        source = None
        if git["dirty"] is False and git["commit"]:
            source = create_source_archive(source_path, version, env)
        else:
            if source_path.exists():
                source_path.unlink()
            print("Skipping source archive for dirty or unavailable Git metadata.")

        signature_path = ROOT / "testing" / "BENCH_SIGNATURE"
        manifest = {
            "schema": 1,
            "engine": "Hypersion",
            "version": version,
            "generated_at_utc": dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat(),
            "git": git,
            "toolchain": compiler_line,
            "bench": {
                "threads": 1,
                "depth": 13,
                "nodes": int(signature_path.read_text(encoding="utf-8").strip()),
            },
            "builds": builds,
            "source": source,
            "assets": assets,
            "uci": uci,
        }
        (output_dir / "release-manifest.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        checksum_items = builds + ([source] if source else [])
        sums = "".join(f"{item['sha256']}  {item['file']}\n" for item in checksum_items)
        (output_dir / "SHA256SUMS").write_text(sums, encoding="utf-8", newline="\n")
        print(f"\nRelease artifacts: {output_dir}")
    finally:
        if not args.no_restore:
            print("\n== Restoring default AVX2 development build ==", flush=True)
            run([make, "clean"], env=env)
            run([make, "-j", "build", "ARCH=x86-64-avx2"], env=env)

    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
