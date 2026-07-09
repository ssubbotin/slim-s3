# slim-s3 — Design

A slim S3 client for C++17. One dependency: libcurl. No SDK heft.

Repository: `slim-s3`. C++ namespace, header directory, and CMake package: `slims3`
(dashes are not valid in C++ identifiers).

## 1. Motivation

Existing options for talking to S3-compatible storage from C++ are either heavy or
fragile:

- **aws-sdk-cpp** is mature but drags in the aws-crt/aws-c-* dependency tree, slow
  builds, and an initialization ceremony — far more than a service needs to PUT and
  GET a handful of objects.
- **minio-cpp** is thin but has release gaps and transport-level defects: as of
  v0.3.0/v0.4.0 the event loop calls `select()` with no timeout and sets no libcurl
  timeout options at all, so a silent socket or an empty fd-set window blocks the
  calling thread forever. Its API is also mid-rewrite on `main` (unreleased,
  breaking).
- Small clients on GitHub are learning projects (self-described "not
  production-ready"), abandoned, or require C++23.

slim-s3 exists to be the boring, dependable middle: a small, auditable client with
first-class timeouts, cancellation, progress, and structured errors — the qualities
production services actually page on.

### Why not sign with CURLOPT_AWS_SIGV4?

libcurl has built-in SigV4 signing, but its behavior depends on the libcurl version
the consumer happens to link: streamed bodies are signed as `UNSIGNED-PAYLOAD`,
and query canonicalization had real bugs (no parameter sorting — curl/curl#9717,
empty parameters — #10129, `=` inside values — #13754; list continuation tokens
contain `=`). A library whose correctness varies with the transport's patch level
is not dependable. slim-s3 signs requests itself (~300 lines, verified against the
official AWS SigV4 test vectors) and uses libcurl strictly as an HTTP transport.

## 2. Goals

- Cover the small API surface most services actually use (see §4).
- C++17, single dependency (libcurl ≥ 7.61; tested against 7.81+).
- Signed payloads always; signing independent of libcurl version.
- Timeouts and a low-speed stall guard **on by default** — no operation can hang
  forever out of the box.
- Cooperative cancellation from another thread.
- Progress callbacks for uploads and downloads.
- Structured errors: HTTP status + S3 error code + message, so callers can build
  retry policies.
- Connection reuse across requests (one persistent curl easy handle per client).
- Tested against MinIO **and** RustFS in CI, plus AWS SigV4 test vectors.

## 3. Non-goals (v1)

- **Multipart upload** — objects are limited to 5 GB (single PUT). Target use is
  objects in the KB–100 MB range, uploaded from memory or small files.
- **Retries** — the caller owns retry policy; slim-s3 returns structured errors to
  classify on. (Rationale: retry budgets, backoff, and idempotency windows are
  application decisions; baking them in makes simple things opaque.) README shows a
  reference retry loop.
- **Async / thread-pool APIs** — the client is synchronous; run it on your worker
  thread. One `Client` = one operation at a time.
- **Virtual-hosted addressing** — v1 is path-style only (`http://host:9000/bucket/key`),
  which is what MinIO, RustFS, and other self-hosted stores use. Virtual-hosted
  style is on the roadmap.
- **Presigned URLs, bucket policies, ACLs, versioning, SSE** — out of scope.

## 4. Public API

Single public header `<slims3/slims3.hpp>`:

```cpp
namespace slims3 {

struct Config {
    std::string endpoint;             // "http://rustfs:9000" or "https://s3.example.com"
    std::string region = "us-east-1";
    std::string accessKey;
    std::string secretKey;

    long connectTimeoutSec  = 30;     // TCP/TLS connect budget
    long operationTimeoutSec = 0;     // whole-request cap; 0 = uncapped (stall guard still applies)
    long lowSpeedLimitBps   = 1;      // stall guard: abort when transfer speed
    long lowSpeedTimeSec    = 60;     //   stays below limit for this long

    std::string caBundlePath;         // optional custom CA bundle
    bool tlsVerify = true;
    std::string userAgent;            // empty -> "slim-s3/<library version>"
};

enum class ErrorKind {
    none,        // success
    transport,   // curl-level failure: DNS, connect, timeout, stall guard
    http,        // HTTP error status without a parseable S3 error body
    s3,          // S3 error response (code + message parsed from XML)
    parse,       // malformed response body
    cancelled,   // aborted via cancel() or a callback returning false
};

struct Error {
    ErrorKind   kind = ErrorKind::none;
    int         httpStatus = 0;   // 0 when no response was received
    std::string s3Code;           // "NoSuchKey", "AccessDenied", ... (kind == s3)
    std::string message;          // human-readable, includes curl detail for transport
    int         curlCode = 0;     // CURLcode (kind == transport)
};

struct Result {
    Error         error;
    std::uint64_t bytesTransferred = 0;
    explicit operator bool() const { return error.kind == ErrorKind::none; }
};

struct ObjectInfo {
    std::string   key;
    std::uint64_t size = 0;
    std::string   etag;
    bool          isPrefix = false;  // true for CommonPrefixes entries (non-recursive listing)
};

struct ObjectMeta {
    ObjectInfo  info;
    std::string contentType;
    std::string contentEncoding;     // as stored by the server; empty if none
};

struct PutOptions {
    std::string contentType;
    std::string contentEncoding;     // e.g. "zstd"; stored as object metadata
    std::vector<std::pair<std::string, std::string>> extraHeaders;
};

// Callbacks return false to cancel the operation (=> ErrorKind::cancelled).
using WriteFn    = std::function<bool(const char* data, std::size_t len)>;
using ProgressFn = std::function<bool(std::uint64_t done, std::uint64_t total)>;
using ListFn     = std::function<bool(const ObjectInfo&)>;   // false stops listing (not an error)

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

    Result putObject(const std::string& bucket, const std::string& key,
                     const void* data, std::size_t len,
                     const PutOptions& opts = {}, const ProgressFn& progress = {});
    Result putFile(const std::string& bucket, const std::string& key,
                   const std::string& filePath,
                   const PutOptions& opts = {}, const ProgressFn& progress = {});

    Result getObject(const std::string& bucket, const std::string& key,
                     const WriteFn& sink,
                     const ProgressFn& progress = {}, ObjectMeta* meta = nullptr);
    Result getToFile(const std::string& bucket, const std::string& key,
                     const std::string& filePath,
                     const ProgressFn& progress = {}, ObjectMeta* meta = nullptr);

    Result statObject(const std::string& bucket, const std::string& key, ObjectMeta& out);
    Result deleteObject(const std::string& bucket, const std::string& key);

    Result listObjects(const std::string& bucket, const std::string& prefix,
                       bool recursive, const ListFn& onObject);

    // Thread-safe. Aborts the operation currently running on this client;
    // it returns ErrorKind::cancelled at the next progress tick.
    void cancel();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace slims3
```

### Operation → HTTP mapping and semantics

| Operation      | Request                                             | Notes |
|----------------|------------------------------------------------------|-------|
| `bucketExists` | `HEAD /{bucket}`                                     | 200 → `exists=true`; 404 → `exists=false`, success. 403 and others → error (unlike clients that swallow the code, the caller can distinguish "no bucket" from "no permission"). |
| `createBucket` | `PUT /{bucket}`                                      | 200 → ok; `BucketAlreadyOwnedByYou` surfaced as error with that s3Code (caller decides). |
| `putObject`    | `PUT /{bucket}/{key}` with body                      | Payload SHA-256 is computed and signed. `Content-Type`, `Content-Encoding`, extra headers included in the signed request. |
| `putFile`      | same                                                 | Reads the file fully into memory (v1 targets small/medium objects), then as `putObject`. |
| `getObject`    | `GET /{bucket}/{key}`                                | Body streamed to `sink` chunk by chunk. Response headers (`Content-Type`, `Content-Encoding`, `Content-Length`, `ETag`) reported via `meta` before the first sink call. On HTTP error the body is captured internally for S3 error parsing instead of being fed to the sink. |
| `getToFile`    | same                                                 | Streams to `filePath + ".part"` opened exclusively (`O_EXCL`/`O_NOFOLLOW` on POSIX — refuses to follow a symlink or reuse a pre-existing/stale temp file; see Implementation notes), renames over `filePath` on success, removes the temp file on failure/cancel. |
| `statObject`   | `HEAD /{bucket}/{key}`                               | 404 → error `{s3, 404, "NoSuchKey"}` (HEAD has no body; code synthesized from status). |
| `deleteObject` | `DELETE /{bucket}/{key}`                             | 204 → ok. Deleting a nonexistent key is success (S3 semantics). |
| `listObjects`  | `GET /{bucket}?list-type=2&encoding-type=url&prefix=…[&delimiter=/][&continuation-token=…]` | Pagination handled internally (loops on `IsTruncated`/`NextContinuationToken`, capped at 100000 pages — see Implementation notes). `recursive=false` sets `delimiter=/` and emits `CommonPrefixes` entries with `isPrefix=true`. Keys are URL-decoded (`encoding-type=url`). |

Header names/values passed via `PutOptions` (`contentType`, `contentEncoding`,
`extraHeaders`) are validated before signing: any byte `< 0x20` (CR, LF, NUL)
is rejected, and header names additionally reject `:`. A caller forwarding
untrusted values (e.g. proxied `x-amz-meta-*` metadata) cannot inject extra
header lines or corrupt the signature this way; a rejected header returns
`ErrorKind::transport` before any network I/O.

The same pre-network validation covers the request target and header set:

- **Bucket names** must be non-empty and within `[A-Za-z0-9._-]` (a deliberate
  superset of AWS's official `[a-z0-9.-]` — legacy buckets and some
  S3-compatible stores allow more; strictness beyond URL/signature safety is
  the server's job). Anything else — a slash (would silently retarget:
  bucket `"b/x"` becomes bucket `b`, key `x`), `?`/`#` (starts the
  query/fragment early), `%`, spaces, control bytes — is rejected.
- **Object keys** must be non-empty for every object-level operation. Without
  this, `deleteObject(bucket, "")` would send `DELETE /{bucket}` — the
  DeleteBucket operation — and `putObject`/`getObject` with an empty key would
  create the bucket / stream the bucket listing into the sink.
- **Duplicate and library-managed header names** are rejected: SigV4 requires
  a repeated header's values to be comma-joined under one name, so emitting a
  name twice guarantees `SignatureDoesNotMatch`; and `host`, `authorization`,
  `x-amz-date`, `x-amz-content-sha256`, `content-length`,
  `transfer-encoding`, and `expect` are set by the library.

`Result.bytesTransferred` is the object body size moved over the wire (uploads:
bytes sent; downloads: bytes written to sink/file) — callers can verify it against
an expected size.

## 5. Architecture

Five internal modules, each independently unit-testable:

```
include/slims3/slims3.hpp     public API (the only installed header)
src/client.cpp                operations, pagination, error mapping
src/transport.{hpp,cpp}       libcurl wrapper: one persistent easy handle,
                              timeouts, stall guard, progress, cancellation
src/signer.{hpp,cpp}          SigV4: canonical request, string-to-sign, signature
src/sha256.{hpp,cpp}          SHA-256 + HMAC-SHA256 (vendored public-domain core)
src/xml_lite.{hpp,cpp}        targeted extraction for the two S3 response shapes
src/uri.{hpp,cpp}             S3 URI/query percent-encoding rules
```

### transport

- One `CURL*` easy handle per `Client`, created lazily, `curl_easy_reset` between
  requests. libcurl's connection cache lives on the handle, so **connections, DNS
  results, and TLS sessions are reused across requests** (minio-cpp created a fresh
  handle per request — a full reconnect every time).
- Every request sets: `CURLOPT_CONNECTTIMEOUT`, `CURLOPT_LOW_SPEED_LIMIT`/`TIME`
  (stall guard), optional `CURLOPT_TIMEOUT` (operation cap),
  `CURLOPT_NOSIGNAL`, `CURLOPT_XFERINFOFUNCTION`.
- Cancellation: `cancel()` sets an atomic flag; the xferinfo callback checks it
  (and the user's `ProgressFn` result) and returns nonzero →
  `CURLE_ABORTED_BY_CALLBACK` → `ErrorKind::cancelled`. libcurl invokes xferinfo
  throughout the transfer, including the connect phase, so cancellation is
  responsive even before the first byte.
- Uploads use `CURLOPT_UPLOAD` + read callback over the in-memory payload;
  downloads use a write callback that routes to the sink or, on error status, to an
  internal buffer for S3 error parsing.
- Response caps guard against a malicious or misbehaving server: a response
  header count cap, a cumulative header-bytes cap, a status-line/interim-
  response cap, and an in-memory body cap on the error/no-sink path. Any cap
  tripping aborts the transfer as `ErrorKind::transport`, never a silent
  truncation — values and rationale in Implementation notes.

### signer

Own SigV4 implementation, libcurl-version-independent:

- Canonical URI: path segments percent-encoded per S3 rules (encode everything
  except unreserved characters; `/` kept as separator; no double-encoding for
  path-style requests).
- Canonical query: parameters sorted by name (then value), keys and values
  percent-encoded, empty values kept as `name=`.
- Signed headers: `host`, `x-amz-content-sha256`, `x-amz-date`, plus
  `content-type`/`content-encoding` and any extra headers when present.
- Payload hash: real SHA-256 of the body (empty-body hash for GET/HEAD/DELETE).
  `UNSIGNED-PAYLOAD` is not used.
- Verified against the official `aws-sig-v4-test-suite` vectors in unit tests.

### sha256

Vendored public-domain SHA-256 core (~150 lines) + HMAC per RFC 2104. Keeps the
dependency list at exactly libcurl. Verified against NIST/RFC test vectors.

### xml_lite

Not a general XML parser — a targeted extractor for exactly two machine-generated
shapes: `<Error><Code>…<Message>…` and `ListBucketResult` (`Contents` →
`Key/Size/ETag`, `CommonPrefixes` → `Prefix`, `IsTruncated`,
`NextContinuationToken`). Handles the five predefined XML entities and numeric
character references; a reference outside the valid Unicode scalar range (`0`,
`> 0x10FFFF`, or the UTF-16 surrogate block `0xD800`–`0xDFFF`) is rejected —
left un-decoded rather than mis-encoded to invalid UTF-8. Listing is requested
with `encoding-type=url`, so keys arrive percent-encoded ASCII and exotic-key
edge cases reduce to URL decoding. Unit-tested
on captured responses from MinIO, RustFS, and AWS documentation examples. If a
server ever produces XML this extractor cannot handle, it fails loudly
(`ErrorKind::parse`), and swapping in a full parser stays an internal change.

## 6. Error model

Exactly one of the `ErrorKind` values describes every outcome. Success is
strictly a 2xx status (`[200,300)`); everything else — including a 3xx
redirect, which slim-s3 never follows (`CURLOPT_FOLLOWLOCATION` is off) — is
an error. `mapError` applies these rules in order; the first match wins:

1. **Aborted** — `cancel()` was called, or a `WriteFn`/`ProgressFn` returned
   `false` → `cancelled`. Takes priority even when curl also reports a
   transport-level failure for the same abort.
2. **Transport** — curl reported a failure: DNS, connect, TLS, timeout, stall
   guard, or one of the response caps in the Implementation notes below →
   `transport` (`curlCode`, `message` from `curl_easy_strerror` + detail).
   `httpStatus = 0`.
3. **S3** — HTTP status ≥ 400 with a parseable S3 error XML body → `s3`
   (`httpStatus`, `s3Code`, `message` from the body); for HEAD requests, which
   carry no body, the code is synthesized on 404 (`"NoSuchKey"`/
   `"NoSuchBucket"`).
4. **HTTP** — any other non-2xx status: ≥ 400 without a parseable body, or a
   stray 3xx/1xx → `http` (`httpStatus`, body snippet in `message`; a
   redirect status is flagged `"(redirect not followed)"`).
5. **Parse** — 2xx but the body failed to parse (listing) → `parse`.

This carries everything a caller's retry policy needs (`transport` and 5xx →
retryable; 4xx → usually fatal) without the library imposing one.

## 7. Concurrency model

- A `Client` runs one operation at a time; concurrent calls on one instance are
  not supported (create one client per worker thread — they are cheap; the curl
  handle is the only state).
- `cancel()` is the only method safe to call from another thread.
- No global state. `curl_global_init` is performed once, thread-safely, on first
  client construction (libcurl ≥ 7.84 does this internally; for older versions a
  `std::call_once` guard wraps it).

## 8. Build & packaging

- CMake ≥ 3.16, C++17, `find_package(CURL REQUIRED)`.
- Static library by default, `BUILD_SHARED_LIBS` respected. Target
  `slims3::slims3`; `install()` + CMake config package so consumers use
  `find_package(slims3)`.
- Options: `SLIMS3_BUILD_TESTS` (default OFF when consumed via
  `add_subdirectory`/FetchContent).
- No generated code, no submodules; FetchContent-friendly.

## 9. Testing

**Unit (no network):**
- SigV4 signer against the official AWS `aws-sig-v4-test-suite` vectors (vendored
  under `tests/data/`).
- SHA-256/HMAC against NIST + RFC 4231 vectors.
- URI/query encoding: spaces, unicode, `=`, `+`, `~`, empty values, sort order.
- `xml_lite` on captured MinIO/RustFS/AWS responses, entity decoding
  (including the surrogate-rejection case above), truncated and hostile
  inputs (must fail cleanly, never crash).
- Header-name/value validation (`validHeaderToken`): CR/LF/NUL rejection, `:`
  rejection in names, accepted-vs-rejected boundary cases; bucket-name
  validation (`validBucketName`) boundary cases; duplicate/reserved header
  rejection.
- Client-level mapping and edge cases: endpoint parsing (explicit scheme
  required, IPv6 literals, malformed inputs), null-data PUT, empty-key and
  invalid-bucket rejection for every operation, HEAD 404 code synthesis,
  delete-of-missing-key success.

**Integration (docker, run in CI):** matrix over **MinIO** and **RustFS**, plus a
raw-socket malicious/misbehaving-server stub (`tests/integration/stub_server.py`):
- bucket create / exists / not-exists / wrong-credentials (403 surfaces, not
  swallowed);
- put → stat → get → byte-for-byte round-trip → delete;
- `Content-Encoding` pass-through: stored and returned;
- listing: >1000 objects (pagination), prefix filtering, recursive vs
  delimiter mode, keys with spaces/unicode/`=`;
- cancellation mid-download and mid-upload, from another thread and via a
  callback returning `false`;
- **silent-server test**: a stub accepts the TCP connection and never responds —
  the operation must fail in ≈`lowSpeedTimeSec`, not hang (the minio-cpp lesson,
  now a permanent regression test);
- **stub scenarios** exercising the response caps and edge cases a real
  MinIO/RustFS never triggers: header-count flood, header-byte flood,
  status-line flood, body-size overflow, delete of a missing key (404 →
  success), malformed/truncated listing XML, an empty or repeated
  continuation token, and a failing PUT (500);
- filesystem-failure paths: `getToFile` against a short write / disk-full
  condition (reported as `transport`, not `cancelled`).

**CI (GitHub Actions):** three jobs — `unit` (gcc, clang, and an ASan/UBSan
build; unit tests plus the silent-server regression on every push),
`integration` (the MinIO/RustFS/stub matrix above), and `coverage` (an
instrumented `--coverage` build re-running the same unit, integration, and
stub suites under `gcovr` with branch/decision coverage, currently ~98.5%
line / ~88% decision on `src/`+`include/`, publishing an HTML report as a
build artifact).

## 10. Implementation notes

Decisions made during the build that refine or extend §2–§9 above.

**Response caps (server-driven DoS defenses).** A malicious or merely
misbehaving server must not be able to hang or exhaust memory in a client
that otherwise has generous timeouts. Four caps, all enforced in
`transport.cpp`, all mapping to `ErrorKind::transport` (abort, never a silent
truncation):
- response header count: 2000 lines;
- cumulative response header bytes: 256 KiB — deliberately set below
  libcurl's own `MAX_HTTP_RESP_HEADER_SIZE` (~300 KiB) so slim-s3's cap fires
  first and owns the error message instead of a generic curl failure;
- status-line/interim-response blocks: 32 — guards against a server streaming
  spoofed or interim (1xx) status lines forever, which would otherwise reset
  the header count/byte counters (those two are cumulative for the whole
  exchange, not per status-line block) and slip past the low-speed stall
  guard with a trickle just above its floor;
- in-memory response body: 8 MiB, on the error/no-sink path only (S3 error
  bodies and whole-body captures for listing — the streaming
  `getObject`/`getToFile` sink path is not capped here; the caller's
  sink/progress callback is the cancellation point for those).

`listObjects` pagination is capped separately at 100000 pages (100M objects
at 1000 keys/page): a per-request timeout doesn't bound a whole paginated
operation, and this bounds it against a server handing out continuation
tokens forever.

**Caller-input hardening.** `putObject` validates `PutOptions.contentType`,
`contentEncoding`, and every `extraHeaders` name/value with `validHeaderToken`
before signing: any byte `< 0x20` (covers CR, LF, NUL) is rejected, and header
names additionally reject `:`. This runs before the canonical request is
built, so a caller forwarding untrusted values (e.g. proxied `x-amz-meta-*`
metadata) cannot inject extra header lines or corrupt the signature this way.
Rejection returns `ErrorKind::transport` with no network I/O performed.

Beyond header tokens (all rejected the same way, before any network I/O; see
§4 for the full rules): bucket names are validated with `validBucketName`,
object keys must be non-empty (an empty key would retarget the request at the
bucket itself — `DELETE /{bucket}` is DeleteBucket), and duplicate or
library-managed header names in `PutOptions` are rejected.

**Endpoint parsing.** The endpoint must carry an explicit `http://` or
`https://` scheme — a scheme-less endpoint is rejected rather than silently
defaulting to plaintext HTTP (this client carries credentials). Bracketed
IPv6 literals are supported (`http://[::1]:9000`, `https://[2001:db8::1]`);
the brackets remain part of the host in both the URL and the signed `host`
header. No path suffix is allowed (path-style addressing owns the path).

**`getToFile` atomicity.** The temp file is opened with
`O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW` (mode 0600) on POSIX: `O_EXCL` refuses to
reuse a pre-existing path — including a stale `.part` left by a crashed prior
download, which now blocks the new download with a clear error instead of
being silently truncated — and `O_NOFOLLOW` refuses to follow a pre-planted
symlink. The file is renamed over the destination only when the transfer, the
final `fflush`, and an `fsync` all succeed — the `fsync` closes the classic
rename-durability gap where a crash shortly after success leaves a truncated
file under the final name (`rename` is atomic in the namespace but does not
flush data blocks). The temp file is removed on any failure, cancellation, or
write error. Windows v1 falls back to a plain `fopen` (no O_EXCL/O_NOFOLLOW
equivalent wired up yet) — CI and the hardening above currently target Linux.

**Cancellation & progress.** `cancel()` sets an atomic flag read by the
xferinfo callback, which libcurl invokes throughout the transfer including
the connect phase; a `WriteFn`/`ProgressFn` returning `false` is funneled
through the same abort path. Both map to `ErrorKind::cancelled`. `cancel()`
is the only method safe to call from a thread other than the one running the
current operation; each operation call resets the flag on entry, so a client
can be reused for a following operation after a cancel.

**Testing.** The raw-socket stub server (`tests/integration/stub_server.py`,
driven by `tests/integration/run_stub.sh`) is what makes the caps above
testable at all — no real S3-compatible server will misbehave this way. The
CI `coverage` job re-runs the unit, MinIO/RustFS, silent-server, and stub
suites under `--coverage` instrumentation and publishes an HTML report; see
§9 for current numbers.

**Dependencies.** Confirmed as designed: the library links exactly
`CURL::libcurl` (`CMakeLists.txt`); doctest (vendored, tests only) and the
python3 stub/silent-server scripts are test-only, not part of the installed
package.

## 11. Roadmap (post-v1)

- Virtual-hosted addressing style (needed for AWS-first users).
- Multipart upload for >5 GB objects / bounded-memory `putFile`.
- Optional response body streaming for `putFile` without full read into memory.
- Presigned URL generation (pure signer work, no transport).

## 12. Downstream (out of repo scope)

The first production consumer is a Qt service that wraps slim-s3 behind its
existing `AbstractClient` interface (QString/QByteArray conversion + its own
`RetryPolicy` on top of the structured errors). That wrapper, its vcpkg port, and
the migration off minio-cpp live in the consumer's tree and are tracked there.
