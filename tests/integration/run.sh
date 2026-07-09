#!/usr/bin/env bash
# Usage: run.sh <endpoint> <access> <secret>   - full matrix run against one server
#        run.sh --silent                        - silent-server regression only
set -euo pipefail
BIN="$(dirname "$0")/../../build/tests/slims3_itest"

if [ "${1:-}" = "--silent" ]; then
    python3 "$(dirname "$0")/silent_server.py" 19999 &
    SRV=$!
    trap 'kill $SRV' EXIT
    sleep 1
    SLIMS3_SILENT_ENDPOINT="http://127.0.0.1:19999" "$BIN" --no-intro
else
    SLIMS3_ENDPOINT="$1" SLIMS3_ACCESS="$2" SLIMS3_SECRET="$3" "$BIN" --no-intro
fi
