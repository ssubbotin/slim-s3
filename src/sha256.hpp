#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace slims3::detail {

std::array<std::uint8_t, 32> sha256(const void* data, std::size_t len);
std::array<std::uint8_t, 32> hmacSha256(const void* key, std::size_t keyLen, const void* msg,
                                        std::size_t msgLen);
std::string toHex(const std::uint8_t* p, std::size_t n);
std::string sha256Hex(std::string_view data);

}  // namespace slims3::detail
