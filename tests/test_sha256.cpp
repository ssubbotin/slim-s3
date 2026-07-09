#include <string>

#include "doctest.h"
#include "sha256.hpp"

using slims3::detail::hmacSha256;
using slims3::detail::sha256Hex;
using slims3::detail::toHex;

TEST_CASE("sha256 NIST vectors") {
    CHECK(sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(sha256Hex(std::string(1000000, 'a')) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("hmac-sha256 RFC 4231 vectors") {
    std::string k1(20, '\x0b');
    auto m1 = hmacSha256(k1.data(), k1.size(), "Hi There", 8);
    CHECK(toHex(m1.data(), 32) ==
          "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");

    std::string k2 = "Jefe", d2 = "what do ya want for nothing?";
    auto m2 = hmacSha256(k2.data(), k2.size(), d2.data(), d2.size());
    CHECK(toHex(m2.data(), 32) ==
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    std::string k3(131, '\xaa');  // key longer than block size -> hashed first
    std::string d3 = "Test Using Larger Than Block-Size Key - Hash Key First";
    auto m3 = hmacSha256(k3.data(), k3.size(), d3.data(), d3.size());
    CHECK(toHex(m3.data(), 32) ==
          "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}
