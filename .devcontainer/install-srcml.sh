#!/usr/bin/env bash
# Install srcML (runtime + dev) from the release tarballs into /usr/local.
#
# Usage: install-srcml.sh [CACHE_DIR]
#
# For the architecture being built (TARGETARCH, else dpkg) two tarballs are
# needed: the runtime archive (bin/srcml, lib/libsrcml.so.1*) and the dev
# archive (include/srcml.h, lib/libsrcml.so, share/cmake/srcml/*). Each one is
# taken from CACHE_DIR/<basename of its URL> when that file exists, otherwise
# it is downloaded from the URL.
#
# Environment (set as Dockerfile ARGs):
#   SRCML_URL_AMD64, SRCML_URL_ARM64         runtime tarball per architecture
#   SRCML_DEV_URL_AMD64, SRCML_DEV_URL_ARM64 dev tarball per architecture
#   SRCML_PREFIX                             install prefix (default /usr/local)
set -euo pipefail

CACHE_DIR="${1:-/tmp/srcml-cache}"
PREFIX="${SRCML_PREFIX:-/usr/local}"
ARCH="${TARGETARCH:-$(dpkg --print-architecture)}"

case "$ARCH" in
    amd64) RUNTIME_URL="${SRCML_URL_AMD64:-}"; DEV_URL="${SRCML_DEV_URL_AMD64:-}" ;;
    arm64) RUNTIME_URL="${SRCML_URL_ARM64:-}"; DEV_URL="${SRCML_DEV_URL_ARM64:-}" ;;
    *)
        echo "install-srcml: unsupported architecture '$ARCH' (expected amd64 or arm64)" >&2
        exit 1
        ;;
esac

for var in RUNTIME_URL DEV_URL; do
    if [ -z "${!var}" ]; then
        echo "install-srcml: $var is not set for architecture $ARCH" >&2
        exit 1
    fi
done

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# fetch URL MARKER  -> prints the path of a verified tarball
# MARKER is an entry that must exist in the archive, so a wrong file fails early.
fetch() {
    local url="$1" marker="$2" name file
    name="$(basename "$url")"
    if [ -f "$CACHE_DIR/$name" ]; then
        echo "install-srcml: using local $CACHE_DIR/$name" >&2
        file="$CACHE_DIR/$name"
    else
        echo "install-srcml: downloading $url" >&2
        file="$WORK/$name"
        if ! curl -fsSL --retry 3 --retry-delay 2 -o "$file" "$url"; then
            cat >&2 <<EOF
install-srcml: failed to download $url

If this is a GitHub *draft* release its assets return 404 unless you are
logged in. Download the file in your browser and place it at

    .devcontainer/srcml/$name

then rebuild the container. Files in that folder take precedence over the URL.
EOF
            exit 1
        fi
    fi
    # plain grep (not -q) so tar is read to the end and pipefail stays clean
    if ! tar -tzf "$file" | grep -x "$marker" > /dev/null; then
        echo "install-srcml: $file is not the expected archive (no '$marker' entry)" >&2
        exit 1
    fi
    printf '%s\n' "$file"
}

RUNTIME_TGZ="$(fetch "$RUNTIME_URL" "bin/srcml")"
DEV_TGZ="$(fetch "$DEV_URL" "include/srcml.h")"

echo "install-srcml: extracting into $PREFIX" >&2
tar -xzf "$RUNTIME_TGZ" -C "$PREFIX" --no-same-owner --no-overwrite-dir
tar -xzf "$DEV_TGZ" -C "$PREFIX" --no-same-owner --no-overwrite-dir
ldconfig

test -f "$PREFIX/include/srcml.h"
test -f "$PREFIX/share/cmake/srcml/srcMLConfig.cmake"
test -x "$PREFIX/bin/srcml"
echo "install-srcml: installed $("$PREFIX/bin/srcml" --version | head -n 1) ($ARCH)" >&2
