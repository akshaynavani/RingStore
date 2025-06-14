#!/usr/bin/env bash
# Runs the standard RingStore benchmark matrix and writes results/sweep.csv.
#
#   scripts/run_sweep.sh /mnt/nvme/ringstore.dat 4g
#
# Environment overrides: BUILD_DIR, TIME, WARMUP, QDS, BSS, EXTRA_FLAGS.
set -euo pipefail

TARGET=${1:?usage: run_sweep.sh <file-or-device> [size]}
SIZE=${2:-1g}
BUILD_DIR=${BUILD_DIR:-build}
TIME=${TIME:-10}
WARMUP=${WARMUP:-1}
QDS=${QDS:-1,2,4,8,16,32,64,128}
BSS=${BSS:-4k,16k,64k,128k}
EXTRA_FLAGS=${EXTRA_FLAGS:-}

BENCH="$BUILD_DIR/ringstore_bench"
if [[ ! -x "$BENCH" ]]; then
  echo "benchmark binary not found at $BENCH; build first (cmake --build $BUILD_DIR)" >&2
  exit 1
fi

mkdir -p results
OUT=results/sweep.csv
rm -f "$OUT"

for rw in randread randwrite randrw:70; do
  # shellcheck disable=SC2086
  "$BENCH" --file "$TARGET" --size "$SIZE" --bs "$BSS" --qd "$QDS" --rw "$rw" \
    --time "$TIME" --warmup "$WARMUP" --engine both --csv "$OUT" $EXTRA_FLAGS
done

# One extra pass showing the effect of registered buffers + files.
# shellcheck disable=SC2086
"$BENCH" --file "$TARGET" --size "$SIZE" --bs 4k --qd "$QDS" --rw randread \
  --time "$TIME" --warmup "$WARMUP" --engine uring --fixed-bufs --fixed-files \
  --no-prepare --csv "$OUT" $EXTRA_FLAGS

echo "results written to $OUT"
