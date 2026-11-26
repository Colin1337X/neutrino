# Night at the Relay sensory verification

Baseline `b6c1db5`. The live run used an optimized Windows C23 build and the same local KoboldCpp model as the preceding Phase II tests, Gemma4-12B-QAT-Uncensored-HauhauCS-Balanced-Q4_K_M. The run started with a fresh Night at the Relay workspace through the loopback web client and gateway, with no style note.

Two opaque baffles split a 10 × 6 × 4 m room. They leave a one-metre opening offset from the line between source `[2,2,2]` and receivers `[8,2,2]`. Typed queries confirmed that the direct ray was blocked and that there were no arrivals at depth one, for both sound and light. At depth two, both signals reached the receivers via four qualifying paths at Mara's thresholds. The shortest path unfolded to `hypot(10,8) = 12.806248 m`. The independent native regression also checks the expected reflection strengths, transmissive material and air loss, and the cutoffs.

## Live trace

The narrator called `scene_emit` with both signals enabled on its first request and accepted the resulting observations with its reply. The tool reported the following for Mara's earliest paths.

```text
Mara · sound
Direct blocked.
2-bounce echo [X-min → Y-max] 12.806 m · 37.311 ms · -29.63 dB · 0 material traversals.

Mara · light
Direct blocked.
2-bounce reflection [X-min → Y-max] 12.806 m · 42.717 ns · -24.09 dB · 0 material traversals.
```

| Character | Saved observations | Reply from the restricted context |
| --- | --- | --- |
| Mara | Echo and reflected flash, source unidentified | “I did notice a reflected flash, though its source was unclear to me. I also heard an echo, but I can't quite tell where it came from.” |
| Iona | Echo, source unidentified | “I did hear an echo just now. It’s difficult to say exactly where it came from, as its source was quite unclear.” |
| Tessa | None | “To be honest, I noticed nothing about the event.” |

The raw log contains the narrator's `/trace`, all three private prompts, and the saved `/knowledge` output. The first in-development run exposed a prose issue in which Tessa guessed at a flicker although her prompt and ledger contained no sensory facts. The final scoped prompt explicitly tells characters to report only recorded sensations when asked about recent events. The native tests also check absence on sibling branches and for a receiver relocated out of the room. [Final live run](live.json) · [Earlier live run](live-first.json)

## Performance

Medians of five batch means, on CPU. Each call includes room query setup, propagation, and text formatting. The room contains opaque baffles and six reflective boundary faces.

| Maximum bounces | Sound, µs | Light, µs | Recursive arrivals per signal |
| ---: | ---: | ---: | ---: |
| 0 | 4.50 | 2.25 | 0 |
| 1 | 5.50 | 3.00 | 0 |
| 2 | 15.00 | 12.50 | 4 |
| 3 | 26.25 | 22.50 | 8 |
| 4 | 45.00 | 42.50 | 18 |

The final depth-one and depth-two loopback query pairs took 61.12 and 49.60 ms median. These include web scheduling and are useful for checking completion rather than for comparing solver cost. The real-model emission and saved narration took 6.31 seconds over two inference requests with 3,917 provider-reported prompt tokens. The subsequent private replies each used one model request and 111–138 prompt tokens. [Native timing samples](propagation-timing.csv)

All five native tests passed on Windows CPU, Windows Vulkan RT, and Ubuntu with AddressSanitizer, LeakSanitizer, and UndefinedBehaviorSanitizer. Hardware RT visibility produced the same blocked-path and recursive-arrival results as CPU visibility. The existing OpenCL CFD parity and scoped browser-to-provider prompt regression also passed.

## Reproduction

```sh
ctest --test-dir build --output-on-failure
build/scene_physics_test --propagation-timing
build/scene_physics_test --rt
python benchmarks/relay_senses.py build/neutrino --provider "$PROVIDER" --model "$MODEL" --out relay-senses.json
```

On Windows, use the `.exe` executables. Recursive search uses CPU geometry and specular box faces. Receiver positions are explicitly configured. An accepted emission records positive observations; a hypothetical or failed emission leaves the branch ledger untouched. Signal identity, diffraction, and diffuse scattering are outside the modeled scope.
