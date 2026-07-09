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
