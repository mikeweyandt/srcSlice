// SPDX-License-Identifier: GPL-3.0-only
/**
 * @file srcslicehandler.cpp
 *
 * @copyright Copyright (C) 2018-2024 srcML, LLC. (www.srcML.org)
 *
 * This file is part of the srcSlice application.
 */

#include <srcslicehandler.hpp>

// Create custom buffers with callbacks
CallbackBuf cbOut(coutHandler);
CallbackBuf cbErr(cerrHandler);

// Number of CPU cores
int CPUCount() {
#ifdef _WIN64
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    return (int) sysinfo.dwNumberOfProcessors;
#elif __APPLE__
    int count = 0;
    size_t len = sizeof(count);
    sysctlbyname("machdep.cpu.core_count", &count, &len, NULL, 0);
    return count;
#else
    cpu_set_t cs;
    CPU_ZERO(&cs);
    sched_getaffinity(0, sizeof(cs), &cs);
    return CPU_COUNT(&cs);
#endif
}

void SrcSliceHandler::ApplyThreadSettings(int threads, int backlog, int inFlight) {
    // if an invalid number is passed default to 1 thread
    threadCount = (threads > 0) ? threads : 1;
    const std::size_t workers = static_cast<std::size_t>(threadCount);

    // parsed units waiting for a worker: one per worker by default, never fewer than one
    backlogCap = (backlog > 0) ? static_cast<std::size_t>(backlog) : workers;
    // started-but-unmerged units (the reorder buffer): room for every worker plus as many
    // finished units waiting on an earlier one, never fewer than the workers
    maxInFlight = (inFlight > 0) ? static_cast<std::size_t>(inFlight) : 2 * workers;
    if (maxInFlight < workers) maxInFlight = workers;
}

SrcSliceHandler::SrcSliceHandler(const CliInfo& info) {
    verboseMode = info.verboseMode;
    progressMode = info.progressMode;
    calculateControlEdges = info.calculateControlEdges;
    expandCalls = info.expandCalls;
    expandAliases = info.expandAliases;
    ApplyThreadSettings(info.threadCount, info.backlogCap, info.maxInFlight);

    auto parse = [this, &info]() {
        // argument must be a c string (const char*) or else a saxError is thrown
        srcSAXController control(info.inputFile.c_str());
        Parse(control);
    };

    if (progressMode) {
        // Save original buffers and Redirect cout and cerr
        IdleBar idlebar;
        std::cout << "[Slicing Started]" << "\n";

        RunPipeline(parse);

        // Restore original buffers
        idlebar.Finish("[srcSAXController Parse]");
        idlebar.Reset();

        Finalize();

        // Restore original buffers
        idlebar.Finish("[Second-Pass]");
        std::cout << "[Slicing Finished]" << "\n";
    } else {
        RunPipeline(parse);
        Finalize();
    }
}

// Use string srcml buffer ctor of srcSAXController
SrcSliceHandler::SrcSliceHandler(std::string& sourceCodeStr, const TestArg& info): verboseMode(false) {
    calculateControlEdges = info.calculateControlEdges;
    expandAliases = info.expandAliases;

    // most test-suite inputs are single units; threading tests pass their own count
    ApplyThreadSettings(info.threadCount, info.backlogCap, info.maxInFlight);

    RunPipeline([this, &sourceCodeStr]() {
        srcSAXController control(sourceCodeStr);
        Parse(control);
    });

    Finalize();
}

SrcSliceHandler::~SrcSliceHandler() {
    // backlog and inFlight own their workers; a worker's destructor joins its thread
}

void SrcSliceHandler::Parse(srcSAXController& control) {
    srcDispatch::srcDispatcher<srcDispatch::UnitPolicy> handler(this);

    // Notify stops this parser when the run is aborted; it is only valid during parse()
    struct Active {
        SrcSliceHandler* self;
        ~Active() { self->activeParser = nullptr; }
    } active{this};
    activeParser = &handler;

    control.parse(&handler); // Start parsing
}

void SrcSliceHandler::RunPipeline(const std::function<void()>& parse) {
    std::thread manager(&SrcSliceHandler::ManageThreads, this);

    // The manager must learn that the parse is over and be joined on every path, or its
    // thread outlives this object. Run() is explicit so firstError can be read after the join.
    struct FinishParse {
        SrcSliceHandler* self;
        std::thread& manager;
        bool done = false;

        void Run(bool parseFailed) {
            if (done) return;
            done = true;
            {
                std::lock_guard<std::mutex> lock(self->backlogMutex);
                self->parseDone = true;
                // a failed parse leaves nothing worth finalizing: drop the queued units
                if (parseFailed) self->abortRun = true;
            }
            self->backlogCv.notify_all();
            self->spaceCv.notify_all();
            if (manager.joinable()) manager.join();
        }
        ~FinishParse() { Run(true); }
    } finish{this, manager};

    std::exception_ptr parseError;
    try {
        parse();
    } catch (const SAXError& e) {
        // srcSAX's error type is not a std::exception; main() and the tests expect one
        parseError = std::make_exception_ptr(std::runtime_error("srcSAX: " + e.message));
    } catch (const std::exception&) {
        parseError = std::current_exception();
    } catch (const std::string& message) {
        // srcSAXController's constructors throw a std::string when no context can be created
        parseError = std::make_exception_ptr(std::runtime_error("srcSAX: " + message));
    } catch (const char* message) {
        parseError = std::make_exception_ptr(std::runtime_error(std::string("srcSAX: ") + message));
    } catch (...) {
        parseError = std::make_exception_ptr(std::runtime_error("parsing failed with an unknown exception"));
    }

    finish.Run(parseError != nullptr);

    // a worker failure stops the parser (XML_ERR_USER_STOP), so it is the cause when both are set
    if (firstError) std::rethrow_exception(firstError);
    if (parseError) std::rethrow_exception(parseError);
}

void SrcSliceHandler::FailRun(std::exception_ptr error, const std::string& where) {
    std::string what = "unknown error";
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& e) {
        what = e.what();
    } catch (...) {}

    auto wrapped = std::make_exception_ptr(std::runtime_error(where + " failed: " + what));
    {
        std::lock_guard<std::mutex> lock(backlogMutex);
        if (!firstError) firstError = wrapped;
        abortRun = true;
    }
    // wake the manager (if this is the parser thread) and the parser (if it waits for room)
    backlogCv.notify_all();
    spaceCv.notify_all();
}

void SrcSliceHandler::Notify(const srcDispatch::PolicyDispatcher *policy, const srcDispatch::srcSAXEventContext &ctx) {
    std::shared_ptr<srcDispatch::UnitData> unit = policy->Data<srcDispatch::UnitData>();
    if (!unit) return;

    // track what files are including to be used for fragment gluing
    for (const auto& includeData : unit->includes) {
        fileDependencyTable[ctx.currentFilePath].push_back(includeData->path.ToString());
    }

    // build the worker outside the lock; the manager only needs the hand-over
    auto worker = std::make_unique<SrcSliceWorker>(std::move(unit), ctx, verboseMode, calculateControlEdges);
    worker->onDone = [this]() {
        {
            std::lock_guard<std::mutex> lock(backlogMutex);
            --running;
        }
        backlogCv.notify_one();
    };

    std::unique_lock<std::mutex> lock(backlogMutex);

    // bounded backlog (M5): block the parser until the manager has taken a unit
    spaceCv.wait(lock, [this]() { return backlog.size() < backlogCap || abortRun; });

    if (abortRun) {
        // never throw through libxml2's C frames: stop the parser, RunPipeline reports firstError
        lock.unlock();
        if (activeParser) activeParser->stop_parser();
        return;
    }

    worker->seq = nextSeq++;
    ++stats.units;
    backlog.push_back(std::move(worker));
    stats.maxBacklog = std::max(stats.maxBacklog, backlog.size());

    lock.unlock();
    backlogCv.notify_one();
}

void SrcSliceHandler::MergeStructures(SrcSliceWorker& worker) {
    Blob& data = worker.data;

    // manually move map entry values, merge will not merge if the keys between both maps exist
    for (auto& profileGroup : data.profileMap) {
        auto& dest = profileMap[profileGroup.first];
        dest.insert(
            dest.end(),
            std::make_move_iterator(profileGroup.second.begin()),
            std::make_move_iterator(profileGroup.second.end())
        );
    }

    // manually move map entry values, merge will not merge if the keys between both maps exist
    for (auto& funcSig : data.functionSigMap) {
        auto& dest = functionSigMap[funcSig.first];
        dest.insert(
            dest.end(),
            std::make_move_iterator(funcSig.second.begin()),
            std::make_move_iterator(funcSig.second.end())
        );
    }

    // make_move_iterator prevents making copies
    loopdata.insert(loopdata.end(),
        std::make_move_iterator(data.loopdata.begin()),
        std::make_move_iterator(data.loopdata.end())
    );
    forloopdata.insert(forloopdata.end(),
        std::make_move_iterator(data.forloopdata.begin()),
        std::make_move_iterator(data.forloopdata.end())
    );
    whileloopdata.insert(whileloopdata.end(),
        std::make_move_iterator(data.whileloopdata.begin()),
        std::make_move_iterator(data.whileloopdata.end())
    );
    dowhileloopdata.insert(dowhileloopdata.end(),
        std::make_move_iterator(data.dowhileloopdata.begin()),
        std::make_move_iterator(data.dowhileloopdata.end())
    );

    ifdata.insert(ifdata.end(),
        std::make_move_iterator(data.ifdata.begin()),
        std::make_move_iterator(data.ifdata.end())
    );
    ifStmts.insert(ifStmts.end(),
        std::make_move_iterator(data.ifStmts.begin()),
        std::make_move_iterator(data.ifStmts.end())
    );
    elsedata.insert(elsedata.end(),
        std::make_move_iterator(data.elsedata.begin()),
        std::make_move_iterator(data.elsedata.end())
    );
    elseifdata.insert(elseifdata.end(),
        std::make_move_iterator(data.elseifdata.begin()),
        std::make_move_iterator(data.elseifdata.end())
    );

    // nothing else: control edges, fragment gluing, call resolution and alias expansion run
    // once over the complete structure in Finalize (perf plan Step 2b)
}

/*
    Manager thread. Units arrive from the parser in archive order (seq); up to threadCount of
    them are sliced concurrently and the results are merged strictly in seq order, so the main
    structure is built exactly as a single-threaded run builds it. Memory is bounded by
    backlogCap unit models waiting, threadCount models being sliced and maxInFlight Blobs.
    The thread sleeps on backlogCv between events instead of polling the queue.
*/
void SrcSliceHandler::ManageThreads() {
    const std::size_t workers = static_cast<std::size_t>(threadCount);
    std::unique_lock<std::mutex> lock(backlogMutex);

    while (!abortRun) {
        try {
            // 1. start workers while there is a unit, a free thread and room in the window
            while (!abortRun && !backlog.empty() && running < workers && inFlight.size() < maxInFlight) {
                inFlight.push_back(std::move(backlog.front()));
                backlog.pop_front();
                SrcSliceWorker* worker = inFlight.back().get();
                ++running;
                stats.maxRunning = std::max(stats.maxRunning, running);
                stats.maxInFlight = std::max(stats.maxInFlight, inFlight.size());

                // only this thread removes from inFlight, so the pointer stays valid unlocked
                lock.unlock();
                spaceCv.notify_one(); // the parser may be waiting for backlog room
                try {
                    worker->Start();
                } catch (...) {
                    // thread creation failed: account for it like a finished worker
                    worker->error = std::current_exception();
                    worker->jobFinished.store(true, std::memory_order_release);
                    lock.lock();
                    --running;
                    continue;
                }
                lock.lock();
            }

            // 2. merge the oldest started unit once it is done (seq order == parse order)
            if (!abortRun && !inFlight.empty() && inFlight.front()->Finished()) {
                std::unique_ptr<SrcSliceWorker> worker = std::move(inFlight.front());
                inFlight.pop_front();

                lock.unlock();
                worker->WaitForJob(); // completion is already published; this reaps the thread
                if (worker->error) {
                    FailRun(worker->error, "slicing unit #" + std::to_string(worker->seq + 1)
                                           + " (" + worker->sctx.currentFilePath + ")");
                } else {
                    MergeStructures(*worker);
                }
                lock.lock();
                ++stats.merged;
                continue;
            }

            // 3. nothing left and nothing more to come
            if (parseDone && backlog.empty() && inFlight.empty()) return;

            // 4. sleep until one of the three conditions above changes
            backlogCv.wait(lock, [&]() {
                return abortRun
                    || (!backlog.empty() && running < workers && inFlight.size() < maxInFlight)
                    || (!inFlight.empty() && inFlight.front()->Finished())
                    || (parseDone && backlog.empty() && inFlight.empty());
            });
        } catch (...) {
            // merging failed (e.g. std::bad_alloc): fail the run like a worker exception
            if (lock.owns_lock()) lock.unlock();
            FailRun(std::current_exception(), "merging unit results");
            lock.lock();
        }
    }

    // aborted: let the running workers publish, then drop every queued and unmerged unit
    backlogCv.wait(lock, [this]() { return running == 0; });
    inFlight.clear(); // joins the (already finished) threads
    backlog.clear();
}

std::unordered_map<std::string, std::vector<SliceProfile>>& SrcSliceHandler::GetProfileMap() {
    return profileMap;
}

// Find forward control-edges between conditional conditions. ie: if -> else if -> else
void SrcSliceHandler::ComputeOuterPaths(EdgeSet& controlEdges, std::vector<SlicePosition>& sLines) {
    /** @todo
     * Checking metadata within each SlicePosition, connect sline[i] to a sline[k] where k > i
     * corresponding if conditions to preceding elif conditions or the first sline within an else
    */

    for (size_t i = 0; i < sLines.size() - 1; ++i) {
        // sline[i] must be within the condition block of an if or else-if
        if (!sLines[i].GetData().isIfCondition && !sLines[i].GetData().isElifCondition)
            continue;

        // find the condition context base
        SlicePosition baseBlock;
        for (SlicePosition& data : ifdata) {
            if (sLines[i] < data) continue;
            if (IsContained(sLines[i], data)) {
                baseBlock = data;
            }
        }
        for (SlicePosition& data : elseifdata) {
            if (sLines[i] < data) continue;
            if (IsContained(sLines[i], data)) {
                baseBlock = data;
            }
        }

        if (baseBlock.GetFileName().empty()) continue;

        // find the preceding sline[k]
        size_t k = i+1;
        while (k < sLines.size()) {
            if (!IsContained(sLines[k], baseBlock)) break;
            ++k;
        }
        if (k == sLines.size()) continue;
        
        controlEdges.insert(std::make_pair(sLines[i], sLines[k]));
    }
}

// Collects the forward exiting control-flows of conditionals. ie: exiting conditional body
void SrcSliceHandler::ComputeExitPaths(EdgeSet& controlEdges, std::vector<SlicePosition>& sLines) {
    /** @todo
     * Find the context if_stmt the sline[i] is within to find the first sline[k] not contained in the context if_stmt where k > i
     * sline[i] must be the sline that occurs LAST within the inner context of the if_stmt context (specific if, elif, else block)
    */

    for (size_t i = 0; i < sLines.size() - 1; ++i) {
        // skip slines that are within the conditions
        if (sLines[i].GetData().isIfCondition || sLines[i].GetData().isElifCondition)
            continue;

        // sline[i] must be contained within the block of either an if, else-if, or else statement
        size_t ctxIndex = FindContextBlock(sLines[i], ifStmts);
        if (ctxIndex == -1) continue;

        // find inner-most context of sline[i]
        SlicePosition innerContext;
        for (SlicePosition& data : ifdata) {
            if (IsContained(sLines[i], data)) {
                innerContext = data;
            }
        }
        for (SlicePosition& data : elseifdata) {
            // if the inner context has been found skip this loop
            if (!innerContext.StartToString().empty()) break;

            if (IsContained(sLines[i], data)) {
                innerContext = data;
            }
        }
        for (SlicePosition& data : elsedata) {
            // if the inner context has been found skip this loop
            if (!innerContext.StartToString().empty()) break;

            if (IsContained(sLines[i], data)) {
                innerContext = data;
            }
        }

        // check if sline[i] is the last sline within the inner-most context
        bool isExitPath = false;
        size_t k = i+1;
        while (k < sLines.size()) {
            // if sline[k] is contained within the inner-most context
            // then sline[i], sline[k] is not an exit-path
            if (IsContained(sLines[k], innerContext)) {
                break;
            }

            // check if sline[k] is outside of the if_stmt context
            if (!IsContained(sLines[k], ifStmts[ctxIndex])) {
                isExitPath = true;
                break;
            }

            ++k;
        }
        if (!isExitPath) continue;

        
        controlEdges.insert(std::make_pair(sLines[i], sLines[k]));
    }
}

// srcSlice focuses on Forward-Slicing, therefor our Control-Flows are going to be forward-flowing
// we are not focusing on backwards-flows.
void SrcSliceHandler::ComputeControlPaths() {
    auto InBlock = [this](SlicePosition& sline, std::vector<SlicePosition>& groups) -> bool {
        // groups are focused on if, elif, else position ranges
        size_t ctxIndex = -1;
        if (groups.size() > 1) {
            ctxIndex = FindContextBlock(sline, ifStmts);
            if (ctxIndex == -1) return false;
        }

        for (auto& data : groups) {
            // if there is nesting we only want to be concerned with data related to the if_stmt context
            if (ctxIndex != -1 && IsContained(ifStmts[ctxIndex], data)) {
                continue;
            }

            if (IsContained(sline, data)) {
                return true;
            }
        }
        return false;
    };
    
    auto InLoop = [](SlicePosition& sline, std::vector<SlicePosition>& loopdata) -> bool {
        size_t ctxIndex = -1;
        if (loopdata.size() > 1) {
            ctxIndex = FindContextBlock(sline, loopdata);
            if (ctxIndex == -1) return false;
        }

        for (auto& data : loopdata) {
            if (ctxIndex != -1 && IsContained(loopdata[ctxIndex], data)) {
                continue;
            }

            if (IsContained(sline, data)) {
                return true;
            }
        }
        return false;
    };

    for (auto& varGroup : profileMap) {
        for (auto& profile : varGroup.second) {
            if (!profile.containsDeclaration) continue;

            // Collect the slice lines and put them in numerical order
            PositionSet sLinesOrdered;
            sLinesOrdered.insert(profile.definitions.begin(), profile.definitions.end());
            sLinesOrdered.insert(profile.uses.begin(), profile.uses.end());

            // Convert set into vector using vector ctor
            std::vector<SlicePosition> sLines(sLinesOrdered.begin(), sLinesOrdered.end());

            // Handles controledges based from: for-loops, while-loops, do-while-loops
            for (auto& loop : loopdata) {
                SlicePosition predecessor;    // origin position
                SlicePosition falseSuccessor; // where to go when loop-condition is false
                SlicePosition trueSuccessor;  // where to go when loop-condition is true (first sline within loop block)

                for (auto& sline : sLines) {
                    // see if the sline is before the loop or in the loop-control statement
                    if (predecessor.GetFileName().empty() && IsContained(sline, loop)) {
                        predecessor = sline;
                        continue;
                    }

                    // find the first sline outside the loop-block
                    if (falseSuccessor.GetFileName().empty() && sline > loop && !IsContained(sline, loop)) {
                        falseSuccessor = sline;
                        continue;
                    }

                    // find the first sline inside the loop-block
                    if (trueSuccessor.GetFileName().empty() && IsContained(sline, loop)) {
                        trueSuccessor = sline;
                        continue;
                    }
                }

                bool validPositions = !predecessor.GetFileName().empty() && !trueSuccessor.GetFileName().empty() && !falseSuccessor.GetFileName().empty();
                if (validPositions && predecessor < falseSuccessor) {
                    profile.controlEdges.insert(
                        std::make_pair(predecessor, trueSuccessor)
                    );
                    profile.controlEdges.insert(
                        std::make_pair(predecessor, falseSuccessor)
                    );
                }
            }

            // Create sline pairs between successive lines with case of if-else matching
            for (size_t i = 0; i < sLines.size() - 1; ++i) {
                if ( !(InBlock(sLines[i], ifdata) && (InBlock(sLines[i+1], elseifdata) || InBlock(sLines[i+1], elsedata)) ) ) {
                    profile.controlEdges.insert(
                        std::make_pair(sLines[i], sLines[i+1])
                    );
                }

                // if the sline[i] does not belong to any loop or condition block
                // then connect this sline to the first successive sline[k] that
                // does not also belong to any loop or condiiton block
                if (!InLoop(sLines[i], loopdata) && !InBlock(sLines[i], ifdata) && !InBlock(sLines[i], elseifdata) && !InBlock(sLines[i], elsedata)) {
                    profile.controlEdges.insert(
                        std::make_pair(sLines[i], sLines[i+1])
                    );
                }
            }

            // Find Exit-Paths and Outer-Paths that are not initially found from this algorithm
            ComputeExitPaths(profile.controlEdges, sLines);
            ComputeOuterPaths(profile.controlEdges, sLines);
        }
    }
}

SliceProfile* SrcSliceHandler::ParameterProfile(const FunctionSignatureData& funcSig, unsigned int paramIndex,
                                                 const std::string& simpleFunctionName) {
    if (paramIndex >= funcSig.parameters.size()) return nullptr;

    const SignatureParameter& param = funcSig.parameters[paramIndex];
    if (param.name.empty()) return nullptr;

    const std::string& paramName = param.name;

    // the parameter is declared in the signature's file at the parameter name's position
    const SlicePosition& paramDeclPos = param.namePosition;

    if (!declaredByPosition.empty()) {
        // Finalize's index: only the profiles declared at exactly this position, in the order
        // the parameter name's vector holds them, so the first match is what the scan found.
        // A position without both ends equals nothing (SlicePosition::operator==).
        if (paramDeclPos.HasStart() && paramDeclPos.HasEnd()) {
            auto hit = declaredByPosition.find(DeclaredKey(paramDeclPos));
            if (hit != declaredByPosition.end()) {
                for (SliceProfile* candidate : hit->second) {
                    if (candidate->variableName != paramName) continue;
                    if (SrcSliceOperations::GetSimpleFunctionName(candidate->function) != simpleFunctionName) continue;
                    return candidate;
                }
            }
        }
        if (verboseMode && profileMap.find(paramName) == profileMap.end()) {
            std::cout << "[-] " << __FUNCTION__ << ":" << __LINE__ << " | Could not find SliceProfile for parameter '"
                      << paramName << "' of function '" << funcSig.name << "'" << "\n";
        }
        return nullptr;
    }

    // outside Finalize (no index): the scan
    auto spi = profileMap.find(paramName);
    if (spi == profileMap.end()) {
        if (verboseMode) {
            std::cout << "[-] " << __FUNCTION__ << ":" << __LINE__ << " | Could not find SliceProfile for parameter '"
                      << paramName << "' of function '" << funcSig.name << "'" << "\n";
        }
        return nullptr;
    }

    // cheap position test first: names like `i` have thousands of profiles and the function
    // name extraction is a string scan
    for (auto& candidate : spi->second) {
        if (!candidate.containsDeclaration) continue;
        if (candidate.declPosition != paramDeclPos) continue;
        if (candidate.variableName != paramName) continue;
        if (SrcSliceOperations::GetSimpleFunctionName(candidate.function) != simpleFunctionName) continue;
        return &candidate;
    }
    return nullptr;
}

void SrcSliceHandler::BuildDeclaredIndex() {
    declaredByPosition.clear();
    for (auto& [name, profiles] : profileMap) {
        for (auto& profile : profiles) {
            if (!profile.containsDeclaration) continue;
            if (!profile.declPosition.HasStart() || !profile.declPosition.HasEnd()) continue;
            declaredByPosition[DeclaredKey(profile.declPosition)].push_back(&profile);
        }
    }
}

// Similiar to ComputeInterprocedural but focuses on building
// a more detailed alias listing for slices of pointers
bool SrcSliceHandler::ComputeAliasInterprocedural() {
    bool changed = false;
    std::unordered_set <std::string> visited_alias;

    for (auto& sliceGroup : profileMap) {
        // expand list of potential targets (aliases)
        for (auto& sp : sliceGroup.second) {
            if (!sp.containsDeclaration) continue;
            for (auto& alias : sp.aliases) {
                // check if the alias has been visited
                if (visited_alias.find(alias.first) != visited_alias.end())
                    break;

                // view aliases of the slice profile
                auto aliasProfileGroup = profileMap.find(alias.first);
                if (aliasProfileGroup == profileMap.end()) continue;

                // fingerprint the profile based on contained use
                for (auto& aspi : aliasProfileGroup->second) {
                    if (!aspi.containsDeclaration) continue;
                    
                    auto usesItr = aspi.uses.find(alias.second);
                    if (usesItr == aspi.uses.end()) continue;
                    
                    // determine if the potential target is a pointer or reference
                    if (!aspi.isPointer && !aspi.isReference) continue;

                    // mark alias as visited so we dont review this alias entry again (circular dependence protection)
                    visited_alias.insert(aspi.jsonKey());

                    // push_back alias slice profile's aliases into the source slice aliases
                    const std::size_t before = sp.aliases.size();
                    sp.aliases.insert(aspi.aliases.begin(), aspi.aliases.end());
                    if (sp.aliases.size() != before) changed = true;
                    break;
                }
            }
        }
    }
    return changed;
}

/*
    One pass over the complete structure, in an order where every step sees finished input:
      1. control edges over each declared profile's own positions (before anything is
         propagated into it, so a caller ends up with its own edges plus its callees');
      2. fragments (uses of a variable in units that do not declare it) are glued into their
         root, which then owns their calls; fragments without a root are dropped;
      3. every declared profile gets its unresolved calls completed from the full signature
         table (UpdateCalls), then all of them are resolved (ResolveCall pre-resolves the
         callee parameters recursively, so the map order does not matter);
      4. alias expansion is repeated until nothing changes (the per-merge passes used to give
         one more hop per unit).
*/
void SrcSliceHandler::Finalize() {
    if (verboseMode || progressMode) {
        std::size_t profiles = 0, fragments = 0, positions = 0, calls = 0;
        for (auto& [name, group] : profileMap) {
            for (auto& p : group) {
                ++profiles;
                fragments += p.isFragment;
                positions += p.uses.size() + p.definitions.size();
                calls += p.cfunctions.size();
            }
        }
        std::cout << "[*] Performing Second-Pass on " << profiles << " profiles (" << fragments << " fragments), "
                  << positions << " positions, " << calls << " call entries" << "\n";
    }

    if (calculateControlEdges) ComputeControlPaths();

    for (auto& [name, profiles] : profileMap) {
        mergeFragments(profiles);
    }

    // nothing moves a profile from here on: index them by declaration position for
    // ParameterProfile (UpdateCalls and ResolveCall call it per call entry)
    BuildDeclaredIndex();

    for (auto& [name, profiles] : profileMap) {
        for (auto& profile : profiles) {
            if (profile.containsDeclaration) UpdateCalls(profile);
        }
    }
    ComputeInterprocedural();
    declaredByPosition.clear();

    if (expandAliases) {
        // each pass can add targets that the next pass follows; 32 hops is far beyond any
        // real alias chain and bounds a pathological cycle
        for (int pass = 0; pass < 32 && ComputeAliasInterprocedural(); ++pass) {}
    }

    if (verboseMode || progressMode) std::cout << "[*] Finished Second-Pass" << "\n";
}

void SrcSliceHandler::mergeFragments(std::vector<SliceProfile>& profiles) {
    typedef std::vector<SliceProfile>::iterator Spi;
    std::unordered_map<std::string, std::vector<Spi>> possibleRootFragments;

    // move fragments to the back so roots are at the front
    std::partition(profiles.begin(), profiles.end(), [](const SliceProfile& p) {
        // root fragments remain at the front of the vector
        // while fragement profiles are moved to the back
        // to ensure upon iteration we mark all root fragments
        // before handling a fragment
        return !p.isFragment;
    });
    
    for (auto itr = profiles.begin(); itr != profiles.end();) {
        if (itr->isFragment) {
            // find the root fragment and merge data
            // then remove fragment and continue
            auto dFiles = fileDependencyTable.find(itr->file);

            auto mergeFragment = [&possibleRootFragments](Spi itr, std::vector<std::string> includedFiles) {
                for (auto& [baseFile, rootFragments] : possibleRootFragments) {
                    for (auto& rootFragment : rootFragments) {
                        // root fragment and partial fragment exist in the same file
                        if (rootFragment->file == itr->file) {
                            rootFragment->merge(*itr);
                            return;
                        }

                        // inclusions in the file the fragment originates from
                        for (auto& includeName : includedFiles) {
                            // search from end of rootFragment file string
                            // if includeName substring occurs (C++20 string)
                            if (rootFragment->file.ends_with(includeName)) {
                                // merge fragment (itr) with rootFragment
                                rootFragment->merge(*itr);
                                return;
                            }
                        }
                    }
                }
            };
            mergeFragment(itr, (dFiles != fileDependencyTable.end()) ? dFiles->second : std::vector<std::string>{});
            itr = profiles.erase(itr);
        } else {
            // check if profile is potential root
            if (itr->isGlobal || itr->classMemberVar) {
                possibleRootFragments[itr->file].push_back(itr);
            }
            ++itr;
        }
    }
}

// Complete the cfunctions entries whose definition position is unknown (the call's unit did
// not contain the function) from the full signature table; overloaded names get one entry per
// signature whose parameter profile exists.
void SrcSliceHandler::UpdateCalls(SliceProfile& sp) {
    if (sp.cfunctions.empty()) return;

    // collection of call data that is to be later added to the given slice profile
    std::vector<FunctionCallData> toInsert, toErase;
    
    for (auto itr = sp.cfunctions.begin(); itr != sp.cfunctions.end(); ++itr) {
        const FunctionCallData& cfunc = *itr;

        // only update cfunc entries that do not have a definition position
        if (!cfunc.funcPos.GetFileName().empty()) {
            continue;
        }

        auto funcSig = functionSigMap.find(cfunc.functionName.str());

        if (funcSig == functionSigMap.end()) continue;

        // if there is only one record of a function signature
        if (funcSig->second.size() == 1) {
            if (funcSig->second[0].parameters.empty()) continue;
            if (cfunc.argumentCount > funcSig->second[0].parameters.size()) continue;

            // update funcPos
            FunctionCallData updatedData(cfunc);
            updatedData.funcPos = funcSig->second[0].position;

            // replace old data with new data
            toErase.push_back(*itr);
            toInsert.push_back(updatedData);

        } else {
            // if a function is overloaded

            unsigned int ArgProfParam = cfunc.parameterIndex - 1;
            std::vector<const FunctionSignatureData*> signatures;

            for (auto& funcData : funcSig->second) {
                if (cfunc.argumentCount == funcData.parameters.size()) {
                    signatures.push_back(&funcData);
                }
            }

            if (signatures.empty()) {
                for (auto& funcData : funcSig->second) {
                    if (cfunc.argumentCount == 0 || cfunc.argumentCount >= funcData.parameters.size()) continue;
                    // 1 <= argCount < paramCount
                    signatures.push_back(&funcData);
                }
            }

            // update the current cfunc and add new entries per possible signature
            for (size_t i = 0; i < signatures.size(); ++i) {

                // only a signature whose parameter some unit declared can resolve the argument
                if (!ParameterProfile(*signatures[i], ArgProfParam, cfunc.functionName)) continue;

                if (cfunc.funcPos.GetFileName().empty()) {
                    // update cfunc data
                    FunctionCallData updatedData(cfunc);
                    updatedData.funcPos = signatures[i]->position;

                    toErase.push_back(*itr);
                    toInsert.push_back(updatedData);
                } else {
                    // append new cfunc data
                    FunctionCallData newData(cfunc);
                    newData.funcPos = signatures[i]->position;
                    toInsert.push_back(newData);
                }
            }
        }
    }

    // remove outdated entries
    for (auto& data : toErase) {
        sp.cfunctions.erase(data);
    }

    // insert the new calls into the profile after iteration
    // to not risk iterator invalidation
    for (auto& data : toInsert) {
        sp.cfunctions.insert(data);
    }
}

// Resolve every declared profile once; fragments were glued into their roots (or dropped)
// before this runs, so they are not resolved on their own any more.
void SrcSliceHandler::ComputeInterprocedural() {
    for (auto& [name, profiles] : profileMap) {
        for (auto& sp : profiles) {
            if (sp.containsDeclaration) ResolveCall(sp);
        }
    }
}

void SrcSliceHandler::ResolveCall(SliceProfile &sp) {
    // safety to ensure we avoid circular profile linkage
    if (sp.visited) return;
    // ignore profiles with no value
    if ((sp.variableName == "*LITERAL*")) return;
    
    // label the profile as visited to avoid circular profile linkage
    sp.visited = true;

    if (!sp.cfunctions.empty()) {
        // calls made by the callee parameters this profile flows into; appended to sp after
        // the loop so the iteration over sp.cfunctions is not invalidated
        std::set<FunctionCallData> complementaryCalls;

        for (auto& cfunc : sp.cfunctions) {
            // if a cfunc ignore flag is enabled skip this index and continue
            if (cfunc.ignore)
                continue;

            auto funcSigCollection = functionSigMap.find(cfunc.functionName);

            if (funcSigCollection != functionSigMap.end()) {
                size_t sigIndex = 0;
                
                // Attempt to fingerprint the right signature based on
                // function call definition line and called function
                // def line data
                for (sigIndex = 0; sigIndex < funcSigCollection->second.size(); ++sigIndex) {
                    if (cfunc.funcPos == funcSigCollection->second[sigIndex].position) {
                        break;
                    }
                }

                if (sigIndex >= funcSigCollection->second.size()) {
                    continue; // no signature could be found
                }

                if (funcSigCollection->second[sigIndex].name.empty()) {
                    continue; // the name is not usable in computation
                }

                std::string simpleFunctionName = SrcSliceOperations::GetSimpleFunctionName(funcSigCollection->second[sigIndex].name);

                unsigned int ArgProfParam = cfunc.parameterIndex - 1;

                // Ensure before resolving the parameter that the signature has parameters and the index is valid

                /*** @todo look for potential issues with overload case */

                bool matchingName = cfunc.functionName == simpleFunctionName;
                bool containsParameters = !funcSigCollection->second[sigIndex].parameters.empty();
                bool validSigIndex = sigIndex < funcSigCollection->second.size();
                bool validArgIndex = ArgProfParam < funcSigCollection->second[sigIndex].parameters.size();

                if (matchingName && containsParameters && validSigIndex && validArgIndex) {
                    // the callee parameter's own profile (declared in the signature's file at the
                    // parameter's position), not the last profile that shares its name
                    SliceProfile* param = ParameterProfile(funcSigCollection->second[sigIndex], ArgProfParam, cfunc.functionName);

                    if (param) {
                        // resolve the parameter first so that data two or more hops away has
                        // reached it before it is copied; the visited guard stops cycles
                        ResolveCall(*param);

                        // the resolved data belongs to the profile being resolved, not to
                        // the last profile that happens to share its name
                        SliceProfile& profile = sp;

                        if (!param->isReference && !param->isPointer) {
                            // pass by value
                            profile.uses.insert(param->definitions.begin(), param->definitions.end());
                        } else {
                            // pass by reference
                            profile.definitions.insert(param->definitions.begin(), param->definitions.end());
                        }

                        // ensure we do not remove the initial decl from a slices definitions set
                        if (profile.declPosition != param->declPosition) {
                            // Parameter initial declaration def line is considered a use towards the argument
                            profile.definitions.erase(param->declPosition);
                        }
                        profile.uses.insert(param->declPosition);
                        profile.uses.insert(param->uses.begin(), param->uses.end());

                        // ensure dependencies and aliases are within local-scope
                        if (profile.function == param->function) {
                            profile.aliases.insert(param->aliases.begin(), param->aliases.end());
                            profile.dvars.insert(param->dvars.begin(), param->dvars.end());
                        }

                        // move the control edges over
                        profile.controlEdges.insert(param->controlEdges.begin(), param->controlEdges.end());

                        // the parameter's own calls become calls of this profile (appended after the loop)
                        complementaryCalls.insert(param->cfunctions.begin(), param->cfunctions.end());
                    } else if (verboseMode) {
                        std::cout << "[-] " << __FUNCTION__ << ":" << __LINE__ << " | no profile for parameter "
                                  << ArgProfParam << " of '" << cfunc.functionName << "'" << "\n";
                    }
                }
            } else {
                if (verboseMode) {
                    std::cout << "[-] " << __FUNCTION__ << ":" << __LINE__ << " | Cannot find Function Signature Collection for '" << cfunc.functionName << "'" << "\n";
                }
            }
        }
        // move cfunc data over
        sp.cfunctions.insert(complementaryCalls.begin(), complementaryCalls.end());
    }
}