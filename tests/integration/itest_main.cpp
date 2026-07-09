// Integration scenarios driven by environment variables:
//   SLIMS3_ENDPOINT / SLIMS3_ACCESS / SLIMS3_SECRET  - target server (full run)
//   SLIMS3_SILENT_ENDPOINT                            - silent-server mode only
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

#include "doctest.h"
#include "slims3/slims3.hpp"

using namespace slims3;

static std::string env(const char* n) {
    const char* v = std::getenv(n);
    return v ? v : "";
}

static Config baseConfig() {
    Config c;
    c.endpoint = env("SLIMS3_ENDPOINT");
    c.accessKey = env("SLIMS3_ACCESS");
    c.secretKey = env("SLIMS3_SECRET");
    return c;
}

static std::string bucketName() {
    static std::string b = "slims3-itest-" + std::to_string(std::time(nullptr));
    return b;
}

TEST_CASE("silent server: stall guard fires instead of hanging forever") {
    std::string ep = env("SLIMS3_SILENT_ENDPOINT");
    if (ep.empty()) return;  // not in silent mode
    Config c;
    c.endpoint = ep;
    c.accessKey = "x";
    c.secretKey = "y";
    c.connectTimeoutSec = 5;
    c.lowSpeedTimeSec = 3;
    Client cl(c);
    auto t0 = std::chrono::steady_clock::now();
    bool exists = false;
    Result r = cl.bucketExists("any", exists);
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - t0).count();
    CHECK_FALSE(r);
    CHECK(r.error.kind == ErrorKind::transport);
    CHECK(secs < 30);  // the whole point of this library
}

TEST_CASE("full server matrix") {
    if (env("SLIMS3_ENDPOINT").empty()) return;  // silent-only mode
    Client cl(baseConfig());
    const std::string b = bucketName();

    // -- bucket lifecycle
    bool exists = true;
    REQUIRE(cl.bucketExists(b, exists));
    CHECK_FALSE(exists);
    REQUIRE(cl.createBucket(b));
    REQUIRE(cl.bucketExists(b, exists));
    CHECK(exists);

    // -- wrong credentials are an error, not "false"
    Config bad = baseConfig();
    bad.secretKey = "wrong-secret-key-000000";
    Client badCl(bad);
    bool e2 = false;
    Result rBad = badCl.bucketExists(b, e2);
    CHECK_FALSE(rBad);
    CHECK(rBad.error.httpStatus == 403);

    // -- put / stat / get round-trip with Content-Encoding passthrough
    std::string payload(100000, '\x5a');
    for (int i = 0; i < 1000; ++i) payload[std::size_t(i) * 100] = char(i & 0xff);
    PutOptions po;
    po.contentType = "application/octet-stream";
    po.contentEncoding = "zstd";
    REQUIRE(cl.putObject(b, "dir/k ey+with=chars.bin", payload.data(), payload.size(), po));

    ObjectMeta meta;
    REQUIRE(cl.statObject(b, "dir/k ey+with=chars.bin", meta));
    CHECK(meta.info.size == payload.size());
    CHECK(meta.contentEncoding == "zstd");
    CHECK_FALSE(meta.info.etag.empty());

    std::string got;
    ObjectMeta gmeta;
    Result rGet = cl.getObject(b, "dir/k ey+with=chars.bin",
                               [&](const char* d, std::size_t n) {
                                   got.append(d, n);
                                   return true;
                               },
                               {}, &gmeta);
    REQUIRE(rGet);
    CHECK(rGet.bytesTransferred == payload.size());
    CHECK(got == payload);
    CHECK(gmeta.contentEncoding == "zstd");

    // -- getToFile atomic write
    REQUIRE(cl.getToFile(b, "dir/k ey+with=chars.bin", "/tmp/slims3_itest.bin"));

    // -- stat of a missing key
    ObjectMeta missing;
    Result rMiss = cl.statObject(b, "no/such/key", missing);
    CHECK_FALSE(rMiss);
    CHECK(rMiss.error.httpStatus == 404);
    CHECK(rMiss.error.s3Code == "NoSuchKey");

    // -- listing: pagination beyond one page (>1000 keys)
    for (int i = 0; i < 1050; ++i) {
        std::string k = "many/obj-" + std::to_string(10000 + i);
        REQUIRE(cl.putObject(b, k, "x", 1));
    }
    std::size_t seen = 0;
    REQUIRE(cl.listObjects(b, "many/", true, [&](const ObjectInfo&) {
        ++seen;
        return true;
    }));
    CHECK(seen == 1050);

    // -- non-recursive listing yields CommonPrefixes
    bool sawPrefix = false, sawKey = false;
    REQUIRE(cl.listObjects(b, "dir/", false, [&](const ObjectInfo& oi) {
        if (oi.isPrefix) sawPrefix = true;   // none expected under dir/ (flat)
        if (!oi.isPrefix) sawKey = true;
        return true;
    }));
    CHECK(sawKey);
    (void)sawPrefix;
    bool sawManyPrefix = false;
    REQUIRE(cl.listObjects(b, "", false, [&](const ObjectInfo& oi) {
        if (oi.isPrefix && oi.key == "many/") sawManyPrefix = true;
        return true;
    }));
    CHECK(sawManyPrefix);

    // -- early stop is not an error
    std::size_t firstOnly = 0;
    REQUIRE(cl.listObjects(b, "many/", true, [&](const ObjectInfo&) {
        ++firstOnly;
        return false;
    }));
    CHECK(firstOnly == 1);

    // -- cancellation mid-download (sink refuses the very first chunk)
    bool sawData = false;
    Result rCancel = cl.getObject(b, "dir/k ey+with=chars.bin",
                                  [&](const char*, std::size_t) {
                                      sawData = true;
                                      return false;
                                  });
    CHECK(sawData);
    CHECK_FALSE(rCancel);
    CHECK(rCancel.error.kind == ErrorKind::cancelled);

    // -- delete: real key, then the same (already gone) key
    REQUIRE(cl.deleteObject(b, "dir/k ey+with=chars.bin"));
    REQUIRE(cl.deleteObject(b, "dir/k ey+with=chars.bin"));
}
