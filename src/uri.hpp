#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slims3::detail {

std::string percentEncode(std::string_view s, bool keepSlash);
std::string canonicalQuery(std::vector<std::pair<std::string, std::string>> params);
std::string urlDecode(std::string_view s);

}  // namespace slims3::detail
