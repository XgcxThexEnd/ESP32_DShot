#!/usr/bin/env python3
"""Local TLS endpoints for the ESP-IDF OTA transport bench test."""
import argparse
import ssl
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        try:
            if self.path == "/ok":
                self.send_response(200)
                self.send_header("Content-Length", "2")
                self.end_headers()
                self.wfile.write(b"OK")
            elif self.path == "/redirect":
                self.send_response(302)
                self.send_header("Location", "/ok")
                self.send_header("Content-Length", "0")
                self.end_headers()
            elif self.path == "/slow-headers":
                # Each fragment makes progress, but total time exceeds budget.
                self.wfile.write(b"HTTP/1.1 200 OK\r\n")
                for _ in range(16):
                    chunk = b"X-Slow: 1\r\n"
                    self.wfile.write(chunk)
                    self.wfile.flush()
                    time.sleep(0.4)
            elif self.path == "/slow-body":
                self.send_response(200)
                self.send_header("Content-Length", "100")
                self.end_headers()
                for _ in range(100):
                    self.wfile.write(b"x")
                    self.wfile.flush()
                    time.sleep(0.4)
            elif self.path == "/truncated":
                self.send_response(200)
                self.send_header("Content-Length", "100")
                self.end_headers()
                self.wfile.write(b"short")
            else:
                self.send_error(404)
        except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
            pass  # Deadline tests intentionally close a live TLS connection.
        finally:
            self.close_connection = True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cert", required=True)
    parser.add_argument("--key", required=True)
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8443)
    args = parser.parse_args()
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(args.cert, args.key)
    with ThreadingHTTPServer((args.bind, args.port), Handler) as server:
        server.socket = context.wrap_socket(server.socket, server_side=True)
        print(f"HTTPS fixture listening on {args.bind}:{args.port}", flush=True)
        server.serve_forever()


if __name__ == "__main__":
    main()
