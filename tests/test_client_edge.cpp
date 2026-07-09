#include "doctest.h"
#include "slims3/slims3.hpp"

using namespace slims3;

namespace {

Config baseConfig(const std::string& endpoint) {
    Config cfg;
    cfg.endpoint = endpoint;
    cfg.accessKey = "AKIDEXAMPLE";
    cfg.secretKey = "secret";
    return cfg;
}

} // namespace

TEST_CASE("Client: move construction and move assignment transfer ownership") {
    Client a(baseConfig("ftp://x"));
    Client moved(std::move(a));
    bool exists = true;
    Result r = moved.bucketExists("b", exists);
    CHECK_FALSE(bool(r));
    CHECK(r.error.kind == ErrorKind::transport);

    Client c2(baseConfig("ftp://y"));
    c2 = std::move(moved);
    Result r2 = c2.bucketExists("b", exists);
    CHECK_FALSE(bool(r2));
    CHECK(r2.error.kind == ErrorKind::transport);
}

TEST_CASE("Client: bad endpoint returns a transport error before any network I/O") {
    Client c(baseConfig("ftp://x"));

    bool exists = true; // pre-set to confirm the early return resets it
    Result r = c.bucketExists("b", exists);
    CHECK_FALSE(bool(r));
    CHECK(r.error.kind == ErrorKind::transport);
    CHECK(r.error.message.find("bad endpoint") != std::string::npos);
    CHECK(r.error.message.find("endpoint scheme must be http or https") != std::string::npos);
    CHECK_FALSE(exists);

    // Same early-return path (Impl::run's epError check) for another operation.
    Result r2 = c.deleteObject("b", "k");
    CHECK_FALSE(bool(r2));
    CHECK(r2.error.kind == ErrorKind::transport);
    CHECK(r2.error.message.find("bad endpoint") != std::string::npos);
}

TEST_CASE("Client::putObject: null data with non-zero length is rejected before network") {
    Client c(baseConfig("http://127.0.0.1:1"));
    Result r = c.putObject("b", "k", nullptr, 5);
    CHECK_FALSE(bool(r));
    CHECK(r.error.kind == ErrorKind::transport);
    CHECK(r.error.message == "null data with non-zero length");
}

TEST_CASE("Client::putObject: invalid header token in contentType is rejected before network") {
    Client c(baseConfig("http://127.0.0.1:1"));
    PutOptions po;
    po.contentType = "text/plain\r\nX-Injected: 1";
    Result r = c.putObject("b", "k", "x", 1, po);
    CHECK_FALSE(bool(r));
    CHECK(r.error.kind == ErrorKind::transport);
    CHECK(r.error.message == "invalid header (control characters not allowed)");
}

TEST_CASE("Client::putObject: invalid header token in extraHeaders is rejected before network") {
    Client c(baseConfig("http://127.0.0.1:1"));
    PutOptions po;
    po.extraHeaders.emplace_back("x-amz-meta-note", "line1\r\nX-Injected: 1");
    Result r = c.putObject("b", "k", "x", 1, po);
    CHECK_FALSE(bool(r));
    CHECK(r.error.kind == ErrorKind::transport);
    CHECK(r.error.message == "invalid header (control characters not allowed)");
}

TEST_CASE("Client::putObject: invalid header token in contentEncoding is rejected before network") {
    Client c(baseConfig("http://127.0.0.1:1"));
    PutOptions po;
    po.contentEncoding = "zstd\r\nX-Injected: 1";
    Result r = c.putObject("b", "k", "x", 1, po);
    CHECK_FALSE(bool(r));
    CHECK(r.error.kind == ErrorKind::transport);
    CHECK(r.error.message == "invalid header (control characters not allowed)");
}

TEST_CASE("Client::putObject: null data with zero length is the explicit empty-body path") {
    // A genuinely bad endpoint (fails parseEndpoint) fails fast in Impl::run,
    // after putObject's own null/len handling has already run -- enough to
    // exercise the data==nullptr (len==0) sentinel-body branch without any
    // real network I/O (a syntactically valid but unreachable endpoint would
    // instead attempt an actual connection here).
    Client c(baseConfig("ftp://x"));
    Result r = c.putObject("b", "k", nullptr, 0);
    CHECK_FALSE(bool(r));
    CHECK(r.error.kind == ErrorKind::transport);
    CHECK(r.error.message.find("bad endpoint") != std::string::npos);
}

TEST_CASE("Client: empty object key is rejected before network for every object op") {
    // Without this guard an empty key silently retargets the operation at the
    // bucket itself: deleteObject(b, "") would send DELETE /b (DeleteBucket!),
    // putObject(b, "") would send PUT /b, getObject(b, "") would stream the
    // bucket listing XML into the sink.
    Client c(baseConfig("http://127.0.0.1:1"));

    auto checkRejected = [](const Result& r) {
        CHECK_FALSE(bool(r));
        CHECK(r.error.kind == ErrorKind::transport);
        CHECK(r.error.message == "empty object key");
        CHECK(r.error.httpStatus == 0); // no network I/O happened
    };

    checkRejected(c.putObject("b", "", "x", 1));
    checkRejected(c.putFile("b", "", "/nonexistent-file"));
    WriteFn sink = [](const char*, std::size_t) { return true; };
    checkRejected(c.getObject("b", "", sink));
    checkRejected(c.getToFile("b", "", "/tmp/slims3-edge-test.bin"));
    ObjectMeta meta;
    checkRejected(c.statObject("b", "", meta));
    checkRejected(c.deleteObject("b", ""));
}

TEST_CASE("Client: invalid bucket name is rejected before network for every op") {
    Client c(baseConfig("http://127.0.0.1:1"));

    auto checkRejected = [](const Result& r) {
        CHECK_FALSE(bool(r));
        CHECK(r.error.kind == ErrorKind::transport);
        CHECK(r.error.message == "invalid bucket name");
        CHECK(r.error.httpStatus == 0);
    };

    bool exists = true;
    checkRejected(c.bucketExists("", exists));
    CHECK_FALSE(exists); // out-param still reset on the early return
    checkRejected(c.createBucket(""));
    // A slash would silently retarget the request (bucket "b", key "x");
    // '?' / '#' / space would corrupt the URL vs the signed canonical URI.
    checkRejected(c.putObject("b/x", "k", "x", 1));
    checkRejected(c.deleteObject("b?x", "k"));
    ObjectMeta meta;
    checkRejected(c.statObject("b#x", "k", meta));
    checkRejected(c.bucketExists("my bucket", exists));
    checkRejected(c.createBucket("b\x01"));
    checkRejected(c.listObjects("", "", true, [](const ObjectInfo&) { return true; }));
}

TEST_CASE("Client: well-formed bucket names pass the guard") {
    // ftp:// endpoint: a bucket that passes validation reaches Impl::run and
    // fails there with "bad endpoint" -- proving the guard did not fire.
    Client c(baseConfig("ftp://x"));
    Result r = c.createBucket("My_Bucket-1.x");
    CHECK_FALSE(bool(r));
    CHECK(r.error.message.find("bad endpoint") != std::string::npos);
}

TEST_CASE("Client::putObject: duplicate header names are rejected before network") {
    // SigV4 requires repeated header values to be comma-joined under one name;
    // emitting the name twice guarantees a server-side SignatureDoesNotMatch.
    Client c(baseConfig("http://127.0.0.1:1"));

    PutOptions po;
    po.extraHeaders.emplace_back("X-Amz-Meta-A", "1");
    po.extraHeaders.emplace_back("x-amz-meta-a", "2"); // same name, case-insensitively
    Result r = c.putObject("b", "k", "x", 1, po);
    CHECK_FALSE(bool(r));
    CHECK(r.error.kind == ErrorKind::transport);
    CHECK(r.error.message == "duplicate header name");

    PutOptions po2;
    po2.contentType = "text/plain";
    po2.extraHeaders.emplace_back("Content-Type", "application/json"); // collides with the option
    Result r2 = c.putObject("b", "k", "x", 1, po2);
    CHECK_FALSE(bool(r2));
    CHECK(r2.error.message == "duplicate header name");
}

TEST_CASE("Client::putObject: headers managed by the library are rejected before network") {
    Client c(baseConfig("http://127.0.0.1:1"));
    for (const char* name : {"Host", "authorization", "X-Amz-Date", "x-amz-content-sha256",
                             "Content-Length", "Transfer-Encoding", "Expect"}) {
        PutOptions po;
        po.extraHeaders.emplace_back(name, "v");
        Result r = c.putObject("b", "k", "x", 1, po);
        CHECK_FALSE(bool(r));
        CHECK(r.error.kind == ErrorKind::transport);
        CHECK(r.error.message == "reserved header name (set by the library)");
    }
}

TEST_CASE("Config: default userAgent is empty (transport substitutes slim-s3/<version>)") {
    // The versioned default lives in the library build (SLIMS3_VERSION from
    // CMake), not hardcoded in the public header where it would drift.
    CHECK(Config{}.userAgent.empty());
}

TEST_CASE("Client::putObject: valid extraHeaders are copied into the request") {
    // Same bad-endpoint trick: extraHdrs.push_back runs before the network
    // call, so this covers the valid-input population loop without a server.
    Client c(baseConfig("ftp://x"));
    PutOptions po;
    po.extraHeaders.emplace_back("x-amz-meta-note", "hello");
    po.extraHeaders.emplace_back("x-amz-meta-other", "world");
    Result r = c.putObject("b", "k", "x", 1, po);
    CHECK_FALSE(bool(r));
    CHECK(r.error.kind == ErrorKind::transport);
    CHECK(r.error.message.find("bad endpoint") != std::string::npos);
}
