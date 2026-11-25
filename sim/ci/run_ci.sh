#!/bin/sh
# Simulator CI entry: build + config drift + staged parity + per-scenario
# deterministic sessions (each scenario gets a fresh sim process and a
# fresh NVS directory). Scenario fixtures use the S3 368x448 structural
# baseline; set SIM_PRODUCT in the environment to run another product.
#
# Usage: sim/ci/run_ci.sh [build-dir] [--scenario NAME]... [--log-dir DIR]
#        [--no-build] [--jobs N] [--keep-going]
set -e

BUILD="build/sim"
SCENARIOS=""
LOG_DIR=""
NO_BUILD=0
JOBS=1
KEEP_GOING=0

usage() {
    cat <<'EOF'
usage: sim/ci/run_ci.sh [build-dir] [--scenario NAME]... [--log-dir DIR]
                        [--no-build] [--jobs N] [--keep-going]

  --no-build      reuse an existing build (CMake cache, binary and resources)
  --jobs N        run up to N scenarios concurrently (default: 1)
  --keep-going    continue after a failed scenario/assertion for diagnosis
                  (default: stop after the first failed scenario)
  --help          show this help
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --scenario)
            [ $# -ge 2 ] || { echo "ERROR: --scenario needs a name" >&2; exit 2; }
            SCENARIOS="$SCENARIOS $2"
            shift 2
            ;;
        --log-dir)
            [ $# -ge 2 ] || { echo "ERROR: --log-dir needs a path" >&2; exit 2; }
            LOG_DIR="$2"
            shift 2
            ;;
        --no-build)
            NO_BUILD=1
            shift
            ;;
        --jobs)
            [ $# -ge 2 ] || { echo "ERROR: --jobs needs a number" >&2; exit 2; }
            JOBS="$2"
            shift 2
            ;;
        --keep-going)
            KEEP_GOING=1
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        --*)
            echo "ERROR: unknown option $1" >&2
            exit 2
            ;;
        *)
            BUILD="$1"
            shift
            ;;
    esac
done

case "$JOBS" in
    ''|*[!0-9]*)
        echo "ERROR: --jobs must be an integer between 1 and 8" >&2
        exit 2
        ;;
esac
if [ "$JOBS" -lt 1 ] || [ "$JOBS" -gt 8 ]; then
    echo "ERROR: --jobs must be between 1 and 8" >&2
    exit 2
fi

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
if [ ! -d "$BUILD" ]; then
    if [ "$NO_BUILD" -eq 1 ]; then
        echo "ERROR: build directory not found: $BUILD" >&2
        exit 2
    fi
    mkdir -p "$BUILD" || {
        echo "ERROR: cannot create build directory: $BUILD" >&2
        exit 2
    }
fi
BUILD="$(cd "$BUILD" && pwd)"

if [ -z "$LOG_DIR" ]; then
    LOG_DIR="$ROOT/build/agent/sim/$(date +%Y%m%d-%H%M%S)-$$"
fi
mkdir -p "$LOG_DIR"
LOG_DIR="$(cd "$LOG_DIR" && pwd)"
echo "logs: $LOG_DIR"

SCENARIO_FILES=""
if [ -n "$SCENARIOS" ]; then
    for name in $SCENARIOS; do
        case "$name" in
            */*|"")
                echo "ERROR: invalid scenario name: $name" >&2
                exit 2
                ;;
            *.json)
                name="${name%.json}"
                ;;
        esac
        scenario="$ROOT/sim/ci/scenarios/$name.json"
        [ -f "$scenario" ] || {
            echo "ERROR: unknown scenario: $name" >&2
            exit 2
        }
        SCENARIO_FILES="$SCENARIO_FILES $scenario"
    done
fi

# Serialize only CMake/Ninja mutations; scenario processes use private data and ports.
DEVICE_PRODUCT="$(sed -n 's/^#define MT_PRODUCT_NAME "\([^"]*\)"/\1/p' "$ROOT/build/mt_product_profile.h" 2>/dev/null | head -n 1)"
SIM_PRODUCT="${SIM_PRODUCT:-s3_reference}"

if [ "$NO_BUILD" -eq 1 ]; then
    [ -x "$BUILD/microtech_sim" ] || {
        echo "ERROR: simulator binary not found: $BUILD/microtech_sim" >&2
        exit 2
    }
    [ -d "$BUILD/sim_res_fs" ] || {
        echo "ERROR: simulator resources not found: $BUILD/sim_res_fs" >&2
        exit 2
    }
    [ -f "$BUILD/gen_inc/sdkconfig.h" ] || {
        echo "ERROR: simulator config not found: $BUILD/gen_inc/sdkconfig.h" >&2
        exit 2
    }
    cached_product="$(sed -n 's/^SIM_PRODUCT:STRING=//p' "$BUILD/CMakeCache.txt" 2>/dev/null || true)"
    [ "$cached_product" = "$SIM_PRODUCT" ] || {
        echo "ERROR: $BUILD is configured for '$cached_product', expected '$SIM_PRODUCT'; rebuild without --no-build" >&2
        exit 2
    }
else
    exec 9>"$BUILD/.sim-ci-build.lock"
    flock 9
    cmake -S "$ROOT/sim" -B "$BUILD" -G Ninja \
        -DSIM_PRODUCT="$SIM_PRODUCT" >/dev/null
    if ! cmake --build "$BUILD" -- --quiet; then
        echo "ERROR: simulator build failed (logs: $LOG_DIR)" >&2
        exit 1
    fi
    flock -u 9
fi

python3 "$ROOT/sim/ci/check_lv_conf.py" --mirror "$BUILD/gen_inc/sdkconfig.h"
python3 "$ROOT/sim/tools/build_context.py" \
    --build "$BUILD" --artifact "$BUILD/microtech_sim" \
    --out "$LOG_DIR/build-context.json" >/dev/null
# Keep the context beside the artifact for `mt.py sim --no-build`.
if [ "$LOG_DIR" != "$BUILD" ]; then
    cp "$LOG_DIR/build-context.json" "$BUILD/build-context.json"
fi

RUN_DIR="$(mktemp -d "${TMPDIR:-/tmp}/mt-ci.XXXXXX")"
cleanup() {
    rm -rf "$RUN_DIR"
}
trap cleanup EXIT INT TERM

SIM_PRODUCT_CACHE="$(sed -n 's/^SIM_PRODUCT:STRING=//p' "$BUILD/CMakeCache.txt" 2>/dev/null || true)"
if [ -d "$ROOT/build/esp-idf/main/app_res_fs" ] &&
        [ -n "$DEVICE_PRODUCT" ] &&
        [ "$SIM_PRODUCT_CACHE" = "$DEVICE_PRODUCT" ]; then
    diff -rq "$BUILD/sim_res_fs" "$ROOT/build/esp-idf/main/app_res_fs" \
        || { echo "ERROR: staged resources differ from device staging" >&2; exit 1; }
else
    echo "note: staging parity skipped for simulator=${SIM_PRODUCT_CACHE:-unknown} device=${DEVICE_PRODUCT:-unknown}"
fi

run_one() {
    scenario="$1"
    keep_flag=""
    if [ "$KEEP_GOING" -eq 1 ]; then
        keep_flag="--keep-going"
    fi
    sh "$ROOT/sim/ci/run_scenario.sh" \
        --build "$BUILD" --log-dir "$LOG_DIR" --run-dir "$RUN_DIR" \
        $keep_flag "$scenario"
}

failed=0
if [ -z "$SCENARIO_FILES" ]; then
    SCENARIO_FILES="$ROOT"/sim/ci/scenarios/*.json
fi

if [ "$JOBS" -eq 1 ]; then
    for scenario in $SCENARIO_FILES; do
        if ! run_one "$scenario"; then
            failed=1
            if [ "$KEEP_GOING" -eq 0 ]; then
                break
            fi
        fi
    done
else
    # --jobs runs up to N scenarios concurrently. Without --keep-going, do
    # not start new scenarios after the first failure; already-running ones
    # finish.
    parallel_failed="$RUN_DIR/.parallel-scenario-failed"
    rm -f "$parallel_failed"
    export ROOT BUILD LOG_DIR RUN_DIR KEEP_GOING parallel_failed
    if ! printf '%s\n' $SCENARIO_FILES |
            xargs -P "$JOBS" -n 1 sh -c '
                scenario=$1
                if [ "$KEEP_GOING" -eq 0 ] && [ -e "$parallel_failed" ]; then
                    exit 0
                fi
                keep_flag=""
                if [ "$KEEP_GOING" -eq 1 ]; then
                    keep_flag="--keep-going"
                fi
                if ! sh "$ROOT/sim/ci/run_scenario.sh" \
                        --build "$BUILD" --log-dir "$LOG_DIR" \
                        --run-dir "$RUN_DIR" $keep_flag "$scenario"; then
                    if [ "$KEEP_GOING" -eq 0 ]; then
                        : > "$parallel_failed"
                    fi
                    exit 1
                fi
            ' sh; then
        failed=1
    fi
    if [ -e "$parallel_failed" ]; then
        failed=1
    fi
fi

if [ "$failed" -ne 0 ]; then
    echo "ERROR: scenario failures (logs: $LOG_DIR)" >&2
    exit 1
fi

echo "logs: $LOG_DIR"
echo "SIM CI GREEN"
