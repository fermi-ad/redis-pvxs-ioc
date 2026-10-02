#!/usr/bin/env python3
"""Validate the publisher against a private HTTP endpoint, without Redis."""
import base64
import http.server
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        mode = self.path.split("/")[2]
        payload = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        with self.server.lock:
            self.server.seen.append((mode, self.headers.get("Authorization"), json.loads(payload)))
        if mode == "slow":
            time.sleep(1)
        if mode == "redirect":
            self.send_response(307)
            self.send_header("Location", f"http://127.0.0.1:{self.server.server_port}/case/target/resources/channels")
            self.end_headers()
            return
        body = b"x" * 4096 if mode == "large" else b"upstream-secret-must-not-be-printed"
        self.send_response(503 if mode == "error" else 200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass


def main():
    executable = sys.argv[1]
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    server.seen = []
    server.lock = threading.Lock()
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        with tempfile.TemporaryDirectory(prefix="redis-pvxs-http-") as directory:
            path = Path(directory) / "config.json"
            environment = dict(os.environ, NO_PROXY="127.0.0.1,localhost", no_proxy="127.0.0.1,localhost")
            for key in ("CHANNELFINDER_USERNAME", "CHANNELFINDER_PASSWORD"):
                environment.pop(key, None)

            def invoke(mode, *options, credentials=False, url=None):
                config = dict(server=dict(instance="http", namespace="TEST"),
                              redis=dict(host="192.0.2.1", port=1, base_key="http"),
                              pvs=[dict(name="value", type="float64", shape="scalar", read=dict(key="value"))],
                              channelfinder=dict(url=url or f"http://127.0.0.1:{server.server_port}/case/{mode}/resources/channels"))
                path.write_text(json.dumps(config))
                env = environment.copy()
                if credentials:
                    env.update(CHANNELFINDER_USERNAME="test-user", CHANNELFINDER_PASSWORD="test-secret")
                return subprocess.run([executable, "--config", str(path), *options], env=env,
                                      text=True, capture_output=True, timeout=4)

            result = invoke("success", credentials=True)
            assert result.returncode == 0, result.stderr
            assert "published 1 channels" in result.stdout
            assert server.seen[-1][1] == "Basic " + base64.b64encode(b"test-user:test-secret").decode()

            result = invoke("error")
            assert result.returncode != 0 and "HTTP 503" in result.stderr
            assert "upstream-secret" not in result.stderr

            started = time.monotonic()
            result = invoke("slow", "--timeout-ms", "100", "--connect-timeout-ms", "50")
            assert result.returncode != 0 and "tim" in result.stderr.splitlines()[0].lower(), result.stderr
            assert time.monotonic() - started < 2

            result = invoke("large", "--max-response-bytes", "64")
            assert result.returncode != 0 and "exceeded 64 bytes" in result.stderr, result.stderr
            result = invoke("large", "--max-response-bytes", "8192")
            assert result.returncode == 0, result.stderr

            result = invoke("redirect", credentials=True)
            assert result.returncode != 0 and "HTTP 307" in result.stderr, result.stderr
            assert not any(request[0] == "target" for request in server.seen)
            before = len(server.seen)
            result = invoke("redirect", "--allow-redirects", credentials=True)
            assert result.returncode != 0 and "redirects with credentials" in result.stderr
            assert len(server.seen) == before
            result = invoke("redirect", "--allow-redirects")
            assert result.returncode != 0, result.stdout  # redirects must remain HTTPS-only
            assert not any(request[0] == "target" for request in server.seen)

            for flag, bad in (("--timeout-ms", "0"), ("--timeout-ms", "300001"),
                              ("--connect-timeout-ms", "-1"), ("--max-response-bytes", "64x"),
                              ("--max-response-bytes", "9999999999999999999999")):
                assert invoke("success", flag, bad).returncode != 0
            result = invoke("success", url="file:///etc/passwd")
            assert result.returncode != 0 and "Unsupported protocol" in result.stderr, result.stderr
            result = invoke("success", url=f"http://test-user:test-secret@127.0.0.1:{server.server_port}/resources/channels")
            assert result.returncode != 0 and "URL credentials" in result.stderr
            assert "test-secret" not in result.stderr
            before = len(server.seen)
            result = invoke("success", "--dry-run")
            assert result.returncode == 0 and json.loads(result.stdout)
            assert len(server.seen) == before
        print("ChannelFinder HTTP deadlines, response bounds, redirects, credentials and offline dry-run passed")
    finally:
        server.shutdown()
        server.server_close()
        worker.join(timeout=3)


if __name__ == "__main__":
    main()
