#include "uri.hpp"

#include <algorithm>

namespace slims3::detail {
namespace {

bool unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '.' || c == '_' || c == '~';
}

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

std::string percentEncode(std::string_view s, bool keepSlash) {
    static const char* d = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (unreserved(c) || (keepSlash && c == '/')) {
            out += char(c);
        } else {
            out += '%';
            out += d[c >> 4];
            out += d[c & 0xf];
        }
    }
    return out;
}

std::string canonicalQuery(std::vector<std::pair<std::string, std::string>> params) {
    for (auto& kv : params) {
        kv.first = percentEncode(kv.first, false);
        kv.second = percentEncode(kv.second, false);
    }
    std::sort(params.begin(), params.end());
    std::string out;
    for (const auto& kv : params) {
        if (!out.empty()) out += '&';
        out += kv.first;
        out += '=';
        out += kv.second;
    }
    return out;
}

std::string urlDecode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + 2 < s.size() && hexVal(s[i + 1]) >= 0 &&
                   hexVal(s[i + 2]) >= 0) {
            out += char(hexVal(s[i + 1]) * 16 + hexVal(s[i + 2]));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

}  // namespace slims3::detail
