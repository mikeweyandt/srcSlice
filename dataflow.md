# How data flows through srcslice

A walk from the srcML archive on disk to the JSON on stdout, in the order things
happen, with the file and line where each step lives. Line numbers refer to the
`benchmarks` branch as of October 2026 (srcDispatch `09a669a`, srcSAX `93cbbee`).

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
                            </unit>  ──▶  SrcSliceHandler::Notify  ──▶  backlog.push(new SrcSliceWorker(UnitData))
                                                                                   │
                     parse thread ◀── backlogMutex ──▶ ManageThreads thread        │ one std::thread per unit
                                                      │                            ▼
                                                      │               SrcSliceWorker::Perform → Blob (per-unit profileMap …)
                                                      ▼
                           MergeStructures: Blob ⊎ global profileMap/functionSigMap, then ComputeInterprocedural() over everything
                                                      │
                                                      ▼  after the last unit
                               Finalize(): mergeFragments + ModifySlice (UpdateCalls / ResolveCall)
                                                      │
                                                      ▼
                               main(): copy of profileMap → ostringstream JSON → string → file/stdout
```

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

## 4. Hand-off: one `SrcSliceWorker` per unit

`UnitPolicy` notifies `SrcSliceHandler`, the single `PolicyListener` registered at construction
(`srcslicehandler.cpp:51`). `SrcSliceHandler::Notify` (`:114`):

- copies the unit (`policy->Data<UnitData>()`, `:115`),
- records `#include`s per file in `fileDependencyTable` (`:118-120`, used later to glue fragments),
- locks `backlogMutex` and pushes `new SrcSliceWorker(unit, ctx, …)` (`:123-128`).

`SrcSliceWorker` (`srcsliceworker.hpp`) owns the `shared_ptr<UnitData>`, a `SliceCtx` snapshot of
the file path/checksum/language/namespaces (`srcslicecollection.cpp:290`), and an empty `Blob`.
The parse thread then returns to libxml2 and continues with the next unit.

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
| signatures | `ProcessSignatures` `:351` → `ProcessFunctionSignature` `:1464` | `FunctionSignatureData` per function/ctor/dtor/method/operator in `functionSigMap[name]`; keeps the `parameters` vector of `DeclData` pointers |
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

## 6. Merge into the global state

Back in `ManageThreads`, when a worker reports `Finished()` the manager joins its thread and calls
`MergeStructures` (`srcslicehandler.cpp:133`) **while still holding `backlogMutex`** (the
`lock_guard` at `:230` spans the whole loop body) and additionally `dataMutex` (`:136`). It appends
every Blob vector into the handler's vectors (`profileMap[name]` gets the worker's profiles appended,
`:140-147`; same for `functionSigMap` and the eight control-flow vectors, `:150-192`), then runs,
over the **entire** accumulated state:

- `ComputeControlPaths()` (`:386`, only with `-c`) — for every profile, order its slice lines and
  derive edges from the loop/if position tables;
- `ComputeAliasInterprocedural()` (`:531`, only with `-a`);
- `ComputeInterprocedural()` (`:829`) — `ResolveCall` on every profile in `profileMap`. `ResolveCall`
  (`:837`) returns immediately for profiles already `visited`; otherwise, for each `cfunctions`
  entry it finds the signature by position, locates the parameter's profile (`ArgumentProfile`
  `:501`), and copies the parameter's uses/definitions/aliases/dvars/controlEdges into the argument's
  profile (`:940-986`). Unresolvable calls mark the profile `partial`.

The worker (and with it the unit's object model) is deleted after the merge (`:243`).

## 7. Finalize: fragments and partial slices

After `control.parse` returns the parse thread sets `unitsScaned`, joins the manager thread and
calls `Finalize()` (`:570`). For every name group:

- `mergeFragments` (`:583`) partitions root profiles (globals, class members) from fragments, then
  for each fragment finds a root in the same file or in a file the fragment's file `#include`s
  (`fileDependencyTable`, matched with `ends_with`), merges its uses/defs/… into the root
  (`SliceProfile::merge`, `srcsliceprofile.cpp:210`) and erases the fragment.
- `ModifySlice` (`:637`) for profiles still `partial`: `UpdateCalls` (`:656`) retries resolving
  `cfunctions` against the now complete `functionSigMap` (handling overloads), then `ResolveCall`.

## 8. Output

`main` (`src/cpp/main.cpp:79`) constructs the handler (which runs everything above), then
`auto sliceProfileMap = srcSliceHandler.GetProfileMap();` (`:81`, a **copy** of the whole map),
streams every profile with `containsDeclaration` into an `std::ostringstream` via
`operator<<(ostream&, SliceProfile&)` (`srcsliceprofile.cpp:102`), takes `.str()` (`:116`), trims
the trailing comma and writes the string to the file or stdout (`:128-133`). Fragments and
profiles without a declaration are never printed.

## 9. Threads and locks

| thread | created at | does | waits on |
|---|---|---|---|
| main / parse | process start | libxml2 parse → policies → `Notify` (push to backlog) → `Finalize` → JSON | `backlogMutex` in `Notify`; joins manager |
| manager (`ManageThreads`) | `srcslicehandler.cpp:47` | spins over worker slots, starts workers, merges Blobs, interprocedural | nothing: loops without sleeping, taking `backlogMutex` every iteration |
| worker ×N (`SrcSliceWorker::Perform`) | per unit, `srcsliceworker.cpp:48` | builds one Blob | — (joined by manager) |
| progress bar (`-p`) | `IdleBar`/`ProgressBar` | redraws every 2 ms | — |

`threadCount` defaults to CPU count + 2 (`main.cpp:13`). Only the Blob construction is parallel;
parsing, merging and all interprocedural work are serial, and the merge blocks the parser because
both need `backlogMutex`.

## 10. What lives in memory, and for how long

| data | created | freed | notes |
|---|---|---|---|
| libxml2 parser buffers | start | end | streaming; small |
| one unit's object model (`UnitData` tree) | during its `<unit>` | after its Blob is merged | copied once at `</unit>`; survives in the backlog if workers/merge lag behind the parser |
| worker `Blob` | worker start | merged, then freed | profiles are moved, not copied, into the global map |
| global `profileMap` (`SliceProfile` per declaration **and per fragment**) | first merge | process exit | the dominant long-lived structure; each profile holds 8 strings and 6 `std::set`s of `SlicePosition` |
| `functionSigMap` (`FunctionSignatureData`) | first merge | exit | keeps `shared_ptr<DeclData>` parameters, so parts of every unit's parse tree stay alive |
| control-flow position vectors | first merge | exit | only read with `-c` |
| JSON | `main` | exit | map copy + ostringstream + `.str()` copy exist at the same time |

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
