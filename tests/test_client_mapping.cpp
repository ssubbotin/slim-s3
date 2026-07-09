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
    r.status = 404; // HEAD: empty body
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
