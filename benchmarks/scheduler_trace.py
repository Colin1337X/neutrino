"""Verify scheduler diagnostics through the browser command API."""
import argparse
import json
import os
import tempfile
from pathlib import Path

from fixtures import build
from gateway_history import free_port, process, request, wait


def cases():
    def plan(name, actions, queries):
        return dict(neutrino_tool="sim_plan", name=name, mode="hypothetical",
                    actions=actions, queries=queries)

    exhaust = plan("24 cubed prescribed exhaust", [
        dict(neutrino_tool="scene_room", min=[0, 0, 0], max=[4, 3, 2]),
        dict(neutrino_tool="scene_medium", method="cfd", resolution=24, seconds=0,
             wind=[0.5, 0, 0], sigma_m=1000, viscosity=0.04, diffusivity=0.01,
             boundary="no_slip", boundaries={
                 "x_min": dict(type="inflow", speed_m_s=0.5, tracer=0),
                 "x_max": dict(type="outflow"),
             }),
        dict(neutrino_tool="scene_flow_step", seconds=1),
        dict(neutrino_tool="scene_flow", probe=[3.75, 1.6875, 1.125]),
    ], [dict(id=direction, predicate=f"flow.{direction}_exceeds", step=4, threshold=2.99)
        for direction in ("inflow", "outflow")])

    actions = [
        dict(neutrino_tool="scene_room", min=[0, 0, 0], max=[10, 6, 4]),
        dict(neutrino_tool="scene_place", kind="wall", name="Lower baffle", min=[4.9, 0, 0], max=[5.1, 3.5, 4]),
        dict(neutrino_tool="scene_place", kind="wall", name="Upper baffle", min=[4.9, 4.5, 0], max=[5.1, 6, 4]),
        dict(neutrino_tool="scene_material", name="reflector", acoustic=dict(reflectivity=0.9, loss_db_per_m=0),
             optical=dict(reflectivity=0.9)),
    ]
    for i in range(12):
        x, y = 1 + 1.25 * (i % 6), 0.25 + 3 * (i // 6)
        actions.append(dict(neutrino_tool="scene_place", kind="object", name=f"reflector{i}",
                            material_id="reflector", min=[x, y, 0], max=[x + 0.5, y + 0.5, 0.5]))
    for tool in ("scene_acoustic", "scene_light"):
        actions.append(dict(neutrino_tool=tool, source=[2, 2, 2], listener=[8, 2, 2],
                            max_bounces=4, wall_reflectivity=0.8, backend="cpu"))
    reflectors = plan("dense reflectors", actions, [
        dict(id=signal, predicate="signal.recursive_reaches_receiver", step=step)
        for step, signal in enumerate(("sound", "light"), len(actions) - 1)
    ])
    return [(exhaust, 1), (reflectors, 2)]


def verify(binary, mode):
    environment = {key: value for key, value in os.environ.items() if not key.startswith("NEUTRINO_")}
    environment.update(NEUTRINO_PHYSICS_WORKERS=mode, NEUTRINO_WEB_BIND="127.0.0.1")
    with tempfile.TemporaryDirectory(prefix="neutrino-scheduler-trace-") as scratch:
        folder = Path(scratch)
        build(folder, 1)
        port = free_port()
        web = f"http://127.0.0.1:{port}/api"
        args = ["--web", "--port", str(port), "--workspace", str(folder / "workspace.json"),
                "--server", "http://127.0.0.1:1/v1"]
        with process(binary, args, environment, folder / "web.log"):
            wait(lambda: request(web + "/state"))

            def submit(text):
                return request(web + "/action", {"action": "submit", "text": text})["panel"]

            scheduler = submit("/neofetch")["neofetch"]["physicsScheduler"]
            assert scheduler == ("Automatic" if mode == "auto" else f"Manual ({mode} requested)"), scheduler
            result = {"neofetch": scheduler, "cases": []}
            for plan, count in cases():
                submit("/sim " + json.dumps(plan, separators=(",", ":")))
                trace = submit("/trace")["text"]
                decisions = [line for line in trace.splitlines() if line.startswith("Physics scheduler")]
                assert len(decisions) == count, trace
                assert all("available CPUs" in line if mode == "auto" else f"{mode} workers (manual; {mode} requested" in line
                           for line in decisions), decisions
                report = next(json.loads(line) for line in trace.splitlines() if line.startswith('{"simulation":'))
                assert report["status"] == "completed" and all(entry["result"] == "TRUE"
                                                              for entry in report["consequences"]), report
                result["cases"].append({"report": report, "scheduler": decisions, "trace": trace})
            return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    automatic, manual = (verify(args.binary.resolve(), mode) for mode in ("auto", "3"))
    assert [case["report"] for case in automatic["cases"]] == [case["report"] for case in manual["cases"]]
    args.out.write_text(json.dumps({"automatic": automatic, "manual": manual}, indent=2, ensure_ascii=False) + "\n",
                        encoding="utf-8", newline="\n")
