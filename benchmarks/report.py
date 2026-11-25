"""Validate raw trials and render benchmark evidence without report dependencies."""
import csv
import html
import json
import math
import statistics
import sys
from collections import Counter, defaultdict
from pathlib import Path


def read_lines(path):
    if not path.is_file():
        raise SystemExit(f"Missing evidence file: {path}")
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]


def write_csv(path, rows):
    keys = list(dict.fromkeys(key for row in rows for key in row))
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, keys)
        writer.writeheader()
        writer.writerows(rows)


def median(values):
    values = [x for x in values if isinstance(x, (int, float)) and math.isfinite(x)]
    return round(statistics.median(values), 3) if values else None


def common(row):
    return {key: row.get(key) for key in (
        "run_id", "configuration", "category", "inference_mode", "track",
        "history_turns", "scenario_id", "repetition", "status", "error",
    )}


def memory_of(row, component, key):
    value = row.get("metrics", {}).get("memory", {}).get(component, {}).get(key)
    return round(value / 1048576, 3) if value is not None else None


def main(folder, command):
    raw = read_lines(folder / "raw_results.jsonl")
    inference = read_lines(folder / "inference.jsonl")
    journals = {row["run_id"] for row in inference}
    invalid = [
        row.get("run_id", "missing identifier") for row in raw
        if row.get("schema_version") != 1 or row.get("status") not in ("completed", "failed", "unavailable")
        or row.get("category") == "inference" and row["run_id"] not in journals
        or row.get("category") == "inference" and row["status"] == "completed"
        and not isinstance(row.get("correctness", {}).get("reply"), str)
    ]
    if invalid:
        raise SystemExit(f"Invalid records: {invalid}")
    print(f"Validated {len(raw)} raw records and {len(inference)} inference journals")
    if command == "validate":
        return

    # Retries remain in the raw files. Use the most recent successful record in summaries.
    unique = {}
    for row in raw:
        if row["category"] == "inference" and row["status"] == "completed" and not row["correctness"]["reply"].strip():
            row = {**row, "status": "failed", "error": "Persisted assistant turn was empty"}
        if row["status"] == "completed" or row["run_id"] not in unique:
            unique[row["run_id"]] = row
    grouped = defaultdict(list)
    latency, memory, cost, correctness, gpu = [], [], [], [], []
    counters = Counter()
    for row in unique.values():
        fields = common(row)
        metrics = row.get("metrics", {})
        scored = row.get("correctness", {})
        counters[row["category"], row["status"]] += 1
        correctness.append({**fields, "checks": json.dumps(scored, ensure_ascii=False)})
        if row["category"] == "gpu_scaling":
            gpu.append({**fields, **metrics, "actual_backend": scored.get("actual_backend")})
        if row["category"] != "inference":
            continue
        key = (row["inference_mode"], row["track"], row["history_turns"], row["configuration"])
        grouped[key].append(row)
        latency.append({**fields, **{k: metrics.get(k) for k in (
            "backend_ready_ms", "interface_ready_ms", "prompt_preparation_ms",
            "server_ttft_ms", "visible_ttft_ms", "usable_ttft_ms", "accepted_ms",
        )}})
        cost.append({**fields, **{k: metrics.get(k) for k in ("request_count", "input_tokens", "output_tokens")}})
        for component, values in metrics.get("memory", {}).items():
            if isinstance(values, dict):
                memory.append({**fields, "component": component, **values})

    summary = []
    for (mode, track, turns, config), rows in sorted(grouped.items()):
        done = [row for row in rows if row["status"] == "completed"]
        accepted = [r["metrics"]["accepted_ms"] for r in done]
        summary.append({
            "mode": mode, "track": track, "turns": turns, "configuration": config,
            "completed": len(done), "attempted": len(rows),
            "accepted_median_ms": median(accepted),
            "accepted_min_ms": round(min(accepted), 3) if accepted else None,
            "accepted_max_ms": round(max(accepted), 3) if accepted else None,
            "visible_ttft_median_ms": median(r["metrics"].get("visible_ttft_ms") for r in done),
            "provider_ttft_median_ms": median(r["metrics"].get("server_ttft_ms") for r in done),
            "app_idle_mib": median(memory_of(r, "app", "idle_tree_bytes") for r in done),
            "app_peak_mib": median(memory_of(r, "app", "peak_tree_bytes") for r in done),
            "browser_idle_mib": median(memory_of(r, "browser", "idle_tree_bytes") for r in done),
            "gateway_idle_mib": median(memory_of(r, "gateway", "idle_tree_bytes") for r in done),
            "combined_peak_mib": median(memory_of(r, "combined", "peak_tree_bytes") for r in done),
            "recall_hits": (sum(r["correctness"].get("recall") is True for r in done)
                            if mode == "live" and all(r["scenario_id"].startswith("relay-recall-") for r in rows) else None),
        })
    native, extension, partial, unknown, external = (
        "SUPPORTED_NATIVE", "SUPPORTED_EXTENSION", "SUPPORTED_PARTIAL", "UNVERIFIED", "NOT_APPLICABLE",
    )
    features = [
        ("Character card import", native, native, "Neutrino workspace.c; SillyTavern src/endpoints/characters.js and seeded card"),
        ("Character card export", unknown, native, "SillyTavern src/endpoints/characters.js"),
        ("Keyword lore", native, native, "1,000-turn shared keyword-lore prompts; Neutrino workspace.c; SillyTavern world-info.js"),
        ("Personas and prompt presets", native, native, "Neutrino main.c /persona and /preset; SillyTavern public/scripts/personas.js and openai.js"),
        ("Context retrieval", native, native, "Captured live and synthetic prompt journals, workspace.c and world-info.js"),
        ("Summarization", native, extension, "Neutrino memory.c; SillyTavern bundled extension memory on recommended track"),
        ("Vector retrieval", unknown, extension, "SillyTavern bundled vectors extension; vector backend execution unmeasured"),
        ("Conversation branching", native, native, "Neutrino branch-evidence.json; SillyTavern public/scripts/bookmarks.js"),
        ("Branch-aware tracked state", native, unknown, "Neutrino branch-evidence.json contains room, inventory, clock, bond, injury and gravity"),
        ("Multi-character roleplay", partial, native, "Neutrino /npc and named address; SillyTavern public/scripts/group-chats.js"),
        ("Character knowledge isolation", unknown, unknown, "No character-perspective benchmark executed"),
        ("Inventory and spatial state", native, unknown, "Neutrino world.c and branch-evidence.json"),
        ("Time, needs, relationships and injuries", native, unknown, "Neutrino simulation.c and branch-evidence.json"),
        ("Deterministic physics and acoustics", native, unknown, "Neutrino physics and numerical evidence"),
        ("GPU ray intersection", native, unknown, "Neutrino direct hardware and application tests"),
        ("Ordered actions and rollback", native, unknown, "Neutrino physics report and scene_physics test"),
        ("Narration measurement validation", native, unknown, "Neutrino measurements.c and scene_physics test"),
        ("Web research", native, unknown, "Neutrino main.c /research and web.c; no browser lookup in this run"),
        ("Integrated manuscript editing", native, unknown, "Neutrino main.c and browser app manuscript controls"),
        ("Native terminal", native, unknown, "Neutrino TUI exercised via a Windows PTY"),
        ("Browser interface", native, native, "Both browser interfaces exercised in Chromium"),
        ("OpenAI-compatible provider connection", native, native, "Both apps used the same proxy and live provider"),
        ("Text to speech", unknown, extension, "SillyTavern bundled tts; network provider deliberately untested"),
        ("Speech to text", unknown, unknown, "No compatible configuration tested"),
        ("Image generation", unknown, extension, "SillyTavern bundled stable-diffusion; service execution untested"),
        ("Multimedia customization", unknown, native, "SillyTavern backgrounds and bundled assets, gallery, expressions"),
        ("User-installed extensions", unknown, native, "SillyTavern public/scripts/extensions.js"),
        ("Theming", partial, native, "Neutrino fixed theme with responsive layouts; SillyTavern public/scripts/power-user.js"),
        ("Accessibility", partial, partial, "Source contains labels and keyboard controls; no assistive technology evaluation"),
        ("Platform comparison", partial, partial, "Windows runtime measured. This suite did not compare Linux or macOS."),
        ("Licensing", external, external, "Neutrino MIT; SillyTavern AGPL-3.0; see each LICENSE file"),
        ("Production dependencies", external, external, "Neutrino CMakeLists.txt and package manifest; SillyTavern production npm dependency tree"),
    ]
    feature_rows = [{"capability":capability, "Neutrino":neutrino, "SillyTavern":st, "evidence":evidence}
                    for capability, neutrino, st, evidence in features]
    for name, rows in (("summary", summary), ("latency", latency), ("memory", memory),
                       ("inference_cost", cost), ("correctness", correctness),
                       ("gpu_scaling", gpu), ("feature_matrix", feature_rows)):
        write_csv(folder / f"{name}.csv", rows)
    failures = [row for row in raw if row["status"] != "completed"]
    failures.extend(row for row in unique.values() if row.get("error") == "Persisted assistant turn was empty"
                    and not any(existing["run_id"] == row["run_id"] for existing in failures))
    (folder / "failures.jsonl").write_text(
        "".join(json.dumps(row, ensure_ascii=False) + "\n" for row in failures), encoding="utf-8")
    notes = [
        "Accepted latency is the time from submit to the first observed persisted assistant reply.",
        "Controlled trials use equivalent fixture messages and a shared provider sampling policy. Product prompts, truncation and token budgeting can differ.",
        "The recall fixture is seeded history. It measures fact availability after prompt assembly. Synthetic replies are scripted and do not score recall.",
        "The recommended track adds the same early fact as a keyword lore entry, with the bundled Main API Summarize setting for ST-EXTENDED.",
        "Memory is sampled process-tree working set. Browser and gateway are reported separately. Combined browser working set can count shared pages more than once. Provider and runner are excluded from combined figures.",
        "The idle sample is taken after backend and interface readiness, before the prompt. A sampled peak can miss short spikes.",
        "Provider TTFT starts at the benchmark proxy. Visible TTFT ends at the first text detected by the UI probe. Exact token cost is blank when the provider omits usage.",
        "Medians describe the observed samples. Small samples do not justify p95 or population claims. Read counts and failures alongside medians.",
        "World Info, Compendium and vector-backed recall differ. No vector provider was configured and this report does not score vector recall, equivalent extensions, or complete security coverage.",
        "Detailed evidence is in raw_results.jsonl, inference.jsonl, gpu_scaling.csv, correctness.csv, and each trial directory.",
    ]
    labels = [(key, key.replace("_", " ")) for key in (
        "mode", "track", "turns", "configuration", "completed", "attempted", "accepted_median_ms",
        "visible_ttft_median_ms", "app_idle_mib", "app_peak_mib", "combined_peak_mib", "recall_hits",
    )]
    def value(row, key):
        return "—" if row[key] is None else str(row[key])

    counts = [f"{category} {status}: {count}" for (category, status), count in sorted(counters.items())]
    links = "See environment.json, revisions.json, and configurations.json for versions and parameters."
    md = ["# Neutrino versus SillyTavern", "", links, "", *[f"- {x}" for x in counts], "",
          "| " + " | ".join(label for _, label in labels) + " |",
          "| " + " | ".join("---" for _ in labels) + " |"]
    md.extend("| " + " | ".join(value(row, key) for key, _ in labels) + " |" for row in summary)
    md.extend(["", "## Measurement notes", "", *[f"- {note}" for note in notes]])
    (folder / "report.md").write_text("\n".join(md) + "\n", encoding="utf-8")
    e = html.escape
    headings = "".join(f"<th>{e(label)}</th>" for _, label in labels)
    rows = "".join("<tr>" + "".join(f"<td>{e(value(row, key))}</td>" for key, _ in labels) + "</tr>" for row in summary)
    chart_rows = [row for row in summary if row["mode"] == "synthetic" and row["track"] == "controlled" and row["turns"] == 10]
    charts = []
    for key, title in (("app_idle_mib", "Application idle working set (MiB)"),
                       ("combined_peak_mib", "Browser configuration peak working set (MiB)"),
                       ("accepted_median_ms", "Synthetic accepted latency (ms)")):
        maximum = max((row[key] or 0 for row in chart_rows), default=0)
        if maximum:
            bars = "".join(f'<div class="bar"><span>{e(r["configuration"])}</span>'
                           f'<meter min="0" max="{maximum}" value="{r[key]}"></meter><span>{r[key]}</span></div>'
                           for r in chart_rows if r[key] is not None)
            charts.append(f"<section><h2>{e(title)}</h2>{bars}</section>")
    body = "".join(f"<li>{e(note)}</li>" for note in [*counts, *notes])
    (folder / "report.html").write_text(
        '<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width">'
        '<title>Neutrino versus SillyTavern</title><style>'
        'body{font:15px system-ui;margin:clamp(1rem,4vw,3rem);background:#101820;color:#edf2f6}a{color:#83e5c2}'
        'table{border-collapse:collapse;font-variant-numeric:tabular-nums}th,td{padding:.5rem;text-align:left;'
        'border-bottom:1px solid #41515c}th{color:#83e5c2}.scroll{overflow-x:auto}.bar{display:grid;'
        'grid-template-columns:8rem minmax(70px,25rem) 5rem;gap:1rem}meter{width:100%;height:1.6rem}'
        '</style><h1>Neutrino versus SillyTavern</h1><p>Evidence and measurement scope</p>'
        '<p><a href="summary.csv">Summary CSV</a> · <a href="raw_results.jsonl">Raw trials</a> · '
        '<a href="failures.jsonl">Failures</a></p>'
        + "".join(charts) + f'<div class="scroll"><table><thead><tr>{headings}</tr></thead><tbody>{rows}'
        + f"</tbody></table></div><h2>Measurement notes</h2><ul>{body}</ul></html>", encoding="utf-8")


if __name__ == "__main__":
    main(Path(sys.argv[1]), sys.argv[2])
