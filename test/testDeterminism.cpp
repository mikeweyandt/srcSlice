// SPDX-License-Identifier: GPL-3.0-only
/**
 * @file testDeterminism.cpp
 *
 * @copyright Copyright (C) 2018-2024 srcML, LLC. (www.srcML.org)
 *
 * This file is part of the srcSlice application.
 *
 * The same archive must yield the same slices whatever the thread count, and twice in a
 * row. A generated program of a few hundred units that call into each other (mostly into
 * units that come later), share local names and a global, makes the merge order with
 * several threads differ from run to run.
 *
 * [!mayfail] cases describe the intended behaviour; see the comment above each for the
 * change that is expected to make them pass. Remove the tag when it lands.
 */

#define CATCH_CONFIG_MAIN
#include "./srcSliceTest.hpp"

#include <algorithm>
#include <random>

namespace {

constexpr int kUnits = 160;

void GenerateUnits(int n, std::vector<std::string>& contents, std::vector<std::string>& names) {
    for (int k = 0; k < n; ++k) {
        std::ostringstream s;
        // k leading comment lines: no two units share a line:column, so the known
        // SlicePosition::operator< file-blind collision cannot mask ordering effects
        for (int j = 0; j < k; ++j) s << "// unit " << k << "\n";
        s << "#include \"shared.hpp\"\n";
        s << "int f" << k << "(int n, int ctx) {\n";
        s << "    int i = n + ctx;\n";
        if (k % 4 == 0) {
            // every fourth unit is heavy enough that its worker is still running when the
            // parser has delivered the next units, so completion order differs from parse order
            for (int j = 0; j < 700; ++j) s << "    i = i + ctx * " << j << ";\n";
        }
        s << "    shared = shared + i;\n";
        s << "    f" << (k + 1) % n << "(i, ctx);\n";
        s << "    f" << (k + 5) % n << "(ctx, i);\n";
        s << "    return i;\n";
        s << "}\n";
        s << "void use" << k << "() {\n";
        s << "    int ctx = " << k << ";\n";
        s << "    int n = ctx * 2;\n";
        s << "    f" << k << "(n, ctx);\n";
        s << "}\n";
        contents.push_back(s.str());
        names.push_back("unit" + std::to_string(k) + ".cpp");
    }
    contents.push_back("#ifndef SHARED_HPP\n#define SHARED_HPP\ninline int shared = 0;\n#endif\n");
    names.push_back("shared.hpp");
}

TestArg WithThreads(int threads) {
    TestArg arg = {false, false, false};
    arg.threadCount = threads;
    return arg;
}

} // namespace

TEST_CASE("Determinism: repeated single-threaded runs are identical", "[srcslice][determinism]") {
    std::vector<std::string> contents, names;
    GenerateUnits(kUnits, contents, names);
    auto first = FetchCanonical(contents, names, WithThreads(1));
    REQUIRE( first.size() > static_cast<size_t>(kUnits) );
    auto second = FetchCanonical(contents, names, WithThreads(1));
    // compare into a bool: Catch's expansion of two large vectors takes minutes to print
    bool same = (first == second);
    INFO( FirstDifference(first, second) );
    REQUIRE( same );
}

// Defect: with several threads units are merged in completion order and the result depends
// on it (use/calls sets change between runs). Expected to pass after Step 1 (in-order merge).
TEST_CASE("Determinism: thread count does not change the slices", "[srcslice][determinism][!mayfail]") {
    std::vector<std::string> contents, names;
    GenerateUnits(kUnits, contents, names);
    auto base = FetchCanonical(contents, names, WithThreads(1));
    REQUIRE( base.size() > static_cast<size_t>(kUnits) );
    for (int threads : {2, 8, 8}) {
        auto other = FetchCanonical(contents, names, WithThreads(threads));
        bool same = (other == base);
        INFO( "threads=" << threads << " " << FirstDifference(base, other) );
        CHECK( same );
    }
}

// Defect: the result depends on the order of the units in the archive (vector order per
// name, first-match lookups, unordered_map iteration). Expected to pass after Step 2c
// (canonical processing order in Finalize).
TEST_CASE("Determinism: unit order does not change the slices", "[srcslice][determinism][!mayfail]") {
    std::vector<std::string> contents, names;
    GenerateUnits(kUnits, contents, names);
    auto base = FetchCanonical(contents, names, WithThreads(1));

    std::vector<std::string> rc(contents.rbegin(), contents.rend()), rn(names.rbegin(), names.rend());
    auto reversed = FetchCanonical(rc, rn, WithThreads(1));
    bool sameReversed = (reversed == base);
    INFO( "reversed: " << FirstDifference(base, reversed) );
    CHECK( sameReversed );

    std::vector<size_t> order(contents.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::shuffle(order.begin(), order.end(), std::mt19937(12345));
    std::vector<std::string> sc, sn;
    for (size_t i : order) { sc.push_back(contents[i]); sn.push_back(names[i]); }
    auto shuffled = FetchCanonical(sc, sn, WithThreads(1));
    bool sameShuffled = (shuffled == base);
    INFO( "shuffled: " << FirstDifference(base, shuffled) );
    CHECK( sameShuffled );
}
