#pragma once

#include <ctime>
#include <string>
#include <utility>
#include <vector>

namespace slims3::detail {

struct SignParams {
    std::string method;          // "GET"
    std::string canonicalUri;    // already percent-encoded path, e.g. "/b/k%20ey"
    std::string canonicalQuery;  // already canonical (Task 3) or ""
    std::vector<std::pair<std::string, std::string>> headers;  // must include "host"
    std::string payloadHashHex;
    std::string amzDate;         // "YYYYMMDDTHHMMSSZ"
    std::string region;
    std::string service = "s3";
    std::string accessKey, secretKey;
};

std::string canonicalRequest(const SignParams& p);
std::string stringToSign(const SignParams& p);
std::string authorizationHeader(const SignParams& p);
std::string formatAmzDate(std::time_t t);   // gmtime -> "YYYYMMDDTHHMMSSZ"

}  // namespace slims3::detail
