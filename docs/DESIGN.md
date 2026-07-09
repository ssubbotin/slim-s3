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
    std::string userAgent = "slim-s3/<version>";
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
| `getToFile`    | same                                                 | Streams to `filePath + ".part"`, renames over `filePath` on success, removes the temp file on failure/cancel. |
| `statObject`   | `HEAD /{bucket}/{key}`                               | 404 → error `{s3, 404, "NoSuchKey"}` (HEAD has no body; code synthesized from status). |
| `deleteObject` | `DELETE /{bucket}/{key}`                             | 204 → ok. Deleting a nonexistent key is success (S3 semantics). |
| `listObjects`  | `GET /{bucket}?list-type=2&encoding-type=url&prefix=…[&delimiter=/][&continuation-token=…]` | Pagination handled internally (loops on `IsTruncated`/`NextContinuationToken`). `recursive=false` sets `delimiter=/` and emits `CommonPrefixes` entries with `isPrefix=true`. Keys are URL-decoded (`encoding-type=url`). |

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
character references. Listing is requested with `encoding-type=url`, so keys arrive
percent-encoded ASCII and exotic-key edge cases reduce to URL decoding. Unit-tested
on captured responses from MinIO, RustFS, and AWS documentation examples. If a
server ever produces XML this extractor cannot handle, it fails loudly
(`ErrorKind::parse`), and swapping in a full parser stays an internal change.

## 6. Error model

Exactly one of the `ErrorKind` values describes every outcome. Mapping rules:

1. curl returned an error → `transport` (`curlCode`, `message` from
   `curl_easy_strerror` + effective URL). `httpStatus = 0`.
2. HTTP status ≥ 400 with parseable S3 XML body → `s3` (`httpStatus`, `s3Code`,
   `message` from the body).
3. HTTP status ≥ 400 otherwise → `http` (`httpStatus`, body snippet in `message`);
   for HEAD requests the code is synthesized (`404` → `"NoSuchKey"`/`"NoSuchBucket"`)
   since HEAD responses carry no body.
4. 2xx but the body failed to parse (listing) → `parse`.
5. Aborted by `cancel()` or a callback → `cancelled`.

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
- `xml_lite` on captured MinIO/RustFS/AWS responses, entity decoding, truncated
  and hostile inputs (must fail cleanly, never crash).

**Integration (docker, run in CI):** matrix over **MinIO** and **RustFS**:
- bucket create / exists / not-exists / wrong-credentials (403 surfaces, not
  swallowed);
- put → stat → get → byte-for-byte round-trip → delete;
- `Content-Encoding` pass-through: stored and returned;
- listing: >1000 objects (pagination), prefix filtering, recursive vs
  delimiter mode, keys with spaces/unicode/`=`;
- cancellation mid-download and mid-upload;
- **silent-server test**: a stub accepts the TCP connection and never responds —
  the operation must fail in ≈`lowSpeedTimeSec`, not hang (the minio-cpp lesson,
  now a permanent regression test).

**CI (GitHub Actions):** gcc + clang builds, one ASan/UBSan job, unit tests on
every push, integration matrix via docker services.

## 10. Roadmap (post-v1)

- Virtual-hosted addressing style (needed for AWS-first users).
- Multipart upload for >5 GB objects / bounded-memory `putFile`.
- Optional response body streaming for `putFile` without full read into memory.
- Presigned URL generation (pure signer work, no transport).

## 11. Downstream (out of repo scope)

The first production consumer is a Qt service that wraps slim-s3 behind its
existing `AbstractClient` interface (QString/QByteArray conversion + its own
`RetryPolicy` on top of the structured errors). That wrapper, its vcpkg port, and
the migration off minio-cpp live in the consumer's tree and are tracked there.
