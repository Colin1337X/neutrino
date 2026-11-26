# Phase II verification

Baseline revision `fc5bb3d`. The implementation revision is the commit that adds this report. Measurements were collected on Windows with a Ryzen 5 9600X and an RTX 5060 Ti, using C23 Release builds with `-O3`. The live provider was KoboldCpp on loopback with Gemma4-12B-QAT-Uncensored-HauhauCS-Balanced-Q4_K_M.

## Reconstruction and solver reuse

The checkpoint fixture uses analytical clock events with sibling histories attached at multiple depths. Full replay and incremental replay restore the same target. Incremental replay applies one descendant event and skips a sibling event. Each value is the median of five batch means from [the raw runs](checkpoint-timing.csv).

| Events at checkpoint | Full replay, ms | Checkpoint, ms | Checkpoint + 1 relevant event, ms | Checkpoint JSON bytes |
| ---: | ---: | ---: | ---: | ---: |
| 10 | 0.003906 | 0.000500 | 0.001000 | 127 |
| 100 | 0.021484 | 0.000500 | 0.001000 | 129 |
| 1,000 | 0.230469 | 0.000750 | 0.000750 | 132 |
| 10,000 | 4.750000 | 0.000500 | 0.000750 | 134 |
| 100,000 | 119.843750 | 0.000500 | 0.001000 | 137 |

The synthetic checkpoint stores a clock and empty physics and character collections. A compound physical checkpoint will use more space. Very small timings are batch averages from a coarse timer. Native tests compare snapshots against event replay after serialization, including fracture, rigid position and orientation, angular state, water parcels, runoff configuration and material overrides. Separate tests exercise sibling branches, a late change at their shared ancestor, staging before commitment and cumulative budget rejection.

At 24³ CFD cells, the median fresh probe took 28.75 ms. Reusing its field took 0.002 ms and produced equal concentration in all timed samples. This compares a full solve plus a probe with a probe of the same solved grid. The ordered-plan regression verifies reuse for optical attenuation and invalidation after a medium edit. OpenCL pressure projection and Windows file-mapped memory matched the CPU reference in the regression fixture. [CFD timings](cfd-reuse-timing.csv) · [OpenCL parity output](opencl-cfd.txt)

The existing hardware RT backend matched CPU hit distances and box IDs for 4,096 rays across cached and edited geometry. The separate Vulkan **compute** benchmark had a median of 7.92 million ray–box intersections per second at 4,096 intersections per batch. First dispatch in that run took 205 ms. [Hardware RT parity output](hardware-rt.txt) · [Vulkan compute timings](vulkan-compute-timing.csv)

## Application and live inference

The Windows CPU executable grew from 704,512 to 722,432 bytes. The Vulkan RT executable grew from 731,136 to 750,080 bytes. Idle browser-process working set was 7.53 MiB for the baseline sample and 6.78 MiB for the Phase II sample, each the median of 15 readings. These are small single-process samples. The seven loopback fixtures preserved workspace bytes and returned the expected typed outcomes. Median request times ranged from 60.29–61.68 ms before and 45.74–60.64 ms after. The loopback times include browser and gateway scheduling. [Baseline samples](baseline.json) · [Phase II samples](current.json)

| Live test | Narrator requests | Provider-reported prompt tokens | Time through save | Result |
| --- | ---: | ---: | ---: | --- |
| Attached runoff plan | 2 | 1,817 | 3.70 s | The model selected the supplied plan and described the west-to-east pooling result |
| Model-authored typed plan from field-level directions | 2 | 3,321 | 6.86 s | The typed predicate passed. The saved fiction omitted the expected drainage terms |
| Natural drain request, first run | 2 | 5,640 | 7.12 s | The model chose the fluid tool on the first request |
| Natural drain request, second run | 4 | 9,026 | 12.76 s | The model chose the fluid tool after a malformed request and used a later prose retry |

The attached experiment reported 0.288 rad of rotation, a fracture, 1 L released, 0.56 L initially on the west floor, then 1 L pooled on the east floor at 3.347 mm peak depth. It also reported a 7.297 ms sound arrival. Both natural drain runs calculated 81.525 L in ten seconds from the scene description, which named no tool or schema. The first run's archived summary used an incorrect litres-text benchmark check. Its tool execution, numerical result and saved reply are visible in the trace. The revised checker inspects the saved tool result. [Attached plan and trace](live-attached.json) · [Model-authored plan and trace](live-autonomous.json) · [First natural run](live-natural-first.json) · [Second natural run](live-natural.json)

The isolated-character test captured the actual provider request after sending `/talk` through the browser and gateway. It received the authorized cue and fact, and excluded the fixture's private markers. A subsequent structured geometry request produced no assistant reply and left the simulation event log unchanged. The first request also demonstrated saving the bare locker code after a provider stop without a retry. [Captured payload and trace](character-isolation.json)

## Verification and limits

- All five CTest targets passed on Windows CPU, Windows Vulkan RT and Ubuntu under AddressSanitizer, LeakSanitizer and UndefinedBehaviorSanitizer.
- Ubuntu reported a pre-existing conservative format-truncation warning in the rigid fragment-name formatter. The tested solver cases passed.
- The code changes cover simulation snapshots and budgets, scoped character inference, solver reuse, provider finish and usage metadata, and the short-answer guard. The native code remains C23 and uses the existing production dependencies.
- Budgets cover the costs modeled before dispatch. Process-wide accounting, VRAM limits, wall-time cancellation and interruption inside solvers remain open.
- Flow fields are reused within an ordered plan. Persistent numerical grids across plans or save files, time-evolving character observations and belief transfer beyond explicit player grants remain open.
- The propagation kernels, hard RT batch ceiling, rigid narrow phase and constraints, and transport model approximations are unchanged. Recursive propagation, numerical free surfaces, full two-way thermal and phase coupling, out-of-core scheduling and the complete epistemic scenario matrix remain unimplemented in this increment.
- macOS execution, large-world physical replay stress with 100,000 expensive steps, and fuzzing were outside the measured run. The tested workloads are fictional simulations.

## Reproduction

Run the following with the corresponding built executable. On Windows, append `.exe`. The live commands also require Python with psutil and an OpenAI-compatible local model server.

```sh
ctest --test-dir build --output-on-failure
build/scene_physics_test --checkpoint-timing
build/scene_physics_test --cfd-reuse-timing
build/scene_physics_test --rt
build/scene_physics_test --cfd-gpu
cmake --build build --target ray_scaling_bench
build/ray_scaling_bench vulkan
python benchmarks/character_isolation.py build/neutrino --out character-isolation.json
python benchmarks/multiphysics.py build/neutrino --fixtures benchmarks/runoff-container.json --out runoff.json
python benchmarks/multiphysics.py build/neutrino --provider "$PROVIDER" --model "$MODEL" --out model-authored.json
python benchmarks/multiphysics.py build/neutrino --provider "$PROVIDER" --model "$MODEL" --natural --out natural.json
python benchmarks/multiphysics.py build/neutrino --provider "$PROVIDER" --model "$MODEL" \
  --fixtures benchmarks/runoff-container.json --live-plan "falling water container with runoff" --out attached.json
```
