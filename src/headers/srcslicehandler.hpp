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
#include <srcslicearchive.hpp>

#include <algorithm>
#include <condition_variable>
#include <functional>
#include <deque>
#include <map>
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
    // parsers reading the archive at once (0 = default: half the threads, at least one)
    int parserCount = 0;
};
struct TestArg {
    bool calculateControlEdges;
    bool expandCalls;
    bool expandAliases;
    // threading knobs for the test-suite (0 = handler default)
    int threadCount = 1;
    int backlogCap = 0;
    int maxInFlight = 0;
    int parserCount = 0;
};

// Counters the manager keeps while a run is in progress (read after the constructor returns).
struct PipelineStats {
    std::size_t units = 0;          // units handed to the manager by the parser
    std::size_t merged = 0;         // units merged into the main structure
    std::size_t maxBacklog = 0;     // most parsed-but-not-started units at any time (<= backlogCap)
    std::size_t maxInFlight = 0;    // most started-but-not-merged units at any time (<= maxInFlight)
    std::size_t maxRunning = 0;     // most worker threads alive at any time (<= threadCount)
    std::size_t parsers = 0;        // parsers that read the archive (1 unless it was split)
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
    void ComputeOuterPaths(EdgeSet& controlEdges, std::vector<SlicePosition>& sLines);
    void ComputeExitPaths(EdgeSet& controlEdges, std::vector<SlicePosition>& sLines);

    // The profile of parameter `paramIndex` of the function `funcSig` describes (declared in the
    // signature's file at the parameter's position), or nullptr when no unit produced it.
    SliceProfile* ParameterProfile(const FunctionSignatureData& funcSig, unsigned int paramIndex,
                                   const std::string& simpleFunctionName);

    // Expand each declared profile's alias list by the aliases of its pointer/reference
    // targets; returns true when any list grew (Finalize repeats it to a fixpoint)
    bool ComputeAliasInterprocedural();
    // Resolve the calls of every declared profile once (ResolveCall, recursive over callees)
    void ComputeInterprocedural();
    // The single pass over the merged structure: control edges, fragment gluing, call
    // completion and resolution, alias expansion (in that order)
    void Finalize();

    void mergeFragments(std::vector<SliceProfile>& profiles);
    void ResolveCall(SliceProfile& sp);
    void UpdateCalls(SliceProfile& sp);

    // thread target for managing SrcSliceWorker jobs
    void ManageThreads();

private:
    // Runs the parse callable with the manager thread alive, waits for every unit to be merged
    // and rethrows the first failure (a worker exception or a parse error) as std::runtime_error.
    void RunPipeline(const std::function<void()>& parse);
    void Parse(srcSAXController& control);
    // Parses the whole input: with one parser through the given controller factory, with
    // several by splitting the archive into per-parser streams (perf plan S10).
    void ParseArchive(const ArchiveBytes& bytes, const std::function<std::unique_ptr<srcSAXController>()>& wholeInput);
    void FailRun(std::exception_ptr error, const std::string& where);
    // call from a catch block: the exception being handled as a std::exception
    static std::exception_ptr ParseFailure();
    void MergeStructures(SrcSliceWorker& worker);
    void ApplyThreadSettings(int threads, int backlog, int inFlight, int parsers);

    std::unordered_map<std::string, std::vector<SliceProfile>> profileMap;
    std::unordered_set <std::string> visited_func;

    // Declared profiles by declaration position, built by Finalize once the fragments are
    // glued (profiles do not move afterwards) and dropped when it returns. ParameterProfile
    // used to scan every profile of the parameter's name (thousands for `i`) for each call
    // entry (perf plan S6). The key mirrors SlicePosition::operator==: both ends present,
    // same interned file, same coordinates; candidates keep their name vector's order.
    struct DeclaredKey {
        const std::string* file;
        std::uint32_t startLine, startColumn, endLine, endColumn;
        explicit DeclaredKey(const SlicePosition& p)
            : file(p.FilePtr()), startLine(static_cast<std::uint32_t>(p.StartLine())),
              startColumn(static_cast<std::uint32_t>(p.StartColumn())),
              endLine(static_cast<std::uint32_t>(p.EndLine())),
              endColumn(static_cast<std::uint32_t>(p.EndColumn())) {}
        bool operator==(const DeclaredKey&) const = default;
    };
    struct DeclaredKeyHash {
        std::size_t operator()(const DeclaredKey& k) const {
            std::size_t h = std::hash<const void*>()(k.file);
            for (std::uint64_t v : {std::uint64_t(k.startLine), std::uint64_t(k.startColumn), std::uint64_t(k.endLine), std::uint64_t(k.endColumn)}) {
                h ^= (v + 0x9e3779b97f4a7c15ULL) + (h << 6) + (h >> 2);
            }
            return h;
        }
    };
    std::unordered_map<DeclaredKey, std::vector<SliceProfile*>, DeclaredKeyHash> declaredByPosition;
    void BuildDeclaredIndex();

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
    // parsed, not started, by archive position (one parser hands units over in order; several
    // parsers interleave, and the manager always starts the earliest)
    std::map<std::size_t, std::unique_ptr<SrcSliceWorker>> backlog;
    // started, not merged, by archive position; the earliest is merged once it is done
    std::map<std::size_t, std::unique_ptr<SrcSliceWorker>> inFlight;
    std::size_t running = 0;             // worker threads that have not published completion yet
    bool parseDone = false;              // the parser has returned; no more units will arrive
    bool abortRun = false;               // a worker or the parser failed: drop everything, stop the parser
    std::exception_ptr firstError;       // the failure that set abortRun
    PipelineStats stats;

    int threadCount = 1;
    int parserCount = 1;                 // >= 1; the archive is split when > 1 and it has enough units
    std::size_t backlogCap = 1;          // >= 1
    std::size_t maxInFlight = 2;         // >= threadCount

    std::unordered_map<std::string, std::vector<std::string>> fileDependencyTable;

    SliceCtx sctx;
};

#endif