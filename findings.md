# Performance findings

Review of the `benchmarks` branch (srcslice at `da079ae`, srcDispatch `09a669a`, srcSAX `93cbbee`)
against the v0.1.0 benchmark baseline. `dataflow.md` explains the pipeline these findings refer to;
the line numbers below are those of the reviewed revision. Everything listed is something a focused
change can fix, ordered by expected payoff. Measured numbers come from the dev container (Ubuntu
26.04, 32 cores, Release build) unless noted.

## Status (October 2026, srcslice v0.2.10)

`progress.md` records each step with its gate and numbers. Done, in order: S2 + M5 (condition-variable
manager, bounded backlog, in-order merge: output no longer depends on the thread count), S1 (one
resolution pass in `Finalize`; `ResolveCall` writes into the profile being resolved and resolves the
exact callee parameter first), M6/S8 (streamed output), M1 (32-byte `SlicePosition`), M3 (slim
signature parameters), M2 (interned profile strings and call names), plus the defaulted copy/move
members that let the merge move instead of copy; then S3 (dispatcher and SAX per-element
plumbing, v0.2.7), S4 (policy data handed over by move, v0.2.8, which also exposed and fixed
the `ControlPolicy` reset bug listed under correctness), the `ParameterProfile` half of S6 (index
by declaration position, v0.2.9), the JSON writer's per-position strings (same version), then
buffered JSON entries and pointer-keyed element lookups in the dispatcher (v0.2.10).

| input | wall v0.1.0 → v0.2.10 | max RSS v0.1.0 → v0.2.10 | output |
|---|---|---|---|
| medium (PostgreSQL) | 24.3 s → 9.5 s | 3.0 GB → 1.3 GB | 265 MB → 942 MB |
| medium-cpp (OpenCV) | 31.2 s → 13.2 s | 4.0 GB → 1.8 GB | 400 MB → 1.7 GB |
| small with `-c -a` | 22.0 s → 0.22 s | 81 MB → 53 MB | 4.7 MB → 5.7 MB |

The output grew because the slices now carry the full transitive closure through the call graph;
before, resolution was frozen after each unit's merge and most cross-unit data never arrived (or
arrived in the wrong entry, see "Correctness issues"). In recursive call families the closure a
profile gets still depends on the traversal order of the depth-first `ResolveCall` (documented in
`progress.md`, Step 2b; a fixpoint would make it exact). Still open: S5, the worker-side scans of S6
(`findValidProfile`, `FetchSliceProfile`, off the critical path), S7, S9, M4, M7, and the kernel
inputs have not been re-measured since v0.1.0. The parse thread is still the critical path on
medium (6.0 s of 9.0 s, libxml2 about 2 s of that), then `Finalize` (1.9 s) and the JSON output
(1.1 s).

## What the baseline says

| input | C/C++ source | files/units | srcslice wall | max RSS | RSS per MB of source | wall per MB |
|---|---|---|---|---|---|---|
| small (srcSlice) | 1.1 MB | 22 | 0.20 s | 78 MB | — | 0.18 s |
| medium (PostgreSQL) | 52.9 MB | 2,565 | 24.3 s | 3.0 GB | 57 MB | 0.46 s |
| medium-cpp (OpenCV) | 43.3 MB | 2,277 | 31.2 s | 4.0 GB | 92 MB | 0.72 s |
| large (kernel minus drivers) | 324 MB | 30,315 | 1,623 s (27 min) | 23.7 GB | 73 MB | 5.0 s |
| x-large (full kernel) | 1,454 MB | 64,579 | killed by the OOM killer after ~64 min | 46.6 GB at death (cgroup peak 49.5 GB) | >32 MB | >2.6 s |

Three things stand out:

1. **Time grows faster than input size.** PostgreSQL costs 0.46 s per MB; the kernel without
   drivers costs 5.0 s per MB, 11x more, for code of the same language and style. The difference is
   the unit count (30,315 vs 2,565): the per-unit merge work grows with the amount already merged
   (finding S1). The full kernel (64,579 units) did not finish: the host OOM killer ended it after
   about an hour at 46.6 GB resident, while it was still parsing (an earlier attempt had read 67 %
   of the archive after 47 minutes at 24 GB). On the current code x-large needs more than this
   machine's 60 GB and several hours.
2. **Only one core does real work.** During the kernel run the process had 2 threads and sat at
   105-120 % CPU on a 32-core machine: the libxml2 parse, the dispatcher, the merge and all
   interprocedural resolution are serial; the parallel part (Blob construction) is small and is
   throttled by the merge holding the backlog lock (S1, S2).
3. **Memory is proportional to everything ever seen.** Every declaration *and every reference to an
   undeclared name* becomes a `SliceProfile` kept until exit, and every use/def position carries its
   own copy of the file path (M1-M4). 3 GB for 53 MB of PostgreSQL is 57 bytes of resident memory per
   byte of source; the kernel subset needs 73 bytes per byte, and its 1.9 GB JSON output is built in
   memory three times over (M6).

## What the profile says (medium, PostgreSQL, 24 s)

`perf record` of the Release-equivalent `RelWithDebInfo` binary with the default 34 threads
(`benchmark/cache/profile/perf-*.txt`), plus two experiments:

- **Threads do not matter.** `-t 1` takes 24.4 s, `-t 34` takes 24.5 s. The worker pool contributes
  nothing to wall time because the parse thread and the manager thread serialize on `backlogMutex`.
- **Where the CPU goes** (share of all samples, all threads; the process uses ~1.4 cores on average):

  | share | what | finding |
  |---|---|---|
  | 16 % | `SrcSliceHandler::ResolveCall` self time, on the manager thread | S1 |
  | 6 % | `GetSimpleFunctionName` (string copies inside the `ResolveCall` scans) | S6, S9 |
  | 12 % + 7 % | `pthread_mutex_unlock` and `ManageThreads` self time: the spin loop | S2 |
  | 17 % | `Finalize → mergeFragments → vector::erase` moving `SliceProfile`s | S7 |
  | 9 % | `Finalize → ModifySlice` (`UpdateCalls`/`ResolveCall` retry) | S1, S6 |
  | 24 % | `malloc`/`free`/`memmove` from everywhere | S3, S4, M1 |
  | < 5 % | libxml2 (`xmlParseDocument` self) | — |
  | ~2 % | dispatcher hash lookups, `get_qualified_name`, `startElement` | S3 |

  The main thread spends about 22 % of all samples parsing (libxml2 + dispatcher + policies +
  `Notify`) and 27 % in `Finalize` after the parse; the manager thread accounts for most of the rest.
  Reading the XML is cheap. Everything expensive is bookkeeping we add on top of it.
- **Phases** (`srcslice -p`, medium): parse 14.2 s, second pass (`Finalize`) 8.5 s, JSON output
  about 2 s. During the parse the process has three threads (parser, manager, one worker at a
  time), during `Finalize` two, during output one.
- **Memory over time** (sampled `VmRSS`, medium): 1.2 GB after 8 s, 1.6 GB when `Finalize` ends at
  22 s, then 2.5 GB one second later while the JSON is built, 3.0 GB peak. The output path alone
  adds almost half of the peak (M6).
- **Allocation rate:** heaptrack counted 12.7 million allocation calls in the first 14 s of a
  single-threaded run (0.9 million per second); the single largest site by count is
  `std::string` construction, 3.1 million calls.

## Speed

### S1. Interprocedural resolution runs over the whole map after every unit, under the parser's lock

- Where: `src/cpp/srcslicehandler.cpp:133-210` (`MergeStructures`), called at `:240` inside the
  `lock_guard` on `backlogMutex` taken at `:230`; `ComputeInterprocedural` `:829`.
- What happens: after each unit's Blob is appended, `ComputeInterprocedural()` iterates **every**
  profile in the global `profileMap`. `ResolveCall` returns immediately for already-visited profiles,
  but the iteration itself is O(all profiles), so total work is O(units × profiles): quadratic in
  input size. With `-c` the same applies to `ComputeControlPaths`, which is far heavier per profile.
  Because the merge runs with `backlogMutex` held, the parse thread blocks in `Notify` (`:123`) for
  the duration, so parsing and merging never overlap.
- Evidence: 16 % of all CPU samples are `ResolveCall` self time on the manager thread and another
  6 % are the `GetSimpleFunctionName` copies it makes while scanning; the `-t 1` vs `-t 34` result
  shows the parser gains nothing from more workers.
- Scale of it: PostgreSQL produces 217,200 declared profiles (plus fragments) over 2,566 units, so
  the per-merge scans add up to roughly 2,566 × (average map size ≈ 110k) ≈ 3 × 10^8 profile visits
  for a 24 s run. The kernel subset has 12x the units and many more profiles, which is where the
  27 minutes go.
- Fix: do the interprocedural pass once, in `Finalize`, or restrict the per-merge pass to the
  profiles that were just merged plus the profiles whose `cfunctions` reference the functions this
  unit defined (track a `functionName → profiles with unresolved calls` index). Release
  `backlogMutex` before merging: take it only to pop/push the queue.
- Expected effect: removes the superlinear term; the kernel should scale like PostgreSQL per MB.
  Also lets the parser run concurrently with merging.

### S2. The manager thread spins

- Where: `ManageThreads`, `srcslicehandler.cpp:228-271`.
- What happens: `while (runningJobs)` re-takes `backlogMutex`, scans all `threadCount` slots and
  loops again with no wait. When all slots are busy or the backlog is empty it burns a full core
  and contends with the parse thread for the lock on every iteration.
- Evidence: `pthread_mutex_unlock` (12 %) and `ManageThreads` self time (7 %) are the second and
  fourth hottest symbols in the whole process.
- Fix: a `std::condition_variable` signalled by `Notify` (new unit) and by workers (job finished);
  wake the manager only then. Combined with S1 this makes the backlog lock nearly uncontended.

### S3. Per-element overhead in the dispatcher and SAX layer

- Where: `srcDispatch/src/dispatcher/srcDispatcher.hpp:910-1026` (`startElement`), `:1035`
  (`charactersUnit`), `:1069` (`endElement`); `srcDispatch/srcSAX/src/srcsax/sax2_srcsax_handler.cpp:108`
  and `:149`; `srcDispatch/src/policy_classes/Position.cpp:19`.
- Scale of it: the PostgreSQL archive has 8.6 million start tags for 53 MB of source (about 160
  elements per KB); the kernel subset has ~60 million and the full kernel ~215 million.
- What happens for every one of those elements:
  - the C layer `calloc`s an attribute array, `strndup`s every attribute value, `calloc`s the
    qualified element name for the element stack, and frees them all again;
  - `startElement` builds a `std::string` for the qualified name, parses `pos:start` and `pos:end`
    with two `substr` + two `stoi` each, builds `attributeName`/`attributeValue` strings and inserts
    them into a `std::map<std::string,std::string>` (`ctx.attributes`) that is cleared at the end of
    the same function and read by nobody in srcslice's policies, does a string-keyed
    `unordered_map` lookup into `process_map`, then fires an `xmlattribute` event per attribute by
    looking up the string literal `"xmlattribute"` in `process_map2` each time;
  - `charactersUnit` looks up `"tokenstring"` by string each call;
  - each event then goes through `std::function` → `openEventMap.find(state)` in the active policy.
- Fix: map element names to a `ParserState` enum once at the SAX boundary (perfect hash or a
  `switch` on length + first chars) and dispatch by enum; cache the `"unit"`, `"tokenstring"`,
  `"xmlattribute"` handlers in members instead of looking them up; parse positions with
  `std::from_chars` on the raw attribute value; drop the `ctx.attributes` map (or build it lazily);
  in srcSAX reuse a per-context scratch buffer for attributes and element names instead of
  `calloc`/`strndup`/`free` per element.
- Expected effect: this is the serial floor of the whole program. Measured on medium, the parse
  phase (everything before the last `</unit>`) is the majority of wall time; see profile section.

### S4. Every policy copies its data on every element close

- Where: `DataInner()` in every policy, e.g. `DeclPolicy.hpp:120` (`std::make_shared<DeclData>(data)`),
  `UnitPolicy.hpp:73`, `NamePolicy.hpp:71`; the reset `data = DeclData{}` at `DeclPolicy.hpp:181`.
- What happens: on `</name>`, `</expr>`, `</decl>`, `</block>` … the child's struct is copied into a
  fresh `shared_ptr` (copying every vector and string it owns) and then the original is destroyed.
  For a `NameData` with nested names this is several allocations per identifier token.
- Fix: `std::make_shared<T>(std::move(data))`, or keep `data` itself as a `shared_ptr<T>` that is
  handed over and replaced with a fresh one. Mechanical change across the policies.

### S5. A new `std::thread` per unit

- Where: `SrcSliceWorker::Start`, `src/cpp/srcsliceworker.cpp:45`; joined in `ManageThreads` `:237`.
- What happens: 64,579 thread creations and joins for the kernel. Each is tens of microseconds plus
  a fresh stack; the manager also only notices completion on its next spin.
- Fix: a fixed pool of `threadCount` workers pulling from the backlog with a condition variable.
  Pairs naturally with S2.

### S6. Name-keyed vectors are scanned linearly

- Where: `findValidProfile` (`src/cpp/srcsliceparse.cpp:61-80`), `ArgumentProfile`
  (`srcslicehandler.cpp:501-527`), `ResolveCall` and `UpdateCalls` loops over `spi->second`
  (`:712-731`, `:774-793`, `:911-933`), `FetchSliceProfile` (`srcsliceworker.cpp:1509`).
- What happens: `profileMap` is `unordered_map<name, vector<SliceProfile>>`. In the global map the
  vector for `i`, `ret`, `dev`, `p` holds every variable of that name in the whole archive (tens of
  thousands in the kernel), and the code either takes `.back()` (the most recently merged one, which
  after merging is an arbitrary unit's variable) or scans the vector comparing strings
  (`variableName`, `function`, `declPosition`) to find the right one. Inside a worker the vectors
  are small; in `ComputeInterprocedural`, `UpdateCalls` and `mergeFragments` they are not.
- Fix: index profiles by (name, file) or (name, function) in the global structures, or keep
  per-file maps and resolve cross-file references through `functionSigMap` only. At minimum,
  `ArgumentProfile` should look up the parameter's profile by its declaration position
  (`unordered_map<SlicePosition, SliceProfile*>`), which is what it ultimately compares against.

### S7. `mergeFragments` is quadratic in the number of fragments per name

- Where: `srcslicehandler.cpp:583-634`.
- What happens: for every fragment it scans all root fragments of all files
  (`possibleRootFragments`) and for each root all include names with `ends_with`, then erases the
  fragment from the middle of the vector (`profiles.erase(itr)`, `:625`, O(n) each). For a name
  like `printk` with a fragment in most of 30k files this is 30k erases from a 30k vector plus the
  nested scans.
- Evidence: 17 % of all CPU samples on medium are inside this `erase`, moving `SliceProfile`
  objects (each with eight strings and six sets) down the vector one slot at a time. It is the
  single hottest inclusive frame after `Finalize` itself, and it runs after the parse, so it is
  pure wall time.
- Fix: group roots by file in an `unordered_map<file, root*>` and resolve each fragment with direct
  lookups (same file, then each included file); collect survivors into a new vector instead of
  erasing in place.

### S8. The output path copies everything twice

- Where: `src/cpp/main.cpp:81` (`auto sliceProfileMap = …GetProfileMap()` copies the whole map),
  `:83-116` (`ostringstream` then `.str()`).
- Fix: `auto&`; stream directly to the destination `std::ostream` and handle the trailing comma
  with a `first` flag. Saves two full copies of the result at the point of peak memory.

### S9. Small per-call waste on hot paths

- `IsContained(SlicePosition& a, SlicePosition b)` takes `b` by value and `GetStart()`/`GetEnd()`
  return `DeltaElement<Position>` by value (`srcslicecollection.cpp:97-101`, `:151`): every
  containment test copies a string and two optionals. Used in every loop of `ComputeControlPaths`.
- `ComputeExitPaths` builds a string just to test emptiness
  (`innerContext.StartToString().empty()`, `srcslicehandler.cpp:344`, `:352`).
- `ProcessDecls`'s `isExtern` lambda takes the `TypeVector` by value (`srcsliceworker.cpp:253`).
- `GetSimpleFunctionName`, `ExtractName` and `ToString()` of names/types are re-run for the same
  data many times (`srcsliceworker.cpp:107`, `:154`; `GetTypeDetails` `:188`).
- `SlicePosition::operator<` (`srcslicecollection.cpp:131`) dereferences two `std::optional`s per
  comparison inside every `std::set` operation.
- `std::set<std::pair<std::string, SlicePosition>>` for `dvars`/`aliases` compares strings first on
  every insert.

## Memory

### M1. `SlicePosition` is ~200 bytes for 16 bytes of information

- Where: `src/headers/srcslicecollection.hpp` (`SlicePosition`), used in every `definitions`,
  `uses`, `dvars`, `aliases`, `controlEdges` set and in every `FunctionCallData` (twice).
- What happens: each position stores its own `std::string filename` (a heap allocation for any path
  longer than 15 characters, which is every path in these inputs) plus two `DeltaElement<Position>`
  (two `std::optional<Position>` each, 24 bytes apiece, plus an enum) and lives in a red-black
  tree node (32 bytes of pointers). Order of 200-250 bytes per use. A use is fully described by
  (file id, start line, start col, end line, end col) in 16-20 bytes.
- Fix: intern file paths in a table owned by the handler and store a 32-bit id; store positions as
  four `uint32_t`; keep `uses`/`definitions` as sorted vectors (or build them unsorted and
  sort+unique once in `Finalize`), which also removes per-node allocation.
- Expected effect: the largest single memory reduction, likely 5-10x on the position data that
  dominates `profileMap`.

### M2. Every `SliceProfile` repeats the same strings

- Where: `src/headers/srcsliceprofile.hpp`: `file`, `function`, `nameOfContainingClass`, `language`,
  `checksum`, `variableName`, `variableType`, `currentPointerReference`, `containingNameSpaces`.
- What happens: the file path, checksum (40 hex characters) and language are identical for every
  profile in a unit, and `function` is repeated for every variable in a function. Each is a separate
  heap string per profile; `SliceProfile` itself is roughly 700 bytes before any slice data.
- Fix: the same interning table as M1 (file, checksum, language, function name, class name) with
  integer ids; resolve back to strings only when printing.

### M3. `FunctionSignatureData` keeps the parse tree alive

- Where: `srcslicecollection.cpp:209` (`parameters = func->parameters`).
- What happens: the signature stores the `DeltaElement<shared_ptr<DeclData>>` vector straight from
  the unit model. Each `DeclData` references its `TypeData`, `NameData` (with nested names and
  template arguments) and default-value `ExpressionData`. So for every function in the archive a
  slice of the srcDispatch object model survives the unit's deletion. The code only ever needs each
  parameter's name string and name position (`UpdateCalls`, `ResolveCall`, `ArgumentProfile`).
- Fix: copy out `vector<{name string, SlicePosition, isPointer, isReference}>` in the constructor.

### M4. Fragments are full `SliceProfile`s and survive to the end

- Where: `srcsliceworker.cpp:1052-1063` (creation), `srcslicehandler.cpp:583` (consumption).
- What happens: every reference to a name that has no declaration *in the current unit* (library
  calls, globals from headers, macros, struct members reached through `FindName`) creates a
  `SliceProfile` with the full 700-byte layout plus a `uses` set. In a C codebase most identifiers
  used in a file are declared elsewhere, so fragments can outnumber real profiles. They are merged
  or dropped only in `Finalize`, i.e. at peak memory.
- Fix: represent fragments as `{name, file id, vector<position>}` in a separate map; or resolve
  fragments against already-merged roots at merge time and drop those that cannot possibly match
  (no root of that name exists yet and the file includes nothing relevant).

### M5. The backlog is unbounded

- Where: `SrcSliceHandler::Notify` `srcslicehandler.cpp:126`, `ManageThreads`.
- What happens: the parser pushes a fully built unit model for every `</unit>`; if the manager is
  busy merging (S1) or the slots are full, the models queue up. On the kernel the queue absorbs
  whole subsystems. Nothing limits it.
- Fix: cap the backlog (e.g. `2 × threadCount`) and make `Notify` wait on a condition variable when
  it is full. Once S1 is fixed the queue should rarely fill, but the cap keeps peak memory bounded.

### M6. Output doubles the peak

- Same as S8: map copy + `ostringstream` + `.str()` at the moment the long-lived data is largest.
  For PostgreSQL the JSON is 265 MB, for OpenCV 400 MB, for the kernel subset 1.9 GB.
- Evidence: on medium the process sits at 1.6 GB resident at the end of `Finalize` and peaks at
  3.0 GB while writing the output. For the kernel subset (23.7 GB peak, 1.9 GB JSON) the same
  pattern means roughly 8-10 GB of the peak is the output path. Two lines (`auto&`, stream
  directly) remove it.

### M7. The `DeltaElement`/`std::optional`/`std::any` model (larger refactor)

- Where: `srcDispatch/src/policy_classes/DeltaElement.hpp`, all `*Data` structs.
- What happens: srcDispatch is built for srcDiff, so every field and every child link is a
  `DeltaElement<T>` holding two `std::optional<T>` plus an operation, and heterogeneous children go
  through `std::any` + `shared_ptr`. A single identifier token costs a `NameData` with a
  `DeltaElement<std::string>` (two optionals of strings, 80 bytes), vectors, positions and its own
  control block, and is then copied on close (S4). This is also where the `bad_any_cast` family of
  bugs lived. A slim, non-diff representation (or a `DeltaElement` specialization that stores one
  value when the operation is `COMMON`) would roughly halve the transient parse memory and the
  per-unit allocation count, but it touches every policy.

## Correctness issues noticed on the way

Not performance, but worth a ticket each:

- `SlicePosition::operator<` orders by start line/column only (`srcslicecollection.cpp:131`), while
  `operator==` also compares the file and the end position. `std::set<SlicePosition>` treats two
  positions in different files with the same line:column as equal, so the second one is silently
  dropped; after `mergeFragments` combines uses from many files into a global's profile this loses
  uses.
- `GetSimpleFunctionName` uses `find_last_of("::")`/`find_last_of("->")` which match any single
  character of the set, and the `->` branch at `srcsliceworker.cpp:1161` splits on `"."`.
- `ManageThreads` reads `backlog.empty()` for the exit test only under the lock, but `Notify` can
  still be pushing after `unitsScaned` is observed true only if the parser sets the flag before the
  last `Notify`; the flag is set after `parse` returns, so this is safe today but fragile.
- srcDispatch `TypeData::copyAs`, `NameData::copyAs`, `ExpressionData::copyAs` cast the
  `DeltaElement` wrapper instead of its value (fixed in this working tree, not yet upstream), and
  srcSAX dereferences a possibly null libxml2 error (same).
- **`ControlPolicy` never reset after a loop** (fixed, v0.2.8, srcDispatch working tree). Its
  `<control>` close handler was missing the `return` after the depth check, so `depth = 0` was the
  body of the `if` and a normal close left `depth` set; the next `<control>` open was ignored and
  that loop was handed the previous loop's `ControlData` (its init/condition/incr overwritten for a
  classic `for`, kept for a range-based `for`, positions always stale). The policy objects live
  for the whole archive, so the stale data crossed units: on medium-cpp 22 entries carried
  positions from other files. Found when S4 made the hand-over a move: the second hand-over of a
  moved-from struct dereferenced a null `ConditionData`. Goldens re-recorded: small loses one use
  attributed to the wrong `m_str`, medium (C, no range-based loops) is unchanged, medium-cpp loses
  68 call entries, 11 definitions and 90 uses from other loops and re-attributes one use to the
  file it is in.
- **Multithreaded output was not reproducible** (fixed, v0.2.0). Two 32-thread runs on `small`
  differed in 8 of 4,669 entries because units were merged in completion order and resolution ran
  per merge. Units are now merged in archive order and resolved once, so `-t N` is byte-identical to
  `-t 1` and the benchmark harness fails a run whose threaded output differs from the golden.
- **Resolution wrote into the wrong profile** (fixed, v0.2.1). `ResolveCall` copied a callee
  parameter's data into `profileMap[name].back()`, the last profile sharing the argument's name,
  and pre-resolved `.back()` of every parameter name; with common names (`i`, `ctx`, `data`) the
  data landed in unrelated variables. It now writes into the profile being resolved and resolves the
  exact parameter (by file and position) first.
- **Worker or parse errors terminated the process** (fixed, v0.2.0): they now surface as a
  `std::runtime_error` naming the unit.
- The output is not valid JSON when a name contains a quote: `operator "" _a` is written
  unescaped (`srcsliceprofile.cpp:102` writes every string with bare `<<`), and duplicate top-level
  keys occur when identical files appear twice in an archive (same unit hash). 8 entries in `small`
  are affected; the regression checker compares those as text.

## Suggested order

1. ~~S1 + S2 + M5 together~~ (done, v0.2.0-v0.2.2).
2. ~~S8/M6, M3, M1 + M2~~ (done, v0.2.3-v0.2.6).
3. ~~S3 and S4 in srcDispatch/srcSAX~~ (done, v0.2.7-v0.2.8); ~~`ParameterProfile` index and the
   JSON writer's position strings~~ (done, v0.2.9); ~~buffered entries, pointer-keyed element
   lookups~~ (done, v0.2.10). Next: sorted vectors instead of `std::set` for the position sets (the
   set inserts of `ResolveCall` are the largest remaining item in `Finalize` and the node overhead
   about a quarter of the resident memory), M4 (slim fragments), S7, S5 (thread pool), then the
   `DeltaElement`/`std::any` model (M7), which is what is left of the parse floor.
4. M7 if the above are not enough for the full kernel.

Measure each step with `./benchmark/run.sh --sizes small,medium,medium-cpp`, confirm the tests
still pass (`ctest --test-dir build -L srcslice`), then `./benchmark/run.sh --bump minor --save-baseline`.
