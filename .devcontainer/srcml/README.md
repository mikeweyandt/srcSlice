# Pre-downloaded srcML tarballs

The dev container installs srcML from the GitHub release tarballs named in
`../Dockerfile`. While that release is still a **draft**, GitHub answers the
download URLs with 404 unless you are logged in, so the container build cannot
fetch them on its own.

Drop the tarballs in this folder and rebuild the container. Any file here whose
name matches the URL's filename is used instead of downloading. Only the pair
for the architecture you build on is needed:

| Architecture | Files |
|--------------|-------|
| amd64 (x86_64) | `srcml_1.2.0-1_ubuntu26.04_amd64.tar.gz`, `srcml-dev_1.2.0-1_ubuntu26.04_amd64.tar.gz` |
| arm64 (aarch64) | `srcml_1.2.0-1_ubuntu26.04_arm64.tar.gz`, `srcml-dev_1.2.0-1_ubuntu26.04_arm64.tar.gz` |

Both the runtime (`srcml_…`) and the dev (`srcml-dev_…`) archive are required:
the runtime one carries `libsrcml.so.1` and the `srcml` CLI, the dev one carries
`srcml.h` and the CMake package config that `find_package(srcML)` needs.

`*.tar.gz` in this folder is gitignored. Once the release is published the
folder can stay empty and the build downloads the files itself.
