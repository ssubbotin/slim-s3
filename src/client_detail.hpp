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

} // namespace slims3::detail
