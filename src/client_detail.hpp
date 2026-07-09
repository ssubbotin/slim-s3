#pragma once

#include <string>

#include "slims3/slims3.hpp"
#include "transport.hpp"

namespace slims3::detail {

// Maps a finished transport exchange onto the public Error model (DESIGN.md §6).
// `headSynthCode`: for HEAD requests there is no body; on 404 synthesize this
// S3 code ("NoSuchKey" for object ops, "NoSuchBucket" for bucket ops).
Error mapError(int curlCode, bool aborted, const std::string& curlError, const HttpResponse& resp,
               const std::string& headSynthCode);

} // namespace slims3::detail
