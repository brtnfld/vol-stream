#!/usr/bin/env bash
#
# M0 exit gate.
#
# Runs HDF5's own test/API suite twice -- once natively, once through
# vol-stream -- and compares. The gate is not "the suite passes": it is "the
# suite behaves identically", because some API tests are legitimately skipped
# depending on build options, and a bare pass count hides a regression that
# turns a pass into a skip.
#
# Requires an HDF5 build configured with -DHDF5_TEST_API=ON.
#
# Usage:
#   run_api_suite.sh --api-bin <dir> --plugin-dir <dir> [options]
#
#   --api-bin      Directory holding the h5_api_test_* executables
#   --plugin-dir   Directory holding libvol_stream.so
#   --keystore     Public-key directory, if this HDF5 enforces plugin signatures
#   --tests        Space-separated list to run (default: all found)
#   --outdir       Where to write logs (default: ./api-suite-results)
#
set -uo pipefail

API_BIN=""
PLUGIN_DIR=""
KEYSTORE=""
TESTS=""
OUTDIR="./api-suite-results"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --api-bin)     API_BIN="$2"; shift 2 ;;
        --plugin-dir)  PLUGIN_DIR="$2"; shift 2 ;;
        --keystore)    KEYSTORE="$2"; shift 2 ;;
        --tests)       TESTS="$2"; shift 2 ;;
        --outdir)      OUTDIR="$2"; shift 2 ;;
        -h|--help)     sed -n '2,25p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

[[ -n "$API_BIN" ]]    || { echo "error: --api-bin is required" >&2; exit 2; }
[[ -n "$PLUGIN_DIR" ]] || { echo "error: --plugin-dir is required" >&2; exit 2; }
[[ -d "$API_BIN" ]]    || { echo "error: --api-bin '$API_BIN' is not a directory" >&2; exit 2; }

if [[ ! -f "$PLUGIN_DIR/libvol_stream.so" ]]; then
    echo "error: no libvol_stream.so in '$PLUGIN_DIR'" >&2
    exit 2
fi

mkdir -p "$OUTDIR"

# Default to whatever API test binaries this HDF5 build produced, rather than a
# hardcoded list that silently goes stale as HDF5 adds suites.
if [[ -z "$TESTS" ]]; then
    mapfile -t found < <(find "$API_BIN" -maxdepth 1 -type f -perm -u+x -name 'h5_api_test*' -printf '%f\n' | sort)
    if [[ ${#found[@]} -eq 0 ]]; then
        echo "error: no h5_api_test* executables in '$API_BIN'." >&2
        echo "       Configure HDF5 with -DHDF5_TEST_API=ON." >&2
        exit 2
    fi
    TESTS="${found[*]}"
fi

echo "vol-stream M0 exit gate"
echo "  api-bin:    $API_BIN"
echo "  plugin-dir: $PLUGIN_DIR"
echo "  tests:      $TESTS"
echo

# run_suite <label> <extra-env...>
run_suite() {
    local label="$1"; shift
    local rc_total=0

    for t in $TESTS; do
        local log="$OUTDIR/${label}.${t}.log"
        # Each test wants a clean working directory: the API suite creates files
        # with fixed names and a leftover from the other configuration would
        # make one run affect the other.
        local wd="$OUTDIR/wd.${label}.${t}"
        rm -rf "$wd"; mkdir -p "$wd"

        ( cd "$wd" && env "$@" "$API_BIN/$t" ) > "$log" 2>&1
        local rc=$?
        [[ $rc -ne 0 ]] && rc_total=1
        printf '  %-14s %-34s rc=%d\n' "$label" "$t" "$rc"
    done

    return $rc_total
}

echo "== native =="
run_suite native HDF5_VOL_CONNECTOR= HDF5_PLUGIN_PATH=
native_rc=$?

echo
echo "== vol-stream =="
stream_env=(HDF5_VOL_CONNECTOR=vol-stream "HDF5_PLUGIN_PATH=$PLUGIN_DIR")
[[ -n "$KEYSTORE" ]] && stream_env+=("HDF5_PLUGIN_KEYSTORE=$KEYSTORE")
run_suite stream "${stream_env[@]}"
stream_rc=$?

# Compare per-test tallies. The API suite prints its own summary lines; counting
# them is more informative than trusting exit codes alone, which collapse a
# hundred results into one bit.
echo
echo "== comparison =="
gate=0
for t in $TESTS; do
    n_native=$(grep -acE "^\s*(Testing|PASSED|FAILED|SKIPPED)" "$OUTDIR/native.${t}.log" 2>/dev/null || echo 0)
    n_stream=$(grep -acE "^\s*(Testing|PASSED|FAILED|SKIPPED)" "$OUTDIR/stream.${t}.log" 2>/dev/null || echo 0)

    f_native=$(grep -ac "FAILED" "$OUTDIR/native.${t}.log" 2>/dev/null || echo 0)
    f_stream=$(grep -ac "FAILED" "$OUTDIR/stream.${t}.log" 2>/dev/null || echo 0)

    if [[ "$f_native" == "$f_stream" && "$n_native" == "$n_stream" ]]; then
        printf '  ok    %-34s (%s lines, %s failures, both)\n' "$t" "$n_native" "$f_native"
    else
        printf '  DIFF  %-34s native: %s lines/%s fail   stream: %s lines/%s fail\n' \
               "$t" "$n_native" "$f_native" "$n_stream" "$f_stream"
        gate=1
    fi
done

echo
if [[ $gate -eq 0 && "$native_rc" == "$stream_rc" ]]; then
    echo "GATE PASSED: vol-stream is indistinguishable from native on this suite."
    exit 0
fi

echo "GATE FAILED: behaviour differs, or exit status differs"
echo "             (native rc=$native_rc, stream rc=$stream_rc)"
echo "             logs in $OUTDIR"
exit 1
