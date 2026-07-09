#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "slims3/slims3.hpp"

namespace slims3::detail {

struct Endpoint {
    std::string scheme = "http";
    std::string host;
    int port = 80;
    bool isDefaultPort = true;
    std::string hostHeader() const; // "host" or "host:port" when non-default
    std::string baseUrl() const;    // "scheme://host[:port]" (port only when non-default)
};

bool parseEndpoint(std::string_view s, Endpoint& out, std::string& err);

struct HttpRequest {
    std::string method;                   // "GET"/"PUT"/"HEAD"/"DELETE"
    std::string url;                      // full URL
    std::vector<std::string> headerLines; // "Name: value"
    const char* body = nullptr;           // upload payload (PUT)
    std::size_t bodyLen = 0;
    bool noBody = false;  // HEAD
    slims3::WriteFn sink; // null -> capture into HttpResponse::body
    slims3::ProgressFn progress;
    slims3::ObjectMeta* meta = nullptr; // filled from headers before first sink call
};

struct HttpResponse {
    long status = 0;
    std::vector<std::pair<std::string, std::string>> headers; // lowercased names
    std::string body;            // error body, or full body when sink==null
    std::uint64_t sinkBytes = 0; // bytes delivered to sink
    const std::string* find(std::string_view name) const; // nullptr if absent
};

void metaFromHeaders(const HttpResponse& r, slims3::ObjectMeta& m); // type/encoding/etag/size

class Transport {
  public:
    explicit Transport(const slims3::Config& cfg);
    ~Transport();

    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;
    Transport(Transport&&) = delete;
    Transport& operator=(Transport&&) = delete;

    // Returns the CURLcode; fills resp. `aborted` reports that one of OUR
    // callbacks stopped the transfer (cancel flag / sink false / progress false).
    int execute(const HttpRequest& req, HttpResponse& resp, std::atomic<bool>& cancel,
                bool& aborted, std::string& curlError);

  private:
    void* state_ = nullptr;
};

} // namespace slims3::detail
