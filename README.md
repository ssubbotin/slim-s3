# slim-s3

A slim S3 client for C++17. One dependency: libcurl. No SDK heft.

**Status: design phase.** See [docs/DESIGN.md](docs/DESIGN.md) for the full design:
motivation, API, signing approach, and testing strategy. Implementation follows.

## Why

- **aws-sdk-cpp** brings the aws-crt dependency tree and slow builds for what is,
  in most services, seven HTTP calls.
- **minio-cpp** has transport defects (unbounded `select()`, no timeout options —
  a silent socket hangs the calling thread forever) and an unreleased breaking
  rewrite on `main`.
- Small clients on GitHub are learning projects, abandoned, or require C++23.

slim-s3 aims to be the boring, dependable middle:

- C++17, libcurl as the only dependency (own SigV4 signing, vendored SHA-256)
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
