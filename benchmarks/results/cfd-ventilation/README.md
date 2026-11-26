# CFD ventilation verification

The 4 × 3 × 2 m room starts with tracer concentration near 1. Air enters the full X-min face at 0.5 m/s with zero tracer. The X-max exhaust balances the inlet, and the four remaining walls are stationary and no-slip. The loopback web fixture advanced the 8³ field to 2 s and reused it for two probes. Each hypothetical repeat left the workspace bytes unchanged.

| Measurement | Result |
| --- | ---: |
| Inlet and exhaust | 3.000 m³/s each |
| Sealed-room baseline at inlet probe | 0.999998 |
| Ventilated inlet probe | 0.130628 |
| Ventilated exhaust probe | 0.975259 |
| Tracer removed in 2 s | 5.923807 concentration·m³ |
| RMS divergence after the step | 0.009594 s⁻¹ |

Native regressions verified the tracer balance to an absolute tolerance of 10⁻¹⁰ concentration·m³, clean-air flushing, injection through a rectangular inlet, zero normal wall velocity, and rejection of mismatched fixed exhaust and isolated compartments. [Typed results and solver trace](ventilated-room.json)

## CPU benchmark

Windows C23 Release at `-O3` on a Ryzen 5 9600X. Each value is the median of five batch means for one simulated second in the same room. The wall velocity is a streamwise sample in the first cell beside the wall, at the midpoint of the flow direction and of the room height.

| Grid | Free-slip solve | No-slip solve | Free-slip velocity | No-slip velocity |
| ---: | ---: | ---: | ---: | ---: |
| 8³ | 1.00 ms | 1.50 ms | 0.500 m/s | 0.422 m/s |
| 12³ | 6.75 ms | 9.60 ms | 0.500 m/s | 0.299 m/s |
| 24³ | 223 ms | 335 ms | 0.500 m/s | 0.147 m/s |

The 24³ no-slip case exceeded the timestep budget when requested as a single 2-second step, so the comparison uses 1-second steps for every grid. The outlet is a prescribed-flow exhaust. The measured pressure residual remains finite and can depend on resolution and timestep. [Raw timing and divergence samples](ventilation-timing.csv)

## Reproduction

```sh
ctest --test-dir build --output-on-failure
build/scene_physics_test --ventilation-timing
python benchmarks/multiphysics.py build/neutrino --fixtures benchmarks/ventilated-room.json --out ventilated-room.json --reps 7
```
