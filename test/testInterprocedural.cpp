// SPDX-License-Identifier: GPL-3.0-only
/**
 * @file testInterprocedural.cpp
 *
 * @copyright Copyright (C) 2018-2024 srcML, LLC. (www.srcML.org)
 *
 * This file is part of the srcSlice application.
 *
 * Cross-unit call resolution. Every program is run in the given unit order and in the
 * reversed order (callee before / after caller) and must produce the same slices.
 *
 * Cases tagged [!mayfail] describe the intended behaviour and currently fail because of a
 * known defect; the comment above each names the change that is expected to fix it. Catch
 * reports them without failing the suite. Remove the tag when the fix lands.
 *
 * Files are written so that no two units share a line:column position that ends up in the
 * same slice: SlicePosition::operator< ignores the file name, so equal positions from
 * different files collapse inside a std::set (see the last case, which pins that defect).
 */

#define CATCH_CONFIG_MAIN
#include "./srcSliceTest.hpp"

#include <algorithm>

static const TestArg data = {false, false, false};

// Runs the program in the given unit order and reversed; both must match `expected`.
static bool CheckBothOrders(const std::string& testName, std::vector<std::string> files,
                            std::vector<std::string> names, const json& expected, bool strict = true) {
    json forward = json::parse(FetchSlices(files, names, data));
    if (!CompareJson("", testName + " [given order]", forward, expected, false, strict)) return false;
    std::reverse(files.begin(), files.end());
    std::reverse(names.begin(), names.end());
    json reversed = json::parse(FetchSlices(files, names, data));
    return CompareJson("", testName + " [reversed order]", reversed, expected, false, strict);
}

// Subset check for sets whose exact closure is not worth pinning (recursion cycles).
static bool ContainsPositions(const json& produced, const std::string& sliceId, const std::string& field,
                              std::initializer_list<const char*> positions) {
    if (!produced.contains(sliceId)) {
        std::cout << ERR << " missing slice " << sliceId << std::endl;
        return false;
    }
    const json& arr = produced[sliceId][field];
    for (const char* p : positions) {
        if (std::find(arr.begin(), arr.end(), json(p)) == arr.end()) {
            std::cout << ERR << " " << sliceId << "." << field << " lacks " << p << " in " << arr.dump() << std::endl;
            return false;
        }
    }
    return true;
}

static std::string CurrentTest() {
    return Catch::getResultCapture().getCurrentTestName();
}

TEST_CASE("Interprocedural: callee defined in a later unit", "[srcslice][interprocedural]") {
    std::vector<std::string> files = {
R"(int main() {
    int b = 0;
    foo(b);
}
)",
R"(void foo(int a) {
    a = 5;
}
)"};
    json expected = R"({
    "a-1-14":{"file":"foo.cpp","language":"C++","namespace":[],"class":"","function":"foo","type":"int","name":"a",
        "decl":"foo.cpp:1:14","dependence":[],"aliases":[],"calls":[],"use":[],
        "definition":["foo.cpp:1:14","foo.cpp:2:5"]},
    "b-2-9":{"file":"main.cpp","language":"C++","namespace":[],"class":"","function":"main","type":"int","name":"b",
        "decl":"main.cpp:2:9","dependence":[],"aliases":[],
        "calls":[{"functionName":"foo","parameter":"1","definitionPosition":"foo.cpp:1:1","invoke":"main.cpp:3:5"}],
        "use":["main.cpp:3:9","foo.cpp:1:14","foo.cpp:2:5"],
        "definition":["main.cpp:2:9"]}
    })"_json;
    REQUIRE( CheckBothOrders(CurrentTest(), files, {"main.cpp", "foo.cpp"}, expected) );
}

// ResolveCall must write its result into the profile being resolved, not into the last
// profile with the argument's name (fixed in Step 2a1: before, `second`'s x received foo's
// data as well and `first`'s x received nothing).
TEST_CASE("Interprocedural: same variable name in two callers", "[srcslice][interprocedural]") {
    std::vector<std::string> files = {
R"(void foo(int a) {
    a = 5;
}
void bar(int c) {
    c = 7;
}
)",
R"(void first() {
    int x = 0;
    foo(x);
}
void second() {
    int x = 1;
    bar(x);
}
)"};
    json expected = R"({
    "a-1-14":{"file":"lib.cpp","language":"C++","namespace":[],"class":"","function":"foo","type":"int","name":"a",
        "decl":"lib.cpp:1:14","dependence":[],"aliases":[],"calls":[],"use":[],"definition":["lib.cpp:1:14","lib.cpp:2:5"]},
    "c-4-14":{"file":"lib.cpp","language":"C++","namespace":[],"class":"","function":"bar","type":"int","name":"c",
        "decl":"lib.cpp:4:14","dependence":[],"aliases":[],"calls":[],"use":[],"definition":["lib.cpp:4:14","lib.cpp:5:5"]},
    "x-2-9":{"file":"use.cpp","language":"C++","namespace":[],"class":"","function":"first","type":"int","name":"x",
        "decl":"use.cpp:2:9","dependence":[],"aliases":[],
        "calls":[{"functionName":"foo","parameter":"1","definitionPosition":"lib.cpp:1:1","invoke":"use.cpp:3:5"}],
        "use":["use.cpp:3:9","lib.cpp:1:14","lib.cpp:2:5"],"definition":["use.cpp:2:9"]},
    "x-6-9":{"file":"use.cpp","language":"C++","namespace":[],"class":"","function":"second","type":"int","name":"x",
        "decl":"use.cpp:6:9","dependence":[],"aliases":[],
        "calls":[{"functionName":"bar","parameter":"1","definitionPosition":"lib.cpp:4:1","invoke":"use.cpp:7:5"}],
        "use":["use.cpp:7:9","lib.cpp:4:14","lib.cpp:5:5"],"definition":["use.cpp:6:9"]}
    })"_json;
    REQUIRE( CheckBothOrders(CurrentTest(), files, {"lib.cpp", "use.cpp"}, expected) );
}

// Defect: v is resolved once at merge time against whatever p carried at that moment and is
// never revisited; whether bar's data reaches v depends on unit and map order (one unit
// order passes today, the other does not). Expected to flip in Step 2b.
TEST_CASE("Interprocedural: callee in a later unit passes the parameter on", "[srcslice][interprocedural][!mayfail]") {
    std::vector<std::string> files = {
R"(int main() {
    int v = 0;
    foo(v);
}
)",
R"(void foo(int p) {
    bar(p);
}
)",
R"(// bar
void bar(int q) {
    q = 9;
}
)"};
    json expected = R"({
    "q-2-14":{"file":"bar.cpp","language":"C++","namespace":[],"class":"","function":"bar","type":"int","name":"q",
        "decl":"bar.cpp:2:14","dependence":[],"aliases":[],"calls":[],"use":[],"definition":["bar.cpp:2:14","bar.cpp:3:5"]},
    "p-1-14":{"file":"foo.cpp","language":"C++","namespace":[],"class":"","function":"foo","type":"int","name":"p",
        "decl":"foo.cpp:1:14","dependence":[],"aliases":[],
        "calls":[{"functionName":"bar","parameter":"1","definitionPosition":"bar.cpp:2:1","invoke":"foo.cpp:2:5"}],
        "use":["foo.cpp:2:9","bar.cpp:2:14","bar.cpp:3:5"],"definition":["foo.cpp:1:14"]},
    "v-2-9":{"file":"main.cpp","language":"C++","namespace":[],"class":"","function":"main","type":"int","name":"v",
        "decl":"main.cpp:2:9","dependence":[],"aliases":[],
        "calls":[{"functionName":"foo","parameter":"1","definitionPosition":"foo.cpp:1:1","invoke":"main.cpp:3:5"},
                 {"functionName":"bar","parameter":"1","definitionPosition":"bar.cpp:2:1","invoke":"foo.cpp:2:5"}],
        "use":["main.cpp:3:9","foo.cpp:1:14","foo.cpp:2:9","bar.cpp:2:14","bar.cpp:3:5"],"definition":["main.cpp:2:9"]}
    })"_json;
    REQUIRE( CheckBothOrders(CurrentTest(), files, {"main.cpp", "foo.cpp", "bar.cpp"}, expected) );
}

// Shared program: inc's parameter `n` is not the last profile named `n` (other's is).
static const std::vector<std::string> notLastFiles = {
R"(void deep(int d) {
    d = 42;
}
void inc(int n) {
    deep(n);
}
void other(int n) {
    n = 3;
}
)",
R"(int main() {
    int v = 1;
    inc(v);
}
)"};

// Write target (fixed in Step 2a1): deep's data must reach inc's `n`, not other's `n` (the
// last `n`).
TEST_CASE("Interprocedural: data lands in the resolved parameter, not the last same-named one",
          "[srcslice][interprocedural]") {
    json expected = R"({
    "d-1-15":{"file":"lib.cpp","language":"C++","namespace":[],"class":"","function":"deep","type":"int","name":"d",
        "decl":"lib.cpp:1:15","dependence":[],"aliases":[],"calls":[],"use":[],"definition":["lib.cpp:1:15","lib.cpp:2:5"]},
    "n-4-14":{"file":"lib.cpp","language":"C++","namespace":[],"class":"","function":"inc","type":"int","name":"n",
        "decl":"lib.cpp:4:14","dependence":[],"aliases":[],
        "calls":[{"functionName":"deep","parameter":"1","definitionPosition":"lib.cpp:1:1","invoke":"lib.cpp:5:5"}],
        "use":["lib.cpp:5:10","lib.cpp:1:15","lib.cpp:2:5"],"definition":["lib.cpp:4:14"]},
    "n-7-16":{"file":"lib.cpp","language":"C++","namespace":[],"class":"","function":"other","type":"int","name":"n",
        "decl":"lib.cpp:7:16","dependence":[],"aliases":[],"calls":[],"use":[],"definition":["lib.cpp:7:16","lib.cpp:8:5"]}
    })"_json;
    REQUIRE( CheckBothOrders(CurrentTest(), notLastFiles, {"lib.cpp", "main.cpp"}, expected, false) );
}

// Two hops: the caller must receive deep's data through inc's `n`. Passes since Step 2a1
// (ArgumentProfile still pre-resolves the last `n`, but inc's `n` is resolved on its own
// before the caller copies it); Step 2a2 makes the recursion target the parameter itself.
TEST_CASE("Interprocedural: caller receives data two hops away through a non-last parameter",
          "[srcslice][interprocedural]") {
    json expected = R"({
    "v-2-9":{"file":"main.cpp","language":"C++","namespace":[],"class":"","function":"main","type":"int","name":"v",
        "decl":"main.cpp:2:9","dependence":[],"aliases":[],
        "calls":[{"functionName":"inc","parameter":"1","definitionPosition":"lib.cpp:4:1","invoke":"main.cpp:3:5"},
                 {"functionName":"deep","parameter":"1","definitionPosition":"lib.cpp:1:1","invoke":"lib.cpp:5:5"}],
        "use":["main.cpp:3:9","lib.cpp:4:14","lib.cpp:5:10","lib.cpp:1:15","lib.cpp:2:5"],"definition":["main.cpp:2:9"]}
    })"_json;
    REQUIRE( CheckBothOrders(CurrentTest(), notLastFiles, {"lib.cpp", "main.cpp"}, expected, false) );
}

TEST_CASE("Interprocedural: parameter name shared by two callees", "[srcslice][interprocedural]") {
    std::vector<std::string> files = {
R"(void foo(int a) {
    a = 1;
}
)",
R"(// bar
void bar(int a) {
    a = 2;
}
)",
R"(int main() {
    int x = 0;
    int y = 0;
    foo(x);
    bar(y);
}
)"};
    json expected = R"({
    "a-1-14":{"file":"u1.cpp","language":"C++","namespace":[],"class":"","function":"foo","type":"int","name":"a",
        "decl":"u1.cpp:1:14","dependence":[],"aliases":[],"calls":[],"use":[],"definition":["u1.cpp:1:14","u1.cpp:2:5"]},
    "a-2-14":{"file":"u2.cpp","language":"C++","namespace":[],"class":"","function":"bar","type":"int","name":"a",
        "decl":"u2.cpp:2:14","dependence":[],"aliases":[],"calls":[],"use":[],"definition":["u2.cpp:2:14","u2.cpp:3:5"]},
    "x-2-9":{"file":"u3.cpp","language":"C++","namespace":[],"class":"","function":"main","type":"int","name":"x",
        "decl":"u3.cpp:2:9","dependence":[],"aliases":[],
        "calls":[{"functionName":"foo","parameter":"1","definitionPosition":"u1.cpp:1:1","invoke":"u3.cpp:4:5"}],
        "use":["u3.cpp:4:9","u1.cpp:1:14","u1.cpp:2:5"],"definition":["u3.cpp:2:9"]},
    "y-3-9":{"file":"u3.cpp","language":"C++","namespace":[],"class":"","function":"main","type":"int","name":"y",
        "decl":"u3.cpp:3:9","dependence":[],"aliases":[],
        "calls":[{"functionName":"bar","parameter":"1","definitionPosition":"u2.cpp:2:1","invoke":"u3.cpp:5:5"}],
        "use":["u3.cpp:5:9","u2.cpp:2:14","u2.cpp:3:5"],"definition":["u3.cpp:3:9"]}
    })"_json;
    REQUIRE( CheckBothOrders(CurrentTest(), files, {"u1.cpp", "u2.cpp", "u3.cpp"}, expected) );
}

TEST_CASE("Interprocedural: overloads defined in a later unit", "[srcslice][interprocedural]") {
    std::vector<std::string> files = {
R"(int main() {
    int x = 0;
    char c = 0;
    foo(x);
    foo(x, c);
}
)",
R"(void foo(int a) {
    a = 1;
}
void foo(int a, char b) {
    a = 2;
    b = 3;
}
)"};
    json expected = R"({
    "a-1-14":{"file":"lib.cpp","language":"C++","namespace":[],"class":"","function":"foo","type":"int","name":"a",
        "decl":"lib.cpp:1:14","dependence":[],"aliases":[],"calls":[],"use":[],"definition":["lib.cpp:1:14","lib.cpp:2:5"]},
    "a-4-14":{"file":"lib.cpp","language":"C++","namespace":[],"class":"","function":"foo","type":"int","name":"a",
        "decl":"lib.cpp:4:14","dependence":[],"aliases":[],"calls":[],"use":[],"definition":["lib.cpp:4:14","lib.cpp:5:5"]},
    "b-4-22":{"file":"lib.cpp","language":"C++","namespace":[],"class":"","function":"foo","type":"char","name":"b",
        "decl":"lib.cpp:4:22","dependence":[],"aliases":[],"calls":[],"use":[],"definition":["lib.cpp:4:22","lib.cpp:6:5"]},
    "x-2-9":{"file":"caller.cpp","language":"C++","namespace":[],"class":"","function":"main","type":"int","name":"x",
        "decl":"caller.cpp:2:9","dependence":[],"aliases":[],
        "calls":[{"functionName":"foo","parameter":"1","definitionPosition":"lib.cpp:1:1","invoke":"caller.cpp:4:5"},
                 {"functionName":"foo","parameter":"1","definitionPosition":"lib.cpp:4:1","invoke":"caller.cpp:5:5"}],
        "use":["caller.cpp:4:9","caller.cpp:5:9","lib.cpp:1:14","lib.cpp:2:5","lib.cpp:4:14","lib.cpp:5:5"],
        "definition":["caller.cpp:2:9"]},
    "c-3-10":{"file":"caller.cpp","language":"C++","namespace":[],"class":"","function":"main","type":"char","name":"c",
        "decl":"caller.cpp:3:10","dependence":[],"aliases":[],
        "calls":[{"functionName":"foo","parameter":"2","definitionPosition":"lib.cpp:4:1","invoke":"caller.cpp:5:5"}],
        "use":["caller.cpp:5:12","lib.cpp:4:22","lib.cpp:6:5"],"definition":["caller.cpp:3:10"]}
    })"_json;
    REQUIRE( CheckBothOrders(CurrentTest(), files, {"caller.cpp", "lib.cpp"}, expected) );
}

TEST_CASE("Interprocedural: pass by reference and by pointer across units", "[srcslice][interprocedural]") {
    std::vector<std::string> files = {
R"(int main() {
    int v = 0;
    int w = 0;
    setr(v);
    setp(&w);
}
)",
R"(void setr(int& r) {
    r = 1;
}
void setp(int* p) {
    *p = 2;
}
)"};
    json expected = R"({
    "r-1-16":{"file":"lib.cpp","language":"C++","namespace":[],"class":"","function":"setr","type":"int&","name":"r",
        "decl":"lib.cpp:1:16","dependence":[],"aliases":[],"calls":[],"use":[],"definition":["lib.cpp:1:16","lib.cpp:2:5"]},
    "p-4-16":{"file":"lib.cpp","language":"C++","namespace":[],"class":"","function":"setp","type":"int*","name":"p",
        "decl":"lib.cpp:4:16","dependence":[],"aliases":[],"calls":[],"use":["lib.cpp:5:6"],"definition":["lib.cpp:4:16"]},
    "v-2-9":{"file":"main.cpp","language":"C++","namespace":[],"class":"","function":"main","type":"int","name":"v",
        "decl":"main.cpp:2:9","dependence":[],"aliases":[],
        "calls":[{"functionName":"setr","parameter":"1","definitionPosition":"lib.cpp:1:1","invoke":"main.cpp:4:5"}],
        "use":["main.cpp:4:10","lib.cpp:1:16"],"definition":["main.cpp:2:9","lib.cpp:2:5"]},
    "w-3-9":{"file":"main.cpp","language":"C++","namespace":[],"class":"","function":"main","type":"int","name":"w",
        "decl":"main.cpp:3:9","dependence":[],"aliases":[],
        "calls":[{"functionName":"setp","parameter":"1","definitionPosition":"lib.cpp:4:1","invoke":"main.cpp:5:5"}],
        "use":["main.cpp:5:11","lib.cpp:4:16","lib.cpp:5:6"],"definition":["main.cpp:3:9"]}
    })"_json;
    REQUIRE( CheckBothOrders(CurrentTest(), files, {"main.cpp", "lib.cpp"}, expected) );
}

// Defect: when the argument has the same name as the callee's parameter, the call is left
// unresolved (callee later) or resolved without any data flowing (callee first), because
// ResolveCall/ArgumentProfile pick profiles by bare name and `.back()`. Same family as the
// write-target defect; expected to flip with Steps 2a1/2a2.
TEST_CASE("Interprocedural: argument named like the parameter", "[srcslice][interprocedural][!mayfail]") {
    std::vector<std::string> files = {
R"(int main() {
    int r = 0;
    setr(r);
}
)",
R"(void setr(int& r) {
    r = 1;
}
)"};
    json expected = R"({
    "r-1-16":{"file":"lib.cpp","language":"C++","namespace":[],"class":"","function":"setr","type":"int&","name":"r",
        "decl":"lib.cpp:1:16","dependence":[],"aliases":[],"calls":[],"use":[],"definition":["lib.cpp:1:16","lib.cpp:2:5"]},
    "r-2-9":{"file":"main.cpp","language":"C++","namespace":[],"class":"","function":"main","type":"int","name":"r",
        "decl":"main.cpp:2:9","dependence":[],"aliases":[],
        "calls":[{"functionName":"setr","parameter":"1","definitionPosition":"lib.cpp:1:1","invoke":"main.cpp:3:5"}],
        "use":["main.cpp:3:10","lib.cpp:1:16"],"definition":["main.cpp:2:9","lib.cpp:2:5"]}
    })"_json;
    REQUIRE( CheckBothOrders(CurrentTest(), files, {"main.cpp", "lib.cpp"}, expected) );
}

// Defect: a global's fragments are glued into its root, but the root is never `partial`, so
// the call its fragment carried keeps an empty definitionPosition and take's data never
// reaches g. Expected to flip in Step 2b (every declared profile goes through UpdateCalls).
TEST_CASE("Interprocedural: global defined in a header, used and passed on in other units",
          "[srcslice][interprocedural][!mayfail]") {
    std::vector<std::string> files = {
R"(inline int g = 0;
)",
R"(#include "g.hpp"
void bump() {
    g = g + 1;
}
)",
R"(#include "g.hpp"
void hand() {
    take(g);
}
)",
R"(void take(int t) {
    t = 0;
}
)"};
    json expected = R"({
    "t-1-15":{"file":"u4.cpp","language":"C++","namespace":[],"class":"","function":"take","type":"int","name":"t",
        "decl":"u4.cpp:1:15","dependence":[],"aliases":[],"calls":[],"use":[],"definition":["u4.cpp:1:15","u4.cpp:2:5"]},
    "g-1-12":{"file":"g.hpp","language":"C++","namespace":[],"class":"","function":"","type":"int","name":"g",
        "decl":"g.hpp:1:12","dependence":[],"aliases":[],
        "calls":[{"functionName":"take","parameter":"1","definitionPosition":"u4.cpp:1:1","invoke":"u3.cpp:3:5"}],
        "use":["u2.cpp:3:9","u3.cpp:3:10","u4.cpp:1:15","u4.cpp:2:5"],"definition":["g.hpp:1:12","u2.cpp:3:5"]}
    })"_json;
    REQUIRE( CheckBothOrders(CurrentTest(), files, {"g.hpp", "u2.cpp", "u3.cpp", "u4.cpp"}, expected) );
}

// Defect: m is resolved when its unit is merged, while ping's parameter is still frozen
// (`visited`) without pong's data; m is never revisited, so y's definition two hops away
// and pong's definition position are missing. Expected to flip in Step 2b.
TEST_CASE("Interprocedural: mutual recursion across units reaches the caller",
          "[srcslice][interprocedural][!mayfail]") {
    std::vector<std::string> files = {
R"(void ping(int x) {
    pong(x);
}
)",
R"(// pong
void pong(int y) {
    y = 1;
    ping(y);
}
)",
R"(// main
// main
int main() {
    int m = 0;
    ping(m);
}
)"};
    std::vector<std::string> names = {"u1.cpp", "u2.cpp", "u3.cpp"};
    json expectedCalls = R"({
    "m-4-9":{"calls":[{"functionName":"ping","parameter":"1","definitionPosition":"u1.cpp:1:1","invoke":"u3.cpp:5:5"},
                      {"functionName":"pong","parameter":"1","definitionPosition":"u2.cpp:2:1","invoke":"u1.cpp:2:5"}]}
    })"_json;
    for (int pass = 0; pass < 2; ++pass) {
        json produced = json::parse(FetchSlices(files, names, data));
        std::string label = CurrentTest() + (pass ? " [reversed order]" : " [given order]");
        REQUIRE( produced.contains("m-4-9") );
        REQUIRE( CheckCalls(label, "m-4-9", produced, expectedCalls) );
        REQUIRE( ContainsPositions(produced, "m-4-9", "use",
                 {"u3.cpp:5:10", "u1.cpp:1:15", "u1.cpp:2:10", "u2.cpp:2:15", "u2.cpp:3:5", "u2.cpp:4:10"}) );
        REQUIRE( produced["m-4-9"]["definition"] == json::array({"u3.cpp:4:9"}) );
        std::reverse(files.begin(), files.end());
        std::reverse(names.begin(), names.end());
    }
}

TEST_CASE("Interprocedural: prototype in a header plus the definition", "[srcslice][interprocedural]") {
    std::vector<std::string> files = {
R"(void foo(int a);
)",
R"(#include "h.hpp"
void foo(int a) {
    a = 1;
}
)",
R"(#include "h.hpp"
int main() {
    int x = 0;
    foo(x);
}
)"};
    // Current behaviour: both the prototype and the definition count as signatures, so the
    // call lists both and the prototype's parameter declaration appears as a use.
    json expected = R"({
    "a-1-14":{"file":"h.hpp","language":"C++","namespace":[],"class":"","function":"foo","type":"int","name":"a",
        "decl":"h.hpp:1:14","dependence":[],"aliases":[],"calls":[],"use":[],"definition":["h.hpp:1:14"]},
    "a-2-14":{"file":"foo.cpp","language":"C++","namespace":[],"class":"","function":"foo","type":"int","name":"a",
        "decl":"foo.cpp:2:14","dependence":[],"aliases":[],"calls":[],"use":[],"definition":["foo.cpp:2:14","foo.cpp:3:5"]},
    "x-3-9":{"file":"main.cpp","language":"C++","namespace":[],"class":"","function":"main","type":"int","name":"x",
        "decl":"main.cpp:3:9","dependence":[],"aliases":[],
        "calls":[{"functionName":"foo","parameter":"1","definitionPosition":"h.hpp:1:1","invoke":"main.cpp:4:5"},
                 {"functionName":"foo","parameter":"1","definitionPosition":"foo.cpp:2:1","invoke":"main.cpp:4:5"}],
        "use":["main.cpp:4:9","h.hpp:1:14","foo.cpp:2:14","foo.cpp:3:5"],"definition":["main.cpp:3:9"]}
    })"_json;
    REQUIRE( CheckBothOrders(CurrentTest(), files, {"h.hpp", "foo.cpp", "main.cpp"}, expected) );
}

TEST_CASE("Interprocedural: call to a function defined nowhere", "[srcslice][interprocedural]") {
    std::vector<std::string> files = {
R"(int main() {
    int x = 0;
    external(x);
}
)"};
    json expected = R"({
    "x-2-9":{"file":"main.cpp","language":"C++","namespace":[],"class":"","function":"main","type":"int","name":"x",
        "decl":"main.cpp:2:9","dependence":[],"aliases":[],
        "calls":[{"functionName":"external","parameter":"1","definitionPosition":"","invoke":"main.cpp:3:5"}],
        "use":["main.cpp:3:14"],"definition":["main.cpp:2:9"]}
    })"_json;
    json produced = json::parse(FetchSlices(files, {"main.cpp"}, data));
    REQUIRE( CompareJson("", CurrentTest(), produced, expected, false, true) );
}

TEST_CASE("Interprocedural: empty archive and single unit", "[srcslice][interprocedural]") {
    json none = json::parse(FetchSlices(std::vector<std::string>{}, std::vector<std::string>{}, data));
    REQUIRE( none.is_object() );
    REQUIRE( none.empty() );

    json one = json::parse(FetchSlices({R"(int main() {
    int a = 0;
}
)"}, {"one.cpp"}, data));
    json expected = R"({
    "a-2-9":{"file":"one.cpp","language":"C++","namespace":[],"class":"","function":"main","type":"int","name":"a",
        "decl":"one.cpp:2:9","dependence":[],"aliases":[],"calls":[],"use":[],"definition":["one.cpp:2:9"]}
    })"_json;
    REQUIRE( CompareJson("", CurrentTest(), one, expected, false, true) );
}

// Defect (not scheduled in the current plan, listed in findings.md): SlicePosition::operator<
// compares line and column only, so a.cpp:1:14 and b.cpp:1:14 are "equal" inside v's use set
// and one of them is silently dropped.
TEST_CASE("Interprocedural: equal positions in different files are distinct",
          "[srcslice][interprocedural][!mayfail]") {
    std::vector<std::string> files = {
R"(void foo(int p) {
    p = 1;
}
)",
R"(void bar(int q) {
    q = 2;
}
)",
R"(int main() {
    int v = 0;
    foo(v);
    bar(v);
}
)"};
    json expected = R"({
    "v-2-9":{"file":"c.cpp","language":"C++","namespace":[],"class":"","function":"main","type":"int","name":"v",
        "decl":"c.cpp:2:9","dependence":[],"aliases":[],
        "calls":[{"functionName":"foo","parameter":"1","definitionPosition":"a.cpp:1:1","invoke":"c.cpp:3:5"},
                 {"functionName":"bar","parameter":"1","definitionPosition":"b.cpp:1:1","invoke":"c.cpp:4:5"}],
        "use":["c.cpp:3:9","c.cpp:4:9","a.cpp:1:14","a.cpp:2:5","b.cpp:1:14","b.cpp:2:5"],"definition":["c.cpp:2:9"]}
    })"_json;
    REQUIRE( CheckBothOrders(CurrentTest(), files, {"a.cpp", "b.cpp", "c.cpp"}, expected, false) );
}
