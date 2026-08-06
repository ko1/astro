#!/bin/sh
# Build koruby_precise for CodeQL extraction: clean + ccache-disabled full build
# so the tracer sees every translation unit.  Invoked by `codeql database create
# --command="sh .../codeql/cqbuild.sh"` (a 2-token command avoids the --command
# whitespace-splitting trap).
cd "$(dirname "$0")/.." || exit 1
make clean
CCACHE_DISABLE=1 exec make -j8 koruby_precise
