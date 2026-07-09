#!/usr/bin/env bash
# Reports what slim-s3 adds to a consumer binary, source-map-explorer style:
#   1. delta: stripped size of a minimal consumer using the full public API
#      minus a baseline that links libcurl but not slims3;
#   2. per-object attribution of the linked bytes, parsed from the link map.
#
# Usage: tools/size-report.sh [build-dir]     (default: build)
# Needs an already-built libslims3.a in <build-dir> (Release recommended).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-build}"
LIB="$ROOT/$BUILD/libslims3.a"
[ -f "$LIB" ] || { echo "no $LIB — build first: cmake -B $BUILD && cmake --build $BUILD -j" >&2; exit 1; }

CXX="${CXX:-c++}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/baseline.cpp" <<'EOF'
#include <curl/curl.h>
#include <cstdio>
int main() {
    CURL* h = curl_easy_init();
    std::printf("%p\n", (void*)h);
    curl_easy_cleanup(h);
    return 0;
}
EOF

cat > "$TMP/consumer.cpp" <<'EOF'
// References the full public API so the linker keeps every archive member
// a realistic consumer could reach.
#include <slims3/slims3.hpp>
#include <cstdio>
int main(int argc, char** argv) {
    slims3::Config cfg;
    cfg.endpoint = argv[1];
    cfg.accessKey = "k";
    cfg.secretKey = "s";
    slims3::Client s3(cfg);
    bool exists = false;
    s3.bucketExists("b", exists);
    s3.createBucket("b");
    const char* data = "x";
    s3.putObject("b", "k", data, 1, {});
    s3.putFile("b", "k", "/tmp/f");
    s3.getObject("b", "k", [](const char*, std::size_t) { return true; });
    s3.getToFile("b", "k", "/tmp/f");
    slims3::ObjectMeta meta;
    s3.statObject("b", "k", meta);
    s3.deleteObject("b", "k");
    s3.listObjects("b", "", true, [](const slims3::ObjectInfo&) { return true; });
    s3.cancel();
    std::printf("%d\n", argc);
    return 0;
}
EOF

"$CXX" -O2 -o "$TMP/baseline" "$TMP/baseline.cpp" -lcurl
"$CXX" -O2 -I"$ROOT/include" -o "$TMP/consumer" "$TMP/consumer.cpp" "$LIB" -lcurl \
    -Wl,-Map="$TMP/consumer.map"
strip -o "$TMP/baseline.s" "$TMP/baseline"
strip -o "$TMP/consumer.s" "$TMP/consumer"

BASE=$(stat -c%s "$TMP/baseline.s" 2>/dev/null || stat -f%z "$TMP/baseline.s")
CONS=$(stat -c%s "$TMP/consumer.s" 2>/dev/null || stat -f%z "$TMP/consumer.s")

echo "consumer (full API, stripped): $CONS bytes"
echo "baseline (libcurl only):       $BASE bytes"
echo "slims3 adds:                   $((CONS - BASE)) bytes ($(( (CONS - BASE) / 1024 )) KiB)"
echo
echo "per-object attribution (from link map):"
python3 - "$TMP/consumer.map" <<'EOF'
import collections, re, sys

sizes = collections.Counter()
pat = re.compile(r'0x[0-9a-f]+\s+(0x[0-9a-f]+)\s+\S*libslims3\.a\(([^)]+)\)\s*$')
for line in open(sys.argv[1]):
    m = pat.search(line)
    if m:
        sizes[m.group(2)] += int(m.group(1), 16)
total = sum(sizes.values())
print(f"  {'member':<18}{'bytes':>9}{'share':>8}")
for k, v in sizes.most_common():
    print(f"  {k:<18}{v:>9}{v/total:>7.1%}")
print(f"  {'TOTAL':<18}{total:>9}")
EOF
