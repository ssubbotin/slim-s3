#include "client_detail.hpp"
#include "doctest.h"

using slims3::detail::validBucketName;
using slims3::detail::validHeaderToken;

TEST_CASE("validHeaderToken: plain tokens are valid") {
    CHECK(validHeaderToken("application/json", /*isName=*/false));
    CHECK(validHeaderToken("x-amz-meta-foo", /*isName=*/true));
    CHECK(validHeaderToken("", /*isName=*/false));
    CHECK(validHeaderToken("", /*isName=*/true));
}

TEST_CASE("validHeaderToken: values may contain a colon, names may not") {
    CHECK(validHeaderToken("http://example.com/x", /*isName=*/false));
    CHECK_FALSE(validHeaderToken("x-amz-meta:foo", /*isName=*/true));
}

TEST_CASE("validHeaderToken: CR/LF/NUL are rejected in names and values") {
    CHECK_FALSE(validHeaderToken(std::string_view("bad\r\nvalue"), /*isName=*/false));
    CHECK_FALSE(validHeaderToken(std::string_view("bad\nvalue"), /*isName=*/false));
    CHECK_FALSE(validHeaderToken(std::string_view("bad\rvalue"), /*isName=*/false));
    CHECK_FALSE(validHeaderToken(std::string_view("bad\0value", 9), /*isName=*/false));
    CHECK_FALSE(validHeaderToken(std::string_view("bad\r\nname"), /*isName=*/true));
}

TEST_CASE("validHeaderToken: other control characters are rejected") {
    CHECK_FALSE(validHeaderToken(std::string_view("bad\tvalue"), /*isName=*/false));
    CHECK_FALSE(validHeaderToken(std::string_view("bad\x01value"), /*isName=*/false));
}

TEST_CASE("validBucketName: names within [A-Za-z0-9._-] are accepted") {
    CHECK(validBucketName("my-bucket"));
    CHECK(validBucketName("my.bucket.01"));
    // Deliberately a superset of AWS's official [a-z0-9.-]: legacy buckets and
    // some S3-compatible stores allow uppercase and underscore.
    CHECK(validBucketName("My_Bucket"));
}

TEST_CASE("validBucketName: empty and URL-breaking characters are rejected") {
    CHECK_FALSE(validBucketName(""));
    CHECK_FALSE(validBucketName("b/x"));  // would retarget: bucket "b", key "x"
    CHECK_FALSE(validBucketName("b?x"));  // would start the query string early
    CHECK_FALSE(validBucketName("b#x"));  // would start a fragment
    CHECK_FALSE(validBucketName("my bucket"));
    CHECK_FALSE(validBucketName(std::string_view("b\r\nx")));
    CHECK_FALSE(validBucketName("b\x7f"));
    CHECK_FALSE(validBucketName("b%41")); // percent would double-encode ambiguously
}
