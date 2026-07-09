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
    std::string userAgent;             // empty -> "slim-s3/<library version>"
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
    // Movable. A moved-from Client may only be destroyed or assigned to.
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
