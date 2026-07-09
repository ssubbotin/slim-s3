# slim-s3

A slim S3 client for C++17. One dependency: libcurl. No SDK heft.

See [docs/DESIGN.md](docs/DESIGN.md) for the full design: motivation, API, signing
approach, and testing strategy.

## Build

```bash
cmake -B build && cmake --build build -j
# or as a subproject: add_subdirectory(slim-s3) + target_link_libraries(app slims3::slims3)
```

## Installation

### vcpkg (overlay port)

A vcpkg port lives in this repo under `packaging/vcpkg/ports/slims3` and is not yet
merged upstream into microsoft/vcpkg. Use it as an overlay port until it is:

```bash
vcpkg install slims3 --overlay-ports=<path-to-slim-s3>/packaging/vcpkg/ports
```

Then, in a consumer's `CMakeLists.txt`:

```cmake
find_package(slims3 CONFIG REQUIRED)
target_link_libraries(app PRIVATE slims3::slims3)
```

configuring with the vcpkg toolchain file:

```bash
cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE=<path-to-vcpkg>/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_OVERLAY_PORTS=<path-to-slim-s3>/packaging/vcpkg/ports
```

Upstream submission to microsoft/vcpkg is planned; once merged, the overlay flag
won't be needed and a plain `vcpkg install slims3` will work.

### Conan

A `conanfile.py` lives at the repo root (`name = "slims3"`, `version = "0.1.1"`).
It is not yet published to ConanCenter, so consume it from a local checkout or a
local Conan cache for now:

```bash
git clone https://github.com/ssubbotin/slim-s3.git
cd slim-s3
conan create .
```

Then, in a consumer's `conanfile.txt`:

```ini
[requires]
slims3/0.1.1

[generators]
CMakeToolchain
CMakeDeps
```

or `conanfile.py`:

```python
def requirements(self):
    self.requires("slims3/0.1.1")
```

and in the consumer's `CMakeLists.txt`, the same target as the other install paths:

```cmake
find_package(slims3 CONFIG REQUIRED)
target_link_libraries(app PRIVATE slims3::slims3)
```

Submission to ConanCenter (conan-center-index) is planned; once accepted, a plain
`conan install --requires=slims3/0.1.1` from the default remote will work without
a local `conan create`.

## Usage

```cpp
#include <slims3/slims3.hpp>

slims3::Config cfg;
cfg.endpoint = "http://127.0.0.1:9000";
cfg.accessKey = "minioadmin";
cfg.secretKey = "minioadmin";
slims3::Client s3(cfg);

slims3::PutOptions po;
po.contentType = "application/octet-stream";
std::string data = "hello";
if (auto r = s3.putObject("bucket", "path/key.bin", data.data(), data.size(), po); !r)
    fprintf(stderr, "%s\n", r.error.message.c_str());

s3.getToFile("bucket", "path/key.bin", "/tmp/key.bin");
```

Errors carry `httpStatus` and `s3Code` — retry policy stays in your hands:

```cpp
bool retryable(const slims3::Error& e) {
    return e.kind == slims3::ErrorKind::transport ||
           (e.kind != slims3::ErrorKind::cancelled && e.httpStatus >= 500);
}
```

## Thread safety

A `Client` runs one operation at a time — don't share an instance between
threads; create one per worker thread instead (they're cheap: the persistent
curl handle is essentially the only state). The one exception is `cancel()`,
which is safe to call from any thread and aborts the operation currently
running on that client. Separate instances are fully independent — no shared
global state (`curl_global_init` is handled once, thread-safely). Progress and
write callbacks run on the thread that called the operation.

See [docs/DESIGN.md](docs/DESIGN.md) §7 for the full concurrency model.

## Why

- **aws-sdk-cpp** brings the aws-crt dependency tree and slow builds for what is,
  in most services, seven HTTP calls.
- **minio-cpp** has transport defects (unbounded `select()`, no timeout options —
  a silent socket hangs the calling thread forever) and an unreleased breaking
  rewrite on `main`.
- Small clients on GitHub are learning projects, abandoned, or require C++23.

slim-s3 aims to be the boring, dependable middle:

- C++17, libcurl as the only dependency (own SigV4 signing, vendored SHA-256)
- the whole library adds **~180 KB** to a stripped Release binary (x86-64 Linux,
  gcc `-O2`) — run `tools/size-report.sh` for the number on your toolchain plus
  a per-source-file breakdown
- timeouts and a low-speed stall guard **on by default** — nothing hangs forever
- cooperative cancellation, upload/download progress callbacks
- structured errors (HTTP status + S3 code) so you can build your own retry policy
- connection reuse across requests
- tested against MinIO and RustFS, signer verified against the official AWS
  SigV4 test vectors

## Scope (v1)

`bucketExists` · `createBucket` · `putObject` / `putFile` · `getObject` /
`getToFile` · `statObject` · `deleteObject` · `listObjects` (paginated,
prefix/delimiter) — path-style addressing, objects up to 5 GB (no multipart yet).

## License

[MIT](LICENSE)
