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

    REQUIRE(parseEndpoint("minio.local", e, err)); // scheme defaults to http
    CHECK(e.scheme == "http");
    CHECK(e.port == 80);
    CHECK(e.hostHeader() == "minio.local");

    CHECK_FALSE(parseEndpoint("", e, err));
    CHECK_FALSE(parseEndpoint("ftp://x", e, err));
    CHECK_FALSE(parseEndpoint("http://host:notaport", e, err));
    CHECK_FALSE(parseEndpoint("http://host/path", e, err)); // path suffix not supported in v1
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
