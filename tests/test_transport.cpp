#include <curl/curl.h>

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

    // A scheme-less endpoint must NOT silently default to plaintext http --
    // for a client carrying credentials that's a security foot-gun, not a
    // convenience. Require the caller to say http:// or https:// explicitly.
    CHECK_FALSE(parseEndpoint("minio.local", e, err));
    CHECK(err.find("scheme") != std::string::npos);
    CHECK_FALSE(parseEndpoint("minio.local:9000", e, err));

    CHECK_FALSE(parseEndpoint("", e, err));
    CHECK_FALSE(parseEndpoint("ftp://x", e, err));
    CHECK_FALSE(parseEndpoint("http://host:notaport", e, err));
    CHECK_FALSE(parseEndpoint("http://host/path", e, err)); // path suffix not supported in v1
}

TEST_CASE("parseEndpoint: IPv6 literals") {
    Endpoint e;
    std::string err;

    REQUIRE(parseEndpoint("http://[::1]:9000", e, err));
    CHECK(e.host == "[::1]");
    CHECK(e.port == 9000);
    CHECK_FALSE(e.isDefaultPort);
    CHECK(e.hostHeader() == "[::1]:9000"); // brackets kept: valid in URLs and Host headers
    CHECK(e.baseUrl() == "http://[::1]:9000");

    REQUIRE(parseEndpoint("https://[2001:db8::1]", e, err)); // no port -> scheme default
    CHECK(e.host == "[2001:db8::1]");
    CHECK(e.port == 443);
    CHECK(e.isDefaultPort);
    CHECK(e.hostHeader() == "[2001:db8::1]");

    CHECK_FALSE(parseEndpoint("http://[::1", e, err));       // unterminated bracket
    CHECK_FALSE(parseEndpoint("http://[]:9000", e, err));    // empty address
    CHECK_FALSE(parseEndpoint("http://[::1]9000", e, err));  // junk after bracket
    CHECK_FALSE(parseEndpoint("http://[::1]:", e, err));     // colon without a port
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

TEST_CASE("Transport::execute: PUT without a body pointer is rejected before any network I/O") {
    slims3::Config cfg;
    cfg.endpoint = "http://127.0.0.1:1"; // would refuse to connect if this ever reached curl
    Transport tr(cfg);

    HttpRequest req;
    req.method = "PUT";
    req.url = "http://127.0.0.1:1/bucket/key";
    req.body = nullptr;
    req.bodyLen = 0;
    req.noBody = false;

    HttpResponse resp;
    std::atomic<bool> cancel{false};
    bool aborted = true; // pre-set to a value the guard must overwrite
    std::string curlError;
    int rc = tr.execute(req, resp, cancel, aborted, curlError);

    CHECK(rc == int(CURLE_FAILED_INIT));
    CHECK_FALSE(aborted);
    CHECK(curlError.find("PUT without a body pointer") != std::string::npos);
    CHECK(resp.status == 0); // never even started the transfer
}
