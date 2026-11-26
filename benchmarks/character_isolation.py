"""Capture the provider payload for a private character conversation."""

import argparse
import http.server
import json
import os
import tempfile
import threading
from contextlib import ExitStack
from pathlib import Path

from fixtures import build
from gateway_history import free_port, process, request, wait


def check(binary, output):
    requests = []

    class Provider(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            payload = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            requests.append(payload)
            wants_oracle = "REQUEST_ORACLE" in payload["messages"][-1]["content"]
            content = '{"neutrino_tool":"scene_catalog"}' if wants_oracle else "LANTERN QUARTZ SEVEN"
            packet = {"choices": [{"delta": {"content": content}, "finish_reason": "stop"}]}
            reply = f"data: {json.dumps(packet)}\n\ndata: [DONE]\n\n".encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Content-Length", str(len(reply)))
            self.end_headers()
            self.wfile.write(reply)

        def log_message(self, *_args):
            pass

    with tempfile.TemporaryDirectory(prefix="neutrino-character-") as scratch, ExitStack() as stack:
        provider = stack.enter_context(http.server.ThreadingHTTPServer(("127.0.0.1", 0), Provider))
        thread = threading.Thread(target=provider.serve_forever, daemon=True)
        thread.start()
        stack.callback(thread.join)
        stack.callback(provider.shutdown)
        folder = Path(scratch)
        build(folder, 1)
        workspace = folder / "workspace.json"
        data = json.loads(workspace.read_text(encoding="utf-8"))
        story = data["stories"][0]
        story["card"]["description"] = "PRIVATE_CARD_BRIEF"
        story["nodes"][0]["content"] = "PRIVATE_NARRATOR_HISTORY"
        story["nodes"][0]["memoryRecap"] = "PRIVATE_RECAP"
        story["authorNote"] = "PRIVATE_AUTHOR_NOTE"
        story["simEnabled"] = True
        workspace.write_text(json.dumps(data), encoding="utf-8")
        gateway_port, web_port = free_port(), free_port()
        gateway = f"http://127.0.0.1:{gateway_port}/v1"
        web = f"http://127.0.0.1:{web_port}/api"
        environment = {key: value for key, value in os.environ.items() if not key.startswith("NEUTRINO_")}
        environment.update(NEUTRINO_SERVER_BIND="127.0.0.1", NEUTRINO_WEB_BIND="127.0.0.1",
                           NEUTRINO_ENDPOINT=f"http://127.0.0.1:{provider.server_port}/v1",
                           NEUTRINO_MODEL="fixture")
        stack.enter_context(process(binary, ["--serve", "--port", str(gateway_port)],
                                    environment, folder / "gateway.log"))
        wait(lambda: request(gateway + "/status"))
        args = ["--web", "--port", str(web_port), "--workspace", str(workspace), "--server", gateway]
        stack.enter_context(process(binary, args, environment, folder / "web.log"))
        wait(lambda: request(web + "/state"))

        def submit(text):
            return request(web + "/action", {"action": "submit", "text": text})

        submit("/room Relay | PRIVATE_ROOM_DETAIL")
        submit("/npc Mara | Keep watch.")
        submit("/npc Tessa | Keep notes.")
        submit("/knowledge Mara | AUTHORIZED_SIGNAL is the next call.")
        submit("/knowledge Tessa | PRIVATE_TESSA_BELIEF")
        request_index = len(requests)
        submit("/talk Mara | Do you remember AUTHORIZED_CUE? Return the code only.")

        def load_story():
            return json.loads(workspace.read_text(encoding="utf-8"))["stories"][0]

        def saved():
            view = request(web + "/state")
            node = load_story()["nodes"][-1]
            return node if not view["busy"] and node.get("characterScope") == "Mara" else None

        reply = wait(saved)
        captured = requests[request_index:]
        assert len(captured) == 1, captured
        messages = captured[0]["messages"]
        payload = json.dumps(messages)
        assert "AUTHORIZED_SIGNAL" in payload and "AUTHORIZED_CUE" in payload, messages
        assert "PRIVATE_" not in payload and "neutrino_tool" not in payload, messages
        trace = submit("/trace")["panel"]["text"]
        assert "Character context uses only" in trace and "provider ended a short answer" in trace, trace
        assert reply["content"] == "LANTERN QUARTZ SEVEN", reply
        events = load_story().get("simLog")
        request_index = len(requests)
        submit("/talk Mara | REQUEST_ORACLE")

        def rejected():
            view = request(web + "/state")
            return not view["busy"] and "did not produce prose" in view["status"]

        wait(rejected)
        after_oracle = load_story()
        assert after_oracle["nodes"][-1]["role"] == "user" and after_oracle.get("simLog") == events
        assert len(requests) == request_index + 1, requests[request_index:]
        oracle_trace = submit("/trace")["panel"]["text"]
        assert "Narrator tools\n" not in oracle_trace, oracle_trace
        output.write_text(json.dumps({"messages": messages, "saved_reply": reply, "trace": trace,
                                     "oracle_rejected": True, "oracle_trace": oracle_trace}, indent=2) + "\n",
                          encoding="utf-8")
        print("Restricted character request passed through browser, gateway, and provider.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    check(args.binary.resolve(), args.out)
