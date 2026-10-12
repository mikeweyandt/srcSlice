// SPDX-License-Identifier: GPL-3.0-only
/**
 * @file srcsliceprofile.cpp
 *
 * @copyright Copyright (C) 2018-2024 srcML, LLC. (www.srcML.org)
 *
 * This file is part of the srcSlice application.
 */

#include "srcsliceprofile.hpp"

#include <charconv>

SliceProfile::SliceProfile() : containsDeclaration(false), potentialAlias(false), dereferenced(false),
                    isGlobal(false) { isPointer = false; isReference = false; }

SliceProfile::SliceProfile(
        std::string name, SlicePosition decl, bool alias, bool global,
        PositionSet aDef, PositionSet aUse,
        FlatSet<FunctionCallData> cFunc,
        NamedPositionSet dv,
        EdgeSet edges,
        bool containsDecl, bool visit) :
        variableName(name), declPosition(decl), potentialAlias(alias),
        isGlobal(global), definitions(aDef), uses(aUse), cfunctions(cFunc),
        dvars(dv), containsDeclaration(containsDecl), controlEdges(edges),
        visited(visit) {}

bool SliceProfile::operator==(const SliceProfile& rhs) const {
    if (this == &rhs) return true; // self-check protection

    if (declPosition != rhs.declPosition) return false;
    if (file != rhs.file) return false;
    if (function != rhs.function) return false;
    if (nameOfContainingClass != rhs.nameOfContainingClass) return false;
    if (containingNameSpaces != rhs.containingNameSpaces) return false;
    if (language != rhs.language) return false;
    if (checksum != rhs.checksum) return false;
    if (potentialAlias != rhs.potentialAlias) return false;
    if (dereferenced != rhs.dereferenced) return false;
    if (isGlobal != rhs.isGlobal) return false;
    if (classMemberVar != rhs.classMemberVar) return false;
    if (containsDeclaration != rhs.containsDeclaration) return false;
    if (isPointer != rhs.isPointer) return false;
    if (isReference != rhs.isReference) return false;
    if (variableName != rhs.variableName) return false;
    if (variableType != rhs.variableType) return false;
    if (definitions != rhs.definitions) return false;
    if (uses != rhs.uses) return false;
    if (dvars != rhs.dvars) return false;
    if (aliases != rhs.aliases) return false;
    if (controlEdges != rhs.controlEdges) return false;
    if (cfunctions != rhs.cfunctions) return false;
    if (endOfScope != rhs.endOfScope) return false;
    if (visited != rhs.visited) return false;
    if (showControlEdges != rhs.showControlEdges) return false;
    if (currentPointerReference != rhs.currentPointerReference) return false;
    if (isPotentialArray != rhs.isPotentialArray) return false;
    if (ignorePtrRef != rhs.ignorePtrRef) return false;
    if (isFragment != rhs.isFragment) return false;
    if (expandCalls != rhs.expandCalls) return false;

    return true;
}
bool SliceProfile::operator!=(const SliceProfile& rhs) const {
    return !(*this == rhs);
}

namespace {
    void AppendUnsigned(std::string& out, unsigned long long value) {
        char buf[24];
        auto r = std::to_chars(buf, buf + sizeof buf, value);
        out.append(buf, static_cast<std::size_t>(r.ptr - buf));
    }
}

// Byte-for-byte the document the std::string-per-position, stream-per-field version
// produced; the entry is assembled in the caller's buffer and written once (perf plan,
// output phase: the per-call ostream machinery was a tenth of the parse thread's time).
void SliceProfile::AppendJson(std::string& out) const {
    out += "    \"file\":\"";     out += file.str();                 out += "\",\n";
    out += "    \"language\":\""; out += language.str();             out += "\",\n";

    out += "    \"namespace\":[";
    bool first = true;
    for (const std::string& nameSpace : containingNameSpaces) {
        if (!first) out += ',';
        out += '"';
        out.append(nameSpace, 0, nameSpace.find(' '));
        out += '"';
        first = false;
    }
    out += "],\n";

    out += "    \"class\":\"";    out += nameOfContainingClass.str(); out += "\",\n";
    out += "    \"function\":\""; out += function.str();             out += "\",\n";
    out += "    \"type\":\"";     out += variableType.str();         out += "\",\n";
    out += "    \"name\":\"";     out += variableName;               out += "\",\n";

    out += "    \"decl\":";
    declPosition.AppendStart(out);
    out += ",\n";

    out += "    \"dependence\":[";
    first = true;
    for (const auto& dvar : dvars) {
        if (!first) out += ',';
        out += "{\"";
        out += dvar.first;
        out += "\":";
        dvar.second.AppendStart(out);
        out += '}';
        first = false;
    }
    out += "],\n";

    out += "    \"aliases\":[";
    first = true;
    for (const auto& alias : aliases) {
        if (!first) out += ',';
        out += "{\"";
        out += alias.first;
        out += "\":";
        alias.second.AppendStart(out);
        out += '}';
        first = false;
    }
    out += "],\n";

    out += "    \"calls\":[";
    first = true;
    for (const auto& cfunc : cfunctions) {
        if (!first) out += ',';
        out += "{\"functionName\":\"";
        out += cfunc.functionName.str();
        out += "\",\"parameter\":\"";
        AppendUnsigned(out, cfunc.parameterIndex);
        out += "\",\"definitionPosition\":";
        cfunc.funcPos.AppendStart(out);
        if (expandCalls) {
            out += ",\"endOfFunction\":";
            cfunc.funcPos.AppendEnd(out);
        }
        out += ",\"invoke\":";
        cfunc.invokePosition.AppendStart(out);
        out += '}';
        first = false;
    }
    out += "],\n";

    // "use": [ "file:line:col", ... ]
    out += "    \"use\":[";
    first = true;
    for (const auto& use : uses) {
        if (!first) out += ',';
        use.AppendStart(out);
        first = false;
    }
    out += "],\n";

    // "definition": [ "file:line:col", ... ]
    out += "    \"definition\":[";
    first = true;
    for (const auto& def : definitions) {
        if (!first) out += ',';
        def.AppendStart(out);
        first = false;
    }

    if (showControlEdges) {
        out += "],\n";

        out += "    \"controlEdges\":[";
        first = true;
        for (const auto& edge : controlEdges) {
            if (!first) out += ',';
            out += '[';
            edge.first.AppendStart(out);
            out += ',';
            edge.second.AppendStart(out);
            out += ']';
            first = false;
        }
        out += "]\n";
    } else {
        out += "]\n";
    }
}

std::ostream& operator<<(std::ostream& out, SliceProfile& profile) {
    std::string entry;
    profile.AppendJson(entry);
    out.write(entry.data(), static_cast<std::streamsize>(entry.size()));
    return out;
}

void SliceProfile::merge(SliceProfile& other) {
    if (!other.isFragment) {
        std::cerr << "[!] SliceProfile::Merge argument should be a fragment, merge might not be correct.\n";
    }
    
    uses.insert(other.uses.begin(), other.uses.end());
    other.uses.clear();

    definitions.insert(other.definitions.begin(), other.definitions.end());
    other.definitions.clear();

    controlEdges.insert(other.controlEdges.begin(), other.controlEdges.end());
    other.controlEdges.clear();

    dvars.insert(other.dvars.begin(), other.dvars.end());
    other.dvars.clear();

    aliases.insert(other.aliases.begin(), other.aliases.end());
    other.aliases.clear();

    cfunctions.insert(other.cfunctions.begin(), other.cfunctions.end());
    other.cfunctions.clear();
}

bool SliceProfile::inScope(const SlicePosition& pos) {
    if (isGlobal || isFragment) {
        return file == pos.GetFileName();
    }

    // class member variables are tricky to scope effectively
    if (classMemberVar) {
        return true;
    }

    if (pos < declPosition) return false;
    if (endOfScope < pos) return false;
    return true;
}

std::string SliceProfile::jsonKey() const {
    return variableName + '-' + declPosition.ToNameString() + '-' + checksum;
}