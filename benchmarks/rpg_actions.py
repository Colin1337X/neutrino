"""Measure classic RPG commands through an isolated local Neutrino web process."""
import argparse
import json
import math
import os
import platform
import statistics
import tempfile
import time
from pathlib import Path

from fixtures import build
from gateway_history import free_port, process, request, wait


def summarize(values):
    ordered = sorted(values)
    return {"p50_ms": round(statistics.median(ordered), 3),
            "p95_ms": round(ordered[math.ceil(0.95 * len(ordered)) - 1], 3),
            "samples": ordered}


def benchmark(binary, starting_size, samples, warmup):
    with tempfile.TemporaryDirectory(prefix="neutrino-rpg-") as directory:
        folder = Path(directory)
        build(folder, 10)
        port = free_port()
        url = f"http://127.0.0.1:{port}/api"
        state = folder / "workspace.json"
        environment = {key: value for key, value in os.environ.items() if not key.startswith("NEUTRINO_")}
        with process(binary, ["--web", "--port", str(port), "--workspace", str(state)],
                     environment, folder / "app.log"):
            wait(lambda: request(url + "/state"))

            def action(text, expected):
                start = time.perf_counter_ns()
                view = request(url + "/action", {"action": "submit", "text": text})
                elapsed = (time.perf_counter_ns() - start) / 1e6
                combined = view.get("status", "") + "\n" + view.get("panel", {}).get("text", "")
                if expected not in combined:
                    raise AssertionError(f"{text!r}: {combined}")
                return elapsed

            action("/room Dock | Stone quay.", "Room created")
            action("/room Loft | High rafters.", "Room created")
            action("/connect Dock | Loft", "Rooms connected")
            for i in range(starting_size):
                action(f"/character create Member-{i} | A party member.", "Character created")
            action("/character switch Member-0", "Active character switched")
            action("/item Lantern | A brass lantern.", "World action saved")
            action("TAKE Lantern", "World action saved")

            tasks = {"switch": [], "inventory": [], "transfer": [], "look": [], "move": [], "create": []}
            for i in range(samples + warmup):
                holder = i % 2
                recipient = 1 - holder
                timings = {
                    "switch": action(f"/character switch Member-{holder}", "Active character switched"),
                    "inventory": action("INVENTORY held", "Lantern"),
                    "transfer": action(f"/give Lantern | Member-{recipient}", "Item handed over"),
                    "look": action("LOOK", "Room: Dock"),
                }
                # Traverse an exit and return. Only time the outward movement.
                timings["move"] = action("GO Loft", "World action saved")
                action("GO Dock", "World action saved")
                if i >= warmup:
                    for key, elapsed in timings.items():
                        tasks[key].append(elapsed)
            for i in range(samples + warmup):
                elapsed = action(f"/character create Reserve-{i} | A reserve.", "Character created")
                if i >= warmup:
                    tasks["create"].append(elapsed)
            result = {key: summarize(values) for key, values in tasks.items()}
            result["starting_party_size"] = starting_size
            result["workspace_bytes"] = state.stat().st_size
            return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--parties", type=int, nargs="+", default=(2, 25, 100))
    parser.add_argument("--samples", type=int, default=30)
    parser.add_argument("--warmup", type=int, default=5)
    args = parser.parse_args()
    if min(args.parties) < 2 or args.samples < 1 or args.warmup < 0:
        parser.error("Use at least two characters and one measured sample.")
    binary = args.binary.resolve(strict=True)
    output = {"endpoint": "local web API, request through response, including command persistence",
              "platform": platform.platform(), "processor": platform.processor(),
              "starting_story_turns": 10, "samples_per_command": args.samples, "discarded_warmups": args.warmup,
              "runs": [benchmark(binary, size, args.samples, args.warmup) for size in args.parties]}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(output, indent=2) + "\n", encoding="utf-8")
