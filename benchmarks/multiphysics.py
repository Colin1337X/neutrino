"""Measure loopback simulation plans and idle web RSS. Requires psutil for RSS."""

import argparse
import json
import math
import os
import re
import tempfile
import time
from contextlib import ExitStack
from pathlib import Path
from statistics import median

import psutil

from fixtures import build
from gateway_history import free_port, process, request, wait


def matches_report(report, example):
    if not report or report["status"] != "completed" or report["pending_commit"]:
        return False
    entries = {item["id"]: item for item in report["consequences"]}
    return ({key: item["result"] for key, item in entries.items()} == example["results"] and
            all(math.isclose(entries.get(key, {}).get(field, math.nan), expected, abs_tol=1e-9)
                for key, fields in example.get("measurements", {}).items()
                for field, expected in fields.items()))


def run(binary, examples, repetitions, output, provider, model, live_example=None):
    with tempfile.TemporaryDirectory(prefix="neutrino-multiphysics-") as scratch, ExitStack() as stack:
        folder = Path(scratch)
        build(folder, 1)
        workspace = folder / "workspace.json"
        data = json.loads(workspace.read_text(encoding="utf-8"))
        data["stories"][0]["simEnabled"] = False
        if provider and live_example:
            story = data["stories"][0]
            story["card"]["description"] = "A coastal relay keeper checking a fixture holding a ceramic jug over a stone floor."
            story["nodes"][0]["content"] = "A ceramic jug hangs above the stone floor. Alice and Bob stand nearby."
            story["nodes"][1]["content"] = "I ask what would happen if the fixture lets go."
            story["nodes"][2]["content"] = "The keeper peers at the jug and considers the question."
        elif provider:
            story = data["stories"][0]
            story["card"]["description"] = "A keeper maintaining the coastal relay's water tank."
            story["nodes"][0]["content"] = "The keeper stands beside the tank and looks at its outlet."
            story["nodes"][1]["content"] = "I ask the keeper to wait before opening the drain."
            story["nodes"][2]["content"] = "The keeper rests a hand on the closed valve and waits."
        workspace.write_text(json.dumps(data), encoding="utf-8")
        web_port = free_port()
        web = f"http://127.0.0.1:{web_port}/api"
        environment = {key: value for key, value in os.environ.items() if not key.startswith("NEUTRINO_")}
        environment.update(NEUTRINO_AUTO_RESEARCH="0", NEUTRINO_SERVER_BIND="127.0.0.1",
                           NEUTRINO_WEB_BIND="127.0.0.1")
        server = "http://127.0.0.1:1/v1"
        if provider:
            port = free_port()
            server = f"http://127.0.0.1:{port}/v1"
            environment.update(NEUTRINO_ENDPOINT=provider, NEUTRINO_MODEL=model)
            stack.enter_context(process(binary, ["--serve", "--port", str(port)],
                                        environment, folder / "gateway.log"))
            wait(lambda: request(server + "/status"))
        args = ["--web", "--port", str(web_port), "--workspace", str(workspace), "--server", server]
        child = stack.enter_context(process(binary, args, environment, folder / "web.log"))
        wait(lambda: request(web + "/state"))
        sampler = psutil.Process(child.pid)
        rss = []
        for _ in range(15):
            rss.append(sampler.memory_info().rss)
            time.sleep(.05)
        result = {"binary": str(binary), "bytes": binary.stat().st_size, "idle_rss_bytes": rss, "plans": []}

        def submit(text):
            return request(web + "/action", {"action": "submit", "text": text})

        for example in examples:
            plan = example["plan"]
            timings = []
            original = workspace.read_bytes()
            for iteration in range(repetitions + 1):
                start = time.perf_counter()
                response = submit("/sim " + json.dumps(plan, separators=(",", ":")))
                elapsed = (time.perf_counter() - start) * 1000
                panel = response["panel"]["text"]
                report = next(json.loads(line) for line in reversed(panel.splitlines())
                              if line.startswith('{"simulation":'))
                assert matches_report(report, example), panel
                assert workspace.read_bytes() == original
                if iteration:
                    timings.append(elapsed)
            result["plans"].append({"name": plan["name"], "times_ms": timings, "report": report,
                                    "median_ms": median(timings), "trace": panel})

        if provider:
            submit("/stats on")
            initial = len(json.loads(workspace.read_text(encoding="utf-8"))["stories"][0]["nodes"])
            cue = ("Before you narrate, run a hypothetical sim_plan named drain test. Use exactly one fluid "
                   "action with height 1, tank_area 2, outlet_area 0.003, and seconds 10. Evaluate "
                   "fluid.volume_exceeds for step 1 with threshold 80 litres and id spills. "
                   "Then describe the attendant's reaction to the predicted water discharge in one complete in-world sentence.")
            if live_example:
                spinning = any(query["predicate"] == "entity.rotated" for query in live_example["plan"]["queries"])
                runoff = any(query["predicate"] == "fluid.pools_on" for query in live_example["plan"]["queries"])
                detail = ("Describe the vertical-axis spin and where the water lands according to the results. "
                          "Use the west and east sections of the floor if the measurements support them. "
                          "Avoid end-over-end tumbling, runoff, or puddle depth.") if spinning else (
                          "Describe the fall without rotation or a spray pattern, since neither is measured.")
                if runoff:
                    detail = ("Use two distinct moments from the measured report. First say which floor gets water at impact. "
                              "Then explicitly say where the puddle settles after runoff. Use 'puddles' or 'pools' for "
                              "the final state. Keep both events conditional and avoid a depth number.")
                cue = ("Select the attached hypothetical experiment. Once the simulator has responded, describe the predicted fall in-world, "
                       "including what happens to the jug and water. Keep it conditional. " + detail + "\n" +
                       "Simulation setup: " +
                       json.dumps(live_example["plan"], separators=(",", ":")))
            start = time.perf_counter()
            submit(cue)

            def saved():
                view = request(web + "/state")
                story = json.loads(workspace.read_text(encoding="utf-8"))["stories"][0]
                if len(story["nodes"]) >= initial + 2 and not view["busy"]:
                    return story["nodes"][-1]

            try:
                reply = wait(saved, seconds=90)
            except AssertionError:
                reply = None
            trace = submit("/trace")["panel"]["text"]
            content = reply["content"] if reply else ""
            typed_report = None
            if live_example:
                for line in trace.splitlines():
                    if line.startswith('{"simulation":'):
                        candidate = json.loads(line)
                        if candidate["simulation"] == live_example["plan"]["name"]:
                            typed_report = candidate
                used_plan = matches_report(typed_report, live_example)
                pattern = r"\b(water|spill\w*)\b"
            else:
                used_plan = '"predicate":"fluid.volume_exceeds"' in trace and '"result":"TRUE"' in trace
                pattern = r"\b(water|drain|tank|valve|outlet)\b"
            result["live"] = {"elapsed_ms": (time.perf_counter() - start) * 1000, "reply": reply,
                              "trace": trace, "used_typed_plan": used_plan, "typed_report": typed_report,
                              "relevant_prose": bool(re.search(pattern, content, re.I))
                              and not re.search(r"\b(neutrino_tool|sim_plan|json)\b", content, re.I)}
            if live_example and runoff:
                result["live"]["reported_pool"] = bool(re.search(r"\b(pools?|puddles?|gathers?|settles?)\b", content, re.I) and
                                                       re.search(r"\b(east|eastern)\b", content, re.I))
        output.write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        print(f"{binary.name} median idle RSS {median(rss)/2**20:.2f} MiB")
        for plan in result["plans"]:
            print(f"{plan['name']} median {plan['median_ms']:.2f} ms")
        if provider:
            print(f"Live typed-plan selection and result: {result['live']['used_typed_plan']}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--fixtures", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--reps", type=int, default=10)
    parser.add_argument("--provider")
    parser.add_argument("--model")
    parser.add_argument("--live-plan", help="Name of the fixture plan to submit to the model")
    args = parser.parse_args()
    if args.reps < 1 or bool(args.provider) != bool(args.model):
        parser.error("Use a positive repeat count and supply provider and model together.")
    scenarios = json.loads(args.fixtures.read_text(encoding="utf-8")) if args.fixtures else []
    selected = next((example for example in scenarios if example["plan"]["name"] == args.live_plan), None)
    if args.live_plan and (not selected or not args.provider):
        parser.error("--live-plan needs a matching fixture and provider")
    run(args.binary.resolve(), scenarios, args.reps, args.out, args.provider, args.model, selected)
