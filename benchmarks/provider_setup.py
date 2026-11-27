"""Check gateway provider flags against a loopback OpenAI-compatible stub."""
import argparse
import http.server
import json
import os
import subprocess
import tempfile
import threading
import urllib.request
from urllib.error import HTTPError
from pathlib import Path

from fixtures import build
from gateway_history import free_port, process, request, wait


def check(binary):
    received = []

    class Provider(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            payload = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            received.append((self.path, payload["model"], self.headers.get("Authorization"), payload.get("stream_options")))
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
                    assert received[-1] == ("/v1/chat/completions", "local/fixture", expected_key, None)
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
            live_switch(binary,Path(scratch),base,endpoint,received)
        finally:
            upstream.shutdown()
            thread.join()


def live_switch(binary, folder, base, endpoint, received):
    admin = "synthetic-provider-admin-token"
    ordinary = "synthetic-inference-token"
    settings = base | {"NEUTRINO_PROVIDER_ADMIN_TOKEN": admin, "NEUTRINO_SERVER_TOKEN": ordinary,
                       "NEUTRINO_PROVIDER_KEY_FIXTURE": "synthetic-runtime-key",
                       "NEUTRINO_PROVIDERS_FILE": str(folder / "providers.json")}
    gateway_port, web_port = free_port(), free_port()
    gateway = f"http://127.0.0.1:{gateway_port}/v1"
    web = f"http://127.0.0.1:{web_port}/api"
    build(folder,1)
    workspace = folder / "workspace.json"
    data = json.loads(workspace.read_text(encoding="utf-8"))
    data["serverNarrator"] = "old-model"
    workspace.write_text(json.dumps(data),encoding="utf-8")
    with process(binary,["--serve","--port",str(gateway_port)],settings,folder/"runtime-gateway.log"), \
         process(binary,["--web","--port",str(web_port),"--server",gateway,"--workspace",str(workspace)],
                 settings,folder/"runtime-web.log"):
        wait(lambda: request(web+"/state"))

        def submit(text):
            return request(web+"/action",{"action":"submit","text":text})

        def finish(text, status):
            submit(text)
            return wait(lambda: state if status in (state := request(web+"/state"))["status"] else None)

        def stream():
            req = urllib.request.Request(gateway+"/chat/completions",
                                         b'{"model":"narrator","messages":[{"role":"user","content":"Ready?"}]}',
                                         {"Content-Type":"application/json","Authorization":"Bearer "+ordinary})
            with urllib.request.urlopen(req,timeout=5) as response:
                assert b"Ready." in response.read()

        assert "Unconfigured" in finish("/model api","Gateway API settings ready")["panel"]["text"]
        req = urllib.request.Request(gateway+"/provider",b"{}",{"Authorization":"Bearer "+ordinary})
        try:
            urllib.request.urlopen(req,timeout=3).close()
        except HTTPError as error:
            assert error.code == 401
        else:
            raise AssertionError("Inference token was allowed to manage providers")

        finish(f"/model provider {endpoint} | local/first | NEUTRINO_PROVIDER_KEY_FIXTURE",
               "Gateway provider switched")
        stream()
        assert received[-1] == ("/v1/chat/completions","local/first","Bearer synthetic-runtime-key",None)
        assert json.loads(workspace.read_text(encoding="utf-8"))["serverNarrator"] == ""

        finish(f"/model provider {endpoint} | rejected | NEUTRINO_SERVER_TOKEN","gateway key variable")
        assert "local/first" in finish("/model api","Gateway API settings ready")["panel"]["text"]

        def show_menu():
            submit("/model")
            return wait(lambda: menu if (menu := request(web+"/state").get("modelMenu")) and not menu["busy"] else None)

        def apply_menu(provider, url="", model="", usage=False, name=""):
            request(web+"/action",{"action":"model-apply","provider":provider,"endpoint":url,
                                   "model":model,"key":"","usage":usage,"name":name})
            wait(lambda: "modelMenu" not in request(web+"/state"))

        menu = show_menu()
        assert menu["endpoint"] == endpoint and menu["model"] == "local/first" and menu["authenticated"]
        assert "synthetic-runtime-key" not in json.dumps(menu)
        apply_menu(0,"","local/current",False,"Private relay")
        stream()
        assert received[-1] == ("/v1/chat/completions","local/current","Bearer synthetic-runtime-key",None)

        show_menu()
        apply_menu(4,endpoint+"/second","local/second",True,"Public relay")
        stream()
        assert received[-1] == ("/v1/second/chat/completions","local/second",None,{"include_usage": True})
        panel = finish("/model api","Gateway API settings ready")["panel"]["text"]
        assert "local/second" in panel and "Stream usage  On" in panel and "synthetic-runtime-key" not in panel
        finish("/model api usage off","Gateway API settings ready")
        stream()
        assert received[-1] == ("/v1/second/chat/completions","local/second",None,None)
        menu = show_menu()
        assert menu["profiles"] == ["Private relay","Public relay"]
        apply_menu(5)
        stream()
        assert received[-1] == ("/v1/chat/completions","local/current","Bearer synthetic-runtime-key",None)
        saved = (folder/"providers.json").read_text(encoding="utf-8")
        assert "synthetic-runtime-key" not in saved
        profiles = json.loads(saved)
        assert profiles["active"] == "Private relay"
        assert profiles["profiles"]["Private relay"]["key_env"] == "NEUTRINO_PROVIDER_KEY_FIXTURE"

    gateway_port, web_port = free_port(), free_port()
    gateway = f"http://127.0.0.1:{gateway_port}/v1"
    web = f"http://127.0.0.1:{web_port}/api"
    settings["NEUTRINO_PROVIDER_KEY_FIXTURE"] = "synthetic-rotated-key"
    with process(binary,["--serve","--port",str(gateway_port)],settings,folder/"restart-gateway.log"), \
         process(binary,["--web","--port",str(web_port),"--server",gateway,"--workspace",str(workspace)],
                 settings,folder/"restart-web.log"):
        wait(lambda: request(web+"/state"))
        stream()
        assert received[-1] == ("/v1/chat/completions","local/current","Bearer synthetic-rotated-key",None)
        menu = show_menu()
        assert menu["active_profile"] == "Private relay" and menu["profiles"] == ["Private relay","Public relay"]
        assert "synthetic-rotated-key" not in json.dumps(menu)
        apply_menu(6)
        stream()
        assert received[-1] == ("/v1/second/chat/completions","local/second",None,{"include_usage": True})

        def edit(profile, **changes):
            menu = show_menu()
            details = menu["profile_details"][profile]
            fields = {"profile": profile, "name": profile, "endpoint": details["endpoint"], "model": details["model"],
                      "usage": details["stream_usage"], "key": ""}
            request(web+"/action",{"action":"model-profile-edit",**(fields | changes)})

        def menu_closed():
            return "modelMenu" not in request(web+"/state")

        edit("Private relay",name="Public relay")
        rejected = wait(lambda: menu if (menu := request(web+"/state").get("modelMenu", {})).get("error") else None)
        assert "already uses" in rejected["error"]
        assert "key_env" not in json.dumps(rejected) and "synthetic-rotated-key" not in json.dumps(rejected)
        edit("Private relay",name="Renamed relay",model="local/renamed",usage=True)
        wait(menu_closed)
        stream()
        assert received[-1] == ("/v1/second/chat/completions","local/second",None,{"include_usage": True})
        menu = show_menu()
        apply_menu(5+menu["profiles"].index("Renamed relay"))
        edit("Renamed relay",model="local/edited",usage=False)
        wait(menu_closed)
        stream()
        assert received[-1] == ("/v1/chat/completions","local/edited","Bearer synthetic-rotated-key",None)
        show_menu()
        request(web+"/action",{"action":"model-profile-delete","profile":"Public relay"})
        wait(menu_closed)

    gateway_port, web_port = free_port(), free_port()
    gateway = f"http://127.0.0.1:{gateway_port}/v1"
    web = f"http://127.0.0.1:{web_port}/api"
    with process(binary,["--serve","--port",str(gateway_port)],settings,folder/"edited-gateway.log"), \
         process(binary,["--web","--port",str(web_port),"--server",gateway,"--workspace",str(workspace)],
                 settings,folder/"edited-web.log"):
        wait(lambda: request(web+"/state"))
        menu = show_menu()
        assert menu["profiles"] == ["Renamed relay"] and menu["active_profile"] == "Renamed relay"
        assert menu["profile_details"]["Renamed relay"]["model"] == "local/edited"
        stream()
        assert received[-1] == ("/v1/chat/completions","local/edited","Bearer synthetic-rotated-key",None)
        request(web+"/action",{"action":"model-profile-delete","profile":"Renamed relay"})
        wait(menu_closed)
        stream()
        assert received[-1] == ("/v1/chat/completions","local/edited","Bearer synthetic-rotated-key",None)
        assert json.loads((folder/"providers.json").read_text(encoding="utf-8")) == {"profiles": {}, "active": ""}
        print("Provider profile edits, rename, deletion, restart restore, and gateway-only keys verified")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    args = parser.parse_args()
    check(args.binary.resolve())
