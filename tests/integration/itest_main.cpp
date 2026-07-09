// Integration scenarios driven by environment variables:
//   SLIMS3_ENDPOINT / SLIMS3_ACCESS / SLIMS3_SECRET  - target server (full run)
//   SLIMS3_SILENT_ENDPOINT                            - silent-server mode only
//   SLIMS3_STUB_ENDPOINT / SLIMS3_STUB_SCENARIO       - raw-socket stub mode (see
//                                                        stub_server.py / run_stub.sh)
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <csignal>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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

// Creates the bucket only if it doesn't already exist, so independent
// TEST_CASEs that each use their own suffix of bucketName() stay safe to
// re-run against a long-lived server without tripping over each other.
static void ensureBucket(Client& cl, const std::string& b) {
    bool exists = false;
    REQUIRE(cl.bucketExists(b, exists));
    if (!exists)
        REQUIRE(cl.createBucket(b));
}

TEST_CASE("silent server: stall guard fires instead of hanging forever") {
    std::string ep = env("SLIMS3_SILENT_ENDPOINT");
    if (ep.empty())
        return; // not in silent mode
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
    auto secs =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0)
            .count();
    CHECK_FALSE(r);
    CHECK(r.error.kind == ErrorKind::transport);
    CHECK(secs < 30); // the whole point of this library
}

TEST_CASE("full server matrix") {
    if (env("SLIMS3_ENDPOINT").empty())
        return; // silent-only mode
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
    for (int i = 0; i < 1000; ++i)
        payload[std::size_t(i) * 100] = char(i & 0xff);
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
    Result rGet = cl.getObject(
        b, "dir/k ey+with=chars.bin",
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
    {
        std::ifstream in("/tmp/slims3_itest.bin", std::ios::binary);
        std::string fileContents((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
        CHECK(fileContents == payload);
    }

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
        if (oi.isPrefix)
            sawPrefix = true; // none expected under dir/ (flat)
        if (!oi.isPrefix)
            sawKey = true;
        return true;
    }));
    CHECK(sawKey);
    (void)sawPrefix;
    bool sawManyPrefix = false;
    REQUIRE(cl.listObjects(b, "", false, [&](const ObjectInfo& oi) {
        if (oi.isPrefix && oi.key == "many/")
            sawManyPrefix = true;
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
    Result rCancel = cl.getObject(b, "dir/k ey+with=chars.bin", [&](const char*, std::size_t) {
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

// ===========================================================================
// Piece 3 -- cancel()/progress against real MinIO/RustFS (SLIMS3_ENDPOINT gate)
// ===========================================================================

TEST_CASE("getObject: ProgressFn observes forward progress and lets the transfer finish") {
    if (env("SLIMS3_ENDPOINT").empty())
        return;
    Client cl(baseConfig());
    const std::string b = bucketName() + "-prog";
    ensureBucket(cl, b);
    std::string payload(4 * 1024 * 1024, '\x37'); // 4 MiB
    REQUIRE(cl.putObject(b, "big", payload.data(), payload.size()));

    bool called = false;
    std::uint64_t lastDone = 0;
    std::string got;
    Result r = cl.getObject(
        b, "big",
        [&](const char* d, std::size_t n) {
            got.append(d, n);
            return true;
        },
        [&](std::uint64_t done, std::uint64_t /*total*/) {
            called = true;
            CHECK(done >= lastDone);
            lastDone = done;
            return true;
        });
    REQUIRE(r);
    CHECK(called);
    CHECK(got == payload);
}

TEST_CASE("getObject: ProgressFn returning false aborts with ErrorKind::cancelled") {
    if (env("SLIMS3_ENDPOINT").empty())
        return;
    Client cl(baseConfig());
    const std::string b = bucketName() + "-prog";
    ensureBucket(cl, b);
    std::string payload(4 * 1024 * 1024, '\x37'); // 4 MiB
    REQUIRE(cl.putObject(b, "big", payload.data(), payload.size()));

    int ticks = 0;
    Result r = cl.getObject(
        b, "big", [](const char*, std::size_t) { return true; },
        [&](std::uint64_t, std::uint64_t) {
            ++ticks;
            return false;
        });
    CHECK_FALSE(r);
    CHECK(r.error.kind == ErrorKind::cancelled);
    CHECK(ticks >= 1);
}

TEST_CASE("Client::cancel() from another thread aborts an in-flight download") {
    if (env("SLIMS3_ENDPOINT").empty())
        return;
    Client cl(baseConfig());
    const std::string b = bucketName() + "-cancel";
    ensureBucket(cl, b);
    // A few MB is plenty: the canceller below is gated on a first-byte
    // handshake rather than a fixed sleep racing against transfer speed, so
    // the object just needs to still be transferring after the first chunk.
    std::string payload(8 * 1024 * 1024, '\x11'); // 8 MiB
    REQUIRE(cl.putObject(b, "huge", payload.data(), payload.size()));

    std::atomic<bool> cancelCalled{false};
    std::atomic<bool> firstByteSeen{false};
    std::thread canceller([&] {
        // Spin-wait for the sink to have been invoked at least once. This
        // guarantees getObject already reset the cancel flag and started the
        // transfer (no before-reset race) and that the transfer is still
        // mid-flight (no completion race), regardless of hardware speed.
        int spins = 0;
        while (!firstByteSeen.load()) {
            if (++spins > 100000)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            else
                std::this_thread::yield();
        }
        cl.cancel();
        cancelCalled = true;
    });
    Result r = cl.getObject(b, "huge", [&](const char*, std::size_t) {
        firstByteSeen = true;
        return true;
    });
    canceller.join();
    CHECK(cancelCalled);
    CHECK_FALSE(r);
    CHECK(r.error.kind == ErrorKind::cancelled);
}

TEST_CASE("putObject rejected by the server reports bytesTransferred == 0") {
    if (env("SLIMS3_ENDPOINT").empty())
        return;
    Config bad = baseConfig();
    bad.secretKey = "wrong-secret-key-000000";
    Client cl(bad);
    std::string data(1000, 'z');
    Result r = cl.putObject(bucketName() + "-badput", "k", data.data(), data.size());
    CHECK_FALSE(r);
    CHECK(r.error.httpStatus == 403);
    CHECK(r.bytesTransferred == 0);
}

// ===========================================================================
// Piece 4 -- putFile / getToFile filesystem-failure paths (SLIMS3_ENDPOINT gate)
// ===========================================================================

TEST_CASE("putFile: happy path round-trips a temp file's bytes") {
    if (env("SLIMS3_ENDPOINT").empty())
        return;
    Client cl(baseConfig());
    const std::string b = bucketName() + "-putfile";
    ensureBucket(cl, b);

    const std::string path = "/tmp/slims3_itest_putfile_src.bin";
    std::string content(50000, '\x99');
    {
        std::ofstream out(path, std::ios::binary);
        out.write(content.data(), std::streamsize(content.size()));
    }
    REQUIRE(cl.putFile(b, "from-file", path));

    std::string got;
    Result r = cl.getObject(b, "from-file", [&](const char* d, std::size_t n) {
        got.append(d, n);
        return true;
    });
    REQUIRE(r);
    CHECK(got == content);
    std::remove(path.c_str());
}

TEST_CASE("putFile: nonexistent source path is a transport error") {
    if (env("SLIMS3_ENDPOINT").empty())
        return;
    Client cl(baseConfig());
    Result r = cl.putFile(bucketName(), "k", "/tmp/slims3_itest_does_not_exist_xyz.bin");
    CHECK_FALSE(r);
    CHECK(r.error.kind == ErrorKind::transport);
    CHECK(r.error.message.find("cannot open file") != std::string::npos);
}

TEST_CASE("getToFile: pre-existing .part file is refused (O_EXCL)") {
    if (env("SLIMS3_ENDPOINT").empty())
        return;
    const std::string target = "/tmp/slims3_itest_preexisting_part.bin";
    const std::string part = target + ".part";
    std::remove(target.c_str());
    std::remove(part.c_str());
    {
        std::ofstream f(part);
        f << "leftover";
    }

    Client cl(baseConfig());
    // This fails before any network I/O happens, so the bucket/key need not exist.
    Result r = cl.getToFile(bucketName(), "any-key", target);
    CHECK_FALSE(r);
    CHECK(r.error.kind == ErrorKind::transport);
    CHECK(r.error.message.find("cannot create temp file") != std::string::npos);
    std::remove(part.c_str());
}

TEST_CASE("getToFile: an underlying getObject failure passes through unchanged") {
    if (env("SLIMS3_ENDPOINT").empty())
        return;
    Client cl(baseConfig());
    const std::string b = bucketName() + "-missingkey";
    ensureBucket(cl, b);
    const std::string target = "/tmp/slims3_itest_missingkey.bin";
    std::remove(target.c_str());
    std::remove((target + ".part").c_str());

    Result r = cl.getToFile(b, "no/such/key/for-getToFile", target);
    CHECK_FALSE(r);
    CHECK(r.error.kind == ErrorKind::s3);
    CHECK(r.error.httpStatus == 404);
    std::remove(target.c_str());
    std::remove((target + ".part").c_str());
}

#ifndef _WIN32
TEST_CASE("getToFile: rename() failure surfaces as a transport error") {
    if (env("SLIMS3_ENDPOINT").empty())
        return;
    Client cl(baseConfig());
    const std::string b = bucketName() + "-renamefail";
    ensureBucket(cl, b);
    REQUIRE(cl.putObject(b, "k", "hello", 5));

    // The destination is an existing empty directory: rename(regular-file,
    // existing-directory) fails with EISDIR after a successful download,
    // without needing any permission trickery.
    const std::string dir = "/tmp/slims3_itest_rename_target_dir";
    std::remove((dir + ".part").c_str());
    ::rmdir(dir.c_str());
    REQUIRE(::mkdir(dir.c_str(), 0700) == 0);

    Result r = cl.getToFile(b, "k", dir);
    CHECK_FALSE(r);
    CHECK(r.error.kind == ErrorKind::transport);
    CHECK(r.error.message.find("rename failed") != std::string::npos);
    std::remove((dir + ".part").c_str());
    ::rmdir(dir.c_str());
}

TEST_CASE("getToFile: fwrite short write (disk full) reports transport, not cancelled") {
    if (env("SLIMS3_ENDPOINT").empty())
        return;
    Client cl(baseConfig());
    const std::string b = bucketName() + "-writefail";
    ensureBucket(cl, b);
    std::string payload(200000, 'w');
    REQUIRE(cl.putObject(b, "k", payload.data(), payload.size()));

    const std::string target = "/tmp/slims3_itest_writefail.bin";
    std::remove(target.c_str());
    std::remove((target + ".part").c_str());

    // Force fwrite() to fail on (or shortly after) the first chunk: cap
    // RLIMIT_FSIZE far below the object size and ignore SIGXFSZ so the write
    // syscall returns EFBIG/a short count instead of killing the process
    // with the default signal disposition.
    struct rlimit oldLim{};
    REQUIRE(getrlimit(RLIMIT_FSIZE, &oldLim) == 0);
    struct sigaction oldSa{};
    struct sigaction sa{};
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    REQUIRE(sigaction(SIGXFSZ, &sa, &oldSa) == 0);
    struct rlimit tinyLim = oldLim;
    tinyLim.rlim_cur = 16;
    REQUIRE(setrlimit(RLIMIT_FSIZE, &tinyLim) == 0);

    Result r = cl.getToFile(b, "k", target);

    setrlimit(RLIMIT_FSIZE, &oldLim);
    sigaction(SIGXFSZ, &oldSa, nullptr);

    CHECK_FALSE(r);
    CHECK(r.error.kind == ErrorKind::transport);
    CHECK(r.error.message.find("write failed") != std::string::npos);
    std::remove(target.c_str());
    std::remove((target + ".part").c_str());
}

TEST_CASE("getToFile: flush failure after all fwrite calls buffered OK reports transport") {
    if (env("SLIMS3_ENDPOINT").empty())
        return;
    Client cl(baseConfig());
    const std::string b = bucketName() + "-flushfail";
    ensureBucket(cl, b);
    // Small enough to sit entirely inside libc's stdio write buffer: every
    // fwrite() call inside the sink succeeds (writeFailed stays false), and
    // the RLIMIT_FSIZE violation only surfaces later at fflush() -- this
    // exercises "r && !flushOk" (client.cpp getToFile), distinct from the
    // writeFailed path exercised by the large-object test above.
    std::string payload(2000, 'q');
    REQUIRE(cl.putObject(b, "k", payload.data(), payload.size()));

    const std::string target = "/tmp/slims3_itest_flushfail.bin";
    std::remove(target.c_str());
    std::remove((target + ".part").c_str());

    struct rlimit oldLim{};
    REQUIRE(getrlimit(RLIMIT_FSIZE, &oldLim) == 0);
    struct sigaction oldSa{};
    struct sigaction sa{};
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    REQUIRE(sigaction(SIGXFSZ, &sa, &oldSa) == 0);
    struct rlimit tinyLim = oldLim;
    tinyLim.rlim_cur = 100; // < payload size, > 0: overflow surfaces at fflush, not fwrite
    REQUIRE(setrlimit(RLIMIT_FSIZE, &tinyLim) == 0);

    Result r = cl.getToFile(b, "k", target);

    setrlimit(RLIMIT_FSIZE, &oldLim);
    sigaction(SIGXFSZ, &oldSa, nullptr);

    CHECK_FALSE(r);
    CHECK(r.error.kind == ErrorKind::transport);
    CHECK(r.error.message.find("write failed") != std::string::npos);
    std::remove(target.c_str());
    std::remove((target + ".part").c_str());
}
#endif // _WIN32

// ===========================================================================
// Piece 2 -- raw-socket stub scenarios (SLIMS3_STUB_ENDPOINT/SCENARIO gate)
// ===========================================================================
// Each stub run (tests/integration/run_stub.sh <scenario>) starts a fresh
// stub_server.py process for exactly one scenario and runs this whole
// binary; every TEST_CASE below returns immediately unless it's the one
// matching SLIMS3_STUB_SCENARIO, so only one of them actually executes.

static bool stubIs(const char* name) {
    return !env("SLIMS3_STUB_ENDPOINT").empty() && env("SLIMS3_STUB_SCENARIO") == name;
}

static Config stubConfig() {
    Config c;
    c.endpoint = env("SLIMS3_STUB_ENDPOINT");
    c.accessKey = "stubkey";
    c.secretKey = "stubsecret";
    c.connectTimeoutSec = 5;
    c.lowSpeedTimeSec = 5;
    return c;
}

TEST_CASE("stub: header count flood aborts with transport, not cancelled") {
    if (!stubIs("header_count_flood"))
        return;
    Client cl(stubConfig());
    Result r = cl.listObjects("b", "", true, [](const ObjectInfo&) { return true; });
    CHECK_FALSE(r);
    CHECK(r.error.kind == ErrorKind::transport);
    CHECK(r.error.message.find("headers exceeded") != std::string::npos);
}

TEST_CASE("stub: oversized header block (byte cap) aborts with transport, not cancelled") {
    // kMaxHeaderBytes (256 KiB) is now below libcurl's own cumulative
    // MAX_HTTP_RESP_HEADER_SIZE (300 KiB), so slim-s3's byte cap fires first
    // and owns the error, unlike before. The stub sends fewer than
    // kMaxHeaderCount (2000) headers whose cumulative bytes still exceed
    // kMaxHeaderBytes, so this exercises the BYTE-cap branch specifically
    // (header_count_flood above exercises the count-cap branch). The two
    // caps share transport.cpp's "response headers exceeded limit" message,
    // so that message is what's asserted here.
    if (!stubIs("header_byte_flood"))
        return;
    Client cl(stubConfig());
    Result r = cl.listObjects("b", "", true, [](const ObjectInfo&) { return true; });
    CHECK_FALSE(r);
    CHECK(r.error.kind == ErrorKind::transport);
    CHECK(r.error.message.find("headers exceeded") != std::string::npos);
}

TEST_CASE("stub: status line flood aborts with transport, not cancelled") {
    if (!stubIs("status_line_flood"))
        return;
    Client cl(stubConfig());
    Result r = cl.listObjects("b", "", true, [](const ObjectInfo&) { return true; });
    CHECK_FALSE(r);
    CHECK(r.error.kind == ErrorKind::transport);
    CHECK(r.error.message.find("interim/redirect flood") != std::string::npos);
}

TEST_CASE("stub: oversized error body aborts with transport, not cancelled") {
    if (!stubIs("body_overflow"))
        return;
    Client cl(stubConfig());
    std::string got;
    Result r = cl.getObject("b", "k", [&](const char* d, std::size_t n) {
        got.append(d, n);
        return true;
    });
    CHECK_FALSE(r);
    CHECK(r.error.kind == ErrorKind::transport);
    CHECK(r.error.message.find("8 MiB") != std::string::npos);
}

TEST_CASE("stub: deleteObject on a 404 key is success (S3 semantics)") {
    if (!stubIs("delete_404"))
        return;
    Client cl(stubConfig());
    Result r = cl.deleteObject("b", "no-such-key");
    CHECK(bool(r));
}

TEST_CASE("stub: listObjects on non-XML body returns ErrorKind::parse") {
    if (!stubIs("list_bad_xml"))
        return;
    Client cl(stubConfig());
    Result r = cl.listObjects("b", "", true, [](const ObjectInfo&) { return true; });
    CHECK_FALSE(r);
    CHECK(r.error.kind == ErrorKind::parse);
}

TEST_CASE("stub: listObjects truncated with an empty continuation token is ErrorKind::parse") {
    if (!stubIs("list_empty_token"))
        return;
    Client cl(stubConfig());
    Result r = cl.listObjects("b", "", true, [](const ObjectInfo&) { return true; });
    CHECK_FALSE(r);
    CHECK(r.error.kind == ErrorKind::parse);
    CHECK(r.error.message.find("fresh continuation token") != std::string::npos);
}

TEST_CASE("stub: mid-pagination page failure surfaces the page's error") {
    if (!stubIs("list_page_fail"))
        return;
    Client cl(stubConfig());
    std::vector<std::string> keys;
    Result r = cl.listObjects("b", "", true, [&](const ObjectInfo& oi) {
        keys.push_back(oi.key);
        return true;
    });
    CHECK_FALSE(r);
    CHECK((r.error.kind == ErrorKind::transport || r.error.kind == ErrorKind::http));
    REQUIRE(keys.size() == 1);
    CHECK(keys[0] == "first-key");
}

TEST_CASE("stub: putObject on a 500 response reports bytesTransferred == 0") {
    if (!stubIs("put_500"))
        return;
    Client cl(stubConfig());
    Result r = cl.putObject("b", "k", "x", 1);
    CHECK_FALSE(r);
    CHECK(r.bytesTransferred == 0);
}

TEST_CASE("stub: TLS option lines + operationTimeoutSec execute cleanly over http") {
    if (!stubIs("ok"))
        return;
    Config c = stubConfig();
    c.caBundlePath = "/dev/null";
    c.tlsVerify = false;
    c.operationTimeoutSec = 5;
    Client cl(c);
    Result r = cl.listObjects("b", "", true, [](const ObjectInfo&) { return true; });
    CHECK(bool(r));
}

// Extra belt (run_stub.sh's own name-validation guard is sufficient on its
// own): if SLIMS3_STUB_SCENARIO is set to a name none of the stubIs() checks
// above recognize, fail loudly here instead of every TEST_CASE above quietly
// returning and the run reporting SUCCESS with zero assertions.
TEST_CASE("stub: SLIMS3_STUB_SCENARIO, if set, matches a known stubIs() case") {
    std::string scen = env("SLIMS3_STUB_SCENARIO");
    if (env("SLIMS3_STUB_ENDPOINT").empty() || scen.empty())
        return;
    static const char* kKnownScenarios[] = {"header_count_flood",
                                            "header_byte_flood",
                                            "status_line_flood",
                                            "body_overflow",
                                            "delete_404",
                                            "list_bad_xml",
                                            "list_empty_token",
                                            "list_page_fail",
                                            "put_500",
                                            "ok"};
    bool known = false;
    for (const char* s : kKnownScenarios)
        if (scen == s) {
            known = true;
            break;
        }
    CHECK(known);
}
