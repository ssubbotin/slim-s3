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
