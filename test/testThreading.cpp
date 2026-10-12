// SPDX-License-Identifier: GPL-3.0-only
/**
 * @file testThreading.cpp
 *
 * @copyright Copyright (C) 2018-2024 srcML, LLC. (www.srcML.org)
 *
 * This file is part of the srcSlice application.
 *
 * The unit pipeline (parser -> bounded backlog -> workers -> in-order merge) must give the
 * single-threaded result for every thread count and queue size, respect its memory bounds,
 * handle the archive edge cases and turn a failing worker or a broken archive into a
 * std::runtime_error out of the constructor instead of a hang or std::terminate.
 */

#define CATCH_CONFIG_MAIN
#include "./srcSliceTest.hpp"

namespace {

constexpr int kUnits = 64;

// Small units that call into each other; k leading comment lines keep every unit's positions
// distinct (SlicePosition::operator< ignores the file).
void GenerateUnits(int n, std::vector<std::string>& contents, std::vector<std::string>& names) {
    for (int k = 0; k < n; ++k) {
        std::ostringstream s;
        for (int j = 0; j < k; ++j) s << "// unit " << k << "\n";
        s << "int f" << k << "(int n, int ctx) {\n";
        s << "    int i = n + ctx;\n";
        if (k % 8 == 0) {
            for (int j = 0; j < 200; ++j) s << "    i = i + ctx * " << j << ";\n";
        }
        s << "    f" << (k + 1) % n << "(i, ctx);\n";
        s << "    return i;\n";
        s << "}\n";
        contents.push_back(s.str());
        names.push_back("unit" + std::to_string(k) + ".cpp");
    }
}

struct Run {
    std::vector<std::string> slices;
    PipelineStats stats;
};

Run Slice(const std::vector<std::string>& contents, const std::vector<std::string>& names,
          int threads, int backlogCap = 0, int maxInFlight = 0, int parsers = 1) {
    std::string srcml = StringsToArchive(contents, names);
    TestArg arg = {false, false, false};
    arg.threadCount = threads;
    arg.backlogCap = backlogCap;
    arg.maxInFlight = maxInFlight;
    arg.parserCount = parsers;
    SrcSliceHandler handler(srcml, arg);
    return { CanonicalSlices(handler), handler.GetStats() };
}

// restores the worker's test hook even when a REQUIRE fails
struct HookGuard {
    ~HookGuard() { SrcSliceWorker::testHook = nullptr; }
};

} // namespace

TEST_CASE("Threading: every thread count and queue size gives the single-threaded result", "[srcslice][threading]") {
    std::vector<std::string> contents, names;
    GenerateUnits(kUnits, contents, names);

    Run base = Slice(contents, names, 1);
    REQUIRE( base.stats.units == static_cast<size_t>(kUnits) );
    REQUIRE( base.stats.merged == static_cast<size_t>(kUnits) );
    REQUIRE( base.slices.size() >= static_cast<size_t>(3 * kUnits) ); // n, ctx, i per unit

    struct Config { int threads, backlog, inFlight; };
    for (Config c : { Config{1, 1, 0}, Config{8, 2, 0}, Config{2, 64, 0}, Config{8, 0, 8},
                      Config{16, 1, 0}, Config{4, 1, 4}, Config{3, 5, 7} }) {
        INFO( "threads=" << c.threads << " backlog=" << c.backlog << " inFlight=" << c.inFlight );
        Run run = Slice(contents, names, c.threads, c.backlog, c.inFlight);
        bool same = (run.slices == base.slices);
        INFO( FirstDifference(base.slices, run.slices) );
        CHECK( same );
        CHECK( run.stats.units == static_cast<size_t>(kUnits) );
        CHECK( run.stats.merged == static_cast<size_t>(kUnits) );
    }
}

TEST_CASE("Threading: several parsers give the single-threaded result", "[srcslice][threading]") {
    std::vector<std::string> contents, names;
    GenerateUnits(kUnits, contents, names);

    Run base = Slice(contents, names, 1);
    REQUIRE( base.stats.parsers == 1 );

    // the archive is dealt round-robin to the parsers and merged in archive order; the
    // merge window (in-flight) must hold at least one unit per parser
    struct Config { int threads, backlog, inFlight, parsers; };
    for (Config c : { Config{2, 0, 0, 2}, Config{4, 0, 0, 4}, Config{8, 1, 0, 3}, Config{8, 2, 8, 8},
                      Config{16, 0, 0, 0}, Config{32, 0, 0, 32}, Config{3, 5, 7, 2} }) {
        INFO( "threads=" << c.threads << " backlog=" << c.backlog << " inFlight=" << c.inFlight << " parsers=" << c.parsers );
        Run run = Slice(contents, names, c.threads, c.backlog, c.inFlight, c.parsers);
        bool same = (run.slices == base.slices);
        INFO( FirstDifference(base.slices, run.slices) );
        CHECK( same );
        CHECK( run.stats.units == static_cast<size_t>(kUnits) );
        CHECK( run.stats.merged == static_cast<size_t>(kUnits) );
        CHECK( run.stats.parsers >= 1 );
        CHECK( run.stats.maxInFlight <= (c.inFlight > 0 ? static_cast<size_t>(std::max(c.inFlight, c.threads)) : static_cast<size_t>(2 * c.threads)) );
        CHECK( run.stats.maxRunning <= static_cast<size_t>(c.threads) );
    }
    // more parsers than the archive can use are cut back; a single-unit input stays one parser
    Run many = Slice(contents, names, 8, 0, 0, 64);
    CHECK( many.slices == base.slices );
    CHECK( many.stats.parsers <= static_cast<size_t>(kUnits / 2) );
    std::vector<std::string> one(contents.begin(), contents.begin() + 1), oneName(names.begin(), names.begin() + 1);
    Run single = Slice(one, oneName, 4, 0, 0, 4);
    CHECK( single.stats.parsers == 1 );
    CHECK( single.stats.units == 1 );
}

TEST_CASE("Threading: the backlog, in-flight window and worker count stay within their bounds", "[srcslice][threading]") {
    std::vector<std::string> contents, names;
    GenerateUnits(kUnits, contents, names);

    struct Config { int threads, backlog, inFlight; size_t expectBacklog, expectInFlight; };
    // defaults: backlog = threads, inFlight = 2 * threads; inFlight is clamped to >= threads
    for (Config c : { Config{1, 0, 0, 1, 2}, Config{8, 2, 0, 2, 16}, Config{2, 64, 0, 64, 4},
                      Config{8, 0, 8, 8, 8}, Config{4, 1, 2, 1, 4}, Config{16, 1, 0, 1, 32} }) {
        INFO( "threads=" << c.threads << " backlog=" << c.backlog << " inFlight=" << c.inFlight );
        Run run = Slice(contents, names, c.threads, c.backlog, c.inFlight);
        CHECK( run.stats.maxBacklog <= c.expectBacklog );
        CHECK( run.stats.maxInFlight <= c.expectInFlight );
        CHECK( run.stats.maxRunning <= static_cast<size_t>(c.threads) );
        CHECK( run.stats.maxInFlight >= 1 );
    }

    // with one thread nothing runs concurrently
    Run serial = Slice(contents, names, 1);
    CHECK( serial.stats.maxRunning == 1 );
}

TEST_CASE("Threading: more threads than units", "[srcslice][threading]") {
    std::vector<std::string> contents, names;
    GenerateUnits(3, contents, names);
    Run base = Slice(contents, names, 1);
    Run wide = Slice(contents, names, 32);
    bool same = (wide.slices == base.slices);
    INFO( FirstDifference(base.slices, wide.slices) );
    CHECK( same );
    CHECK( wide.stats.units == 3 );
    CHECK( wide.stats.merged == 3 );
    CHECK( wide.stats.maxRunning <= 3 );
}

TEST_CASE("Threading: a single unit", "[srcslice][threading]") {
    std::vector<std::string> contents = {"int f(int a) { int b = a; return b; }\n"};
    std::vector<std::string> names = {"only.cpp"};
    Run base = Slice(contents, names, 1);
    Run threaded = Slice(contents, names, 8);
    REQUIRE( base.slices.size() == 2 );
    bool same = (threaded.slices == base.slices);
    CHECK( same );
    CHECK( threaded.stats.units == 1 );
    CHECK( threaded.stats.merged == 1 );
}

TEST_CASE("Threading: an archive without units", "[srcslice][threading]") {
    // libsrcml writes an archive with no units as a bare root element, which srcSAX reports
    // as one (empty) unit; whatever arrives must be merged and must yield no slices
    std::vector<std::string> none;
    for (int threads : {1, 8}) {
        Run run = Slice(none, none, threads);
        CHECK( run.slices.empty() );
        CHECK( run.stats.units <= 1 );
        CHECK( run.stats.merged == run.stats.units );
    }
}

TEST_CASE("Threading: a failing worker fails the run and names the unit", "[srcslice][threading]") {
    using Catch::Matchers::Contains;
    std::vector<std::string> contents, names;
    GenerateUnits(kUnits, contents, names);
    std::string srcml = StringsToArchive(contents, names);

    HookGuard guard;
    for (const char* victim : {"unit7.cpp", "unit0.cpp", "unit63.cpp"}) {
        SrcSliceWorker::testHook = [victim](const SliceCtx& ctx) {
            if (ctx.currentFilePath == victim) throw std::runtime_error("synthetic worker failure");
        };
        for (int threads : {1, 4, 16}) {
            INFO( "victim=" << victim << " threads=" << threads );
            TestArg arg = {false, false, false};
            arg.threadCount = threads;
            REQUIRE_THROWS_WITH( SrcSliceHandler(srcml, arg),
                                 Contains(victim) && Contains("synthetic worker failure") );
        }
    }

    // a failure that is not a std::exception is reported too
    SrcSliceWorker::testHook = [](const SliceCtx& ctx) {
        if (ctx.currentFilePath == "unit5.cpp") throw 42;
    };
    TestArg arg = {false, false, false};
    arg.threadCount = 4;
    REQUIRE_THROWS_AS( SrcSliceHandler(srcml, arg), std::runtime_error );
}

TEST_CASE("Threading: the hook does not fire once reset", "[srcslice][threading]") {
    std::vector<std::string> contents, names;
    GenerateUnits(4, contents, names);
    REQUIRE( !SrcSliceWorker::testHook );
    Run run = Slice(contents, names, 4);
    CHECK( run.stats.merged == 4 );
}

TEST_CASE("Threading: a broken archive throws std::runtime_error", "[srcslice][threading]") {
    for (int threads : {1, 8}) {
        TestArg arg = {false, false, false};
        arg.threadCount = threads;
        std::string truncated = "<unit xmlns=\"http://www.srcML.org/srcML/src\" revision=\"1.0.0\" language=\"C++\" filename=\"a.cpp\">"
                                "<function><type><name>int</name></type> <name>f</name>";
        REQUIRE_THROWS_AS( SrcSliceHandler(truncated, arg), std::runtime_error );
        std::string empty;
        REQUIRE_THROWS_AS( SrcSliceHandler(empty, arg), std::runtime_error );
    }
}
