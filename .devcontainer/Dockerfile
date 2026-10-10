# Dev container for srcSlice: Ubuntu 26.04 C++ toolchain + srcML 1.2.0.
# srcML is installed from the official release tarballs (see install-srcml.sh);
# the Ubuntu 26.04 build is required because libsrcml links libxml2.so.16.
FROM mcr.microsoft.com/devcontainers/cpp:ubuntu26.04
USER root

# Set automatically by BuildKit/buildah (amd64 | arm64); the install script
# falls back to `dpkg --print-architecture` when it is empty.
ARG TARGETARCH

# srcML release tarballs. "dev" = header + CMake package config + libsrcml.so
# symlink; the runtime archive = libsrcml.so.1.* + the srcml CLI. Both are
# needed. Override these via devcontainer.json "build.args" (or --build-arg)
# when the release moves, e.g. SRCML_BASE_URL=.../releases/download/v1.2.0
ARG SRCML_BASE_URL=https://github.com/srcML/srcML/releases/download/untagged-b0663745d1d69d837151
ARG SRCML_DEV_URL_AMD64=${SRCML_BASE_URL}/srcml-dev_1.2.0-1_ubuntu26.04_amd64.tar.gz
ARG SRCML_DEV_URL_ARM64=${SRCML_BASE_URL}/srcml-dev_1.2.0-1_ubuntu26.04_arm64.tar.gz
ARG SRCML_URL_AMD64=${SRCML_BASE_URL}/srcml_1.2.0-1_ubuntu26.04_amd64.tar.gz
ARG SRCML_URL_ARM64=${SRCML_BASE_URL}/srcml_1.2.0-1_ubuntu26.04_arm64.tar.gz

# Build and runtime dependencies for srcSlice, srcDispatch/srcSAX and srcML:
#   libxml2-dev        srcSlice, srcSAX, libsrcml (pulls libxml2-16)
#   libxslt1.1         libsrcml (libxslt.so.1, libexslt.so.0)
#   libarchive13t64    srcml CLI
#   libcurl4t64        srcml CLI
#   nlohmann-json3-dev srcSlice tests
#   libboost-dev       srcDispatch tests (header-only)
RUN apt-get update && export DEBIAN_FRONTEND=noninteractive \
    && apt-get -y install --no-install-recommends \
        ninja-build \
        libxml2-dev \
        libxslt1.1 \
        libarchive13t64 \
        libcurl4t64 \
        nlohmann-json3-dev \
        libboost-dev \
        ca-certificates \
        curl \
    && apt-get clean -y && rm -rf /var/lib/apt/lists/*

# Optional pre-downloaded tarballs. The folder is tracked in git (README +
# .gitignore) so this COPY always succeeds; *.tar.gz inside it is ignored.
COPY srcml/ /tmp/srcml-cache/
COPY install-srcml.sh /tmp/install-srcml.sh
RUN chmod +x /tmp/install-srcml.sh \
    && /tmp/install-srcml.sh /tmp/srcml-cache \
    && rm -rf /tmp/srcml-cache /tmp/install-srcml.sh
