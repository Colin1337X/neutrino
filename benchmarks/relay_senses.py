"""Run Night at the Relay with a blocked direct path and capture scoped prompts."""

import argparse
import json
import os
import tempfile
import time
from contextlib import ExitStack
from pathlib import Path
from statistics import median

from fixtures import build
from gateway_history import free_port, process, request, wait


GEOMETRY = [
    {"neutrino_tool": "scene_room", "min": [0, 0, 0], "max": [10, 6, 4]},
    {"neutrino_tool": "scene_place", "kind": "wall", "name": "Lower baffle",
     "min": [4.9, 0, 0], "max": [5.1, 3.5, 4]},
    {"neutrino_tool": "scene_place", "kind": "wall", "name": "Upper baffle",
     "min": [4.9, 4.5, 0], "max": [5.1, 6, 4]},
]
SENSORS = {"Mara": (-45, 1e-5), "Iona": (-45, .01), "Tessa": (-20, .01)}
CUE = ("At [2,2,2], a brief sound and flash occur together behind the baffles. "
       "Before writing the reply, emit this as a real event so the room's sensors decide what the three "
       "watchkeepers perceive. Use two reflections and a wall reflectivity of 0.8 for light. "
       "Keep the source unidentified and describe each person's immediate reaction in a short reply.")


def run(binary, provider, model, output, reps):
    with tempfile.TemporaryDirectory(prefix="neutrino-relay-senses-") as scratch, ExitStack() as stack:
        folder = Path(scratch)
        build(folder, 1)
        workspace = folder / "workspace.json"
        data = json.loads(workspace.read_text(encoding="utf-8"))
        data["stories"][0]["nodes"] = [{"id": 1, "parent": 0, "role": "assistant",
                                       "content": "Three watchkeepers wait on the east side of the relay baffles."}]
        data["stories"][0]["head"] = 1
        workspace.write_text(json.dumps(data), encoding="utf-8")
        ports = (free_port(), free_port())
        gateway, web = f"http://127.0.0.1:{ports[0]}/v1", f"http://127.0.0.1:{ports[1]}/api"
        environment = {key: value for key, value in os.environ.items() if not key.startswith("NEUTRINO_")}
        environment.update(NEUTRINO_SERVER_BIND="127.0.0.1", NEUTRINO_WEB_BIND="127.0.0.1",
                           NEUTRINO_AUTO_RESEARCH="0", NEUTRINO_STREAM_USAGE="1",
                           NEUTRINO_ENDPOINT=provider, NEUTRINO_MODEL=model)
        stack.enter_context(process(binary, ["--serve", "--port", str(ports[0])], environment, folder / "gateway.log"))
        wait(lambda: request(gateway + "/status"))
        args = ["--web", "--port", str(ports[1]), "--workspace", str(workspace), "--server", gateway]
        stack.enter_context(process(binary, args, environment, folder / "web.log"))
        wait(lambda: request(web + "/state"))

        def submit(text):
            return request(web + "/action", {"action": "submit", "text": text})

        def panel(text):
            return submit(text)["panel"]["text"]

        def story():
            return json.loads(workspace.read_text(encoding="utf-8"))["stories"][0]

        submit("/room Relay | A watch station divided by opaque baffles.")
        for name in SENSORS:
            submit(f"/npc {name} | A watchkeeper stationed at the east console.")
        submit("/sim " + json.dumps({"actions": GEOMETRY}))
        for name, (hearing, vision) in SENSORS.items():
            submit("/sim " + json.dumps({"neutrino_tool": "scene_sensor", "name": name, "listener": [8, 2, 2],
                                         "min_level_db": hearing, "min_power": vision}))

        measurements = []
        for depth in (1, 2):
            actions = [{"neutrino_tool": tool, "source": [2, 2, 2], "listener": [8, 2, 2],
                        "max_bounces": depth, "wall_reflectivity": .8} for tool in ("scene_acoustic", "scene_light")]
            queries = [{"id": f"{step}.{name}", "step": step, "predicate": f"signal.{name}"}
                       for step in (1, 2) for name in ("direct_blocked", "reaches_receiver", "recursive_reaches_receiver")]
            plan = {"neutrino_tool": "sim_plan", "mode": "hypothetical", "name": f"depth {depth}",
                    "actions": actions, "queries": queries}
            timings = []
            before = workspace.read_bytes()
            for rep in range(reps + 1):
                start = time.perf_counter()
                text = panel("/sim " + json.dumps(plan, separators=(",", ":")))
                if rep:
                    timings.append((time.perf_counter() - start) * 1000)
                assert before == workspace.read_bytes(), "Hypothetical plan changed the workspace"
            report = next(json.loads(line) for line in text.splitlines() if line.startswith('{"simulation":'))
            assert report["status"] == "completed", text
            expected = ["TRUE", "FALSE", "FALSE"] if depth == 1 else ["TRUE", "TRUE", "TRUE"]
            assert [q["result"] for q in report["consequences"]] == expected * 2, report
            measurements.append({"depth": depth, "times_ms": timings, "median_ms": median(timings), "report": report,
                                 "trace": text})

        def generate(text):
            previous = len(story()["nodes"])
            start = time.perf_counter()
            submit(text)

            def idle():
                return not request(web + "/state")["busy"]

            wait(idle, seconds=90)
            nodes = story()["nodes"]
            reply = nodes[-1] if len(nodes) == previous + 2 and nodes[-1]["role"] == "assistant" else None
            return {"reply": reply, "elapsed_ms": (time.perf_counter() - start) * 1000, "trace": panel("/trace")}

        emissions = [generate(CUE)]
        observations = {name: panel(f"/knowledge {name}") for name in SENSORS}
        characters = {}
        if "reflected flash" in observations["Mara"]:
            for name in SENSORS:
                characters[name] = generate(f"/talk {name} | What did you personally notice just now?")
        output.write_text(json.dumps({"model": model, "measurements": measurements, "emission": emissions[0],
                                     "observations": observations, "characters": characters}, indent=2) + "\n",
                          encoding="utf-8")
        assert emissions[0]["reply"] and '"neutrino_tool":"scene_emit"' in emissions[0]["trace"], emissions[0]
        assert "2-bounce echo" in emissions[0]["trace"] and "2-bounce reflection" in emissions[0]["trace"]
        assert "Heard an echo" in observations["Mara"] and "reflected flash" in observations["Mara"]
        assert "Heard an echo" in observations["Iona"] and "reflected flash" not in observations["Iona"]
        assert "No sensory observations" in observations["Tessa"]
        for name, has_sound, has_light in (("Mara", True, True), ("Iona", True, False), ("Tessa", False, False)):
            result = characters[name]
            assert result["reply"] and result["reply"]["characterScope"] == name, result
            assert ("You observed: Heard an echo" in result["trace"]) == has_sound, result
            assert ("You observed: Saw a reflected flash" in result["trace"]) == has_light, result
            assert "Lower baffle" not in result["trace"] and "source_entity" not in result["trace"], result
        print("Verified blocked direct paths, the live emission, and all three character contexts.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--provider", required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--reps", type=int, default=5)
    args = parser.parse_args()
    if args.reps < 1:
        parser.error("--reps must be positive")
    run(args.binary.resolve(strict=True), args.provider, args.model, args.out, args.reps)
