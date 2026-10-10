#!/usr/bin/env python3
"""
srcslice benchmark runner.

Times `srcslice` on cached srcML archives of pinned codebases (inputs.json), writes every
run to results/, keeps versioned baselines in baselines/ and compares runs against them.
Also checks that the slice output still matches the recorded golden outputs (golden/).
Run ./benchmark/run.sh --help, or read benchmark/README.md.
"""
from __future__ import annotations

import argparse
import collections
import gzip
import hashlib
import json
import os
import platform
import re
import shlex
import shutil
import socket
import statistics
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

BENCH_DIR = Path(__file__).resolve().parent
ROOT = BENCH_DIR.parent
VERSION_FILE = ROOT / "VERSION"
INPUTS_FILE = BENCH_DIR / "inputs.json"
BASELINES_DIR = BENCH_DIR / "baselines"
CACHE_DIR = BENCH_DIR / "cache"
RESULTS_DIR = BENCH_DIR / "results"
RELEASE_BUILD_DIR = ROOT / "build-release"

DEFAULT_REPEAT = {"small": 5, "small-ca": 5, "medium": 3, "medium-cpp": 3, "large": 3, "x-large": 1}
SIZE_ORDER = ["small", "small-ca", "medium", "medium-cpp", "large", "x-large"]
# the default set: the kernel inputs (large, x-large) take 30+ minutes and are run only when named
QUICK_SIZES = ["small", "small-ca", "medium", "medium-cpp"]
VERSION_RE = re.compile(r"^(\d+)\.(\d+)\.(\d+)$")


# ----------------------------------------------------------------------------- helpers

def log(msg: str) -> None:
    print(f"[bench] {msg}", file=sys.stderr, flush=True)


def die(msg: str, code: int = 2) -> None:
    print(f"[bench] error: {msg}", file=sys.stderr, flush=True)
    sys.exit(code)


class BenchError(Exception):
    """A benchmark run of srcslice failed (crash or non-zero exit); other inputs continue."""


def sh(cmd: list[str], cwd: Path | None = None) -> str:
    """Run a command, abort with its stderr on failure, return stdout."""
    try:
        res = subprocess.run(cmd, cwd=cwd, check=True, text=True,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except FileNotFoundError:
        die(f"command not found: {cmd[0]}")
    except subprocess.CalledProcessError as e:
        die(f"command failed ({e.returncode}): {shlex.join(cmd)}\n{(e.stderr or '').strip()}")
    return res.stdout


def rel(p: Path) -> str:
    try:
        return str(p.relative_to(ROOT))
    except ValueError:
        return str(p)


# ----------------------------------------------------------------------------- versions

def read_version() -> str:
    if not VERSION_FILE.exists():
        die(f"{rel(VERSION_FILE)} is missing")
    v = VERSION_FILE.read_text().strip()
    if not VERSION_RE.match(v):
        die(f"VERSION must be MAJOR.MINOR.PATCH, got {v!r}")
    return v


def write_version(v: str) -> None:
    VERSION_FILE.write_text(v + "\n")


def bump_version(v: str, part: str) -> str:
    major, minor, patch = (int(x) for x in VERSION_RE.match(v).groups())
    if part == "major":
        return f"{major + 1}.0.0"
    if part == "minor":
        return f"{major}.{minor + 1}.0"
    return f"{major}.{minor}.{patch + 1}"


def version_key(v: str) -> tuple[int, ...]:
    return tuple(int(x) for x in v.split("."))


def baseline_path(version: str) -> Path:
    return BASELINES_DIR / f"v{version}.json"


def list_baselines() -> list[tuple[str, dict]]:
    """[(version, data)] for baselines/v*.json, sorted by semantic version."""
    out = []
    for p in BASELINES_DIR.glob("v*.json"):
        v = p.stem[1:]
        if VERSION_RE.match(v):
            out.append((v, json.loads(p.read_text())))
    out.sort(key=lambda t: version_key(t[0]))
    return out


# ----------------------------------------------------------------------------- inputs

def load_inputs() -> dict:
    if not INPUTS_FILE.exists():
        die(f"{rel(INPUTS_FILE)} is missing")
    return json.loads(INPUTS_FILE.read_text())


def update_inputs() -> None:
    inputs = load_inputs()
    changed = False
    for name, spec in inputs.items():
        out = sh(["git", "ls-remote", spec["repo"], f"refs/heads/{spec['branch']}"])
        if not out.strip():
            die(f"{name}: branch {spec['branch']!r} not found at {spec['repo']}")
        sha = out.split()[0]
        if sha != spec["commit"]:
            log(f"{name}: {spec['commit'][:12]} -> {sha[:12]}")
            spec["commit"] = sha
            changed = True
        else:
            log(f"{name}: unchanged at {sha[:12]}")
    if changed:
        INPUTS_FILE.write_text(json.dumps(inputs, indent=2) + "\n")
        log(f"wrote {rel(INPUTS_FILE)}")


def input_args(spec: dict) -> list[str]:
    """Extra srcslice arguments an input is always run with (e.g. -c -a for small-ca)."""
    return list(spec.get("args", []))


def repo_slug(spec: dict) -> str:
    """<repo name>-<sha12>, so inputs that share a repository share one checkout."""
    name = spec["repo"].rstrip("/").rsplit("/", 1)[-1]
    if name.endswith(".git"):
        name = name[:-4]
    return f"{name}-{spec['commit'][:12]}"


def ensure_repo(name: str, spec: dict) -> Path:
    """Shallow checkout of the pinned commit in cache/repos/<repo>-<sha12>."""
    repo_dir = CACHE_DIR / "repos" / repo_slug(spec)
    commit = spec["commit"]
    if (repo_dir / ".git").exists():
        head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=repo_dir, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.DEVNULL).stdout.strip()
        if head == commit:
            return repo_dir
        log(f"{name}: cached checkout is {head[:12]}, want {commit[:12]}; refetching")
        shutil.rmtree(repo_dir)
    repo_dir.mkdir(parents=True)
    log(f"{name}: fetching {spec['repo']} @ {commit[:12]} (shallow)")
    sh(["git", "init", "-q"], cwd=repo_dir)
    sh(["git", "remote", "add", "origin", spec["repo"]], cwd=repo_dir)
    sh(["git", "fetch", "-q", "--depth", "1", "origin", commit], cwd=repo_dir)
    sh(["git", "checkout", "-q", "FETCH_HEAD"], cwd=repo_dir)
    return repo_dir


def collect_sources(repo_dir: Path, spec: dict) -> tuple[list[str], int]:
    """Relative paths of files matching the configured extensions, and their total size.

    "paths" restricts the walk to those subdirectories; "exclude" prunes subdirectories
    (both relative to the repository root).
    """
    exts = tuple(spec["extensions"])
    roots = [repo_dir / p for p in spec.get("paths", ["."])]
    exclude = {p.strip("/") for p in spec.get("exclude", [])}
    files: list[str] = []
    total = 0
    for root in roots:
        for dirpath, dirnames, filenames in os.walk(root):
            rel_dir = Path(dirpath).relative_to(repo_dir)
            dirnames[:] = sorted(d for d in dirnames
                                 if d != ".git" and str(rel_dir / d) not in exclude)
            for fn in sorted(filenames):
                if not fn.endswith(exts):
                    continue
                p = Path(dirpath) / fn
                try:
                    total += p.stat().st_size
                except OSError:
                    continue  # broken symlink etc.
                files.append(str(p.relative_to(repo_dir)))
    return files, total


def srcml_version() -> str:
    return sh(["srcml", "--version"]).splitlines()[0].strip()


def ensure_archive(name: str, spec: dict, repo_dir: Path) -> tuple[Path, dict]:
    """srcML archive (with --position --hash) of the input, converted once and cached."""
    arch_dir = CACHE_DIR / "archives"
    arch_dir.mkdir(parents=True, exist_ok=True)
    stem = f"{name}-{spec['commit'][:12]}"
    archive = arch_dir / f"{stem}.xml"
    meta_file = arch_dir / f"{stem}.meta.json"
    if archive.exists() and meta_file.exists():
        return archive, json.loads(meta_file.read_text())

    files, source_bytes = collect_sources(repo_dir, spec)
    if not files:
        die(f"{name}: no files matched {spec['extensions']} in {rel(repo_dir)}")
    list_file = arch_dir / f"{stem}.files"
    list_file.write_text("\n".join(files) + "\n")
    log(f"{name}: converting {len(files)} files ({source_bytes / 1e6:.1f} MB) with srcml ...")
    t0 = time.perf_counter()
    sh(["srcml", "--files-from", str(list_file), "-o", str(archive), "--position", "--hash"],
       cwd=repo_dir)
    secs = time.perf_counter() - t0
    meta = {
        "commit": spec["commit"],
        "files": len(files),
        "source_bytes": source_bytes,
        "archive_bytes": archive.stat().st_size,
        "srcml_seconds": round(secs, 2),
        "srcml_version": srcml_version(),
    }
    meta_file.write_text(json.dumps(meta, indent=2) + "\n")
    log(f"{name}: archive {meta['archive_bytes'] / 1e6:.1f} MB written in {secs:.1f} s")
    return archive, meta


# ----------------------------------------------------------------------------- srcslice binary

def cmake_cache_value(cache: Path, key: str) -> str:
    if not cache.exists():
        return ""
    for line in cache.read_text(errors="replace").splitlines():
        if line.startswith(key + ":"):
            return line.split("=", 1)[1].strip()
    return ""


def build_info(srcslice: Path) -> dict:
    """Build type and compiler for a binary living in <build dir>/bin/."""
    cache = srcslice.parent.parent / "CMakeCache.txt"
    build_type = cmake_cache_value(cache, "CMAKE_BUILD_TYPE")
    if not build_type:
        build_type = "none" if cache.exists() else "unknown"
    compiler = ""
    cxx = cmake_cache_value(cache, "CMAKE_CXX_COMPILER")
    if cxx:
        try:
            compiler = subprocess.run([cxx, "--version"], text=True, stdout=subprocess.PIPE,
                                      stderr=subprocess.DEVNULL).stdout.splitlines()[0]
        except (OSError, IndexError):
            pass
    return {"path": str(srcslice), "build_type": build_type, "compiler": compiler}


def git_info(repo: Path) -> dict:
    def g(*a: str) -> str:
        r = subprocess.run(["git", *a], cwd=repo, text=True, stdout=subprocess.PIPE,
                           stderr=subprocess.DEVNULL)
        return r.stdout.strip() if r.returncode == 0 else ""
    return {"commit": g("rev-parse", "HEAD"),
            "dirty": bool(g("status", "--porcelain", "--untracked-files=no"))}


def ensure_srcslice(args: argparse.Namespace) -> Path:
    if args.srcslice:
        p = Path(args.srcslice).resolve()
        if not p.is_file():
            die(f"srcslice binary not found: {p}")
        return p
    if not (RELEASE_BUILD_DIR / "CMakeCache.txt").exists():
        cmd = ["cmake", "-S", str(ROOT), "-B", str(RELEASE_BUILD_DIR), "-DCMAKE_BUILD_TYPE=Release"]
        if shutil.which("ninja"):
            cmd += ["-G", "Ninja"]
        log(f"configuring Release build in {rel(RELEASE_BUILD_DIR)}/")
        sh(cmd)
    log("building srcslice (Release)")
    sh(["cmake", "--build", str(RELEASE_BUILD_DIR), "--target", "srcslice"])
    p = RELEASE_BUILD_DIR / "bin" / "srcslice"
    if not p.is_file():
        die(f"build did not produce {rel(p)}")
    return p


# ----------------------------------------------------------------------------- timing

def rss_to_mb(maxrss: int) -> float:
    # ru_maxrss is bytes on macOS, kilobytes on Linux
    return maxrss / (1024 * 1024) if sys.platform == "darwin" else maxrss / 1024


def time_once(cmd: list[str]) -> dict:
    """Run cmd once; wall time plus the child's own rusage via wait4."""
    with tempfile.TemporaryFile(mode="w+") as err:
        t0 = time.perf_counter()
        proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=err)
        _, status, ru = os.wait4(proc.pid, 0)
        wall = time.perf_counter() - t0
        rc = os.waitstatus_to_exitcode(status)
        proc.returncode = rc  # already reaped by wait4
        if rc != 0:
            err.seek(0)
            how = f"killed by signal {-rc}" if rc < 0 else f"exited with {rc}"
            raise BenchError(f"srcslice {how}: {shlex.join(cmd)}\n{err.read().strip()}")
    return {
        "wall_s": round(wall, 4),
        "user_s": round(ru.ru_utime, 4),
        "sys_s": round(ru.ru_stime, 4),
        "maxrss_mb": round(rss_to_mb(ru.ru_maxrss), 1),
    }


def prepare_input(name: str, spec: dict) -> tuple[Path, dict]:
    """Checkout + archive for an input; returns (archive, info for the results file)."""
    repo_dir = ensure_repo(name, spec)
    archive, meta = ensure_archive(name, spec, repo_dir)
    info = {"commit": spec["commit"], "files": meta["files"], "source_bytes": meta["source_bytes"],
            "archive_bytes": meta["archive_bytes"], "srcml_seconds": meta["srcml_seconds"]}
    return archive, info


def run_srcslice(srcslice: Path, archive: Path, out: Path, extra_args: list[str]) -> dict:
    """One timed srcslice run writing to out (removed first); raises BenchError on failure."""
    if out.exists():
        out.unlink()
    r = time_once([str(srcslice), str(archive), "-o", str(out), *extra_args])
    r["output_bytes"] = out.stat().st_size if out.exists() else 0
    return r


def bench_input(name: str, spec: dict, srcslice: Path, extra_args: list[str],
                repeat: int, verify: bool, diff_limit: int) -> tuple[dict, dict]:
    archive, info = prepare_input(name, spec)
    out_dir = CACHE_DIR / "out"
    out_dir.mkdir(parents=True, exist_ok=True)
    out = out_dir / f"{name}.json"
    extra_args = [*input_args(spec), *extra_args]

    runs = []
    for i in range(repeat):
        try:
            r = run_srcslice(srcslice, archive, out, extra_args)
        except BenchError as e:
            log(f"{name}: FAILED on run {i + 1}/{repeat}: {str(e).splitlines()[0]}")
            return info, {"repeat": repeat, "error": str(e), "runs": runs}
        runs.append(r)
        log(f"{name}: run {i + 1}/{repeat}  wall {r['wall_s']:.2f} s  max RSS {r['maxrss_mb']:.0f} MB")

    walls = [r["wall_s"] for r in runs]
    result = {
        "repeat": repeat,
        "wall_median_s": round(statistics.median(walls), 4),
        "wall_min_s": min(walls),
        "user_s": round(statistics.median([r["user_s"] for r in runs]), 4),
        "sys_s": round(statistics.median([r["sys_s"] for r in runs]), 4),
        "maxrss_mb": max(r["maxrss_mb"] for r in runs),
        "output_bytes": runs[-1]["output_bytes"],
        "runs": runs,
    }
    if verify:
        result["output_check"] = check_timed_output(name, spec, srcslice, archive, out, extra_args, diff_limit)
    return info, result


def check_timed_output(name: str, spec: dict, srcslice: Path, archive: Path, out: Path,
                       extra_args: list[str], diff_limit: int) -> dict | None:
    """Verify the last timed output against the golden.

    A multithreaded output that differs is re-checked with a single-threaded run, since
    only that one is reproducible; the number of files the threaded run got differently
    is reported alongside.
    """
    golden = load_golden(name)
    if golden is None:
        return None
    run_args = verify_args(extra_args)
    warn_golden_mismatch(name, golden, spec, run_args)
    v = verify_output(name, out, golden, diff_limit)
    if v["status"] != "mismatch" or extra_args == run_args:
        log(f"{name}: output {check_label(v)}")
        return v
    log(f"{name}: threaded output differs in {v['changed_files']} file(s); re-checking with {shlex.join(run_args)}")
    out1 = out.with_name(f"{name}.single-thread.json")
    try:
        run_srcslice(srcslice, archive, out1, run_args)
    except BenchError as e:
        log(f"{name}: single-threaded check run FAILED: {str(e).splitlines()[0]}")
        v["single_thread_error"] = str(e)
        return v
    v1 = verify_output(name, out1, golden, diff_limit)
    v1["threaded_changed_files"] = v["changed_files"]
    if v1["status"] == "ok":
        v1["status"] = "ok-single-thread"
    log(f"{name}: output {check_label(v1)}")
    return v1


def detect_threads(extra_args: list[str]) -> int:
    """Thread count srcslice will use: -t/--threads if given, else what CPUCount() returns."""
    for i, a in enumerate(extra_args):
        if a in ("-t", "--threads") and i + 1 < len(extra_args):
            return int(extra_args[i + 1])
        if a.startswith("--threads="):
            return int(a.split("=", 1)[1])
        if a.startswith("-t") and a[2:].isdigit():
            return int(a[2:])
    try:
        return len(os.sched_getaffinity(0))
    except AttributeError:
        return os.cpu_count() or 0


def system_meta() -> dict:
    try:
        mem_gb = round(os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES") / 1e9, 1)
    except (ValueError, OSError, AttributeError):
        mem_gb = None
    in_container = (Path("/.dockerenv").exists() or Path("/run/.containerenv").exists()
                    or bool(os.environ.get("REMOTE_CONTAINERS")))
    return {"hostname": socket.gethostname(), "platform": platform.platform(),
            "machine": platform.machine(), "cpu_count": os.cpu_count(), "mem_gb": mem_gb,
            "in_container": in_container}


# ----------------------------------------------------------------------------- reporting

def fmt_table(headers: list[str], rows: list[list[str]], markdown: bool,
              left_cols: set[int] = frozenset({0})) -> str:
    cols = list(zip(headers, *rows)) if rows else [(h,) for h in headers]
    widths = [max(len(str(c)) for c in col) for col in cols]

    def line(cells) -> str:
        parts = [str(c).ljust(w) if i in left_cols else str(c).rjust(w)
                 for i, (c, w) in enumerate(zip(cells, widths))]
        return "| " + " | ".join(parts) + " |" if markdown else "  ".join(parts).rstrip()

    out = [line(headers)]
    if markdown:
        out.append("|" + "|".join(":" + "-" * (w + 1) if i in left_cols else "-" * (w + 1) + ":"
                                  for i, w in enumerate(widths)) + "|")
    else:
        out.append("  ".join("-" * w for w in widths))
    out += [line(r) for r in rows]
    return "\n".join(out)


def fmt_s(x: float) -> str:
    return f"{x:.3f} s" if x < 10 else f"{x:.1f} s"


def fmt_mb(x: float) -> str:
    return f"{x:,.0f} MB"


def fmt_bytes(n: int) -> str:
    return f"{n / 1e6:,.1f} MB" if n < 1e9 else f"{n / 1e9:,.2f} GB"


def pct(cur: float, base: float) -> float | None:
    return None if not base else (cur - base) / base * 100.0


def fmt_pct(p: float | None, threshold: float) -> str:
    if p is None:
        return "n/a"
    mark = "▲" if p > threshold else ("▼" if p < -threshold else "·")
    return f"{mark} {p:+.1f}%"


def ordered(names) -> list[str]:
    names = set(names)
    return [n for n in SIZE_ORDER if n in names] + sorted(names - set(SIZE_ORDER))


def print_summary(result: dict, markdown: bool) -> None:
    m = result["meta"]
    s = m["srcslice"]
    print(f"\nsrcslice v{m['version']}  commit {s['commit'][:9]}{'*' if s['dirty'] else ''}  "
          f"build {s['build_type']}  threads {m['threads']}  "
          f"{'args ' + shlex.join(m['srcslice_args']) + '  ' if m['srcslice_args'] else ''}"
          f"host {m['hostname']}{' (container)' if m['in_container'] else ''}")
    rows, failed = [], []
    for name in ordered(result["results"]):
        i, r = result["inputs"][name], result["results"][name]
        if "error" in r:
            failed.append(name)
            continue
        rows.append([name, f"{i['files']:,}", fmt_bytes(i["source_bytes"]), fmt_bytes(i["archive_bytes"]),
                     f"{i['srcml_seconds']:.1f} s", str(r["repeat"]), fmt_s(r["wall_median_s"]),
                     fmt_s(r["wall_min_s"]), fmt_s(r["user_s"]), fmt_s(r["sys_s"]),
                     fmt_mb(r["maxrss_mb"]), fmt_bytes(r["output_bytes"]),
                     check_label(r.get("output_check")) if "output_check" in r else "skipped"])
    if rows:
        print(fmt_table(["input", "files", "source", "archive", "srcml", "runs", "wall median",
                         "wall min", "user", "sys", "max RSS", "output", "output check"], rows, markdown,
                        left_cols={0, 12}))
    for name in failed:
        r, i = result["results"][name], result["inputs"][name]
        print(f"\n{name}: FAILED ({i['files']:,} files, {fmt_bytes(i['archive_bytes'])} srcML) "
              f"after {len(r['runs'])} good run(s)\n  " + r["error"].replace("\n", "\n  "))


def resolve_compare_targets(arg: str | None) -> list[tuple[str, dict]]:
    baselines = [(f"v{v}", d) for v, d in list_baselines()]
    if arg in (None, "latest"):
        return baselines[-1:]
    if arg == "all":
        return baselines
    p = Path(arg)
    if p.is_file():
        return [(p.name, json.loads(p.read_text()))]
    label = arg if arg.startswith("v") else f"v{arg}"
    for lbl, data in baselines:
        if lbl == label:
            return [(lbl, data)]
    die(f"no baseline {arg!r} (have: {', '.join(l for l, _ in baselines) or 'none'})")


def warn_mismatch(cur: dict, ref: dict, ref_label: str) -> None:
    cm, rm = cur["meta"], ref["meta"]
    for name, ci in cur["inputs"].items():
        ri = ref["inputs"].get(name)
        if ri and ri["commit"] != ci["commit"]:
            log(f"warning: {name} input commit {ci['commit'][:12]} differs from {ref_label} ({ri['commit'][:12]})")
    checks = [("build type", cm["srcslice"]["build_type"], rm["srcslice"]["build_type"]),
              ("threads", cm["threads"], rm["threads"]),
              ("srcslice args", cm["srcslice_args"], rm["srcslice_args"]),
              ("host", cm["hostname"], rm["hostname"])]
    for what, a, b in checks:
        if a != b:
            log(f"warning: {what} {a!r} differs from {ref_label} ({b!r})")


def compare(current: dict, targets: list[tuple[str, dict]], threshold: float,
            markdown: bool, saving: bool = False) -> bool:
    """Print the comparison table; return True if any input regressed beyond the threshold."""
    if not targets:
        if saving:
            print("\nno previous baseline to compare against")
        else:
            print(f"\nno baseline yet; run with --save-baseline to create "
                  f"{rel(baseline_path(current['meta']['version']))}")
        return False
    ref_label, ref = targets[-1]
    warn_mismatch(current, ref, ref_label)

    headers = (["input"] + [f"{lbl} wall" for lbl, _ in targets]
               + ["current wall", f"Δ vs {ref_label}", f"{ref_label} RSS", "current RSS", "Δ RSS"])
    rows, regression = [], False
    for name in ordered(current["results"]):
        cur = current["results"][name]
        row = [name]
        for _, data in targets:
            r = data["results"].get(name)
            row.append(fmt_s(r["wall_median_s"]) if r and "error" not in r else "—")
        r = ref["results"].get(name)
        if r and "error" in r:
            r = None
        if "error" in cur:
            row += ["crash", "—", fmt_mb(r["maxrss_mb"]) if r else "—", "—", "—"]
            rows.append(row)
            continue
        row.append(fmt_s(cur["wall_median_s"]))
        if r:
            d = pct(cur["wall_median_s"], r["wall_median_s"])
            regression |= d is not None and d > threshold
            row += [fmt_pct(d, threshold), fmt_mb(r["maxrss_mb"]), fmt_mb(cur["maxrss_mb"]),
                    fmt_pct(pct(cur["maxrss_mb"], r["maxrss_mb"]), threshold)]
        else:
            row += ["—", "—", fmt_mb(cur["maxrss_mb"]), "—"]
        rows.append(row)
    print(f"\ncomparison (threshold ±{threshold:g}%)")
    print(fmt_table(headers, rows, markdown))
    return regression


def history(markdown: bool, threshold: float) -> None:
    baselines = list_baselines()
    if not baselines:
        die("no baselines yet")
    for name in ordered(n for _, d in baselines for n in d["results"]):
        rows, first, prev = [], None, None
        for ver, d in baselines:
            r = d["results"].get(name)
            if not r or "error" in r:
                continue
            w = r["wall_median_s"]
            m = d["meta"]
            rows.append([f"v{ver}", m["timestamp"][:10],
                         m["srcslice"]["commit"][:9] + ("*" if m["srcslice"]["dirty"] else ""),
                         fmt_s(w), fmt_mb(r["maxrss_mb"]),
                         fmt_pct(pct(w, prev), threshold) if prev is not None else "—",
                         fmt_pct(pct(w, first), threshold) if first is not None else "—",
                         m.get("note", "")])
            first = w if first is None else first
            prev = w
        inp = baselines[-1][1]["inputs"].get(name, {})
        print(f"\n{name}" + (f"  ({inp['files']:,} files, {fmt_bytes(inp['archive_bytes'])} srcML)" if inp else ""))
        print(fmt_table(["version", "date", "srcslice", "wall", "max RSS", "Δ prev", "Δ first", "note"],
                        rows, markdown, left_cols={0, 7}))


# ----------------------------------------------------------------------------- output verification
#
# Golden outputs: golden/<input>.json (committed) holds per-file hashes of the canonical
# form of srcslice's output for that input; cache/golden/<input>.canon.gz (ignored) keeps
# the full canonical output so a mismatch can be explained entry by entry.
#
# srcslice's output is only reproducible single-threaded: with several threads the
# interprocedural pass sees units in a timing-dependent order and use/call sets change
# from run to run. Goldens are therefore recorded and checked with `-t 1` unless the
# caller picks a thread count explicitly.

GOLDEN_DIR = BENCH_DIR / "golden"
GOLDEN_CACHE_DIR = CACHE_DIR / "golden"
CANON_FORMAT = 1
# fields whose arrays come from std::set in srcslice: compared as sets, not sequences
SET_FIELDS = {"dependence", "aliases", "calls", "use", "definition", "controlEdges"}
ENTRY_KEY_RE = re.compile(r'^"(.*)":\{$')
FILE_LINE_RE = re.compile(r'^\s*"file"\s*:\s*"(.*)"\s*,?$')


class OutputLayoutError(Exception):
    """The output does not have the line layout the entry splitter understands."""


def golden_path(name: str) -> Path:
    return GOLDEN_DIR / f"{name}.json"


def golden_canon_path(name: str) -> Path:
    return GOLDEN_CACHE_DIR / f"{name}.canon.gz"


def golden_canon_meta_path(name: str) -> Path:
    return GOLDEN_CACHE_DIR / f"{name}.canon.meta.json"


def load_golden(name: str) -> dict | None:
    p = golden_path(name)
    return json.loads(p.read_text()) if p.exists() else None


def iter_entries(path: Path):
    """Yield (key, field lines) for every top-level entry of a srcslice output file.

    Streams line by line (outputs reach gigabytes) and relies on the writer's layout:
    one `"key":{` line per entry, indented field lines, a `},` or `}` closer. json.load
    would not do anyway: keys repeat and some strings contain unescaped quotes.
    """
    key, buf = None, []
    with path.open(encoding="utf-8", errors="surrogateescape") as f:
        for ln, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if key is None:
                m = ENTRY_KEY_RE.match(line)
                if m:
                    key, buf = m.group(1), []
                elif line not in ("{", "}", ""):
                    raise OutputLayoutError(f"{path}:{ln}: unexpected line {line[:80]!r}")
            elif line in ("},", "}"):
                yield key, buf
                key = None
            else:
                buf.append(line)
    if key is not None:
        raise OutputLayoutError(f"{path}: unterminated entry {key!r}")


def canonical_entry(key: str, lines: list[str]) -> tuple[str, str, bool]:
    """(file, canonical text, parsed) for one entry.

    Parsed entries are re-serialized with sorted keys and sorted set-like arrays, so a
    change in formatting or in element order is not an output change. Entries that are
    not valid JSON (srcslice does not escape quotes in names such as `operator "" _a`)
    fall back to their stripped text.
    """
    file = ""
    for l in lines:
        m = FILE_LINE_RE.match(l)
        if m:
            file = m.group(1)
            break
    try:
        obj = json.loads("{" + "\n".join(lines) + "}")
    except json.JSONDecodeError:
        return file, key + "\tRAW\t" + "\\n".join(l.strip() for l in lines), False
    if isinstance(obj.get("file"), str):
        file = obj["file"]
    for k, v in obj.items():
        if k in SET_FIELDS and isinstance(v, list):
            obj[k] = sorted(v, key=lambda e: json.dumps(e, sort_keys=True))
    return file, key + "\t" + json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=False), True


def utf8(s: str) -> bytes:
    return s.encode("utf-8", "surrogateescape")


def digest_output(path: Path, canon_out=None) -> dict:
    """Per-file and overall hashes of an output's canonical form.

    Hashes are over sorted per-entry digests, so the nondeterministic key order of the
    output is invisible. With canon_out, also writes `<file>\\t<canonical entry>` lines.
    """
    per_file: dict[str, list[bytes]] = {}
    entries = unparsed = 0
    for key, lines in iter_entries(path):
        file, canon, ok = canonical_entry(key, lines)
        entries += 1
        unparsed += not ok
        per_file.setdefault(file, []).append(hashlib.sha256(utf8(canon)).digest())
        if canon_out is not None:
            canon_out.write(utf8(file + "\t" + canon + "\n"))
    files = {}
    for file in sorted(per_file):
        digests = sorted(per_file[file])
        files[file] = f"{hashlib.sha256(b''.join(digests)).hexdigest()[:16]}:{len(digests)}"
    overall = hashlib.sha256(utf8("\n".join(f"{f}\t{v}" for f, v in files.items()))).hexdigest()
    return {"canon_format": CANON_FORMAT, "entries": entries, "unparsed_entries": unparsed,
            "files": len(files), "sha256": overall, "per_file": files}


def canon_entries_for(name: str, out_path: Path, files: set[str], golden: dict) -> tuple[dict, dict] | None:
    """{file: [canonical entries]} for `files`, from the cached golden and from out_path.

    None when the cached golden output is missing or belongs to another golden.
    """
    canon, meta = golden_canon_path(name), golden_canon_meta_path(name)
    if not (canon.exists() and meta.exists()):
        return None
    if json.loads(meta.read_text()).get("sha256") != golden["sha256"]:
        return None
    g: dict[str, list[str]] = {f: [] for f in files}
    f_: dict[str, list[str]] = {f: [] for f in files}
    with gzip.open(canon, "rb") as fh:
        for raw in fh:
            file, _, entry = raw.decode("utf-8", "surrogateescape").rstrip("\n").partition("\t")
            if file in g:
                g[file].append(entry)
    for key, lines in iter_entries(out_path):
        file, entry, _ = canonical_entry(key, lines)
        if file in f_:
            f_[file].append(entry)
    return g, f_


def field_diff(key: str, old: str, new: str) -> list[str]:
    """Readable difference between two canonical entries with the same key."""
    oj, nj = old.split("\t", 1)[1], new.split("\t", 1)[1]
    if oj.startswith("RAW\t") or nj.startswith("RAW\t"):
        return [f"~ {key}", f"    - {oj[:240]}", f"    + {nj[:240]}"]
    oo, no = json.loads(oj), json.loads(nj)
    out = [f"~ {key}"]
    for field in sorted(set(oo) | set(no)):
        ov, nv = oo.get(field), no.get(field)
        if ov == nv:
            continue
        if isinstance(ov, list) and isinstance(nv, list):
            oc = collections.Counter(json.dumps(e, sort_keys=True) for e in ov)
            nc = collections.Counter(json.dumps(e, sort_keys=True) for e in nv)
            rem, add = sorted((oc - nc).elements()), sorted((nc - oc).elements())
            out.append(f"    {field}: -{len(rem)} +{len(add)}")
            out += [f"      - {e}" for e in rem[:5]] + [f"      + {e}" for e in add[:5]]
            if len(rem) > 5 or len(add) > 5:
                out.append("      ...")
        else:
            out.append(f"    {field}: {json.dumps(ov, ensure_ascii=False)} -> {json.dumps(nv, ensure_ascii=False)}")
    return out


def entry_diff_lines(old_entries: list[str], new_entries: list[str]) -> list[str]:
    """Differences between the canonical entries of one file, paired by key."""
    oc, nc = collections.Counter(old_entries), collections.Counter(new_entries)
    by_key_old: dict[str, list[str]] = {}
    by_key_new: dict[str, list[str]] = {}
    for e in sorted((oc - nc).elements()):
        by_key_old.setdefault(e.split("\t", 1)[0], []).append(e)
    for e in sorted((nc - oc).elements()):
        by_key_new.setdefault(e.split("\t", 1)[0], []).append(e)
    lines: list[str] = []
    for key in sorted(set(by_key_old) | set(by_key_new)):
        olds, news = by_key_old.get(key, []), by_key_new.get(key, [])
        for o, n in zip(olds, news):
            lines += field_diff(key, o, n)
        lines += [f"- {key}  (entry gone)" for _ in olds[len(news):]]
        lines += [f"+ {key}  (new entry)" for _ in news[len(olds):]]
    return lines


def diff_summary(old: dict[str, list[str]], new: dict[str, list[str]]) -> list[str]:
    """Aggregate the differences over every file: per set-valued field, how many positions
    were removed and added, and how many removed ones reappear in another entry with the
    same variable name ("moved") versus genuinely lost or new. The review instrument for
    output-changing steps."""
    removed: collections.Counter = collections.Counter()   # (name, field, value) -> n
    added: collections.Counter = collections.Counter()
    field_removed: collections.Counter = collections.Counter()
    field_added: collections.Counter = collections.Counter()
    scalar_changed: collections.Counter = collections.Counter()
    churn: collections.Counter = collections.Counter()
    counts = collections.Counter()

    def parse(entry: str | None) -> dict | None:
        if entry is None:
            return None
        body = entry.split("\t", 1)[1]
        if body.startswith("RAW\t"):
            counts["raw"] += 1
            return None
        return json.loads(body)

    def elements(obj: dict | None, field: str) -> collections.Counter:
        if obj is None or not isinstance(obj.get(field), list):
            return collections.Counter()
        return collections.Counter(json.dumps(e, sort_keys=True) for e in obj[field])

    def tally(old_entry: str | None, new_entry: str | None) -> None:
        oo, no = parse(old_entry), parse(new_entry)
        if (old_entry and oo is None) or (new_entry and no is None):
            return
        name = (oo or no).get("name", "?")
        for field in sorted(SET_FIELDS):
            oe, ne = elements(oo, field), elements(no, field)
            for v, n in (oe - ne).items():
                removed[(name, field, v)] += n
                field_removed[field] += n
                churn[name] += n
            for v, n in (ne - oe).items():
                added[(name, field, v)] += n
                field_added[field] += n
                churn[name] += n
        if oo is not None and no is not None:
            for field in set(oo) | set(no):
                if field not in SET_FIELDS and oo.get(field) != no.get(field):
                    scalar_changed[field] += 1

    for file in set(old) | set(new):
        oc, nc = collections.Counter(old.get(file, [])), collections.Counter(new.get(file, []))
        by_key_old: dict[str, list[str]] = {}
        by_key_new: dict[str, list[str]] = {}
        for e in (oc - nc).elements():
            by_key_old.setdefault(e.split("\t", 1)[0], []).append(e)
        for e in (nc - oc).elements():
            by_key_new.setdefault(e.split("\t", 1)[0], []).append(e)
        for key in set(by_key_old) | set(by_key_new):
            olds, news = sorted(by_key_old.get(key, [])), sorted(by_key_new.get(key, []))
            for o, n in zip(olds, news):
                counts["changed"] += 1
                tally(o, n)
            for o in olds[len(news):]:
                counts["gone"] += 1
                tally(o, None)
            for n in news[len(olds):]:
                counts["new"] += 1
                tally(None, n)

    moved_by_field: collections.Counter = collections.Counter()
    for k, n in removed.items():
        if k in added:
            moved_by_field[k[1]] += min(n, added[k])
    rows = []
    for field in sorted(SET_FIELDS):
        r, a, m = field_removed[field], field_added[field], moved_by_field[field]
        if r or a:
            rows.append([field, f"{r:,}", f"{a:,}", f"{m:,}", f"{r - m:,}", f"{a - m:,}"])
    lines = [f"summary over all changed files: {counts['changed']:,} entries changed, "
             f"{counts['gone']:,} gone, {counts['new']:,} new"
             + (f", {counts['raw']:,} compared as text" if counts["raw"] else "")]
    if rows:
        lines.append("  positions per field (moved = removed from one entry and added to another with the same name):")
        lines += ["  " + l for l in fmt_table(["field", "removed", "added", "moved", "lost", "new"], rows, False).splitlines()]
    if scalar_changed:
        lines.append("  scalar fields changed: " + ", ".join(f"{f} {n:,}" for f, n in scalar_changed.most_common()))
    if churn:
        lines.append("  names with the most churn: " + ", ".join(f"{n} ({c:,})" for n, c in churn.most_common(12)))
    return lines


def verify_output(name: str, out_path: Path, golden: dict, diff_limit: int, summary: bool = False) -> dict:
    """Digest out_path and compare with the golden; on mismatch write cache/out/<name>.verify.txt."""
    t0 = time.perf_counter()
    try:
        fresh = digest_output(out_path)
    except OutputLayoutError as e:
        return {"status": "unreadable", "detail": str(e)}
    g, f = golden["per_file"], fresh["per_file"]
    changed = sorted(x for x in g if x in f and g[x] != f[x])
    removed = sorted(x for x in g if x not in f)
    added = sorted(x for x in f if x not in g)
    res = {"status": "ok" if fresh["sha256"] == golden["sha256"] else "mismatch",
           "entries": fresh["entries"], "files": fresh["files"], "unparsed_entries": fresh["unparsed_entries"],
           "changed_files": len(changed), "added_files": len(added), "removed_files": len(removed),
           "digest_s": round(time.perf_counter() - t0, 2)}
    if res["status"] == "ok":
        return res

    lines = [f"{name}: output differs from {rel(golden_path(name))} "
             f"(recorded {golden['created'][:10]} with srcslice {golden['srcslice']['commit'][:9]})",
             f"  files: {len(changed)} changed, {len(added)} added, {len(removed)} removed;"
             f" entries {golden['entries']:,} -> {fresh['entries']:,}"]
    if summary:
        pair = canon_entries_for(name, out_path, set(changed) | set(added) | set(removed), golden)
        if pair is None:
            lines.append("  (--diff-summary needs the cached golden output; see below)")
        else:
            lines += diff_summary(*pair)
        lines.append("")
    lines += [f"+ file {x} ({f[x].split(':')[1]} entries)" for x in added[:diff_limit]]
    lines += [f"- file {x} ({g[x].split(':')[1]} entries)" for x in removed[:diff_limit]]
    if changed:
        shown = set(changed[:diff_limit])
        pair = canon_entries_for(name, out_path, shown, golden)
        if pair is None:
            lines.append("~ changed files: " + ", ".join(changed[:diff_limit]))
            lines.append(f"  (no local copy of the golden output in {rel(golden_canon_path(name))}; "
                         f"entry-level diff unavailable. --save-golden --force on the golden commit recreates it.)")
        else:
            old, new = pair
            for x in sorted(shown):
                lines.append(f"~ file {x}")
                lines += ["  " + l for l in entry_diff_lines(old[x], new[x])]
        if len(changed) > diff_limit:
            lines.append(f"  ... {len(changed) - diff_limit} more changed file(s); raise --diff-limit to see them")
    report = out_path.with_name(f"{name}.verify.txt")
    report.write_text("\n".join(lines) + "\n")
    res["report"] = str(report)
    return res


def save_golden(name: str, spec: dict, out_path: Path, meta: dict, note: str, run_args: list[str]) -> dict:
    """Write golden/<name>.json and the cached canonical output for out_path."""
    GOLDEN_DIR.mkdir(exist_ok=True)
    GOLDEN_CACHE_DIR.mkdir(parents=True, exist_ok=True)
    canon = golden_canon_path(name)
    tmp = canon.with_name(canon.name + ".tmp")
    with gzip.open(tmp, "wb", compresslevel=3) as fh:
        digest = digest_output(out_path, canon_out=fh)
    tmp.replace(canon)
    golden_canon_meta_path(name).write_text(json.dumps({"sha256": digest["sha256"]}) + "\n")
    manifest = {
        "input": name,
        "input_commit": spec["commit"],
        "created": meta["timestamp"],
        "note": note,
        "version": meta["version"],
        "srcslice": {k: meta["srcslice"][k] for k in ("commit", "dirty", "build_type")},
        "srcslice_args": run_args,
        **digest,
    }
    golden_path(name).write_text(json.dumps(manifest, indent=1) + "\n")
    return manifest


def has_thread_arg(extra_args: list[str]) -> bool:
    return any(a in ("-t", "--threads") or a.startswith("--threads=") or (a.startswith("-t") and a[2:].isdigit())
               for a in extra_args)


def verify_args(extra_args: list[str]) -> list[str]:
    """Arguments for a reproducible run: the caller's, plus -t 1 unless a thread count was given."""
    return extra_args if has_thread_arg(extra_args) else [*extra_args, "-t", "1"]


def check_label(c: dict | None) -> str:
    if c is None:
        return "—"
    s = c["status"]
    if s == "ok":
        return "ok"
    if s == "ok-single-thread":
        return f"ok (-t 1); threaded run differs in {c['threaded_changed_files']} file(s)"
    if s == "mismatch":
        return f"MISMATCH ({c['changed_files']} changed, {c['added_files']} added, {c['removed_files']} removed)"
    if s == "unreadable":
        return "unreadable output"
    return s


def without_thread_args(args: list[str]) -> list[str]:
    out, skip = [], False
    for a in args:
        if skip:
            skip = False
            continue
        if a in ("-t", "--threads"):
            skip = True
            continue
        if a.startswith("--threads=") or (a.startswith("-t") and a[2:].isdigit()):
            continue
        out.append(a)
    return out


def warn_golden_mismatch(name: str, golden: dict, spec: dict, run_args: list[str]) -> None:
    if golden["input_commit"] != spec["commit"]:
        log(f"warning: {name} golden was recorded for input commit {golden['input_commit'][:12]}, "
            f"this run uses {spec['commit'][:12]}; differences are expected")
    # the thread count must not change the output, so a differing -t is not worth a warning
    if without_thread_args(golden.get("srcslice_args", [])) != without_thread_args(run_args):
        log(f"warning: {name} golden was recorded with srcslice args {shlex.join(golden.get('srcslice_args', []))!r}, "
            f"this run uses {shlex.join(run_args)!r}")


def verify_main(args: argparse.Namespace, inputs: dict, sizes: list[str], extra_args: list[str],
                srcslice: Path, meta: dict, markdown: bool) -> int:
    """--verify / --save-golden: one single-threaded run per input, checked against or recorded as golden."""
    out_dir = CACHE_DIR / "out"
    out_dir.mkdir(parents=True, exist_ok=True)
    if args.save_golden:
        if args.sizes == "all":
            # don't spend an hour on an input the baseline already shows cannot finish (x-large OOM)
            latest = list_baselines()[-1][1]["results"] if list_baselines() else {}
            broken = [n for n in sizes if "error" in latest.get(n, {})]
            for n in broken:
                log(f"skipping {n}: the latest baseline records a failure for it (pass --sizes {n} to insist)")
            sizes = [n for n in sizes if n not in broken]
        for name in sizes:
            if golden_path(name).exists() and not args.force:
                die(f"{rel(golden_path(name))} exists; use --force to replace it")
        if meta["srcslice"]["dirty"]:
            log("warning: recording goldens from a srcslice build with uncommitted changes")
    rows, failed, mismatched, reports = [], [], [], []
    for name in sizes:
        spec = inputs[name]
        golden = load_golden(name)
        if golden is None and not args.save_golden:
            log(f"{name}: no golden recorded ({rel(golden_path(name))} missing); run --save-golden --sizes {name}")
            rows.append([name, "—", "—", "—", "no golden"])
            continue
        archive, info = prepare_input(name, spec)
        out = out_dir / f"{name}.json"
        run_args = verify_args([*input_args(spec), *extra_args])
        log(f"{name}: running srcslice {shlex.join(run_args)} ({info['files']:,} files)")
        try:
            r = run_srcslice(srcslice, archive, out, run_args)
        except BenchError as e:
            log(f"{name}: FAILED: {str(e).splitlines()[0]}")
            print("  " + str(e).replace("\n", "\n  "), file=sys.stderr)
            failed.append(name)
            rows.append([name, "—", "—", "—", "FAILED"])
            continue
        if args.save_golden:
            m = save_golden(name, spec, out, meta, args.note, run_args)
            log(f"{name}: golden saved to {rel(golden_path(name))} "
                f"({m['entries']:,} entries in {m['files']:,} files, {m['unparsed_entries']} kept as text)")
            rows.append([name, f"{m['entries']:,}", f"{m['files']:,}", fmt_s(r["wall_s"]), "golden saved"])
            continue
        warn_golden_mismatch(name, golden, spec, run_args)
        v = verify_output(name, out, golden, args.diff_limit, args.diff_summary)
        label = check_label(v)
        if v["status"] != "ok":
            mismatched.append(name)
            if "report" in v:
                reports.append(v["report"])
            if "detail" in v:
                log(f"{name}: {v['detail']}")
        rows.append([name, f"{v.get('entries', 0):,}", f"{v.get('files', 0):,}", fmt_s(r["wall_s"]), label])
        log(f"{name}: {label}")
    s = meta["srcslice"]
    print(f"\noutput check  srcslice commit {s['commit'][:9]}{'*' if s['dirty'] else ''}  build {s['build_type']}  "
          f"args {shlex.join(verify_args(extra_args))} (+ per-input args)")
    print(fmt_table(["input", "entries", "files", "wall", "result"], rows, markdown, left_cols={0, 4}))
    for report in reports:
        print(f"\n{rel(Path(report))}:")
        lines = Path(report).read_text().splitlines()
        print("  " + "\n  ".join(lines[:args.diff_lines]))
        if len(lines) > args.diff_lines:
            print(f"  ... {len(lines) - args.diff_lines} more line(s) in the report")
    if mismatched:
        log(f"OUTPUT MISMATCH: {', '.join(mismatched)}")
    if failed:
        log(f"FAILED inputs: {', '.join(failed)}")
    return 1 if (failed or mismatched) else 0


# ----------------------------------------------------------------------------- main

def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="benchmark/run.sh", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sizes", default="quick",
                    help="comma list of inputs, quick (default: small,medium,medium-cpp) or all (adds the kernel inputs)")
    ap.add_argument("--repeat", type=int, help="override the per-size repeat count")
    ap.add_argument("--srcslice", help="time this binary instead of building build-release/")
    ap.add_argument("--srcslice-args", default="", help='extra srcslice arguments, e.g. --srcslice-args="-t 8"')
    ap.add_argument("--compare-to", help="latest (default) | all | vX.Y.Z | results file")
    ap.add_argument("--threshold", type=float, default=5.0, help="marker threshold in percent (default 5)")
    ap.add_argument("--fail-on-regression", action="store_true", help="exit 1 if slower than the threshold")
    ap.add_argument("--save-baseline", action="store_true", help="store this run as baselines/v<VERSION>.json")
    ap.add_argument("--force", action="store_true", help="allow --save-baseline to overwrite an existing version")
    ap.add_argument("--bump", choices=["major", "minor", "patch"], help="bump VERSION before saving the baseline")
    ap.add_argument("--note", default="", help="free text stored with the result")
    ap.add_argument("--history", action="store_true", help="print all baseline versions and exit")
    ap.add_argument("--format", choices=["text", "markdown"], default="text")
    ap.add_argument("--update-inputs", action="store_true", help="re-pin inputs.json to the branch heads and exit")
    g = ap.add_argument_group("output regression", "the slice output is checked against golden/<input>.json")
    g.add_argument("--verify", action="store_true", help="run each input once (single-threaded), check its output and exit")
    g.add_argument("--save-golden", action="store_true", help="record the current output as the golden for the selected inputs (--force to replace)")
    g.add_argument("--no-verify", action="store_true", help="skip the output check after the timing runs")
    g.add_argument("--diff-limit", type=int, default=10, help="files detailed in a mismatch report (default 10)")
    g.add_argument("--diff-lines", type=int, default=40, help="report lines echoed to the console (default 40)")
    g.add_argument("--diff-summary", action="store_true",
                   help="on a mismatch, aggregate the differences over all changed files (moved vs lost vs new positions)")
    args = ap.parse_args(argv)
    markdown = args.format == "markdown"

    if args.update_inputs:
        update_inputs()
        return 0
    if args.history:
        history(markdown, args.threshold)
        return 0

    inputs = load_inputs()
    if args.sizes == "all":
        sizes = list(inputs)
    elif args.sizes == "quick":
        sizes = [n for n in QUICK_SIZES if n in inputs]
    else:
        sizes = [s.strip() for s in args.sizes.split(",") if s.strip()]
    unknown = [s for s in sizes if s not in inputs]
    if unknown:
        die(f"unknown input(s) {unknown}; choose from {list(inputs)}")

    version = read_version()
    if args.bump:
        if not args.save_baseline:
            die("--bump requires --save-baseline")
        version = bump_version(version, args.bump)
    if (args.verify or args.save_golden) and (args.save_baseline or args.compare_to or args.repeat):
        die("--verify/--save-golden do not time anything; drop --save-baseline/--compare-to/--repeat")
    if args.save_baseline:
        target = baseline_path(version)
        if target.exists() and not args.force:
            die(f"{rel(target)} exists; use --bump to create a new version or --force to overwrite it")
        if set(sizes) != set(inputs):
            log(f"warning: baseline v{version} will only contain: {', '.join(sizes)}")

    targets = resolve_compare_targets(args.compare_to)  # before writing a new baseline
    extra_args = shlex.split(args.srcslice_args)
    srcslice = ensure_srcslice(args)
    meta = {
        "version": version,
        "note": args.note,
        "timestamp": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        **system_meta(),
        "srcml_version": srcml_version(),
        "srcslice": {**build_info(srcslice), **git_info(ROOT)},
        "srcslice_args": extra_args,
        "threads": detect_threads(extra_args),
    }
    if args.verify or args.save_golden:
        return verify_main(args, inputs, sizes, extra_args, srcslice, meta, markdown)
    if meta["srcslice"]["build_type"].lower() != "release":
        log(f"warning: srcslice build type is {meta['srcslice']['build_type']!r}, not Release")

    result = {"meta": meta, "inputs": {}, "results": {}}
    for name in sizes:
        repeat = args.repeat or DEFAULT_REPEAT.get(name, 3)
        result["inputs"][name], result["results"][name] = bench_input(
            name, inputs[name], srcslice, extra_args, repeat, not args.no_verify, args.diff_limit)

    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    out_file = RESULTS_DIR / (meta["timestamp"].replace("-", "").replace(":", "") + ".json")
    out_file.write_text(json.dumps(result, indent=2) + "\n")

    print_summary(result, markdown)
    regression = compare(result, targets, args.threshold, markdown, saving=args.save_baseline)
    failed = [n for n, r in result["results"].items() if "error" in r]

    if args.save_baseline:
        if failed:
            log(f"warning: baseline v{version} records failures for: {', '.join(failed)}")
        if args.bump:
            write_version(version)
            log(f"VERSION -> {version}")
        BASELINES_DIR.mkdir(exist_ok=True)
        target = baseline_path(version)
        to_save = result
        if target.exists() and args.force and set(sizes) != set(inputs):
            # Re-measure only the inputs that were run; keep the rest of this version's baseline.
            existing = json.loads(target.read_text())
            for what in ("commit", "build_type"):
                old, new = existing["meta"]["srcslice"].get(what), meta["srcslice"].get(what)
                if old != new:
                    log(f"warning: merging into v{version} measured with srcslice {what} {old!r}, this run is {new!r}")
            existing["inputs"].update(result["inputs"])
            existing["results"].update(result["results"])
            existing["meta"] = {**meta, "merged_inputs": sorted(set(existing["meta"].get("merged_inputs", [])) | set(sizes))}
            to_save = existing
            log(f"updated {', '.join(sizes)} in {rel(target)}; other inputs kept")
        target.write_text(json.dumps(to_save, indent=2) + "\n")
        log(f"saved baseline {rel(target)}")
    log(f"results written to {rel(out_file)}")
    mismatched = [n for n, r in result["results"].items()
                  if r.get("output_check") and r["output_check"]["status"] not in ("ok", "ok-single-thread")]
    for n in mismatched:
        c = result["results"][n]["output_check"]
        if "report" in c:
            lines = Path(c["report"]).read_text().splitlines()
            print(f"\n{rel(Path(c['report']))}:\n  " + "\n  ".join(lines[:args.diff_lines]))
            if len(lines) > args.diff_lines:
                print(f"  ... {len(lines) - args.diff_lines} more line(s) in the report")
    if mismatched:
        log(f"OUTPUT MISMATCH: {', '.join(mismatched)}")
    if failed:
        log(f"FAILED inputs: {', '.join(failed)}")
    return 1 if (failed or mismatched or (regression and args.fail_on_regression)) else 0


if __name__ == "__main__":
    sys.exit(main())
