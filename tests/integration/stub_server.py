#!/usr/bin/env python3
"""Raw-socket "malicious/misbehaving S3 server" stub.

Like silent_server.py, this is a plain TCP socket server (NOT http.server):
the scenarios below need byte-level control over status lines, headers, and
body framing that Python's http.server would normalize away. It does NOT
validate SigV4 -- the client signs the request, the stub ignores auth
entirely and replies with a canned response for the scenario named by the
SCENARIO env var.

Usage: stub_server.py <port>
"""
import os
import socket
import sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9998
SCENARIO = os.environ.get("SCENARIO", "")


def read_request(conn):
    """Read the request line + headers (until the blank line), then drain
    any request body per Content-Length so the client's send() doesn't wedge
    on a full socket buffer before we reply. Returns the request line."""
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = conn.recv(4096)
        if not chunk:
            break
        buf += chunk
    head, _, rest = buf.partition(b"\r\n\r\n")
    lines = head.split(b"\r\n")
    request_line = lines[0].decode("latin-1") if lines else ""
    content_length = 0
    for line in lines[1:]:
        if line.lower().startswith(b"content-length:"):
            try:
                content_length = int(line.split(b":", 1)[1].strip())
            except ValueError:
                content_length = 0
    have = len(rest)
    while have < content_length:
        chunk = conn.recv(min(65536, content_length - have))
        if not chunk:
            break
        have += len(chunk)
    return request_line


def send(conn, data):
    try:
        conn.sendall(data)
    except (BrokenPipeError, ConnectionResetError, OSError):
        pass


def status_line_flood(conn):
    # kMaxStatusLines is 32; stream well past that before the real response.
    block = b"HTTP/1.1 100 Continue\r\n\r\n"
    send(conn, block * 40)
    send(conn, b"HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")


def header_count_flood(conn):
    # kMaxHeaderCount is 2000; send well past that in one status block.
    hdrs = "".join("X-Pad-%d: v\r\n" % i for i in range(2500))
    resp = "HTTP/1.1 200 OK\r\n%sContent-Length: 0\r\nConnection: close\r\n\r\n" % hdrs
    send(conn, resp.encode("latin-1"))


def header_byte_flood(conn):
    # kMaxHeaderBytes is 256 KiB; 400 headers * ~700-byte values ~= 280 KiB
    # cumulative, comfortably over the byte cap while staying well under the
    # 2000-line count cap (400 < 2000) -- the byte cap trips first, around
    # header ~370, so this exercises the BYTE-cap branch specifically, not
    # the count cap (which header_count_flood covers separately).
    value = "A" * 700
    hdrs = "".join("X-Pad-%d: %s\r\n" % (i, value) for i in range(400))
    resp = "HTTP/1.1 200 OK\r\n%sContent-Length: 0\r\nConnection: close\r\n\r\n" % hdrs
    send(conn, resp.encode("latin-1"))


def body_overflow(conn):
    # Non-2xx error body over the 8 MiB in-memory accumulation cap.
    body = b"E" * (9 * 1024 * 1024)
    head = ("HTTP/1.1 403 Forbidden\r\nContent-Length: %d\r\n"
            "Connection: close\r\n\r\n" % len(body)).encode("latin-1")
    send(conn, head + body)


def delete_404(conn):
    body = b"not found"
    head = ("HTTP/1.1 404 Not Found\r\nContent-Length: %d\r\n"
            "Connection: close\r\n\r\n" % len(body)).encode("latin-1")
    send(conn, head + body)


def list_bad_xml(conn):
    body = b"this is not xml at all <<>>"
    head = ("HTTP/1.1 200 OK\r\nContent-Length: %d\r\n"
            "Connection: close\r\n\r\n" % len(body)).encode("latin-1")
    send(conn, head + body)


def list_empty_token(conn):
    body = (b"<?xml version=\"1.0\"?>\n"
            b"<ListBucketResult><Name>b</Name>"
            b"<IsTruncated>true</IsTruncated>"
            b"<NextContinuationToken></NextContinuationToken>"
            b"</ListBucketResult>")
    head = ("HTTP/1.1 200 OK\r\nContent-Length: %d\r\n"
            "Connection: close\r\n\r\n" % len(body)).encode("latin-1")
    send(conn, head + body)


def list_page_fail(conn, request_line):
    if "continuation-token=" in request_line:
        body = b"stub mid-pagination failure"
        head = ("HTTP/1.1 500 Internal Server Error\r\nContent-Length: %d\r\n"
                "Connection: close\r\n\r\n" % len(body)).encode("latin-1")
        send(conn, head + body)
        return
    body = (b"<?xml version=\"1.0\"?>\n"
            b"<ListBucketResult><Name>b</Name>"
            b"<IsTruncated>true</IsTruncated>"
            b"<NextContinuationToken>t1</NextContinuationToken>"
            b"<Contents><Key>first-key</Key><Size>3</Size>"
            b"<ETag>&quot;abc&quot;</ETag></Contents>"
            b"</ListBucketResult>")
    head = ("HTTP/1.1 200 OK\r\nContent-Length: %d\r\n"
            "Connection: close\r\n\r\n" % len(body)).encode("latin-1")
    send(conn, head + body)


def put_500(conn):
    body = b"nope"
    head = ("HTTP/1.1 500 Internal Server Error\r\nContent-Length: %d\r\n"
            "Connection: close\r\n\r\n" % len(body)).encode("latin-1")
    send(conn, head + body)


def ok(conn):
    # Plain, well-formed response used where the scenario under test is a
    # Transport::execute option (TLS lines / operationTimeoutSec) rather than
    # the response body/headers themselves.
    body = (b"<?xml version=\"1.0\"?>\n"
            b"<ListBucketResult><Name>b</Name>"
            b"<IsTruncated>false</IsTruncated></ListBucketResult>")
    head = ("HTTP/1.1 200 OK\r\nContent-Length: %d\r\n"
            "Connection: close\r\n\r\n" % len(body)).encode("latin-1")
    send(conn, head + body)


SCENARIOS = {
    "status_line_flood": lambda conn, _rl: status_line_flood(conn),
    "header_count_flood": lambda conn, _rl: header_count_flood(conn),
    "header_byte_flood": lambda conn, _rl: header_byte_flood(conn),
    "body_overflow": lambda conn, _rl: body_overflow(conn),
    "delete_404": lambda conn, _rl: delete_404(conn),
    "list_bad_xml": lambda conn, _rl: list_bad_xml(conn),
    "list_empty_token": lambda conn, _rl: list_empty_token(conn),
    "list_page_fail": list_page_fail,
    "put_500": lambda conn, _rl: put_500(conn),
}


def handle(conn):
    request_line = read_request(conn)
    SCENARIOS.get(SCENARIO, lambda conn, _rl: ok(conn))(conn, request_line)


def main():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", PORT))
    srv.listen(16)
    print("stub server (%s) on 127.0.0.1:%d" % (SCENARIO or "ok", PORT), flush=True)
    while True:
        conn, _ = srv.accept()
        try:
            handle(conn)
        except OSError:
            pass
        finally:
            try:
                conn.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            conn.close()


if __name__ == "__main__":
    main()
