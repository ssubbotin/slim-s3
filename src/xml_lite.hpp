#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace slims3::detail {

struct S3ErrorBody {
    std::string code;
    std::string message;
};

bool parseErrorBody(std::string_view xml, S3ErrorBody& out);

struct ListEntry {
    std::string key;
    std::uint64_t size = 0;
    std::string etag;
    bool isPrefix = false;
};

struct ListPage {
    std::vector<ListEntry> entries;
    bool truncated = false;
    std::string nextToken;
};

bool parseListPage(std::string_view xml, ListPage& out);

std::string xmlUnescape(std::string_view s);

}  // namespace slims3::detail
