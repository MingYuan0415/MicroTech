#!/bin/sh
# Simulator CI entry: build + config drift + staged parity + per-scenario
# deterministic sessions (each scenario gets a fresh sim process and a
# fresh NVS directory).
# Usage: sim/ci/run_ci.sh [build-dir] [--update] [--scenario NAME]...
#        [--log-dir DIR]
set -e

BUILD="build/sim"
UPDATE=""
SCENARIOS=""
LOG_DIR=""

while [ $# -gt 0 ]; do
    case "$1" in
        --update)
            UPDATE="--update"
            shift
            ;;
        --scenario)
            [ $# -ge 2 ] || { echo "ERROR: --scenario needs a name"; exit 2; }
            SCENARIOS="$SCENARIOS $2"
            shift 2
            ;;
        --log-dir)
            [ $# -ge 2 ] || { echo "ERROR: --log-dir needs a path"; exit 2; }
            LOG_DIR="$2"
            shift 2
            ;;
        --*)
            echo "ERROR: unknown option $1"
            exit 2
            ;;
        *)
            BUILD="$1"
            shift
            ;;
    esac
done

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
mkdir -p "$BUILD"
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
                echo "ERROR: invalid scenario name: $name"
                exit 2
                ;;
            *.json)
                name="${name%.json}"
                ;;
        esac
        scenario="$ROOT/sim/ci/scenarios/$name.json"
        [ -f "$scenario" ] || {
            echo "ERROR: unknown scenario: $name"
            exit 2
        }
        SCENARIO_FILES="$SCENARIO_FILES $scenario"
    done
fi

# Serialize only CMake/Ninja mutations; scenario processes use private data and ports.
exec 9>"$BUILD/.sim-ci-build.lock"
flock 9
cmake -S "$ROOT/sim" -B "$BUILD" -G Ninja >/dev/null
if ! cmake --build "$BUILD" -- --quiet; then
    echo "ERROR: simulator build failed (logs: $LOG_DIR)"
    exit 1
fi
python3 "$ROOT/sim/ci/check_lv_conf.py" --mirror "$BUILD/gen_inc/sdkconfig.h"
flock -u 9

RUN_DIR="$(mktemp -d "${TMPDIR:-/tmp}/mt-ci.XXXXXX")"
exec 8>"${TMPDIR:-/tmp}/microtech-sim-port-allocator.lock"
cleanup() {
    if [ -n "${SIM_PID:-}" ]; then kill -9 "$SIM_PID" 2>/dev/null || true; fi
    rm -rf "$RUN_DIR"
}
trap cleanup EXIT INT TERM

if [ -d "$ROOT/build/esp-idf/main/app_res_fs" ]; then
    diff -rq "$BUILD/sim_res_fs" "$ROOT/build/esp-idf/main/app_res_fs" \
        || { echo "ERROR: staged resources differ from device staging"; exit 1; }
fi

# PNG aux gate: off unless SIM_PNG_GOLDEN=1 or --update. Scenario goldens live
# under the tracked CI directory; page-review goldens use sim/ci/golden/ui.
GOLDEN_ARG=""
if [ "$UPDATE" = "--update" ] || [ "${SIM_PNG_GOLDEN:-}" = "1" ]; then
    GOLDEN_ARG="--golden $ROOT/sim/ci/golden/scenarios"
else
    echo "note: PNG aux gate off (set SIM_PNG_GOLDEN=1 to enable; tree asserts are the primary gate)"
fi

run_scenario() {
    scenario="$1"
    name="$(basename "$scenario" .json)"
    nvs="$RUN_DIR/nvs_$name"
    sd="$RUN_DIR/sd_$name"
    data="$RUN_DIR/data_$name"
    shots="$RUN_DIR/shots_$name"
    log="$LOG_DIR/sim_$name.log"
    report="$LOG_DIR/$name.json"
    rm -rf "$nvs" "$sd" "$data" "$shots"
    mkdir -p "$shots"
    flock 8
    while true; do
        PORT="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')"
        [ "$PORT" = 5001 ] && continue
        exec 7>"${TMPDIR:-/tmp}/microtech-sim-port-$PORT.lock"
        if flock -n 7; then break; fi
    done
    SIM_DATA_DIR="$data" \
    "$BUILD/microtech_sim" --headless --ci --res-dir "$BUILD/sim_res_fs" \
        --nvs-dir "$nvs" --sd-dir "$sd" --out-dir "$shots" --agent-port "$PORT" \
        >"$log" 2>&1 &
    SIM_PID=$!

    ready=0
    for i in $(seq 1 120); do
        if python3 - "$PORT" <<'PY' 2>/dev/null
import socket
import sys
socket.create_connection(('127.0.0.1', int(sys.argv[1])), timeout=1).close()
PY
        then ready=1; break; fi
        sleep 0.5
    done

    [ "$ready" = 1 ] || {
        echo "agent socket never came up for $name (see $log)"
        exit 1
    }
    flock -u 7
    flock -u 8
    runner_args="--port $PORT --out-dir $shots --report $report"
    if [ "$UPDATE" = "--update" ]; then
        if ! python3 "$ROOT/sim/tools/run_scenarios.py" "$scenario" \
            $runner_args $GOLDEN_ARG --update; then
            cp -r "$shots" "$LOG_DIR/shots_$name" 2>/dev/null || true
            echo "ERROR: scenario $name failed (logs: $LOG_DIR)"
            exit 1
        fi
    else
        if ! python3 "$ROOT/sim/tools/run_scenarios.py" "$scenario" \
            $runner_args $GOLDEN_ARG; then
            cp -r "$shots" "$LOG_DIR/shots_$name" 2>/dev/null || true
            echo "ERROR: scenario $name failed (logs: $LOG_DIR)"
            exit 1
        fi
    fi
    kill -9 "$SIM_PID" 2>/dev/null || true
    SIM_PID=""
}

if [ -n "$SCENARIO_FILES" ]; then
    for scenario in $SCENARIO_FILES; do
        run_scenario "$scenario"
    done
else
    for scenario in "$ROOT"/sim/ci/scenarios/*.json; do
        run_scenario "$scenario"
    done
fi

echo "logs: $LOG_DIR"
echo "SIM CI GREEN"
