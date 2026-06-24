#!/bin/bash
# One-click build & run for the HDR accuracy self-test.
# Compiles hdr.c together with the test driver and runs it.
# Usage: ./run_hdr_test.sh [seed] [n_samples]

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HDR_DIR="$SCRIPT_DIR/../src/urpc/umq/dfx"
TEST_SRC="$SCRIPT_DIR/hdr_accuracy_test.c"
BIN="$SCRIPT_DIR/hdr_accuracy_test"

CC="${CC:-gcc}"
CFLAGS="${CFLAGS:--O2 -Wall -Wextra -std=c11}"

echo "=== [1/3] build ==="
echo "CC=$CC  HDR_DIR=$HDR_DIR"
"$CC" $CFLAGS -I"$HDR_DIR" "$TEST_SRC" "$HDR_DIR/hdr.c" -o "$BIN" -lm
build_rc=$?
if [ $build_rc -ne 0 ]; then
    echo "build FAILED (rc=$build_rc)"
    exit 2
fi
echo "build OK -> $BIN"

echo "=== [2/3] run ==="
"$BIN" "$@"
run_rc=$?

echo "=== [3/3] done (rc=$run_rc) ==="
exit $run_rc
