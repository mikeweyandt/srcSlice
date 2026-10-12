// SPDX-License-Identifier: GPL-3.0-only
/**
 * @file main.cpp
 *
 * @copyright Copyright (C) 2018-2024 srcML, LLC. (www.srcML.org)
 *
 * This file is part of the srcSlice application.
 */

#include <srcslicehandler.hpp>
#include <vector>
#include <algorithm>
#include <thread>

// determine default max threads
int defaultThreads() {
    int processorCount = CPUCount();
    if (processorCount < 2)
        return 2;
    else if (processorCount == 2)
        return 3;
    else
        return processorCount + 2;
}

int main(int argc, char **argv)
{
    // search for the output flag and set a marker
    std::string inputFile = "", outputFile = "";
    int threadCount = defaultThreads();
    int backlogCap = 0, maxInFlight = 0, parserCount = 0;
    std::ofstream outFile;
    bool verboseMode = false, feedbackMode = false,
        showControlEdges = false, expandCalls = false,
        expandAliases = false;

    std::ostringstream ss;
    ss << "Number of concurrent threads [Default " << threadCount << "]";

    CLI::App app{"srcSlice (srcML Slicing Tool)"};
    // Options hold extra data
    app.add_option  ("input",   inputFile,                          "Name of the srcML input file [Must be built using the --position and --hash flags]")
        ->required()
        ->type_name("srcML FILE");
    app.add_option  ("-o, --output",  outputFile,                   "Name of the JSON output file [Stdout is Default]");
    app.add_option  ("-t, --threads", threadCount,                  ss.str())
        ->default_val(threadCount);
    
    app.add_flag    ("-c, --control-edges", showControlEdges,       "Display Control-Edges of the Slice");
    app.add_flag    ("-e, --expand-calls", expandCalls,             "Generate more details in cfunction entries");
    app.add_flag    ("-a, --expand-aliases", expandAliases,         "Expand Alias list");
    app.add_flag    ("-v, --verbose", verboseMode,                  "Display Debug Info when Slicing");
    app.add_flag    ("-p, --progress", feedbackMode,                "Display Feedback Progress Bars");
    app.add_option  ("--backlog", backlogCap,                       "Parsed units that may wait for a worker before the parser pauses [Default: threads]")
        ->group("Tuning");
    app.add_option  ("--parsers", parserCount,                      "Parsers reading the archive at once, each a strided share of its units [Default: threads / 2]")
        ->check(CLI::NonNegativeNumber);
    app.add_option  ("--in-flight", maxInFlight,                    "Started units that may wait to be merged in archive order [Default: 2 x threads]")
        ->group("Tuning");
    
    CLI11_PARSE(app, argc, argv);

    CliInfo info = {
        inputFile, outputFile,
        threadCount,
        showControlEdges,
        expandCalls,
        expandAliases,
        verboseMode,
        feedbackMode,
        backlogCap,
        maxInFlight,
        parserCount
    };

    try {
        // check if input file exists
        std::ifstream srcmlFile(inputFile);
        if (!srcmlFile.is_open()) {
            throw std::runtime_error("Input file does not exist");
        }
        srcmlFile.close();

        if (showControlEdges) {
            std::cout << "[\033[33mWARNING\033[0m] Computing control-edges may increase the time required to complete the program\n";
        }

        if (expandAliases) {
            std::cout << "[\033[33mWARNING\033[0m] Expanding Pointer Aliasing may increase the time required to complete the program\n";
        }

        // Deliberately never deleted: freeing the hundreds of thousands of slices one by one
        // at exit took 2 % of a medium run and seconds on the kernel, for memory the process
        // is about to give back anyway. Every thread has been joined by the time the
        // constructor returns, so nothing depends on the destructor.
        SrcSliceHandler* srcSliceHandler = new SrcSliceHandler(info);

        auto& sliceProfileMap = srcSliceHandler->GetProfileMap();

        if (!outputFile.empty()) {
            // allow output file to be written in
            outFile = std::ofstream(outputFile);
            std::cout << "Writing to :: " << outputFile << "\n";
        } else {
            // close output file
            outFile.close();
        }

        // stream every slice straight to its destination: building the whole document in
        // memory first doubled the peak RSS (perf plan M6). Same bytes as before: entries
        // separated by ",\n", the document closed with "\n}".
        std::ostream& sliceOutput = outputFile.empty() ? static_cast<std::ostream&>(std::cout) : outFile;

        // opening of the entire JSON object
        sliceOutput << "{\n";

        // The entries, in the order they are written. They are formatted in chunks on
        // several threads, one wave of chunks at a time, while the previous wave is written
        // in order: the same bytes as one thread writing entry by entry, and at most two
        // waves of buffers in memory (perf plan, output phase).
        std::vector<SliceProfile*> slices;
        for (auto& profiles : sliceProfileMap) {
            for (auto& slice : profiles.second) {
                if (!slice.containsDeclaration)
                    continue;
                // if we need more call details in the call entries
                // we need to set the respective bool true
                slice.expandCalls = expandCalls;
                slices.push_back(&slice);
            }
        }
        // Chunks hold about 2 MB of output each (estimated from the entry's set sizes, so an
        // entry with a million positions is a chunk of its own), a wave is eight chunks, and a
        // buffer that an oversized entry inflated is released after its wave: the formatting
        // is fast, the point is not to hold much of the document at once.
        const std::size_t chunkBytes = 2u << 20;
        const std::size_t waveChunks = static_cast<std::size_t>(std::clamp(threadCount, 1, 8));
        std::vector<std::size_t> chunkStarts;   // first entry of each chunk, then slices.size()
        {
            std::size_t estimate = 0;
            for (std::size_t i = 0; i < slices.size(); ++i) {
                if (i == 0 || estimate >= chunkBytes) {
                    chunkStarts.push_back(i);
                    estimate = 0;
                }
                const SliceProfile& slice = *slices[i];
                estimate += 400 + 40 * (slice.uses.size() + slice.definitions.size() + slice.controlEdges.size())
                                + 60 * (slice.dvars.size() + slice.aliases.size()) + 160 * slice.cfunctions.size();
            }
            chunkStarts.push_back(slices.size());
        }
        const std::size_t chunks = chunkStarts.size() - 1;
        auto formatChunk = [&slices, &chunkStarts](std::size_t chunk, std::string& out) {
            out.clear();
            for (std::size_t i = chunkStarts[chunk]; i < chunkStarts[chunk + 1]; ++i) {
                if (i > 0) out += ",\n";
                out += '"';
                out += slices[i]->jsonKey();
                out += "\":{\n";
                slices[i]->AppendJson(out);
                out += '}';
            }
        };
        auto formatWave = [&](std::size_t firstChunk, std::vector<std::string>& buffers) {
            const std::size_t count = std::min(waveChunks, chunks - std::min(chunks, firstChunk));
            buffers.resize(count);
            std::vector<std::thread> pool;
            for (std::size_t k = 1; k < count; ++k) {
                pool.emplace_back([&, k]() { formatChunk(firstChunk + k, buffers[k]); });
            }
            if (count > 0) formatChunk(firstChunk, buffers[0]);
            for (std::thread& thread : pool) thread.join();
        };
        std::vector<std::string> current, next;
        formatWave(0, current);
        for (std::size_t firstChunk = 0; firstChunk < chunks; firstChunk += waveChunks) {
            // the next wave is formatted while this one is written
            std::thread ahead;
            if (firstChunk + waveChunks < chunks) {
                ahead = std::thread([&]() { formatWave(firstChunk + waveChunks, next); });
            }
            for (std::string& buffer : current) {
                sliceOutput.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                if (buffer.capacity() > 4 * chunkBytes) std::string().swap(buffer);
            }
            if (ahead.joinable()) ahead.join();
            current.swap(next);
        }

        // closing of the entire JSON object
        sliceOutput << "\n}";
        sliceOutput.flush();
    } catch (const std::exception& e) {
        std::cout << "\033[31m" << e.what() << "\033[0m" << "\n";
        return 3;
    }

    // ensure if we do have an output file we close it
    // alert user that an output file has been created
    if (!outputFile.empty()) {
        std::cout << "Output Saved to :: " << outputFile << "\n";
        outFile.close();
    }
}