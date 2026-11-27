# Live scheduler switching in v0.19.0

One web process started with `NEUTRINO_PHYSICS_WORKERS=4`. The test submitted each scheduler command, checked the status and `/neofetch`, reran the same 24³ exhaust and dense sound and light cases, and captured their worker decisions from `/trace`.

| Command | Mode in /neofetch | Exhaust | Dense sound | Dense light |
| --- | --- | ---: | ---: | ---: |
| `/sim workers auto` | Automatic | 4 | 2 | 2 |
| `/sim workers 3` | Manual (3 requested) | 3 | 3 | 3 |
| `/sim workers auto` | Automatic | 4 | 2 | 2 |

`/sim workers 17` reported a validation error and preserved the current mode in every run. The typed reports were identical. The inlet and exhaust measured 3 m³/s each, and each dense reflection query received four recursive paths.

Reproduce the command checks with `python benchmarks/scheduler_trace.py PATH_TO_NEUTRINO --out traces.json`. The [captured reports and traces](traces.json) include all three runs. Run `python benchmarks/provider_setup.py PATH_TO_NEUTRINO` to check provider flags and legacy environment-only startup against a loopback OpenAI-compatible endpoint.
