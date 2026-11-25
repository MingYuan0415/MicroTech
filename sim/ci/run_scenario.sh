#!/bin/sh
# Run one simulator CI scenario in an isolated process and data directory.
# This helper is called by run_ci.sh; it owns only its own sim process so
# scenarios can be started in parallel without sharing cleanup state.
set -eu

BUILD=""
LOG_DIR=""
RUN_DIR=""
SCENARIO=""
KEEP_GOING=0

usage() {
    cat <<'EOF'
usage: run_scenario.sh --build DIR --log-dir DIR --run-dir DIR [--keep-going] SCENARIO
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --build)
            [ $# -ge 2 ] || { echo "ERROR: --build needs a path" >&2; exit 2; }
            BUILD="$2"
            shift 2
            ;;
        --log-dir)
            [ $# -ge 2 ] || { echo "ERROR: --log-dir needs a path" >&2; exit 2; }
            LOG_DIR="$2"
            shift 2
            ;;
        --run-dir)
            [ $# -ge 2 ] || { echo "ERROR: --run-dir needs a path" >&2; exit 2; }
            RUN_DIR="$2"
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
            [ -z "$SCENARIO" ] || {
                echo "ERROR: only one scenario may be passed" >&2
                exit 2
            }
            SCENARIO="$1"
            shift
            ;;
    esac
done

[ -n "$BUILD" ] || { echo "ERROR: --build is required" >&2; exit 2; }
[ -n "$LOG_DIR" ] || { echo "ERROR: --log-dir is required" >&2; exit 2; }
[ -n "$RUN_DIR" ] || { echo "ERROR: --run-dir is required" >&2; exit 2; }
[ -n "$SCENARIO" ] || { echo "ERROR: scenario is required" >&2; exit 2; }
[ -f "$SCENARIO" ] || { echo "ERROR: scenario not found: $SCENARIO" >&2; exit 2; }
[ -x "$BUILD/microtech_sim" ] || {
    echo "ERROR: simulator binary not found: $BUILD/microtech_sim" >&2
    exit 2
}

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
mkdir -p "$LOG_DIR" "$RUN_DIR"

name="$(basename "$SCENARIO" .json)"
nvs="$RUN_DIR/nvs_$name"
sd="$RUN_DIR/sd_$name"
data="$RUN_DIR/data_$name"
shots="$LOG_DIR/snapshots_$name"
log="$LOG_DIR/sim_$name.log"
report="$LOG_DIR/$name.json"

rm -rf "$nvs" "$sd" "$data" "$shots"
mkdir -p "$shots"

# Serialize port selection with the shared simulator CI allocator.
# scenario locks its chosen port while starting the sim; the bound socket
# prevents another process from reusing it after the lock is released.
exec 8>"${TMPDIR:-/tmp}/microtech-sim-port-allocator.lock"
flock 8
while true; do
    PORT="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')"
    exec 7>"${TMPDIR:-/tmp}/microtech-sim-port-$PORT.lock"
    if flock -n 7; then
        break
    fi
    exec 7>&-
done

SIM_PID=""
cleanup() {
    if [ -n "$SIM_PID" ]; then
        kill -9 "$SIM_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

SIM_DATA_DIR="$data" \
    "$BUILD/microtech_sim" --headless --ci --res-dir "$BUILD/sim_res_fs" \
    --nvs-dir "$nvs" --sd-dir "$sd" --out-dir "$shots" \
    --agent-port "$PORT" >"$log" 2>&1 &
SIM_PID=$!

ready=0
for _ in $(seq 1 120); do
    if python3 - "$PORT" <<'PY' 2>/dev/null
import socket
import sys
socket.create_connection(('127.0.0.1', int(sys.argv[1])), timeout=1).close()
PY
    then
        ready=1
        break
    fi
    sleep 0.5
done

if [ "$ready" != 1 ]; then
    echo "agent socket never came up for $name (see $log)" >&2
    exit 1
fi
flock -u 7
flock -u 8

runner_args="--port $PORT --out-dir $shots --report $report"
if [ "$KEEP_GOING" -eq 1 ]; then
    runner_args="$runner_args --keep-going"
fi
if ! python3 "$ROOT/sim/tools/run_scenarios.py" "$SCENARIO" $runner_args; then
    echo "ERROR: scenario $name failed (logs: $LOG_DIR)" >&2
    exit 1
fi

kill -9 "$SIM_PID" 2>/dev/null || true
SIM_PID=""
