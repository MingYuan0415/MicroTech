#!/bin/sh
# Simulator CI entry: build + config drift + staged parity + per-scenario
# deterministic sessions (each scenario gets a fresh sim process and a
# fresh NVS directory).
# Usage: sim/ci/run_ci.sh [build-dir] [--update]
set -e
BUILD="build/sim"
UPDATE=""
for arg in "$@"; do
    case "$arg" in
        --update) UPDATE="--update" ;;
        *) BUILD="$arg" ;;
    esac
done
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

# Fail fast: a stale dev session on 5002 would absorb scenario traffic.
if python3 -c "import socket;socket.create_connection(('127.0.0.1',5002),timeout=1).close()" 2>/dev/null; then
    echo "ERROR: port 5002 already has a live agent (stop the dev session first)"
    exit 1
fi

cmake -S "$ROOT/sim" -B "$BUILD" -G Ninja >/dev/null
cmake --build "$BUILD"
python3 "$ROOT/sim/ci/check_lv_conf.py" --mirror "$BUILD/gen_inc/sdkconfig.h"

if [ -d "$ROOT/build/esp-idf/main/app_res_fs" ]; then
    diff -rq "$BUILD/sim_res_fs" "$ROOT/build/esp-idf/main/app_res_fs" \
        || { echo "ERROR: staged resources differ from device staging"; exit 1; }
fi

# PNG aux gate: off unless SIM_PNG_GOLDEN=1 or --update (local leftover
# goldens must not silently re-enable the gate).
GOLDEN_ARG=""
if [ "$UPDATE" = "--update" ] || [ "${SIM_PNG_GOLDEN:-}" = "1" ]; then
    GOLDEN_ARG="--golden $ROOT/sim/golden"
else
    echo "note: PNG aux gate off (set SIM_PNG_GOLDEN=1 to enable; tree asserts are the primary gate)"
fi

for scenario in "$ROOT"/sim/ci/scenarios/*.json; do
    name="$(basename "$scenario" .json)"
    nvs="$BUILD/ci_nvs_$name"
    rm -rf "$nvs"
    "$BUILD/microtech_sim" --headless --ci --res-dir "$BUILD/sim_res_fs" \
        --nvs-dir "$nvs" --out-dir "$BUILD/shots" --agent-port 5002 \
        >"$BUILD/sim_ci_$name.log" 2>&1 &
    SIM_PID=$!
    # shellcheck disable=SC2064
    trap "kill -9 $SIM_PID 2>/dev/null || true" EXIT

    ready=0
    for i in $(seq 1 120); do
        if python3 - <<'PY' 2>/dev/null
import socket
socket.create_connection(('127.0.0.1', 5002), timeout=1).close()
PY
        then ready=1; break; fi
        sleep 0.5
    done
    [ "$ready" = 1 ] || { echo "agent socket never came up for $name"; exit 1; }

    if [ "$UPDATE" = "--update" ]; then
        python3 "$ROOT/sim/tools/run_scenarios.py" "$scenario" \
            $GOLDEN_ARG --update
    else
        python3 "$ROOT/sim/tools/run_scenarios.py" "$scenario" \
            $GOLDEN_ARG
    fi
    kill -9 $SIM_PID 2>/dev/null || true
    trap - EXIT
done

echo "SIM CI GREEN"
