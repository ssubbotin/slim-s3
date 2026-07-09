#!/usr/bin/env bash
# Usage: run_stub.sh <scenario> [port]
# Starts tests/integration/stub_server.py for one scenario, points the itest
# binary at it (only the matching stub-gated TEST_CASE actually runs), then
# tears the stub down.
set -euo pipefail
SCEN="${1:?usage: run_stub.sh <scenario> [port]}"

# Guard against a typo'd scenario name (ci.yml vs stubIs() vs stub_server.py
# drifting out of sync): without this, an unrecognized name would start the
# stub, run the itest binary with zero matching TEST_CASEs, and exit 0 --
# silently asserting nothing. Validate against the known list up front so a
# typo fails this step loudly instead.
KNOWN_SCENARIOS="header_count_flood header_byte_flood status_line_flood body_overflow delete_404 list_bad_xml list_empty_token list_page_fail put_500 ok"
known=0
for s in $KNOWN_SCENARIOS; do
    if [ "$s" = "$SCEN" ]; then
        known=1
        break
    fi
done
if [ "$known" -ne 1 ]; then
    echo "run_stub.sh: unknown scenario '$SCEN' (known: $KNOWN_SCENARIOS)" >&2
    exit 1
fi

PORT="${2:-19500}"
DIR="$(dirname "$0")"
BIN="$DIR/../../build/tests/slims3_itest"

SCENARIO="$SCEN" python3 "$DIR/stub_server.py" "$PORT" &
SRV=$!
trap 'kill $SRV 2>/dev/null || true' EXIT
sleep 1
SLIMS3_STUB_ENDPOINT="http://127.0.0.1:$PORT" SLIMS3_STUB_SCENARIO="$SCEN" "$BIN" --no-intro
