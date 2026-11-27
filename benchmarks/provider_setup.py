"""Check gateway provider flags against a loopback OpenAI-compatible stub."""
import argparse
import http.server
import json
import os
import subprocess
import tempfile
import threading
import urllib.request
from pathlib import Path

from gateway_history import free_port, process, request, wait


def check(binary):
    received = []

    class Provider(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            payload = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            received.append((self.path, payload["model"], self.headers.get("Authorization")))
            chunk = json.dumps({"choices": [{"delta": {"content": "Ready."}, "finish_reason": "stop"}]})
            data = f"data: {chunk}\n\ndata: [DONE]\n\n".encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def log_message(self, _format, *_args):
            pass

    with tempfile.TemporaryDirectory(prefix="neutrino-provider-") as scratch, \
            http.server.ThreadingHTTPServer(("127.0.0.1", 0), Provider) as upstream:
        thread = threading.Thread(target=upstream.serve_forever, daemon=True)
        thread.start()
        base = {key: value for key, value in os.environ.items()
                if not key.startswith("NEUTRINO_") and key != "OPENAI_API_KEY"}
        endpoint = f"http://127.0.0.1:{upstream.server_port}/v1"
        try:
            for name, flags, settings, expected_key in (
                ("flag with named key", ["--provider", endpoint, "--model", "local/fixture",
                                        "--api-key-env", "FIXTURE_PROVIDER_KEY"],
                 {"NEUTRINO_ENDPOINT": "http://127.0.0.1:1/v1", "NEUTRINO_MODEL": "stale",
                  "NEUTRINO_API_KEY": "stale-key", "FIXTURE_PROVIDER_KEY": "synthetic-named-key"},
                 "Bearer synthetic-named-key"),
                ("flag without key", ["--provider", endpoint, "--model", "local/fixture"],
                 {"NEUTRINO_API_KEY": "stale-key"}, None),
                ("environment-only startup", [],
                 {"NEUTRINO_ENDPOINT": endpoint, "NEUTRINO_MODEL": "local/fixture",
                  "NEUTRINO_API_KEY": "synthetic-env-key"}, "Bearer synthetic-env-key"),
            ):
                port = free_port()
                gateway = f"http://127.0.0.1:{port}/v1"
                with process(binary, ["--serve", "--port", str(port), *flags],
                             base | settings, Path(scratch) / "gateway.log"):
                    status = wait(lambda: request(gateway + "/status"))
                    assert status["model"] == "local/fixture", status
                    body = {"model": "narrator", "messages": [{"role": "user", "content": "Ready?"}]}
                    req = urllib.request.Request(gateway + "/chat/completions", json.dumps(body).encode(),
                                                 {"Content-Type": "application/json"})
                    with urllib.request.urlopen(req, timeout=5) as response:
                        assert b"Ready." in response.read()
                    assert received[-1] == ("/v1/chat/completions", "local/fixture", expected_key)
                    print(f"{name} verified")
            for flags, expected in (
                (["--provider", "openai"], b"API key environment variable"),
                (["--provider", "https://user:pass@example.invalid/v1"], b"HTTP(S) API URL"),
                (["--provider", endpoint, "--api-key-env", "MISSING_FIXTURE_KEY"], b"API key environment variable"),
            ):
                failed = subprocess.run([binary, "--serve", "--model", "local/fixture", *flags],
                                        env=base, capture_output=True, timeout=5)
                assert failed.returncode == 2 and expected in failed.stderr
                assert b"user:pass" not in failed.stderr
            print("Missing keys and credential-bearing URLs rejected at startup")
        finally:
            upstream.shutdown()
            thread.join()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    args = parser.parse_args()
    check(args.binary.resolve())
