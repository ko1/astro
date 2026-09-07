#!/usr/bin/env python3
"""Static server with the two headers SharedArrayBuffer needs.

    python3 serve.py [port]     # default 8000, serves this directory

Atomics.wait is how the guest blocks for a frame tick, so the page needs a
SharedArrayBuffer, and a SharedArrayBuffer needs the document to be
cross-origin isolated.  python3 -m http.server does not set those headers.
"""
import functools
import http.server
import sys

class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map,
                      '.wasm': 'application/wasm',
                      '.js': 'text/javascript'}

    def end_headers(self):
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        self.send_header('Cross-Origin-Resource-Policy', 'same-origin')
        self.send_header('Cache-Control', 'no-store')
        super().end_headers()

if __name__ == '__main__':
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
    http.server.ThreadingHTTPServer.allow_reuse_address = True
    with http.server.ThreadingHTTPServer(('127.0.0.1', port), Handler) as httpd:
        print(f'http://127.0.0.1:{port}/  (cross-origin isolated)')
        httpd.serve_forever()
