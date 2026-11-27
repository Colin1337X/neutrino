# Scheduler diagnostics in v0.18.1

The browser command API ran the 24³ prescribed exhaust benchmark and the relay scene with 12 added reflectors. It sent each hypothetical `/sim` plan, opened `/trace`, and read the scheduler mode from `/neofetch`. The commands ran against an isolated workspace.

| Mode in /neofetch | Volume exhaust | Dense sound | Dense light |
| --- | ---: | ---: | ---: |
| Automatic | 4 workers | 2 workers | 2 workers |
| Manual (3 requested) | 3 workers | 3 workers | 3 workers |

The automatic exhaust decision used 13,824 grid cells at 3,456 per worker. Each reflection decision used 8,192 candidate work units at 4,096 per worker. The host exposed six logical CPUs. Both runs measured 3 m³/s at the inlet and exhaust and four recursive arrivals each for sound and light. The full typed reports matched.

Run with `python benchmarks/scheduler_trace.py PATH_TO_NEUTRINO --out traces.json`. The captured command panels and typed reports are in [traces.json](traces.json).
