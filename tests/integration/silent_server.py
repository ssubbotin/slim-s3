#!/usr/bin/env python3
"""Accepts TCP connections and never responds - a black-hole S3 endpoint."""
import socket
import sys

port = int(sys.argv[1]) if len(sys.argv) > 1 else 9999
srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", port))
srv.listen(16)
print(f"silent server on 127.0.0.1:{port}", flush=True)
conns = []
while True:
    c, _ = srv.accept()
    conns.append(c)  # keep the socket open, read nothing, send nothing
