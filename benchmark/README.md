# srcslice benchmarks

Times `srcslice` on srcML archives of five inputs from four real codebases and keeps
**versioned baselines**, so every performance change can be compared with the previous
version and with the first one.

| name       | input                                                          | C/C++ source  | repeats |
|------------|----------------------------------------------------------------|---------------|---------|
| small      | https://github.com/srcML/srcSlice                              | ~1 MB, C++    | 5       |
| medium     | https://github.com/postgres/postgres                           | ~53 MB, C     | 3       |
| medium-cpp | https://github.com/opencv/opencv (`modules/`, `hal/`, `apps/`) | ~43 MB, C++   | 3       |
| large      | https://github.com/torvalds/linux without `drivers/`           | ~324 MB, C    | 3       |
| x-large    | https://github.com/torvalds/linux, full tree                   | ~1.45 GB, C   | 1       |

Inputs are pinned to exact commits in `inputs.json` so all versions slice identical code.
Only C/C++ files (`.c .h .cc .cpp .cxx .hh .hpp .hxx`) go into the archives. An optional
`"paths"` list restricts an input to subdirectories (OpenCV skips its vendored `3rdparty/`)
and `"exclude"` prunes subdirectories (large drops `drivers/`, 78 % of the kernel's C).
Inputs that share a repository and commit share one checkout under `cache/repos/`.
`medium-cpp` is the input that exercises the C++ handling (classes, templates, structured
bindings) the C codebases never touch. `x-large` is a stress test: with the current
srcslice it runs for about an hour and needs well over 20 GB of RAM, so skip it with
`--sizes` while iterating and run it for baselines.

## Quick start (inside the dev container)

```bash
./benchmark/run.sh                                   # quick set (small, medium, medium-cpp) vs the latest baseline
./benchmark/run.sh --sizes small                     # fastest loop while iterating
./benchmark/run.sh --sizes all                       # adds the kernel inputs: large ~30 min per run, x-large OOMs
./benchmark/run.sh --history                         # every baseline version, Δ vs previous and vs first
```

The first run clones each input (shallow), converts it once with `srcml --position --hash`
into `cache/archives/`, builds an optimized `srcslice` in `build-release/`
(`-DCMAKE_BUILD_TYPE=Release`), and times it. Later runs reuse the clones and archives.

## Recording a performance update

```bash
# 1. change code, then check it against the latest baseline
./benchmark/run.sh

# 2. keeper? bump the version and store the new baseline
./benchmark/run.sh --bump minor --save-baseline --note "cache variable lookups"

# 3. commit VERSION, benchmark/baselines/vX.Y.Z.json and the code together
```

`--bump major|minor|patch` rewrites the root `VERSION` file (also the CMake project
version). `--save-baseline` writes `baselines/v<VERSION>.json` and refuses to overwrite
an existing version unless `--force` is given. With `--force` and a `--sizes` subset only
the inputs you ran are replaced in that version's file and the others are kept, so an
hour-long `x-large` can be added or refreshed on its own:

```bash
./benchmark/run.sh --sizes small,medium,medium-cpp,large --save-baseline --note "initial"
./benchmark/run.sh --sizes x-large --save-baseline --force      # adds x-large to the same version
```

The latest baseline is the highest version number present, so the files can be
committed in any order.

## Output regression tests

Every input also has a recorded *golden* slice in `golden/<input>.json`, so a performance
change can be checked for changed results, not just changed timing:

```bash
./benchmark/run.sh --verify                          # quick set: slice once each, compare with the goldens (~2 min)
./benchmark/run.sh --verify --sizes small            # just srcSlice itself (a second)
./benchmark/run.sh --verify --srcslice-args="-t 8"   # the same with 8 threads (must match once merging is deterministic)
ctest --test-dir build -R regression                 # same check for `small`, wired into ctest
```

The quick set is `small`, `small-ca`, `medium` and `medium-cpp`. `small-ca` is the srcSlice
repository sliced with `-c -a` (control edges and alias expansion; set per input with
`"args"` in `inputs.json`), so changes to those passes get a golden diff too. It takes about
25 s today because the control-edge pass is recomputed after every unit (S1 in findings.md).

A timing run checks its last output as well (`output check` column; `--no-verify` skips
it), and the exit code is 1 whenever an output differs. On a mismatch the console shows
the first lines of `cache/out/<input>.verify.txt`, which lists the changed files and, per
changed entry, the fields that differ and the positions added or removed. Add
`--diff-summary` to get, at the top of the report, totals over *all* changed files: per
field, positions removed and added, how many of the removed ones reappear in another entry
with the same variable name ("moved") and how many are genuinely lost or new, plus the
names with the most churn. That summary is the review instrument for a change that is
meant to alter the output:

```
~ file src/backend/parser/parse_expr.c
  ~ node-1234-17-9f2b...
      use: -1 +0
        - "src/backend/parser/parse_expr.c:1250:9"
```

What is compared is a canonical form of the output, not the bytes: entries are matched
by key, object keys are sorted and the set-valued arrays (`use`, `definition`, `calls`,
`dependence`, `aliases`, `controlEdges`) are compared as sets. Reordering output or
changing its whitespace is therefore not a regression; dropping or adding a position is.
The committed manifest holds one hash per source file (so a diff in git shows which files
moved) and `cache/golden/<input>.canon.gz` keeps the full canonical output that the
entry-level diff needs. Entries srcslice writes as invalid JSON (it does not escape quotes
in names such as `operator "" _a`) are compared as text.

**Goldens are recorded with `-t 1` and must match at every thread count.** Since the
in-order merge (perf plan Step 1) srcslice merges units in archive order whatever the thread
count, so a threaded run is byte-for-byte reproducible and checked against the same golden.
When a threaded timing run differs, the check reruns that input with `-t 1` to tell the two
failure kinds apart: `THREADED OUTPUT DIFFERS in N file(s); -t 1 matches` is a threading
defect (the merge order leaked into the result), a plain `MISMATCH` is an output change.
Both make the run exit 1. `--allow-thread-divergence` downgrades the first kind to
`ok (-t 1); threaded run differs in N file(s) (allowed)` for bisecting old commits. Before
Step 1 the threaded output changed from run to run (8 of 22 files between two 32-thread runs
of `small`) because interprocedural resolution saw units in completion order.

When a change is *meant* to alter the output (a fixed slice), re-record and commit the
goldens with the change:

```bash
./benchmark/run.sh --save-golden --force --note "fix cross-file position collisions"
```

Goldens exist for the quick set. The kernel input `large` has none yet: it takes about 30
minutes and 24 GB per run, so record it deliberately when a large check is wanted
(`./benchmark/run.sh --save-golden --sizes large`, then `--verify --sizes large`).
`--save-golden --sizes all` skips inputs whose latest baseline recorded a failure (`x-large`
cannot finish on 60 GB). A golden records the input commit, the srcslice commit
and the arguments it was produced with; verifying against a golden recorded for a different
input commit or with different arguments prints a warning first.

## Reading the tables

The run summary lists, per input: file count, source and archive size, one-off srcml
conversion time, then `srcslice` wall time (median and min over the repeats), user/sys
CPU time, maximum resident memory and output size.

The comparison shows the baseline median, the current median and the delta:
`▲ +7.3%` is slower than the baseline by more than the threshold (default 5 %),
`▼ -12.0%` is faster, `· +1.1%` is within noise. Warnings are printed when the input
commit, build type, thread count, srcslice arguments or host differ from the baseline.

`--history` prints one table per input across all versions with `Δ prev` (vs the
previous version) and `Δ first` (overall improvement since the first baseline).
Add `--format markdown` to paste either table into a PR.

## All options

```
--sizes quick|all|small,...  which inputs to run (default quick = small,medium,medium-cpp; all adds large,x-large)
--repeat N                   override the per-size repeat count
--srcslice PATH              time this binary instead of building build-release/
--srcslice-args="-t 8"       extra arguments passed to srcslice (use the = form)
--compare-to latest|all|v0.1.0|FILE
--threshold PCT              regression/improvement marker threshold (default 5)
--fail-on-regression         exit 1 when any input is slower than the threshold
--save-baseline [--force]    store this run as baselines/v<VERSION>.json (--force + --sizes: update those inputs only)
--bump major|minor|patch     bump VERSION first (requires --save-baseline)
--note TEXT                  free text stored with the result/baseline
--history [--format markdown]
--update-inputs              move inputs.json pins to the current default-branch heads

--verify                     slice each input once (-t 1), compare with golden/, exit 1 on a difference
--save-golden [--force]      record the current output as the golden for the selected inputs
--no-verify                  skip the output check after the timing runs
--allow-thread-divergence    do not fail when only the threaded output differs (the -t 1 recheck matches)
--diff-limit N               files detailed in a mismatch report (default 10)
--diff-lines N               report lines echoed to the console (default 40)
--diff-summary               on a mismatch, aggregate moved / lost / new positions over all changed files
```

## Notes

- Baselines are machine specific. Record them on the same machine, ideally in the
  dev container, and read deltas rather than absolute numbers across machines.
- `cache/` (including the full golden outputs) and `results/` are gitignored. The kernel inputs need about 19 GB of disk
  (one 2 GB checkout shared by large and x-large, a 3 GB and a 13 GB srcML archive; srcml
  writes the big one in under a minute on a 32-core machine). Delete `cache/` to start over.
- Each run is also written to `results/<timestamp>.json`, whether or not it becomes a
  baseline, so nothing is lost if you decide afterwards.
