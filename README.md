# srcSlice

## Description

srcSlice is a fast, lightweight srcML tool for static slicing. srcSlice reads srcML
output files and produces variable slices in JSON, these slices contain details
such as where a variable is used and defined, passed into a function as an argument,
function of origin, class it is contained in, variables that are data-dependent
of it, and potential aliases.

## Table of Contents:
- [Building srcSlice](#building-srcslice)
- [Using srcSlice](#using-srcslice)
- [Options](#options)
- [Examples](#examples)
- [Output](#srcslice-output)

<br>

## Building srcSlice

### Prepare dependencies:
This is a srcML tool, if you do not have srcML installed you must install it.
You can download an installer [here](https://www.srcml.org/#download) or you can
```bash
# After running 'make' when building srcML, run 'make install' to ensure cmake can find the srcML package when building srcSlice
git clone --recursive https://github.com/srcML/srcML.git
```
This project uses the `nlohmann json` for handling JSON
```bash
# Linux
apt install nlohmann-json3-dev
sudo pacman -S nlohmann-json
# macOS
brew install nlohmann-json
```

### Preparing srcSlice:
```bash
# Clone srcSlice repository
git clone --recursive https://github.com/srcML/srcSlice.git

# (This guide assumes the cloned directory and build directory are at the same working directory)
mkdir build
cd build

# Run cmake to create the build files and run make to build the executable
cmake ../srcSlice

# You can run make but it will take longer
make srcslice
```

### Dev container:
The repository ships a [dev container](.devcontainer/) based on Ubuntu 26.04 with the
C++ toolchain, libxml2, nlohmann json, Boost headers and srcML 1.2.0 preinstalled
(srcML comes from the release tarballs, for amd64 and arm64). Open the folder in
VS Code and pick "Reopen in Container", or use the devcontainer CLI:
```bash
devcontainer up --workspace-folder .
devcontainer exec --workspace-folder . cmake --build build --target srcslice
```
Submodules are initialized and CMake is configured into `build/` when the container
is created. While the srcML release is still a GitHub draft, place the tarballs in
`.devcontainer/srcml/` first (see the README in that folder).

### Testing:
`ctest --test-dir build -L srcslice` runs the unit tests (Catch2). Besides the original
suites there are `testInterprocedural` (cross-unit call resolution, each program run in both
unit orders), `testDeterminism` (the same archive must give identical slices for 1, 2 and 8
threads and when repeated) and `testThreading` (the unit pipeline: every thread count and
queue size gives the single-threaded result, the backlog / in-flight / worker bounds hold, a
failing worker or a broken archive throws `std::runtime_error` out of the constructor). Cases
tagged `[!mayfail]` describe intended behaviour that a known defect still breaks; Catch
reports them without failing the suite, and the comment above each names the change expected
to fix it. `progress.md` logs each performance step with its gate results and numbers.

Sanitizer builds instrument srcSlice and the srcDispatch/srcSAX submodules. Each instrumented
compile of the handler peaks at about 1.3 GB, so keep the job count small on a desktop:
```bash
cmake -S . -B build-tsan -G Ninja -DSRCSLICE_SANITIZER=thread -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-tsan -j 8 && ctest --test-dir build-tsan -L srcslice --timeout 1800
```
(`address` and `undefined` work the same way; with `address` set `ASAN_OPTIONS=detect_leaks=0`,
libsrcml leaks.) `ctest -R regression` checks a real codebase against its recorded golden
output; see Benchmarks.

### Benchmarks:
`benchmark/` times `srcslice` on five inputs from four pinned codebases (srcSlice, PostgreSQL, OpenCV, the Linux kernel without and with `drivers/`)
and keeps versioned baselines so performance changes can be compared release to release.
From the dev container:
```bash
./benchmark/run.sh             # time the quick set (small, medium, medium-cpp), compare with the baseline, check outputs
./benchmark/run.sh --sizes all # include the kernel inputs (30+ min)
./benchmark/run.sh --verify    # only check the slice outputs against the recorded goldens
./benchmark/run.sh --history   # how every version compares
```
Each input has a recorded golden output in `benchmark/golden/`; a timing run and
`ctest -R regression` fail when a slice no longer matches it. See
[benchmark/README.md](benchmark/README.md) for the performance-update workflow.

<br>

## Using srcSlice
Input: A srcML file of source code with `--position` and `--hash` options.
srcML file can be a single unit (one source code file) or an archive (multiple source code files).
```text
srcml shell_sort.cpp -o shell_sort.cpp.xml --position --hash
```

Output: A JSON file containing the slice information about each variable contained in the input file.<br>
```text
./srcslice shell_sort.cpp.xml -o results.json
```

srcSlice supports multi-file slicing against a srcML archive:
```text
srcml src/ -o src.xml -r --position --hash
./srcslice src.xml -o results.json
```

<br>

## Options
```
srcSlice (srcML Slicing Tool)
Usage: srcslice [OPTIONS] input

Positionals:
  input srcML FILE REQUIRED   Name of the srcML input file [Must be built using the --position and --hash flags]

Options:
  -h,--help                   Print this help message and exit
  -o,--output TEXT            Name of the JSON output file [Stdout is Default]
  -t,--threads INT [7]        Number of concurrent threads [Default 7]
  -c,--control-edges          Display Control-Edges of the Slice
  -e,--expand-calls           Generate more details in cfunction entries
  -a,--expand-aliases         Expand Alias list
  -v,--verbose                Display Debug Info when Slicing
  -p,--progress               Display Feedback Progress Bars

Tuning:
  --backlog INT               Parsed units that may wait for a worker before the parser pauses [Default: threads]
  --in-flight INT             Started units that may wait to be merged in archive order [Default: 2 x threads]
```
The output does not depend on the thread count: units are merged in archive order, so `-t 1`
and `-t 32` produce the same bytes. The two tuning options bound how many parsed units and
finished-but-unmerged units are held in memory at once.

<br>

## Examples

Generate slices and write output to standard output<br>
```
./srcslice shell_sort.cpp.xml
```

Generate slices and write output to **slices.json**<br>
```
./srcslice shell_sort.cpp.xml -o slices.json
```

<br>

## srcSlice Output

<table>
  <thead>
    <tr>
      <th>Attribute</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><code>File</code></td>
      <td>File path of the source code the slice variable originates from</td>
    </tr>
    <tr>
      <td><code>Language</code></td>
      <td>Language of the source code file</td>
    </tr>
    <tr>
      <td><code>Namespace</code></td>
      <td>List of namespaces the slice variable is contained in</td>
    </tr>
    <tr>
      <td><code>Class</code></td>
      <td>Name of the class containing the slice variable</td>
    </tr>
    <tr>
      <td><code>Function</code></td>
      <td>Name of the function containing the slice variable</td>
    </tr>
    <tr>
      <td><code>Type</code></td>
      <td>Data type of the slice variable</td>
    </tr>
    <tr>
      <td><code>Name</code></td>
      <td>Variable name of the slice variable</td>
    </tr>
    <tr>
      <td><code>Decl</code></td>
      <td>Position of the slice variable's declaration</td>
    </tr>
    <tr>
      <td><code>Dependence</code></td>
      <td>
        A list of variable-position pairs, containing:
        <ul>
        <li>Name of Variable Data-Dependent of the Slice</li>
        <li>Position of the Dependence</li>
        </ul>
      </td>
    </tr>
    <tr>
      <td><code>Aliases</code></td>
      <td>
        A list of potential targets of a pointer or reference
      </td>
    </tr>
    <tr>
      <td><code>Calls</code></td>
      <td>
        A list of function calls where the slice variable is used as an argument.<br>
        This includes:
        <ul>
        <li>Function Name</li>
        <li>Argument Index</li>
        <li>Position of Function Definition</li>
        <li>End Position of the Function Block</li>
        <li>Position of Method Invokation</li>
        </ul>
      </td>
    </tr>
    <tr>
      <td><code>Use</code></td>
      <td>A set of positions where the slice variable is used</td>
    </tr>
    <tr>
      <td><code>Definition</code></td>
      <td>A set of positions where the slice variable is defined or redefined</td>
    </tr>
    <tr>
      <td><code>Control Edges</code></td>
      <td>A set of position pairs indicating the potential flow from a given starting point</td>
    </tr>
  </tbody>
</table>