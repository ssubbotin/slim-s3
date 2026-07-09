#include "client_detail.hpp"
#include "doctest.h"

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
