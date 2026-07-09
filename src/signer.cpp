#include "signer.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "sha256.hpp"

namespace slims3::detail {
namespace {

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

// SigV4: trim ends, collapse inner space runs to a single space.
std::string trimCollapse(const std::string& v) {
    std::string out;
    bool inSpace = false;
    for (char c : v) {
        if (c == ' ' || c == '\t') {
            inSpace = true;
        } else {
            if (inSpace && !out.empty()) out += ' ';
            inSpace = false;
            out += c;
        }
    }
    return out;
}

// -> (canonicalHeaders, signedHeaders); adds x-amz-content-sha256 and x-amz-date.
std::pair<std::string, std::string> headerLists(const SignParams& p) {
    std::vector<std::pair<std::string, std::string>> items;
    items.reserve(p.headers.size() + 2);
    for (const auto& kv : p.headers) items.emplace_back(toLower(kv.first), trimCollapse(kv.second));
    items.emplace_back("x-amz-content-sha256", p.payloadHashHex);
    items.emplace_back("x-amz-date", p.amzDate);
    std::sort(items.begin(), items.end());
    std::string canon, signedList;
    for (const auto& kv : items) {
        canon += kv.first;
        canon += ':';
        canon += kv.second;
        canon += '\n';
        if (!signedList.empty()) signedList += ';';
        signedList += kv.first;
    }
    return {canon, signedList};
}

std::string scope(const SignParams& p) {
    return p.amzDate.substr(0, 8) + "/" + p.region + "/" + p.service + "/aws4_request";
}

}  // namespace

std::string canonicalRequest(const SignParams& p) {
    auto [canonHeaders, signedHeaders] = headerLists(p);
    return p.method + "\n" + p.canonicalUri + "\n" + p.canonicalQuery + "\n" + canonHeaders +
           "\n" + signedHeaders + "\n" + p.payloadHashHex;
}

std::string stringToSign(const SignParams& p) {
    return "AWS4-HMAC-SHA256\n" + p.amzDate + "\n" + scope(p) + "\n" +
           sha256Hex(canonicalRequest(p));
}

std::string authorizationHeader(const SignParams& p) {
    std::string kSecret = "AWS4" + p.secretKey;
    std::string date = p.amzDate.substr(0, 8);
    auto kDate = hmacSha256(kSecret.data(), kSecret.size(), date.data(), date.size());
    auto kRegion = hmacSha256(kDate.data(), 32, p.region.data(), p.region.size());
    auto kService = hmacSha256(kRegion.data(), 32, p.service.data(), p.service.size());
    auto kSigning = hmacSha256(kService.data(), 32, "aws4_request", 12);
    std::string sts = stringToSign(p);
    auto sig = hmacSha256(kSigning.data(), 32, sts.data(), sts.size());
    auto [canonHeaders, signedHeaders] = headerLists(p);
    (void)canonHeaders;
    return "AWS4-HMAC-SHA256 Credential=" + p.accessKey + "/" + scope(p) +
           ", SignedHeaders=" + signedHeaders + ", Signature=" + toHex(sig.data(), 32);
}

std::string formatAmzDate(std::time_t t) {
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%dT%H%M%SZ", &tm);
    return buf;
}

}  // namespace slims3::detail
