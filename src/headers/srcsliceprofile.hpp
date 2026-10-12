// SPDX-License-Identifier: GPL-3.0-only
/**
 * @file srcsliceprofile.hpp
 *
 * @copyright Copyright (C) 2018-2024 srcML, LLC. (www.srcML.org)
 *
 * This file is part of the srcSlice application.
 */

#ifndef SRCSLICEPROFILE
#define SRCSLICEPROFILE

#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <set>
#include <utility>
#include <vector>
#include <string>
#include <iterator>
#include <srcslicecollection.hpp> // contains FunctionCallData and other items
#include <flatset.hpp>

class SliceProfile {
public:
    SliceProfile();

    SliceProfile(
            std::string name, SlicePosition line, bool alias = 0, bool global = 0,
            PositionSet aDef = {}, PositionSet aUse = {},
            FlatSet<FunctionCallData> cFunc = {},
            NamedPositionSet dv = {},
            EdgeSet edges = {},
            bool containsDecl = false, bool visit = false);

    // defaulted so the implicit move operations exist: merging and fragment removal move
    // profiles (eight strings and six sets each) instead of copying them
    SliceProfile(const SliceProfile& rhs) = default;
    SliceProfile(SliceProfile&& rhs) noexcept = default;
    SliceProfile& operator=(const SliceProfile& rhs) = default;
    SliceProfile& operator=(SliceProfile&& rhs) noexcept = default;

    bool operator==(const SliceProfile& rhs) const;
    bool operator!=(const SliceProfile& rhs) const;

    SlicePosition declPosition;
    // interned (shared text, 8 bytes each): identical for every profile of a unit or function
    InternedString file;
    InternedString function;
    InternedString nameOfContainingClass;
    std::vector<std::string> containingNameSpaces;
    InternedString language;
    InternedString checksum;
    bool potentialAlias = false;
    bool dereferenced = false;

    bool isGlobal = false;
    bool classMemberVar = false;
    bool containsDeclaration = false;

    bool isPointer = false;
    bool isReference = false;

    std::string variableName;
    InternedString variableType;

    // sorted vectors (flatset.hpp) with std::set's order and duplicate rule; a tree node per
    // position used to cost more than the position itself
    PositionSet definitions;
    PositionSet uses;

    NamedPositionSet dvars;
    NamedPositionSet aliases;
    FlatSet<FunctionCallData> cfunctions;

    SlicePosition endOfScope;

    void merge(SliceProfile& other);
    bool inScope(const SlicePosition& pos);

    EdgeSet controlEdges;

    // set by ResolveCall: resolved once, and the cycle guard for recursive resolution
    bool visited = false;

    bool showControlEdges = false;
    bool expandCalls = false;

    std::string currentPointerReference;
    bool ignorePtrRef = false;
    bool isPotentialArray = false;

    bool isFragment = false;

    std::string jsonKey() const;
    // the entry's body (the lines between the braces) appended to a buffer; the stream
    // operator writes the same bytes
    void AppendJson(std::string& out) const;
    friend std::ostream& operator<<(std::ostream& out, SliceProfile& profile);
};

#endif