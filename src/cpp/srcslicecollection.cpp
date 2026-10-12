// SPDX-License-Identifier: GPL-3.0-only
/**
 * @file srcslicecollection.cpp
 *
 * @copyright Copyright (C) 2018-2024 srcML, LLC. (www.srcML.org)
 *
 * This file is part of the srcSlice application.
 */

#include "srcslicecollection.hpp"

#include <charconv>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

const std::string* InternedString::Intern(const std::string& text) {
    // worker threads live for one unit: the cache is small and dies with the thread
    thread_local std::unordered_map<std::string, const std::string*> cache;
    auto hit = cache.find(text);
    if (hit != cache.end()) return hit->second;

    // node-based: element addresses stay valid when the table rehashes
    static std::mutex poolMutex;
    static std::unordered_set<std::string> pool;
    const std::string* shared;
    {
        std::lock_guard<std::mutex> lock(poolMutex);
        shared = &*pool.insert(text).first;
    }
    cache.emplace(text, shared);
    return shared;
}

const std::string* SlicePosition::InternFileName(const std::string& filename) {
    return InternedString::Intern(filename);
}

SlicePosition::SlicePosition() {}
SlicePosition::SlicePosition(
                    srcDispatch::DeltaElement<srcDispatch::Position> start,
                    srcDispatch::DeltaElement<srcDispatch::Position> end,
                    const std::string& filename
                ): file(InternFileName(filename)), hasStart(static_cast<bool>(start)), hasEnd(static_cast<bool>(end)) {
    if (hasStart) {
        startLine = static_cast<std::uint32_t>(start->GetLine());
        startColumn = static_cast<std::uint32_t>(start->GetColumn());
    }
    if (hasEnd) {
        endLine = static_cast<std::uint32_t>(end->GetLine());
        endColumn = static_cast<std::uint32_t>(end->GetColumn());
    }
}

namespace {
    std::string PositionText(std::size_t line, std::size_t column) {
        return std::to_string(line) + ':' + std::to_string(column);
    }

    // '"' + file + (":line:column" when present) + '"', without a temporary string
    void WriteQuoted(std::ostream& out, const std::string& file, bool hasPosition, std::uint32_t line, std::uint32_t column) {
        out.put('"');
        if (!file.empty()) out.write(file.data(), static_cast<std::streamsize>(file.size()));
        if (hasPosition) {
            char buf[32];
            buf[0] = ':';
            auto r = std::to_chars(buf + 1, buf + sizeof buf, line);
            *r.ptr++ = ':';
            r = std::to_chars(r.ptr, buf + sizeof buf, column);
            out.write(buf, r.ptr - buf);
        }
        out.put('"');
    }
}

namespace {
    void AppendQuoted(std::string& out, const std::string& file, bool hasPosition, std::uint32_t line, std::uint32_t column) {
        out.push_back('"');
        out.append(file);
        if (hasPosition) {
            char buf[32];
            buf[0] = ':';
            auto r = std::to_chars(buf + 1, buf + sizeof buf, line);
            *r.ptr++ = ':';
            r = std::to_chars(r.ptr, buf + sizeof buf, column);
            out.append(buf, static_cast<std::size_t>(r.ptr - buf));
        }
        out.push_back('"');
    }
}

void SlicePosition::AppendStart(std::string& out) const {
    AppendQuoted(out, GetFileName(), hasStart, startLine, startColumn);
}

void SlicePosition::AppendEnd(std::string& out) const {
    AppendQuoted(out, GetFileName(), hasEnd, endLine, endColumn);
}

void SlicePosition::WriteStart(std::ostream& out) const {
    WriteQuoted(out, GetFileName(), hasStart, startLine, startColumn);
}

void SlicePosition::WriteEnd(std::ostream& out) const {
    WriteQuoted(out, GetFileName(), hasEnd, endLine, endColumn);
}

// Creates a String of a JSON object, ie: "file.cpp:2:12"
// based on the start position
std::string SlicePosition::StartToString() const {
    std::string s;

    s += "\"";

    if (!GetFileName().empty()) {
        s += GetFileName();
    }

    if (hasStart) {
        s += ":";
        s += PositionText(startLine, startColumn);
    }

    s += "\"";

    return s;
}
// Creates a String of a JSON object, ie: "file.cpp:2:12"
// based on the end position
std::string SlicePosition::EndToString() const {
    std::string s;

    s += "\"";

    if (!GetFileName().empty()) {
        s += GetFileName();
    }

    if (hasEnd) {
        s += ":";
        s += PositionText(endLine, endColumn);
    }

    s += "\"";

    return s;
}
// Creates a string formated "file:start-file:end"
std::string SlicePosition::RangeToString() const {
    std::string s;

    s += "\"";
    if (hasStart && !GetFileName().empty()) {
        s += GetFileName(); s += ":";
        s += PositionText(startLine, startColumn);
    }
    s += "-";
    if (hasEnd && !GetFileName().empty()) {
        s += GetFileName(); s += ":";
        s += PositionText(endLine, endColumn);
    }
    s += "\"";

    return s;
}
// produces a string used in JSON key creation, ie: argc-3-14-e12a342c...2a41
// introducing line-col into key string helps keep keys unique when there are
// multiple variables of the same name throughout a file
std::string SlicePosition::ToNameString() const {
    std::string s;

    std::string lineStr = hasStart ? std::to_string(startLine) : "0";
    std::string colStr = hasStart ? std::to_string(startColumn) : "0";
    s += lineStr + "-" + colStr;

    return s;
}

const std::string& SlicePosition::GetFileName() const {
    static const std::string none;
    return file ? *file : none;
}
// return specific data about a position
PositionMeta& SlicePosition::GetData() { return data; }

bool SlicePosition::operator==(const SlicePosition& rhs) const {
    bool validPositions = (hasStart && hasEnd && rhs.hasStart && rhs.hasEnd);
    if (validPositions) {
        bool matchingStarts = startLine == rhs.startLine && startColumn == rhs.startColumn;
        bool matchingEnds = endLine == rhs.endLine && endColumn == rhs.endColumn;
        // interned: equal paths share one string; fall back to the text for safety
        bool matchingFiles = file == rhs.file || GetFileName() == rhs.GetFileName();

        return matchingStarts && matchingEnds && matchingFiles;
    }
    return false;
}
bool SlicePosition::operator!=(const SlicePosition& rhs) const {
    return !(*this == rhs);
}
bool SlicePosition::operator<(const SlicePosition& rhs) const {
    if (!hasStart || !rhs.hasStart) return false;

    // line comparison
    if (startLine > rhs.startLine) return false;
    if (startLine < rhs.startLine) return true;

    return startColumn < rhs.startColumn;
}
bool SlicePosition::operator<=(const SlicePosition& rhs) const {
    return (*this < rhs) || *this == rhs;
}

bool SlicePosition::operator>(const SlicePosition& rhs) const {
    return (rhs < *this);
}
bool SlicePosition::operator>=(const SlicePosition& rhs) const {
    return (rhs < *this) || *this == rhs;
}

bool IsContained(SlicePosition& a, SlicePosition b) {
    if (a.GetFileName() != b.GetFileName()) return false;
    
    bool lineContained = a.StartLine() >= b.StartLine() && a.EndLine() <= b.EndLine();

    bool isOneLiner = a.StartLine() == b.StartLine() && a.EndLine() == b.EndLine();
    bool colContained = true;
    
    if (isOneLiner) {
        // start column of a should be greater than the start column of b
        // end column of a should be less than the end column of b
        colContained = a.StartColumn() >= b.StartColumn() && a.EndColumn() <= b.EndColumn();
    } else {
        // if the end row of b and the start row of a are the same
        // check if the start column of a is less than the end column of b
        if (a.StartLine() == b.EndLine()) {
            colContained = a.StartColumn() < b.EndColumn();
        }
    }

    return lineContained && colContained;
};

size_t GetDistance(SlicePosition& a, SlicePosition& b) {
    size_t dist = 0;
    dist = b.StartLine() - a.StartLine();

    // if row a > row b => a isnt connected to b
    if (dist >= 0) {
        size_t bc = b.EndColumn();
        size_t ac = a.StartColumn();
        
        dist += ac - bc;
    }

    return dist;
}

size_t FindContextBlock(SlicePosition& sline, std::vector<SlicePosition>& group) {
    size_t ctxIndex = -1;
    for (size_t i = 0; i < group.size(); ++i) {
        // skip ifStmt blocks that are below the sline
        if (sline < group[i]) continue;

        // may trigger multiple times in nesting situations
        if (IsContained(sline, group[i])) {
            ctxIndex = i;
        }
    }
    return ctxIndex;
};

FunctionSignatureData::FunctionSignatureData(
    srcDispatch::DeltaElement<std::shared_ptr<srcDispatch::FunctionData>>& func,
    std::string className, bool blockDefined, const SliceCtx& ctx) {
    position = SlicePosition(func->startPosition, func->endPosition, ctx.currentFilePath);
    name = func->name.ToString();
    returnType = func->returnType.ToString();
    parameters.reserve(func->parameters.size());
    for (const auto& param : func->parameters) {
        SignatureParameter slim;
        if (param && param->name && param->name.GetElement()) {
            slim.name = param->name.ToString();
            slim.namePosition = SlicePosition(param->name->startPosition, param->name->endPosition, ctx.currentFilePath);
        }
        parameters.push_back(std::move(slim));
    }
    containingClass = className;
    currentFilePath = ctx.currentFilePath;
    currentFileChecksum = ctx.currentFileChecksum;
    currentFileLanguage = ctx.currentFileLanguage;
    containingNamespaces = ctx.containingNamespaces;
    isDefined = blockDefined;
}

FunctionCallData::FunctionCallData(
    std::string funcName,
    unsigned int paramIndex,
    unsigned int argc,
    SlicePosition funcPos,
    SlicePosition invokePos,
    bool ignore_
): functionName(funcName), parameterIndex(paramIndex),
funcPos(funcPos), invokePosition(invokePos),
argumentCount(argc), ignore(ignore_) {};

bool FunctionCallData::operator==(const FunctionCallData& rhs) const {
    if (functionName != rhs.functionName) return false;
    if (invokePosition != rhs.invokePosition) return false;
    if (parameterIndex != rhs.parameterIndex) return false;
    if (argumentCount != rhs.argumentCount) return false;
    if (funcPos != rhs.funcPos) return false;
    if (ignore != rhs.ignore) return false;
    return true;
}
bool FunctionCallData::operator!=(const FunctionCallData& rhs) const {
    return !(*this == rhs);
}

bool FunctionCallData::operator<(const FunctionCallData& rhs) const {
    // interned: equal pointers are equal names; otherwise one three-way compare instead of an
    // equality test followed by a less-than (set inserts of call entries are a hot path of
    // ResolveCall)
    if (functionName.ptr() != rhs.functionName.ptr()) {
        const int order = functionName.str().compare(rhs.functionName.str());
        if (order != 0) return order < 0;
    }
    if (invokePosition != rhs.invokePosition)
        return invokePosition < rhs.invokePosition;
    if (argumentCount != rhs.argumentCount)
        return argumentCount < rhs.argumentCount;
    if (parameterIndex != rhs.parameterIndex)
        return parameterIndex < rhs.parameterIndex;
    if (funcPos != rhs.funcPos)
        return funcPos < rhs.funcPos;
    if (ignore != rhs.ignore)
        return ignore < rhs.ignore;
        
    return false;
}
bool FunctionCallData::operator>(const FunctionCallData& rhs) const {
    return rhs < *this;
}

std::ostream& operator<<(std::ostream& outStream, const FunctionCallData& data) {
    outStream << "[" << data.functionName << " | " << data.parameterIndex
    << " | " << data.funcPos.StartToString() << " | " << data.invokePosition.StartToString() << "]";
    return outStream;
}

SliceCtx::SliceCtx(const srcDispatch::srcSAXEventContext& ctx) {
    defined = true;
    currentFilePath = ctx.currentFilePath;
    currentFileChecksum = ctx.currentFileChecksum;
    currentFileLanguage = ctx.currentFileLanguage;
    containingNamespaces = ctx.currentNamespaces;
}
