#!/usr/bin/env python3
"""
srcslice benchmark runner.

Times `srcslice` on cached srcML archives of pinned codebases (inputs.json), writes every
run to results/, keeps versioned baselines in baselines/ and compares runs against them.
Run ./benchmark/run.sh --help, or read benchmark/README.md.
"""
from __future__ import annotations

import argparse
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

DEFAULT_REPEAT = {"small": 5, "medium": 3, "medium-cpp": 3, "large": 3, "x-large": 1}
SIZE_ORDER = ["small", "medium", "medium-cpp", "large", "x-large"]
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


def bench_input(name: str, spec: dict, srcslice: Path, extra_args: list[str],
                repeat: int) -> tuple[dict, dict]:
    repo_dir = ensure_repo(name, spec)
    archive, meta = ensure_archive(name, spec, repo_dir)
    out_dir = CACHE_DIR / "out"
    out_dir.mkdir(parents=True, exist_ok=True)
    out = out_dir / f"{name}.json"
    cmd = [str(srcslice), str(archive), "-o", str(out), *extra_args]

    info = {"commit": spec["commit"], "files": meta["files"], "source_bytes": meta["source_bytes"],
            "archive_bytes": meta["archive_bytes"], "srcml_seconds": meta["srcml_seconds"]}
    runs = []
    for i in range(repeat):
        if out.exists():
            out.unlink()
        try:
            r = time_once(cmd)
        except BenchError as e:
            log(f"{name}: FAILED on run {i + 1}/{repeat}: {str(e).splitlines()[0]}")
            return info, {"repeat": repeat, "error": str(e), "runs": runs}
        r["output_bytes"] = out.stat().st_size if out.exists() else 0
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
    return info, result


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
                     fmt_mb(r["maxrss_mb"]), fmt_bytes(r["output_bytes"])])
    if rows:
        print(fmt_table(["input", "files", "source", "archive", "srcml", "runs", "wall median",
                         "wall min", "user", "sys", "max RSS", "output"], rows, markdown))
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


# ----------------------------------------------------------------------------- main

def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="benchmark/run.sh", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sizes", default="all", help="comma list of inputs to run (default: all)")
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
    args = ap.parse_args(argv)
    markdown = args.format == "markdown"

    if args.update_inputs:
        update_inputs()
        return 0
    if args.history:
        history(markdown, args.threshold)
        return 0

    inputs = load_inputs()
    sizes = list(inputs) if args.sizes == "all" else [s.strip() for s in args.sizes.split(",") if s.strip()]
    unknown = [s for s in sizes if s not in inputs]
    if unknown:
        die(f"unknown input(s) {unknown}; choose from {list(inputs)}")

    version = read_version()
    if args.bump:
        if not args.save_baseline:
            die("--bump requires --save-baseline")
        version = bump_version(version, args.bump)
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
    if meta["srcslice"]["build_type"].lower() != "release":
        log(f"warning: srcslice build type is {meta['srcslice']['build_type']!r}, not Release")

    result = {"meta": meta, "inputs": {}, "results": {}}
    for name in sizes:
        repeat = args.repeat or DEFAULT_REPEAT.get(name, 3)
        result["inputs"][name], result["results"][name] = bench_input(
            name, inputs[name], srcslice, extra_args, repeat)

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
    if failed:
        log(f"FAILED inputs: {', '.join(failed)}")
    return 1 if (failed or (regression and args.fail_on_regression)) else 0


if __name__ == "__main__":
    sys.exit(main())
