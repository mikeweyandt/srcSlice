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

## Step 2a — resolution write target and recursion target (output change, reviewed) — done

Baseline **v0.2.1**. Goldens re-recorded (`--save-golden --force --note "2a: ..."`).
Combines the plan's 2a1 and 2a2 because 2a1 alone cannot be reviewed on its own (below).

What changed (`src/cpp/srcslicehandler.cpp`):

- `ResolveCall` writes the callee parameter's data into the profile being resolved (`sp`),
  not into `profileMap[sp.variableName].back()`, the last profile that happens to share the
  name (Bug A). The callee parameter's own calls are appended to `sp` after the loop.
- `ArgumentProfile` is replaced by `ParameterProfile(signature, index, functionName)`, which
  returns the parameter's own profile (declared, same simple function name, declared in the
  signature's file at the parameter's position) or nullptr. `ResolveCall` resolves that
  profile first (recursion, cycle-guarded by `visited`) and copies from it directly; the old
  code pre-resolved `.back()` of *every* parameter name of the signature and then scanned
  for the real one.
- `UpdateCalls` loses two dead scans (their result was never used; their only effect was the
  `.back()` pre-resolution). The overload branch keeps its rule "a candidate signature is
  accepted only if its parameter profile exists", now through `ParameterProfile`.

Tests: three `testInterprocedural` cases lost their `[!mayfail]`: same variable name in two
callers, data lands in the resolved parameter (both 2a1), and the two-hop case through a
non-last parameter (planned for 2a2). Still `[!mayfail]`: the three 2b cases (callee later
passes the parameter on, header global, mutual recursion), "argument named like the
parameter", and the file-blind `operator<` case.

Golden review (`--verify --diff-summary`, positions over all changed files; "moved" =
removed from one entry and added to another with the same name):

| input      | entries changed | field | removed | added | moved | lost | new |
|------------|----------------:|-------|--------:|------:|------:|-----:|----:|
| small      |   790 / 4,669   | use   | 3,840 | 10,600 | 2,822 | 1,018 | 7,778 |
|            |                 | definition | 24 | 55 | 19 | 5 | 36 |
|            |                 | calls | 2,965 | 5,908 | 2,108 | 857 | 3,800 |
| medium     | 49,251 / 217,200 | use  | 331,414 | 1,025,710 | 243,019 | 88,395 | 782,691 |
|            |                 | definition | 20,903 | 72,344 | 16,854 | 4,049 | 55,490 |
| medium-cpp | 41,183 / 243,246 | use  | 363,270 | 688,879 | 194,050 | 169,220 | 494,829 |

The plan's rule for 2a1 was "removed ⊆ added per name" (pure moves). Three quarters of the
removed `use` positions on small and medium are moves; the rest is "lost", and the
investigation of that remainder is the reason 2a1 and 2a2 were done together:

1. **With 2a1 alone the lost count barely changes when 2a2 is added** (small: 1,150 → 1,018
   uses), so the loss is not the recursion target.
2. A temporary diagnostic dumped the data of every profile that never reaches the output
   (fragments erased without a root in `mergeFragments`, profiles without a declaration).
   On small it accounts for 77 of the 354 name-level lost uses and all 5 lost definitions;
   on medium 167 of 72,492. That part is pollution removed: resolution results that the old
   code attached to an unrelated declared profile because the real owner was never printed.
3. The rest is **hop-2 data from chains that span units**: caller `a` (unit 1) → parameter
   `p` of `f` (unit 2) → parameter `q` of `g` (unit 3). The per-merge pass resolves `a`
   right after unit 2 arrives and copies `p` before `q`'s data has reached `p`; `visited`
   then freezes `a`. The old code still delivered `q`'s positions, into `.back()` of `a`'s
   name, because `p`'s calls were appended to `.back()` and resolved when *that* profile's
   turn came, later. Examples checked by hand: `funcSig` (srcsliceworker.cpp:1478) carried
   the positions of `FunctionCallData`'s `funcPos` parameter; `testCaseInfo`, `m_regex`, `f`
   and `p` carried a Catch benchmark helper's parameter use at catch.hpp:7048 that now sits
   only in that helper's own locals. Nothing in today's output receives these positions
   under the right name, because the owner was resolved too early. This is precisely the
   "frozen `visited`" problem Step 2b removes by resolving once after all units are merged,
   so these positions must come back there, in the right entries. The name-level lost sets
   are saved (`benchmark/cache/review/2a-lost-<input>.json`, from
   `benchmark/cache/save_lost.py`) and 2b's review checks them.

The gains are the mirror image: uses and definitions now reach every caller that passes a
variable on, not only the last profile of its name; on small 1,853 of 1,861 name-level new
use positions land in an entry of the position's own file.

Timing (baseline v0.2.1, 32 threads, output check `ok` for all four inputs; the baseline
was re-measured after the check-order fix described below):

| input      | v0.2.0 wall | v0.2.1 wall | Δ wall | v0.2.0 RSS | v0.2.1 RSS | output size v0.2.0 → v0.2.1 |
|------------|------------:|------------:|-------:|-----------:|-----------:|-----------------------------:|
| small      |     0.197 s |     0.194 s |   -2 % |      80 MB |      86 MB |            3.6 MB →   4.2 MB |
| small-ca   |      25.1 s |      37.7 s |  +50 % |      87 MB |      97 MB |            4.7 MB →   5.8 MB |
| medium     |      27.6 s |      22.8 s |  -17 % |   2,146 MB |   2,689 MB |          265.4 MB → 351.8 MB |
| medium-cpp |      30.4 s |      19.8 s |  -35 % |   2,771 MB |   3,198 MB |          399.9 MB → 481.4 MB |

Interleaved A/B against the Step 0 binary (same minute, medium, 32 threads): 27.7 / 25.6 s
before vs 22.8 / 21.9 s after, RSS 3.0 → 2.7 GB.

How the numbers came about:

- The first measurement of this state had medium at 36 s and small-ca at 38 s. A perf
  profile (medium, `-t 1`) showed `ResolveCall` at 18 % self time and
  `GetSimpleFunctionName` at 7 %: `ParameterProfile` extracted the function name of every
  same-named candidate before comparing positions, and names like `i` have thousands of
  candidates. Testing the declaration position first (same conjunction, cheaper term first)
  is output-neutral (goldens unchanged) and removed that cost. The remaining `ResolveCall`
  self time is the per-call scan over all signatures of a name and the set insertions.
- medium-cpp gains the most because `UpdateCalls` no longer pre-resolves `.back()` of every
  parameter of every candidate overload, and its dead scans are gone.
- **small-ca (`-c -a`) is 50 % slower and stays so until 2b.** With `-c` the per-merge
  `ComputeControlPaths` pass walks every position of every profile after every unit, and 2a
  distributes about 2.5× as many positions (uses 3,840 → 10,600 on this input, control edges
  4,461 → 11,565), so the quadratic pass costs proportionally more. 2b replaces it with a
  single pass at the end.
- Outputs are a third larger: every caller that passes a variable on now carries the
  callee's data, where before only the last same-named profile did. RSS follows the output
  size (the JSON is built in memory before it is written).

## Step 2b — resolve once in Finalize (S1; output change, reviewed) — done

Baseline **v0.2.2**. Goldens re-recorded (`--save-golden --force --note "2b: ..."`).

What changed (`src/cpp/srcslicehandler.cpp`, `srcslicehandler.hpp`, `srcsliceprofile.hpp/.cpp`,
`srcsliceworker.cpp`):

- `MergeStructures` only appends. The per-unit whole-map passes (`ComputeControlPaths`,
  `ComputeAliasInterprocedural`, `ComputeInterprocedural`) are gone from the merge.
- `Finalize` is one ordered pass: (1) control edges over every declared profile's own
  positions; (2) `mergeFragments` for every name; (3) `UpdateCalls` for every declared
  profile (completing calls whose function was not in the call's unit from the full
  signature table), then `ResolveCall` for every declared profile (recursion pre-resolves the
  callee parameters, so the map order does not matter); (4) alias expansion repeated until
  no list grows (cap 32).
- `partial`, `updated`, `ModifySlice` and `addPartialSlice` are deleted; fragments are no
  longer resolved on their own (their calls are resolved by the root they are glued into).

Tests: four more `testInterprocedural` cases lost `[!mayfail]` (callee later passes the
parameter on; argument named like the parameter; header global passed on; mutual recursion
across units). The mutual-recursion expectation gained the third call that the slice really
contains (m → ping's x → pong's y → ping again). Only the file-blind `operator<` case is still
`[!mayfail]`. All 15 `testCtrlEdges` cases (edges compared) still pass.

Golden review (`--verify --diff-summary` against the 2a goldens):

| input      | entries changed | use removed / added | calls removed / added | definition removed / added |
|------------|----------------:|--------------------:|----------------------:|---------------------------:|
| small      |    160 / 4,669  |         108 / 1,326 |           607 / 1,345 |                      0 / 3 |
| small-ca   |    837 / 4,669  |         108 / 1,326 |           607 / 1,345 |  0 / 3 (controlEdges 4,784 / 808) |
| medium     | 62,233 / 217,200 |   23,967 / 4,189,631 |   267,080 / 2,857,687 |          1,243 / 220,206 |
| medium-cpp | 42,596 / 243,246 |      565 / 3,911,557 |   123,266 / 4,754,879 |              77 / 38,534 |

**Gains.** The closure now reaches every caller. Before, a profile was resolved in the pass
right after its unit was merged and then frozen (`visited`), so anything that arrived later
in its call chain never reached it; `use` positions on medium go from 1.85 M to 6.0 M in
total, concentrated in hub variables (`root`, `parsetree`, `conn`, `estate`: the top entry
has 5,547 uses, 901 entries have more than 1,000; the median entry goes from 2 to 3). The
output files grow accordingly: medium 352 → 942 MB, medium-cpp 481 MB → 1.7 GB.
Of the positions 2a had lost, 26 % are back on medium under the same name (18,695 of 72,492
uses); the rest were owned by profiles that never reach the output.

**Control edges (small-ca).** 4,784 edges removed, 808 added: the per-merge pass used to
recompute a profile's edges after positions from other functions had been propagated into
it, producing pairs between a profile's own positions and foreign ones sorted by line. Now a
profile's edges are its own plus its callees' (the semantics `testCtrlEdges` pins).

**Losses, and what they are.** The plan's rule was "no entry loses a `use`". small loses 108
uses, all in one entry; medium loses 23,967 in 508 entries (60,878 gain). Every lost position
is still present in another entry of the output. The affected entries are parameters of
mutually recursive function families: Catch's `stringify` ↔ `StringMaker<T>::convert`
(`pair`, 156 → 48 uses), PostgreSQL's `transformJson*` (`ve`, `behavior`, `sublink`, `agg`),
`exec_stmt_*` (`estate`), `executeItem*` (`cxt`), the planner (`best_path`). `ResolveCall` is
a depth-first walk with a `visited` guard: the first profile to enter a cycle copies its
cycle-mates while they are still being resolved and ends up with a truncated closure; later
entrants get more. Which profile enters first depends on the traversal order, and the old
per-merge schedule simply entered these cycles in a different order. Neither result is the
complete slice. The complete answer needs a fixpoint (repeat the pass until nothing changes,
or resolve strongly connected components as a unit); that is outside the approved plan and is
raised as a decision in the recap. 2c (canonical order) makes the current result independent
of the archive's file order but does not change this.

The diff is accepted as the intended semantics of S1 (one resolution over the complete
structure) with the cycle behaviour documented, not as a loss-free step.

Timing (baseline v0.2.2, 32 threads, output check `ok` for all four inputs at 32 threads and
with `-t 8`; `ctest -R regression` green):

| input      | v0.2.1 wall | v0.2.2 wall | v0.2.1 RSS | v0.2.2 RSS | output v0.2.1 → v0.2.2 |
|------------|------------:|------------:|-----------:|-----------:|-----------------------:|
| small      |     0.194 s |     0.189 s |      86 MB |      78 MB |      4.2 MB →   4.4 MB |
| small-ca   |      37.7 s |     1.18 s  |      97 MB |      81 MB |      5.8 MB →   5.7 MB |
| medium     |      22.8 s |      25.3 s |   2,689 MB |   5,978 MB |   351.8 MB → 941.6 MB |
| medium-cpp |      19.8 s |      34.9 s |   3,198 MB |   9,362 MB |   481.4 MB →  1.69 GB |

Against the starting point (v0.1.0: medium 24.3 s / 3,021 MB / 265 MB output; medium-cpp
31.2 s / 3,973 MB / 400 MB):

- The pipeline work itself is far cheaper: `-c -a` on small went from 22 s to 1.2 s (the
  per-unit whole-map passes are gone), user time on medium is 27 s against 33 s, and the
  per-merge cost no longer grows with the number of units, which is what made the kernel
  11× more expensive per MB than PostgreSQL.
- Wall and RSS on the medium inputs are now dominated by the result itself: three to four
  times the positions, held in `std::set<SlicePosition>` (each position carries its file
  name as a `std::string`) and then serialised into one in-memory string before it is
  written (the output phase alone accounts for roughly half of the peak, as noted in
  `findings.md`). medium-cpp spends most of its 35 s producing 1.7 GB of JSON.
- Whether slices of this size are wanted is a product decision, not a performance one; see
  the options in the recap (cap the propagation depth, keep the complete closure, or compute
  it exactly with a fixpoint). The memory items in `findings.md` (shared file-name strings,
  streaming the output) become the next lever either way.

## Step 1b — defaulted copy/move members (output-neutral) — done

Baseline **v0.2.3**. Applied from the stash after 2b; with `partial` gone nothing depends on
what the hand-written copy constructors omitted, and the goldens are unchanged (`--verify`
exact on all four inputs with `-t 1`). `SliceProfile`, `SlicePosition`, `FunctionCallData`
and `SliceCtx` have `= default` copy and move operations; the merge appends, `std::partition`
and `erase` in `mergeFragments` now move profiles instead of deep-copying eight strings and
six sets.

| input      | v0.2.2 wall | v0.2.3 wall | Δ wall | v0.2.2 RSS | v0.2.3 RSS |
|------------|------------:|------------:|-------:|-----------:|-----------:|
| small      |     0.189 s |     0.198 s |   +5 % |      78 MB |      70 MB |
| small-ca   |     1.18 s  |     1.16 s  |   -2 % |      81 MB |      72 MB |
| medium     |      25.3 s |      16.5 s |  -35 % |   5,978 MB |   5,873 MB |
| medium-cpp |      34.9 s |      32.6 s |   -7 % |   9,362 MB |   9,261 MB |

medium's user time fell from 27.2 s to 17.6 s. medium-cpp gains little because its time is
the 1.7 GB output (next step).

## Memory step, part 1 — M1 + M6 (output-neutral) — done

Baseline **v0.2.4**. Goldens unchanged (byte-identical output), suite 9/9.

- **M1.** `SlicePosition` is now 32 bytes: an interned file name (one `std::string` per
  distinct path for the whole process, shared by every position in that file, via
  `SlicePosition::InternFileName` with a per-thread cache so workers do not take the lock
  per position) and four 32-bit coordinates, instead of its own `std::string` plus two
  `DeltaElement<Position>` (about 200 bytes). Every `uses`, `definitions`, `controlEdges`,
  `dvars`, `aliases` set element and both positions of every call entry shrink with it, and
  copying a position no longer allocates. Ordering, equality (incl. the file-blind
  `operator<`) and all string forms are ported unchanged.
- **M6.** `main.cpp` streams each slice straight to the file or stdout instead of building the
  whole document in an `ostringstream`, copying it into a string and writing that. The JSON
  writer iterates its sets by reference instead of copying every element.

| input      | v0.2.3 wall | v0.2.4 wall | v0.2.3 RSS | v0.2.4 RSS | Δ RSS |
|------------|------------:|------------:|-----------:|-----------:|------:|
| small      |     0.198 s |     0.189 s |      70 MB |      53 MB | -24 % |
| small-ca   |     1.16 s  |     0.27 s  |      72 MB |      53 MB | -27 % |
| medium     |      16.5 s |      13.2 s |   5,873 MB |   1,642 MB | -72 % |
| medium-cpp |      32.6 s |      21.7 s |   9,261 MB |   2,227 MB | -76 % |

Against the starting point v0.1.0, medium now produces 3.5× the slice data (942 MB vs
265 MB of JSON) in half the time (13.2 s vs 24.3 s) and half the memory (1.6 vs 3.0 GB);
medium-cpp 4.2× the data (1.7 GB vs 400 MB) in 21.7 s vs 31.2 s and 2.2 vs 4.0 GB.

## Memory step, part 2 — M3 (output-neutral) — done

Baseline **v0.2.5**. Goldens unchanged, suite 9/9, ThreadSanitizer suite clean.

`FunctionSignatureData` stored each parameter as the unit model's
`DeltaElement<shared_ptr<DeclData>>`, which kept the parameter's `DeclData`, `TypeData` and
`NameData` trees alive for the whole run although resolution only ever needs the parameter's
name and the position of that name. It now copies out a `SignatureParameter {name,
namePosition}`; `ParameterProfile` reads those directly. `Finalize` (with `-v`/`-p`) prints
what it starts from; on medium: 276,919 profiles (59,719 of them fragments), 1.22 M
positions and 488 k call entries before the closure, which grows them to 6.0 M positions
and about 3.3 M call entries.

| input      | v0.2.4 wall | v0.2.5 wall | v0.2.4 RSS | v0.2.5 RSS | Δ RSS |
|------------|------------:|------------:|-----------:|-----------:|------:|
| small      |     0.189 s |     0.184 s |      53 MB |      53 MB |   0 % |
| small-ca   |     0.274 s |     0.275 s |      53 MB |      51 MB |  -3 % |
| medium     |      13.2 s |      12.9 s |   1,642 MB |   1,492 MB |  -9 % |
| medium-cpp |      21.7 s |      21.2 s |   2,227 MB |   2,065 MB |  -7 % |

## Memory step, part 3 — M2 (output-neutral) — done

Baseline **v0.2.6**. Goldens unchanged, suite 9/9, ThreadSanitizer and AddressSanitizer suites
clean (the sanitizer builds need about 1.3 GB per translation unit; build them with `-j 8` on
a desktop, a full-parallel AddressSanitizer build froze the host on 2026-10-10).

`InternedString` (in `srcslicecollection.hpp`) holds a pointer to one process-wide copy of a
text; comparison and ordering are by text so sets keyed on it behave as before. It replaces
`std::string` for `SliceProfile::file`, `function`, `nameOfContainingClass`, `language`,
`checksum`, `variableType` and for `FunctionCallData::functionName`, i.e. for the strings that
are repeated per unit, per function or per call entry (about 3.3 M call entries on medium each
carried their own copy of the callee's name). The interner uses a per-thread cache and takes
its lock only on a miss; `SlicePosition::InternFileName` now goes through the same pool.

| input      | v0.2.5 wall | v0.2.6 wall | v0.2.5 RSS | v0.2.6 RSS | Δ RSS |
|------------|------------:|------------:|-----------:|-----------:|------:|
| small      |     0.184 s |     0.193 s |      53 MB |      51 MB |  -5 % |
| small-ca   |     0.275 s |     0.273 s |      51 MB |      50 MB |  -1 % |
| medium     |      12.9 s |      12.8 s |   1,492 MB |   1,242 MB | -17 % |
| medium-cpp |      21.2 s |      20.7 s |   2,065 MB |   1,747 MB | -15 % |

Remaining memory levers, not done: M4 (fragments are full `SliceProfile`s until `Finalize`;
60 k of 277 k profiles on medium), the `std::set` node overhead of the position sets (32 bytes
per element on top of the 32-byte position; sorted vectors built once in `Finalize` would
halve it), and M7 (the srcDispatch object model) for the transient parse memory.

## Step 3a — S3: dispatcher and SAX per-element plumbing (output-neutral) — done

Baseline **v0.2.7**. Goldens unchanged (`--verify` exact with `-t 1` and `-t 8`), suite 9/9 plus
the regression test, ThreadSanitizer and AddressSanitizer suites clean.

Profile of v0.2.6 that chose this step (medium, 32 threads, `benchmark/cache/profile/v026/`):
`-t 1` and `-t 32` both take 12.6 s, so the main thread is the critical path. Of its samples,
60 % are the parse (libxml2 itself about a third of that; `startElement` 26 %, `endElement`
11 %, `charactersUnit` 2 %), 20 % `Finalize` (`ResolveCall` 16 %, of which the
`ParameterProfile` scan is 5 %), 11 % the JSON writer (`StartToString` 5 %). The workers
together take a third of all samples but run alongside; the merge thread takes 2 %.

What changed, all in the srcDispatch and srcSAX submodules (`srcDispatcher.hpp`,
`srcDispatchUtilities.hpp`, `sax2_srcsax_handler.{hpp,cpp}`, `cppCallbackAdapter.hpp`,
`Position.{hpp,cpp}`, `ClassPolicy.hpp`, `FunctionPolicy.hpp`, `ConvertPlexerPolicy.hpp`):

- srcSAX converts each element's attributes and namespaces into arrays owned by the parser
  state (one value buffer) instead of a `calloc`, a `strndup` per attribute and the matching
  frees; the strings of the C element stack live in a deque instead of a `calloc`/`free` pair
  per element; the C++ adapter builds its stack entry in place.
- The dispatcher builds the qualified tag name in a reused buffer (no `std::string` per
  element, none per attribute), parses `pos:start`/`pos:end` in place
  (`Position(const char*)`, falling back to the string constructor for anything that is not
  `digits:digits`), resolves the handlers of the `unit`, `tokenstring` and `xmlattribute`
  events once instead of hashing the literal for every element or text node, and exposes the
  attribute list through `ctx.GetAttribute(name)` instead of filling and clearing a
  `std::map<std::string, std::string>` per element (`ctx.attributes` is gone; the two
  stereotype readers use the accessor).
- Events nobody listens to are not dispatched: the per-attribute `xmlattribute` event (no
  policy here handles it: about 21 M empty dispatches on medium) and the `tokenstring` event
  when the listener in charge has no text handler. `EventListener::HasOpenHandler` /
  `HasCloseHandler` answer that; the srcDiff plexer, which forwards events itself, overrides
  them to always say yes.

| input      | v0.2.6 wall | v0.2.7 wall | Δ wall | v0.2.6 RSS | v0.2.7 RSS |
|------------|------------:|------------:|-------:|-----------:|-----------:|
| small      |     0.193 s |     0.144 s |  -25 % |      51 MB |      54 MB |
| small-ca   |     0.273 s |     0.253 s |   -8 % |      50 MB |      54 MB |
| medium     |      12.8 s |      10.8 s |  -15 % |   1,242 MB |   1,244 MB |
| medium-cpp |      20.7 s |      18.5 s |  -11 % |   1,747 MB |   1,747 MB |

User time on medium fell from 14.4 s to 12.4 s; the parse phase from 8.1 s to about 6 s.

The srcDispatch submodule's own 27 tests (`testCall` … `testWhile`) cannot run in the dev
container: they shell out to `srcdiff`, which is not installed, and fail identically on the
pre-change build. The gate for submodule changes is therefore the srcslice suites, the
regression test, the goldens and the sanitizers.

## Step 3b — S4: policy data handed over by move, plus a ControlPolicy fix — done

Baseline **v0.2.8**. Suite 9/9 plus the regression test, ThreadSanitizer and AddressSanitizer
suites clean; goldens re-recorded for the bug fix below (review follows) and `--verify` exact
with `-t 8` against them. Phase split on medium afterwards: parse 6.05 s, `Finalize` 2.5 s,
10.4 s in all.

What changed (srcDispatch `srcDispatchUtilities.hpp`, the 28 policies, `ControlPolicy.hpp`):

- `PolicyDispatcher::HandOver(data)` replaces `std::make_shared<T>(data)` in every policy's
  `DataInner()` except the srcDiff plexer's: the struct is moved into the `shared_ptr` on the
  first `Data<T>()` request of a `NotifyAll` and later requests of the same `NotifyAll` get the
  same pointer; `NotifyAll` forgets it afterwards. Before, every request deep-copied the struct
  (a `NameData` or `ExpressionData` with its vectors of `std::any`, and the whole `UnitData`
  tree once per unit on the parse thread). Audit before the change: every policy assigns a
  fresh `data` in its open handler before reading it, and each has one listener.
- The move exposed a bug the copies had hidden. `ControlPolicy`'s `<control>` close handler was
  missing the `return` after `if(!depth || depth != ctx.depth)`, so `depth = 0` was the body of
  the `if` and a normal close never reset `depth`; the next `<control>` open was ignored and
  that loop was handed the previous loop's `ControlData` (init/condition/incr overwritten by
  the new loop's children for a classic `for`, kept for a range-based `for`, start/end
  positions always stale). The policy objects live for the whole archive, so the stale data
  crossed units. With the move the second hand-over dereferenced a null `ConditionData`
  (found by bisecting the small archive down to a classic `for` followed by a range-based `for`
  in a later function). Fixed by the `return`.

Golden review (`--verify --diff-summary` before re-recording):

| input      | entries changed | removed                           | added | what the diff is |
|------------|----------------:|-----------------------------------|------:|------------------|
| small      |       1 / 4,669 | 1 use                             |     0 | `m_str` declared at catch.hpp:3545 loses the use at catch.hpp:15513:41, the condition of a loop in `XmlEncode::encodeTo` (another class's `m_str`) |
| small-ca   |       1 / 4,669 | 1 use                             |     0 | the same entry |
| medium     |               0 | —                                 |     — | C code, no range-based loops |
| medium-cpp |    22 / 243,246 | 68 calls, 11 definitions, 90 uses |     1 | every removed position is in a different file than its entry (e.g. `d` of test_intrin_utils.hpp:160 with uses at ccm/operations.cpp:62); the one addition re-attributes a use of `layers` (net_impl.hpp:102) from op_timvx.cpp:508:68 to net_openvino.cpp:508:68, the file whose loop it is |

| input      | v0.2.7 wall | v0.2.8 wall | Δ wall | v0.2.7 RSS | v0.2.8 RSS |
|------------|------------:|------------:|-------:|-----------:|-----------:|
| small      |     0.144 s |     0.136 s |   -6 % |      54 MB |      54 MB |
| small-ca   |     0.253 s |     0.234 s |   -7 % |      54 MB |      54 MB |
| medium     |      10.8 s |      11.0 s |   +1 % |   1,244 MB |   1,243 MB |
| medium-cpp |      18.5 s |      18.5 s |    0 % |   1,747 MB |   1,747 MB |

Neutral on the medium inputs within run-to-run noise (medium's fastest run was 10.7 s): the
per-close copies were not among the parse thread's hot spots once S3 had removed the plumbing
around them. Kept because it removes a copy of the whole unit model per unit on the parse
thread and the allocation churn of the per-close copies, both of which grow with unit size;
it can be reverted independently of the `ControlPolicy` fix (one line) if the moved-from
state of a policy's `data` between close and the next open is unwanted.

## Step 3c — S6 (ParameterProfile index) and the JSON writer (output-neutral) — done

Baseline **v0.2.9**. Goldens unchanged (`--verify` exact with `-t 1` and `-t 8`), suite 9/9 plus
the regression test, ThreadSanitizer and AddressSanitizer suites clean.

- `Finalize` builds `declaredByPosition` once the fragments are glued (profiles do not move
  after that): every declared profile with a complete declaration position, keyed on the
  interned file pointer and the four coordinates, which is exactly what
  `SlicePosition::operator==` compares. `ParameterProfile` looks the parameter's declaration
  position up and checks the few candidates for name and simple function name in the order
  their name's vector holds them, so it returns what the scan returned. It used to walk every
  profile of the parameter's name (thousands for `i`, `p`, `ctx`) for every call entry, once
  from `UpdateCalls` and once from `ResolveCall`; outside `Finalize` the scan remains as the
  fallback. `UpdateCalls` no longer copies each call entry, its name and every candidate
  signature. The index is dropped when `Finalize` returns.
- The JSON writer writes positions and numbers straight into the stream
  (`SlicePosition::WriteStart`/`WriteEnd`, `std::to_chars`) instead of building a `std::string`
  per position: the same bytes, no temporaries.

| input      | v0.2.8 wall | v0.2.9 wall | Δ wall | v0.2.8 RSS | v0.2.9 RSS |
|------------|------------:|------------:|-------:|-----------:|------------:|
| small      |     0.136 s |     0.139 s |   +2 % |      54 MB |      53 MB |
| small-ca   |     0.234 s |     0.223 s |   -5 % |      54 MB |      53 MB |
| medium     |      11.0 s |      9.69 s |  -12 % |   1,243 MB |   1,268 MB |
| medium-cpp |      18.5 s |      14.2 s |  -23 % |   1,747 MB |   1,776 MB |

Phase split on medium: parse 5.8 s, `Finalize` 2.0 s (was 2.5 s), everything else (mostly the
942 MB of JSON) 1.6 s; 9.4 s in all. medium-cpp gains most because its C++ call entries
resolve against names with many same-named profiles. The index costs about 25 MB while
`Finalize` runs, which shows in the peak RSS.

## Step 3d — buffered JSON entries and pointer-keyed element lookups (output-neutral) — done

Baseline **v0.2.10**. Goldens unchanged (`--verify` exact with `-t 1` and `-t 8`), suite 9/9
plus the regression test, ThreadSanitizer and AddressSanitizer suites clean.

- Each JSON entry is assembled in one reused `std::string` (`SliceProfile::AppendJson`,
  `SlicePosition::AppendStart/AppendEnd`) and written with a single `write` call instead of
  some dozens of `operator<<` calls per entry, each with its sentry, virtual `xsputn` and
  codecvt check. `operator<<(ostream&, SliceProfile&)` remains and produces the same bytes.
- The dispatcher looks element handlers up through a direct-mapped cache keyed on libxml2's
  dictionary pointers for (localname, prefix); every hit is verified against the stored text,
  so a pointer that means something else only costs a miss. The `elseif` rewrite still goes
  through the string map, and the cache is reset whenever `process_map`/`process_map2` change.
  The srcDiff namespace test remembers the last pointer that was not it.
- `FunctionCallData::operator<` does one three-way compare of the (interned) function names
  instead of an equality test followed by a less-than.

| input      | v0.2.9 wall | v0.2.10 wall | Δ wall | v0.2.9 RSS | v0.2.10 RSS |
|------------|------------:|-------------:|-------:|-----------:|------------:|
| small      |     0.139 s |      0.135 s |   -3 % |      53 MB |       54 MB |
| small-ca   |     0.223 s |      0.223 s |    0 % |      53 MB |       53 MB |
| medium     |      9.69 s |       9.48 s |   -2 % |   1,268 MB |    1,272 MB |
| medium-cpp |      14.2 s |       13.2 s |   -6 % |   1,776 MB |    1,782 MB |

The output phase on medium fell from about 1.6 s to 1.1 s (medium-cpp writes 1.7 GB, hence
its larger gain); the parse phase did not move measurably (6.0 s), so the per-element string
hashing the cache replaces was cheaper than its share of samples suggested. Phase split on
medium: parse 6.0 s, `Finalize` 1.9 s, output 1.1 s; 9.0 s in all.

## Step 4 — sorted vectors for the profile sets (output-neutral) — done

Baseline **v0.2.11**. Goldens unchanged (`--verify` exact with `-t 1` and `-t 8`), suite 9/9
plus the regression test, ThreadSanitizer and AddressSanitizer suites clean (plus a `-c -a` run
under AddressSanitizer, since control edges and aliases exercise the sets too).

`FlatSet<T>` (`src/headers/flatset.hpp`) replaces `std::set` for the six collections of a
`SliceProfile` (`uses`, `definitions`, `cfunctions`, `dvars`, `aliases`, `controlEdges`), for
the two locals of `Finalize` and the control-edge helpers' parameters. It is a sorted,
duplicate-free `std::vector` with the operations the code uses (insert, range insert, erase,
find, iteration, equality); it keeps `std::set`'s element order and its rule that the first
inserted of two equivalent elements stays (the file-blind `SlicePosition::operator<` makes
positions in different files with the same line and column equivalent, see findings), so the
result is the same. What changes is the cost: a position no longer carries a 32-byte tree
node plus the allocator's overhead, and merging one set into another (`ResolveCall`,
`mergeFragments`, the alias pass) is one linear `set_union` instead of a tree insertion per
element, with an append fast path when everything new sorts after what is there. A range
insert checks whether its input is sorted and unique first and falls back to element-wise
insertion otherwise, so it stays correct for any input. `Finalize` drops the vectors' slack
before the output is written.

| input      | v0.2.10 wall | v0.2.11 wall | Δ wall | v0.2.10 RSS | v0.2.11 RSS | Δ RSS |
|------------|-------------:|-------------:|-------:|------------:|------------:|------:|
| small      |      0.135 s |      0.126 s |   -7 % |       54 MB |       52 MB |  -4 % |
| small-ca   |      0.223 s |      0.207 s |   -7 % |       53 MB |       52 MB |  -2 % |
| medium     |       9.48 s |       7.43 s |  -22 % |    1,272 MB |      846 MB | -34 % |
| medium-cpp |       13.2 s |       9.00 s |  -32 % |    1,782 MB |    1,173 MB | -34 % |

Phase split on medium: parse 6.0 s, `Finalize` 0.77 s (was 1.9 s), output and the rest 0.5 s;
7.2 s in all. The parse thread is now five sixths of the run.

## Step 5 — M7, first part: the object model's `any` holds its pointer in place — done

Baseline **v0.2.12**. Goldens unchanged (`--verify` exact with `-t 1` and `-t 8`), suite 9/9
plus the regression test, ThreadSanitizer and AddressSanitizer suites clean, plus `small -c -a`
and `medium-cpp` (the C++ model) under AddressSanitizer.

Every child link of the srcDispatch model is a `shared_ptr` to a data struct held in a
`std::any` (`DeltaElement<std::any>`: the statements of a block, the parts of an expression,
the names in a name, the types of a type). `std::any` keeps anything larger than one pointer
on the heap, so each link cost a heap block of its own next to the control block the
`shared_ptr` already has, plus the copies of those blocks whenever the link was copied.
`srcDispatch::Any` (`srcDispatch/src/dispatcher/Any.hpp`) stores values of up to two pointers
in place (a `shared_ptr` is exactly that) and keeps the heap for anything larger; it offers
the operations the code used (construction and assignment from any value, copy, move,
`type()`, `has_value()`, `reset()`, `any_cast` by value or by pointer, `bad_any_cast`). The
change is a rename in 44 files (`std::any` → `srcDispatch::Any`, `std::any_cast` →
`srcDispatch::any_cast`), nothing else.

| input      | v0.2.11 wall | v0.2.12 wall | Δ wall | v0.2.11 RSS | v0.2.12 RSS |
|------------|-------------:|-------------:|-------:|------------:|------------:|
| small      |      0.126 s |      0.118 s |   -7 % |       52 MB |       51 MB |
| small-ca   |      0.207 s |      0.200 s |   -3 % |       52 MB |       52 MB |
| medium     |       7.43 s |       6.77 s |   -9 % |      846 MB |      845 MB |
| medium-cpp |       9.00 s |       8.69 s |   -3 % |    1,173 MB |    1,173 MB |

Phase split on medium: parse 5.4 s (was 6.0 s), `Finalize` 0.8 s, the rest 0.5 s; 6.7 s in
all. The transient model never shows in the peak RSS, which is set after the parse.

## Step 5b — per-name flags in the element slot (output-neutral) — done

Baseline **v0.2.13**. Goldens unchanged (`--verify` exact with `-t 1` and `-t 8`), suite 9/9
plus the regression test, ThreadSanitizer and AddressSanitizer suites clean.

`startElement`/`endElement` tested the tag text against eight literals per element
(`argument_list`, `type`, `function`, `function_decl`, `block`, `if`, `namespace`, `name`,
`super_list`) and copied it twice. The element slot of the pointer-keyed cache now carries
those answers, decided once per distinct name, the tag text is copied once (into
`ctx.currentTag`), and the attribute-name and first-attribute-value tests check the first
character before calling `strcmp`. The C++ adapter's element stack finds the `type` attribute
the same way.

| input      | v0.2.12 wall | v0.2.13 wall | Δ wall | RSS      |
|------------|-------------:|-------------:|-------:|----------|
| small      |      0.118 s |      0.115 s |   -3 % | 52 MB    |
| small-ca   |      0.200 s |      0.201 s |    0 % | 51 MB    |
| medium     |       6.77 s |       6.65 s |   -2 % | 844 MB   |
| medium-cpp |       8.69 s |       8.44 s |   -3 % | 1,173 MB |

Phase split on medium: parse 5.3 s, `Finalize` 0.8 s, the rest 0.4 s; 6.5 s in all.

## Step 5c — namespace cache in srcSAX, no teardown at exit (output-neutral) — done

Baseline **v0.2.14**. Goldens unchanged (`--verify` exact with `-t 1` and `-t 8`), suite 9/9
plus the regression test, ThreadSanitizer and AddressSanitizer suites clean.

- srcSAX's `start_element_ns` compared every element's prefix and URI against every namespace
  of the root (two or three `strcmp` of a 30-character URI per element) to hand the policies
  the root's copy of the string. libxml2 passes the same interned pointer for element after
  element, so a one-entry cache per kind answers it: the previous element's result is reused
  when the pointer and its text are unchanged (the text check keeps a reused pointer from
  going wrong), otherwise the search runs and refills the cache.
- `main` no longer destroys the `SrcSliceHandler` at exit: freeing the hundreds of thousands
  of slices one by one took 2 % of a medium run, for memory the process returns anyway. Every
  thread is joined before the constructor returns, so nothing depends on the destructor.

| input      | v0.2.13 wall | v0.2.14 wall | Δ wall | RSS      |
|------------|-------------:|-------------:|-------:|----------|
| small      |      0.115 s |      0.113 s |   -2 % | 52 MB    |
| small-ca   |      0.201 s |      0.200 s |    0 % | 52 MB    |
| medium     |       6.65 s |       6.54 s |   -2 % | 845 MB   |
| medium-cpp |       8.44 s |       8.30 s |   -2 % | 1,174 MB |

The dispatcher and SAX plumbing is at diminishing returns now: the parse thread is 5.3 s of
6.5 s on medium, about 2 s of it libxml2 and the rest the policies building the object
model. `-t 1` and `-t 32` take the same time because one thread parses the whole archive.

## Step 6 — S10: the archive is parsed by several parsers at once (output-neutral) — done

Baseline **v0.2.15**. Goldens unchanged (`--verify` exact with `-t 1`, `-t 8` and `-t 32`),
suite 9/9 plus the regression test; `testThreading` gained a case that runs the 64-unit
archive with 2, 3, 4, 8, 16 and 32 parsers against the single-threaded result.
ThreadSanitizer: suite clean, `small` with 4 parsers and `medium` with 16 parsers clean;
AddressSanitizer: suite, `small -c -a` and `medium-cpp` with 8 parsers clean.

The parse thread was the critical path of every run since Step 1: one thread read the whole
archive, so `-t 1` and `-t 32` took the same time. A srcML archive is a root `<unit>` holding
one `<unit>` per file, never nested deeper, and every `<` inside source text is escaped, so
the byte range of each unit can be found with one pass over the bytes (`ScanArchive`,
`src/cpp/srcslicearchive.cpp`: `memchr` for `<`, a quote-aware scan for the end of each
start tag, self-closing units handled; anything unexpected falls back to one parser).
Parser *j* of *N* then reads the prolog, units *j*, *j + N*, *j + 2N*, … and the root's end
tag through libxml2's IO callback (`ChunkStream`), from its own handle on the file, so no
part of the archive is held in memory, and hands its units over with their archive positions
as sequence numbers. The manager merges by sequence number as before, from queues keyed on it,
so the main structure is built exactly as a single parser builds it: the goldens are
byte-identical at every parser count.

The round-robin deal is what keeps the pipeline bounded: a parser may hand a unit over only
when fewer than `maxInFlight` units are ahead of the merge frontier (`Notify`), so the unit
the manager needs next always finds a slot, and every parser is woken when a merge moves the
frontier (the first version forgot that wake-up and deadlocked on the second unit). Each
unit's `#include` list now travels with the unit and reaches `fileDependencyTable` when the
unit is merged, so the table is filled in archive order whichever parser read the file. The
parsers' libxml2 contexts are created on one thread (srcSAX's one-time initialization is not
thread-safe), the active parser and the sequence counters are thread-local, and a parser
that fails aborts the run like a worker does. `--parsers N` sets the count (default: half
the threads); `-t 1` is the old path.

Parser sweep on medium (`-t 32`, two runs each; the backlog and in-flight knobs make no
difference, the queues never fill):

| parsers | wall   | max RSS  |
|--------:|-------:|---------:|
|       1 | 6.3 s  |   844 MB |
|       4 | 2.87 s | 1,005 MB |
|       8 | 2.42 s | 1,103 MB |
|      16 | 2.22 s | 1,234 MB |
|      32 | 2.12 s | 1,427 MB |

Memory grows with the parser count, about 18 MB per parser on medium: each parser holds the
unit model it is building, and some of PostgreSQL's generated files are enormous. The queues
are not the cause. On the kernel subset the same cost is small next to the slices themselves.

| input      | v0.2.14 wall | v0.2.15 wall | Δ wall | v0.2.14 RSS | v0.2.15 RSS | Δ RSS |
|------------|-------------:|-------------:|-------:|------------:|------------:|------:|
| small      |      0.113 s |      0.100 s |  -12 % |       52 MB |       62 MB | +20 % |
| small-ca   |      0.200 s |      0.181 s |  -10 % |       52 MB |       62 MB | +20 % |
| medium     |       6.54 s |       2.24 s |  -66 % |      845 MB |    1,273 MB | +51 % |
| medium-cpp |       8.30 s |       3.94 s |  -53 % |    1,174 MB |    1,618 MB | +38 % |
| large      |        64 s* |       29.6 s |  -54 % |     9.2 GB* |      9.8 GB |  +7 % |

(* large at v0.2.11, the last measurement before this step; one repeat each.) A golden for
`large` was recorded with `-t 1` (1,649,630 entries in 24,610 files) and the 32-thread run
with 16 parsers matches it byte for byte. User time on
medium went from 7.9 s to 9.7 s (sixteen dispatchers instead of one), wall from 6.5 s to
2.2 s. For a memory-bound run, `--parsers 4` keeps most of the gain for a third of the cost.

## Step 7 — `ResolveCall` merges the callees' positions level by level (output-neutral) — done

Baseline **v0.2.16**. Goldens unchanged (`--verify` exact with `-t 1` and `-t 32`), suite 9/9
plus the regression test, ThreadSanitizer and AddressSanitizer suites clean.

With the parse spread over sixteen parsers, `Finalize` is the largest serial phase (0.75 s of
2.2 s on medium, 2.2 s of 3.8 s on medium-cpp), and inside it `ResolveCall` is 85 %: each
caller merged every callee parameter's positions and calls into its own sets one callee at a
time, each merge a pass over sets that grew with every callee. `ResolveCall` now records the
callees' position, alias, dependence and edge ranges (pointers into the resolved parameters'
sets, which nothing changes again) and merges them into the profile once per set with
`FlatSet::InsertRanges`: adjacent pairs of ranges are united level by level, log(k) linear
passes instead of k merges. Two rules keep the result identical to the callee-by-callee
merge: the pending ranges are flushed before a callee is resolved for the first time
(that resolution may read this profile in a recursive family and must find what the old
code had merged by then), and a parameter passed to its own function is merged directly
after a flush, because its ranges would point into the sets being replaced (the first
version crashed on medium there). A sort-based batch was tried first and was slower: the
inputs are already sorted ranges.

What stays callee by callee: `definitions` (its inserts interleave with erases) and the call
entries, whose ordering is not a strict weak ordering (an unresolved call's empty definition
position compares equivalent to any other), so the order of their merges decides which
entries survive; reordering them changed 22 entries on medium-cpp. That is the larger half
of `ResolveCall`'s merge cost and waits for the ordering fix (findings, correctness).

| input      | v0.2.15 wall | v0.2.16 wall | Δ wall | RSS      |
|------------|-------------:|-------------:|-------:|----------|
| small      |      0.100 s |      0.098 s |   -2 % | 62 MB    |
| small-ca   |      0.181 s |      0.180 s |    0 % | 62 MB    |
| medium     |       2.24 s |       2.24 s |    0 % | 1,252 MB |
| medium-cpp |       3.94 s |       3.74 s |   -5 % | 1,593 MB |

`Finalize` on medium-cpp: 2.17 s → 2.07 s (the position merges; the call-entry merges remain).

## Step 8 — Finalize's per-name passes and the output on the worker threads (output-neutral) — done

Baseline **v0.2.17**. Goldens unchanged (`--verify` exact with `-t 1` and `-t 32`), suite 9/9
plus the regression test; ThreadSanitizer (suite, `small` with 4 parsers, `medium` with 16)
and AddressSanitizer (suite, `small -c -a`, `medium-cpp` with 8 parsers) clean.

- `Finalize` ran everything on the main thread. Three of its passes touch one name's profile
  vector at a time and read everything else: fragment gluing (`mergeFragments`, reads
  `fileDependencyTable`), call completion (`UpdateCalls`, reads the signature table and the
  declaration index) and the shrink pass. `ForEachProfileGroup` now spreads them over the
  `-t` threads; `BuildDeclaredIndex` and `ResolveCall` stay on one thread (the latter is the
  recursive, order-dependent part).
- The JSON writer formatted and wrote entry by entry on the main thread. The entries are now
  formatted in chunks of about 2 MB (estimated from the sets' sizes, so an entry with a
  million positions is a chunk of its own) on up to eight threads, a wave of eight chunks at
  a time, while the previous wave is written in order; a buffer an oversized entry inflated
  is released after its wave. A first version with 256-entry chunks and a wave per thread
  held hundreds of megabytes of the document at once (medium's `pstate` profiles alone carry
  a million positions); this one holds at most two waves.

| input      | v0.2.16 wall | v0.2.17 wall | Δ wall | v0.2.16 RSS | v0.2.17 RSS | Δ RSS |
|------------|-------------:|-------------:|-------:|------------:|------------:|------:|
| small      |      0.100 s |      0.100 s |    0 % |       62 MB |       66 MB |  +8 % |
| small-ca   |      0.181 s |      0.182 s |   +1 % |       63 MB |       70 MB | +11 % |
| medium     |       2.24 s |       1.90 s |  -16 % |    1,252 MB |    1,324 MB |  +6 % |
| medium-cpp |       3.74 s |       3.28 s |  -13 % |    1,593 MB |    1,691 MB |  +6 % |

Phase split on medium: parse 1.1 s, `Finalize` 0.62 s (was 0.75 s), the rest 0.2 s; on
medium-cpp `Finalize` 1.83 s (was 2.07 s), of which `ResolveCall` is nearly all. The few
megabytes on small are the thread stacks and buffers of a run that is over in a tenth of a
second.

## Step 9 — S5: a pool of worker threads (output-neutral) — done

Baseline **v0.2.18**. Goldens unchanged (`--verify` exact with `-t 1` and `-t 32`), suite 9/9
plus the regression test; ThreadSanitizer (suite, `small` with 4 parsers, `medium` with 16)
and AddressSanitizer (suite, `small -c -a`, `medium-cpp` with 8 parsers) clean.

A per-thread profile of a 32-thread run (medium, `benchmark/cache/profile/v0217/`) showed
where the parse phase's 1.1 s go now that sixteen parsers share the machine with the
workers: the parsers wait a third of their time in `Notify` for the merge window, the merge
thread is idle most of the time (0.8 thread-seconds in all), and the workers run near
saturation. The run created a thread per unit (2,565 of them), and thread creation, stack
set-up and the per-thread locale initialization were about a tenth of the workers' time and
most of the system time. The handler now keeps `-t` persistent threads fed from a job queue:
the manager enqueues a started unit, a pool thread runs `Perform`, then does the completion
accounting, after which nothing on that thread touches the worker again (the per-worker
`onDone` callback, which ran before `Perform` returned, is gone). An aborted run marks the
units still queued as finished and skips them.

| input      | v0.2.17 wall | v0.2.18 wall | Δ wall | v0.2.17 RSS | v0.2.18 RSS |
|------------|-------------:|-------------:|-------:|------------:|------------:|
| small      |      0.100 s |      0.099 s |   -2 % |       66 MB |       62 MB |
| small-ca   |      0.182 s |      0.181 s |   -1 % |       70 MB |       66 MB |
| medium     |       1.90 s |       1.83 s |   -3 % |    1,324 MB |    1,354 MB |
| medium-cpp |       3.28 s |       3.16 s |   -4 % |    1,691 MB |    1,745 MB |

The single-threaded run gains more (medium 6.5 s → 5.9 s), where a thread per unit weighed
more. The parse phase is CPU-bound now: parsers and workers together exceed the cores, so
the next gains there are in the workers' own cost (allocation churn, string formatting),
not in the pipeline.

## Measured, not applied — a file-aware ordering for the slice sets

The sets order positions with `SlicePosition::operator<`, which compares the start line and
column only (findings, correctness): positions of different files with the same line and
column are equivalent and one is silently dropped, and a call entry whose definition
position is unset is equivalent to any other, so the order in which entries are merged
decides which survive. That is also what blocks the larger half of `ResolveCall`'s merge
cost (Step 7). A strict weak ordering (file text, then start line and column; call entries
likewise) was built and measured on top of v0.2.16, with `--verify --diff-summary`:

| input      | entries changed | uses added | definitions added | call entries added |
|------------|----------------:|-----------:|------------------:|-------------------:|
| small      |               0 |          0 |                 0 |                  0 |
| medium     |   3,679 / 217k  |     19,216 |               941 |              2,706 |
| medium-cpp |  16,857 / 243k  |     97,098 |             1,449 |             81,311 |

The added uses are the positions the file-blind order had dropped (a global's uses from
several files). The added call entries are the ones a non-transitive equivalence had
collapsed: on medium-cpp most are duplicates of resolved calls with an empty definition
position or with an empty function name, which the writer then prints, so enabling the
ordering also needs a decision about those entries. `testDeterminism` stopped finishing
within five minutes with the ordering enabled (not investigated). The change is reverted;
`FlatSet` keeps the comparator parameter it gained, and now uses its comparator in every
operation (its equivalence tests and append fast path used the raw `<`, which is why the
first measurement showed millions of duplicates). The test case for the defect keeps its
`[!mayfail]` tag.

## Tried and reverted — direct-indexed event handlers

`EventListener::HandleEvent` finds the handler for an event in an `unordered_map<ParserState,
std::function>` per listener, twice per element. Three replacements were measured on top of
v0.2.11 (goldens unchanged for all three):

| variant                                         | medium  | medium-cpp | small                      |
|-------------------------------------------------|--------:|-----------:|----------------------------|
| array with one slot per state                   |  -6 %   |    -1 %    | +10 % wall, 52 → 85 MB RSS |
| flat vector of (state, heap-allocated handler)  |  -2 %   |    +1 %    | 52 → 50 MB                 |
| deque of (state, handler)                       |  -2 %   |    -1 %    | +7 % wall, 52 → 56 MB      |

The array's cost comes from the dispatcher keeping every lazily created policy object alive
for the whole archive: a deeply nested C++ unit leaves thousands of them, each now carrying
two 86-slot tables of `std::function` (5 KB). The two compact variants keep the memory but
give up most of the speed: the handler objects must not move while one of them runs (a
running handler registers further handlers), so they need an indirection or a block
container. None is a clear win, so the map stays. Also measured and dropped: glibc malloc
tunables (`mmap_threshold`, `trim_threshold`, `top_pad`) against the 1.3 s of system time on
medium, which made no difference; the 220 k page faults are the resident set being touched
once.

## The kernel input, re-measured at v0.2.10

`large` (the kernel without `drivers/`, 324 MB of C in 30,315 units) had only its v0.1.0
measurement until now. One run at v0.2.10 (`--sizes large --repeat 1 --save-baseline --force`):

| version | wall       | user   | max RSS   | output  | wall per MB of source |
|---------|-----------:|-------:|----------:|--------:|----------------------:|
| v0.1.0  | 1,623 s    | —      | 23.7 GB   | 1.9 GB  | 5.0 s                 |
| v0.2.10 | 81 s       | 93 s   | 14.4 GB   | 10.2 GB | 0.25 s                |
| v0.2.11 | 64 s       | 78 s   | 9.2 GB    | 10.2 GB | 0.20 s                |
| v0.2.15 | 29.6 s     | —      | 9.8 GB    | 10.2 GB | 0.09 s                |

The superlinear term is gone: PostgreSQL costs 0.18 s per MB and the kernel subset 0.25 s,
against 11× at v0.1.0. The output is five times larger (the complete closure). Memory is the
kernel's constraint: the sorted vectors of v0.2.11 took it from 14.4 GB to 9.2 GB (28 bytes
per byte of source), most of what remains being the position and call-entry data of the
slices themselves. `x-large` (the full kernel, 1.45 GB of source) was not re-run; by these
numbers it would need about five minutes and roughly 40 GB, the container's limit.

## Where things stand (v0.1.0 → v0.2.18)

| input      | wall v0.1.0 | wall v0.2.18 | RSS v0.1.0 | RSS v0.2.18 | output v0.1.0 | output v0.2.18 |
|------------|------------:|-------------:|-----------:|------------:|--------------:|---------------:|
| small      |     0.202 s |      0.099 s |      78 MB |       62 MB |        3.6 MB |         4.4 MB |
| small-ca   |      22.0 s |      0.181 s |      81 MB |       66 MB |        4.7 MB |         5.7 MB |
| medium     |      24.3 s |       1.83 s |   3,021 MB |    1,354 MB |      265.4 MB |       941.6 MB |
| medium-cpp |      31.2 s |       3.16 s |   3,973 MB |    1,745 MB |      399.9 MB |        1.69 GB |
| large      |     1,623 s |       23.6 s*|  23.7 GB   |      9.9 GB*|        1.9 GB |        10.2 GB |
| x-large    |     killed* |        153 s*|  46.6 GB*  |     36.2 GB*|             — |        41.7 GB |

(* large and x-large at v0.2.18, one repeat each, 2026-10-11. x-large, the full kernel, was not
feasible at v0.1.0: the OOM killer ended it after about an hour at 46.6 GB with no output; at
v0.2.18 it runs in 153 s inside the 40 GB container cap. small-ca's v0.1.0 figure is its first measurement, taken
with the Step 0 binary.) The slices now contain the complete transitive closure through the
call graph, which is 3.5-5× more data than before, computed in 8-10 % of the time and about
44 % of the memory on the medium inputs, and in 1.5 % of the time and 42 % of the memory on
the kernel subset. The parse is spread over `threads / 2` parsers, the units over a pool of
`threads` workers and the output over eight threads; what runs on one thread is
`ResolveCall` (0.5 s of medium's 1.8 s, 1.7 s of medium-cpp's 3.2 s), whose call-entry
merges wait for the set-ordering decision above.
Threaded runs are reproducible and identical to `-t 1`; a run with a failing unit reports
the unit instead of aborting the process; `-c -a` costs a fraction of a second on small
instead of 22 s.
