// SPDX-License-Identifier: GPL-3.0-only
/**
 * @file srcslicehandler.hpp
 *
 * @copyright Copyright (C) 2018-2024 srcML, LLC. (www.srcML.org)
 *
 * This file is part of the srcSlice application.
 */

#ifndef SRCSLICEHANDLER
#define SRCSLICEHANDLER

#include <srcsliceprofile.hpp>
#include <srcsliceprogress.hpp>
#include <srcsliceparse.hpp>
#include <srcsliceworker.hpp>

#include <algorithm>
#include <condition_variable>
#include <functional>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include <exception>
#include <unordered_map>
#include <unordered_set>
#include <fstream>
#include <sstream>
#include <CLI11.hpp>

#include <srcDispatcher.hpp>
#include <srcSAXHandler.hpp>

#include <IfStmtPolicy.hpp>
#include <SwitchPolicy.hpp>
#include <WhilePolicy.hpp>
#include <ForPolicy.hpp>
#include <DoPolicy.hpp>
#include <TryPolicy.hpp>
#include <FunctionPolicy.hpp>
#include <ClassPolicy.hpp>
#include <UnitPolicy.hpp>

#if _WIN64
    #include <windows.h>
#elif __APPLE__
    #include <sys/types.h>
    #include <sys/sysctl.h>
#else
    #include <sched.h>
#endif

int CPUCount();

// type aliasing
typedef std::vector<srcDispatch::DeltaElement<std::shared_ptr<srcDispatch::DeclStmtData>>> DeclStmts;
typedef srcDispatch::DeltaElement<std::shared_ptr<srcDispatch::DeclData>> DeclInfo;
typedef srcDispatch::DeltaElement<std::shared_ptr<srcDispatch::ExpressionData>> ExprInfo;
typedef srcDispatch::DeltaElement<std::shared_ptr<srcDispatch::FunctionData>> FunctionInfo;
typedef srcDispatch::DeltaElement<std::shared_ptr<srcDispatch::BlockData>> BlockInfo;
typedef std::vector<srcDispatch::DeltaElement<std::shared_ptr<srcDispatch::FunctionData>>> Functions;
typedef std::vector<srcDispatch::DeltaElement<std::shared_ptr<srcDispatch::ClassData>>> Classes;

struct CliInfo {
    std::string inputFile;
    std::string outputFile;
    int threadCount;
    bool calculateControlEdges;
    bool expandCalls;
    bool expandAliases;
    bool verboseMode;
    bool progressMode;
    // pipeline tuning (0 = default: backlogCap = threadCount, maxInFlight = 2 * threadCount)
    int backlogCap = 0;
    int maxInFlight = 0;
};
struct TestArg {
    bool calculateControlEdges;
    bool expandCalls;
    bool expandAliases;
    // threading knobs for the test-suite (0 = handler default)
    int threadCount = 1;
    int backlogCap = 0;
    int maxInFlight = 0;
};

// Counters the manager keeps while a run is in progress (read after the constructor returns).
struct PipelineStats {
    std::size_t units = 0;          // units handed to the manager by the parser
    std::size_t merged = 0;         // units merged into the main structure
    std::size_t maxBacklog = 0;     // most parsed-but-not-started units at any time (<= backlogCap)
    std::size_t maxInFlight = 0;    // most started-but-not-merged units at any time (<= maxInFlight)
    std::size_t maxRunning = 0;     // most worker threads alive at any time (<= threadCount)
};

class SrcSliceHandler : public srcDispatch::PolicyListener {
public:
    ~SrcSliceHandler();

    // Use information from cli (main.cpp)
    SrcSliceHandler(const CliInfo& info);

    // Use string srcml buffer ctor of srcSAXController
    SrcSliceHandler(std::string& sourceCodeStr, const TestArg& info);

    void Notify(const srcDispatch::PolicyDispatcher *policy, const srcDispatch::srcSAXEventContext &ctx) override;

    void NotifyWrite(const srcDispatch::PolicyDispatcher *policy [[maybe_unused]], srcDispatch::srcSAXEventContext &ctx [[maybe_unused]]) {};

    std::unordered_map<std::string, std::vector<SliceProfile>>& GetProfileMap();
    const PipelineStats& GetStats() const { return stats; }

    /*
        srcSlice focuses on Forward-Slicing, therefor our Control-Flows are going to be forward-flowing
        we are not focusing on backwards-flows.
    */
    void ComputeControlPaths();
    void ComputeOuterPaths(std::set<std::pair<SlicePosition,SlicePosition>>& controlEdges, std::vector<SlicePosition>& sLines);
    void ComputeExitPaths(std::set<std::pair<SlicePosition,SlicePosition>>& controlEdges, std::vector<SlicePosition>& sLines);

    SliceProfileIterator ArgumentProfile(const std::string& funcName, FunctionSignatureData& funcSig, int paramIndex);

    // Need to track Aliases we have already read through
    // InterProcedural from the normal call should also be reflected
    // if a profile with alias(s) is used in function calls
    void ComputeAliasInterprocedural();
    void ComputeInterprocedural();
    // Perform a second pass over the system dictionary performing another round of ComputeInterprocedural
    // against slices who have cfunction elements that could not resolve on the initial pass
    void Finalize();

    void mergeFragments(std::vector<SliceProfile>& profiles);
    void ModifySlice(SliceProfile& sp);
    void ResolveCall(SliceProfile& sp);
    void UpdateCalls(SliceProfile& sp);

    // thread target for managing SrcSliceWorker jobs
    void ManageThreads();

private:
    // Runs the parse callable with the manager thread alive, waits for every unit to be merged
    // and rethrows the first failure (a worker exception or a parse error) as std::runtime_error.
    void RunPipeline(const std::function<void()>& parse);
    void Parse(srcSAXController& control);
    void FailRun(std::exception_ptr error, const std::string& where);
    void MergeStructures(SrcSliceWorker& worker);
    void ApplyThreadSettings(int threads, int backlog, int inFlight);

    std::unordered_map<std::string, std::vector<SliceProfile>> profileMap;
    std::unordered_set <std::string> visited_func;

    std::vector<SlicePosition> loopdata;
    std::vector<SlicePosition> forloopdata;
    std::vector<SlicePosition> whileloopdata;
    std::vector<SlicePosition> dowhileloopdata;

    std::vector<SlicePosition> ifdata;
    std::vector<SlicePosition> ifStmts;
    std::vector<SlicePosition> elseifdata;
    std::vector<SlicePosition> elsedata;

    std::unordered_map<std::string, std::vector<FunctionSignatureData>> functionSigMap;

    bool verboseMode = false;
    bool calculateControlEdges = false;
    bool progressMode = false;
    bool expandCalls = false;
    bool expandAliases = false;

    // ---- unit pipeline: parser thread (Notify) -> workers -> manager thread (merge in parse order)
    // Every field below is guarded by backlogMutex unless stated otherwise.
    std::mutex backlogMutex;
    std::condition_variable backlogCv;   // wakes the manager: unit queued, worker finished, parse done, abort
    std::condition_variable spaceCv;     // wakes the parser: backlog has room, or abort
    std::deque<std::unique_ptr<SrcSliceWorker>> backlog;    // parsed, not started (FIFO)
    std::deque<std::unique_ptr<SrcSliceWorker>> inFlight;   // started, not merged, in start (= parse) order
    std::size_t nextSeq = 0;             // sequence number of the next unit the parser hands over
    std::size_t running = 0;             // worker threads that have not published completion yet
    bool parseDone = false;              // the parser has returned; no more units will arrive
    bool abortRun = false;               // a worker or the parser failed: drop everything, stop the parser
    std::exception_ptr firstError;       // the failure that set abortRun
    PipelineStats stats;
    srcSAXHandler* activeParser = nullptr;   // set by the parser thread for the duration of the parse

    int threadCount = 1;
    std::size_t backlogCap = 1;          // >= 1
    std::size_t maxInFlight = 2;         // >= threadCount

    std::unordered_map<std::string, std::vector<std::string>> fileDependencyTable;

    SliceCtx sctx;
};

#endif