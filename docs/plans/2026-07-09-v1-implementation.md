# slim-s3 v1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement slim-s3 v1 per `docs/DESIGN.md`: a C++17 S3 client with libcurl as the only dependency, own SigV4 signing, default-on stall guard, and a MinIO+RustFS integration matrix.

**Architecture:** A static library with five internal modules (`sha256`, `uri`, `signer`, `xml_lite`, `transport`) behind one public header (`include/slims3/slims3.hpp`), assembled by `client.cpp`. One persistent curl easy handle per `Client` (connection reuse); every request carries connect/stall/optional total timeouts; cancellation via the xferinfo callback.

**Tech Stack:** C++17, CMake ≥ 3.16, libcurl (transport only), doctest (vendored, tests only), GitHub Actions.

## Global Constraints

- C++17; no compiler extensions (`CMAKE_CXX_STANDARD_REQUIRED ON`, `CXX_EXTENSIONS OFF`).
- The library links exactly one external dependency: `CURL::libcurl`. Test-only tools (doctest, python3 for the silent server) do not count.
- Everything in English: code comments, commit messages, docs.
- Git identity for all commits: `Sergey Subbotin <ssubbotin@gmail.com>`. No AI attribution anywhere.
- Public API is exactly the one in `docs/DESIGN.md` §4 — do not add methods "while at it".
- Internal code lives in `namespace slims3::detail`; public API in `namespace slims3`.
- Payload is always signed with its real SHA-256 (never `UNSIGNED-PAYLOAD`).
- Every request suppresses `Expect: 100-continue` (header line `Expect:`).
- Working directory: `~/slim-s3`. Run all commands from the repo root.
- Build/test cycle: `cmake -B build -DSLIMS3_BUILD_TESTS=ON && cmake --build build -j && ctest --test-dir build -LE integration --output-on-failure`.

---

### Task 1: Project skeleton — CMake, vendored doctest, smoke test

**Files:**
- Create: `CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/test_main.cpp`, `tests/test_smoke.cpp`, `src/client.cpp` (stub), `include/slims3/slims3.hpp` (full public header), `.clang-format`
- Create (download): `tests/doctest.h`

**Interfaces:**
- Produces: CMake targets `slims3` (static lib, alias `slims3::slims3`) and `slims3_tests`; the complete public header used verbatim by every later task.

- [ ] **Step 1: Write the public header** — this is the API contract from DESIGN.md §4, used by all tasks:

`include/slims3/slims3.hpp`:
```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace slims3 {

struct Config {
    std::string endpoint;              // "http://rustfs:9000" or "https://s3.example.com"
    std::string region = "us-east-1";
    std::string accessKey;
    std::string secretKey;

    long connectTimeoutSec = 30;       // TCP/TLS connect budget
    long operationTimeoutSec = 0;      // whole-request cap; 0 = uncapped (stall guard still applies)
    long lowSpeedLimitBps = 1;         // stall guard: abort when transfer speed
    long lowSpeedTimeSec = 60;         //   stays below the limit for this long

    std::string caBundlePath;          // optional custom CA bundle
    bool tlsVerify = true;
    std::string userAgent = "slim-s3/0.1";
};

enum class ErrorKind {
    none,       // success
    transport,  // curl-level failure: DNS, connect, timeout, stall guard; also local file I/O
    http,       // HTTP error status without a parseable S3 error body
    s3,         // S3 error response (code + message)
    parse,      // malformed response body
    cancelled,  // aborted via cancel() or a callback returning false
};

struct Error {
    ErrorKind kind = ErrorKind::none;
    int httpStatus = 0;                // 0 when no response was received
    std::string s3Code;                // "NoSuchKey", "AccessDenied", ... (kind == s3)
    std::string message;               // human-readable; includes curl detail for transport
    int curlCode = 0;                  // CURLcode (kind == transport)
};

struct Result {
    Error error;
    std::uint64_t bytesTransferred = 0;
    explicit operator bool() const { return error.kind == ErrorKind::none; }
};

struct ObjectInfo {
    std::string key;
    std::uint64_t size = 0;
    std::string etag;
    bool isPrefix = false;             // true for CommonPrefixes entries (non-recursive listing)
};

struct ObjectMeta {
    ObjectInfo info;
    std::string contentType;
    std::string contentEncoding;       // as stored by the server; empty if none
};

struct PutOptions {
    std::string contentType;
    std::string contentEncoding;       // e.g. "zstd"; stored as object metadata
    std::vector<std::pair<std::string, std::string>> extraHeaders;
};

// Callbacks return false to cancel the operation (=> ErrorKind::cancelled).
using WriteFn = std::function<bool(const char* data, std::size_t len)>;
using ProgressFn = std::function<bool(std::uint64_t done, std::uint64_t total)>;
using ListFn = std::function<bool(const ObjectInfo&)>;  // false stops listing (not an error)

class Client {
public:
    explicit Client(Config cfg);
    ~Client();
    Client(Client&&) noexcept;
    Client& operator=(Client&&) noexcept;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    Result bucketExists(const std::string& bucket, bool& exists);
    Result createBucket(const std::string& bucket);

    Result putObject(const std::string& bucket, const std::string& key, const void* data,
                     std::size_t len, const PutOptions& opts = {}, const ProgressFn& progress = {});
    Result putFile(const std::string& bucket, const std::string& key, const std::string& filePath,
                   const PutOptions& opts = {}, const ProgressFn& progress = {});

    Result getObject(const std::string& bucket, const std::string& key, const WriteFn& sink,
                     const ProgressFn& progress = {}, ObjectMeta* meta = nullptr);
    Result getToFile(const std::string& bucket, const std::string& key,
                     const std::string& filePath, const ProgressFn& progress = {},
                     ObjectMeta* meta = nullptr);

    Result statObject(const std::string& bucket, const std::string& key, ObjectMeta& out);
    Result deleteObject(const std::string& bucket, const std::string& key);

    Result listObjects(const std::string& bucket, const std::string& prefix, bool recursive,
                       const ListFn& onObject);

    // Thread-safe. Aborts the operation currently running on this client;
    // it returns ErrorKind::cancelled at the next progress tick.
    void cancel();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace slims3
```

- [ ] **Step 2: Write CMakeLists**

`CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.16)
project(slims3 VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SLIMS3_BUILD_TESTS "Build slim-s3 tests" OFF)

find_package(CURL REQUIRED)

add_library(slims3
    src/client.cpp
    src/sha256.cpp
    src/uri.cpp
    src/signer.cpp
    src/xml_lite.cpp
    src/transport.cpp
)
add_library(slims3::slims3 ALIAS slims3)
target_include_directories(slims3 PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(slims3 PUBLIC CURL::libcurl)
target_compile_features(slims3 PUBLIC cxx_std_17)
if(NOT MSVC)
    target_compile_options(slims3 PRIVATE -Wall -Wextra)
endif()

include(GNUInstallDirs)
install(TARGETS slims3 EXPORT slims3-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/slims3 DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT slims3-targets NAMESPACE slims3:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/slims3)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/slims3Config.cmake
    "include(CMakeFindDependencyMacro)\nfind_dependency(CURL)\ninclude(\"\${CMAKE_CURRENT_LIST_DIR}/slims3-targets.cmake\")\n")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/slims3Config.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/slims3)

if(SLIMS3_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
```

Until Task 2–6 create the real module files, create them as empty translation units so the target links:
```bash
for f in sha256 uri signer xml_lite transport; do printf '// filled in by later tasks\n' > src/$f.cpp; done
```
`src/client.cpp` stub for now:
```cpp
#include "slims3/slims3.hpp"
// Implementation arrives in Tasks 7-9. The stub keeps the target linking
// until then; nothing references Client yet.
```

- [ ] **Step 3: Vendor doctest and write the smoke test**

```bash
curl -fsSL -o tests/doctest.h https://raw.githubusercontent.com/doctest/doctest/v2.4.11/doctest/doctest.h
```

`tests/CMakeLists.txt`:
```cmake
add_executable(slims3_tests
    test_main.cpp
    test_smoke.cpp
)
target_link_libraries(slims3_tests PRIVATE slims3)
target_include_directories(slims3_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src)
add_test(NAME unit COMMAND slims3_tests)
```

`tests/test_main.cpp`:
```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
```

`tests/test_smoke.cpp`:
```cpp
#include "doctest.h"
#include "slims3/slims3.hpp"

TEST_CASE("public header compiles and Result semantics hold") {
    slims3::Result r;
    CHECK(static_cast<bool>(r));  // default = success
    r.error.kind = slims3::ErrorKind::transport;
    CHECK_FALSE(static_cast<bool>(r));
}
```

- [ ] **Step 4: Add `.clang-format`**

`.clang-format`:
```yaml
BasedOnStyle: LLVM
IndentWidth: 4
ColumnLimit: 100
PointerAlignment: Left
AllowShortFunctionsOnASingleLine: Inline
```

- [ ] **Step 5: Build and run the smoke test**

Run: `cmake -B build -DSLIMS3_BUILD_TESTS=ON && cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: `unit ... Passed` (1 test case).

- [ ] **Step 6: Commit**

```bash
git add -A && git -c user.name="Sergey Subbotin" -c user.email="ssubbotin@gmail.com" \
  commit -m "Add project skeleton: CMake, public API header, vendored doctest, smoke test"
```

---

### Task 2: sha256 — SHA-256 + HMAC-SHA256

**Files:**
- Create: `src/sha256.hpp`, replace stub `src/sha256.cpp`
- Create: `tests/test_sha256.cpp` (add to `tests/CMakeLists.txt` sources)

**Interfaces:**
- Produces:
  - `std::array<std::uint8_t, 32> slims3::detail::sha256(const void* data, std::size_t len)`
  - `std::array<std::uint8_t, 32> slims3::detail::hmacSha256(const void* key, std::size_t keyLen, const void* msg, std::size_t msgLen)`
  - `std::string slims3::detail::toHex(const std::uint8_t* p, std::size_t n)` (lowercase)
  - `std::string slims3::detail::sha256Hex(std::string_view data)`

- [ ] **Step 1: Write the failing test** — all expected values below were computed with an independent implementation (Python `hashlib`/`hmac`); NIST FIPS 180-4 and RFC 4231 vectors:

`tests/test_sha256.cpp`:
```cpp
#include <string>

#include "doctest.h"
#include "sha256.hpp"

using slims3::detail::hmacSha256;
using slims3::detail::sha256Hex;
using slims3::detail::toHex;

TEST_CASE("sha256 NIST vectors") {
    CHECK(sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(sha256Hex(std::string(1000000, 'a')) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("hmac-sha256 RFC 4231 vectors") {
    std::string k1(20, '\x0b');
    auto m1 = hmacSha256(k1.data(), k1.size(), "Hi There", 8);
    CHECK(toHex(m1.data(), 32) ==
          "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");

    std::string k2 = "Jefe", d2 = "what do ya want for nothing?";
    auto m2 = hmacSha256(k2.data(), k2.size(), d2.data(), d2.size());
    CHECK(toHex(m2.data(), 32) ==
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    std::string k3(131, '\xaa');  // key longer than block size -> hashed first
    std::string d3 = "Test Using Larger Than Block-Size Key - Hash Key First";
    auto m3 = hmacSha256(k3.data(), k3.size(), d3.data(), d3.size());
    CHECK(toHex(m3.data(), 32) ==
          "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build -j 2>&1 | tail -5`
Expected: compile error — `sha256.hpp` not found.

- [ ] **Step 3: Implement**

`src/sha256.hpp`:
```cpp
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace slims3::detail {

std::array<std::uint8_t, 32> sha256(const void* data, std::size_t len);
std::array<std::uint8_t, 32> hmacSha256(const void* key, std::size_t keyLen, const void* msg,
                                        std::size_t msgLen);
std::string toHex(const std::uint8_t* p, std::size_t n);
std::string sha256Hex(std::string_view data);

}  // namespace slims3::detail
```

`src/sha256.cpp` — FIPS 180-4; the K table and initial state below are the standard constants (fractional parts of cube/square roots of the first primes):
```cpp
#include "sha256.hpp"

#include <cstring>

namespace slims3::detail {
namespace {

constexpr std::uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

struct Ctx {
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::uint8_t buf[64];
    std::uint64_t totalLen = 0;  // bytes
    std::size_t bufLen = 0;
};

void compress(Ctx& c, const std::uint8_t* p) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (std::uint32_t(p[4 * i]) << 24) | (std::uint32_t(p[4 * i + 1]) << 16) |
               (std::uint32_t(p[4 * i + 2]) << 8) | std::uint32_t(p[4 * i + 3]);
    for (int i = 16; i < 64; ++i) {
        std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = c.h[0], b = c.h[1], cc = c.h[2], d = c.h[3];
    std::uint32_t e = c.h[4], f = c.h[5], g = c.h[6], h = c.h[7];
    for (int i = 0; i < 64; ++i) {
        std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        std::uint32_t ch = (e & f) ^ (~e & g);
        std::uint32_t t1 = h + s1 + ch + kK[i] + w[i];
        std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        std::uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        std::uint32_t t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c.h[0] += a; c.h[1] += b; c.h[2] += cc; c.h[3] += d;
    c.h[4] += e; c.h[5] += f; c.h[6] += g; c.h[7] += h;
}

void update(Ctx& c, const void* data, std::size_t len) {
    auto* p = static_cast<const std::uint8_t*>(data);
    c.totalLen += len;
    if (c.bufLen != 0) {
        std::size_t take = std::min(len, std::size_t(64) - c.bufLen);
        std::memcpy(c.buf + c.bufLen, p, take);
        c.bufLen += take;
        p += take;
        len -= take;
        if (c.bufLen == 64) {
            compress(c, c.buf);
            c.bufLen = 0;
        }
    }
    while (len >= 64) {
        compress(c, p);
        p += 64;
        len -= 64;
    }
    if (len != 0) {
        std::memcpy(c.buf, p, len);
        c.bufLen = len;
    }
}

std::array<std::uint8_t, 32> finish(Ctx& c) {
    std::uint64_t bits = c.totalLen * 8;
    std::uint8_t pad = 0x80;
    update(c, &pad, 1);
    std::uint8_t zero = 0;
    while (c.bufLen != 56) update(c, &zero, 1);
    std::uint8_t lenBe[8];
    for (int i = 0; i < 8; ++i) lenBe[i] = std::uint8_t(bits >> (56 - 8 * i));
    // update() counts these bytes into totalLen, but bits was captured first.
    update(c, lenBe, 8);
    std::array<std::uint8_t, 32> out;
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 4; ++j) out[4 * i + j] = std::uint8_t(c.h[i] >> (24 - 8 * j));
    return out;
}

}  // namespace

std::array<std::uint8_t, 32> sha256(const void* data, std::size_t len) {
    Ctx c;
    update(c, data, len);
    return finish(c);
}

std::array<std::uint8_t, 32> hmacSha256(const void* key, std::size_t keyLen, const void* msg,
                                        std::size_t msgLen) {
    std::uint8_t k[64] = {0};
    if (keyLen > 64) {
        auto kh = sha256(key, keyLen);
        std::memcpy(k, kh.data(), 32);
    } else {
        std::memcpy(k, key, keyLen);
    }
    std::uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    Ctx inner;
    update(inner, ipad, 64);
    update(inner, msg, msgLen);
    auto ih = finish(inner);
    Ctx outer;
    update(outer, opad, 64);
    update(outer, ih.data(), 32);
    return finish(outer);
}

std::string toHex(const std::uint8_t* p, std::size_t n) {
    static const char* d = "0123456789abcdef";
    std::string out(n * 2, '0');
    for (std::size_t i = 0; i < n; ++i) {
        out[2 * i] = d[p[i] >> 4];
        out[2 * i + 1] = d[p[i] & 0xf];
    }
    return out;
}

std::string sha256Hex(std::string_view data) {
    auto h = sha256(data.data(), data.size());
    return toHex(h.data(), h.size());
}

}  // namespace slims3::detail
```

Add `test_sha256.cpp` to `tests/CMakeLists.txt` sources.

- [ ] **Step 4: Run tests**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: PASS (sha256 + hmac cases green).

- [ ] **Step 5: Commit**

```bash
git add -A && git -c user.name="Sergey Subbotin" -c user.email="ssubbotin@gmail.com" \
  commit -m "Add vendored SHA-256 and HMAC-SHA256 with NIST/RFC 4231 vectors"
```

---

### Task 3: uri — percent-encoding, canonical query, URL decoding

**Files:**
- Create: `src/uri.hpp`, replace stub `src/uri.cpp`
- Create: `tests/test_uri.cpp` (add to `tests/CMakeLists.txt`)

**Interfaces:**
- Produces (all in `slims3::detail`):
  - `std::string percentEncode(std::string_view s, bool keepSlash)` — RFC 3986 unreserved set (`A-Za-z0-9-._~`); uppercase hex; `keepSlash=true` leaves `/` (for object-key paths).
  - `std::string canonicalQuery(std::vector<std::pair<std::string, std::string>> params)` — encodes names and values, sorts by (name, value), joins `name=value` with `&`; empty vector → `""`.
  - `std::string urlDecode(std::string_view s)` — `%XX` → byte, `+` → space (S3 `encoding-type=url` semantics).

- [ ] **Step 1: Write the failing test**

`tests/test_uri.cpp`:
```cpp
#include "doctest.h"
#include "uri.hpp"

using namespace slims3::detail;

TEST_CASE("percentEncode: unreserved kept, everything else escaped, uppercase hex") {
    CHECK(percentEncode("AZaz09-._~", false) == "AZaz09-._~");
    CHECK(percentEncode("a b", false) == "a%20b");
    CHECK(percentEncode("a+b=c/d", false) == "a%2Bb%3Dc%2Fd");
    CHECK(percentEncode("a/b c", true) == "a/b%20c");           // keepSlash for key paths
    CHECK(percentEncode("\xD0\xB6", false) == "%D0%B6");        // UTF-8 bytes escaped
}

TEST_CASE("canonicalQuery: sorted, encoded, empty values kept") {
    CHECK(canonicalQuery({}) == "");
    CHECK(canonicalQuery({{"prefix", "data/"}, {"list-type", "2"}, {"encoding-type", "url"}}) ==
          "encoding-type=url&list-type=2&prefix=data%2F");
    // '=' and '+' inside a value (continuation tokens) must be escaped
    CHECK(canonicalQuery({{"continuation-token", "1/wJ+=xyz"}}) ==
          "continuation-token=1%2FwJ%2B%3Dxyz");
    CHECK(canonicalQuery({{"delimiter", ""}}) == "delimiter=");  // empty value kept as name=
}

TEST_CASE("urlDecode: %XX and plus-as-space") {
    CHECK(urlDecode("a%20b") == "a b");
    CHECK(urlDecode("a+b") == "a b");
    CHECK(urlDecode("a%2Bb") == "a+b");
    CHECK(urlDecode("%D0%B6") == "\xD0\xB6");
    CHECK(urlDecode("bad%2") == "bad%2");                        // truncated escape left as-is
}
```

- [ ] **Step 2: Run to verify it fails** — compile error, `uri.hpp` not found.

- [ ] **Step 3: Implement**

`src/uri.hpp`:
```cpp
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slims3::detail {

std::string percentEncode(std::string_view s, bool keepSlash);
std::string canonicalQuery(std::vector<std::pair<std::string, std::string>> params);
std::string urlDecode(std::string_view s);

}  // namespace slims3::detail
```

`src/uri.cpp`:
```cpp
#include "uri.hpp"

#include <algorithm>

namespace slims3::detail {
namespace {

bool unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '.' || c == '_' || c == '~';
}

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

std::string percentEncode(std::string_view s, bool keepSlash) {
    static const char* d = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (unreserved(c) || (keepSlash && c == '/')) {
            out += char(c);
        } else {
            out += '%';
            out += d[c >> 4];
            out += d[c & 0xf];
        }
    }
    return out;
}

std::string canonicalQuery(std::vector<std::pair<std::string, std::string>> params) {
    for (auto& kv : params) {
        kv.first = percentEncode(kv.first, false);
        kv.second = percentEncode(kv.second, false);
    }
    std::sort(params.begin(), params.end());
    std::string out;
    for (const auto& kv : params) {
        if (!out.empty()) out += '&';
        out += kv.first;
        out += '=';
        out += kv.second;
    }
    return out;
}

std::string urlDecode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + 2 < s.size() && hexVal(s[i + 1]) >= 0 &&
                   hexVal(s[i + 2]) >= 0) {
            out += char(hexVal(s[i + 1]) * 16 + hexVal(s[i + 2]));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

}  // namespace slims3::detail
```

- [ ] **Step 4: Run tests** — expected PASS.

- [ ] **Step 5: Commit**

```bash
git add -A && git -c user.name="Sergey Subbotin" -c user.email="ssubbotin@gmail.com" \
  commit -m "Add URI helpers: percent-encoding, canonical query, url-decode"
```

---

### Task 4: signer — SigV4

**Files:**
- Create: `src/signer.hpp`, replace stub `src/signer.cpp`
- Create: `tests/test_signer.cpp` (add to `tests/CMakeLists.txt`)

**Interfaces:**
- Consumes: `sha256Hex`, `hmacSha256`, `toHex` from Task 2.
- Produces (all in `slims3::detail`):
  ```cpp
  struct SignParams {
      std::string method;          // "GET"
      std::string canonicalUri;    // already percent-encoded path, e.g. "/b/k%20ey"
      std::string canonicalQuery;  // already canonical (Task 3) or ""
      std::vector<std::pair<std::string, std::string>> headers;  // must include "host"
      std::string payloadHashHex;
      std::string amzDate;         // "YYYYMMDDTHHMMSSZ"
      std::string region;
      std::string service = "s3";
      std::string accessKey, secretKey;
  };
  std::string canonicalRequest(const SignParams&);
  std::string stringToSign(const SignParams&);
  std::string authorizationHeader(const SignParams&);
  std::string formatAmzDate(std::time_t t);   // gmtime -> "YYYYMMDDTHHMMSSZ"
  ```

- [ ] **Step 1: Write the failing test** — expected strings below were produced by an independent Python reference implementation of the SigV4 spec (hashlib/hmac), with the AWS example credentials:

`tests/test_signer.cpp`:
```cpp
#include "doctest.h"
#include "signer.hpp"

using namespace slims3::detail;

static SignParams v1() {
    SignParams p;
    p.method = "GET";
    p.canonicalUri = "/test-bucket/";
    p.canonicalQuery = "encoding-type=url&list-type=2&prefix=data%2F";
    p.headers = {{"host", "127.0.0.1:9000"}};
    p.payloadHashHex = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    p.amzDate = "20260709T120000Z";
    p.region = "us-east-1";
    p.accessKey = "AKIDEXAMPLE";
    p.secretKey = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
    return p;
}

TEST_CASE("canonical request: GET listing with port in host") {
    CHECK(canonicalRequest(v1()) ==
          "GET\n/test-bucket/\nencoding-type=url&list-type=2&prefix=data%2F\n"
          "host:127.0.0.1:9000\n"
          "x-amz-content-sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\n"
          "x-amz-date:20260709T120000Z\n\n"
          "host;x-amz-content-sha256;x-amz-date\n"
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_CASE("string to sign and authorization: GET listing") {
    CHECK(stringToSign(v1()) ==
          "AWS4-HMAC-SHA256\n20260709T120000Z\n20260709/us-east-1/s3/aws4_request\n"
          "823997dc0701a108c9e48f8ee89f220004f27feeabcb1f0e47c6f8b144cc89cd");
    CHECK(authorizationHeader(v1()) ==
          "AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20260709/us-east-1/s3/aws4_request, "
          "SignedHeaders=host;x-amz-content-sha256;x-amz-date, "
          "Signature=c1095b92f0ea59cb6e5a45f6444fb903a99068e165ab3d9ac27aeb873da1b1c2");
}

TEST_CASE("authorization: PUT with body, content-type, content-encoding, key with space") {
    SignParams p;
    p.method = "PUT";
    p.canonicalUri = "/b/k%20ey";
    p.canonicalQuery = "";
    p.headers = {{"host", "127.0.0.1:9000"},
                 {"Content-Type", "application/octet-stream"},   // mixed case on purpose
                 {"Content-Encoding", "zstd"}};
    // sha256("hello world")
    p.payloadHashHex = "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9";
    p.amzDate = "20260709T120000Z";
    p.region = "us-east-1";
    p.accessKey = "AKIDEXAMPLE";
    p.secretKey = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
    CHECK(authorizationHeader(p) ==
          "AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20260709/us-east-1/s3/aws4_request, "
          "SignedHeaders=content-encoding;content-type;host;x-amz-content-sha256;x-amz-date, "
          "Signature=19bd87603cb61e3801c5c8e6c879e2ec181e6823fd2a23f039c3bb59af09b14a");
}

TEST_CASE("authorization: HEAD, default port host, another region") {
    SignParams p;
    p.method = "HEAD";
    p.canonicalUri = "/bucket/obj";
    p.canonicalQuery = "";
    p.headers = {{"host", "s3.example.com"}};
    p.payloadHashHex = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    p.amzDate = "20260709T120000Z";
    p.region = "eu-west-1";
    p.accessKey = "AKIDEXAMPLE";
    p.secretKey = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
    CHECK(authorizationHeader(p) ==
          "AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20260709/eu-west-1/s3/aws4_request, "
          "SignedHeaders=host;x-amz-content-sha256;x-amz-date, "
          "Signature=3d8fc65224f33e12c6323386d84a1335721ea2012a919ca1bedb64bb9bbbf848");
}

TEST_CASE("formatAmzDate") {
    CHECK(formatAmzDate(0) == "19700101T000000Z");
    CHECK(formatAmzDate(1767960000) == "20260109T120000Z");
}
```

- [ ] **Step 2: Run to verify it fails** — `signer.hpp` not found.

- [ ] **Step 3: Implement**

`src/signer.hpp`: the `SignParams` struct and four function declarations exactly as in **Interfaces** above, wrapped in `#pragma once` + includes (`<ctime>`, `<string>`, `<utility>`, `<vector>`).

`src/signer.cpp`:
```cpp
#include "signer.hpp"

#include <algorithm>
#include <cctype>

#include "sha256.hpp"

namespace slims3::detail {
namespace {

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

// SigV4: trim ends, collapse inner space runs to a single space.
std::string trimCollapse(const std::string& v) {
    std::string out;
    bool inSpace = false;
    for (char c : v) {
        if (c == ' ' || c == '\t') {
            inSpace = true;
        } else {
            if (inSpace && !out.empty()) out += ' ';
            inSpace = false;
            out += c;
        }
    }
    return out;
}

// -> (canonicalHeaders, signedHeaders); adds x-amz-content-sha256 and x-amz-date.
std::pair<std::string, std::string> headerLists(const SignParams& p) {
    std::vector<std::pair<std::string, std::string>> items;
    items.reserve(p.headers.size() + 2);
    for (const auto& kv : p.headers) items.emplace_back(toLower(kv.first), trimCollapse(kv.second));
    items.emplace_back("x-amz-content-sha256", p.payloadHashHex);
    items.emplace_back("x-amz-date", p.amzDate);
    std::sort(items.begin(), items.end());
    std::string canon, signedList;
    for (const auto& kv : items) {
        canon += kv.first;
        canon += ':';
        canon += kv.second;
        canon += '\n';
        if (!signedList.empty()) signedList += ';';
        signedList += kv.first;
    }
    return {canon, signedList};
}

std::string scope(const SignParams& p) {
    return p.amzDate.substr(0, 8) + "/" + p.region + "/" + p.service + "/aws4_request";
}

}  // namespace

std::string canonicalRequest(const SignParams& p) {
    auto [canonHeaders, signedHeaders] = headerLists(p);
    return p.method + "\n" + p.canonicalUri + "\n" + p.canonicalQuery + "\n" + canonHeaders +
           "\n" + signedHeaders + "\n" + p.payloadHashHex;
}

std::string stringToSign(const SignParams& p) {
    return "AWS4-HMAC-SHA256\n" + p.amzDate + "\n" + scope(p) + "\n" +
           sha256Hex(canonicalRequest(p));
}

std::string authorizationHeader(const SignParams& p) {
    std::string kSecret = "AWS4" + p.secretKey;
    std::string date = p.amzDate.substr(0, 8);
    auto kDate = hmacSha256(kSecret.data(), kSecret.size(), date.data(), date.size());
    auto kRegion = hmacSha256(kDate.data(), 32, p.region.data(), p.region.size());
    auto kService = hmacSha256(kRegion.data(), 32, p.service.data(), p.service.size());
    auto kSigning = hmacSha256(kService.data(), 32, "aws4_request", 12);
    std::string sts = stringToSign(p);
    auto sig = hmacSha256(kSigning.data(), 32, sts.data(), sts.size());
    auto [canonHeaders, signedHeaders] = headerLists(p);
    (void)canonHeaders;
    return "AWS4-HMAC-SHA256 Credential=" + p.accessKey + "/" + scope(p) +
           ", SignedHeaders=" + signedHeaders + ", Signature=" + toHex(sig.data(), 32);
}

std::string formatAmzDate(std::time_t t) {
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%dT%H%M%SZ", &tm);
    return buf;
}

}  // namespace slims3::detail
```

- [ ] **Step 4: Run tests** — expected PASS (all four signer cases + date).

- [ ] **Step 5: Commit**

```bash
git add -A && git -c user.name="Sergey Subbotin" -c user.email="ssubbotin@gmail.com" \
  commit -m "Add SigV4 signer verified against independent reference vectors"
```

---

### Task 5: xml_lite — targeted extraction of S3 responses

**Files:**
- Create: `src/xml_lite.hpp`, replace stub `src/xml_lite.cpp`
- Create: `tests/test_xml.cpp` (add to `tests/CMakeLists.txt`)

**Interfaces:**
- Produces (all in `slims3::detail`):
  ```cpp
  struct S3ErrorBody { std::string code, message; };
  bool parseErrorBody(std::string_view xml, S3ErrorBody& out);   // false if not an <Error> doc

  struct ListEntry { std::string key; std::uint64_t size = 0; std::string etag; bool isPrefix = false; };
  struct ListPage { std::vector<ListEntry> entries; bool truncated = false; std::string nextToken; };
  bool parseListPage(std::string_view xml, ListPage& out);       // false on malformed input

  std::string xmlUnescape(std::string_view s);
  ```
- Keys inside `ListEntry.key` stay URL-encoded here; the client decodes them (Task 9).

- [ ] **Step 1: Write the failing test**

`tests/test_xml.cpp`:
```cpp
#include "doctest.h"
#include "xml_lite.hpp"

using namespace slims3::detail;

static const char* kListSample = R"(<?xml version="1.0" encoding="UTF-8"?>
<ListBucketResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
  <Name>bkt</Name><Prefix>data/</Prefix><KeyCount>2</KeyCount><MaxKeys>1000</MaxKeys>
  <EncodingType>url</EncodingType>
  <IsTruncated>true</IsTruncated>
  <NextContinuationToken>1/wJ+=abc</NextContinuationToken>
  <Contents>
    <Key>data/file%20one.rss</Key>
    <LastModified>2026-07-09T12:00:00.000Z</LastModified>
    <ETag>&quot;9bb58f26192e4ba00f01e2e7b136bbd8&quot;</ETag>
    <Size>9672</Size>
    <StorageClass>STANDARD</StorageClass>
  </Contents>
  <Contents>
    <Key>data/two</Key><ETag>"abc"</ETag><Size>0</Size>
  </Contents>
  <CommonPrefixes><Prefix>data/sub%2Fdir/</Prefix></CommonPrefixes>
</ListBucketResult>)";

TEST_CASE("parseListPage: entries, prefixes, truncation") {
    ListPage p;
    REQUIRE(parseListPage(kListSample, p));
    REQUIRE(p.entries.size() == 3);
    CHECK(p.entries[0].key == "data/file%20one.rss");   // still URL-encoded at this layer
    CHECK(p.entries[0].size == 9672);
    CHECK(p.entries[0].etag == "9bb58f26192e4ba00f01e2e7b136bbd8");  // quotes stripped
    CHECK_FALSE(p.entries[0].isPrefix);
    CHECK(p.entries[1].etag == "abc");
    CHECK(p.entries[2].isPrefix);
    CHECK(p.entries[2].key == "data/sub%2Fdir/");
    CHECK(p.truncated);
    CHECK(p.nextToken == "1/wJ+=abc");
}

TEST_CASE("parseListPage: empty result, not truncated") {
    ListPage p;
    REQUIRE(parseListPage("<ListBucketResult><IsTruncated>false</IsTruncated></ListBucketResult>", p));
    CHECK(p.entries.empty());
    CHECK_FALSE(p.truncated);
    CHECK(p.nextToken.empty());
}

TEST_CASE("parseErrorBody") {
    S3ErrorBody e;
    REQUIRE(parseErrorBody(
        "<?xml version=\"1.0\"?><Error><Code>NoSuchKey</Code>"
        "<Message>The specified key does not exist.</Message><Key>x</Key></Error>", e));
    CHECK(e.code == "NoSuchKey");
    CHECK(e.message == "The specified key does not exist.");
    CHECK_FALSE(parseErrorBody("<NotAnError/>", e));
    CHECK_FALSE(parseErrorBody("plain text, no xml", e));
}

TEST_CASE("xmlUnescape: predefined entities and numeric refs") {
    CHECK(xmlUnescape("a&lt;b&gt;c&amp;d&quot;e&apos;f") == "a<b>c&d\"e'f");
    CHECK(xmlUnescape("&#65;&#x42;") == "AB");
    CHECK(xmlUnescape("no entities") == "no entities");
    CHECK(xmlUnescape("&bogus;") == "&bogus;");  // unknown entity left as-is
}

TEST_CASE("hostile input fails cleanly") {
    ListPage p;
    CHECK_FALSE(parseListPage("<ListBucketResult><Contents><Key>a", p));  // unclosed tag
    S3ErrorBody e;
    CHECK_FALSE(parseErrorBody("", e));
}
```

- [ ] **Step 2: Run to verify it fails** — `xml_lite.hpp` not found.

- [ ] **Step 3: Implement**

`src/xml_lite.hpp`: the declarations exactly as in **Interfaces** (with `#pragma once`, `<cstdint>`, `<string>`, `<string_view>`, `<vector>`).

`src/xml_lite.cpp`:
```cpp
#include "xml_lite.hpp"

#include <cstdlib>

namespace slims3::detail {
namespace {

// Inner text of the first <tag>...</tag> at or after `from`.
// S3 list/error documents never put attributes on these tags, so the plain
// form "<tag>" is matched; this is a targeted extractor, not an XML parser.
bool innerText(std::string_view xml, std::string_view tag, std::size_t from,
               std::string_view& out, std::size_t& endPos) {
    std::string open = "<" + std::string(tag) + ">";
    std::string close = "</" + std::string(tag) + ">";
    std::size_t s = xml.find(open, from);
    if (s == std::string_view::npos) return false;
    s += open.size();
    std::size_t e = xml.find(close, s);
    if (e == std::string_view::npos) return false;
    out = xml.substr(s, e - s);
    endPos = e + close.size();
    return true;
}

std::string stripQuotes(std::string s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') return s.substr(1, s.size() - 2);
    return s;
}

}  // namespace

std::string xmlUnescape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] != '&') {
            out += s[i++];
            continue;
        }
        std::size_t semi = s.find(';', i);
        if (semi == std::string_view::npos || semi - i > 10) {
            out += s[i++];
            continue;
        }
        std::string_view ent = s.substr(i + 1, semi - i - 1);
        if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "amp") out += '&';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (!ent.empty() && ent[0] == '#') {
            long code = (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X'))
                            ? std::strtol(std::string(ent.substr(2)).c_str(), nullptr, 16)
                            : std::strtol(std::string(ent.substr(1)).c_str(), nullptr, 10);
            if (code <= 0 || code > 0x10FFFF) { out += s[i++]; continue; }
            // Encode the code point as UTF-8.
            unsigned cp = unsigned(code);
            if (cp < 0x80) out += char(cp);
            else if (cp < 0x800) {
                out += char(0xC0 | (cp >> 6));
                out += char(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                out += char(0xE0 | (cp >> 12));
                out += char(0x80 | ((cp >> 6) & 0x3F));
                out += char(0x80 | (cp & 0x3F));
            } else {
                out += char(0xF0 | (cp >> 18));
                out += char(0x80 | ((cp >> 12) & 0x3F));
                out += char(0x80 | ((cp >> 6) & 0x3F));
                out += char(0x80 | (cp & 0x3F));
            }
        } else {
            out += s[i++];  // unknown entity: emit '&' and continue scanning
            continue;
        }
        i = semi + 1;
    }
    return out;
}

bool parseErrorBody(std::string_view xml, S3ErrorBody& out) {
    if (xml.find("<Error>") == std::string_view::npos &&
        xml.find("<Error ") == std::string_view::npos)
        return false;
    std::string_view v;
    std::size_t end;
    if (innerText(xml, "Code", 0, v, end)) out.code = xmlUnescape(v);
    if (innerText(xml, "Message", 0, v, end)) out.message = xmlUnescape(v);
    return !out.code.empty();
}

bool parseListPage(std::string_view xml, ListPage& out) {
    if (xml.find("<ListBucketResult") == std::string_view::npos) return false;
    // Reject documents with an opened-but-unclosed Contents block.
    std::size_t pos = 0;
    std::string_view block;
    std::size_t end;
    std::size_t contentsOpens = 0, contentsParsed = 0;
    for (std::size_t p = xml.find("<Contents>"); p != std::string_view::npos;
         p = xml.find("<Contents>", p + 1))
        ++contentsOpens;
    while (innerText(xml, "Contents", pos, block, end)) {
        pos = end;
        ++contentsParsed;
        ListEntry e;
        std::string_view v;
        std::size_t be;
        if (!innerText(block, "Key", 0, v, be)) return false;
        e.key = xmlUnescape(v);
        if (innerText(block, "Size", 0, v, be)) e.size = std::strtoull(std::string(v).c_str(), nullptr, 10);
        if (innerText(block, "ETag", 0, v, be)) e.etag = stripQuotes(xmlUnescape(v));
        out.entries.push_back(std::move(e));
    }
    if (contentsParsed != contentsOpens) return false;
    pos = 0;
    while (innerText(xml, "CommonPrefixes", pos, block, end)) {
        pos = end;
        std::string_view v;
        std::size_t be;
        if (!innerText(block, "Prefix", 0, v, be)) return false;
        ListEntry e;
        e.key = xmlUnescape(v);
        e.isPrefix = true;
        out.entries.push_back(std::move(e));
    }
    std::string_view v;
    if (innerText(xml, "IsTruncated", 0, v, end)) out.truncated = (v == "true");
    if (innerText(xml, "NextContinuationToken", 0, v, end)) out.nextToken = xmlUnescape(v);
    return true;
}

}  // namespace slims3::detail
```

- [ ] **Step 4: Run tests** — expected PASS.

- [ ] **Step 5: Commit**

```bash
git add -A && git -c user.name="Sergey Subbotin" -c user.email="ssubbotin@gmail.com" \
  commit -m "Add targeted XML extraction for S3 error and ListObjectsV2 responses"
```

---

### Task 6: transport — endpoint parsing and the curl wrapper

**Files:**
- Create: `src/transport.hpp`, replace stub `src/transport.cpp`
- Create: `tests/test_transport.cpp` (add to `tests/CMakeLists.txt`) — covers the pure parts (endpoint parsing, header lookup, meta extraction); the network path is exercised by Task 10.

**Interfaces:**
- Consumes: `slims3::Config`, `slims3::WriteFn/ProgressFn/ObjectMeta` from the public header.
- Produces (all in `slims3::detail`):
  ```cpp
  struct Endpoint {
      std::string scheme = "http";
      std::string host;
      int port = 80;
      bool isDefaultPort = true;
      std::string hostHeader() const;   // "host" or "host:port" when non-default
      std::string baseUrl() const;      // "scheme://host[:port]" (port only when non-default)
  };
  bool parseEndpoint(std::string_view s, Endpoint& out, std::string& err);

  struct HttpRequest {
      std::string method;                       // "GET"/"PUT"/"HEAD"/"DELETE"
      std::string url;                          // full URL
      std::vector<std::string> headerLines;     // "Name: value"
      const char* body = nullptr;               // upload payload (PUT)
      std::size_t bodyLen = 0;
      bool noBody = false;                      // HEAD
      slims3::WriteFn sink;                     // null -> capture into HttpResponse::body
      slims3::ProgressFn progress;
      slims3::ObjectMeta* meta = nullptr;       // filled from headers before first sink call
  };
  struct HttpResponse {
      long status = 0;
      std::vector<std::pair<std::string, std::string>> headers;  // lowercased names
      std::string body;                         // error body, or full body when sink==null
      std::uint64_t sinkBytes = 0;              // bytes delivered to sink
      const std::string* find(std::string_view name) const;      // nullptr if absent
  };
  void metaFromHeaders(const HttpResponse& r, slims3::ObjectMeta& m);  // type/encoding/etag/size

  class Transport {
  public:
      explicit Transport(const slims3::Config& cfg);
      ~Transport();
      // Returns the CURLcode; fills resp. `aborted` reports that one of OUR
      // callbacks stopped the transfer (cancel flag / sink false / progress false).
      int execute(const HttpRequest& req, HttpResponse& resp, std::atomic<bool>& cancel,
                  bool& aborted, std::string& curlError);
  };
  ```

- [ ] **Step 1: Write the failing test**

`tests/test_transport.cpp`:
```cpp
#include "doctest.h"
#include "transport.hpp"

using namespace slims3::detail;

TEST_CASE("parseEndpoint") {
    Endpoint e;
    std::string err;
    REQUIRE(parseEndpoint("http://rustfs:9000", e, err));
    CHECK(e.scheme == "http");
    CHECK(e.host == "rustfs");
    CHECK(e.port == 9000);
    CHECK_FALSE(e.isDefaultPort);
    CHECK(e.hostHeader() == "rustfs:9000");
    CHECK(e.baseUrl() == "http://rustfs:9000");

    REQUIRE(parseEndpoint("https://s3.example.com", e, err));
    CHECK(e.port == 443);
    CHECK(e.isDefaultPort);
    CHECK(e.hostHeader() == "s3.example.com");
    CHECK(e.baseUrl() == "https://s3.example.com");

    REQUIRE(parseEndpoint("minio.local", e, err));   // scheme defaults to http
    CHECK(e.scheme == "http");
    CHECK(e.port == 80);
    CHECK(e.hostHeader() == "minio.local");

    CHECK_FALSE(parseEndpoint("", e, err));
    CHECK_FALSE(parseEndpoint("ftp://x", e, err));
    CHECK_FALSE(parseEndpoint("http://host:notaport", e, err));
    CHECK_FALSE(parseEndpoint("http://host/path", e, err));  // path suffix not supported in v1
}

TEST_CASE("HttpResponse::find and metaFromHeaders") {
    HttpResponse r;
    r.headers = {{"content-type", "application/octet-stream"},
                 {"content-encoding", "zstd"},
                 {"content-length", "9672"},
                 {"etag", "\"9bb58f26192e4ba00f01e2e7b136bbd8\""}};
    REQUIRE(r.find("Content-Type") != nullptr);
    CHECK(*r.find("Content-Type") == "application/octet-stream");
    CHECK(r.find("x-missing") == nullptr);

    slims3::ObjectMeta m;
    metaFromHeaders(r, m);
    CHECK(m.contentType == "application/octet-stream");
    CHECK(m.contentEncoding == "zstd");
    CHECK(m.info.size == 9672);
    CHECK(m.info.etag == "9bb58f26192e4ba00f01e2e7b136bbd8");
}
```

- [ ] **Step 2: Run to verify it fails** — `transport.hpp` not found.

- [ ] **Step 3: Implement**

`src/transport.hpp`: declarations exactly as in **Interfaces** (includes: `<atomic>`, `<cstdint>`, `<string>`, `<string_view>`, `<utility>`, `<vector>`, `"slims3/slims3.hpp"`; forward-declare `typedef void CURL;` is unnecessary — keep curl includes in the .cpp by storing `void* handle_`).

`src/transport.cpp`:
```cpp
#include "transport.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>

namespace slims3::detail {
namespace {

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

void globalInitOnce() {
    static std::once_flag flag;
    std::call_once(flag, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

struct CbCtx {
    CURL* handle = nullptr;
    const HttpRequest* req = nullptr;
    HttpResponse* resp = nullptr;
    std::atomic<bool>* cancel = nullptr;
    bool aborted = false;
    bool metaFilled = false;
    std::size_t bodyOff = 0;
};

size_t onHeader(char* buf, size_t size, size_t nitems, void* ud) {
    auto* ctx = static_cast<CbCtx*>(ud);
    std::string line(buf, size * nitems);
    if (line.rfind("HTTP/", 0) == 0) {
        ctx->resp->headers.clear();  // new response block (e.g. after redirect)
        return size * nitems;
    }
    auto colon = line.find(':');
    if (colon != std::string::npos) {
        std::string name = toLower(line.substr(0, colon));
        std::string value = line.substr(colon + 1);
        // trim spaces and trailing CRLF
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(0, 1);
        while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) value.pop_back();
        ctx->resp->headers.emplace_back(std::move(name), std::move(value));
    }
    return size * nitems;
}

size_t onWrite(char* buf, size_t size, size_t nmemb, void* ud) {
    auto* ctx = static_cast<CbCtx*>(ud);
    size_t len = size * nmemb;
    long status = 0;
    curl_easy_getinfo(ctx->handle, CURLINFO_RESPONSE_CODE, &status);
    if (status >= 400 || !ctx->req->sink) {
        // Error body (for S3 error parsing) or caller wants the whole body.
        // Cap accumulation to keep a hostile server from ballooning memory.
        constexpr size_t kCap = 8 * 1024 * 1024;
        if (ctx->resp->body.size() < kCap)
            ctx->resp->body.append(buf, std::min(len, kCap - ctx->resp->body.size()));
        return len;
    }
    if (ctx->req->meta && !ctx->metaFilled) {
        metaFromHeaders(*ctx->resp, *ctx->req->meta);
        ctx->metaFilled = true;
    }
    if (!ctx->req->sink(buf, len)) {
        ctx->aborted = true;
        return 0;  // CURLE_WRITE_ERROR
    }
    ctx->resp->sinkBytes += len;
    return len;
}

size_t onRead(char* buf, size_t size, size_t nmemb, void* ud) {
    auto* ctx = static_cast<CbCtx*>(ud);
    size_t room = size * nmemb;
    size_t left = ctx->req->bodyLen - ctx->bodyOff;
    size_t take = std::min(room, left);
    std::memcpy(buf, ctx->req->body + ctx->bodyOff, take);
    ctx->bodyOff += take;
    return take;
}

int onXfer(void* ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    auto* ctx = static_cast<CbCtx*>(ud);
    if (ctx->cancel->load(std::memory_order_relaxed)) {
        ctx->aborted = true;
        return 1;  // CURLE_ABORTED_BY_CALLBACK
    }
    if (ctx->req->progress) {
        bool up = ctx->req->body != nullptr;
        auto done = std::uint64_t(up ? ulnow : dlnow);
        auto total = std::uint64_t(up ? ultotal : dltotal);
        if (!ctx->req->progress(done, total)) {
            ctx->aborted = true;
            return 1;
        }
    }
    return 0;
}

}  // namespace

std::string Endpoint::hostHeader() const {
    return isDefaultPort ? host : host + ":" + std::to_string(port);
}

std::string Endpoint::baseUrl() const { return scheme + "://" + hostHeader(); }

bool parseEndpoint(std::string_view s, Endpoint& out, std::string& err) {
    out = Endpoint{};
    std::string rest(s);
    auto sep = rest.find("://");
    if (sep != std::string::npos) {
        out.scheme = rest.substr(0, sep);
        rest = rest.substr(sep + 3);
    }
    if (out.scheme != "http" && out.scheme != "https") {
        err = "endpoint scheme must be http or https";
        return false;
    }
    if (rest.find('/') != std::string::npos) {
        err = "endpoint must not contain a path";
        return false;
    }
    auto colon = rest.rfind(':');
    if (colon != std::string::npos) {
        std::string p = rest.substr(colon + 1);
        char* endp = nullptr;
        long v = std::strtol(p.c_str(), &endp, 10);
        if (p.empty() || *endp != '\0' || v < 1 || v > 65535) {
            err = "invalid endpoint port";
            return false;
        }
        out.port = int(v);
        out.host = rest.substr(0, colon);
    } else {
        out.port = (out.scheme == "https") ? 443 : 80;
        out.host = rest;
    }
    if (out.host.empty()) {
        err = "endpoint host is empty";
        return false;
    }
    out.isDefaultPort = (out.scheme == "https") ? (out.port == 443) : (out.port == 80);
    return true;
}

const std::string* HttpResponse::find(std::string_view name) const {
    std::string lower = toLower(std::string(name));
    for (const auto& kv : headers)
        if (kv.first == lower) return &kv.second;
    return nullptr;
}

void metaFromHeaders(const HttpResponse& r, slims3::ObjectMeta& m) {
    if (const auto* v = r.find("content-type")) m.contentType = *v;
    if (const auto* v = r.find("content-encoding")) m.contentEncoding = *v;
    if (const auto* v = r.find("content-length")) m.info.size = std::strtoull(v->c_str(), nullptr, 10);
    if (const auto* v = r.find("etag")) {
        std::string e = *v;
        if (e.size() >= 2 && e.front() == '"' && e.back() == '"') e = e.substr(1, e.size() - 2);
        m.info.etag = e;
    }
}

struct TransportState {
    CURL* handle = nullptr;
    slims3::Config cfg;
};

Transport::Transport(const slims3::Config& cfg) {
    globalInitOnce();
    auto* st = new TransportState;
    st->cfg = cfg;
    st->handle = curl_easy_init();
    state_ = st;
}

Transport::~Transport() {
    auto* st = static_cast<TransportState*>(state_);
    if (st->handle) curl_easy_cleanup(st->handle);
    delete st;
}

int Transport::execute(const HttpRequest& req, HttpResponse& resp, std::atomic<bool>& cancel,
                       bool& aborted, std::string& curlError) {
    auto* st = static_cast<TransportState*>(state_);
    CURL* h = st->handle;
    // Reset options but keep the handle: libcurl's connection/DNS/TLS caches
    // live on the handle and survive curl_easy_reset, so requests reuse
    // connections instead of reconnecting every time.
    curl_easy_reset(h);

    CbCtx ctx;
    ctx.handle = h;
    ctx.req = &req;
    ctx.resp = &resp;
    ctx.cancel = &cancel;

    char errbuf[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(h, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(h, CURLOPT_USERAGENT, st->cfg.userAgent.c_str());
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, st->cfg.connectTimeoutSec);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, st->cfg.lowSpeedLimitBps);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, st->cfg.lowSpeedTimeSec);
    if (st->cfg.operationTimeoutSec > 0)
        curl_easy_setopt(h, CURLOPT_TIMEOUT, st->cfg.operationTimeoutSec);
    if (!st->cfg.caBundlePath.empty())
        curl_easy_setopt(h, CURLOPT_CAINFO, st->cfg.caBundlePath.c_str());
    if (!st->cfg.tlsVerify) {
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    if (req.noBody) {
        curl_easy_setopt(h, CURLOPT_NOBODY, 1L);
    } else if (req.body != nullptr) {
        curl_easy_setopt(h, CURLOPT_UPLOAD, 1L);  // implies PUT
        curl_easy_setopt(h, CURLOPT_READFUNCTION, onRead);
        curl_easy_setopt(h, CURLOPT_READDATA, &ctx);
        curl_easy_setopt(h, CURLOPT_INFILESIZE_LARGE, curl_off_t(req.bodyLen));
    }
    if (req.method == "DELETE") curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, "DELETE");

    curl_slist* hdrs = nullptr;
    for (const auto& line : req.headerLines) hdrs = curl_slist_append(hdrs, line.c_str());
    hdrs = curl_slist_append(hdrs, "Expect:");  // never wait for 100-continue
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);

    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, onHeader);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, &ctx);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, onWrite);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, onXfer);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    // Note: CURLOPT_ACCEPT_ENCODING is deliberately NOT set — the body must
    // arrive exactly as stored (a zstd-encoded object stays zstd-encoded).

    CURLcode rc = curl_easy_perform(h);
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &resp.status);
    curl_slist_free_all(hdrs);

    // HEAD/empty-body responses never hit onWrite; fill meta from headers here.
    if (req.meta && !ctx.metaFilled && resp.status < 400) metaFromHeaders(resp, *req.meta);

    aborted = ctx.aborted;
    curlError = errbuf[0] ? errbuf : (rc != CURLE_OK ? curl_easy_strerror(rc) : "");
    return int(rc);
}

}  // namespace slims3::detail
```

Note: `Transport` in the header stores `void* state_;` (private member) so `<curl/curl.h>` stays out of headers. Declare `state_` in `transport.hpp` and adjust the constructor/destructor as shown.

- [ ] **Step 4: Run tests** — expected PASS (endpoint + meta cases).

- [ ] **Step 5: Commit**

```bash
git add -A && git -c user.name="Sergey Subbotin" -c user.email="ssubbotin@gmail.com" \
  commit -m "Add curl transport: persistent handle, timeouts, stall guard, cancellation hooks"
```

---

### Task 7: client — request pipeline, error mapping, bucket/stat/delete ops

**Files:**
- Replace stub: `src/client.cpp`
- Create: `tests/test_client_mapping.cpp` (add to `tests/CMakeLists.txt`)

**Interfaces:**
- Consumes: everything produced by Tasks 2–6.
- Produces: working `Client::bucketExists / createBucket / statObject / deleteObject / cancel`, plus the internal `Impl::run()` pipeline and `mapError()` used by Tasks 8–9. To make error mapping unit-testable without a network, expose in `client.cpp` (declared in a tiny internal header):

`src/client_detail.hpp`:
```cpp
#pragma once

#include <string>

#include "slims3/slims3.hpp"
#include "transport.hpp"

namespace slims3::detail {

// Maps a finished transport exchange onto the public Error model (DESIGN.md §6).
// `headSynthCode`: for HEAD requests there is no body; on 404 synthesize this
// S3 code ("NoSuchKey" for object ops, "NoSuchBucket" for bucket ops).
Error mapError(int curlCode, bool aborted, const std::string& curlError,
               const HttpResponse& resp, const std::string& headSynthCode);

}  // namespace slims3::detail
```

- [ ] **Step 1: Write the failing test**

`tests/test_client_mapping.cpp`:
```cpp
#include <curl/curl.h>

#include "client_detail.hpp"
#include "doctest.h"

using namespace slims3::detail;
using slims3::ErrorKind;

TEST_CASE("mapError: success") {
    HttpResponse r;
    r.status = 200;
    auto e = mapError(0, false, "", r, "");
    CHECK(e.kind == ErrorKind::none);
}

TEST_CASE("mapError: cancelled beats curl code") {
    HttpResponse r;
    auto e = mapError(int(CURLE_ABORTED_BY_CALLBACK), true, "aborted", r, "");
    CHECK(e.kind == ErrorKind::cancelled);
    // sink returning false surfaces as CURLE_WRITE_ERROR but is still a cancel
    auto e2 = mapError(int(CURLE_WRITE_ERROR), true, "write error", r, "");
    CHECK(e2.kind == ErrorKind::cancelled);
}

TEST_CASE("mapError: transport failure") {
    HttpResponse r;
    auto e = mapError(int(CURLE_OPERATION_TIMEDOUT), false, "timeout", r, "");
    CHECK(e.kind == ErrorKind::transport);
    CHECK(e.curlCode == int(CURLE_OPERATION_TIMEDOUT));
    CHECK(e.httpStatus == 0);
    CHECK(e.message.find("timeout") != std::string::npos);
}

TEST_CASE("mapError: S3 error body parsed") {
    HttpResponse r;
    r.status = 404;
    r.body = "<Error><Code>NoSuchKey</Code><Message>nope</Message></Error>";
    auto e = mapError(0, false, "", r, "");
    CHECK(e.kind == ErrorKind::s3);
    CHECK(e.httpStatus == 404);
    CHECK(e.s3Code == "NoSuchKey");
    CHECK(e.message == "nope");
}

TEST_CASE("mapError: HEAD 404 synthesizes the code") {
    HttpResponse r;
    r.status = 404;  // HEAD: empty body
    auto e = mapError(0, false, "", r, "NoSuchBucket");
    CHECK(e.kind == ErrorKind::s3);
    CHECK(e.s3Code == "NoSuchBucket");
}

TEST_CASE("mapError: HTTP error without S3 body") {
    HttpResponse r;
    r.status = 503;
    r.body = "Service Unavailable";
    auto e = mapError(0, false, "", r, "");
    CHECK(e.kind == ErrorKind::http);
    CHECK(e.httpStatus == 503);
}
```

- [ ] **Step 2: Run to verify it fails** — `client_detail.hpp` not found.

- [ ] **Step 3: Implement the client core**

`src/client.cpp` (full file; `run()` and the op helpers below are also the foundation Tasks 8–9 extend):
```cpp
#include <curl/curl.h>

#include <atomic>
#include <cstdio>
#include <ctime>
#include <fstream>

#include "client_detail.hpp"
#include "signer.hpp"
#include "slims3/slims3.hpp"
#include "transport.hpp"
#include "uri.hpp"
#include "xml_lite.hpp"

namespace slims3 {

using namespace detail;

namespace detail {

Error mapError(int curlCode, bool aborted, const std::string& curlError,
               const HttpResponse& resp, const std::string& headSynthCode) {
    Error e;
    if (aborted) {
        e.kind = ErrorKind::cancelled;
        e.message = "operation cancelled";
        return e;
    }
    if (curlCode != 0) {
        e.kind = ErrorKind::transport;
        e.curlCode = curlCode;
        e.message = curlError.empty() ? "transport failure" : curlError;
        return e;
    }
    if (resp.status >= 400) {
        e.httpStatus = int(resp.status);
        S3ErrorBody body;
        if (parseErrorBody(resp.body, body)) {
            e.kind = ErrorKind::s3;
            e.s3Code = body.code;
            e.message = body.message.empty() ? body.code : body.message;
        } else if (!headSynthCode.empty() && resp.status == 404) {
            e.kind = ErrorKind::s3;
            e.s3Code = headSynthCode;
            e.message = headSynthCode;
        } else {
            e.kind = ErrorKind::http;
            e.message = "HTTP " + std::to_string(resp.status) +
                        (resp.body.empty() ? "" : (": " + resp.body.substr(0, 200)));
        }
        return e;
    }
    return e;  // none
}

}  // namespace detail

struct Client::Impl {
    Config cfg;
    Endpoint ep;
    Transport tr;
    std::atomic<bool> cancel{false};
    std::string epError;  // non-empty if the endpoint failed to parse

    explicit Impl(Config c) : cfg(std::move(c)), tr(cfg) {
        if (!parseEndpoint(cfg.endpoint, ep, epError) && epError.empty())
            epError = "invalid endpoint";
    }

    struct Op {
        std::string method;
        std::string bucket;
        std::string key;  // empty for bucket-level ops
        std::vector<std::pair<std::string, std::string>> query;      // raw, unencoded
        std::vector<std::pair<std::string, std::string>> extraHdrs;  // e.g. Content-Type
        const char* body = nullptr;
        std::size_t bodyLen = 0;
        bool noBody = false;
        WriteFn sink;
        ProgressFn progress;
        ObjectMeta* meta = nullptr;
        std::string headSynthCode;  // "NoSuchKey"/"NoSuchBucket" for HEAD 404
    };

    Result run(const Op& op, HttpResponse& resp) {
        if (!epError.empty())
            return Result{Error{ErrorKind::transport, 0, "", "bad endpoint: " + epError, 0}, 0};

        std::string payloadHash =
            op.body ? sha256Hex(std::string_view(op.body, op.bodyLen)) : sha256Hex("");
        std::string amzDate = formatAmzDate(std::time(nullptr));
        std::string canonicalUri = "/" + op.bucket;
        if (!op.key.empty()) canonicalUri += "/" + percentEncode(op.key, /*keepSlash=*/true);
        std::string cq = canonicalQuery(op.query);

        SignParams sp;
        sp.method = op.method;
        sp.canonicalUri = canonicalUri;
        sp.canonicalQuery = cq;
        sp.headers = op.extraHdrs;
        sp.headers.emplace_back("host", ep.hostHeader());
        sp.payloadHashHex = payloadHash;
        sp.amzDate = amzDate;
        sp.region = cfg.region;
        sp.accessKey = cfg.accessKey;
        sp.secretKey = cfg.secretKey;

        HttpRequest req;
        req.method = op.method;
        req.url = ep.baseUrl() + canonicalUri + (cq.empty() ? "" : "?" + cq);
        for (const auto& kv : op.extraHdrs) req.headerLines.push_back(kv.first + ": " + kv.second);
        req.headerLines.push_back("x-amz-content-sha256: " + payloadHash);
        req.headerLines.push_back("x-amz-date: " + amzDate);
        req.headerLines.push_back("Authorization: " + authorizationHeader(sp));
        req.body = op.body;
        req.bodyLen = op.bodyLen;
        req.noBody = op.noBody;
        req.sink = op.sink;
        req.progress = op.progress;
        req.meta = op.meta;

        bool aborted = false;
        std::string curlErr;
        int rc = tr.execute(req, resp, cancel, aborted, curlErr);

        Result r;
        r.error = mapError(rc, aborted, curlErr, resp, op.headSynthCode);
        r.bytesTransferred = op.body ? std::uint64_t(op.bodyLen) : resp.sinkBytes;
        if (r.error.kind != ErrorKind::none && op.body) r.bytesTransferred = 0;
        return r;
    }
};

Client::Client(Config cfg) : impl_(new Impl(std::move(cfg))) {}
Client::~Client() = default;
Client::Client(Client&&) noexcept = default;
Client& Client::operator=(Client&&) noexcept = default;

void Client::cancel() { impl_->cancel.store(true); }

Result Client::bucketExists(const std::string& bucket, bool& exists) {
    exists = false;
    impl_->cancel.store(false);
    Impl::Op op;
    op.method = "HEAD";
    op.bucket = bucket;
    op.noBody = true;
    HttpResponse resp;
    Result r = impl_->run(op, resp);
    if (r) {
        exists = true;
        return r;
    }
    if (resp.status == 404) {
        exists = false;
        return Result{};  // definite "no" is a success
    }
    return r;  // 403 etc. stay errors — the caller can tell "no bucket" from "no access"
}

Result Client::createBucket(const std::string& bucket) {
    impl_->cancel.store(false);
    Impl::Op op;
    op.method = "PUT";
    op.bucket = bucket;
    // v1 sends no CreateBucketConfiguration body: for us-east-1 and for
    // MinIO/RustFS the LocationConstraint element is not required.
    static const char kEmpty = 0;
    op.body = &kEmpty;
    op.bodyLen = 0;
    HttpResponse resp;
    return impl_->run(op, resp);
}

Result Client::statObject(const std::string& bucket, const std::string& key, ObjectMeta& out) {
    impl_->cancel.store(false);
    Impl::Op op;
    op.method = "HEAD";
    op.bucket = bucket;
    op.key = key;
    op.noBody = true;
    op.meta = &out;
    op.headSynthCode = "NoSuchKey";
    HttpResponse resp;
    Result r = impl_->run(op, resp);
    if (r) out.info.key = key;
    return r;
}

Result Client::deleteObject(const std::string& bucket, const std::string& key) {
    impl_->cancel.store(false);
    Impl::Op op;
    op.method = "DELETE";
    op.bucket = bucket;
    op.key = key;
    op.noBody = false;
    HttpResponse resp;
    Result r = impl_->run(op, resp);
    // S3 semantics: deleting a nonexistent key succeeds (some servers say 404).
    if (!r && resp.status == 404) return Result{};
    return r;
}

// putObject/putFile/getObject/getToFile/listObjects arrive in Tasks 8-9.
Result Client::putObject(const std::string&, const std::string&, const void*, std::size_t,
                         const PutOptions&, const ProgressFn&) {
    return Result{Error{ErrorKind::transport, 0, "", "not implemented", 0}, 0};
}
Result Client::putFile(const std::string&, const std::string&, const std::string&,
                       const PutOptions&, const ProgressFn&) {
    return Result{Error{ErrorKind::transport, 0, "", "not implemented", 0}, 0};
}
Result Client::getObject(const std::string&, const std::string&, const WriteFn&,
                         const ProgressFn&, ObjectMeta*) {
    return Result{Error{ErrorKind::transport, 0, "", "not implemented", 0}, 0};
}
Result Client::getToFile(const std::string&, const std::string&, const std::string&,
                         const ProgressFn&, ObjectMeta*) {
    return Result{Error{ErrorKind::transport, 0, "", "not implemented", 0}, 0};
}
Result Client::listObjects(const std::string&, const std::string&, bool, const ListFn&) {
    return Result{Error{ErrorKind::transport, 0, "", "not implemented", 0}, 0};
}

}  // namespace slims3
```

Note: `Transport` must accept a `Config` whose parsing happened in `Impl` — the `Transport(cfg)` constructor only stores timeouts/TLS/user-agent; ordering in the `Impl` member list is `cfg, ep, tr` — initialize `tr(cfg)` after `cfg` (member order already guarantees it).

- [ ] **Step 4: Run tests** — expected PASS (mapping cases).

- [ ] **Step 5: Commit**

```bash
git add -A && git -c user.name="Sergey Subbotin" -c user.email="ssubbotin@gmail.com" \
  commit -m "Add client core: signed request pipeline, error mapping, bucket/stat/delete ops"
```

---

### Task 8: client — putObject / putFile / getObject / getToFile

**Files:**
- Modify: `src/client.cpp` (replace the four "not implemented" stubs)

**Interfaces:**
- Consumes: `Impl::run`/`Op` from Task 7.
- Produces: the four object-transfer methods per the public header. No new unit tests here — these paths are network-bound; Task 10 exercises them against real servers. (The pure pieces they use — signing, meta, mapping — are already unit-covered.)

- [ ] **Step 1: Implement the four methods** (replace stubs):

```cpp
Result Client::putObject(const std::string& bucket, const std::string& key, const void* data,
                         std::size_t len, const PutOptions& opts, const ProgressFn& progress) {
    impl_->cancel.store(false);
    Impl::Op op;
    op.method = "PUT";
    op.bucket = bucket;
    op.key = key;
    op.body = static_cast<const char*>(data);
    op.bodyLen = len;
    op.progress = progress;
    if (!opts.contentType.empty()) op.extraHdrs.emplace_back("Content-Type", opts.contentType);
    if (!opts.contentEncoding.empty())
        op.extraHdrs.emplace_back("Content-Encoding", opts.contentEncoding);
    for (const auto& kv : opts.extraHeaders) op.extraHdrs.push_back(kv);
    HttpResponse resp;
    return impl_->run(op, resp);
}

Result Client::putFile(const std::string& bucket, const std::string& key,
                       const std::string& filePath, const PutOptions& opts,
                       const ProgressFn& progress) {
    // v1 reads the whole file into memory (targets small/medium objects, and the
    // payload hash must cover the full body anyway). Local I/O failures are
    // reported as transport errors with curlCode == 0.
    std::ifstream f(filePath, std::ios::binary);
    if (!f) return Result{Error{ErrorKind::transport, 0, "", "cannot open file: " + filePath, 0}, 0};
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (f.bad()) return Result{Error{ErrorKind::transport, 0, "", "read failed: " + filePath, 0}, 0};
    return putObject(bucket, key, data.data(), data.size(), opts, progress);
}

Result Client::getObject(const std::string& bucket, const std::string& key, const WriteFn& sink,
                         const ProgressFn& progress, ObjectMeta* meta) {
    impl_->cancel.store(false);
    Impl::Op op;
    op.method = "GET";
    op.bucket = bucket;
    op.key = key;
    op.sink = sink;
    op.progress = progress;
    op.meta = meta;
    HttpResponse resp;
    Result r = impl_->run(op, resp);
    if (r && meta) meta->info.key = key;
    return r;
}

Result Client::getToFile(const std::string& bucket, const std::string& key,
                         const std::string& filePath, const ProgressFn& progress,
                         ObjectMeta* meta) {
    const std::string part = filePath + ".part";
    std::FILE* f = std::fopen(part.c_str(), "wb");
    if (!f) return Result{Error{ErrorKind::transport, 0, "", "cannot open file: " + part, 0}, 0};
    WriteFn sink = [f](const char* data, std::size_t len) {
        return std::fwrite(data, 1, len, f) == len;
    };
    Result r = getObject(bucket, key, sink, progress, meta);
    bool flushOk = (std::fflush(f) == 0);
    std::fclose(f);
    if (r && flushOk) {
        if (std::rename(part.c_str(), filePath.c_str()) != 0) {
            std::remove(part.c_str());
            return Result{Error{ErrorKind::transport, 0, "", "rename failed: " + filePath, 0},
                          r.bytesTransferred};
        }
        return r;
    }
    std::remove(part.c_str());
    if (r && !flushOk)
        return Result{Error{ErrorKind::transport, 0, "", "write failed: " + part, 0}, 0};
    return r;
}
```

- [ ] **Step 2: Build and run existing tests** (no regressions):

Run: `cmake --build build -j && ctest --test-dir build -LE integration --output-on-failure`
Expected: PASS.

- [ ] **Step 3: Commit**

```bash
git add -A && git -c user.name="Sergey Subbotin" -c user.email="ssubbotin@gmail.com" \
  commit -m "Add object transfer: putObject/putFile, getObject/getToFile with atomic .part rename"
```

---

### Task 9: client — listObjects with pagination

**Files:**
- Modify: `src/client.cpp` (replace the `listObjects` stub)

**Interfaces:**
- Consumes: `parseListPage` (Task 5), `urlDecode` (Task 3), `Impl::run` (Task 7).
- Produces: `Client::listObjects` per the public header.

- [ ] **Step 1: Implement** (replace stub):

```cpp
Result Client::listObjects(const std::string& bucket, const std::string& prefix, bool recursive,
                           const ListFn& onObject) {
    impl_->cancel.store(false);
    std::string token;
    while (true) {
        Impl::Op op;
        op.method = "GET";
        op.bucket = bucket;
        op.query = {{"list-type", "2"}, {"encoding-type", "url"}};
        if (!prefix.empty()) op.query.emplace_back("prefix", prefix);
        if (!recursive) op.query.emplace_back("delimiter", "/");
        if (!token.empty()) op.query.emplace_back("continuation-token", token);
        HttpResponse resp;
        Result r = impl_->run(op, resp);  // sink == null -> body accumulated in resp.body
        if (!r) return r;

        detail::ListPage page;
        if (!detail::parseListPage(resp.body, page))
            return Result{Error{ErrorKind::parse, int(resp.status), "",
                                "cannot parse ListObjectsV2 response", 0}, 0};
        for (const auto& e : page.entries) {
            ObjectInfo info;
            info.key = detail::urlDecode(e.key);  // encoding-type=url
            info.size = e.size;
            info.etag = e.etag;
            info.isPrefix = e.isPrefix;
            if (!onObject(info)) return Result{};  // caller stop is not an error
        }
        if (!page.truncated) return Result{};
        if (page.nextToken.empty() || page.nextToken == token)
            return Result{Error{ErrorKind::parse, 0, "",
                                "truncated listing without a fresh continuation token", 0}, 0};
        token = page.nextToken;
    }
}
```

- [ ] **Step 2: Build and run existing tests** — expected PASS, no regressions.

- [ ] **Step 3: Commit**

```bash
git add -A && git -c user.name="Sergey Subbotin" -c user.email="ssubbotin@gmail.com" \
  commit -m "Add listObjects: internal ListObjectsV2 pagination, delimiter mode, url-decoded keys"
```

---

### Task 10: Integration tests — MinIO + RustFS matrix, silent-server regression

**Files:**
- Create: `tests/integration/itest_main.cpp`, `tests/integration/silent_server.py`, `tests/integration/run.sh`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: the full public API.
- Produces: `slims3_itest` binary driven by env vars `SLIMS3_ENDPOINT`, `SLIMS3_ACCESS`, `SLIMS3_SECRET`; silent-server mode via `SLIMS3_SILENT_ENDPOINT`.

- [ ] **Step 1: Add the integration binary to CMake**

Append to `tests/CMakeLists.txt`:
```cmake
add_executable(slims3_itest integration/itest_main.cpp)
target_link_libraries(slims3_itest PRIVATE slims3)
target_include_directories(slims3_itest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME integration COMMAND slims3_itest)
set_tests_properties(integration PROPERTIES LABELS integration)
```

- [ ] **Step 2: Write the integration scenarios**

`tests/integration/itest_main.cpp`:
```cpp
// Integration scenarios driven by environment variables:
//   SLIMS3_ENDPOINT / SLIMS3_ACCESS / SLIMS3_SECRET  - target server (full run)
//   SLIMS3_SILENT_ENDPOINT                            - silent-server mode only
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <chrono>
#include <cstdlib>
#include <string>
#include <vector>

#include "doctest.h"
#include "slims3/slims3.hpp"

using namespace slims3;

static std::string env(const char* n) {
    const char* v = std::getenv(n);
    return v ? v : "";
}

static Config baseConfig() {
    Config c;
    c.endpoint = env("SLIMS3_ENDPOINT");
    c.accessKey = env("SLIMS3_ACCESS");
    c.secretKey = env("SLIMS3_SECRET");
    return c;
}

static std::string bucketName() {
    static std::string b = "slims3-itest-" + std::to_string(std::time(nullptr));
    return b;
}

TEST_CASE("silent server: stall guard fires instead of hanging forever") {
    std::string ep = env("SLIMS3_SILENT_ENDPOINT");
    if (ep.empty()) return;  // not in silent mode
    Config c;
    c.endpoint = ep;
    c.accessKey = "x";
    c.secretKey = "y";
    c.connectTimeoutSec = 5;
    c.lowSpeedTimeSec = 3;
    Client cl(c);
    auto t0 = std::chrono::steady_clock::now();
    bool exists = false;
    Result r = cl.bucketExists("any", exists);
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - t0).count();
    CHECK_FALSE(r);
    CHECK(r.error.kind == ErrorKind::transport);
    CHECK(secs < 30);  // the whole point of this library
}

TEST_CASE("full server matrix") {
    if (env("SLIMS3_ENDPOINT").empty()) return;  // silent-only mode
    Client cl(baseConfig());
    const std::string b = bucketName();

    // -- bucket lifecycle
    bool exists = true;
    REQUIRE(cl.bucketExists(b, exists));
    CHECK_FALSE(exists);
    REQUIRE(cl.createBucket(b));
    REQUIRE(cl.bucketExists(b, exists));
    CHECK(exists);

    // -- wrong credentials are an error, not "false"
    Config bad = baseConfig();
    bad.secretKey = "wrong-secret-key-000000";
    Client badCl(bad);
    bool e2 = false;
    Result rBad = badCl.bucketExists(b, e2);
    CHECK_FALSE(rBad);
    CHECK(rBad.error.httpStatus == 403);

    // -- put / stat / get round-trip with Content-Encoding passthrough
    std::string payload(100000, '\x5a');
    for (int i = 0; i < 1000; ++i) payload[std::size_t(i) * 100] = char(i & 0xff);
    PutOptions po;
    po.contentType = "application/octet-stream";
    po.contentEncoding = "zstd";
    REQUIRE(cl.putObject(b, "dir/k ey+with=chars.bin", payload.data(), payload.size(), po));

    ObjectMeta meta;
    REQUIRE(cl.statObject(b, "dir/k ey+with=chars.bin", meta));
    CHECK(meta.info.size == payload.size());
    CHECK(meta.contentEncoding == "zstd");
    CHECK_FALSE(meta.info.etag.empty());

    std::string got;
    ObjectMeta gmeta;
    Result rGet = cl.getObject(b, "dir/k ey+with=chars.bin",
                               [&](const char* d, std::size_t n) {
                                   got.append(d, n);
                                   return true;
                               },
                               {}, &gmeta);
    REQUIRE(rGet);
    CHECK(rGet.bytesTransferred == payload.size());
    CHECK(got == payload);
    CHECK(gmeta.contentEncoding == "zstd");

    // -- getToFile atomic write
    REQUIRE(cl.getToFile(b, "dir/k ey+with=chars.bin", "/tmp/slims3_itest.bin"));

    // -- stat of a missing key
    ObjectMeta missing;
    Result rMiss = cl.statObject(b, "no/such/key", missing);
    CHECK_FALSE(rMiss);
    CHECK(rMiss.error.httpStatus == 404);
    CHECK(rMiss.error.s3Code == "NoSuchKey");

    // -- listing: pagination beyond one page (>1000 keys)
    for (int i = 0; i < 1050; ++i) {
        std::string k = "many/obj-" + std::to_string(10000 + i);
        REQUIRE(cl.putObject(b, k, "x", 1));
    }
    std::size_t seen = 0;
    REQUIRE(cl.listObjects(b, "many/", true, [&](const ObjectInfo&) {
        ++seen;
        return true;
    }));
    CHECK(seen == 1050);

    // -- non-recursive listing yields CommonPrefixes
    bool sawPrefix = false, sawKey = false;
    REQUIRE(cl.listObjects(b, "dir/", false, [&](const ObjectInfo& oi) {
        if (oi.isPrefix) sawPrefix = true;   // none expected under dir/ (flat)
        if (!oi.isPrefix) sawKey = true;
        return true;
    }));
    CHECK(sawKey);
    (void)sawPrefix;
    bool sawManyPrefix = false;
    REQUIRE(cl.listObjects(b, "", false, [&](const ObjectInfo& oi) {
        if (oi.isPrefix && oi.key == "many/") sawManyPrefix = true;
        return true;
    }));
    CHECK(sawManyPrefix);

    // -- early stop is not an error
    std::size_t firstOnly = 0;
    REQUIRE(cl.listObjects(b, "many/", true, [&](const ObjectInfo&) {
        ++firstOnly;
        return false;
    }));
    CHECK(firstOnly == 1);

    // -- cancellation mid-download
    std::size_t chunks = 0;
    Result rCancel = cl.getObject(b, "dir/k ey+with=chars.bin",
                                  [&](const char*, std::size_t) { return ++chunks < 2; });
    CHECK_FALSE(rCancel);
    CHECK(rCancel.error.kind == ErrorKind::cancelled);

    // -- delete: real key, then the same (already gone) key
    REQUIRE(cl.deleteObject(b, "dir/k ey+with=chars.bin"));
    REQUIRE(cl.deleteObject(b, "dir/k ey+with=chars.bin"));
}
```

- [ ] **Step 3: Silent server + runner**

`tests/integration/silent_server.py`:
```python
#!/usr/bin/env python3
"""Accepts TCP connections and never responds - a black-hole S3 endpoint."""
import socket
import sys

port = int(sys.argv[1]) if len(sys.argv) > 1 else 9999
srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", port))
srv.listen(16)
print(f"silent server on 127.0.0.1:{port}", flush=True)
conns = []
while True:
    c, _ = srv.accept()
    conns.append(c)  # keep the socket open, read nothing, send nothing
```

`tests/integration/run.sh`:
```bash
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
```
`chmod +x tests/integration/run.sh`

- [ ] **Step 4: Run locally against MinIO and the silent server**

```bash
docker run -d --name slims3-minio -p 19000:9000 \
  -e MINIO_ROOT_USER=testkey -e MINIO_ROOT_PASSWORD=testsecret123 \
  minio/minio:latest server /data
cmake --build build -j
sleep 3
tests/integration/run.sh http://127.0.0.1:19000 testkey testsecret123
tests/integration/run.sh --silent
docker rm -f slims3-minio
```
Expected: both invocations end with `[doctest] Status: SUCCESS!`. The silent run completes in well under 30 seconds.

- [ ] **Step 5: Commit**

```bash
git add -A && git -c user.name="Sergey Subbotin" -c user.email="ssubbotin@gmail.com" \
  commit -m "Add integration suite: server matrix scenarios and silent-server stall regression"
```

---

### Task 11: CI workflow and README completion

**Files:**
- Create: `.github/workflows/ci.yml`
- Modify: `README.md` (usage example, build instructions, drop "design phase")

- [ ] **Step 1: Write the workflow**

`.github/workflows/ci.yml`:
```yaml
name: ci
on:
  push:
    branches: [main]
  pull_request:

jobs:
  unit:
    runs-on: ubuntu-24.04
    strategy:
      matrix:
        toolchain: [gcc, clang, asan]
    steps:
      - uses: actions/checkout@v4
      - run: sudo apt-get update && sudo apt-get install -y libcurl4-openssl-dev
      - name: Configure
        run: |
          case "${{ matrix.toolchain }}" in
            gcc)   cmake -B build -DSLIMS3_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release ;;
            clang) cmake -B build -DSLIMS3_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release \
                     -DCMAKE_CXX_COMPILER=clang++ ;;
            asan)  cmake -B build -DSLIMS3_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug \
                     -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
                     -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" ;;
          esac
      - run: cmake --build build -j
      - run: ctest --test-dir build -LE integration --output-on-failure
      - name: Silent-server regression
        run: tests/integration/run.sh --silent

  integration:
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4
      - run: sudo apt-get update && sudo apt-get install -y libcurl4-openssl-dev
      - run: cmake -B build -DSLIMS3_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
      - name: Start MinIO
        run: |
          docker run -d --name minio -p 19000:9000 \
            -e MINIO_ROOT_USER=testkey -e MINIO_ROOT_PASSWORD=testsecret123 \
            minio/minio:latest server /data
      - name: Start RustFS
        run: |
          docker run -d --name rustfs -p 19010:9000 \
            -e RUSTFS_ADDRESS=0.0.0.0:9000 \
            -e RUSTFS_ACCESS_KEY=testkey -e RUSTFS_SECRET_KEY=testsecret123 \
            rustfs/rustfs:latest
      - name: Wait for servers
        run: |
          for p in 19000 19010; do
            for i in $(seq 1 30); do
              curl -sf "http://127.0.0.1:$p" -o /dev/null && break || true
              # any HTTP response (incl. 403) means the port is serving
              code=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$p" || echo 000)
              [ "$code" != "000" ] && break
              sleep 2
            done
          done
      - name: Run against MinIO
        run: tests/integration/run.sh http://127.0.0.1:19000 testkey testsecret123
      - name: Run against RustFS
        run: tests/integration/run.sh http://127.0.0.1:19010 testkey testsecret123
```

Note: if the `rustfs/rustfs:latest` image tag or its env variable names differ at execution time, check `https://hub.docker.com/r/rustfs/rustfs` and adjust only the `Start RustFS` step; the test binary itself is server-agnostic.

- [ ] **Step 2: Update README** — replace the "Status: design phase" paragraph with a build/usage section:

````markdown
## Build

```bash
cmake -B build && cmake --build build -j
# or as a subproject: add_subdirectory(slim-s3) + target_link_libraries(app slims3::slims3)
```

## Usage

```cpp
#include <slims3/slims3.hpp>

slims3::Config cfg;
cfg.endpoint = "http://127.0.0.1:9000";
cfg.accessKey = "minioadmin";
cfg.secretKey = "minioadmin";
slims3::Client s3(cfg);

slims3::PutOptions po;
po.contentType = "application/octet-stream";
std::string data = "hello";
if (auto r = s3.putObject("bucket", "path/key.bin", data.data(), data.size(), po); !r)
    fprintf(stderr, "%s\n", r.error.message.c_str());

s3.getToFile("bucket", "path/key.bin", "/tmp/key.bin");
```

Errors carry `httpStatus` and `s3Code` — retry policy stays in your hands:

```cpp
bool retryable(const slims3::Error& e) {
    return e.kind == slims3::ErrorKind::transport ||
           (e.kind != slims3::ErrorKind::cancelled && e.httpStatus >= 500);
}
```
````

- [ ] **Step 3: Push and verify CI**

```bash
git add -A && git -c user.name="Sergey Subbotin" -c user.email="ssubbotin@gmail.com" \
  commit -m "Add GitHub Actions CI: unit matrix with sanitizers, MinIO+RustFS integration"
git push
gh run watch --exit-status || gh run view --log-failed
```
Expected: all jobs green. If the RustFS container step fails, fix per the note in Step 1 and push again.

- [ ] **Step 4: Tag**

```bash
git tag v0.1.0 && git push origin v0.1.0
```

---

## Plan Self-Review (done at write time)

- **Spec coverage:** DESIGN §4 API — Tasks 1, 7–9; §5 modules — Tasks 2–6; §6 error model — Task 7 (unit-tested); §7 concurrency — Task 6 (atomic cancel) + Task 7 (`cancel()`); §8 build/packaging — Task 1; §9 testing — Tasks 2–5 (unit), 10 (integration + silent server), 11 (CI matrix + sanitizers). Non-goals respected: no retries, no multipart, path-style only.
- **Known deferred check:** RustFS docker image tag/env names in Task 11 are best-known values with an explicit verification note — the only externally-owned detail in the plan.
- **Type consistency:** `Impl::Op`/`run()` signatures used in Tasks 8–9 match Task 7; `ListPage`/`ListEntry` names match Tasks 5 and 9; `SignParams` fields match Tasks 4 and 7.
