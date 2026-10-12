// SPDX-License-Identifier: GPL-3.0-only
/**
 * @file srcslicecollection.hpp
 *
 * @copyright Copyright (C) 2018-2024 srcML, LLC. (www.srcML.org)
 *
 * This file is part of the srcSlice application.
 */

#ifndef SRCSLICECOLLETION
#define SRCSLICECOLLETION

#include <cstdint>
#include <exception>
#include <unordered_map>
#include <set>
#include <memory>
#include <any>
#include <utility>
#include <vector>
#include <string>
#include <string_view>
#include <iostream>
#include <fstream>
#include <sstream>

#include <flatset.hpp>

#include <srcSAXHandler.hpp>
#include <srcDispatcher.hpp>
#include <FunctionPolicy.hpp>
#include <ClassPolicy.hpp>
#include <UnitPolicy.hpp>

struct PositionMeta {
    bool isIfCondition = false;
    bool isElifCondition = false;
};

// A string shared process-wide: equal texts point at one std::string that lives for the rest
// of the process (perf plan M1/M2). Eight bytes per holder instead of a std::string (32 bytes
// plus a heap block beyond 15 characters); millions of positions, call entries and profiles
// repeat the same file names, function names, types and checksums. Comparison and ordering
// are by text, so containers keyed on it behave exactly as with std::string.
class InternedString {
public:
    InternedString() = default;
    InternedString(const std::string& text) : p(Intern(text)) {}
    InternedString(const char* text) : p(Intern(text)) {}

    const std::string& str() const { static const std::string none; return p ? *p : none; }
    operator const std::string&() const { return str(); }
    bool empty() const { return !p || p->empty(); }
    std::size_t size() const { return p ? p->size() : 0; }
    const char* c_str() const { return str().c_str(); }
    bool ends_with(std::string_view suffix) const { return str().ends_with(suffix); }
    const std::string* ptr() const { return p; }

    // Thread-safe. A per-thread cache answers repeats (a worker sees the same file name and
    // the same function names over and over) without taking the pool's lock.
    static const std::string* Intern(const std::string& text);
private:
    const std::string* p = nullptr;
};

inline bool operator==(const InternedString& a, const InternedString& b) { return a.ptr() == b.ptr() || a.str() == b.str(); }
inline bool operator!=(const InternedString& a, const InternedString& b) { return !(a == b); }
inline bool operator<(const InternedString& a, const InternedString& b) { return a.str() < b.str(); }
inline bool operator==(const InternedString& a, const std::string& b) { return a.str() == b; }
inline bool operator==(const std::string& a, const InternedString& b) { return a == b.str(); }
inline bool operator!=(const InternedString& a, const std::string& b) { return a.str() != b; }
inline bool operator!=(const std::string& a, const InternedString& b) { return a != b.str(); }
inline bool operator==(const InternedString& a, const char* b) { return a.str() == b; }
inline bool operator!=(const InternedString& a, const char* b) { return a.str() != b; }
inline std::string operator+(const std::string& a, const InternedString& b) { return a + b.str(); }
inline std::string operator+(const InternedString& a, const std::string& b) { return a.str() + b; }
inline std::string operator+(const InternedString& a, const char* b) { return a.str() + b; }
inline std::string operator+(const char* a, const InternedString& b) { return a + b.str(); }
inline std::string operator+(const InternedString& a, char b) { return a.str() + b; }
inline std::ostream& operator<<(std::ostream& out, const InternedString& s) { return out << s.str(); }

// A source range in one file. The file name is interned (one std::string per distinct path,
// shared by every position in that file) and the coordinates are four 32-bit integers, so a
// position is about 32 bytes; it used to carry its own std::string and two
// DeltaElement<Position> (two std::optional<Position> each), about 200 bytes, and lives in
// millions of std::set nodes (perf plan, M1).
class SlicePosition {
public:
    SlicePosition();
    SlicePosition(srcDispatch::DeltaElement<srcDispatch::Position> start,
            srcDispatch::DeltaElement<srcDispatch::Position> end,
            const std::string& filename);

    SlicePosition(const SlicePosition& position) = default;
    SlicePosition(SlicePosition&& position) noexcept = default;
    SlicePosition& operator=(const SlicePosition& rhs) = default;
    SlicePosition& operator=(SlicePosition&& rhs) noexcept = default;

    bool operator==(const SlicePosition& rhs) const;
    bool operator!=(const SlicePosition& rhs) const;

    // file-blind: orders by start line, then start column (known limitation, see findings.md)
    bool operator<(const SlicePosition& rhs) const;
    bool operator<=(const SlicePosition& rhs) const;
    bool operator>(const SlicePosition& rhs) const;
    bool operator>=(const SlicePosition& rhs) const;

    std::string StartToString() const;
    std::string EndToString() const;

    // the same text as StartToString()/EndToString(), written straight into the stream (the
    // JSON writer emits millions of positions; building a std::string for each was half of
    // the output phase on medium)
    void WriteStart(std::ostream& out) const;
    void WriteEnd(std::ostream& out) const;
    // the same text appended to a buffer
    void AppendStart(std::string& out) const;
    void AppendEnd(std::string& out) const;

    std::string RangeToString() const;
    std::string ToNameString() const;

    bool HasStart() const { return hasStart; }
    bool HasEnd() const { return hasEnd; }
    std::size_t StartLine() const { return startLine; }
    std::size_t StartColumn() const { return startColumn; }
    std::size_t EndLine() const { return endLine; }
    std::size_t EndColumn() const { return endColumn; }

    // the interned path ("" for a default-constructed position); valid for the whole process
    const std::string& GetFileName() const;
    // the interned pointer itself (nullptr without a file name): equal texts share one pointer,
    // so it serves as a hash key
    const std::string* FilePtr() const { return file; }
    PositionMeta& GetData();
    const PositionMeta& GetData() const { return data; }

    // Returns the process-wide shared copy of `filename`. Thread-safe; a worker builds the
    // positions of one file at a time, so a per-thread cache of the last answer makes the
    // common call lock-free.
    static const std::string* InternFileName(const std::string& filename);
private:
    const std::string* file = nullptr;   // nullptr = no file name
    std::uint32_t startLine = 0;
    std::uint32_t startColumn = 0;
    std::uint32_t endLine = 0;
    std::uint32_t endColumn = 0;
    bool hasStart = false;
    bool hasEnd = false;
    PositionMeta data;
};

// The slice sets, ordered by SlicePosition::operator< (start line and column, file-blind:
// a known limitation, see findings.md "Correctness issues" for what a file-aware ordering
// changes and why it is not enabled).
using PositionSet = FlatSet<SlicePosition>;
using NamedPositionSet = FlatSet<std::pair<std::string, SlicePosition>>;
using EdgeSet = FlatSet<std::pair<SlicePosition, SlicePosition>>;

// return if SlicePosition a is contained in the scope of SlicePosition b
bool IsContained(SlicePosition& a, SlicePosition b);

// return distance between SlicePosition a from b
size_t GetDistance(SlicePosition& a, SlicePosition& b);

// return index to a group element context that a sline is contained in
// i.e if group = if_stmt group, return index to if_stmt the sline is contained in
size_t FindContextBlock(SlicePosition& sline, std::vector<SlicePosition>& group);

// Hold context data srcslice uses based off srcSAXEventContext without copying
// the entirety of srcSAXEventContext
class SliceCtx {
public:
    SliceCtx(): defined(false) {}
    SliceCtx(const srcDispatch::srcSAXEventContext& ctx);

    SliceCtx(const SliceCtx& rhs) = default;
    SliceCtx(SliceCtx&& rhs) noexcept = default;
    SliceCtx& operator=(const SliceCtx& rhs) = default;
    SliceCtx& operator=(SliceCtx&& rhs) noexcept = default;

    std::string currentFilePath;
    std::string currentFileChecksum;
    std::string currentFileLanguage;
    std::vector<std::string> containingNamespaces;
    bool defined = false;
};

// What call resolution needs to know about one parameter of a signature: its name and where
// the name is declared (in the signature's file). Copied out of the unit model so that the
// model can be freed once the unit is sliced (perf plan M3); `name` is empty when the
// parameter has no usable name.
struct SignatureParameter {
    std::string name;
    SlicePosition namePosition;
};

// Store Data about Function Signatures by grouping functions of the same name
// like Slices in profileMap
class FunctionSignatureData {
public:
    FunctionSignatureData(){};
    FunctionSignatureData(srcDispatch::DeltaElement<std::shared_ptr<srcDispatch::FunctionData>>& func,
                            std::string className, bool blockDefined, const SliceCtx& ctx);
    SlicePosition position;
    std::string name;
    std::string returnType;
    std::vector<SignatureParameter> parameters;
    std::string containingClass;
    std::string currentFilePath;
    std::string currentFileChecksum;
    std::string currentFileLanguage;
    std::vector<std::string> containingNamespaces;
    bool isDefined;
};

// Store meta-data about a function-call by tracking:
// name of the function being called, line where the call is made, index of the parameter,
// and the line where the function is potentially defined
class FunctionCallData {
public:
    FunctionCallData(){}
    FunctionCallData(
        std::string funcName,
        unsigned int paramIndex,
        unsigned int argc,
        SlicePosition funcPos,
        SlicePosition invokePos,
        bool ignore_ = false
    );

    FunctionCallData(const FunctionCallData& rhs) = default;
    FunctionCallData(FunctionCallData&& rhs) noexcept = default;
    FunctionCallData& operator=(const FunctionCallData& rhs) = default;
    FunctionCallData& operator=(FunctionCallData&& rhs) noexcept = default;

    bool operator==(const FunctionCallData& rhs) const;
    bool operator!=(const FunctionCallData& rhs) const;

    bool operator<(const FunctionCallData& rhs) const;
    bool operator>(const FunctionCallData& rhs) const;

    // for debugging
    friend std::ostream& operator<<(std::ostream& outStream, const FunctionCallData& data);

    SlicePosition invokePosition;
    SlicePosition funcPos;
    bool ignore = false;
    InternedString functionName;
    unsigned int parameterIndex;
    unsigned int argumentCount;
};

#endif