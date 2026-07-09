#include <curl/curl.h>

#include <atomic>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>

#include "client_detail.hpp"
#include "sha256.hpp"
#include "signer.hpp"
#include "slims3/slims3.hpp"
#include "transport.hpp"
#include "uri.hpp"
#include "xml_lite.hpp"

namespace slims3 {

using namespace detail;

namespace detail {

Error mapError(int curlCode, bool aborted, const std::string& curlError, const HttpResponse& resp,
               const std::string& headSynthCode) {
    Error e;
    if (aborted) {
        e.kind = ErrorKind::cancelled;
        e.message = "operation cancelled";
        return e;
    }
    if (curlCode != 0) {
        e.kind = ErrorKind::transport;
        e.curlCode = curlCode;
        e.message = curlError.empty() ? "transport failure" : curlError;
        return e;
    }
    if (resp.status >= 400) {
        e.httpStatus = int(resp.status);
        S3ErrorBody body;
        if (parseErrorBody(resp.body, body)) {
            e.kind = ErrorKind::s3;
            e.s3Code = body.code;
            e.message = body.message.empty() ? body.code : body.message;
        } else if (!headSynthCode.empty() && resp.status == 404) {
            e.kind = ErrorKind::s3;
            e.s3Code = headSynthCode;
            e.message = headSynthCode;
        } else {
            e.kind = ErrorKind::http;
            e.message = "HTTP " + std::to_string(resp.status) +
                        (resp.body.empty() ? "" : (": " + resp.body.substr(0, 200)));
        }
        return e;
    }
    if (resp.status < 200 || resp.status >= 300) {
        // Success boundary is strictly [200,300): a 3xx (FOLLOWLOCATION is off,
        // so we never actually follow one) or a stray 1xx must not be treated
        // as success, or a redirect body could be written to the sink / used
        // to fill meta from redirect headers.
        e.kind = ErrorKind::http;
        e.httpStatus = int(resp.status);
        e.message = "HTTP " + std::to_string(resp.status) +
                    ((resp.status >= 300 && resp.status < 400) ? " (redirect not followed)" : "");
        return e;
    }
    return e; // none
}

} // namespace detail

struct Client::Impl {
    Config cfg;
    Endpoint ep;
    Transport tr;
    std::atomic<bool> cancel{false};
    std::string epError; // non-empty if the endpoint failed to parse

    explicit Impl(Config c) : cfg(std::move(c)), tr(cfg) {
        if (!parseEndpoint(cfg.endpoint, ep, epError) && epError.empty())
            epError = "invalid endpoint";
    }

    struct Op {
        std::string method;
        std::string bucket;
        std::string key;                                            // empty for bucket-level ops
        std::vector<std::pair<std::string, std::string>> query;     // raw, unencoded
        std::vector<std::pair<std::string, std::string>> extraHdrs; // e.g. Content-Type
        const char* body = nullptr;
        std::size_t bodyLen = 0;
        bool noBody = false;
        WriteFn sink;
        ProgressFn progress;
        ObjectMeta* meta = nullptr;
        std::string headSynthCode; // "NoSuchKey"/"NoSuchBucket" for HEAD 404
    };

    Result run(const Op& op, HttpResponse& resp) {
        if (!epError.empty())
            return Result{Error{ErrorKind::transport, 0, "", "bad endpoint: " + epError, 0}, 0};

        std::string payloadHash =
            op.body ? sha256Hex(std::string_view(op.body, op.bodyLen)) : sha256Hex("");
        std::string amzDate = formatAmzDate(std::time(nullptr));
        std::string canonicalUri = "/" + op.bucket;
        if (!op.key.empty())
            canonicalUri += "/" + percentEncode(op.key, /*keepSlash=*/true);
        std::string cq = canonicalQuery(op.query);

        SignParams sp;
        sp.method = op.method;
        sp.canonicalUri = canonicalUri;
        sp.canonicalQuery = cq;
        sp.headers = op.extraHdrs;
        // Invariant: the signed "host" value must equal what curl derives from
        // the URL below; Endpoint::hostHeader() and Endpoint::baseUrl() are both
        // built from the same host/port, and no explicit Host header is sent.
        sp.headers.emplace_back("host", ep.hostHeader());
        sp.payloadHashHex = payloadHash;
        sp.amzDate = amzDate;
        sp.region = cfg.region;
        sp.accessKey = cfg.accessKey;
        sp.secretKey = cfg.secretKey;

        HttpRequest req;
        req.method = op.method;
        req.url = ep.baseUrl() + canonicalUri + (cq.empty() ? "" : "?" + cq);
        for (const auto& kv : op.extraHdrs)
            req.headerLines.push_back(kv.first + ": " + kv.second);
        req.headerLines.push_back("x-amz-content-sha256: " + payloadHash);
        req.headerLines.push_back("x-amz-date: " + amzDate);
        req.headerLines.push_back("Authorization: " + authorizationHeader(sp));
        req.body = op.body;
        req.bodyLen = op.bodyLen;
        req.noBody = op.noBody;
        req.sink = op.sink;
        req.progress = op.progress;
        req.meta = op.meta;

        bool aborted = false;
        std::string curlErr;
        int rc = tr.execute(req, resp, cancel, aborted, curlErr);

        Result r;
        r.error = mapError(rc, aborted, curlErr, resp, op.headSynthCode);
        r.bytesTransferred = op.body ? std::uint64_t(op.bodyLen) : resp.sinkBytes;
        if (r.error.kind != ErrorKind::none && op.body)
            r.bytesTransferred = 0;
        return r;
    }
};

Client::Client(Config cfg) : impl_(new Impl(std::move(cfg))) {}
Client::~Client() = default;
Client::Client(Client&&) noexcept = default;
Client& Client::operator=(Client&&) noexcept = default;

void Client::cancel() {
    impl_->cancel.store(true);
}

Result Client::bucketExists(const std::string& bucket, bool& exists) {
    exists = false;
    impl_->cancel.store(false);
    Impl::Op op;
    op.method = "HEAD";
    op.bucket = bucket;
    op.noBody = true;
    HttpResponse resp;
    Result r = impl_->run(op, resp);
    if (r) {
        exists = true;
        return r;
    }
    if (resp.status == 404) {
        exists = false;
        return Result{}; // definite "no" is a success
    }
    return r; // 403 etc. stay errors — the caller can tell "no bucket" from "no access"
}

Result Client::createBucket(const std::string& bucket) {
    impl_->cancel.store(false);
    Impl::Op op;
    op.method = "PUT";
    op.bucket = bucket;
    // v1 sends no CreateBucketConfiguration body: for us-east-1 and for
    // MinIO/RustFS the LocationConstraint element is not required.
    static const char kEmpty = 0;
    op.body = &kEmpty;
    op.bodyLen = 0;
    HttpResponse resp;
    return impl_->run(op, resp);
}

Result Client::statObject(const std::string& bucket, const std::string& key, ObjectMeta& out) {
    impl_->cancel.store(false);
    Impl::Op op;
    op.method = "HEAD";
    op.bucket = bucket;
    op.key = key;
    op.noBody = true;
    op.meta = &out;
    op.headSynthCode = "NoSuchKey";
    HttpResponse resp;
    Result r = impl_->run(op, resp);
    if (r)
        out.info.key = key;
    return r;
}

Result Client::deleteObject(const std::string& bucket, const std::string& key) {
    impl_->cancel.store(false);
    Impl::Op op;
    op.method = "DELETE";
    op.bucket = bucket;
    op.key = key;
    op.noBody = false;
    HttpResponse resp;
    Result r = impl_->run(op, resp);
    // S3 semantics: deleting a nonexistent key succeeds (some servers say 404).
    if (!r && resp.status == 404)
        return Result{};
    return r;
}

Result Client::putObject(const std::string& bucket, const std::string& key, const void* data,
                         std::size_t len, const PutOptions& opts, const ProgressFn& progress) {
    impl_->cancel.store(false);
    if (data == nullptr && len > 0)
        return Result{Error{ErrorKind::transport, 0, "", "null data with non-zero length", 0}, 0};
    Impl::Op op;
    op.method = "PUT";
    op.bucket = bucket;
    op.key = key;
    if (data == nullptr) {
        // Empty body: use the same non-null sentinel trick as createBucket so
        // this never turns into a bodyless PUT (which transport.cpp's PUT
        // invariant check would reject, and which curl would send as a GET).
        static const char kEmpty = 0;
        op.body = &kEmpty;
        op.bodyLen = 0;
    } else {
        op.body = static_cast<const char*>(data);
        op.bodyLen = len;
    }
    op.progress = progress;
    if (!opts.contentType.empty())
        op.extraHdrs.emplace_back("Content-Type", opts.contentType);
    if (!opts.contentEncoding.empty())
        op.extraHdrs.emplace_back("Content-Encoding", opts.contentEncoding);
    for (const auto& kv : opts.extraHeaders)
        op.extraHdrs.push_back(kv);
    HttpResponse resp;
    return impl_->run(op, resp);
}

Result Client::putFile(const std::string& bucket, const std::string& key,
                       const std::string& filePath, const PutOptions& opts,
                       const ProgressFn& progress) {
    // v1 reads the whole file into memory (targets small/medium objects, and the
    // payload hash must cover the full body anyway). Local I/O failures are
    // reported as transport errors with curlCode == 0.
    std::ifstream f(filePath, std::ios::binary);
    if (!f)
        return Result{Error{ErrorKind::transport, 0, "", "cannot open file: " + filePath, 0}, 0};
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (f.bad())
        return Result{Error{ErrorKind::transport, 0, "", "read failed: " + filePath, 0}, 0};
    return putObject(bucket, key, data.data(), data.size(), opts, progress);
}

Result Client::getObject(const std::string& bucket, const std::string& key, const WriteFn& sink,
                         const ProgressFn& progress, ObjectMeta* meta) {
    impl_->cancel.store(false);
    Impl::Op op;
    op.method = "GET";
    op.bucket = bucket;
    op.key = key;
    op.sink = sink;
    op.progress = progress;
    op.meta = meta;
    HttpResponse resp;
    Result r = impl_->run(op, resp);
    if (r && meta)
        meta->info.key = key;
    return r;
}

Result Client::getToFile(const std::string& bucket, const std::string& key,
                         const std::string& filePath, const ProgressFn& progress,
                         ObjectMeta* meta) {
    const std::string part = filePath + ".part";
    std::FILE* f = std::fopen(part.c_str(), "wb");
    if (!f)
        return Result{Error{ErrorKind::transport, 0, "", "cannot open file: " + part, 0}, 0};
    bool writeFailed = false;
    WriteFn sink = [f, &writeFailed](const char* data, std::size_t len) {
        if (std::fwrite(data, 1, len, f) != len) {
            writeFailed = true;
            return false;
        }
        return true;
    };
    Result r = getObject(bucket, key, sink, progress, meta);
    bool flushOk = (std::fflush(f) == 0);
    std::fclose(f);
    if (r && flushOk) {
        if (std::rename(part.c_str(), filePath.c_str()) != 0) {
            std::remove(part.c_str());
            return Result{Error{ErrorKind::transport, 0, "", "rename failed: " + filePath, 0},
                          r.bytesTransferred};
        }
        return r;
    }
    std::remove(part.c_str());
    // A disk write failure makes the sink return false, which transport.cpp
    // reports as an aborted transfer; surface it as a transport error, not
    // the generic "operation cancelled" (that must mean user/cancel/sink-
    // refusal aborts unrelated to local I/O).
    if (writeFailed)
        return Result{Error{ErrorKind::transport, 0, "", "write failed: " + part, 0}, 0};
    if (r && !flushOk)
        return Result{Error{ErrorKind::transport, 0, "", "write failed: " + part, 0}, 0};
    return r;
}
Result Client::listObjects(const std::string& bucket, const std::string& prefix, bool recursive,
                           const ListFn& onObject) {
    impl_->cancel.store(false);
    std::string token;
    while (true) {
        Impl::Op op;
        op.method = "GET";
        op.bucket = bucket;
        op.query = {{"list-type", "2"}, {"encoding-type", "url"}};
        if (!prefix.empty())
            op.query.emplace_back("prefix", prefix);
        if (!recursive)
            op.query.emplace_back("delimiter", "/");
        if (!token.empty())
            op.query.emplace_back("continuation-token", token);
        HttpResponse resp;
        Result r = impl_->run(op, resp); // sink == null -> body accumulated in resp.body
        if (!r)
            return r;

        detail::ListPage page;
        if (!detail::parseListPage(resp.body, page))
            return Result{Error{ErrorKind::parse, int(resp.status), "",
                                "cannot parse ListObjectsV2 response", 0},
                          0};
        for (const auto& e : page.entries) {
            ObjectInfo info;
            info.key = detail::urlDecode(e.key); // encoding-type=url
            info.size = e.size;
            info.etag = e.etag;
            info.isPrefix = e.isPrefix;
            if (!onObject(info))
                return Result{}; // caller stop is not an error
        }
        if (!page.truncated)
            return Result{};
        if (page.nextToken.empty() || page.nextToken == token)
            return Result{Error{ErrorKind::parse, 0, "",
                                "truncated listing without a fresh continuation token", 0},
                          0};
        token = page.nextToken;
    }
}

} // namespace slims3
