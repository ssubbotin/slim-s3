#pragma once

#include <string>
#include <string_view>

#include "slims3/slims3.hpp"
#include "transport.hpp"

namespace slims3::detail {

// Maps a finished transport exchange onto the public Error model (DESIGN.md §6).
// `headSynthCode`: for HEAD requests there is no body; on 404 synthesize this
// S3 code ("NoSuchKey" for object ops, "NoSuchBucket" for bucket ops).
Error mapError(int curlCode, bool aborted, const std::string& curlError, const HttpResponse& resp,
               const std::string& headSynthCode);

// Rejects header-injection payloads in a caller-supplied header name or
// value before it reaches the wire (and the signed canonical request): any
// byte < 0x20 (which covers CR, LF and NUL) is invalid, and header NAMES may
// additionally not contain ':'. Header VALUES may legitimately contain ':'
// (e.g. a URL in a metadata value), so pass isName=false for those.
inline bool validHeaderToken(std::string_view s, bool isName) {
    for (unsigned char c : s) {
        if (c < 0x20)
            return false;
        if (isName && c == ':')
            return false;
    }
    return true;
}

// Rejects bucket names that would change the request target or corrupt the
// URL vs the signed canonical URI: an empty name (operations would hit "/"),
// a slash (silently retargets: bucket "b/x" becomes bucket "b", key "x"),
// '?'/'#' (start the query/fragment early), '%' (ambiguous double-encoding),
// spaces and control bytes. The accepted set [A-Za-z0-9._-] is deliberately a
// superset of AWS's official [a-z0-9.-]: legacy buckets and some
// S3-compatible stores allow uppercase and underscore; strictness beyond
// URL/signature safety is the server's job.
inline bool validBucketName(std::string_view b) {
    if (b.empty())
        return false;
    for (unsigned char c : b) {
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '.' || c == '_';
        if (!ok)
            return false;
    }
    return true;
}

} // namespace slims3::detail
