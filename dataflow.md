# How data flows through srcslice

A walk from the srcML archive on disk to the JSON on stdout, in the order things
happen, with the file where each step lives. Describes the `benchmarks` branch after the
performance work of October 2026 (`progress.md` has the step-by-step record; sections 1-3 and
5 still cite the line numbers of the srcDispatch/srcSAX and worker code, which did not move).

```
 archive.xml ──libxml2 SAX2──▶ srcSAX (C) ──▶ srcSAXController/cppCallbackAdapter (C++)
                                                      │
                                                      ▼
                                   srcDispatcher<UnitPolicy>  (element-name → event routing)
                                                      │  open/close events + token strings
                                                      ▼
                        policy stack: UnitPolicy ▶ FunctionPolicy ▶ BlockPolicy ▶ ExprStmtPolicy ▶ ExpressionPolicy ▶ NamePolicy …
                                                      │  each </element> copies its *Data into the parent
                                                      ▼
                            </unit>  ──▶  SrcSliceHandler::Notify  ──▶  bounded backlog (waits when full)
                                                                                   │
                     parse thread ◀── backlogMutex + two condition variables ──▶ ManageThreads thread
                                                      │                            │ one std::thread per unit, ≤ threadCount at a time
                                                      │                            ▼
                                                      │               SrcSliceWorker::Perform → Blob (per-unit profileMap …), unit model dropped
                                                      ▼
                           MergeStructures (outside the lock, in archive order): Blob ⊎ global profileMap/functionSigMap — appends only
                                                      │
                                                      ▼  after the last unit
            Finalize(): ComputeControlPaths (-c) → mergeFragments → UpdateCalls + ResolveCall for every declared profile → alias fixpoint (-a)
                                                      │
                                                      ▼
                               main(): profileMap by reference → JSON streamed to file/stdout
```

---

## 1. libxml2 → srcSAX (C layer)

- `srcSAXController` (`srcDispatch/srcSAX/src/cpp/srcSAXController.cpp`) wraps a `srcsax_context`
  created by `srcsax_create_context_filename` (`srcsax_controller.cpp:172`). The whole archive is one
  libxml2 push-parser document; `xmlParseDocument` runs **on the calling thread** and never returns
  until the last byte is consumed (`srcsax_controller.cpp:342`).
- The SAX2 callback table is built by `srcsax_sax2_factory()`
  (`srcsax/sax2_srcsax_handler.cpp:36`). The callbacks are swapped as parsing proceeds:
  `start_root` → `start_element_ns_first` → `start_element_ns`, and `characters_first` →
  `characters_unit`, so per-element work after the first unit is `start_element_ns` (`:572`),
  `end_element_ns` (`:653`) and `characters_unit` (`:891`).
- Per element, the C layer: `calloc`s a `srcsax_namespace`/`srcsax_attribute` array and `strndup`s
  every attribute value (`libxml2_attributes2srcsax_attributes`, `:108`), `calloc`s the
  qualified element name and pushes it on `srcml_element_stack` (`:149`), calls the C++ handler, then
  frees everything (`:633-634`). The `srcsax_handler` function table it calls is filled by
  `cppCallbackAdapter::factory()` (`srcSAXController.cpp:377`), which forwards to the virtual methods of
  `srcSAXHandler` implemented by `srcDispatcher`.

## 2. srcDispatcher: element names → events

`srcDispatch/src/dispatcher/srcDispatcher.hpp`, class `srcDispatcher<policies...>`
(srcslice instantiates `srcDispatcher<srcDispatch::UnitPolicy>` at `src/cpp/srcslicehandler.cpp:51`).

- `startUnit` (`:874`) copies the unit attributes into `ctx.currentFilePath`, `currentFileLanguage`,
  `currentFileChecksum`, `currentsrcMLRevision` (`:887-893`).
- `startElement` (`:910`): builds a `std::string` for the qualified name, scans the attributes
  (`pos:start`/`pos:end` are parsed into `ctx.startPosition`/`ctx.endPosition` through
  `Position(const std::string&)`, `policy_classes/Position.cpp:19`, two `substr` + two `stoi` per
  element), stores every attribute into the `std::map<std::string,std::string> ctx.attributes`
  (`:981-982`, cleared again at `:991`), looks the name up in `process_map`
  (`std::unordered_map<std::string, std::function<void()>>`, `:986`) and runs the lambda, which bumps
  `ctx.triggerField[state]` and calls `DispatchEvent(state, open)`. A second loop then fires one
  `xmlattribute` event per attribute (`:1009-1022`).
- `charactersUnit` (`:1035`) copies the text into `ctx.currentToken` and fires the `tokenstring` event.
- `endElement` (`:1069`) mirrors `startElement` through `process_map2`.
- `DispatchEvent` (`:91`) always delivers to the **top** of the listener stack
  (`elementListeners.back()->HandleEvent(...)`). `EventListener::HandleEvent`
  (`srcDispatchUtilities.hpp:256`) looks the state up in the listener's own
  `openEventMap`/`closeEventMap` (`unordered_map<ParserState, std::function<...>>`) and runs it.

`ctx` is one `srcSAXEventContext` (`srcDispatchUtilities.hpp:70`) shared by everything: current
tag/token/attribute strings, the trigger counters, the current positions, file path, language,
checksum, namespaces and class name.

## 3. Policies build the per-unit object model

Every syntactic construct has a policy in `srcDispatch/src/policy_classes/`. A policy is an
`EventListener` (reacts to open/close/token events), a `PolicyDispatcher` (owns a `*Data` struct and
notifies parents) and a `PolicyListener` (receives children's data). `DeclPolicy.hpp` is the canonical
example:

1. On `<decl>` open (`startDecl`, `DeclPolicy.hpp:154`) it resets `data = DeclData{}`, records
   `startPosition`/`endPosition` from `ctx`, and installs handlers that **push a child policy** onto the
   dispatcher's listener stack when the matching child element opens
   (`ctx.dispatcher->AddListenerDispatch(typePolicy.get())`, `:229`; name `:241`; init expr `:254`).
   From then on all events go to that child until it pops itself.
2. On its own close (`endDecl`, `:176`) it calls `NotifyAll(ctx)`
   (`srcDispatchUtilities.hpp:382`): every registered parent `PolicyListener::Notify` is called with
   the child policy pointer, and the parent pulls the data with `policy->Data<DeclData>()`
   (`srcDispatchUtilities.hpp:374`) → `DataInner()` → **`std::make_shared<DeclData>(data)`**, a copy
   of the struct (`DeclPolicy.hpp:120`). The child then resets its data. The parent wraps the
   `shared_ptr` in a `DeltaElement<std::shared_ptr<T>>` and stores it (`DeclPolicy::Notify`,
   `:122-142`: type, name, init/range/arguments).
3. `ctx.dispatcher->RemoveListener(nullptr)` (`:144`) pops the finished child off the stack.

The result at `</unit>` is a tree of heap objects rooted in `UnitData` (`UnitPolicy.hpp:31`):

```
UnitData { includes[], declStmts[], typedefs[], classes[], functions[] }
  FunctionData { name: NameData, returnType: TypeData, parameters[]: DeclData, block: BlockData, memberInitList[], namespaces[] … }
    BlockData { statements[]: any(ExprStmtData | DeclStmtData | ReturnData | IfStmtData | ForData | WhileData | SwitchData | TryData | …), blocks[], cases[] }
      DeclStmtData { decls[]: DeclData }      DeclData { type: TypeData, name: NameData, init: ExpressionData, arguments[], range }
      ExprStmtData { expr: ExpressionData }  ExpressionData { expr[]: any(NameData | OperatorData | LiteralData | CallData) }
        NameData { name: string, names[]: any(NameData | OperatorData), templateArgumentList, indices[]: ExpressionData }
        CallData { name: NameData, arguments[]: ExpressionData }
  ClassData { name, fields[]: DeclData, constructors[], destructor, methods[], operators[], innerClasses[] }
```

Every link is a `DeltaElement<T>` (`DeltaElement.hpp:23`): two `std::optional<T>` (original/modified,
for srcDiff) plus a `DiffOperation`. Children of mixed type are held as `DeltaElement<std::any>` and
recovered with `std::any_cast<std::shared_ptr<X>>(elem.GetElement())`. Every `ElementData` carries
`startPosition`/`endPosition` as `DeltaElement<Position>` (`ElementData.hpp:16`).

## 4. Hand-off: one `SrcSliceWorker` per unit, bounded

`UnitPolicy` notifies `SrcSliceHandler`, the single `PolicyListener` registered at construction.
`SrcSliceHandler::Notify`:

- takes the unit (`policy->Data<UnitData>()`),
- records `#include`s per file in `fileDependencyTable` (used later to glue fragments),
- builds a `SrcSliceWorker` outside the lock, then waits on `spaceCv` until the backlog has fewer
  than `backlogCap` units (`--backlog`, default = threads), gives the worker its sequence number
  (archive order) and pushes it; `backlogCv` wakes the manager.
- If the run has been aborted (a worker threw, or the parse failed) it calls `stop_parser()` on
  the active srcSAX handler instead, so no exception crosses libxml2's C frames.

`SrcSliceWorker` owns the `shared_ptr<UnitData>`, a `SliceCtx` snapshot of the file
path/checksum/language/namespaces, an empty `Blob`, its `seq`, an `error` slot and an `onDone`
callback. The parse thread then returns to libxml2 and continues with the next unit; when the
backlog is full it blocks there, which is what bounds the number of parsed unit models in
memory (M5).

## 5. Worker: unit model → `Blob`

`ManageThreads` (`srcslicehandler.cpp:132`) runs on its own thread, started before parsing
(`:47`). It keeps `threadCount` slots and, for each free slot, pops the backlog and calls
`worker->Start()` (`:212-223`), which spawns `std::thread(&SrcSliceWorker::Perform)`
(`srcsliceworker.cpp:45`). `Perform` (`:26`) walks the unit model in four passes, all appending to
the worker-private `Blob` (`srcsliceworker.hpp`, essentially a private copy of the handler's state:
`profileMap`, `functionSigMap`, loop/if position vectors):

| pass | entry | what it produces |
|---|---|---|
| globals | `ProcessDecls` `:233` | a `SliceProfile` per file-scope declaration (`isGlobal`), plus initializer uses/defs via `ParseExpr` |
| signatures | `ProcessSignatures` `:351` → `ProcessFunctionSignature` `:1464` | `FunctionSignatureData` per function/ctor/dtor/method/operator in `functionSigMap[name]`; copies each parameter's name and name position (`SignatureParameter`), nothing of the unit model |
| free functions | `ProcessFunctions` `:388` | parameters → profiles (`ProcessFunctionParameters` `:1405`), ctor init lists (`:527`), then the body (`ProcessStmts` `:559`) |
| classes | `ProcessClasses` `:402` | fields → profiles, then ctors/dtor/methods/operators recursively, nested classes |

`ProcessStmts` dispatches on the `std::any` type of each statement (`:565-647`): declarations become
profiles (`CreateSliceProfile` `:438`), expression/return/throw statements go to `ProcessExprStmt`
(`:656`), control flow goes to `CollectConditionalData` (`:732`), which records if/else/loop position
ranges into the Blob vectors and recurses into their blocks and conditions; `try` is handled by
`CollectTryBlockData` (`:713`). Then it recurses into nested blocks (`:651`).

`ParseExpr` (`:1010`) is the heart of slicing. It walks the flat token list of an `ExpressionData`
left to right with an `ExprCtx` (`srcsliceparse.hpp`) and a transparent `lhsStack`:

- **name token** (`:1025`): resolve the root identifier (`FindName`/`ExtractName`), look it up in
  `data.profileMap`; if it is unknown in this unit, create a **fragment** profile (`isFragment`,
  `:1053-1063`) so uses of externally declared names are not lost; then push a *use*
  (`ExprParse::pushUse`, `srcsliceparse.cpp:111`), hand data dependence/alias information to every
  name on the `lhsStack` (`updateLHS` `:168` → `pushDvar`/`pushAlias`), follow the pointer
  reference chain (`:1070-1105`), handle `++/--`, `cin >>` etc. If the token is a call argument it
  also records a `FunctionCallData` in the profile's `cfunctions` (`CreateSliceCallData` `:666`),
  resolving the callee against `functionSigMap` (`:1172-1255`).
- **operator token** (`:1261`): assignment turns the pending name into a *def* and pushes it on the
  `lhsStack`; `&`, `*`, `++`, `--`, `<<`, `>>` set context flags consumed by the next name.
- **call token** (`:1347`): a use of the callee name, then `ParseExpr` recursively on each argument
  with `isArg = true`.

The profile that receives a use/def is chosen by `findValidProfile` (`srcsliceparse.cpp:61`): the
most recently declared profile of that name whose scope contains the position
(`SliceProfile::inScope`, `srcsliceprofile.cpp:234`), searching the name's vector backwards.

## 6. Manager: start workers, merge in archive order

`ManageThreads` runs on its own thread for the duration of the parse and sleeps on `backlogCv`
between events (no polling). Each wake-up does three things, in order:

1. **Start** workers while the backlog is non-empty, fewer than `threadCount` are running and
   fewer than `maxInFlight` (`--in-flight`, default 2 × threads) units are started-but-unmerged.
   A started worker moves from `backlog` to `inFlight` (a deque in start = archive order) and
   `spaceCv` wakes the parser. `worker->Start()` spawns `std::thread(&SrcSliceWorker::Perform)`;
   `Perform` publishes completion on every exit path (`jobFinished`, then `onDone`, which
   decrements `running` under the lock and notifies the manager) and drops the unit model as
   soon as the Blob exists.
2. **Merge** the front of `inFlight` once it has finished: the manager joins the thread and calls
   `MergeStructures` **with the lock released**. The merge only appends: `profileMap[name]` gets
   the worker's profiles moved in (likewise `functionSigMap` and the eight control-flow vectors).
   Nothing else happens per unit any more, so the merge cost no longer grows with the amount
   already merged, and because units are merged strictly in sequence order the result is the
   same for every thread count (`-t N` output is byte-identical to `-t 1`).
3. **Exit** when the parse is done and both queues are empty. If a worker failed, the manager
   records the first error, sets `abortRun`, lets running workers finish and drops the rest;
   `RunPipeline` then rethrows it (a `std::runtime_error` naming the unit) after joining.

Memory held by the pipeline at any time: at most `backlogCap` unit models waiting,
`threadCount` being sliced, `maxInFlight` Blobs waiting to be merged.

## 7. Finalize: one pass over the complete structure

After `control.parse` returns, `RunPipeline` marks the parse done, joins the manager and the
constructor calls `Finalize()`, a single pass in an order where every step sees finished input:

1. `ComputeControlPaths()` (only with `-c`): for every declared profile, order its own slice
   lines and derive edges from the loop/if position tables. Runs before any propagation, so a
   caller's edges end up being its own plus its callees' (copied in step 3).
2. `mergeFragments` for every name: partitions root profiles (globals, class members) from
   fragments, then for each fragment finds a root in the same file or in a file the fragment's
   file `#include`s (`fileDependencyTable`, matched with `ends_with`), merges its
   uses/defs/calls/… into the root (`SliceProfile::merge`) and erases the fragment. Fragments
   without a root are dropped here and are never resolved.
3. `UpdateCalls` for every declared profile completes the `cfunctions` entries whose function
   was not in the call's unit from the full `functionSigMap` (overloaded names get one entry per
   same-arity signature whose parameter profile exists). Then `ComputeInterprocedural()` calls
   `ResolveCall` on every declared profile: for each call entry it finds the signature by
   position, takes the callee parameter's own profile (`ParameterProfile`: declared in the
   signature's file at the parameter's position), resolves that profile first (recursion,
   cycle-guarded by `visited`) and copies its uses/definitions/aliases/dvars/control edges and
   calls into the profile being resolved. The result is the transitive closure through the
   call graph; in a cycle the first profile to enter it copies its cycle-mates while they are
   still being resolved, so recursive families get a traversal-order-dependent subset (see
   `progress.md`, Step 2b).
4. `ComputeAliasInterprocedural()` (only with `-a`), repeated until no alias list grows.

## 8. Output

`main` constructs the handler (which runs everything above), takes `GetProfileMap()` by
reference and streams every profile with `containsDeclaration` straight to the output file or
stdout through `operator<<(ostream&, SliceProfile&)`; entries are separated by `",\n"` and the
document closed with `"\n}"`, the same bytes the old build-a-string-then-trim code produced.
Fragments and profiles without a declaration are never printed.

## 9. Threads and locks

| thread | created at | does | waits on |
|---|---|---|---|
| main / parse | process start | libxml2 parse → policies → `Notify` (bounded push) → joins manager → `Finalize` → JSON | `spaceCv` in `Notify` when the backlog is full |
| manager (`ManageThreads`) | `RunPipeline` | starts workers, merges Blobs in archive order | `backlogCv`: unit queued, worker finished, parse done or abort |
| worker ×N (`SrcSliceWorker::Perform`) | per unit | builds one Blob, drops the unit model, publishes completion | — (joined by the manager before its Blob is merged) |
| progress bar (`-p`) | `IdleBar`/`ProgressBar` | redraws every 2 ms | — |

`threadCount` defaults to CPU count + 2 (`main.cpp`). Blob construction is the parallel part;
the parse, the merge appends and the single `Finalize` pass are serial. One mutex
(`backlogMutex`) guards the queues and counters; the merge itself runs outside it.
`SlicePosition::InternFileName` / `InternedString::Intern` take a second, process-wide mutex
only on a per-thread cache miss.

## 10. What lives in memory, and for how long

| data | created | freed | notes |
|---|---|---|---|
| libxml2 parser buffers | start | end | streaming; small |
| one unit's object model (`UnitData` tree) | during its `<unit>` | when its worker has built the Blob | at most `backlogCap` waiting + `threadCount` being sliced |
| worker `Blob` | worker start | merged, then freed | at most `maxInFlight`; profiles are moved into the global map |
| global `profileMap` (`SliceProfile` per declaration **and per fragment**) | first merge | process exit | the dominant long-lived structure; fragments are erased in `Finalize` step 2 |
| `functionSigMap` (`FunctionSignatureData`) | first merge | exit | slim parameter copies (`SignatureParameter`); the parse tree is not retained |
| control-flow position vectors | first merge | exit | only read with `-c` |
| interned strings (file names, function names, types, checksums) | first use | exit | one copy per distinct text; `SlicePosition` is 32 bytes |
| JSON | `main` | — | streamed; nothing is buffered beyond the ostream |

## 11. Key types

- `SlicePosition` (`srcslicecollection.hpp`): `std::string filename` + `DeltaElement<Position>` start
  and end + `PositionMeta`. Ordered by start line/column only (`operator<`,
  `srcslicecollection.cpp:131`); equality also compares the filename and end.
- `SliceProfile` (`srcsliceprofile.hpp`): identity (`variableName`, `declPosition`, `file`, `function`,
  class, namespaces, language, checksum, type), flags (pointer/reference/global/member/fragment/
  partial/visited/…), and the slice itself: `definitions`, `uses` (sets of `SlicePosition`),
  `dvars`, `aliases` (sets of name+position), `cfunctions` (set of `FunctionCallData`), `controlEdges`.
  `jsonKey()` = `name-line-col-checksum`.
- `FunctionSignatureData` (`srcslicecollection.hpp`): position, name, return type, `parameters`
  (the srcDispatch `DeclData` pointers), containing class/file/checksum/language/namespaces, `isDefined`.
- `FunctionCallData`: callee name, argument index, argument count, callee definition position,
  call position, `ignore` flag.
- `Blob` (`srcsliceworker.hpp`): the per-unit copy of all handler state that gets merged.
- `SliceCtx` (`srcslicecollection.hpp`): the per-unit file path/checksum/language/namespaces snapshot
  given to every worker function.
