#include "doctest.h"
#include "uri.hpp"

using namespace slims3::detail;

TEST_CASE("percentEncode: unreserved kept, everything else escaped, uppercase hex") {
    CHECK(percentEncode("AZaz09-._~", false) == "AZaz09-._~");
    CHECK(percentEncode("a b", false) == "a%20b");
    CHECK(percentEncode("a+b=c/d", false) == "a%2Bb%3Dc%2Fd");
    CHECK(percentEncode("a/b c", true) == "a/b%20c");           // keepSlash for key paths
    CHECK(percentEncode("\xD0\xB6", false) == "%D0%B6");        // UTF-8 bytes escaped
}

TEST_CASE("canonicalQuery: sorted, encoded, empty values kept") {
    CHECK(canonicalQuery({}) == "");
    CHECK(canonicalQuery({{"prefix", "data/"}, {"list-type", "2"}, {"encoding-type", "url"}}) ==
          "encoding-type=url&list-type=2&prefix=data%2F");
    // '=' and '+' inside a value (continuation tokens) must be escaped
    CHECK(canonicalQuery({{"continuation-token", "1/wJ+=xyz"}}) ==
          "continuation-token=1%2FwJ%2B%3Dxyz");
    CHECK(canonicalQuery({{"delimiter", ""}}) == "delimiter=");  // empty value kept as name=
}

TEST_CASE("urlDecode: %XX and plus-as-space") {
    CHECK(urlDecode("a%20b") == "a b");
    CHECK(urlDecode("a+b") == "a b");
    CHECK(urlDecode("a%2Bb") == "a+b");
    CHECK(urlDecode("%D0%B6") == "\xD0\xB6");
    CHECK(urlDecode("bad%2") == "bad%2");                        // truncated escape left as-is
}
