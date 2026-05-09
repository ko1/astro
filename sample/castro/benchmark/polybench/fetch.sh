#!/bin/bash
# Fetch the original PolyBench-C 4.2.1 source.
# Sets up under upstream/ for the adapter script to read from.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

if [ -d upstream ]; then
    echo "upstream/ already present — skip fetch"
    exit 0
fi

echo "Fetching PolyBench-C 4.2.1 from MatthiasJReisinger/PolyBenchC-4.2.1..."
TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
gh api repos/MatthiasJReisinger/PolyBenchC-4.2.1/tarball/master > "$TMP/pb.tgz"
tar xzf "$TMP/pb.tgz" -C "$TMP"
SRC=$(ls -d "$TMP"/MatthiasJReisinger-PolyBenchC-* | head -1)
mv "$SRC" upstream
echo "OK: upstream/ set up ($(find upstream -name '*.c' | wc -l) .c files)"
