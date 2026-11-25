"""Exercise long history through a real web process, gateway, and loopback model stub."""
import argparse
import http.server
import json
import os
import socket
import subprocess
import tempfile
import threading
import time
import urllib.error
import urllib.request
from contextlib import contextmanager
from pathlib import Path

from fixtures import PROMPT, build

REPLY = "The emergency locker is ready for inspection."


def free_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


@contextmanager
def process(binary, arguments, environment, logfile):
    with logfile.open("wb") as log:
        flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        with subprocess.Popen([str(binary), *arguments], env=environment, stdout=log,
                              stderr=subprocess.STDOUT, creationflags=flags) as child:
            try:
                yield child
            finally:
                child.terminate()
                try:
                    child.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    child.kill()


def request(url, data=None):
    headers = {"X-Neutrino-Web": "1", "Content-Type": "application/json"}
    payload = json.dumps(data).encode() if data is not None else None
    with urllib.request.urlopen(urllib.request.Request(url, payload, headers), timeout=2) as result:
        return json.load(result)


def wait(check, seconds=12):
    deadline = time.monotonic() + seconds
    error = None
    while time.monotonic() < deadline:
        try:
            result = check()
            if result:
                return result
        except (OSError, ValueError, urllib.error.URLError) as exc:
            error = exc
        time.sleep(.05)
    raise AssertionError(f"Condition timed out. Last connection error: {error}")


def branch_check(web, story_state, received):
    def submit(text):
        return request(web + "/action", {"action": "submit", "text": text})

    def panel(command):
        return submit(command)["panel"]["text"]

    def snapshot():
        return {"world": panel("/world"), "stats": panel("/stats")}

    def send(text, base):
        submit(text)

        def finished():
            request(web + "/state")
            story = story_state()
            return story["head"] if len(story["nodes"]) >= base + 2 and story["nodes"][-1]["role"] == "assistant" else None

        return wait(finished)

    original = story_state()["head"]
    submit("/room Dock | The relay dock.")
    submit("/room Loft | The upstairs relay loft.")
    submit("/connect Dock | Loft")
    submit("/item Copper Key | A key left beside the dock.")
    submit("/time set 1 08:00")
    baseline = snapshot()
    first = send("Record the fictional branch marker BLUE HARBOR WINDOW.", original)
    submit("/go Loft")
    failure = submit("/take Copper Key")["status"]
    assert "unavailable" in failure, failure
    submit("/go Dock")
    submit("/take Copper Key")
    submit("/go Loft")
    submit("/time set 2 14:15")
    submit('/sim {"neutrino_tool":"bond","actor":"Iona","other":"Mara","value":-35}')
    submit('/sim {"neutrino_tool":"injury","actor":"Mara","part":"hand","kind":"bruise","severity":"mild"}')
    submit('/sim {"neutrino_tool":"tune","setting":"gravity","value":1}')
    changed = snapshot()
    assert "Loft" in changed["world"] and "Copper Key" in changed["world"], changed
    assert all(field in changed["stats"] for field in ("14:15", "-35", "bruise", "gravity 1 m/s")), changed
    submit(f"/branch {original}")
    restored = snapshot()
    assert baseline == restored, (baseline, restored)
    second = send("Record the fictional sibling marker RIVER AMBER TERN.", first)
    serialized = json.dumps(received[-1]["messages"])
    assert "RIVER AMBER TERN" in serialized and "BLUE HARBOR WINDOW" not in serialized
    submit(f"/branch {first}")
    assert snapshot() == changed
    return {"original_head": original, "alternate_head": first, "sibling_head": second,
            "prerequisite_rejection": failure, "initial_state": baseline,
            "alternate_state": changed, "restored_state": restored, "sibling_prompt_excluded_alternate_marker": True}


def run(binary, turns, fault, branches=None):
    received = []

    class Stub(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            message = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            received.append(message)
            if fault and len(received) == 1:
                self.send_response(fault)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            reply_text = REPLY
            if branches and len(received) == 2:
                reply_text = "The keeper establishes BLUE HARBOR WINDOW as the west marker."
            elif branches and len(received) == 3:
                reply_text = "The keeper logs RIVER AMBER TERN as the eastern marker."
            chunks = [
                {"choices": [{"delta": {"content": reply_text}}]},
                {"choices": [{"delta": {}, "finish_reason": "stop"}]},
            ]
            reply = "".join("data: " + json.dumps(chunk) + "\n\n" for chunk in chunks)
            reply = (reply + "data: [DONE]\n\n").encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Content-Length", str(len(reply)))
            self.end_headers()
            self.wfile.write(reply)

        def log_message(self, _format, *args):
            pass

    with tempfile.TemporaryDirectory(prefix="neutrino-regression-") as scratch, \
            http.server.ThreadingHTTPServer(("127.0.0.1", 0), Stub) as stub:
        thread = threading.Thread(target=stub.serve_forever, daemon=True)
        thread.start()
        try:
            folder = Path(scratch)
            build(folder, turns)
            state = folder / "workspace.json"
            gateway_port, web_port = free_port(), free_port()
            gateway = f"http://127.0.0.1:{gateway_port}/v1"
            web = f"http://127.0.0.1:{web_port}/api"
            environment = {key: value for key, value in os.environ.items() if not key.startswith("NEUTRINO_")}
            environment.update(NEUTRINO_ENDPOINT=f"http://127.0.0.1:{stub.server_port}/v1",
                               NEUTRINO_MODEL="fixture", NEUTRINO_AUTO_RESEARCH="0")
            with process(binary, ["--serve", "--port", str(gateway_port)], environment, folder / "gateway.log"):
                wait(lambda: request(gateway + "/status"))
                args = ["--web", "--port", str(web_port), "--workspace", str(state), "--server", gateway]
                with process(binary, args, environment, folder / "web.log"):
                    wait(lambda: request(web + "/state"))
                    expected_head = 2 * turns + 3
                    text = PROMPT

                    def story_state():
                        return json.loads(state.read_text(encoding="utf-8"))["stories"][0]

                    if fault:
                        request(web + "/action", {"action": "submit", "text": PROMPT})

                        def failed():
                            view = request(web + "/state")
                            return received and not view["busy"] and "error" in view["status"].lower()

                        wait(failed)
                        failed_story = story_state()
                        assert failed_story["head"] <= 2 * turns + 2, failed_story["head"]
                        expected_head = failed_story["head"] + 2
                        text = "Is the emergency locker ready?"

                    request(web + "/action", {"action": "submit", "text": text})

                    def saved():
                        # The state endpoint also evaluates background jobs.
                        request(web + "/state")
                        story = story_state()
                        return story["nodes"][-1] if story["head"] == expected_head else None

                    try:
                        last = wait(saved)
                    except AssertionError as exc:
                        raise AssertionError(f"Reply not saved. Upstream requests: {len(received)}. "
                                             f"Gateway: {(folder / 'gateway.log').read_text()}. "
                                             f"Web: {(folder / 'web.log').read_text()}.") from exc
                    assert last["role"] == "assistant" and last["content"] == REPLY, last
                    assert len(received) == (2 if fault else 1), len(received)
                    prompt = received[-1]["messages"]
                    assert prompt[0]["role"] == "system", prompt[0]
                    assert prompt[1]["role"] == "user", prompt[1]
                    assert 3 <= len(prompt) <= 512, len(prompt)
                    assert prompt[-1] == {"role": "user", "content": text}, prompt[-1]
                    prefix = f"Recovered from HTTP {fault}. " if fault else ""
                    print(f"{prefix}Saved reply through the gateway with {turns:,} turns and {len(prompt)} prompt messages.")
                    if branches:
                        evidence = branch_check(web, story_state, received)
                if branches:
                    with process(binary, args, environment, folder / "web-reload.log"):
                        wait(lambda: request(web + "/state"))

                        def read_panel(command):
                            response = request(web + "/action", {"action": "submit", "text": command})
                            return response["panel"]["text"]

                        after_reload = {"world": read_panel("/world"), "stats": read_panel("/stats")}
                        assert after_reload == evidence["alternate_state"], after_reload
                        evidence["reloaded_state"] = after_reload
                        branches.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
                        print("Verified branch isolation, reload, and the unavailable-item prerequisite.")
        finally:
            stub.shutdown()
            thread.join()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("--turns", type=int, default=500)
    parser.add_argument("--fault", type=int, choices=(429, 500))
    parser.add_argument("--branches", type=Path)
    args = parser.parse_args()
    if args.branches and args.fault:
        parser.error("Run branch and provider-fault checks separately.")
    run(args.binary.resolve(strict=True), args.turns, args.fault, args.branches)
