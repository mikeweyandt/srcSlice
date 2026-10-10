# Performance work log

Running record of the S1 + S2 + M5 plan (see `findings.md` for the findings these steps
address and `benchmark/README.md` for the measurement tooling). One section per step:
what changed, how it was gated, and what it measured. Numbers come from
`./benchmark/run.sh` in the dev container (32 cores, Release build) unless stated otherwise;
wall is the median of the run's repeats, RSS the maximum. Results files live in
`benchmark/results/`, versioned baselines in `benchmark/baselines/`.

## Reference point (before any change)

Measured 2026-10-10 on the Step 0 binary (commit 478e660, behaviour identical to v0.1.0):

| input      | files | runs | wall    | user    | max RSS  | output   |
|------------|------:|-----:|--------:|--------:|---------:|---------:|
| small      |    22 |    5 | 0.205 s | 0.359 s |    78 MB |   3.6 MB |
| small-ca   |    22 |    5 |  22.0 s |  22.1 s |    81 MB |   4.7 MB |
| medium     | 2,565 |    3 |  24.9 s |  33.3 s | 3,019 MB | 265.4 MB |
| medium-cpp | 2,277 |    3 |  31.6 s |  40.3 s | 3,975 MB | 399.9 MB |

(`small-ca` is `small` run with `-c -a`; its 22 s are the per-unit control-edge pass over the
whole map, which Step 2b removes.) The default thread count in the container is 34; the
pipeline was fully serial, so `-t 1` and `-t 34` took the same time, and the threaded output
was not reproducible (8 of 22 `small` files differed between two runs).

## Step 0 — test scaffolding (no behaviour change) — done

Commit 478e660. Added the output-regression goldens (`-t 1`, incl. `small-ca`), the
`CanonicalSlices` harness, strict `CompareJson`, control-edge checks turned on,
`testInterprocedural` (15 cases, both unit orders), `testDeterminism` (160 generated units),
the `SRCSLICE_SANITIZER` build option and `--diff-summary`. Eight interprocedural cases and
two determinism cases are `[!mayfail]`, each naming the step expected to fix it.

Gate: unit tests 8/8, ThreadSanitizer suite clean, `--verify` exact on the quick set with
`-t 1`, `--verify --srcslice-args="-t 8"` mismatching as documented.

## Step 1 — S2 + M5 + in-order merge (output-neutral) — done

Baseline **v0.2.0** (`benchmark/baselines/v0.2.0.json`, results `20261010T223732Z`).

What changed (`src/cpp/srcslicehandler.cpp`, `src/cpp/srcsliceworker.cpp`, headers, `main.cpp`):

- The manager thread sleeps on a condition variable instead of re-taking the backlog mutex in
  a loop while merging (S2). The parser is woken by a second condition variable.
- The backlog of parsed units is bounded (`--backlog`, default = threads); the parser blocks in
  `Notify` when it is full (M5). Started-but-unmerged units are bounded too (`--in-flight`,
  default = 2 x threads). A worker drops its unit model as soon as its Blob exists.
- Units are merged strictly in archive order (a reorder buffer in start order), so the result
  no longer depends on which worker finishes first: `-t N` output is byte-identical to `-t 1`.
- Failures no longer `std::terminate`: a worker exception aborts the run and the constructor
  throws `std::runtime_error` naming the unit; srcSAX's `SAXError` / `std::string` throws are
  translated the same way. `main.cpp` takes the profile map by reference (M6).
- The merge itself (appends + the per-unit whole-map passes) is unchanged; that is Step 2.

Tests: `test/testThreading.cpp` (8 cases: every thread / backlog / in-flight combination gives
the single-threaded result; bounds hold; more threads than units; one unit; no units; failing
worker in the first, a middle and the last unit at 1/4/16 threads; non-`std::exception`
throw; truncated and empty archive). The determinism thread-count case lost its `[!mayfail]`.
`bench.py` now fails a timing run whose threaded output differs from the golden
(`--allow-thread-divergence` to downgrade).

Gate: unit tests 9/9 (debug and ThreadSanitizer, no reports); `--verify` exact with `-t 1` and
with `-t 8`; the timing run's output check is `ok` at 32 threads for all four inputs.

| input      | before wall | v0.2.0 wall | Δ wall | before RSS | v0.2.0 RSS | Δ RSS |
|------------|------------:|------------:|-------:|-----------:|-----------:|------:|
| small      |     0.205 s |     0.197 s |   -4 % |      78 MB |      80 MB |  +3 % |
| small-ca   |      22.0 s |      25.1 s |  +14 % |      81 MB |      87 MB |  +7 % |
| medium     |      24.9 s |      27.6 s |  +11 % |   3,019 MB |   2,146 MB | -29 % |
| medium-cpp |      31.6 s |      30.4 s |   -4 % |   3,975 MB |   2,771 MB | -30 % |

The wall deltas above are between runs an hour apart on a desktop that was in use; an
interleaved A/B of the two binaries right after (same minute, alternating) gives the real
picture:

| run (32 threads unless noted) | before  | Step 1  |
|-------------------------------|--------:|--------:|
| medium, pair 1                |  26.1 s |  26.1 s |
| medium, pair 2 (machine busy) |  30.8 s |  31.4 s |
| small-ca `-t 1`               |  25.0 s |  25.1 s |
| small-ca `-t 32`              |  22.5 s |  25.5 s |

- **Wall on medium is unchanged.** The merge thread was and is the bottleneck: after every
  unit it runs `ComputeInterprocedural` (and with `-c`/`-a` the control-edge and alias passes)
  over the whole map, so removing the spin and overlapping parse with merge cannot shorten the
  critical path. That work goes away in Step 2b. User time dropped slightly (the spin is gone),
  system time rose (condition-variable hand-offs).
- **small-ca is 12 % slower at 32 threads, and equal at `-t 1`.** Merging in completion order
  happened to merge short units first, which keeps the per-merge whole-map passes cheaper; the
  archive order does not. Same cause, same fix (2b). The price of a reproducible result.
- **RSS is down 29-30 % on both medium inputs**: the unbounded backlog held about 1 GB of
  parsed unit models; at most `threads` of them wait now.
- `-t 1` on medium is about 10 % slower than before (27.5 s vs 25 s in one-off runs) because
  the parser is throttled to one unit ahead; `--backlog 8` recovers most of it. Not tuned
  further, since 2b changes the balance.

## Step 1b — defaulted copy/move members — measured, parked (not applied)

Planned as an output-neutral refactor: `= default` copy and move constructors / assignments
for `SliceProfile`, `SlicePosition`, `FunctionCallData` and `SliceCtx`, deleting the
hand-written copy constructors so that merging and `mergeFragments` move profiles instead of
deep-copying them. The edit is kept in `git stash` (`step-1b: ...`).

**It is not output-neutral.** The hand-written `SliceProfile` copy constructor omitted
`partial`, and the old code *copied* profiles at every hand-over (Blob vector growth in the
worker, the merge appends, `std::partition` and `erase` in `mergeFragments`), so the flag the
worker sets for calls it could not resolve in its own unit was almost always lost before
`Finalize` looked at it. With real moves the flag survives and `Finalize` runs
`UpdateCalls` + `ResolveCall` for every such profile, i.e. the cross-unit resolution that
Step 2b is meant to introduce deliberately starts happening now, but through today's
`.back()` write targets (Bug A). `--verify --diff-summary` on the result:

| input      | entries changed | calls removed / added | use removed / added | definition removed / added |
|------------|----------------:|----------------------:|--------------------:|---------------------------:|
| small      |     134 / 4,669 |             181 / 874 |             0 / 887 |                      0 / 8 |
| medium-cpp | 33,654 / 243,246 |    60,733 / 1,037,254 |     2,540 / 577,304 |                  54 / 12,971 |

On `small` every removed call is an unresolved entry (empty `definitionPosition`) replaced by
its resolved overloads, and the gains are the propagated uses: the intended direction. On
`medium-cpp` 2,461 `use` positions and 48 `definition` positions disappear outright and most
additions land in unrelated entries (e.g. `gridSize` in `calibController.cpp` gains 53 calls
invoked from `imgproc`, `dnn` and `objdetect` files), because the last profile of a name is
a different one at `Finalize` time than during the per-merge pass. That diff cannot be
reviewed against a rule.

Timing (two runs each, output check skipped, not saved as a baseline):

| input      | v0.2.0 wall | 1b wall | v0.2.0 RSS | 1b RSS   | v0.2.0 output | 1b output |
|------------|------------:|--------:|-----------:|---------:|--------------:|----------:|
| small      |     0.197 s | 0.189 s |      80 MB |    71 MB |        3.6 MB |    3.7 MB |
| small-ca   |      25.1 s |  25.1 s |      87 MB |    79 MB |        4.7 MB |    4.9 MB |
| medium     |      27.6 s |  21.8 s |   2,146 MB | 2,520 MB |      265.4 MB |  355.9 MB |
| medium-cpp |      30.4 s |  45.7 s |   2,771 MB | 3,762 MB |      399.9 MB |  633.0 MB |

The 21 % gain on medium is the move semantics the step was after (less copying in
`mergeFragments` and the merge appends). The 50 % loss on medium-cpp, the larger RSS and the
one-third larger outputs are the unintended resolution work and its mis-targeted writes.
The clean number for 1b can only be taken once nothing depends on `partial` any more.

**Decision: reorder.** Apply 2a1 (write into the profile being resolved) and 2b (resolve once
in `Finalize`, delete the `partial` gating) first; then 1b is a pure refactor again and its
gate is "goldens unchanged". Nothing else in the plan changes.
