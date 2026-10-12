// SPDX-License-Identifier: GPL-3.0-only
/**
 * @file srcslicearchive.hpp
 *
 * @copyright Copyright (C) 2018-2024 srcML, LLC. (www.srcML.org)
 *
 * This file is part of the srcSlice application.
 */

#ifndef SRCSLICEARCHIVE
#define SRCSLICEARCHIVE

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Parsing an archive with several parsers at once (perf plan S10). A srcML archive is a
// root <unit> holding one <unit> per file, never nested deeper, and every '<' inside source
// text is escaped, so the units' byte ranges can be found with one pass over the bytes.
// Each parser then reads the prolog, every k-th unit and the root's end tag through
// libxml2's IO callback, from its own handle on the source, so no part of the archive is
// held in memory and the units it hands over are a strided subsequence of the archive.

// Where the archive's bytes come from. A reader is one caller's handle on it.
class ArchiveBytes {
public:
    class Reader {
    public:
        virtual ~Reader() = default;
        // copies up to `count` bytes starting at `offset`; returns how many (0 at the end)
        virtual std::size_t ReadAt(std::uint64_t offset, char* buffer, std::size_t count) = 0;
    };

    virtual ~ArchiveBytes() = default;
    virtual std::unique_ptr<Reader> OpenReader() const = 0;
};

// an archive on disk, read by offset
class ArchiveFile : public ArchiveBytes {
public:
    explicit ArchiveFile(std::string path) : path(std::move(path)) {}
    std::unique_ptr<Reader> OpenReader() const override;
    const std::string path;
};

// an archive held in a string (the test suites)
class ArchiveText : public ArchiveBytes {
public:
    explicit ArchiveText(const std::string& text) : text(text) {}
    std::unique_ptr<Reader> OpenReader() const override;
    const std::string& text;
};

// The top-level units of an archive, as byte ranges.
struct ArchiveLayout {
    using Range = std::pair<std::uint64_t, std::uint64_t>;   // [begin, end)

    bool isArchive = false;      // a root unit with child units, laid out as expected
    std::string prolog;          // the bytes up to and including the root unit's start tag
    std::vector<Range> units;    // each child unit, from its start tag to the end of its end tag
};

// One pass over the bytes. Anything unexpected (no child units, a unit without an end tag,
// an end tag without a unit) leaves isArchive false, and the caller parses the whole input
// with one parser as before.
ArchiveLayout ScanArchive(const ArchiveBytes& bytes);

// One parser's input: the prolog, the given units in order, then the root's end tag,
// handed to libxml2 through its IO callback in whatever piece sizes it asks for.
class ChunkStream {
public:
    ChunkStream(const ArchiveBytes& bytes, const ArchiveLayout& layout, std::vector<ArchiveLayout::Range> units);

    static int Read(void* context, char* buffer, int length);
    static int Close(void* context);

private:
    struct Piece {
        const std::string* text = nullptr;   // a piece held in memory (prolog, end tag), or
        std::uint64_t begin = 0, end = 0;    // a byte range of the source
    };
    std::size_t ReadSome(char* buffer, std::size_t length);

    std::unique_ptr<ArchiveBytes::Reader> reader;
    std::string prolog;
    std::string epilog;
    std::vector<Piece> pieces;
    std::size_t piece = 0;        // the piece being read
    std::uint64_t offset = 0;     // position within it
};

#endif
