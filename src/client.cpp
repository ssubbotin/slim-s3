#include <curl/curl.h>

#include <atomic>
#include <cstdio>
#include <ctime>
#include <fstream>

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

// putObject/putFile/getObject/getToFile/listObjects arrive in Tasks 8-9.
Result Client::putObject(const std::string&, const std::string&, const void*, std::size_t,
                         const PutOptions&, const ProgressFn&) {
    return Result{Error{ErrorKind::transport, 0, "", "not implemented", 0}, 0};
}
Result Client::putFile(const std::string&, const std::string&, const std::string&,
                       const PutOptions&, const ProgressFn&) {
    return Result{Error{ErrorKind::transport, 0, "", "not implemented", 0}, 0};
}
Result Client::getObject(const std::string&, const std::string&, const WriteFn&, const ProgressFn&,
                         ObjectMeta*) {
    return Result{Error{ErrorKind::transport, 0, "", "not implemented", 0}, 0};
}
Result Client::getToFile(const std::string&, const std::string&, const std::string&,
                         const ProgressFn&, ObjectMeta*) {
    return Result{Error{ErrorKind::transport, 0, "", "not implemented", 0}, 0};
}
Result Client::listObjects(const std::string&, const std::string&, bool, const ListFn&) {
    return Result{Error{ErrorKind::transport, 0, "", "not implemented", 0}, 0};
}

} // namespace slims3
