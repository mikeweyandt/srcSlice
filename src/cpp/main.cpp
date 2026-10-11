// SPDX-License-Identifier: GPL-3.0-only
/**
 * @file main.cpp
 *
 * @copyright Copyright (C) 2018-2024 srcML, LLC. (www.srcML.org)
 *
 * This file is part of the srcSlice application.
 */

#include <srcslicehandler.hpp>

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
    int backlogCap = 0, maxInFlight = 0;
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
        maxInFlight
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

        SrcSliceHandler srcSliceHandler(info);

        auto& sliceProfileMap = srcSliceHandler.GetProfileMap();

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
        bool writtenSlices = false;

        for (auto& profiles : sliceProfileMap) {
            for (auto& slice : profiles.second) {
                if (!slice.containsDeclaration)
                    continue;

                // if we need more call details in the call entries
                // we need to set the respective bool true
                slice.expandCalls = expandCalls;

                if (writtenSlices) sliceOutput << ",\n";
                writtenSlices = true;

                sliceOutput << "\"" << slice.jsonKey() << "\":{\n";
                sliceOutput << slice;
                sliceOutput << "}";
            }
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