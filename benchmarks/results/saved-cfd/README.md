# Persistent CFD verification

The baseline revision was `da8dcce`. The implementation saves the tracer and three velocity components of an accepted CFD step as a versioned, lossless binary64 frame. Later plans restore that frame for probing, acoustic attenuation and optical extinction. The next step continues from its elapsed simulation time.

## Night at the Relay

The 12³ fog fixture ran on the same loopback KoboldCpp Gemma 4 12B model used for the preceding relay test. A manual setup advanced the field to 0.25 s. The narrator then called `scene_flow_step` to advance it to 0.50 s and saved prose. The web client rewound to the earlier node, reopened the workspace, and returned to the advanced node. Measurements matched the original values after each restoration.

| Accepted field time | Tracer at probe | Received light relative to the source at 1 m |
| ---: | ---: | ---: |
| 0.25 s | 0.370933 | 0.785923 |
| 0.50 s | 0.587099 | 0.610738 |

The initial model run put speculative prose immediately after each tool object. The parser rejected the combined replies and left the field at 0.25 s. The fixed parser keeps the CFD step at the start of the reply, discards the prose draft, and requests narration after the solver returns. In the final run, the model's first request omitted `seconds`. The corrected request advanced the field once, and the turn saved in 10.70 seconds. [Final trace and measured values](live.json) · [Initial run](live-first.json)

## Native benchmark

Windows C23 Release build with `-O3`, measured on a Ryzen 5 9600X. Values are medians of five batch means. Each comparison uses the same 0.5-second field and probe. Restore includes decoding, allocation and diagnostics. It excludes reading the workspace file, replaying its JSON event history and rendering the UI.

| Grid | Fresh solve and probe | Restore and probe | Encoded bytes per accepted step |
| ---: | ---: | ---: | ---: |
| 6³ | 0.170 ms | 0.034 ms | 9,216 |
| 12³ | 2.070 ms | 0.332 ms | 73,728 |
| 24³ | 31.083 ms | 3.080 ms | 589,824 |

All samples returned the same concentration as a fresh solve. Native tests cover lossless round-trip, malformed frames, sibling isolation, checkpoint and event-replay parity, attenuation from the restored field, the per-plan storage budget, and rollback after a hypothetical step. All five CTest targets passed on Windows CPU, Windows Vulkan RT and Ubuntu with ASan, LSan and UBSan. The Windows OpenCL and mapped-memory parity fixture also passed. [Raw timings](persistent-cfd-timing.csv)

Each accepted step appends its own frame. Log growth, historical deduplication, and loading very long workspaces remain open. The frame persists tracer and velocity and rebuilds the pressure and geometry working buffers.

## Reproduction

```sh
ctest --test-dir build --output-on-failure
build/scene_physics_test --persistent-cfd-timing
python benchmarks/saved_cfd.py build/neutrino --provider "$PROVIDER" --model "$MODEL" --out saved-cfd.json
```
