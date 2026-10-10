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
./benchmark/run.sh                                   # run all three, compare with the latest baseline
./benchmark/run.sh --sizes small,medium              # fast loop while iterating (also medium-cpp)
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
--sizes small,medium,...      which inputs to run (default: all; see inputs.json)
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
```

## Notes

- Baselines are machine specific. Record them on the same machine, ideally in the
  dev container, and read deltas rather than absolute numbers across machines.
- `cache/` and `results/` are gitignored. The kernel inputs need about 19 GB of disk
  (one 2 GB checkout shared by large and x-large, a 3 GB and a 13 GB srcML archive; srcml
  writes the big one in under a minute on a 32-core machine). Delete `cache/` to start over.
- Each run is also written to `results/<timestamp>.json`, whether or not it becomes a
  baseline, so nothing is lost if you decide afterwards.
