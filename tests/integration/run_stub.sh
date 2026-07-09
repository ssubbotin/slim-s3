#!/usr/bin/env bash
# Usage: run_stub.sh <scenario> [port]
# Starts tests/integration/stub_server.py for one scenario, points the itest
# binary at it (only the matching stub-gated TEST_CASE actually runs), then
# tears the stub down.
set -euo pipefail
SCEN="${1:?usage: run_stub.sh <scenario> [port]}"
PORT="${2:-19500}"
DIR="$(dirname "$0")"
BIN="$DIR/../../build/tests/slims3_itest"

SCENARIO="$SCEN" python3 "$DIR/stub_server.py" "$PORT" &
SRV=$!
trap 'kill $SRV 2>/dev/null || true' EXIT
sleep 1
SLIMS3_STUB_ENDPOINT="http://127.0.0.1:$PORT" SLIMS3_STUB_SCENARIO="$SCEN" "$BIN" --no-intro
