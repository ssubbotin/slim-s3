# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

slim-s3: a slim S3 client library for C++17 with exactly one dependency (libcurl). Static library `slims3::slims3`, single public header `include/slims3/slims3.hpp`.

**`docs/DESIGN.md` is the authoritative design document** — motivation, full API, error model, response caps, testing strategy. Keep it in sync when behavior changes. (`docs/plans/` and `.superpowers/` are git-ignored working files, not part of the repo.)

## Build & test commands

```bash
# Configure + build (tests OFF by default; the existing build/ has them ON)
cmake -B build -DSLIMS3_BUILD_TESTS=ON && cmake --build build -j

# Unit tests (no network)
ctest --test-dir build -LE integration --output-on-failure

# Single test case (doctest; wildcards allowed, -ltc lists names)
./build/tests/slims3_tests -tc="signer: *"
```

Integration tests (`build/tests/slims3_itest`) are gated by environment variables — with none set, every TEST_CASE skips. Use the runner scripts:

```bash
# Full matrix against a live server (e.g. local MinIO/RustFS docker on :9000)
tests/integration/run.sh http://127.0.0.1:9000 <accessKey> <secretKey>

# Silent-server regression (stall guard must fire, not hang)
tests/integration/run.sh --silent

# One malicious/misbehaving-server stub scenario
tests/integration/run_stub.sh header_byte_flood
```

**Adding a stub scenario requires updating four places in sync**: `tests/integration/stub_server.py` (the scenario), `stubIs()`-gated TEST_CASE in `tests/integration/itest_main.cpp`, `KNOWN_SCENARIOS` in `run_stub.sh`, and the scenario loops in `.github/workflows/ci.yml` (both `integration` and `coverage` jobs).

CI runs a gcc/clang/ASan+UBSan unit matrix, integration against MinIO **and** RustFS in docker, all stub scenarios, and a gcovr coverage job (~98.5% line / ~88% decision on `src/`+`include/` — don't regress it; new branches in src/ need a test that reaches them, usually a stub scenario for transport paths).

Formatting: `.clang-format` (LLVM base, 4-space indent, 100 columns, left-aligned pointers).

`tools/size-report.sh` measures the library's contribution to a stripped consumer binary (delta vs a libcurl-only baseline, plus per-object attribution from the link map). The README quotes ~165 KB from it — re-run and update that figure if the library grows materially.

## Architecture

Six internal modules, each unit-tested directly (the test target's include path contains `src/`, so tests include internal headers):

- `src/client.cpp` + `client_detail.hpp` — public API operations, listObjects pagination (capped at 100k pages), and `mapError`, which applies the `ErrorKind` precedence order: cancelled → transport → s3 → http → parse. Success is strictly 2xx; redirects are never followed.
- `src/transport.{hpp,cpp}` — libcurl wrapper. One persistent easy handle per Client (`curl_easy_reset` between requests → connection/TLS reuse). Owns the always-on defaults: connect timeout, low-speed stall guard, cancellation via atomic flag checked in the xferinfo callback, and the four response caps against malicious servers (header count 2000, header bytes 256 KiB — deliberately below libcurl's own cap so our error message wins, 32 status-line blocks, 8 MiB in-memory body on the error/no-sink path). Caps map to `ErrorKind::transport`, never silent truncation.
- `src/signer.{hpp,cpp}` — own SigV4 implementation, verified against the official AWS test vectors. **Deliberately not `CURLOPT_AWS_SIGV4`** (libcurl's signing behavior varies by version — see DESIGN §1); libcurl is used strictly as transport. Payloads are always signed (no UNSIGNED-PAYLOAD).
- `src/sha256.{hpp,cpp}` — vendored public-domain SHA-256 + HMAC. Exists to keep the dependency list at exactly libcurl; don't replace with OpenSSL/etc.
- `src/xml_lite.{hpp,cpp}` — targeted extractor for the only two S3 response shapes used (`<Error>` and `ListBucketResult`), not a general XML parser. Fails loudly (`ErrorKind::parse`) on anything it can't handle.
- `src/uri.{hpp,cpp}` — S3 percent-encoding rules for paths and query strings.

## Design constraints (v1 — don't silently violate)

- C++17, CMake ≥ 3.16, libcurl is the **only** dependency. doctest is vendored, tests-only.
- No retries in the library — structured errors (`ErrorKind` + `httpStatus` + `s3Code`) let callers build their own policy.
- Synchronous client, one operation at a time; `cancel()` is the only method safe to call from another thread. No global state.
- Path-style addressing only; no multipart (objects ≤ 5 GB); presigned URLs/ACLs/SSE out of scope.
- Nothing may hang forever: timeouts and the stall guard are on by default, and every unbounded server-driven input has a cap.
- Caller-supplied header names/values are validated (`validHeaderToken`: rejects bytes < 0x20; names also reject `:`) before signing — header injection returns an error before any network I/O.
- `getToFile` writes `<path>.part` opened with `O_EXCL|O_NOFOLLOW` (POSIX), renames on success, removes on failure.
