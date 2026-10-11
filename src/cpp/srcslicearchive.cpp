// SPDX-License-Identifier: GPL-3.0-only
/**
 * @file srcslicearchive.cpp
 *
 * @copyright Copyright (C) 2018-2024 srcML, LLC. (www.srcML.org)
 *
 * This file is part of the srcSlice application.
 */

#include "srcslicearchive.hpp"

#include <algorithm>
#include <cstring>

namespace {
    class FileReader : public ArchiveBytes::Reader {
    public:
        explicit FileReader(const std::string& path) : in(path, std::ios::binary) {}
        bool IsOpen() const { return in.is_open(); }
        std::size_t ReadAt(std::uint64_t offset, char* buffer, std::size_t count) override {
            if (!in.is_open()) return 0;
            in.clear();
            in.seekg(static_cast<std::streamoff>(offset));
            if (!in) return 0;
            in.read(buffer, static_cast<std::streamsize>(count));
            return static_cast<std::size_t>(in.gcount());
        }
    private:
        std::ifstream in;
    };

    class TextReader : public ArchiveBytes::Reader {
    public:
        explicit TextReader(const std::string& text) : text(text) {}
        std::size_t ReadAt(std::uint64_t offset, char* buffer, std::size_t count) override {
            if (offset >= text.size()) return 0;
            const std::size_t n = std::min<std::size_t>(count, text.size() - static_cast<std::size_t>(offset));
            std::memcpy(buffer, text.data() + offset, n);
            return n;
        }
    private:
        const std::string& text;
    };

    bool IsTagDelimiter(char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '>' || c == '/';
    }
}

std::unique_ptr<ArchiveBytes::Reader> ArchiveFile::OpenReader() const {
    auto reader = std::make_unique<FileReader>(path);
    if (!reader->IsOpen()) return nullptr;
    return reader;
}

std::unique_ptr<ArchiveBytes::Reader> ArchiveText::OpenReader() const {
    return std::make_unique<TextReader>(text);
}

ArchiveLayout ScanArchive(const ArchiveBytes& bytes) {
    ArchiveLayout layout;
    std::unique_ptr<ArchiveBytes::Reader> reader = bytes.OpenReader();
    if (!reader) return layout;

    struct StartTag { std::uint64_t begin, afterTag; bool selfClosing; };
    std::vector<StartTag> starts;
    std::vector<std::uint64_t> ends;   // position just after each "</unit>"

    // the bytes are scanned in blocks; a tag that straddles a block boundary is carried over
    const std::size_t blockSize = 8u << 20;
    std::string window;
    std::uint64_t windowBase = 0;      // absolute offset of window[0]
    std::vector<char> block(blockSize);
    bool atEnd = false;

    while (!atEnd) {
        const std::size_t got = reader->ReadAt(windowBase + window.size(), block.data(), blockSize);
        if (got == 0) atEnd = true;
        window.append(block.data(), got);

        std::size_t pos = 0;
        std::size_t keepFrom = window.size();   // the first byte that must stay for the next block
        while (pos < window.size()) {
            const char* found = static_cast<const char*>(std::memchr(window.data() + pos, '<', window.size() - pos));
            if (!found) break;
            const std::size_t at = static_cast<std::size_t>(found - window.data());
            if (window.size() - at < 8 && !atEnd) { keepFrom = at; break; }   // "<unit " or "</unit>" may straddle

            if (window.compare(at, 7, "</unit>") == 0) {
                ends.push_back(windowBase + at + 7);
                pos = at + 7;
                continue;
            }
            if (window.compare(at, 5, "<unit") == 0 && at + 5 < window.size() && IsTagDelimiter(window[at + 5])) {
                // the end of the start tag: '>' outside quotes
                char quote = 0;
                std::size_t p = at + 5;
                for (; p < window.size(); ++p) {
                    const char c = window[p];
                    if (quote) { if (c == quote) quote = 0; }
                    else if (c == '"' || c == '\'') quote = c;
                    else if (c == '>') break;
                }
                if (p >= window.size()) {
                    if (atEnd) { pos = window.size(); break; }   // unterminated: not an archive we understand
                    keepFrom = at;
                    break;
                }
                starts.push_back(StartTag{windowBase + at, windowBase + p + 1, window[p - 1] == '/'});
                pos = p + 1;
                continue;
            }
            pos = at + 1;
        }

        if (keepFrom < window.size()) {
            window.erase(0, keepFrom);
            windowBase += keepFrom;
        } else {
            windowBase += window.size();
            window.clear();
        }
    }

    // the first start tag is the root; every other one is a child unit
    if (starts.size() < 2 || starts[0].selfClosing) return layout;
    const StartTag& root = starts[0];

    std::size_t endIndex = 0;
    for (std::size_t i = 1; i < starts.size(); ++i) {
        const StartTag& start = starts[i];
        if (start.selfClosing) {
            layout.units.emplace_back(start.begin, start.afterTag);
            continue;
        }
        // the child's end tag is the next one after its start, and the next child starts after it
        while (endIndex < ends.size() && ends[endIndex] <= start.begin) ++endIndex;
        if (endIndex >= ends.size()) return layout;
        const std::uint64_t end = ends[endIndex++];
        if (i + 1 < starts.size() && starts[i + 1].begin < end) return layout;   // nested units: not handled
        layout.units.emplace_back(start.begin, end);
    }
    // what remains must be the root's own end tag, after the last child
    if (endIndex + 1 != ends.size()) return layout;
    if (!layout.units.empty() && ends.back() < layout.units.back().second) return layout;

    // the prolog: everything up to and including the root's start tag
    layout.prolog.resize(static_cast<std::size_t>(root.afterTag));
    std::unique_ptr<ArchiveBytes::Reader> prologReader = bytes.OpenReader();
    if (!prologReader) return layout;
    std::size_t read = 0;
    while (read < layout.prolog.size()) {
        const std::size_t n = prologReader->ReadAt(read, &layout.prolog[read], layout.prolog.size() - read);
        if (n == 0) return layout;
        read += n;
    }

    layout.isArchive = true;
    return layout;
}

ChunkStream::ChunkStream(const ArchiveBytes& bytes, const ArchiveLayout& layout, std::vector<ArchiveLayout::Range> units)
    : reader(bytes.OpenReader()), prolog(layout.prolog), epilog("\n</unit>\n") {
    pieces.push_back(Piece{&prolog, 0, 0});
    for (const ArchiveLayout::Range& unit : units) {
        pieces.push_back(Piece{nullptr, unit.first, unit.second});
        pieces.push_back(Piece{&epilog, 0, 1});   // a newline between units, as in the archive
    }
    // the last piece: the root's end tag (replaces the trailing newline piece)
    if (!units.empty()) pieces.pop_back();
    pieces.push_back(Piece{&epilog, 0, 0});
}

std::size_t ChunkStream::ReadSome(char* buffer, std::size_t length) {
    std::size_t total = 0;
    while (total < length && piece < pieces.size()) {
        const Piece& current = pieces[piece];
        std::size_t n = 0;
        if (current.text) {
            // a text piece: [begin, end) within it, or the whole text when end == 0
            const std::uint64_t pieceEnd = current.end ? current.end : current.text->size();
            const std::uint64_t at = current.begin + offset;
            if (at < pieceEnd) {
                n = static_cast<std::size_t>(std::min<std::uint64_t>(length - total, pieceEnd - at));
                std::memcpy(buffer + total, current.text->data() + at, n);
            }
        } else if (reader) {
            const std::uint64_t at = current.begin + offset;
            if (at < current.end) {
                n = reader->ReadAt(at, buffer + total, static_cast<std::size_t>(std::min<std::uint64_t>(length - total, current.end - at)));
            }
        }
        if (n == 0) {
            ++piece;
            offset = 0;
            continue;
        }
        total += n;
        offset += n;
    }
    return total;
}

int ChunkStream::Read(void* context, char* buffer, int length) {
    if (!context || length <= 0) return 0;
    return static_cast<int>(static_cast<ChunkStream*>(context)->ReadSome(buffer, static_cast<std::size_t>(length)));
}

int ChunkStream::Close(void* /*context*/) {
    return 0;
}
