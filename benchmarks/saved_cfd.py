"""Advance a saved fog field through a live narrator and reopen its workspace."""

import argparse
import json
import os
import tempfile
import time
from pathlib import Path

from fixtures import build
from gateway_history import free_port, process, request, wait


SETUP = [
    {"neutrino_tool": "scene_room", "min": [0, 0, 0], "max": [6, 6, 6]},
    {"neutrino_tool": "scene_medium", "method": "cfd", "resolution": 12, "seconds": 0.5,
     "wind": [1, 0, 0], "center": [2.75, 2.75, 2.75], "diffusivity": 0, "viscosity": 0,
     "light_loss": 8, "sound_loss": 5},
    {"neutrino_tool": "scene_flow_step", "seconds": 0.25},
]
PROBE = {"neutrino_tool": "sim_plan", "name": "fog probe", "mode": "hypothetical",
         "actions": [{"neutrino_tool": "scene_flow", "probe": [3.75, 2.75, 2.75]},
                     {"neutrino_tool": "scene_light", "source": [3.75, 2.75, 2.75],
                      "listener": [4.75, 2.75, 2.75], "max_bounces": 0}],
         "queries": [{"id": "time", "predicate": "flow.time_exceeds", "step": 1, "threshold": 0},
                     {"id": "tracer", "predicate": "fluid.concentration_exceeds", "step": 1, "threshold": 0},
                     {"id": "light", "predicate": "signal.power_exceeds", "step": 2, "threshold": 0}]}
CUE = ("A patch of fog is drifting through the watch room. Before narrating, advance the already saved CFD "
       "field another 0.25 seconds using scene_flow_step. Then write a short in-world reply.")


def run(binary, provider, model, output):
    with tempfile.TemporaryDirectory(prefix="neutrino-saved-cfd-") as scratch:
        folder = Path(scratch)
        build(folder, 1)
        workspace = folder / "workspace.json"
        data = json.loads(workspace.read_text(encoding="utf-8"))
        data["stories"][0]["nodes"] = [{"id": 1, "parent": 0, "role": "assistant",
                                       "content": "The keeper watches as fog drifts through the relay."}]
        data["stories"][0]["head"] = 1
        workspace.write_text(json.dumps(data), encoding="utf-8")
        gateway_port, web_port = free_port(), free_port()
        gateway = f"http://127.0.0.1:{gateway_port}/v1"
        web = f"http://127.0.0.1:{web_port}/api"
        env = {key: value for key, value in os.environ.items() if not key.startswith("NEUTRINO_")}
        env.update(NEUTRINO_SERVER_BIND="127.0.0.1", NEUTRINO_WEB_BIND="127.0.0.1", NEUTRINO_AUTO_RESEARCH="0",
                   NEUTRINO_STREAM_USAGE="1", NEUTRINO_ENDPOINT=provider, NEUTRINO_MODEL=model)
        args = ["--web", "--port", str(web_port), "--workspace", str(workspace), "--server", gateway]

        def submit(text):
            return request(web + "/action", {"action": "submit", "text": text})

        def panel(text):
            return submit(text)["panel"]["text"]

        def probe():
            before = workspace.read_bytes()
            text = panel("/sim " + json.dumps(PROBE))
            report = next(json.loads(line) for line in text.splitlines() if line.startswith('{"simulation":'))
            assert report["status"] == "completed" and workspace.read_bytes() == before, report
            return {entry["id"]: entry["value"] for entry in report["consequences"]}

        def story():
            return json.loads(workspace.read_text(encoding="utf-8"))["stories"][0]

        with process(binary, ["--serve", "--port", str(gateway_port)], env, folder / "gateway.log"):
            wait(lambda: request(gateway + "/status"))
            with process(binary, args, env, folder / "web.log"):
                wait(lambda: request(web + "/state"))
                setup = panel("/sim " + json.dumps({"actions": SETUP}))
                before = probe()
                assert before["time"] == .25, setup
                start = time.perf_counter()
                submit(CUE)
                wait(lambda: not request(web + "/state")["busy"], seconds=90)
                elapsed_ms = (time.perf_counter() - start) * 1000
                reply = story()["nodes"][-1]
                trace = panel("/trace")
                evidence = {"model": model, "before": before, "reply": reply, "trace": trace, "elapsed_ms": elapsed_ms}
                output.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
                assert reply["role"] == "assistant" and "Advanced saved CFD field from 0.25 to 0.5 s" in trace, trace
                assert '"data"' not in trace, "Encoded CFD data reached the prompt"
                after = probe()
                assert after["time"] == .5 and after["tracer"] > before["tracer"] and after["light"] < before["light"]
                submit("/branch 1")
                assert probe() == before
            with process(binary, args, env, folder / "reopened.log"):
                wait(lambda: request(web + "/state"))
                assert probe() == before
                submit(f"/branch {reply['id']}")
                assert probe() == after
            evidence.update(after=after, branch_rewind=True, workspace_reopened=True)
            output.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
            print(f"Live CFD step saved and reopened: {before} -> {after}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--provider", required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    run(args.binary.resolve(strict=True), args.provider, args.model, args.out)
