#!/usr/bin/env python3
"""Static file server for the wasm iPod emulator.

The emscripten build uses -sPROXY_TO_PTHREAD, which needs
SharedArrayBuffer, which browsers only enable for cross-origin-isolated
pages: every response must carry COOP/COEP headers.  Plain
`python3 -m http.server` will NOT work.

Usage: serve.py [port] [directory]
"""
import http.server
import functools
import sys


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        '.js': 'text/javascript',
        '.mjs': 'text/javascript',
        '.wasm': 'application/wasm',
    }

    def end_headers(self):
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        self.send_header('Cache-Control', 'no-store')
        super().end_headers()


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    directory = sys.argv[2] if len(sys.argv) > 2 else '.'
    handler = functools.partial(Handler, directory=directory)
    with http.server.ThreadingHTTPServer(('127.0.0.1', port), handler) as srv:
        print(f'serving {directory} on http://127.0.0.1:{port}/ '
              '(with COOP/COEP)')
        srv.serve_forever()


if __name__ == '__main__':
    main()
