#include "transport.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>

namespace slims3::detail {
namespace {

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

void globalInitOnce() {
    static std::once_flag flag;
    std::call_once(flag, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

struct CbCtx {
    CURL* handle = nullptr;
    const HttpRequest* req = nullptr;
    HttpResponse* resp = nullptr;
    std::atomic<bool>* cancel = nullptr;
    bool aborted = false;
    bool overflowed = false;
    bool headersOverflowed = false;
    bool statusLineOverflowed = false;
    bool metaFilled = false;
    std::size_t bodyOff = 0;
    // headerBytes/headerCount are cumulative for the whole exchange: they must
    // NOT be reset when a new status line clears the header vector below (see
    // statusLineCount), or a server that keeps emitting interim/status-line
    // blocks could ride that reset past the caps forever.
    std::size_t headerBytes = 0;
    std::size_t headerCount = 0;
    std::size_t statusLineCount = 0;
};

// Caps against a malicious/misbehaving server streaming endless response
// headers: a per-line count cap and a cumulative-bytes cap. Both are well
// past anything a real S3-compatible server sends.
constexpr std::size_t kMaxHeaderCount = 2000;
constexpr std::size_t kMaxHeaderBytes = 1 * 1024 * 1024;
// Cap on the number of status-line blocks ("HTTP/..." lines) in a single
// exchange. A legitimate response has exactly one; we don't follow redirects
// (FOLLOWLOCATION is off) and suppress Expect, so a stray 1xx like 103 Early
// Hints adds at most a couple more. A server that streams interim responses
// (or spoofed status lines) forever would otherwise reset headers.clear()
// indefinitely -- memory stays bounded, but without this cap the exchange
// never terminates even though the M1 byte/count caps above are cumulative
// (a slowloris the low-speed stall guard doesn't catch with a trickle just
// above its floor and operationTimeoutSec==0).
constexpr std::size_t kMaxStatusLines = 32;

size_t onHeader(char* buf, size_t size, size_t nitems, void* ud) {
    auto* ctx = static_cast<CbCtx*>(ud);
    size_t len = size * nitems;
    std::string line(buf, len);
    if (line.rfind("HTTP/", 0) == 0) {
        ctx->resp->headers.clear(); // new response block (e.g. after redirect); memory only
        if (++ctx->statusLineCount > kMaxStatusLines) {
            ctx->overflowed = true;
            ctx->statusLineOverflowed = true;
            return 0; // abort the transfer, same path as the other caps
        }
        return len;
    }
    ctx->headerBytes += len;
    if (ctx->headerCount >= kMaxHeaderCount || ctx->headerBytes > kMaxHeaderBytes) {
        ctx->overflowed = true;
        ctx->headersOverflowed = true;
        return 0; // abort the transfer, same path as the body-size cap
    }
    auto colon = line.find(':');
    if (colon != std::string::npos) {
        std::string name = toLower(line.substr(0, colon));
        std::string value = line.substr(colon + 1);
        // trim spaces and trailing CRLF
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
            value.erase(0, 1);
        while (!value.empty() && (value.back() == '\r' || value.back() == '\n'))
            value.pop_back();
        ctx->resp->headers.emplace_back(std::move(name), std::move(value));
        ++ctx->headerCount;
    }
    return len;
}

size_t onWrite(char* buf, size_t size, size_t nmemb, void* ud) {
    auto* ctx = static_cast<CbCtx*>(ud);
    size_t len = size * nmemb;
    long status = 0;
    curl_easy_getinfo(ctx->handle, CURLINFO_RESPONSE_CODE, &status);
    bool success = (status >= 200 && status < 300);
    if (!success || !ctx->req->sink) {
        // Error/diagnostic body (S3 error XML, a redirect body we refuse to
        // hand to the sink, or the caller wants the whole body). Cap
        // accumulation to keep a hostile server from ballooning memory; going
        // over the cap aborts the transfer loudly instead of truncating.
        constexpr size_t kCap = 8 * 1024 * 1024;
        if (ctx->resp->body.size() + len > kCap) {
            ctx->overflowed = true;
            return 0; // CURLE_WRITE_ERROR
        }
        ctx->resp->body.append(buf, len);
        return len;
    }
    if (ctx->req->meta && !ctx->metaFilled) {
        metaFromHeaders(*ctx->resp, *ctx->req->meta);
        ctx->metaFilled = true;
    }
    if (!ctx->req->sink(buf, len)) {
        ctx->aborted = true;
        return 0; // CURLE_WRITE_ERROR
    }
    ctx->resp->sinkBytes += len;
    return len;
}

size_t onRead(char* buf, size_t size, size_t nmemb, void* ud) {
    auto* ctx = static_cast<CbCtx*>(ud);
    size_t room = size * nmemb;
    size_t left = ctx->req->bodyLen - ctx->bodyOff;
    size_t take = std::min(room, left);
    std::memcpy(buf, ctx->req->body + ctx->bodyOff, take);
    ctx->bodyOff += take;
    return take;
}

int onXfer(void* ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    auto* ctx = static_cast<CbCtx*>(ud);
    if (ctx->cancel->load(std::memory_order_relaxed)) {
        ctx->aborted = true;
        return 1; // CURLE_ABORTED_BY_CALLBACK
    }
    if (ctx->req->progress) {
        bool up = ctx->req->body != nullptr;
        auto done = std::uint64_t(up ? ulnow : dlnow);
        auto total = std::uint64_t(up ? ultotal : dltotal);
        if (!ctx->req->progress(done, total)) {
            ctx->aborted = true;
            return 1;
        }
    }
    return 0;
}

} // namespace

std::string Endpoint::hostHeader() const {
    return isDefaultPort ? host : host + ":" + std::to_string(port);
}

std::string Endpoint::baseUrl() const {
    return scheme + "://" + hostHeader();
}

bool parseEndpoint(std::string_view s, Endpoint& out, std::string& err) {
    out = Endpoint{};
    std::string rest(s);
    auto sep = rest.find("://");
    if (sep != std::string::npos) {
        out.scheme = rest.substr(0, sep);
        rest = rest.substr(sep + 3);
    }
    if (out.scheme != "http" && out.scheme != "https") {
        err = "endpoint scheme must be http or https";
        return false;
    }
    if (rest.find('/') != std::string::npos) {
        err = "endpoint must not contain a path";
        return false;
    }
    auto colon = rest.rfind(':');
    if (colon != std::string::npos) {
        std::string p = rest.substr(colon + 1);
        char* endp = nullptr;
        long v = std::strtol(p.c_str(), &endp, 10);
        if (p.empty() || *endp != '\0' || v < 1 || v > 65535) {
            err = "invalid endpoint port";
            return false;
        }
        out.port = int(v);
        out.host = rest.substr(0, colon);
    } else {
        out.port = (out.scheme == "https") ? 443 : 80;
        out.host = rest;
    }
    if (out.host.empty()) {
        err = "endpoint host is empty";
        return false;
    }
    out.isDefaultPort = (out.scheme == "https") ? (out.port == 443) : (out.port == 80);
    return true;
}

const std::string* HttpResponse::find(std::string_view name) const {
    std::string lower = toLower(std::string(name));
    for (const auto& kv : headers)
        if (kv.first == lower)
            return &kv.second;
    return nullptr;
}

void metaFromHeaders(const HttpResponse& r, slims3::ObjectMeta& m) {
    if (const auto* v = r.find("content-type"))
        m.contentType = *v;
    if (const auto* v = r.find("content-encoding"))
        m.contentEncoding = *v;
    if (const auto* v = r.find("content-length"))
        m.info.size = std::strtoull(v->c_str(), nullptr, 10);
    if (const auto* v = r.find("etag")) {
        std::string e = *v;
        if (e.size() >= 2 && e.front() == '"' && e.back() == '"')
            e = e.substr(1, e.size() - 2);
        m.info.etag = e;
    }
}

// Holds only the Config fields the transport layer actually reads. In
// particular this deliberately does NOT copy accessKey/secretKey/region/
// endpoint: there is no reason for a second live copy of the secret key to
// sit around in this object once Transport is constructed (signing reads
// Config directly in client.cpp). Full zeroization of Config's own copy is
// out of scope for v1.
struct TransportState {
    CURL* handle = nullptr;
    long connectTimeoutSec = 30;
    long operationTimeoutSec = 0;
    long lowSpeedLimitBps = 1;
    long lowSpeedTimeSec = 60;
    std::string caBundlePath;
    bool tlsVerify = true;
    std::string userAgent;
};

Transport::Transport(const slims3::Config& cfg) {
    globalInitOnce();
    auto* st = new TransportState;
    st->connectTimeoutSec = cfg.connectTimeoutSec;
    st->operationTimeoutSec = cfg.operationTimeoutSec;
    st->lowSpeedLimitBps = cfg.lowSpeedLimitBps;
    st->lowSpeedTimeSec = cfg.lowSpeedTimeSec;
    st->caBundlePath = cfg.caBundlePath;
    st->tlsVerify = cfg.tlsVerify;
    st->userAgent = cfg.userAgent;
    st->handle = curl_easy_init();
    state_ = st;
}

Transport::~Transport() {
    auto* st = static_cast<TransportState*>(state_);
    if (st->handle)
        curl_easy_cleanup(st->handle);
    delete st;
}

int Transport::execute(const HttpRequest& req, HttpResponse& resp, std::atomic<bool>& cancel,
                       bool& aborted, std::string& curlError) {
    auto* st = static_cast<TransportState*>(state_);
    CURL* h = st->handle;
    if (!h) {
        // curl_easy_init() failed in the constructor; report instead of
        // dereferencing a null handle below.
        curlError = "curl_easy_init failed";
        aborted = false;
        return int(CURLE_FAILED_INIT);
    }
    // The HTTP method is routed purely from req.body/req.noBody below: a non-
    // null body implies CURLOPT_UPLOAD (PUT). PUT therefore requires a
    // non-null body pointer even for empty payloads, or curl silently issues
    // a GET instead.
    if (req.method == "PUT" && req.body == nullptr) {
        curlError = "PUT without a body pointer (internal invariant)";
        aborted = false;
        return int(CURLE_FAILED_INIT);
    }
    // Reset options but keep the handle: libcurl's connection/DNS/TLS caches
    // live on the handle and survive curl_easy_reset, so requests reuse
    // connections instead of reconnecting every time.
    curl_easy_reset(h);

    CbCtx ctx;
    ctx.handle = h;
    ctx.req = &req;
    ctx.resp = &resp;
    ctx.cancel = &cancel;

    char errbuf[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(h, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(h, CURLOPT_USERAGENT, st->userAgent.c_str());
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, st->connectTimeoutSec);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, st->lowSpeedLimitBps);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, st->lowSpeedTimeSec);
    if (st->operationTimeoutSec > 0)
        curl_easy_setopt(h, CURLOPT_TIMEOUT, st->operationTimeoutSec);
    if (!st->caBundlePath.empty())
        curl_easy_setopt(h, CURLOPT_CAINFO, st->caBundlePath.c_str());
    if (!st->tlsVerify) {
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    if (req.noBody) {
        curl_easy_setopt(h, CURLOPT_NOBODY, 1L);
    } else if (req.body != nullptr) {
        curl_easy_setopt(h, CURLOPT_UPLOAD, 1L); // implies PUT
        curl_easy_setopt(h, CURLOPT_READFUNCTION, onRead);
        curl_easy_setopt(h, CURLOPT_READDATA, &ctx);
        curl_easy_setopt(h, CURLOPT_INFILESIZE_LARGE, curl_off_t(req.bodyLen));
    }
    if (req.method == "DELETE")
        curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, "DELETE");

    curl_slist* hdrs = nullptr;
    for (const auto& line : req.headerLines)
        hdrs = curl_slist_append(hdrs, line.c_str());
    hdrs = curl_slist_append(hdrs, "Expect:"); // never wait for 100-continue
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);

    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, onHeader);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, &ctx);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, onWrite);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, onXfer);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    // Note: CURLOPT_ACCEPT_ENCODING is deliberately NOT set -- the body must
    // arrive exactly as stored (a zstd-encoded object stays zstd-encoded).

    CURLcode rc = curl_easy_perform(h);
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &resp.status);
    curl_slist_free_all(hdrs);

    // HEAD/empty-body responses never hit onWrite; fill meta from headers here.
    // Gated on 2xx: a redirect's headers must not populate meta either.
    if (req.meta && !ctx.metaFilled && resp.status >= 200 && resp.status < 300)
        metaFromHeaders(resp, *req.meta);

    aborted = ctx.aborted;
    curlError = errbuf[0] ? errbuf : (rc != CURLE_OK ? curl_easy_strerror(rc) : "");
    if (ctx.overflowed) {
        // aborted stays false: this must map to ErrorKind::transport, not
        // ErrorKind::cancelled (which is reserved for user/cancel-flag/sink
        // refusal aborts).
        if (ctx.statusLineOverflowed)
            curlError = "too many response header blocks (interim/redirect flood)";
        else if (ctx.headersOverflowed)
            curlError = "response headers exceeded limit";
        else
            curlError = "response body exceeded the 8 MiB in-memory cap";
    }
    return int(rc);
}

} // namespace slims3::detail
