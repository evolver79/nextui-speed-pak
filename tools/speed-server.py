#!/usr/bin/env python3
"""A home server for Speed on the TrimUI Brick: run it on any PC on the same network, then put
server=http://<this PC's IP>:8080 in the SD card's .userdata/tg5040/Speed/speed.txt and pick "Home server" in Speed.

    python3 speed-server.py [port]

GET /down?bytes=N  sends N bytes (at most 1 GB); POST /up  takes whatever comes; GET /  answers "speed".
Only the Python standard library; nothing is stored.
"""
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

CHUNK = bytes(65536)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        u = urlparse(self.path)
        if u.path == "/down":
            n = min(int(parse_qs(u.query).get("bytes", ["100000000"])[0]), 1 << 30)
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(n))
            self.end_headers()
            try:
                while n > 0:
                    k = min(n, len(CHUNK)); self.wfile.write(CHUNK[:k]); n -= k
            except (BrokenPipeError, ConnectionResetError):
                pass
        else:
            self.reply(b"speed\n")

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        try:
            while n > 0:
                got = self.rfile.read(min(n, 65536))
                if not got:
                    break
                n -= len(got)
        except (ConnectionResetError, TimeoutError):
            return
        self.reply(b"ok\n")

    def reply(self, body):
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        pass


port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
print(f"Speed home server on port {port}: in speed.txt, server=http://<this PC's IP>:{port}")
ThreadingHTTPServer(("0.0.0.0", port), Handler).serve_forever()
